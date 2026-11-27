#include "export.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "data/font_roboto.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace ncvpp {

const unsigned char* uiFontTTF(int* len) {
    if (len) *len = (int)roboto_medium_ttf_len;
    return roboto_medium_ttf;
}

namespace {

std::string fmtG(double v) {
    char b[48];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

// ------------------------------------------------------------------ canvas and text

struct Canvas {
    int w = 0, h = 0;
    std::vector<unsigned char> rgb;
    Canvas(int w_, int h_) : w(w_), h(h_), rgb(size_t(w_) * h_ * 3, 255) {}
    void blend(int x, int y, uint32_t c, float a) {
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
        a = std::min(a, 1.f);
        unsigned char* p = &rgb[(size_t(y) * w + x) * 3];
        for (int k = 0; k < 3; ++k) p[k] = (unsigned char)std::lround(p[k] * (1 - a) + ((c >> (8 * k)) & 255) * a);
    }
    void rect(int x0, int y0, int x1, int y1, uint32_t c) {   // filled, [x0,x1) x [y0,y1)
        x0 = std::max(x0, 0); y0 = std::max(y0, 0); x1 = std::min(x1, w); y1 = std::min(y1, h);
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                unsigned char* p = &rgb[(size_t(y) * w + x) * 3];
                p[0] = c & 255; p[1] = (c >> 8) & 255; p[2] = (c >> 16) & 255;
            }
    }
    void frame(int x0, int y0, int x1, int y1, int lw, uint32_t c) {   // outline just outside the box
        rect(x0 - lw, y0 - lw, x1 + lw, y0, c);
        rect(x0 - lw, y1, x1 + lw, y1 + lw, c);
        rect(x0 - lw, y0, x0, y1, c);
        rect(x1, y0, x1 + lw, y1, c);
    }
    void ring(float cx, float cy, float r, float lw, uint32_t c) {
        int x0 = int(cx - r - lw - 1), x1 = int(cx + r + lw + 1), y0 = int(cy - r - lw - 1), y1 = int(cy + r + lw + 1);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                float d = std::fabs(std::hypot(x + 0.5f - cx, y + 0.5f - cy) - r);
                blend(x, y, c, lw * 0.5f + 0.5f - d);
            }
    }
};

std::vector<int> utf8(const std::string& s) {
    std::vector<int> cp;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        int n = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
        int v = n == 0 ? c : c & (0x3F >> n);
        for (int k = 1; k <= n && i + k < s.size(); ++k) v = (v << 6) | (s[i + k] & 0x3F);
        cp.push_back(v);
        i += n + 1;
    }
    return cp;
}

class Font {
public:
    Font() { ok_ = stbtt_InitFont(&info_, roboto_medium_ttf, stbtt_GetFontOffsetForIndex(roboto_medium_ttf, 0)) != 0; }

    // Size of the text box (width x line height) at pixel height px.
    void size(const std::string& s, float px, int& w, int& h) const {
        float sc = stbtt_ScaleForPixelHeight(&info_, px);
        int asc, desc, gap;
        stbtt_GetFontVMetrics(&info_, &asc, &desc, &gap);
        h = int(std::ceil((asc - desc) * sc));
        w = int(std::ceil(advance(utf8(s), sc)));
    }
    int width(const std::string& s, float px) const { int w, h; size(s, px, w, h); return w; }
    int height(float px) const { int w, h; size("", px, w, h); return h; }

    // Draws with the top-left of the text box at (x, y). rot: text runs bottom-to-top.
    void draw(Canvas& cv, int x, int y, const std::string& s, float px, uint32_t c, bool rot = false) const {
        if (!ok_) return;
        float sc = stbtt_ScaleForPixelHeight(&info_, px);
        int asc, desc, gap;
        stbtt_GetFontVMetrics(&info_, &asc, &desc, &gap);
        auto cps = utf8(s);
        int bw = int(std::ceil(advance(cps, sc))) + 2, bh = int(std::ceil((asc - desc) * sc));
        float base = asc * sc;
        std::vector<unsigned char> buf(size_t(bw) * bh, 0);
        float pen = 0;
        for (size_t i = 0; i < cps.size(); ++i) {
            int g = stbtt_FindGlyphIndex(&info_, cps[i]);
            int adv, lsb, x0, y0, x1, y1;
            stbtt_GetGlyphHMetrics(&info_, g, &adv, &lsb);
            float sx = pen - std::floor(pen);
            stbtt_GetGlyphBitmapBoxSubpixel(&info_, g, sc, sc, sx, 0, &x0, &y0, &x1, &y1);
            int gw = x1 - x0, gh = y1 - y0;
            if (gw > 0 && gh > 0) {
                std::vector<unsigned char> gb(size_t(gw) * gh);
                stbtt_MakeGlyphBitmapSubpixel(&info_, gb.data(), gw, gh, gw, sc, sc, sx, 0, g);
                int ox = int(std::floor(pen)) + x0, oy = int(std::lround(base)) + y0;
                for (int v = 0; v < gh; ++v)
                    for (int u = 0; u < gw; ++u) {
                        int bx = ox + u, by = oy + v;
                        if (bx >= 0 && by >= 0 && bx < bw && by < bh)
                            buf[size_t(by) * bw + bx] = (unsigned char)std::min(255, buf[size_t(by) * bw + bx] + gb[size_t(v) * gw + u]);
                    }
            }
            pen += adv * sc;
            if (i + 1 < cps.size()) pen += stbtt_GetCodepointKernAdvance(&info_, cps[i], cps[i + 1]) * sc;
        }
        for (int v = 0; v < bh; ++v)
            for (int u = 0; u < bw; ++u) {
                unsigned char a = buf[size_t(v) * bw + u];
                if (!a) continue;
                if (rot) cv.blend(x + v, y + bw - 1 - u, c, a / 255.f);
                else cv.blend(x + u, y + v, c, a / 255.f);
            }
    }

private:
    float advance(const std::vector<int>& cps, float sc) const {
        if (!ok_) return 0;
        float w = 0;
        for (size_t i = 0; i < cps.size(); ++i) {
            int adv, lsb;
            stbtt_GetCodepointHMetrics(&info_, cps[i], &adv, &lsb);
            w += adv * sc;
            if (i + 1 < cps.size()) w += stbtt_GetCodepointKernAdvance(&info_, cps[i], cps[i + 1]) * sc;
        }
        return w;
    }
    stbtt_fontinfo info_{};
    bool ok_ = false;
};

// ------------------------------------------------------------------ axes

struct Tick { float pos; std::string label; };

// Fractional data index of coordinate value v (monotonic coordinates), or v itself without one.
double coordToIndex(const DimInfo& d, size_t n, double v) {
    const auto& c = d.coord;
    if (c.size() != n || n < 2 || !d.monotonic) return v;
    bool inc = c[n - 1] > c[0];
    size_t k = 0;
    if (inc) k = std::upper_bound(c.begin(), c.end(), v) - c.begin();
    else k = std::upper_bound(c.begin(), c.end(), v, [](double a, double b) { return a > b; }) - c.begin();
    k = std::clamp<size_t>(k, 1, n - 1);
    return (k - 1) + (v - c[k - 1]) / (c[k] - c[k - 1]);
}

// Ticks along an axis of n cells drawn over len pixels; pos(i) maps a fractional index to pixels.
// spacing(labels) returns the pixels the labels need per tick, to thin out crowded axes.
template <class Pos, class Spacing>
std::vector<Tick> axisTicks(const DimInfo& d, size_t n, float len, Pos pos, Spacing spacing) {
    const bool haveCoord = d.coord.size() == n && n > 0 && d.monotonic;
    double lo = haveCoord ? std::min(d.coord.front(), d.coord.back()) : 0;
    double hi = haveCoord ? std::max(d.coord.front(), d.coord.back()) : double(n ? n - 1 : 0);
    auto label = [&](double v) { return haveCoord ? d.formatValue(v) : fmtG(v); };
    std::vector<Tick> out;
    if (n == 1) { out.push_back({pos(0.0), label(lo)}); return out; }
    for (int target = 10; target >= 2; --target) {
        out.clear();
        for (double v : niceTicks(lo, hi, target)) {
            if (!d.hasCoordVar) {   // index axes: integers only
                if (std::fabs(v - std::round(v)) > 1e-6) continue;
                v = std::round(v);
            }
            out.push_back({pos(haveCoord ? coordToIndex(d, n, v) : v), label(v)});
        }
        std::vector<std::string> labels;
        for (auto& t : out) labels.push_back(t.label);
        if (out.size() < 2 || spacing(labels) * out.size() <= len) break;
    }
    return out;
}

std::string axisLabel(const DimInfo& d) {
    std::string s = d.longName.empty() ? d.name : d.longName;
    if (!d.units.empty() && !d.time.valid()) s += " [" + d.units + "]";
    return s;
}

} // namespace

ExportSpec makeExportSpec(const Dataset& ds, const SliceKey& key, const Slice& s, const RenderParams& rp) {
    ExportSpec e;
    const VarInfo& vi = ds.vars()[key.var];
    e.slice = &s;
    e.rp = rp;
    e.xdim = &ds.dims()[vi.dims[key.xd]];
    e.ydim = &ds.dims()[vi.dims[key.yd]];
    e.title = vi.longName.empty() ? vi.fullName : vi.longName;
    auto shape = ds.shape(key.var);
    for (size_t i = 0; i < vi.dims.size(); ++i) {
        if ((int)i == key.xd || (int)i == key.yd || shape[i] <= 1) continue;
        const DimInfo& d = ds.dims()[vi.dims[i]];
        if (!e.subtitle.empty()) e.subtitle += ",  ";
        e.subtitle += d.name + " = " + d.format(key.pos[i]);
        if (!d.units.empty() && !d.time.valid()) e.subtitle += " " + d.units;
    }
    e.cbarLabel = vi.units;
    return e;
}

std::string exportFileName(const Dataset& ds, const SliceKey& key, int animDim) {
    std::string n = ds.vars()[key.var].fullName;
    std::replace(n.begin(), n.end(), '/', '_');
    if (animDim >= 0 && animDim < (int)key.pos.size()) {
        char b[32];
        std::snprintf(b, sizeof b, "_%04zu", key.pos[animDim]);
        n += b;
    }
    return n + ".png";
}

bool exportPNG(const std::string& path, const ExportSpec& e, std::string& err) {
    const Slice& s = *e.slice;
    if (s.nx == 0 || s.ny == 0 || s.data.size() != s.nx * s.ny) { err = "no data to export"; return false; }
    const Font font;
    const uint32_t black = packRGBA(0, 0, 0), grey = packRGBA(80, 80, 80);
    const float pt = e.dpi / 72.f;
    const float fsTick = 9 * pt, fsLabel = 10 * pt, fsTitle = 12 * pt, fsSub = 9 * pt;
    const int lw = std::max(1, int(std::lround(0.75f * pt)));          // 0.75 pt lines
    const int tickLen = int(4 * pt), gap = int(4 * pt), pad = int(10 * pt);
    const int hTick = font.height(fsTick), hLabel = font.height(fsLabel);

    // Data image, upscaled by an integer factor.
    RenderParams rp = e.rp;
    rp.decim = 1;
    std::vector<uint32_t> px;
    int nx = 0, ny = 0;
    renderSlice(s, rp, px, nx, ny);
    const int S = std::max(1, int(std::ceil(double(e.minSize) / std::max(nx, ny))));
    const int iw = nx * S, ih = ny * S;
    if (double(iw) * ih > 4e8) { err = "image too large"; return false; }

    // Axes and ticks (positions relative to the image's top-left corner).
    auto xpos = [&](double i) { return float((i + 0.5) * S); };
    auto ypos = [&](double j) { return float((rp.flipY ? (ny - 1 - j) : j) + 0.5) * S; };
    auto xs = axisTicks(*e.xdim, s.nx, iw, xpos, [&](const std::vector<std::string>& l) {
        int m = 0;
        for (auto& t : l) m = std::max(m, font.width(t, fsTick));
        return float(m + 2 * gap);
    });
    auto ys = axisTicks(*e.ydim, s.ny, ih, ypos, [&](const std::vector<std::string>&) { return float(hTick * 2); });
    int ytw = 0;
    for (auto& t : ys) ytw = std::max(ytw, font.width(t.label, fsTick));

    // Colour bar: vertical on the right, or horizontal below for wide images.
    const bool trueLog = rp.transform == Transform::Log && rp.vmin > 0 && rp.vmax > rp.vmin;
    const bool cbHoriz = iw > 2 * ih;
    const int cbThick = int(10 * pt);
    const int cbLen = cbHoriz ? iw : std::max(ih, int(80 * pt));
    std::vector<double> cvals;
    if (rp.vmax > rp.vmin) {
        if (trueLog) {
            for (double ex = std::floor(std::log10(rp.vmin)); ex <= std::ceil(std::log10(rp.vmax)); ex += 1) {
                double v = std::pow(10.0, ex);
                if (v >= rp.vmin && v <= rp.vmax) cvals.push_back(v);
            }
            if (cvals.size() < 2) cvals = {rp.vmin, rp.vmax};
        } else {
            float per = cbHoriz ? fsTick * 6 : hTick * 2.5f;
            cvals = niceTicks(rp.vmin, rp.vmax, std::max(2, int(cbLen / per)));
        }
    } else {
        cvals = {rp.vmin};
    }
    int ctw = 0;
    for (double v : cvals) ctw = std::max(ctw, font.width(fmtG(v), fsTick));
    const bool hasCbLabel = !e.cbarLabel.empty();

    // Layout.
    const std::string xl = axisLabel(*e.xdim), yl = axisLabel(*e.ydim);
    const int titleH = font.height(fsTitle), subH = e.subtitle.empty() ? 0 : font.height(fsSub) + gap / 2;
    const int left = pad + hLabel + gap + ytw + gap + tickLen;
    const int top = pad + titleH + subH + 2 * gap;
    const int belowAxes = tickLen + gap + hTick + gap + hLabel;
    int right = pad, bottom = pad + belowAxes;
    if (cbHoriz) bottom += 3 * gap + cbThick + tickLen + gap + hTick + (hasCbLabel ? gap + hLabel : 0);
    else right += 4 * gap + cbThick + tickLen + gap + ctw + (hasCbLabel ? 2 * gap + hLabel : 0);
    int W = left + iw + right;
    int H = top + std::max(ih, cbHoriz ? 0 : cbLen) + bottom;
    W = std::max(W, font.width(e.title, fsTitle) + 2 * pad);
    W = std::max(W, font.width(e.subtitle, fsSub) + 2 * pad);
    Canvas cv(W, H);
    const int ix0 = left, iy0 = top + (cbHoriz ? 0 : std::max(0, (cbLen - ih) / 2));

    // Title.
    font.draw(cv, std::max(pad, left + iw / 2 - font.width(e.title, fsTitle) / 2), pad, e.title, fsTitle, black);
    if (!e.subtitle.empty()) {
        int sw = font.width(e.subtitle, fsSub);
        font.draw(cv, std::max(pad, left + iw / 2 - sw / 2), pad + titleH + gap / 2, e.subtitle, fsSub, grey);
    }

    // Image with overlay and markers.
    for (int y = 0; y < ih; ++y) {
        const uint32_t* row = &px[size_t(y / S) * nx];
        unsigned char* dst = &cv.rgb[(size_t(iy0 + y) * W + ix0) * 3];
        for (int x = 0; x < iw; ++x) {
            uint32_t c = row[x / S];
            dst[3 * x] = c & 255; dst[3 * x + 1] = (c >> 8) & 255; dst[3 * x + 2] = (c >> 16) & 255;
        }
    }
    if (e.overlay != Overlay::None && e.xdim->kind == AxisKind::Lon && e.ydim->kind == AxisKind::Lat) {
        for (uint32_t c : overlayCells(e.overlay, *e.xdim, *e.ydim)) {
            size_t i = c % s.nx, j = c / s.nx;
            int col = int(i), row = rp.flipY ? int(ny - 1 - j) : int(j);
            cv.rect(ix0 + col * S, iy0 + row * S, ix0 + (col + 1) * S, iy0 + (row + 1) * S, black);
        }
    }
    for (auto& m : e.markers) {
        float cx = ix0 + xpos(m.ix), cy = iy0 + ypos(m.iy), r = 4 * pt;
        cv.ring(cx, cy, r, 3.f * lw, black);
        cv.ring(cx, cy, r, 1.5f * lw, m.rgba);
    }
    cv.frame(ix0, iy0, ix0 + iw, iy0 + ih, lw, black);

    // X axis.
    for (auto& t : xs) {
        int x = ix0 + int(std::lround(t.pos));
        cv.rect(x - lw / 2, iy0 + ih + lw, x - lw / 2 + lw, iy0 + ih + lw + tickLen, black);
        int w = font.width(t.label, fsTick);
        font.draw(cv, std::clamp(x - w / 2, 0, W - w), iy0 + ih + lw + tickLen + gap, t.label, fsTick, black);
    }
    font.draw(cv, ix0 + iw / 2 - font.width(xl, fsLabel) / 2, iy0 + ih + lw + tickLen + gap + hTick + gap, xl, fsLabel, black);
    // Y axis.
    for (auto& t : ys) {
        int y = iy0 + int(std::lround(t.pos));
        cv.rect(ix0 - lw - tickLen, y - lw / 2, ix0 - lw, y - lw / 2 + lw, black);
        font.draw(cv, ix0 - lw - tickLen - gap - font.width(t.label, fsTick), y - hTick / 2, t.label, fsTick, black);
    }
    {
        int w = font.width(yl, fsLabel);
        font.draw(cv, pad, std::max(0, iy0 + ih / 2 - w / 2), yl, fsLabel, black, true);
    }

    // Colour bar.
    {
        auto tbl = colormapTable(rp.cmap, rp.invert);
        int bx0, by0, bx1, by1;
        if (cbHoriz) { bx0 = ix0; bx1 = ix0 + iw; by0 = iy0 + ih + lw + belowAxes + 3 * gap; by1 = by0 + cbThick; }
        else { bx0 = ix0 + iw + lw + 4 * gap; bx1 = bx0 + cbThick; by0 = top; by1 = top + cbLen; }
        const int len = cbHoriz ? bx1 - bx0 : by1 - by0;
        for (int i = 0; i < len; ++i) {
            float t = (i + 0.5f) / len;
            float tt = trueLog ? t : applyTransform(rp.transform, t);
            uint32_t c = tbl[std::clamp(int(tt * 255 + 0.5f), 0, 255)];
            if (cbHoriz) cv.rect(bx0 + i, by0, bx0 + i + 1, by1, c);
            else cv.rect(bx0, by1 - 1 - i, bx1, by1 - i, c);
        }
        cv.frame(bx0, by0, bx1, by1, lw, black);
        auto frac = [&](double v) {
            return trueLog ? (std::log(v) - std::log(rp.vmin)) / (std::log(rp.vmax) - std::log(rp.vmin))
                           : rp.vmax > rp.vmin ? (v - rp.vmin) / (double(rp.vmax) - rp.vmin) : 0.5;
        };
        for (double v : cvals) {
            std::string l = fmtG(v);
            int w = font.width(l, fsTick);
            if (cbHoriz) {
                int x = bx0 + int(std::lround(frac(v) * len));
                cv.rect(x - lw / 2, by1 + lw, x - lw / 2 + lw, by1 + lw + tickLen, black);
                font.draw(cv, std::clamp(x - w / 2, 0, W - w), by1 + lw + tickLen + gap, l, fsTick, black);
            } else {
                int y = by1 - int(std::lround(frac(v) * len));
                cv.rect(bx1 + lw, y - lw / 2, bx1 + lw + tickLen, y - lw / 2 + lw, black);
                font.draw(cv, bx1 + lw + tickLen + gap, y - hTick / 2, l, fsTick, black);
            }
        }
        if (hasCbLabel) {
            int w = font.width(e.cbarLabel, fsLabel);
            if (cbHoriz) font.draw(cv, bx0 + len / 2 - w / 2, by1 + lw + tickLen + gap + hTick + gap, e.cbarLabel, fsLabel, black);
            else font.draw(cv, bx1 + lw + tickLen + gap + ctw + 2 * gap, by0 + len / 2 - w / 2, e.cbarLabel, fsLabel, black, true);
        }
    }

    // Encode, then add a pHYs chunk so viewers and word processors pick up the DPI.
    int n = 0;
    unsigned char* png = stbi_write_png_to_mem(cv.rgb.data(), W * 3, W, H, 3, &n);
    if (!png) { err = "PNG encoding failed"; return false; }
    unsigned char phys[21];
    const uint32_t ppm = uint32_t(std::lround(e.dpi / 0.0254));
    auto be32 = [](unsigned char* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; };
    be32(phys, 9);
    std::memcpy(phys + 4, "pHYs", 4);
    be32(phys + 8, ppm);
    be32(phys + 12, ppm);
    phys[16] = 1;   // unit: metre
    be32(phys + 17, stbiw__crc32(phys + 4, 13));
    FILE* f = std::fopen(path.c_str(), "wb");
    bool ok = f != nullptr;
    if (ok) {
        const int ihdrEnd = 8 + 25;   // signature + IHDR chunk
        ok = std::fwrite(png, 1, ihdrEnd, f) == size_t(ihdrEnd) && std::fwrite(phys, 1, 21, f) == 21 &&
             std::fwrite(png + ihdrEnd, 1, n - ihdrEnd, f) == size_t(n - ihdrEnd);
        ok = std::fclose(f) == 0 && ok;
    }
    STBIW_FREE(png);
    if (!ok) { err = path + ": " + std::strerror(errno); return false; }
    return true;
}

} // namespace ncvpp
