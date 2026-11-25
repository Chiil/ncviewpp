#include "overlay.h"

#include <algorithm>
#include <cmath>

#include "data/overlay_coasts_p08deg.h"
#include "data/overlay_coasts_p8deg.h"

namespace ncvpp {

// Nearest index of value v in a monotonic coordinate, or -1 if outside the grid.
static long nearest(const std::vector<double>& c, double v) {
    size_t n = c.size();
    if (n == 0) return -1;
    if (n == 1) return 0;
    bool inc = c[n - 1] > c[0];
    double lo = inc ? c[0] : c[n - 1], hi = inc ? c[n - 1] : c[0];
    double half = std::fabs(c[1] - c[0]) * 0.5;
    if (v < lo - half || v > hi + half) return -1;
    size_t i = inc ? std::lower_bound(c.begin(), c.end(), v) - c.begin()
                   : std::lower_bound(c.begin(), c.end(), v, [](double a, double b) { return a > b; }) - c.begin();
    if (i >= n) return long(n - 1);
    if (i > 0 && std::fabs(c[i - 1] - v) < std::fabs(c[i] - v)) --i;
    return long(i);
}

std::vector<uint32_t> overlayCells(Overlay o, const DimInfo& xd, const DimInfo& yd) {
    std::vector<uint32_t> cells;
    if (o == Overlay::None || !xd.monotonic || !yd.monotonic) return cells;
    const float* pts = o == Overlay::Coast ? overlay_coasts_p8deg : overlay_coasts_p08deg;
    long npts = (o == Overlay::Coast ? n_overlay_coasts_p8deg : n_overlay_coasts_p08deg) / 2;
    size_t nx = xd.coord.size();
    for (long i = 0; i < npts; ++i) {
        long ix = nearest(xd.coord, pts[2 * i]), iy = nearest(yd.coord, pts[2 * i + 1]);
        if (ix >= 0 && iy >= 0) cells.push_back(uint32_t(iy * nx + ix));
    }
    std::sort(cells.begin(), cells.end());
    cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
    return cells;
}

} // namespace ncvpp
