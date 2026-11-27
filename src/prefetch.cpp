#include "prefetch.h"

#ifdef NCVPP_HAVE_HDF5
#include <hdf5.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <thread>
#endif

namespace ncvpp {

#ifndef NCVPP_HAVE_HDF5
void prefetchCoords(const std::vector<std::string>&) {}
#else

namespace {

struct Range { size_t file; haddr_t addr; hsize_t size; };

struct Ctx {
    size_t file;
    std::vector<Range>* out;
};

// Only small variables: coordinates, not the data that happens to be a dimension scale.
constexpr hsize_t kMaxElems = 1u << 20;
constexpr hsize_t kMaxBytes = 64u << 20;   // per file

int chunkCb(const hsize_t*, unsigned, haddr_t addr, hsize_t size, void* data) {
    auto* c = static_cast<Ctx*>(data);
    if (addr != HADDR_UNDEF && size > 0) c->out->push_back({c->file, addr, size});
    return H5_ITER_CONT;
}

bool isDimScale(hid_t ds) {
    // netCDF-4 coordinate variables are HDF5 dimension scales (attribute CLASS).
    if (H5Aexists(ds, "CLASS") <= 0) return false;
    hid_t a = H5Aopen(ds, "CLASS", H5P_DEFAULT);
    if (a < 0) return false;
    bool ok = false;
    hid_t t = H5Aget_type(a);
    if (t >= 0 && H5Tget_class(t) == H5T_STRING && !H5Tis_variable_str(t)) {
        size_t n = H5Tget_size(t);
        std::string s(n, '\0');
        if (H5Aread(a, t, s.data()) >= 0) ok = s.compare(0, 15, "DIMENSION_SCALE") == 0;
    }
    if (t >= 0) H5Tclose(t);
    H5Aclose(a);
    return ok;
}

herr_t visitCb(hid_t loc, const char* name, const H5O_info2_t* info, void* data) {
    if (info->type != H5O_TYPE_DATASET) return 0;
    auto* c = static_cast<Ctx*>(data);
    hid_t ds = H5Dopen2(loc, name, H5P_DEFAULT);
    if (ds < 0) return 0;
    hid_t sp = H5Dget_space(ds);
    hsize_t n = 0;
    if (sp >= 0 && H5Sget_simple_extent_ndims(sp) == 1) H5Sget_simple_extent_dims(sp, &n, nullptr);
    if (sp >= 0) H5Sclose(sp);
    if (n > 0 && n <= kMaxElems && isDimScale(ds)) {
        hid_t pl = H5Dget_create_plist(ds);
        H5D_layout_t lay = pl >= 0 ? H5Pget_layout(pl) : H5D_LAYOUT_ERROR;
        if (pl >= 0) H5Pclose(pl);
        if (lay == H5D_CHUNKED) {
            H5Dchunk_iter(ds, H5P_DEFAULT, chunkCb, c);
        } else if (lay == H5D_CONTIGUOUS) {
            haddr_t a = H5Dget_offset(ds);
            hsize_t s = H5Dget_storage_size(ds);
            if (a != HADDR_UNDEF && s > 0) c->out->push_back({c->file, a, s});
        }
    }
    H5Dclose(ds);
    return 0;
}

} // namespace

void prefetchCoords(const std::vector<std::string>& paths) {
    std::vector<Range> ranges;
    H5E_BEGIN_TRY {
        for (size_t fi = 0; fi < paths.size(); ++fi) {
            if (H5Fis_accessible(paths[fi].c_str(), H5P_DEFAULT) <= 0) continue;   // not HDF5
            hid_t f = H5Fopen(paths[fi].c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
            if (f < 0) continue;
            std::vector<Range> fr;
            Ctx c{fi, &fr};
            H5Ovisit3(f, H5_INDEX_NAME, H5_ITER_NATIVE, visitCb, &c, H5O_INFO_BASIC);
            H5Fclose(f);   // closed before netCDF opens it, so no shared-handle surprises
            hsize_t total = 0;
            for (const Range& r : fr) {
                if ((total += r.size) > kMaxBytes) break;
                ranges.push_back(r);
            }
        }
    } H5E_END_TRY
    if (ranges.empty()) return;

    // Plain POSIX reads from here on: safe to run concurrently.
    std::vector<int> fds(paths.size(), -1);
    for (const Range& r : ranges)
        if (fds[r.file] < 0) fds[r.file] = ::open(paths[r.file].c_str(), O_RDONLY);
    std::atomic<size_t> next{0};
    auto worker = [&] {
        std::vector<char> buf;
        for (size_t i; (i = next.fetch_add(1)) < ranges.size();) {
            const Range& r = ranges[i];
            if (fds[r.file] < 0) continue;
            buf.resize(r.size);
            size_t done = 0;
            while (done < r.size) {
                ssize_t k = ::pread(fds[r.file], buf.data() + done, r.size - done, (off_t)(r.addr + done));
                if (k <= 0) break;
                done += (size_t)k;
            }
        }
    };
    size_t nt = std::min<size_t>(64, ranges.size());
    std::vector<std::thread> th;
    for (size_t t = 1; t < nt; ++t) th.emplace_back(worker);
    worker();
    for (auto& t : th) t.join();
    for (int fd : fds) if (fd >= 0) ::close(fd);
}

#endif

} // namespace ncvpp
