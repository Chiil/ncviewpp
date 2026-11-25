// Generates synthetic netCDF files for ncview++ tests.
//   make_testdata OUTDIR [--big]
#include <netcdf.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define CHK(x) do { int _s = (x); if (_s != NC_NOERR) { std::fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, nc_strerror(_s)); std::exit(1); } } while (0)

static void textAtt(int nc, int v, const char* n, const char* s) { CHK(nc_put_att_text(nc, v, n, std::strlen(s), s)); }

static float field(int t, int k, double lat, double lon) {
    return 280.f + 25.f * (float)std::cos(lat * M_PI / 180) + 5.f * (float)std::sin((lon + 15.0 * t) * M_PI / 90) - 2.f * k;
}

// Classic-format file: tas(time, lat, lon) with a land mask of fill values, times t0..t0+nt-1.
static void writeTas(const std::string& path, int t0, int nt, int nlat, int nlon) {
    int nc, dt, dla, dlo, vt, vla, vlo, vtas, vpr, vsh;
    CHK(nc_create(path.c_str(), NC_CLOBBER, &nc));
    CHK(nc_def_dim(nc, "time", NC_UNLIMITED, &dt));
    CHK(nc_def_dim(nc, "lat", nlat, &dla));
    CHK(nc_def_dim(nc, "lon", nlon, &dlo));
    CHK(nc_def_var(nc, "time", NC_DOUBLE, 1, &dt, &vt));
    textAtt(nc, vt, "units", "days since 2000-01-01 00:00:00");
    textAtt(nc, vt, "calendar", "noleap");
    CHK(nc_def_var(nc, "lat", NC_FLOAT, 1, &dla, &vla));
    textAtt(nc, vla, "units", "degrees_north");
    CHK(nc_def_var(nc, "lon", NC_FLOAT, 1, &dlo, &vlo));
    textAtt(nc, vlo, "units", "degrees_east");
    int d3[3] = {dt, dla, dlo};
    CHK(nc_def_var(nc, "tas", NC_FLOAT, 3, d3, &vtas));
    textAtt(nc, vtas, "long_name", "near-surface air temperature");
    textAtt(nc, vtas, "units", "K");
    float fill = 1e20f;
    CHK(nc_put_att_float(nc, vtas, "_FillValue", NC_FLOAT, 1, &fill));
    CHK(nc_def_var(nc, "pr", NC_DOUBLE, 3, d3, &vpr));          // double, default fill
    textAtt(nc, vpr, "units", "mm/day");
    CHK(nc_def_var(nc, "packed", NC_SHORT, 3, d3, &vsh));        // scale/offset + short fill
    double sc = 0.01, of = 280.0;
    short sfill = -32767;
    CHK(nc_put_att_double(nc, vsh, "scale_factor", NC_DOUBLE, 1, &sc));
    CHK(nc_put_att_double(nc, vsh, "add_offset", NC_DOUBLE, 1, &of));
    CHK(nc_put_att_short(nc, vsh, "_FillValue", NC_SHORT, 1, &sfill));
    textAtt(nc, NC_GLOBAL, "title", "ncview++ synthetic test data");
    CHK(nc_enddef(nc));

    std::vector<float> lat(nlat), lon(nlon);
    for (int j = 0; j < nlat; ++j) lat[j] = -90.f + (j + 0.5f) * 180.f / nlat;
    for (int i = 0; i < nlon; ++i) lon[i] = (i + 0.5f) * 360.f / nlon;
    CHK(nc_put_var_float(nc, vla, lat.data()));
    CHK(nc_put_var_float(nc, vlo, lon.data()));
    std::vector<float> a(size_t(nlat) * nlon);
    std::vector<double> p(a.size());
    std::vector<short> s(a.size());
    for (int t = 0; t < nt; ++t) {
        double tv = 30.0 * (t0 + t);
        size_t st1 = t, ct1 = 1;
        CHK(nc_put_vara_double(nc, vt, &st1, &ct1, &tv));
        for (int j = 0; j < nlat; ++j)
            for (int i = 0; i < nlon; ++i) {
                size_t k = size_t(j) * nlon + i;
                bool land = lat[j] > 20 && lat[j] < 50 && lon[i] > 240 && lon[i] < 290;   // "North America"
                a[k] = land ? fill : field(t0 + t, 0, lat[j], lon[i]);
                p[k] = land ? NC_FILL_DOUBLE : std::max(0.0, 10 * std::sin(lon[i] * M_PI / 45) * std::cos(lat[j] * M_PI / 60));
                s[k] = land ? sfill : (short)std::lround((field(t0 + t, 0, lat[j], lon[i]) - of) / sc);
            }
        size_t st[3] = {(size_t)t, 0, 0}, ct[3] = {1, (size_t)nlat, (size_t)nlon};
        CHK(nc_put_vara_float(nc, vtas, st, ct, a.data()));
        CHK(nc_put_vara_double(nc, vpr, st, ct, p.data()));
        CHK(nc_put_vara_short(nc, vsh, st, ct, s.data()));
    }
    CHK(nc_close(nc));
}

// netCDF-4 file: 4-D ta(time, lev, lat, lon) chunked+compressed, transposed var(lon, lat), 1-D series.
static void writeNc4(const std::string& path, int nt, int nlev, int nlat, int nlon, bool deflate) {
    int nc, dt, dl, dla, dlo, vt, vl, vla, vlo, vta, vtr, v1;
    CHK(nc_create(path.c_str(), NC_CLOBBER | NC_NETCDF4, &nc));
    CHK(nc_def_dim(nc, "time", nt, &dt));
    CHK(nc_def_dim(nc, "lev", nlev, &dl));
    CHK(nc_def_dim(nc, "lat", nlat, &dla));
    CHK(nc_def_dim(nc, "lon", nlon, &dlo));
    CHK(nc_def_var(nc, "time", NC_DOUBLE, 1, &dt, &vt));
    textAtt(nc, vt, "units", "hours since 1979-01-01");
    textAtt(nc, vt, "calendar", "proleptic_gregorian");
    CHK(nc_def_var(nc, "lev", NC_FLOAT, 1, &dl, &vl));
    textAtt(nc, vl, "units", "hPa");
    textAtt(nc, vl, "positive", "down");
    CHK(nc_def_var(nc, "lat", NC_DOUBLE, 1, &dla, &vla));
    textAtt(nc, vla, "units", "degrees_north");
    CHK(nc_def_var(nc, "lon", NC_DOUBLE, 1, &dlo, &vlo));
    textAtt(nc, vlo, "units", "degrees_east");
    int d4[4] = {dt, dl, dla, dlo};
    CHK(nc_def_var(nc, "ta", NC_FLOAT, 4, d4, &vta));
    size_t chunks[4] = {1, 1, (size_t)nlat, (size_t)nlon};
    CHK(nc_def_var_chunking(nc, vta, NC_CHUNKED, chunks));
    if (deflate) CHK(nc_def_var_deflate(nc, vta, 1, 1, 1));
    textAtt(nc, vta, "units", "K");
    int dtr[2] = {dlo, dla};
    CHK(nc_def_var(nc, "transposed", NC_FLOAT, 2, dtr, &vtr));
    CHK(nc_def_var(nc, "series", NC_FLOAT, 1, &dt, &v1));
    CHK(nc_enddef(nc));

    std::vector<double> lat(nlat), lon(nlon);
    for (int j = 0; j < nlat; ++j) lat[j] = 90.0 - j * 180.0 / (nlat - 1);   // decreasing, like ERA5
    for (int i = 0; i < nlon; ++i) lon[i] = -180.0 + i * 360.0 / nlon;
    CHK(nc_put_var_double(nc, vla, lat.data()));
    CHK(nc_put_var_double(nc, vlo, lon.data()));
    std::vector<float> lev(nlev);
    for (int k = 0; k < nlev; ++k) lev[k] = 1000.f - 100.f * k;
    CHK(nc_put_var_float(nc, vl, lev.data()));
    std::vector<float> a(size_t(nlat) * nlon), ser(nt);
    for (int t = 0; t < nt; ++t) {
        double tv = 6.0 * t;
        size_t st1 = t, ct1 = 1;
        CHK(nc_put_vara_double(nc, vt, &st1, &ct1, &tv));
        ser[t] = std::sin(t * 0.3f);
        for (int k = 0; k < nlev; ++k) {
            for (int j = 0; j < nlat; ++j)
                for (int i = 0; i < nlon; ++i) a[size_t(j) * nlon + i] = field(t, k, lat[j], lon[i]);
            size_t st[4] = {(size_t)t, (size_t)k, 0, 0}, ct[4] = {1, 1, (size_t)nlat, (size_t)nlon};
            CHK(nc_put_vara_float(nc, vta, st, ct, a.data()));
        }
    }
    CHK(nc_put_var_float(nc, v1, ser.data()));
    std::vector<float> tr(size_t(nlon) * nlat);
    for (int i = 0; i < nlon; ++i)
        for (int j = 0; j < nlat; ++j) tr[size_t(i) * nlat + j] = float(i * 1000 + j);
    CHK(nc_put_var_float(nc, vtr, tr.data()));
    CHK(nc_close(nc));
}

// netCDF-4 file with nested groups, MicroHH-stats-like: root time(unlimited)/z, variables in
// groups that use the root dims, a subgroup with its own dim + coord var, and a name clash.
static void writeGroups(const std::string& path, int t0, int nt) {
    const int nz = 16, nzh = 17;
    int nc, dt, dz, vt, vz, gth, gbud, gdyn, dzh, vzh, vth, vfl, vu, vth2, vroot;
    CHK(nc_create(path.c_str(), NC_CLOBBER | NC_NETCDF4, &nc));
    CHK(nc_def_dim(nc, "time", NC_UNLIMITED, &dt));
    CHK(nc_def_dim(nc, "z", nz, &dz));
    CHK(nc_def_var(nc, "time", NC_DOUBLE, 1, &dt, &vt));
    textAtt(nc, vt, "units", "seconds since 2020-01-01");
    CHK(nc_def_var(nc, "z", NC_FLOAT, 1, &dz, &vz));
    textAtt(nc, vz, "units", "m");
    int d2[2] = {dt, dz};
    CHK(nc_def_var(nc, "area", NC_FLOAT, 2, d2, &vroot));
    textAtt(nc, NC_GLOBAL, "title", "grouped test data");
    CHK(nc_def_grp(nc, "thermo", &gth));
    textAtt(gth, NC_GLOBAL, "description", "thermodynamics");
    CHK(nc_def_var(gth, "th", NC_FLOAT, 2, d2, &vth));            // uses parent dims
    textAtt(gth, vth, "units", "K");
    CHK(nc_def_grp(gth, "budget", &gbud));
    CHK(nc_def_dim(gbud, "zh", nzh, &dzh));
    CHK(nc_def_var(gbud, "zh", NC_FLOAT, 1, &dzh, &vzh));
    textAtt(gbud, vzh, "units", "m");
    int d2h[2] = {dt, dzh};
    CHK(nc_def_var(gbud, "flux", NC_FLOAT, 2, d2h, &vfl));
    CHK(nc_def_grp(nc, "dyn", &gdyn));
    CHK(nc_def_var(gdyn, "u", NC_FLOAT, 2, d2, &vu));
    CHK(nc_def_var(gdyn, "th", NC_FLOAT, 2, d2, &vth2));          // same name as thermo/th
    CHK(nc_enddef(nc));

    std::vector<float> z(nz), zh(nzh);
    for (int k = 0; k < nz; ++k) z[k] = 10.f * k + 5.f;
    for (int k = 0; k < nzh; ++k) zh[k] = 10.f * k;
    CHK(nc_put_var_float(nc, vz, z.data()));
    CHK(nc_put_var_float(gbud, vzh, zh.data()));
    std::vector<float> a(nz), b(nzh);
    for (int t = 0; t < nt; ++t) {
        int T = t0 + t;
        double tv = 60.0 * T;
        size_t st1 = t, ct1 = 1;
        CHK(nc_put_vara_double(nc, vt, &st1, &ct1, &tv));
        size_t st[2] = {(size_t)t, 0}, ct[2] = {1, (size_t)nz}, cth[2] = {1, (size_t)nzh};
        for (int k = 0; k < nz; ++k) a[k] = 300.f + T + 0.01f * k;
        CHK(nc_put_vara_float(gth, vth, st, ct, a.data()));
        for (int k = 0; k < nz; ++k) a[k] = -(float)T;
        CHK(nc_put_vara_float(gdyn, vth2, st, ct, a.data()));
        for (int k = 0; k < nz; ++k) a[k] = (float)k;
        CHK(nc_put_vara_float(gdyn, vu, st, ct, a.data()));
        CHK(nc_put_vara_float(nc, vroot, st, ct, a.data()));
        for (int k = 0; k < nzh; ++k) b[k] = 1000.f * T + k;
        CHK(nc_put_vara_float(gbud, vfl, st, cth, b.data()));
    }
    CHK(nc_close(nc));
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: make_testdata OUTDIR [--big]\n"); return 2; }
    std::string d = argv[1];
    writeTas(d + "/tas_all.nc", 0, 24, 90, 180);
    writeTas(d + "/tas_part1.nc", 0, 10, 90, 180);
    writeTas(d + "/tas_part2.nc", 10, 14, 90, 180);
    writeNc4(d + "/ta4d.nc", 8, 3, 73, 144, true);
    writeGroups(d + "/groups.nc", 0, 6);
    writeGroups(d + "/groups_part1.nc", 0, 2);
    writeGroups(d + "/groups_part2.nc", 2, 4);
    if (argc > 2 && std::string(argv[2]) == "--big") {
        // ERA5-sized grid (0.25°): 721 x 1440, 48 times, compressed.
        writeNc4(d + "/big.nc", 48, 1, 721, 1440, true);
        std::printf("wrote big.nc\n");
    }
    std::printf("test data written to %s\n", d.c_str());
    return 0;
}
