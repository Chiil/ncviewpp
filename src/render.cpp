#include "render.h"

#include <algorithm>
#include <cmath>
#include <thread>

// Colormaps from the original ncview (GPL-3, David W. Pierce).
#include "data/colormaps_3gauss.h"
#include "data/colormaps_3saw.h"
#include "data/colormaps_banded.h"
#include "data/colormaps_blu_red.h"
#include "data/colormaps_blue_red.h"
#include "data/colormaps_bright.h"
#include "data/colormaps_bw.h"
#include "data/colormaps_default.h"
#include "data/colormaps_detail.h"
#include "data/colormaps_extrema.h"
#include "data/colormaps_helix.h"
#include "data/colormaps_helix2.h"
#include "data/colormaps_hotres.h"
#include "data/colormaps_jaisn2.h"
#include "data/colormaps_jaisnb.h"
#include "data/colormaps_jaisnc.h"
#include "data/colormaps_jaisnd.h"
#include "data/colormaps_jaison.h"
#include "data/colormaps_jet.h"
#include "data/colormaps_manga.h"
#include "data/colormaps_rainbow.h"
#include "data/colormaps_roullet.h"
#include "data/colormaps_ssec.h"
#include "data/colormaps_wheel.h"

namespace ncvpp {

const std::vector<Colormap>& colormaps() {
    static const std::vector<Colormap> maps = {
        {"3gauss", cmap_3gauss},   {"detail", cmap_detail},   {"ssec", cmap_ssec},
        {"bright", cmap_bright},   {"banded", cmap_banded},   {"rainbow", cmap_rainbow},
        {"jet", cmap_jet},         {"hotres", cmap_hotres},   {"bw", cmap_bw},
        {"blu_red", cmap_blu_red}, {"blue_red", cmap_blue_red}, {"extrema", cmap_extrema},
        {"helix", cmap_helix},     {"helix2", cmap_helix2},   {"3saw", cmap_3saw},
        {"default", cmap_default}, {"jaisn2", cmap_jaisn2},   {"jaisnb", cmap_jaisnb},
        {"jaisnc", cmap_jaisnc},   {"jaisnd", cmap_jaisnd},   {"jaison", cmap_jaison},
        {"manga", cmap_manga},     {"roullet", cmap_roullet}, {"wheel", cmap_wheel},
    };
    return maps;
}

int findColormap(const std::string& name) {
    const auto& m = colormaps();
    for (size_t i = 0; i < m.size(); ++i) if (m[i].name == name) return (int)i;
    return -1;
}

const char* transformName(Transform t) {
    switch (t) {
    case Transform::Linear: return "linear";
    case Transform::Low: return "low";
    case Transform::High: return "high";
    case Transform::Log: return "log";
    }
    return "?";
}

float applyTransform(Transform t, float x) {
    switch (t) {
    case Transform::Low: return std::sqrt(x);         // more colours for low values
    case Transform::High: return x * x;               // more colours for high values
    case Transform::Log: return std::log10(1.f + 999.f * x) / 3.f;
    default: return x;
    }
}

std::vector<uint32_t> colormapTable(int cmap, bool invert) {
    const int* c = colormaps()[cmap].rgb;
    std::vector<uint32_t> t(256);
    for (int i = 0; i < 256; ++i) {
        int k = invert ? 255 - i : i;
        t[i] = packRGBA(c[3 * k], c[3 * k + 1], c[3 * k + 2]);
    }
    return t;
}

template <class F>
static void parallelRows(int rows, size_t work, F&& f) {
    unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    unsigned nt = work < 262144 ? 1u : std::min<unsigned>(hw, (unsigned)std::max(1, rows / 16));
    if (nt <= 1) { f(0, rows); return; }
    std::vector<std::thread> th;
    int per = (rows + nt - 1) / nt;
    for (unsigned t = 0; t < nt; ++t) {
        int a = t * per, b = std::min(rows, a + per);
        if (a < b) th.emplace_back([&f, a, b] { f(a, b); });
    }
    for (auto& x : th) x.join();
}

void renderSlice(const Slice& s, const RenderParams& p, std::vector<uint32_t>& out, int& outW, int& outH) {
    const int d = std::max(1, p.decim);
    outW = int((s.nx + d - 1) / d);
    outH = int((s.ny + d - 1) / d);
    out.resize(size_t(outW) * outH);

    // 4096-entry LUT bakes transform + colormap + invert.
    constexpr int N = 4096;
    std::vector<uint32_t> table = colormapTable(p.cmap, p.invert), lut(N);
    const bool trueLog = p.transform == Transform::Log && p.vmin > 0 && p.vmax > p.vmin;
    for (int i = 0; i < N; ++i) {
        float t = float(i) / (N - 1);
        if (!trueLog) t = applyTransform(p.transform, t);
        lut[i] = table[std::clamp(int(t * 255.f + 0.5f), 0, 255)];
    }
    float lo = p.vmin, hi = p.vmax;
    if (trueLog) { lo = std::log(p.vmin); hi = std::log(p.vmax); }
    const float k = hi > lo ? float(N - 1) / (hi - lo) : 0.f;
    const uint32_t miss = p.missing;
    const uint32_t* L = lut.data();

    parallelRows(outH, out.size(), [&](int r0, int r1) {
        for (int r = r0; r < r1; ++r) {
            size_t y = size_t(p.flipY ? (outH - 1 - r) : r) * d;
            const float* src = s.data.data() + y * s.nx;
            uint32_t* dst = out.data() + size_t(r) * outW;
            for (int c = 0; c < outW; ++c) {
                float v = src[size_t(c) * d];
                if (std::isnan(v)) { dst[c] = miss; continue; }
                if (trueLog) v = v > 0 ? std::log(v) : lo;
                float f = (v - lo) * k;
                int idx = f <= 0.f ? 0 : f >= float(N - 1) ? N - 1 : int(f + 0.5f);
                dst[c] = L[idx];
            }
        }
    });
}

std::vector<double> niceTicks(double lo, double hi, int target) {
    std::vector<double> t;
    if (!(hi > lo) || !std::isfinite(lo) || !std::isfinite(hi)) { t.push_back(lo); return t; }
    double raw = (hi - lo) / std::max(1, target);
    double mag = std::pow(10.0, std::floor(std::log10(raw)));
    double r = raw / mag;
    double step = (r < 1.5 ? 1 : r < 3 ? 2 : r < 7 ? 5 : 10) * mag;
    for (double v = std::ceil(lo / step) * step; v <= hi + step * 1e-9; v += step)
        t.push_back(std::fabs(v) < step * 1e-9 ? 0.0 : v);
    return t;
}

} // namespace ncvpp
