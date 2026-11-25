#include "dataset.h"

#include <netcdf.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <mutex>

namespace ncvpp {

// netCDF-C / HDF5 are not thread-safe: every library call goes through this lock.
static std::mutex g_ncMutex;

static std::string getTextAtt(int ncid, int varid, const char* name) {
    size_t len = 0;
    nc_type t;
    if (nc_inq_att(ncid, varid, name, &t, &len) != NC_NOERR) return {};
    if (t == NC_CHAR) {
        std::string s(len, '\0');
        if (nc_get_att_text(ncid, varid, name, s.data()) != NC_NOERR) return {};
        while (!s.empty() && s.back() == '\0') s.pop_back();
        return s;
    }
    if (t == NC_STRING && len >= 1) {
        std::vector<char*> v(len);
        if (nc_get_att_string(ncid, varid, name, v.data()) != NC_NOERR) return {};
        std::string s = v[0] ? v[0] : "";
        nc_free_string(len, v.data());
        return s;
    }
    return {};
}

static bool getNumAtt(int ncid, int varid, const char* name, double* vals, size_t maxn, size_t* nout = nullptr) {
    size_t len = 0;
    nc_type t;
    if (nc_inq_att(ncid, varid, name, &t, &len) != NC_NOERR) return false;
    if (t == NC_CHAR || t == NC_STRING || len == 0) return false;
    std::vector<double> buf(len);
    if (nc_get_att_double(ncid, varid, name, buf.data()) != NC_NOERR) return false;
    for (size_t i = 0; i < std::min(len, maxn); ++i) vals[i] = buf[i];
    if (nout) *nout = len;
    return true;
}

static std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static std::string fmtNum(double v) {
    char b[64];
    if (v == std::floor(v) && std::fabs(v) < 1e15) std::snprintf(b, sizeof b, "%.0f", v);
    else std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

std::string DimInfo::formatValue(double v) const {
    if (time.valid()) return time.format(v);
    return fmtNum(v);
}

std::string DimInfo::format(size_t idx) const {
    if (idx >= coord.size()) return "?";
    return formatValue(coord[idx]);
}

Dataset::~Dataset() {
    std::lock_guard<std::mutex> lk(g_ncMutex);
    for (auto& f : files_) if (f.ncid >= 0) nc_close(f.ncid);
}

int Dataset::findVar(const std::string& name) const {
    std::string n = !name.empty() && name[0] == '/' ? name.substr(1) : name;
    for (size_t i = 0; i < vars_.size(); ++i) if (vars_[i].fullName == n) return (int)i;
    for (size_t i = 0; i < vars_.size(); ++i) if (vars_[i].name == n) return (int)i;
    return -1;
}

std::vector<size_t> Dataset::shape(int v) const {
    std::vector<size_t> s;
    for (int d : vars_[v].dims) s.push_back(dims_[d].len);
    return s;
}

void Dataset::scanGroup(int ncid, int parent, const std::string& name) {
    GroupInfo g;
    g.name = name;
    g.parent = parent;
    if (parent >= 0) g.path = groups_[parent].path.empty() ? name : groups_[parent].path + "/" + name;
    g.ncid.push_back(ncid);
    int idx = (int)groups_.size();
    groups_.push_back(std::move(g));
    if (parent >= 0) groups_[parent].children.push_back(idx);
    int n = 0;
    if (nc_inq_grps(ncid, &n, nullptr) != NC_NOERR || n <= 0) return;   // classic files: no groups
    std::vector<int> ids(n);
    nc_inq_grps(ncid, &n, ids.data());
    for (int id : ids) {
        char nm[NC_MAX_NAME + 1];
        if (nc_inq_grpname(id, nm) == NC_NOERR) scanGroup(id, idx, nm);
    }
}

bool Dataset::open(const std::vector<std::string>& paths, std::string& err) {
    std::lock_guard<std::mutex> lk(g_ncMutex);
    // A larger chunk cache makes stepping through compressed netCDF-4 data much faster.
    nc_set_chunk_cache(256u << 20, 4133, 0.75f);
    paths_ = paths;
    for (const auto& p : paths) {
        File f; f.path = p;
        int st = nc_open(p.c_str(), NC_NOWRITE, &f.ncid);
        if (st != NC_NOERR) { err = p + ": " + nc_strerror(st); return false; }
        files_.push_back(f);
    }
    int ncid = files_[0].ncid;

    // Group tree of the first file; matching groups in the other files are looked up by path.
    scanGroup(ncid, -1, "");
    for (size_t fi = 1; fi < files_.size(); ++fi)
        for (auto& g : groups_) {
            int id = -1;
            if (g.path.empty()) id = files_[fi].ncid;
            else if (nc_inq_grp_full_ncid(files_[fi].ncid, ("/" + g.path).c_str(), &id) != NC_NOERR) id = -1;
            g.ncid.push_back(id);
        }

    // Dimensions of all groups. Dim ids are unique within a file, so variables can refer to
    // dimensions of ancestor groups through the same map.
    int rootUnlim = -1;
    nc_inq_unlimdim(ncid, &rootUnlim);
    int firstUnlim = -1;
    std::map<int, int> dimIndex;
    for (size_t gi = 0; gi < groups_.size(); ++gi) {
        int gid = groups_[gi].ncid[0];
        int ndims = 0;
        if (nc_inq_dimids(gid, &ndims, nullptr, 0) != NC_NOERR || ndims <= 0) continue;
        std::vector<int> dimids(ndims);
        nc_inq_dimids(gid, &ndims, dimids.data(), 0);
        std::vector<int> unl;
        if (gi > 0) {
            int nu = 0;
            if (nc_inq_unlimdims(gid, &nu, nullptr) == NC_NOERR && nu > 0) {
                unl.resize(nu);
                nc_inq_unlimdims(gid, &nu, unl.data());
            }
        }
        for (int id : dimids) {
            char name[NC_MAX_NAME + 1];
            size_t len;
            nc_inq_dim(gid, id, name, &len);
            DimInfo d; d.name = name; d.len = len; d.group = (int)gi;
            dimIndex[id] = (int)dims_.size();
            if (id == rootUnlim) recDim_ = (int)dims_.size();
            if (firstUnlim < 0 && std::find(unl.begin(), unl.end(), id) != unl.end()) firstUnlim = (int)dims_.size();
            dims_.push_back(d);
        }
    }
    if (recDim_ < 0) recDim_ = firstUnlim;   // no unlimited dim in the root: use the first one in a group

    // Aggregate the record dimension across files.
    if (files_.size() > 1) {
        if (recDim_ < 0) {
            err = "multiple files given but the first file has no unlimited (record) dimension";
            return false;
        }
        const GroupInfo& rg = groups_[dims_[recDim_].group];
        size_t total = 0;
        for (size_t fi = 0; fi < files_.size(); ++fi) {
            File& f = files_[fi];
            int did;
            if (rg.ncid[fi] < 0 || nc_inq_dimid(rg.ncid[fi], dims_[recDim_].name.c_str(), &did) != NC_NOERR) {
                err = f.path + ": has no dimension '" + (rg.path.empty() ? "" : rg.path + "/") + dims_[recDim_].name + "'";
                return false;
            }
            nc_inq_dimlen(rg.ncid[fi], did, &f.recLen);
            f.recStart = total;
            total += f.recLen;
        }
        dims_[recDim_].len = total;
        dims_[recDim_].isRecord = true;
    } else if (!files_.empty()) {
        files_[0].recLen = recDim_ >= 0 ? dims_[recDim_].len : 0;
    }

    for (size_t gi = 0; gi < groups_.size(); ++gi) {
        const GroupInfo& g = groups_[gi];
        int gid = g.ncid[0];
        int nvars = 0;
        if (nc_inq_varids(gid, &nvars, nullptr) != NC_NOERR || nvars <= 0) continue;
        std::vector<int> varids(nvars);
        nc_inq_varids(gid, &nvars, varids.data());
        for (int vid : varids) {
            char name[NC_MAX_NAME + 1];
            nc_type type;
            int nd;
            int vd[NC_MAX_VAR_DIMS];
            nc_inq_var(gid, vid, name, &type, &nd, vd, nullptr);
            if (nd == 0) continue;
            if (type == NC_CHAR || type == NC_STRING || type > NC_STRING) continue;
            VarInfo v;
            v.name = name;
            v.group = (int)gi;
            v.fullName = g.path.empty() ? v.name : g.path + "/" + v.name;
            v.type = type;
            bool ok = true;
            for (int k = 0; k < nd; ++k) {
                auto it = dimIndex.find(vd[k]);
                if (it == dimIndex.end()) { ok = false; break; }
                v.dims.push_back(it->second);
            }
            if (!ok) continue;
            v.isRecordVar = files_.size() > 1 && v.dims[0] == recDim_;
            for (size_t fi = 0; fi < files_.size(); ++fi) {
                int id = -1;
                if (fi == 0) id = vid;
                else if (v.isRecordVar && (g.ncid[fi] < 0 || nc_inq_varid(g.ncid[fi], name, &id) != NC_NOERR)) {
                    err = files_[fi].path + ": variable '" + v.fullName + "' missing";
                    return false;
                }
                v.varid.push_back(id);
                v.ncid.push_back(g.ncid[fi]);
            }
            v.longName = getTextAtt(gid, vid, "long_name");
            v.units = getTextAtt(gid, vid, "units");
            v.coordinatesAttr = getTextAtt(gid, vid, "coordinates");
            double a[2];
            size_t n = 0;
            if (getNumAtt(gid, vid, "_FillValue", a, 1)) { v.hasFill = true; v.fill = (float)a[0]; }
            else if (type == NC_FLOAT || type == NC_DOUBLE) { v.hasFill = true; v.fill = (float)NC_FILL_DOUBLE; }
            if (getNumAtt(gid, vid, "missing_value", a, 1)) { v.hasMissing = true; v.missing = (float)a[0]; }
            if (getNumAtt(gid, vid, "valid_range", a, 2, &n) && n >= 2) {
                v.hasValidMin = v.hasValidMax = true; v.validMin = (float)a[0]; v.validMax = (float)a[1];
            }
            if (getNumAtt(gid, vid, "valid_min", a, 1)) { v.hasValidMin = true; v.validMin = (float)a[0]; }
            if (getNumAtt(gid, vid, "valid_max", a, 1)) { v.hasValidMax = true; v.validMax = (float)a[0]; }
            if (getNumAtt(gid, vid, "scale_factor", a, 1)) v.scale = a[0];
            if (getNumAtt(gid, vid, "add_offset", a, 1)) v.offset = a[0];
            // A coordinate variable lives in the same group as its dimension.
            v.isCoordVar = nd == 1 && dims_[v.dims[0]].name == v.name && dims_[v.dims[0]].group == (int)gi;
            vars_.push_back(std::move(v));
        }
    }

    loadCoords();
    return true;
}

void Dataset::loadCoords() {
    // Called with g_ncMutex held; readRaw does not lock.
    for (size_t di = 0; di < dims_.size(); ++di) {
        DimInfo& d = dims_[di];
        d.coord.resize(d.len);
        for (size_t i = 0; i < d.len; ++i) d.coord[i] = (double)i;
        int v = -1;
        for (size_t k = 0; k < vars_.size(); ++k)
            if (vars_[k].isCoordVar && vars_[k].dims[0] == (int)di) v = (int)k;
        std::string axis, stdName, positive;
        if (v >= 0 && d.len > 0) {
            std::vector<size_t> st{0}, ct{d.len};
            if (readRaw(v, st, ct, nullptr, d.coord.data(), nullptr)) d.hasCoordVar = true;
            const VarInfo& cv = vars_[v];
            d.units = cv.units;
            d.longName = cv.longName;
            int vid = cv.varid[0], ncid = cv.ncid[0];
            axis = lower(getTextAtt(ncid, vid, "axis"));
            stdName = lower(getTextAtt(ncid, vid, "standard_name"));
            positive = getTextAtt(ncid, vid, "positive");
            std::string cal = getTextAtt(ncid, vid, "calendar");
            d.time.parse(d.units, cal);
        }
        std::string n = lower(d.name), u = lower(d.units);
        if (d.time.valid() || axis == "t") d.kind = AxisKind::Time;
        else if (u == "degrees_east" || u == "degree_east" || u == "degree_e" || u == "degrees_e" ||
                 stdName == "longitude" || n == "lon" || n == "longitude" || n == "nav_lon")
            d.kind = AxisKind::Lon;
        else if (u == "degrees_north" || u == "degree_north" || u == "degree_n" || u == "degrees_n" ||
                 stdName == "latitude" || n == "lat" || n == "latitude" || n == "nav_lat")
            d.kind = AxisKind::Lat;
        else if (axis == "z" || !positive.empty() || n == "lev" || n == "level" || n == "plev" || n == "depth" || n == "z")
            d.kind = AxisKind::Vertical;
        bool inc = true, dec = true;
        for (size_t i = 1; i < d.len; ++i) {
            if (!(d.coord[i] > d.coord[i - 1])) inc = false;
            if (!(d.coord[i] < d.coord[i - 1])) dec = false;
        }
        d.monotonic = inc || dec;
    }
}

bool Dataset::readRaw(int vi, const std::vector<size_t>& start, const std::vector<size_t>& count,
                      float* fout, double* dout, std::string* err) const {
    const VarInfo& v = vars_[vi];
    auto get = [&](int ncid, int varid, const size_t* s, const size_t* c, size_t offset) {
        int st = fout ? nc_get_vara_float(ncid, varid, s, c, fout + offset)
                      : nc_get_vara_double(ncid, varid, s, c, dout + offset);
        if (st == NC_ERANGE) st = NC_NOERR;  // out-of-range values handled as non-finite later
        if (st != NC_NOERR && err) *err = v.name + ": " + nc_strerror(st);
        return st == NC_NOERR;
    };
    if (!v.isRecordVar) return get(v.ncid[0], v.varid[0], start.data(), count.data(), 0);

    size_t inner = 1;
    for (size_t k = 1; k < count.size(); ++k) inner *= count[k];
    size_t lo0 = start[0], hi0 = start[0] + count[0];
    for (size_t fi = 0; fi < files_.size(); ++fi) {
        const File& f = files_[fi];
        size_t lo = std::max(lo0, f.recStart), hi = std::min(hi0, f.recStart + f.recLen);
        if (lo >= hi) continue;
        std::vector<size_t> s = start, c = count;
        s[0] = lo - f.recStart;
        c[0] = hi - lo;
        if (!get(v.ncid[fi], v.varid[fi], s.data(), c.data(), (lo - lo0) * inner)) return false;
    }
    return true;
}

bool Dataset::read(int vi, const std::vector<size_t>& start, const std::vector<size_t>& count,
                   std::vector<float>& out, std::string* err) const {
    size_t n = 1;
    for (size_t c : count) n *= c;
    out.resize(n);
    {
        std::lock_guard<std::mutex> lk(g_ncMutex);
        if (!readRaw(vi, start, count, out.data(), nullptr, err)) return false;
    }
    const VarInfo& v = vars_[vi];
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const bool scaled = v.scale != 1.0 || v.offset != 0.0;
    const float sc = (float)v.scale, of = (float)v.offset;
    for (size_t i = 0; i < n; ++i) {
        float x = out[i];
        if (!std::isfinite(x) || (v.hasFill && x == v.fill) || (v.hasMissing && x == v.missing) ||
            (v.hasValidMin && x < v.validMin) || (v.hasValidMax && x > v.validMax)) {
            out[i] = nan;
            continue;
        }
        if (scaled) out[i] = x * sc + of;
    }
    return true;
}

std::vector<std::pair<std::string, std::string>> Dataset::attrsOf(int ncid, int varid) {
    // Called with g_ncMutex held.
    std::vector<std::pair<std::string, std::string>> res;
    int natts = 0;
    if (ncid < 0 || nc_inq_varnatts(ncid, varid, &natts) != NC_NOERR) return res;
    for (int i = 0; i < natts; ++i) {
        char name[NC_MAX_NAME + 1];
        if (nc_inq_attname(ncid, varid, i, name) != NC_NOERR) continue;
        nc_type t; size_t len;
        nc_inq_att(ncid, varid, name, &t, &len);
        std::string val;
        if (t == NC_CHAR || t == NC_STRING) val = getTextAtt(ncid, varid, name);
        else {
            std::vector<double> b(len);
            if (len > 0 && nc_get_att_double(ncid, varid, name, b.data()) == NC_NOERR) {
                for (size_t k = 0; k < len && k < 16; ++k) { if (k) val += ", "; val += fmtNum(b[k]); }
                if (len > 16) val += ", ...";
            }
        }
        res.emplace_back(name, val);
    }
    return res;
}

std::vector<std::pair<std::string, std::string>> Dataset::attributes(int vi) const {
    if (vi < 0) return groupAttributes(0);
    std::lock_guard<std::mutex> lk(g_ncMutex);
    return attrsOf(vars_[vi].ncid[0], vars_[vi].varid[0]);
}

std::vector<std::pair<std::string, std::string>> Dataset::groupAttributes(int g) const {
    std::lock_guard<std::mutex> lk(g_ncMutex);
    if (g < 0 || g >= (int)groups_.size()) return {};
    return attrsOf(groups_[g].ncid[0], NC_GLOBAL);
}

} // namespace ncvpp
