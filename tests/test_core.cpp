// Core tests: CF time, dataset reading, multi-file aggregation, fill/scale, transposes, rendering.
//   test_core DIR   (DIR contains output of make_testdata)
#include <algorithm>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include <netcdf.h>

#include "cftime.h"
#include "dataset.h"
#include "export.h"
#include "loader.h"
#include "overlay.h"
#include "render.h"

using namespace ncvpp;

static int fails = 0;
#define EXPECT(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static SliceKey key(const Dataset& ds, int v, std::vector<size_t> pos) {
    SliceKey k;
    k.var = v;
    k.xd = (int)ds.shape(v).size() - 1;
    k.yd = (int)ds.shape(v).size() - 2;
    k.pos = pos;
    return k;
}

static void testTime() {
    CFTime t;
    EXPECT(t.parse("days since 2000-01-01 00:00:00", "noleap"));
    EXPECT(t.format(59) == "2000-03-01 00:00");          // no Feb 29 in noleap
    EXPECT(t.parse("days since 2000-01-01", "standard"));
    EXPECT(t.format(59) == "2000-02-29 00:00");
    EXPECT(t.format(366) == "2001-01-01 00:00");
    EXPECT(t.parse("hours since 1979-01-01", "proleptic_gregorian"));
    EXPECT(t.format(6) == "1979-01-01 06:00");
    EXPECT(t.parse("days since 2000-01-01", "360_day"));
    EXPECT(t.format(30) == "2000-02-01 00:00");
    EXPECT(t.format(359) == "2000-12-30 00:00");
    EXPECT(t.parse("seconds since 1970-01-01T00:00:00Z", ""));
    EXPECT(t.format(86400.0 * 365 + 3661) == "1971-01-01 01:01:01");
    EXPECT(t.parse("days since 1582-10-04", "gregorian"));  // Julian→Gregorian switch
    EXPECT(t.format(1) == "1582-10-15 00:00");
    EXPECT(t.parse("days since 0001-01-01", "julian"));
    EXPECT(t.format(-1) == "0000-12-31 00:00");
    EXPECT(!t.parse("K", ""));
    // Round trips across calendars.
    for (Calendar c : {Calendar::Standard, Calendar::ProlepticGregorian, Calendar::Julian, Calendar::NoLeap, Calendar::AllLeap, Calendar::Day360}) {
        int64_t y; int m, d;
        for (int64_t n = CFTime::dayNumber(c, 1500, 1, 1); n < CFTime::dayNumber(c, 2100, 1, 1); n += 37) {
            CFTime::fromDayNumber(c, n, y, m, d);
            if (CFTime::dayNumber(c, y, m, d) != n) { EXPECT(false); break; }
        }
    }
}

static void testDataset(const std::string& dir) {
    Dataset all;
    std::string err;
    EXPECT(all.open({dir + "/tas_all.nc"}, err));
    int v = all.findVar("tas");
    EXPECT(v >= 0);
    EXPECT(all.shape(v) == (std::vector<size_t>{24, 90, 180}));
    const DimInfo& tdim = all.dims()[all.vars()[v].dims[0]];
    EXPECT(tdim.kind == AxisKind::Time);
    EXPECT(tdim.format(2) == "2000-03-02 00:00");   // 60 days, noleap
    EXPECT(all.dims()[all.vars()[v].dims[1]].kind == AxisKind::Lat);
    EXPECT(all.dims()[all.vars()[v].dims[2]].kind == AxisKind::Lon);

    SliceP s = readSlice(all, key(all, v, {5, 0, 0}));
    EXPECT(s->error.empty());
    EXPECT(s->nx == 180 && s->ny == 90);
    // Land mask (fill) must be NaN; ocean must be finite.
    int nanCount = 0;
    for (float x : s->data) nanCount += std::isnan(x);
    EXPECT(nanCount > 0 && nanCount < 90 * 180);
    EXPECT(s->vmin > 240 && s->vmax < 320);

    // Double var with default fill.
    SliceP p = readSlice(all, key(all, all.findVar("pr"), {0, 0, 0}));
    int pn = 0;
    for (float x : p->data) pn += std::isnan(x);
    EXPECT(pn == nanCount);
    // Packed shorts unpack to the same field (±0.01).
    SliceP pk = readSlice(all, key(all, all.findVar("packed"), {5, 0, 0}));
    double maxdiff = 0;
    for (size_t i = 0; i < s->data.size(); ++i)
        if (!std::isnan(s->data[i])) maxdiff = std::max(maxdiff, (double)std::fabs(s->data[i] - pk->data[i]));
        else EXPECT(std::isnan(pk->data[i]));
    EXPECT(maxdiff < 0.011);

    // Multi-file aggregation equals the single file.
    Dataset multi;
    EXPECT(multi.open({dir + "/tas_part1.nc", dir + "/tas_part2.nc"}, err));
    int mv = multi.findVar("tas");
    EXPECT(multi.shape(mv)[0] == 24);
    for (size_t t : {0, 9, 10, 23}) {
        SliceP a = readSlice(all, key(all, v, {t, 0, 0}));
        SliceP b = readSlice(multi, key(multi, mv, {t, 0, 0}));
        bool same = a->data.size() == b->data.size();
        for (size_t i = 0; same && i < a->data.size(); ++i)
            same = (std::isnan(a->data[i]) && std::isnan(b->data[i])) || a->data[i] == b->data[i];
        EXPECT(same);
    }
    EXPECT(multi.dims()[multi.vars()[mv].dims[0]].coord == tdim.coord);
    // Time series across the file boundary (Hovmöller-style read of the record dim).
    std::vector<float> ts1, ts2;
    EXPECT(all.read(v, {0, 45, 10}, {24, 1, 1}, ts1));
    EXPECT(multi.read(mv, {0, 45, 10}, {24, 1, 1}, ts2));
    EXPECT(ts1 == ts2);
    // time-lon slice crossing files.
    SliceKey hk; hk.var = mv; hk.xd = 2; hk.yd = 0; hk.pos = {0, 45, 0};
    SliceP hov = readSlice(multi, hk);
    EXPECT(hov->nx == 180 && hov->ny == 24 && hov->data[23 * 180 + 10] == ts1[23]);

    // netCDF-4: 4-D, chunked, compressed; transposed storage.
    Dataset n4;
    EXPECT(n4.open({dir + "/ta4d.nc"}, err));
    int tv = n4.findVar("ta");
    EXPECT(n4.shape(tv) == (std::vector<size_t>{8, 3, 73, 144}));
    EXPECT(n4.dims()[n4.vars()[tv].dims[1]].kind == AxisKind::Vertical);
    EXPECT(n4.dims()[n4.vars()[tv].dims[0]].format(5) == "1979-01-02 06:00");
    int tr = n4.findVar("transposed");
    SliceKey tk; tk.var = tr; tk.xd = 0; tk.yd = 1; tk.pos = {0, 0};   // x = lon (stored outer)
    SliceP ts = readSlice(n4, tk);
    EXPECT(ts->nx == 144 && ts->ny == 73);
    EXPECT(ts->data[7 * 144 + 100] == 100 * 1000 + 7);

    // Overlay finds coast cells on a lon/lat grid (both lon conventions).
    const auto& d4 = n4.dims();
    auto cells = overlayCells(Overlay::Coast, d4[n4.vars()[tv].dims[3]], d4[n4.vars()[tv].dims[2]]);
    EXPECT(cells.size() > 500);
    auto cells2 = overlayCells(Overlay::Coast, all.dims()[all.vars()[v].dims[2]], all.dims()[all.vars()[v].dims[1]]);
    EXPECT(cells2.size() > 500);

    // Rendering.
    RenderParams rp;
    rp.vmin = s->vmin; rp.vmax = s->vmax;
    std::vector<uint32_t> px;
    int w, h;
    renderSlice(*s, rp, px, w, h);
    EXPECT(w == 180 && h == 90 && px.size() == 180u * 90u);
    // Missing → missing colour; flipY puts data row 0 at the bottom.
    size_t missIdx = 0;
    for (size_t i = 0; i < s->data.size(); ++i) if (std::isnan(s->data[i])) { missIdx = i; break; }
    size_t r = missIdx / 180, c = missIdx % 180;
    EXPECT(px[(89 - r) * 180 + c] == rp.missing);
    rp.decim = 4;
    renderSlice(*s, rp, px, w, h);
    EXPECT(w == 45 && h == 23);
    EXPECT(colormaps().size() == 24);

    // Loader: async request completes and is cached.
    Loader ld(all, 64u << 20);
    SliceKey k0 = key(all, v, {3, 0, 0});
    ld.request(k0, true);
    for (int i = 0; i < 2000 && !ld.get(k0); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT(ld.get(k0) != nullptr);
}

static void testGroups(const std::string& dir) {
    Dataset ds;
    std::string err;
    EXPECT(ds.open({dir + "/groups.nc"}, err));
    // Groups: root, thermo, thermo/budget, dyn.
    EXPECT(ds.groups().size() == 4);
    EXPECT(ds.groups()[0].path.empty() && ds.groups()[0].children.size() == 2);
    int th = ds.findVar("thermo/th");
    EXPECT(th >= 0 && ds.findVar("/thermo/th") == th);
    EXPECT(ds.findVar("th") == th);                         // bare name: first match
    int dth = ds.findVar("dyn/th");
    EXPECT(dth >= 0 && dth != th);
    int fl = ds.findVar("thermo/budget/flux");
    EXPECT(fl >= 0 && ds.groups()[ds.vars()[fl].group].path == "thermo/budget");
    EXPECT(ds.shape(th) == (std::vector<size_t>{6, 16}));
    EXPECT(ds.shape(fl) == (std::vector<size_t>{6, 17}));
    // Parent-group dims resolve to the root's dims and coordinate values.
    const DimInfo& z = ds.dims()[ds.vars()[th].dims[1]];
    EXPECT(z.name == "z" && z.group == 0 && z.hasCoordVar && z.coord[1] == 15.0);
    const DimInfo& zh = ds.dims()[ds.vars()[fl].dims[1]];
    EXPECT(zh.name == "zh" && zh.hasCoordVar && zh.coord[2] == 20.0);
    EXPECT(ds.vars()[ds.findVar("thermo/budget/zh")].isCoordVar);
    std::vector<float> out;
    EXPECT(ds.read(th, {3, 0}, {1, 16}, out) && out[0] == 303.f);
    EXPECT(ds.read(dth, {3, 0}, {1, 16}, out) && out[0] == -3.f);
    EXPECT(ds.read(fl, {5, 0}, {1, 17}, out) && out[4] == 5004.f);
    auto ga = ds.groupAttributes(1);
    EXPECT(ga.size() == 1 && ga[0].second == "thermodynamics");
    EXPECT(ds.attributes(th).size() == 1);

    // Multi-file aggregation of group variables along the root record dim.
    Dataset m;
    EXPECT(m.open({dir + "/groups_part1.nc", dir + "/groups_part2.nc"}, err));
    int mfl = m.findVar("thermo/budget/flux");
    EXPECT(mfl >= 0 && m.shape(mfl)[0] == 6);
    EXPECT(m.read(mfl, {1, 0}, {3, 17}, out) && out[0] == 1000.f && out[17] == 2000.f && out[34 + 1] == 3001.f);

    // Classic files have just the root group.
    Dataset c;
    EXPECT(c.open({dir + "/tas_all.nc"}, err) && c.groups().size() == 1);
    EXPECT(c.vars()[c.findVar("tas")].fullName == "tas");
}

static void testMerge(const std::string& dir) {
    std::string err;
    Dataset m;
    EXPECT(m.open({dir + "/merge_a.nc", dir + "/merge_b.nc", dir + "/merge_c.nc"}, err));
    EXPECT(m.merged());
    int a = m.findVar("a"), b = m.findVar("b");
    EXPECT(a >= 0 && b >= 0);
    // Common times: 120, 180, 240 s (b is in minutes, every 30 s).
    const DimInfo& t = m.dims()[m.vars()[a].dims[0]];
    EXPECT(t.len == 3 && t.coord == (std::vector<double>{120, 180, 240}));
    EXPECT(m.shape(a) == (std::vector<size_t>{3, 16}) && m.shape(b) == (std::vector<size_t>{3, 16}));
    EXPECT(m.vars()[a].dims == m.vars()[b].dims);
    std::vector<float> out;
    EXPECT(m.read(a, {0, 0}, {3, 1}, out) && out == (std::vector<float>{200, 300, 400}));
    EXPECT(m.read(b, {0, 3}, {3, 1}, out) && out == (std::vector<float>{-3, -2003, -4003}));   // b steps 0, 2, 4
    EXPECT(m.read(b, {1, 0}, {1, 16}, out) && out[5] == -2005.f);
    EXPECT(m.findVar("s") >= 0);
    // z of length 8 conflicts: c is hidden; d on its own grid is shown; the clashing a is renamed.
    EXPECT(m.findVar("c") < 0);
    int d = m.findVar("d");
    EXPECT(d >= 0 && m.read(d, {0, 0}, {4, 6}, out) && out[23] == 7.f);
    EXPECT(m.findVar("a (merge_c.nc)") >= 0);
    EXPECT(!m.notes().empty());

    // Order matters only for which file defines the reference time axis.
    Dataset r;
    EXPECT(r.open({dir + "/merge_b.nc", dir + "/merge_a.nc"}, err) && r.merged());
    int ra = r.findVar("a");
    EXPECT(r.shape(ra)[0] == 3 && r.read(ra, {2, 0}, {1, 1}, out) && out[0] == 400.f);

    // Consecutive files still concatenate automatically; forcing merge keeps the first one's times.
    Dataset c;
    EXPECT(c.open({dir + "/tas_part1.nc", dir + "/tas_part2.nc"}, err) && !c.merged());
    Dataset f;
    EXPECT(f.open({dir + "/tas_part1.nc", dir + "/tas_part2.nc"}, err, MultiFile::Merge) && f.merged());
    EXPECT(f.shape(f.findVar("tas"))[0] == 10);
}

// A running simulation appends records while the file is open in the viewer.
static void testReload(const std::string& dir) {
    for (int fmt : {NC_CLOBBER, NC_CLOBBER | NC_NETCDF4}) {
        std::string path = dir + (fmt & NC_NETCDF4 ? "/grow4.nc" : "/grow3.nc");
        int nc, dt, dx, vt, vu;
        EXPECT(nc_create(path.c_str(), fmt, &nc) == NC_NOERR);
        nc_def_dim(nc, "time", NC_UNLIMITED, &dt);
        nc_def_dim(nc, "x", 4, &dx);
        int dims[2] = {dt, dx};
        nc_def_var(nc, "time", NC_DOUBLE, 1, &dt, &vt);
        nc_def_var(nc, "u", NC_FLOAT, 2, dims, &vu);
        nc_enddef(nc);
        auto write = [&](size_t t) {
            double tv = 10.0 * t;
            float row[4] = {float(t), float(t), float(t), float(t)};
            size_t s[2] = {t, 0}, c[2] = {1, 4};
            nc_put_vara_double(nc, vt, s, c, &tv);
            nc_put_vara_float(nc, vu, s, c, row);
            nc_sync(nc);
        };
        write(0);
        write(1);

        Dataset ds;
        std::string err;
        EXPECT(ds.open({path}, err));
        int u = ds.findVar("u");
        EXPECT(u >= 0 && ds.shape(u)[0] == 2);
        write(2);
        write(3);
        EXPECT(ds.reload(err));
        u = ds.findVar("u");
        EXPECT(u >= 0 && ds.shape(u)[0] == 4);
        std::vector<float> out;
        EXPECT(ds.read(u, {3, 0}, {1, 4}, out) && out[0] == 3.f);
        EXPECT(ds.dims()[ds.vars()[u].dims[0]].coord.back() == 30.0);
        nc_close(nc);

        // A failed reload leaves the dataset usable for another try.
        std::string moved = path + ".moved";
        std::rename(path.c_str(), moved.c_str());
        EXPECT(!ds.reload(err));
        EXPECT(!ds.read(u, {0, 0}, {1, 4}, out));
        std::rename(moved.c_str(), path.c_str());
        EXPECT(ds.reload(err) && ds.read(u, {0, 0}, {1, 4}, out) && out[0] == 0.f);
    }
}

static void testExport(const std::string& dir) {
    Dataset ds;
    std::string err;
    EXPECT(ds.open({dir + "/tas_all.nc"}, err));
    int v = ds.findVar("tas");
    if (v < 0) { EXPECT(false); return; }
    SliceKey k = key(ds, v, {2, 0, 0});
    SliceP s = readSlice(ds, k);
    RenderParams rp;
    rp.vmin = s->vmin;
    rp.vmax = s->vmax;
    ExportSpec e = makeExportSpec(ds, k, *s, rp);
    EXPECT(e.title == "near-surface air temperature");
    EXPECT(e.cbarLabel == "K");
    EXPECT(e.subtitle.rfind("time = ", 0) == 0);
    EXPECT(exportFileName(ds, k, 0) == "tas_0002.png");
    std::string path = dir + "/export_test.png";
    EXPECT(exportPNG(path, e, err));
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { EXPECT(false); return; }
    unsigned char b[64] = {};
    size_t n = std::fread(b, 1, sizeof b, f);
    std::fclose(f);
    auto be32 = [&](int o) { return (uint32_t(b[o]) << 24) | (uint32_t(b[o + 1]) << 16) | (uint32_t(b[o + 2]) << 8) | b[o + 3]; };
    EXPECT(n == sizeof b && std::memcmp(b, "\x89PNG", 4) == 0);
    EXPECT(be32(16) > s->nx && be32(20) > s->ny);                     // image plus margins
    EXPECT(std::max(be32(16), be32(20)) >= 1800);                     // upscaled to the minimum size
    EXPECT(std::memcmp(b + 37, "pHYs", 4) == 0 && be32(41) == 11811);  // 300 dpi in px/m
    EXPECT(!exportPNG(dir + "/no/such/dir/x.png", e, err) && !err.empty());
}

static void bench(const std::string& path) {
    Dataset ds;
    std::string err;
    if (!ds.open({path}, err)) return;
    int v = ds.findVar("ta");
    size_t nt = ds.shape(v)[0];
    std::vector<uint32_t> px;
    int w, h;
    auto t0 = std::chrono::steady_clock::now();
    double tRead = 0, tRender = 0;
    for (size_t t = 0; t < nt; ++t) {
        auto a = std::chrono::steady_clock::now();
        SliceP s = readSlice(ds, key(ds, v, {t, 0, 0, 0}));
        auto b = std::chrono::steady_clock::now();
        RenderParams rp; rp.vmin = s->vmin; rp.vmax = s->vmax;
        renderSlice(*s, rp, px, w, h);
        auto c = std::chrono::steady_clock::now();
        tRead += std::chrono::duration<double>(b - a).count();
        tRender += std::chrono::duration<double>(c - b).count();
    }
    double tot = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("bench %s: %zu frames of %dx%d: read %.1f ms/frame, render %.1f ms/frame, %.0f fps total\n",
                path.c_str(), nt, w, h, 1e3 * tRead / nt, 1e3 * tRender / nt, nt / tot);
}

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : ".";
    testTime();
    testDataset(dir);
    testGroups(dir);
    testMerge(dir);
    testReload(dir);
    testExport(dir);
    bench(dir + "/big.nc");
    std::printf(fails ? "%d FAILURES\n" : "all tests passed\n", fails);
    return fails ? 1 : 0;
}
