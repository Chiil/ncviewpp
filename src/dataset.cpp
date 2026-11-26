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

void Dataset::scanGroup(int ncid, int parent, const std::string& name, size_t fi) {
    // Groups of all files form one tree, matched by path.
    std::string path = parent < 0 ? "" : groups_[parent].path.empty() ? name : groups_[parent].path + "/" + name;
    int idx = -1;
    for (size_t i = 0; i < groups_.size(); ++i)
        if (groups_[i].path == path && (parent < 0) == (groups_[i].parent < 0)) { idx = (int)i; break; }
    if (idx < 0) {
        GroupInfo g;
        g.name = name;
        g.parent = parent;
        g.path = path;
        g.ncid.assign(files_.size(), -1);
        idx = (int)groups_.size();
        groups_.push_back(std::move(g));
        if (parent >= 0) groups_[parent].children.push_back(idx);
    }
    groups_[idx].ncid[fi] = ncid;
    int n = 0;
    if (nc_inq_grps(ncid, &n, nullptr) != NC_NOERR || n <= 0) return;   // classic files: no groups
    std::vector<int> ids(n);
    nc_inq_grps(ncid, &n, ids.data());
    for (int id : ids) {
        char nm[NC_MAX_NAME + 1];
        if (nc_inq_grpname(id, nm) == NC_NOERR) scanGroup(id, idx, nm, fi);
    }
}

static std::string baseName(const std::string& p) {
    size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

// Values of the coordinate variable `name` (1-D, length len) in group gid.
static bool readCoordVar(int gid, const std::string& name, size_t len, std::vector<double>& out,
                         std::string* units = nullptr, std::string* cal = nullptr) {
    int vid, nd;
    if (gid < 0 || nc_inq_varid(gid, name.c_str(), &vid) != NC_NOERR) return false;
    if (nc_inq_varndims(gid, vid, &nd) != NC_NOERR || nd != 1) return false;
    out.assign(len, 0.0);
    if (len > 0 && nc_get_var_double(gid, vid, out.data()) != NC_NOERR) return false;
    if (units) *units = getTextAtt(gid, vid, "units");
    if (cal) *cal = getTextAtt(gid, vid, "calendar");
    return true;
}

static bool sameValues(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!(std::fabs(a[i] - b[i]) <= 1e-5 * std::max(1.0, std::max(std::fabs(a[i]), std::fabs(b[i]))))) return false;
    return true;
}

bool Dataset::open(const std::vector<std::string>& paths, std::string& err, MultiFile mode) {
    std::lock_guard<std::mutex> lk(g_ncMutex);
    // A larger chunk cache makes stepping through compressed netCDF-4 data much faster.
    nc_set_chunk_cache(256u << 20, 4133, 0.75f);
    paths_ = paths;
    mode_ = mode;
    for (const auto& p : paths) {
        File f; f.path = p;
        int st = nc_open(p.c_str(), NC_NOWRITE, &f.ncid);
        if (st != NC_NOERR) { err = p + ": " + nc_strerror(st); return false; }
        files_.push_back(f);
    }
    const size_t nf = files_.size();
    if (nf == 0) { err = "no files"; return false; }
    for (size_t fi = 0; fi < nf; ++fi) scanGroup(files_[fi].ncid, -1, "", fi);

    // Dimensions of all groups of all files, matched by (group, name). Dim ids are unique within
    // a file, so variables can refer to dimensions of ancestor groups through the per-file map.
    std::vector<std::map<int, int>> dimMap(nf);       // file dimid -> index into dims_
    std::vector<std::map<int, size_t>> dimLen(nf);    // index into dims_ -> length in that file
    for (size_t fi = 0; fi < nf; ++fi) {
        int rootUnlim = -1, firstUnlim = -1, fileRec = -1;
        nc_inq_unlimdim(files_[fi].ncid, &rootUnlim);
        for (size_t gi = 0; gi < groups_.size(); ++gi) {
            int gid = groups_[gi].ncid[fi];
            int ndims = 0;
            if (gid < 0 || nc_inq_dimids(gid, &ndims, nullptr, 0) != NC_NOERR || ndims <= 0) continue;
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
                int idx = -1;
                for (size_t k = 0; k < dims_.size(); ++k)
                    if (dims_[k].group == (int)gi && dims_[k].name == name) { idx = (int)k; break; }
                if (idx < 0) {
                    DimInfo d; d.name = name; d.len = len; d.group = (int)gi;
                    idx = (int)dims_.size();
                    dims_.push_back(d);
                }
                dimMap[fi][id] = idx;
                dimLen[fi][idx] = len;
                if (id == rootUnlim) fileRec = idx;
                if (firstUnlim < 0 && std::find(unl.begin(), unl.end(), id) != unl.end()) firstUnlim = idx;
            }
        }
        // The record dim is the root's unlimited dim, else the first one in a group, of the
        // first file that has one.
        if (recDim_ < 0) recDim_ = fileRec >= 0 ? fileRec : firstUnlim;
    }
    auto hasRec = [&](size_t fi) { return recDim_ >= 0 && dimLen[fi].count(recDim_) > 0; };

    // Record coordinate of every file, in the units of the first file that has it.
    std::vector<std::vector<double>> ftime(nf);
    std::vector<char> ftimeCoord(nf, 0);
    if (nf > 1 && recDim_ >= 0) {
        const DimInfo& rd = dims_[recDim_];
        std::string refUnits;
        CFTime refT;
        bool haveRef = false;
        for (size_t fi = 0; fi < nf; ++fi) {
            if (!hasRec(fi)) continue;
            size_t len = dimLen[fi][recDim_];
            std::string units, cal;
            if (readCoordVar(groups_[rd.group].ncid[fi], rd.name, len, ftime[fi], &units, &cal)) {
                ftimeCoord[fi] = 1;
                if (!haveRef) { haveRef = true; refUnits = units; refT.parse(units, cal); }
                else if (units != refUnits) {
                    CFTime t;
                    if (t.parse(units, cal) && refT.valid() && t.calendar() == refT.calendar())
                        for (double& x : ftime[fi]) x = t.convertTo(x, refT);
                }
            } else {
                ftime[fi].resize(len);
                for (size_t i = 0; i < len; ++i) ftime[fi][i] = (double)i;
            }
        }
    }

    // Full names of the displayable variables of a file.
    auto varNames = [&](size_t fi) {
        std::vector<std::string> names;
        for (const auto& g : groups_) {
            int gid = g.ncid[fi], nvars = 0;
            if (gid < 0 || nc_inq_varids(gid, &nvars, nullptr) != NC_NOERR || nvars <= 0) continue;
            std::vector<int> ids(nvars);
            nc_inq_varids(gid, &nvars, ids.data());
            for (int vid : ids) {
                char name[NC_MAX_NAME + 1];
                nc_inq_varname(gid, vid, name);
                names.push_back(g.path.empty() ? name : g.path + "/" + name);
            }
        }
        std::sort(names.begin(), names.end());
        return names;
    };

    // Concatenate or merge?
    if (nf > 1) {
        if (mode == MultiFile::Merge) merged_ = true;
        else if (mode == MultiFile::Concat) merged_ = false;
        else {
            bool allRec = true, allCoord = true;
            for (size_t fi = 0; fi < nf; ++fi) { allRec = allRec && hasRec(fi); allCoord = allCoord && ftimeCoord[fi]; }
            if (!allRec) merged_ = true;
            else if (allCoord) {
                // Concatenate only when each file's records come after the previous file's.
                bool ordered = true, have = false;
                double last = 0;
                for (size_t fi = 0; fi < nf && ordered; ++fi) {
                    if (ftime[fi].empty()) continue;
                    if (have && !(ftime[fi].front() > last)) ordered = false;
                    last = ftime[fi].back();
                    have = true;
                }
                merged_ = !ordered;
            } else {
                auto n0 = varNames(0);
                for (size_t fi = 1; fi < nf && !merged_; ++fi) merged_ = varNames(fi) != n0;
            }
        }
    }

    if (!merged_) {
        // Aggregate the record dimension across files.
        if (nf > 1) {
            if (recDim_ < 0) {
                err = "multiple files given but the first file has no unlimited (record) dimension";
                return false;
            }
            const GroupInfo& rg = groups_[dims_[recDim_].group];
            size_t total = 0;
            for (size_t fi = 0; fi < nf; ++fi) {
                File& f = files_[fi];
                if (!hasRec(fi)) {
                    err = f.path + ": has no dimension '" + (rg.path.empty() ? "" : rg.path + "/") + dims_[recDim_].name + "'";
                    return false;
                }
                f.recLen = dimLen[fi][recDim_];
                f.recStart = total;
                total += f.recLen;
            }
            dims_[recDim_].len = total;
            dims_[recDim_].isRecord = true;
        } else {
            files_[0].recLen = recDim_ >= 0 ? dims_[recDim_].len : 0;
        }
    } else if (recDim_ >= 0) {
        // Common record axis: the time steps shared by all files that have the record dim.
        // A file with no time step in common is left out of the intersection.
        std::vector<double> common;
        int ref = -1;
        for (size_t fi = 0; fi < nf; ++fi) {
            if (!hasRec(fi)) continue;
            File& f = files_[fi];
            const std::vector<double>& t = ftime[fi];
            if (ref < 0) {
                ref = (int)fi;
                common = t;
                f.tmap.resize(t.size());
                for (size_t i = 0; i < t.size(); ++i) f.tmap[i] = i;
                continue;
            }
            double spacing = 0;
            for (size_t i = 1; i < common.size(); ++i) {
                double d = std::fabs(common[i] - common[i - 1]);
                if (d > 0 && (spacing == 0 || d < spacing)) spacing = d;
            }
            std::vector<std::pair<double, size_t>> sorted;
            for (size_t i = 0; i < t.size(); ++i) sorted.emplace_back(t[i], i);
            std::sort(sorted.begin(), sorted.end());
            std::vector<char> keep(common.size(), 0);
            std::vector<size_t> local;
            for (size_t i = 0; i < common.size(); ++i) {
                double c = common[i];
                double tol = spacing > 0 ? 1e-3 * spacing : 1e-6 * std::max(1.0, std::fabs(c));
                auto it = std::lower_bound(sorted.begin(), sorted.end(), std::make_pair(c - tol, size_t(0)));
                if (it != sorted.end() && std::fabs(it->first - c) <= tol) { keep[i] = 1; local.push_back(it->second); }
            }
            if (local.empty()) {
                f.timeOk = false;
                notes_.push_back(baseName(f.path) + ": no time steps in common with " + baseName(files_[ref].path) +
                                 "; its time-dependent variables are hidden");
                continue;
            }
            auto filter = [&](std::vector<size_t>& m) {
                std::vector<size_t> r;
                for (size_t i = 0; i < keep.size(); ++i) if (keep[i]) r.push_back(m[i]);
                m.swap(r);
            };
            for (size_t pj = 0; pj < fi; ++pj) if (hasRec(pj) && files_[pj].timeOk) filter(files_[pj].tmap);
            std::vector<double> c2;
            for (size_t i = 0; i < keep.size(); ++i) if (keep[i]) c2.push_back(common[i]);
            common.swap(c2);
            f.tmap = local;
        }
        size_t refLen = ref >= 0 ? ftime[ref].size() : 0;
        if (common.size() < refLen)
            notes_.push_back(std::to_string(common.size()) + " of " + std::to_string(refLen) + " time steps of " +
                             baseName(files_[ref].path) + " are common to all files");
        dims_[recDim_].len = common.size();
        dims_[recDim_].isRecord = true;
    }

    // Per file: do its dimensions agree with the combined ones (length; merge: coordinates)?
    std::vector<std::map<int, bool>> dimOk(nf);
    for (size_t di = 0; di < dims_.size(); ++di) {
        if ((int)di == recDim_) { for (size_t fi = 0; fi < nf; ++fi) dimOk[fi][di] = true; continue; }
        const DimInfo& d = dims_[di];
        std::vector<double> refCoord;
        bool refRead = false, refHas = false;
        for (size_t fi = 0; fi < nf; ++fi) {
            auto it = dimLen[fi].find((int)di);
            if (it == dimLen[fi].end()) continue;
            bool ok = it->second == d.len;
            std::string why = ok ? "" : "length " + std::to_string(it->second) + " instead of " + std::to_string(d.len);
            if (ok && merged_) {
                std::vector<double> c;
                bool has = readCoordVar(groups_[d.group].ncid[fi], d.name, d.len, c);
                if (!refRead) { refRead = true; refHas = has; refCoord = c; }
                else if (has && refHas && !sameValues(c, refCoord)) { ok = false; why = "different coordinate values"; }
            }
            dimOk[fi][di] = ok;
            if (!ok) notes_.push_back(baseName(files_[fi].path) + ": dimension '" + d.name + "' has " + why +
                                      "; its variables using it are hidden");
        }
    }
    auto dimsOk = [&](size_t fi, const std::vector<int>& dl) {
        for (int d : dl) if (!dimOk[fi][d]) return false;
        return true;
    };

    std::map<std::string, int> byName;   // fullName -> index into vars_
    for (size_t fi = 0; fi < (merged_ ? nf : 1); ++fi) {
      for (size_t gi = 0; gi < groups_.size(); ++gi) {
        const GroupInfo& g = groups_[gi];
        int gid = g.ncid[fi];
        int nvars = 0;
        if (gid < 0 || nc_inq_varids(gid, &nvars, nullptr) != NC_NOERR || nvars <= 0) continue;
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
                auto it = dimMap[fi].find(vd[k]);
                if (it == dimMap[fi].end()) { ok = false; break; }
                v.dims.push_back(it->second);
            }
            if (!ok) continue;
            const std::string fname = baseName(files_[fi].path);
            if (nf > 1 && !dimsOk(fi, v.dims)) continue;   // noted per dimension above
            v.varid.assign(nf, -1);
            v.ncid.assign(nf, -1);
            if (!merged_) {
                v.isRecordVar = nf > 1 && v.dims[0] == recDim_;
                for (size_t fj = 0; fj < nf; ++fj) {
                    v.ncid[fj] = g.ncid[fj];
                    if (fj == 0) { v.varid[0] = vid; continue; }
                    if (!v.isRecordVar) continue;
                    // A record variable must exist with the same dimensions in every file.
                    int id = -1, nd2 = 0;
                    int vd2[NC_MAX_VAR_DIMS];
                    bool same = g.ncid[fj] >= 0 && nc_inq_varid(g.ncid[fj], name, &id) == NC_NOERR &&
                                nc_inq_var(g.ncid[fj], id, nullptr, nullptr, &nd2, vd2, nullptr) == NC_NOERR && nd2 == nd;
                    for (int k = 0; same && k < nd; ++k) {
                        auto it = dimMap[fj].find(vd2[k]);
                        same = it != dimMap[fj].end() && it->second == v.dims[k];
                    }
                    if (!same || !dimsOk(fj, v.dims)) {
                        notes_.push_back("'" + v.fullName + "' hidden: missing or different in " + baseName(files_[fj].path));
                        ok = false;
                        break;
                    }
                    v.varid[fj] = id;
                }
                if (!ok) continue;
            } else {
                v.file = (int)fi;
                v.varid[fi] = vid;
                v.ncid[fi] = gid;
                auto rp = std::find(v.dims.begin(), v.dims.end(), recDim_);
                if (rp != v.dims.end()) {
                    const File& f = files_[fi];
                    if (!f.timeOk) continue;   // already noted for the whole file
                    bool identity = f.tmap.size() == dimLen[fi][recDim_];
                    for (size_t i = 0; identity && i < f.tmap.size(); ++i) identity = f.tmap[i] == i;
                    if (!identity && rp != v.dims.begin()) {
                        notes_.push_back(fname + ": '" + v.fullName + "' hidden (record dimension is not its first dimension)");
                        continue;
                    }
                    v.isRecordVar = !identity;
                }
                auto prev = byName.find(v.fullName);
                if (prev != byName.end()) {
                    const VarInfo& pv = vars_[prev->second];
                    if (pv.dims == v.dims) {
                        // Same variable on the same grid: keep the first (coordinates, duplicates).
                        bool isCoord = nd == 1 && dims_[v.dims[0]].name == v.name;
                        if (!isCoord)
                            notes_.push_back(fname + ": '" + v.fullName + "' also in " +
                                             baseName(files_[pv.file].path) + ", showing that one");
                        continue;
                    }
                    v.fullName += " (" + fname + ")";   // same name, other dimensions: show both
                }
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
            byName.emplace(v.fullName, (int)vars_.size());
            vars_.push_back(std::move(v));
        }
      }
    }

    loadCoords();
    return true;
}

bool Dataset::reload(std::string& err) {
    // Close first: HDF5 shares an already-open file between handles, so opening it again
    // while the old handle lives would return the stale cached metadata.
    {
        std::lock_guard<std::mutex> lk(g_ncMutex);
        for (auto& f : files_) if (f.ncid >= 0) nc_close(f.ncid);
        for (auto& f : files_) f.ncid = -1;
        closed_ = true;
    }
    Dataset fresh;
    if (!fresh.open(paths_, err, mode_)) return false;
    std::swap(files_, fresh.files_);
    std::swap(groups_, fresh.groups_);
    std::swap(dims_, fresh.dims_);
    std::swap(vars_, fresh.vars_);
    std::swap(notes_, fresh.notes_);
    recDim_ = fresh.recDim_;
    merged_ = fresh.merged_;
    closed_ = false;
    return true;   // fresh now owns the closed old handles (all -1)
}

void Dataset::loadCoords() {
    // Called with g_ncMutex held; readRaw does not lock.
    for (size_t di = 0; di < dims_.size(); ++di) {
        DimInfo& d = dims_[di];
        d.coord.resize(d.len);
        for (size_t i = 0; i < d.len; ++i) d.coord[i] = (double)i;
        int v = -1;
        for (size_t k = 0; k < vars_.size(); ++k)
            if (v < 0 && vars_[k].isCoordVar && vars_[k].dims[0] == (int)di) v = (int)k;
        std::string axis, stdName, positive;
        if (v >= 0 && d.len > 0) {
            std::vector<size_t> st{0}, ct{d.len};
            if (readRaw(v, st, ct, nullptr, d.coord.data(), nullptr)) d.hasCoordVar = true;
            const VarInfo& cv = vars_[v];
            d.units = cv.units;
            d.longName = cv.longName;
            int vid = cv.varid[fileOf(v)], ncid = cv.ncid[fileOf(v)];
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
    if (closed_) {
        if (err) *err = "files closed after a failed reload; reload again";
        return false;
    }
    auto get = [&](int ncid, int varid, const size_t* s, const size_t* c, size_t offset) {
        int st = fout ? nc_get_vara_float(ncid, varid, s, c, fout + offset)
                      : nc_get_vara_double(ncid, varid, s, c, dout + offset);
        if (st == NC_ERANGE) st = NC_NOERR;  // out-of-range values handled as non-finite later
        if (st != NC_NOERR && err) *err = v.name + ": " + nc_strerror(st);
        return st == NC_NOERR;
    };
    const int vf = fileOf(vi);
    if (!v.isRecordVar) return get(v.ncid[vf], v.varid[vf], start.data(), count.data(), 0);

    size_t inner = 1;
    for (size_t k = 1; k < count.size(); ++k) inner *= count[k];
    if (v.file >= 0) {
        // Merged file: map common record indices to this file's, reading runs of consecutive ones.
        const std::vector<size_t>& m = files_[v.file].tmap;
        std::vector<size_t> s = start, c = count;
        for (size_t i = 0; i < count[0];) {
            size_t l = m[start[0] + i], n = 1;
            while (i + n < count[0] && m[start[0] + i + n] == l + n) ++n;
            s[0] = l;
            c[0] = n;
            if (!get(v.ncid[vf], v.varid[vf], s.data(), c.data(), i * inner)) return false;
            i += n;
        }
        return true;
    }

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
    return attrsOf(vars_[vi].ncid[fileOf(vi)], vars_[vi].varid[fileOf(vi)]);
}

std::vector<std::pair<std::string, std::string>> Dataset::groupAttributes(int g) const {
    std::lock_guard<std::mutex> lk(g_ncMutex);
    if (g < 0 || g >= (int)groups_.size()) return {};
    for (int id : groups_[g].ncid)
        if (id >= 0) return attrsOf(id, NC_GLOBAL);
    return {};
}

} // namespace ncvpp
