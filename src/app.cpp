#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>

#include "imgui.h"

namespace ncvpp {

static const ImU32 kLineColors[] = {IM_COL32(255, 200, 60, 255), IM_COL32(90, 200, 255, 255),
                                    IM_COL32(255, 110, 110, 255), IM_COL32(120, 230, 120, 255),
                                    IM_COL32(220, 140, 255, 255), IM_COL32(255, 255, 255, 255)};

static std::string fmtG(double v) {
    char b[48];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

static double nowSec() { return SDL_GetTicksNS() * 1e-9; }

App::App(SDL_Renderer* ren, Dataset& ds, const Options& opt)
    : ren_(ren), ds_(ds), loader_(ds, opt.cacheMB << 20), opt_(opt) {
    // Wake the (event-driven) main loop whenever background I/O finishes.
    loader_.onDone = [] {
        SDL_Event e;
        SDL_zero(e);
        e.type = SDL_EVENT_USER;
        SDL_PushEvent(&e);
    };
    if (ren_) {
        SDL_PropertiesID props = SDL_GetRendererProperties(ren_);
        maxTex_ = (int)SDL_GetNumberProperty(props, SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 8192);
        if (maxTex_ <= 0) maxTex_ = 16384;
    }
    if (!opt.cmap.empty()) {
        int c = findColormap(opt.cmap);
        if (c >= 0) rp_.cmap = c;
        else SDL_Log("ncview++: unknown colormap '%s'", opt.cmap.c_str());
    }

    // Initial variable: requested, else the first with >=2 non-trivial dims, else first.
    int v = -1;
    if (!opt.var.empty()) {
        v = ds_.findVar(opt.var);
        if (v < 0) SDL_Log("ncview++: variable '%s' not found", opt.var.c_str());
    }
    for (size_t i = 0; v < 0 && i < ds_.vars().size(); ++i) {
        int big = 0;
        for (size_t s : ds_.shape((int)i)) big += s > 1;
        if (big >= 2 && !ds_.vars()[i].isCoordVar) v = (int)i;
    }
    for (size_t i = 0; v < 0 && i < ds_.vars().size(); ++i)
        if (!ds_.vars()[i].isCoordVar) v = (int)i;
    if (v < 0 && !ds_.vars().empty()) v = 0;
    if (v >= 0) selectVar(v);
    if (animd_ >= 0 && opt.frame > 0) setFrame(std::min<size_t>(opt.frame, ds_.shape(var_)[animd_] - 1));
    if (opt.coasts) overlay_ = Overlay::Coast;
    if (opt.haveMin) { vmin_ = opt.vmin; rangeUser_ = true; }
    if (opt.haveMax) { vmax_ = opt.vmax; rangeUser_ = true; }
    if (opt.haveMin && opt.haveMax) rangeValid_ = true;
}

App::~App() {
    loader_.shutdown();  // jobs reference members; stop the thread before anything is destroyed
    if (tex_) SDL_DestroyTexture(tex_);
}

std::string App::title() const {
    std::string t = "ncview++";
    if (var_ >= 0) t += " — " + ds_.vars()[var_].fullName;
    if (!ds_.paths().empty()) {
        std::string p = ds_.paths()[0];
        size_t s = p.find_last_of('/');
        t += " — " + (s == std::string::npos ? p : p.substr(s + 1));
        if (ds_.paths().size() > 1) t += " (+" + std::to_string(ds_.paths().size() - 1) + " files)";
    }
    return t;
}

int App::waitTimeoutMs() const {
    if (playDir_ != 0) {
        double rem = 1.0 / fps_ - (nowSec() - lastAdvance_);
        return std::max(1, int(rem * 1000));
    }
    return -1;
}

bool App::isImageVar() const { return var_ >= 0 && ds_.vars()[var_].dims.size() >= 2 && xd_ >= 0 && yd_ >= 0; }

// ------------------------------------------------------------------ selection

void App::defaultAxes() {
    const auto shape = ds_.shape(var_);
    int n = (int)shape.size();
    xd_ = yd_ = animd_ = -1;
    if (n < 2) { animd_ = -1; return; }
    // Last two dims with length > 1, else simply the last two.
    std::vector<int> big;
    for (int i = n - 1; i >= 0 && big.size() < 2; --i) if (shape[i] > 1) big.push_back(i);
    if (big.size() == 2) { xd_ = big[0]; yd_ = big[1]; }
    else { xd_ = n - 1; yd_ = n - 2; }
}

// Orient so the Y coordinate increases upward (north up for both N→S and S→N grids).
void App::autoFlip() {
    if (var_ < 0 || yd_ < 0) return;
    const DimInfo& d = ds_.dims()[ds_.vars()[var_].dims[yd_]];
    bool decreasing = d.coord.size() > 1 && d.coord.back() < d.coord.front();
    rp_.flipY = !decreasing;
    renderDirty_ = true;
}

void App::selectVar(int v) {
    if (v < 0 || v >= (int)ds_.vars().size()) return;
    // Remember the current indices by global dim, and the current axes by dim name.
    std::string prevX, prevY;
    if (var_ >= 0) {
        const auto& od = ds_.vars()[var_].dims;
        for (size_t i = 0; i < od.size() && i < pos_.size(); ++i) dimPos_[od[i]] = pos_[i];
        if (xd_ >= 0) prevX = ds_.dims()[od[xd_]].name;
        if (yd_ >= 0) prevY = ds_.dims()[od[yd_]].name;
    }
    bool sameGrid = false;
    var_ = v;
    const VarInfo& vi = ds_.vars()[v];
    const auto shape = ds_.shape(v);
    pos_.assign(vi.dims.size(), 0);
    for (size_t i = 0; i < vi.dims.size(); ++i) {
        auto it = dimPos_.find(vi.dims[i]);
        if (it != dimPos_.end()) pos_[i] = std::min(it->second, shape[i] ? shape[i] - 1 : 0);
    }
    defaultAxes();
    for (size_t i = 0; i < vi.dims.size() && vi.dims.size() >= 2; ++i) {
        const std::string& n = ds_.dims()[vi.dims[i]].name;
        if (n == prevX && (int)i != yd_) xd_ = (int)i;
    }
    for (size_t i = 0; i < vi.dims.size() && vi.dims.size() >= 2; ++i) {
        const std::string& n = ds_.dims()[vi.dims[i]].name;
        if (n == prevY && (int)i != xd_) yd_ = (int)i;
    }
    if (xd_ == yd_) defaultAxes();
    autoFlip();
    if (shownKey_.var >= 0 && xd_ >= 0 && yd_ >= 0 && shown_ &&
        shown_->nx == shape[xd_] && shown_->ny == shape[yd_]) sameGrid = true;

    // Animation dim: prefer a time dim, else the first remaining dim with length > 1.
    animd_ = -1;
    for (size_t i = 0; i < shape.size(); ++i)
        if ((int)i != xd_ && (int)i != yd_ && shape[i] > 1 && ds_.dims()[vi.dims[i]].kind == AxisKind::Time) { animd_ = (int)i; break; }
    for (size_t i = 0; animd_ < 0 && i < shape.size(); ++i)
        if ((int)i != xd_ && (int)i != yd_ && shape[i] > 1) animd_ = (int)i;
    if (vi.dims.size() == 1) animd_ = -1;
    plotDim_ = animd_ >= 0 ? animd_ : (xd_ >= 0 ? xd_ : 0);

    loader_.dropPrefetch();
    shown_.reset();
    renderDirty_ = true;
    rangeValid_ = false;
    rangeUser_ = false;
    playDir_ = 0;
    if (!sameGrid) fit_ = true;
    {
        std::lock_guard<std::mutex> lk(plotMutex_);
        lines_.clear();
        line1d_.reset();
    }
    if (vi.dims.size() == 1) {
        auto L = std::make_shared<PlotLine>();
        L->var = v; L->dim = 0; L->pos = {0};
        line1d_ = L;
        size_t len = shape[0];
        loader_.submit([this, L, len] {
            std::vector<float> buf;
            ds_.read(L->var, {0}, {len}, buf);
            std::lock_guard<std::mutex> lk(plotMutex_);
            L->series.x = ds_.dims()[ds_.vars()[L->var].dims[0]].coord;
            L->series.y.assign(buf.begin(), buf.end());
            L->series.color = kLineColors[0];
            L->ready = true;
        });
    } else {
        startScan(false);
    }
}

SliceKey App::currentKey() const {
    SliceKey k;
    k.var = var_;
    k.xd = xd_;
    k.yd = yd_;
    k.pos = pos_;
    if (xd_ >= 0) k.pos[xd_] = 0;
    if (yd_ >= 0) k.pos[yd_] = 0;
    return k;
}

SliceKey App::keyWithAnim(size_t idx) const {
    SliceKey k = currentKey();
    if (animd_ >= 0) k.pos[animd_] = idx;
    return k;
}

void App::setFrame(size_t idx) {
    if (animd_ < 0) return;
    pos_[animd_] = idx;
}

void App::stepFrame(int delta, bool wrap) {
    if (animd_ < 0) return;
    long len = (long)ds_.shape(var_)[animd_];
    long p = (long)pos_[animd_] + delta;
    if (wrap) p = ((p % len) + len) % len;
    else p = std::clamp(p, 0L, len - 1);
    setFrame((size_t)p);
}

// ------------------------------------------------------------------ range scanning

void App::startScan(bool full) {
    auto st = std::make_shared<ScanState>();
    st->full = full;
    st->base = currentKey();
    const auto shape = ds_.shape(var_);
    if (full) {
        st->total = 1;
        for (size_t i = 0; i < shape.size(); ++i)
            if ((int)i != xd_ && (int)i != yd_ && shape[i] > 1) {
                st->dims.push_back((int)i);
                st->lens.push_back(shape[i]);
                st->total *= shape[i];
            }
    } else if (animd_ >= 0) {
        // Like ncview's default: estimate from a handful of frames spread over the animation dim.
        st->dims.push_back(animd_);
        st->lens.push_back(shape[animd_]);
        st->total = std::min<size_t>(shape[animd_], 10);
    } else {
        st->total = 1;
    }
    {
        std::lock_guard<std::mutex> lk(scanMutex_);
        st->gen = ++scanGen_;
        scan_ = st;
    }
    loader_.submit([this, st] { scanStep(st); });
}

void App::scanStep(std::shared_ptr<ScanState> st) {
    SliceKey k = st->base;
    {
        std::lock_guard<std::mutex> lk(scanMutex_);
        if (st->gen != scanGen_ || st->finished) return;
        size_t i = st->next++;
        if (st->full) {
            for (int d = (int)st->dims.size() - 1; d >= 0; --d) {
                k.pos[st->dims[d]] = i % st->lens[d];
                i /= st->lens[d];
            }
        } else if (!st->dims.empty()) {
            size_t len = st->lens[0];
            k.pos[st->dims[0]] = st->total > 1 ? i * (len - 1) / (st->total - 1) : 0;
        }
    }
    SliceP s = loader_.get(k);
    if (!s) s = readSlice(ds_, k);
    bool more;
    {
        std::lock_guard<std::mutex> lk(scanMutex_);
        if (s->anyValid) {
            if (!st->any) { st->lo = s->vmin; st->hi = s->vmax; st->any = true; }
            else { st->lo = std::min(st->lo, s->vmin); st->hi = std::max(st->hi, s->vmax); }
        }
        st->done++;
        if (st->done >= st->total) st->finished = true;
        more = st->next < st->total && st->gen == scanGen_;
    }
    // Re-queue at the back so interactive slice requests (pushed to the front) stay responsive.
    if (more) loader_.submit([this, st] { scanStep(st); });
}

// ------------------------------------------------------------------ texture

void App::updateTexture() {
    if (!isImageVar()) return;
    // Apply finished range scans.
    {
        std::lock_guard<std::mutex> lk(scanMutex_);
        if (scan_ && scan_->finished && scan_->gen == scanGen_) {
            if (scan_->any && !rangeUser_) {
                vmin_ = scan_->lo; vmax_ = scan_->hi;
                rangeValid_ = true;
                renderDirty_ = true;
            }
            scan_.reset();
        }
    }
    SliceKey k = currentKey();
    SliceP s = loader_.get(k);
    loading_ = !s;
    if (!s) loader_.request(k, true);
    if (s && s != shown_) {
        shown_ = s;
        shownKey_ = k;
        renderDirty_ = true;
        if (!rangeValid_ && s->anyValid) { vmin_ = s->vmin; vmax_ = s->vmax; rangeValid_ = true; }
    }
    if (!renderDirty_ || !shown_ || !ren_) return;
    if (shown_->data.size() != shown_->nx * shown_->ny) {
        // Failed read (e.g. missing HDF5 filter plugin): no data to draw; the canvas shows the error.
        if (tex_) { SDL_DestroyTexture(tex_); tex_ = nullptr; texW_ = texH_ = 0; }
        renderDirty_ = false;
        return;
    }

    rp_.vmin = vmin_;
    rp_.vmax = vmax_;
    rp_.decim = int(std::max<size_t>(1, (std::max(shown_->nx, shown_->ny) + maxTex_ - 1) / maxTex_));
    int w = 0, h = 0;
    renderSlice(*shown_, rp_, pixels_, w, h);

    // Coastline overlay on lon/lat grids.
    const VarInfo& vi = ds_.vars()[shownKey_.var];
    const DimInfo& dx = ds_.dims()[vi.dims[shownKey_.xd]];
    const DimInfo& dy = ds_.dims()[vi.dims[shownKey_.yd]];
    if (overlay_ != Overlay::None && dx.kind == AxisKind::Lon && dy.kind == AxisKind::Lat) {
        std::string key = dx.name + "/" + dy.name + "/" + std::to_string((int)overlay_);
        if (key != ovKey_) { ovCells_ = overlayCells(overlay_, dx, dy); ovKey_ = key; }
        const int d = rp_.decim;
        for (uint32_t c : ovCells_) {
            size_t ix = c % shown_->nx, iy = c / shown_->nx;
            int col = int(ix / d), row = int(iy / d);
            if (rp_.flipY) row = h - 1 - row;
            if (col < w && row >= 0 && row < h) pixels_[size_t(row) * w + col] = packRGBA(0, 0, 0);
        }
    }

    if (!tex_ || w != texW_ || h != texH_) {
        if (tex_) SDL_DestroyTexture(tex_);
        tex_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, w, h);
        texW_ = w;
        texH_ = h;
        if (!tex_) SDL_Log("ncview++: cannot create %dx%d texture: %s", w, h, SDL_GetError());
    }
    if (tex_) SDL_UpdateTexture(tex_, nullptr, pixels_.data(), w * 4);
    renderDirty_ = false;
    version_++;
}

// ------------------------------------------------------------------ plots

void App::addPlotLine(size_t ix, size_t iy, bool append) {
    auto L = std::make_shared<PlotLine>();
    L->ix = ix;
    L->iy = iy;
    std::lock_guard<std::mutex> lk(plotMutex_);
    if (!append) lines_.clear();
    if (lines_.size() >= 6) lines_.erase(lines_.begin());
    lines_.push_back(L);
    showPlot_ = true;
}

// Ensures every plot line holds data for the current var / plot dim / position.
void App::reloadPlotLines() {
    if (!isImageVar() || plotDim_ < 0) return;
    const auto shape = ds_.shape(var_);
    std::lock_guard<std::mutex> lk(plotMutex_);
    for (size_t li = 0; li < lines_.size(); ++li) {
        auto L = lines_[li];
        std::vector<size_t> want = pos_;
        want[xd_] = L->ix;
        want[yd_] = L->iy;
        want[plotDim_] = 0;
        if (L->var == var_ && L->dim == plotDim_ && L->pos == want) continue;
        L->var = var_;
        L->dim = plotDim_;
        L->pos = want;
        L->ready = false;
        const DimInfo& dx = ds_.dims()[ds_.vars()[var_].dims[xd_]];
        const DimInfo& dy = ds_.dims()[ds_.vars()[var_].dims[yd_]];
        std::string label = dx.name + "=" + dx.format(L->ix) + ", " + dy.name + "=" + dy.format(L->iy);
        ImU32 color = kLineColors[li % 6];
        std::vector<size_t> count(shape.size(), 1);
        count[plotDim_] = shape[plotDim_];
        int var = var_, dim = plotDim_;
        loader_.submit([this, L, want, count, var, dim, label, color] {
            std::vector<float> buf;
            bool ok = ds_.read(var, want, count, buf);
            std::lock_guard<std::mutex> lk2(plotMutex_);
            if (L->pos != want || L->dim != dim || L->var != var) return;  // superseded
            L->series.x = ds_.dims()[ds_.vars()[var].dims[dim]].coord;
            L->series.y.assign(buf.begin(), buf.end());
            if (!ok) L->series.y.assign(L->series.x.size(), NAN);
            L->series.label = label;
            L->series.color = color;
            L->ready = true;
        });
    }
}

// ------------------------------------------------------------------ frame

void App::handleKeys() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) playDir_ = playDir_ ? 0 : 1;
    int step = io.KeyShift ? 10 : 1;
    if (pressed(ImGuiKey_RightArrow)) { playDir_ = 0; stepFrame(step, true); }
    if (pressed(ImGuiKey_LeftArrow)) { playDir_ = 0; stepFrame(-step, true); }
    if (pressed(ImGuiKey_Home)) { playDir_ = 0; setFrame(0); }
    if (pressed(ImGuiKey_End) && animd_ >= 0) { playDir_ = 0; setFrame(ds_.shape(var_)[animd_] - 1); }
    auto nextVar = [&](int dir) {
        int n = (int)ds_.vars().size();
        for (int k = 1; k < n; ++k) {
            int v = ((var_ + dir * k) % n + n) % n;
            if (showCoordVars_ || !ds_.vars()[v].isCoordVar) { selectVar(v); return; }
        }
    };
    if (pressed(ImGuiKey_PageDown) || pressed(ImGuiKey_DownArrow)) nextVar(1);
    if (pressed(ImGuiKey_PageUp) || pressed(ImGuiKey_UpArrow)) nextVar(-1);
    if (pressed(ImGuiKey_C)) {
        int n = (int)colormaps().size();
        rp_.cmap = ((rp_.cmap + (io.KeyShift ? -1 : 1)) % n + n) % n;
        renderDirty_ = true;
    }
    if (pressed(ImGuiKey_I)) { rp_.invert = !rp_.invert; renderDirty_ = true; }
    if (pressed(ImGuiKey_T)) { rp_.transform = Transform(((int)rp_.transform + 1) % 4); renderDirty_ = true; }
    if (pressed(ImGuiKey_R) && shown_ && shown_->anyValid) {
        vmin_ = shown_->vmin; vmax_ = shown_->vmax; rangeUser_ = true; renderDirty_ = true;
    }
    if (pressed(ImGuiKey_A)) { rangeUser_ = false; startScan(false); }
    if (pressed(ImGuiKey_F)) fit_ = true;
    if (pressed(ImGuiKey_O)) { overlay_ = overlay_ == Overlay::None ? Overlay::Coast : Overlay::None; renderDirty_ = true; }
    if (pressed(ImGuiKey_Equal) || pressed(ImGuiKey_KeypadAdd)) { if (fit_) { fit_ = false; zoom_ = 1; } zoom_ *= 1.25f; }
    if (pressed(ImGuiKey_Minus) || pressed(ImGuiKey_KeypadSubtract)) { if (fit_) { fit_ = false; zoom_ = 1; } zoom_ /= 1.25f; }
    if (pressed(ImGuiKey_P)) showPlot_ = !showPlot_;
    if (pressed(ImGuiKey_Q) || (io.KeyCtrl && pressed(ImGuiKey_W))) quit_ = true;
}

void App::frame() {
    handleKeys();

    // Playback: advance only once the next frame is in memory, so no frame is ever skipped.
    if (playDir_ != 0 && animd_ >= 0) {
        double t = nowSec();
        if (t - lastAdvance_ >= 1.0 / fps_) {
            size_t len = ds_.shape(var_)[animd_];
            size_t next = (pos_[animd_] + len + playDir_) % len;
            if (loader_.get(keyWithAnim(next))) { setFrame(next); lastAdvance_ = t; }
            else loader_.request(keyWithAnim(next), true);
        }
    }

    updateTexture();

    // Prefetch around the current frame (further ahead while playing).
    if (isImageVar() && animd_ >= 0) {
        size_t len = ds_.shape(var_)[animd_];
        int n = playDir_ ? 8 : 2;
        for (int k = 1; k <= n && k < (int)len; ++k) {
            if (playDir_ >= 0) loader_.request(keyWithAnim((pos_[animd_] + k) % len), false);
            if (playDir_ <= 0) loader_.request(keyWithAnim((pos_[animd_] + len - (k % len)) % len), false);
        }
    }
    reloadPlotLines();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);

    const float fh = ImGui::GetFontSize();
    if (sidebarW_ <= 0) sidebarW_ = fh * 22;
    sidebarW_ = std::clamp(sidebarW_, fh * 10, std::max(fh * 10, vp->WorkSize.x - fh * 20));

    ImGui::BeginChild("##sidebar", ImVec2(sidebarW_, 0), ImGuiChildFlags_Borders);
    drawSidebar(sidebarW_);
    ImGui::EndChild();

    // Splitter.
    ImGui::SameLine(0, 0);
    ImGui::InvisibleButton("##split", ImVec2(fh * 0.4f, ImGui::GetContentRegionAvail().y));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActive()) sidebarW_ += ImGui::GetIO().MouseDelta.x;
    ImGui::SameLine(0, 0);

    ImGui::BeginChild("##right", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    drawToolbar();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float bottom = fh * 3.4f + ImGui::GetStyle().ItemSpacing.y * 3;
    if (var_ >= 0 && ds_.vars()[var_].dims.size() == 1) {
        std::lock_guard<std::mutex> lk(plotMutex_);
        if (line1d_ && line1d_->ready) {
            const VarInfo& vi = ds_.vars()[var_];
            const DimInfo& d = ds_.dims()[vi.dims[0]];
            PlotOptions po;
            po.xlabel = d.name + (d.units.empty() || d.time.valid() ? "" : " (" + d.units + ")");
            if (d.time.valid()) po.xfmt = [&d](double x) { return d.time.format(x).substr(0, 10); };
            line1d_->series.label = vi.name + (vi.units.empty() ? "" : " (" + vi.units + ")");
            drawPlot("##1d", ImVec2(avail.x, avail.y - fh * 1.6f), {line1d_->series}, po, nullptr);
        } else {
            ImGui::TextDisabled("loading...");
        }
    } else {
        drawCanvas(ImVec2(avail.x, std::max(fh * 4, avail.y - bottom)));
        drawColorbar(avail.x);
    }
    drawStatus();
    ImGui::EndChild();
    ImGui::End();

    if (showPlot_) drawPlotWindow();
    if (showInfo_) drawInfoWindow();
}

// ------------------------------------------------------------------ sidebar

void App::drawSidebar(float) {
    const float fh = ImGui::GetFontSize();
    // Files
    {
        const auto& p = ds_.paths();
        std::string f = p.empty() ? "" : p[0];
        size_t s = f.find_last_of('/');
        ImGui::TextUnformatted(s == std::string::npos ? f.c_str() : f.c_str() + s + 1);
        if (p.size() > 1) {
            ImGui::SameLine();
            ImGui::TextDisabled("+%zu files%s", p.size() - 1, ds_.merged() ? " (merged)" : "");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            for (auto& x : p) ImGui::TextUnformatted(x.c_str());
            ImGui::EndTooltip();
        }
        if (!ds_.notes().empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%zu notes)", ds_.notes().size());
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                for (auto& n : ds_.notes()) ImGui::TextUnformatted(n.c_str());
                ImGui::EndTooltip();
            }
        }
    }
    ImGui::SetNextItemWidth(-fh * 4.5f);
    ImGui::InputTextWithHint("##filter", "filter variables", filter_, sizeof filter_);
    ImGui::SameLine();
    ImGui::Checkbox("coords", &showCoordVars_);

    // Variables grouped by dimensions, like ncview.
    float dimsH = fh * 1.9f * (var_ >= 0 ? (float)ds_.vars()[var_].dims.size() + 2.5f : 1.f);
    ImGui::BeginChild("##vars", ImVec2(0, std::max(fh * 6, ImGui::GetContentRegionAvail().y - dimsH)), ImGuiChildFlags_Borders);
    // Per netCDF group: visible variables bucketed by dimension signature.
    using SigList = std::vector<std::pair<std::string, std::vector<int>>>;
    const auto& grps = ds_.groups();
    std::vector<SigList> byGroup(grps.size());
    std::vector<int> visible(grps.size(), 0);   // visible variables in the group's subtree
    std::string flt = filter_;
    for (auto& c : flt) c = (char)std::tolower((unsigned char)c);
    for (size_t i = 0; i < ds_.vars().size(); ++i) {
        const VarInfo& v = ds_.vars()[i];
        if (v.isCoordVar && !showCoordVars_) continue;
        if (!flt.empty()) {
            std::string hay = v.fullName + " " + v.longName;
            for (auto& c : hay) c = (char)std::tolower((unsigned char)c);
            if (hay.find(flt) == std::string::npos) continue;
        }
        std::string sig = "(";
        for (size_t k = 0; k < v.dims.size(); ++k) sig += (k ? ", " : "") + ds_.dims()[v.dims[k]].name;
        sig += ")";
        SigList& groups = byGroup[v.group];
        auto it = std::find_if(groups.begin(), groups.end(), [&](auto& g) { return g.first == sig; });
        if (it == groups.end()) groups.push_back({sig, {(int)i}});
        else it->second.push_back((int)i);
        for (int g = v.group; g >= 0; g = grps[g].parent) ++visible[g];
    }
    auto drawSigs = [&](const SigList& groups) {
        for (auto& g : groups) {
            ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            if (ImGui::TreeNode(g.first.c_str())) {
                for (int i : g.second) {
                    const VarInfo& v = ds_.vars()[i];
                    ImGui::PushID(i);
                    if (ImGui::Selectable(v.name.c_str(), i == var_)) selectVar(i);
                    bool hov = ImGui::IsItemHovered();
                    if (!v.longName.empty()) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", v.longName.c_str());
                        hov = hov || ImGui::IsItemHovered();
                    }
                    if (hov) ImGui::SetTooltip("%s\n%s [%s]", v.fullName.c_str(), v.longName.c_str(), v.units.c_str());
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
        }
    };
    // Root variables at the top level, subgroups as nested tree nodes (hidden when empty).
    std::function<void(int)> drawGroup = [&](int gi) {
        drawSigs(byGroup[gi]);
        for (int c : grps[gi].children) {
            if (!visible[c]) continue;
            ImGui::PushID(c);
            ImGui::SetNextItemOpen(true, flt.empty() ? ImGuiCond_Once : ImGuiCond_Always);   // filtering reveals matches
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_PlotLinesHovered));
            bool open = ImGui::TreeNodeEx("##grp", ImGuiTreeNodeFlags_SpanAvailWidth, "/%s", grps[c].name.c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("group /%s  (%d variables)", grps[c].path.c_str(), visible[c]);
            if (open) {
                drawGroup(c);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    };
    if (!grps.empty()) drawGroup(0);
    ImGui::EndChild();

    // Dimensions of the current variable.
    if (var_ < 0) return;
    const VarInfo& vi = ds_.vars()[var_];
    const auto shape = ds_.shape(var_);
    ImGui::SeparatorText("Dimensions");
    if (vi.dims.size() >= 2) {
        auto axisCombo = [&](const char* label, int& which, int other) {
            ImGui::SetNextItemWidth(fh * 7);
            if (ImGui::BeginCombo(label, ds_.dims()[vi.dims[which]].name.c_str())) {
                for (size_t i = 0; i < vi.dims.size(); ++i) {
                    if ((int)i == other) continue;
                    std::string l = ds_.dims()[vi.dims[i]].name + " (" + std::to_string(shape[i]) + ")";
                    if (ImGui::Selectable(l.c_str(), (int)i == which)) {
                        which = (int)i;
                        if (animd_ == which) animd_ = -1;
                        for (size_t k = 0; animd_ < 0 && k < shape.size(); ++k)
                            if ((int)k != xd_ && (int)k != yd_ && shape[k] > 1) animd_ = (int)k;
                        if (plotDim_ == xd_ || plotDim_ == yd_) plotDim_ = animd_ >= 0 ? animd_ : xd_;
                        loader_.dropPrefetch();
                        shown_.reset();
                        fit_ = true;
                        autoFlip();
                        rangeValid_ = rangeUser_;
                        startScan(false);
                        {
                            std::lock_guard<std::mutex> lk(plotMutex_);
                            lines_.clear();
                        }
                    }
                }
                ImGui::EndCombo();
            }
        };
        axisCombo("X", xd_, yd_);
        ImGui::SameLine();
        axisCombo("Y", yd_, xd_);
        ImGui::SameLine();
        if (ImGui::Button("swap")) { std::swap(xd_, yd_); autoFlip(); shown_.reset(); fit_ = true; std::lock_guard<std::mutex> lk(plotMutex_); lines_.clear(); }
    }
    if (ImGui::BeginTable("##dims", 3, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("anim", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
        for (size_t i = 0; i < vi.dims.size(); ++i) {
            const DimInfo& d = ds_.dims()[vi.dims[i]];
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool isAxis = (int)i == xd_ || (int)i == yd_;
            if (!isAxis && shape[i] > 1) {
                if (ImGui::RadioButton("##anim", animd_ == (int)i)) { animd_ = (int)i; playDir_ = 0; loader_.dropPrefetch(); }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Animate along %s", d.name.c_str());
            }
            ImGui::TableNextColumn();
            ImGui::Text("%s", d.name.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\nlength %zu%s\n%s", d.longName.c_str(), d.len, d.isRecord ? (ds_.merged() ? " (common time steps of all files)" : " (record, multi-file)") : "", d.units.c_str());
            ImGui::TableNextColumn();
            if (isAxis) {
                ImGui::TextDisabled("%s axis: %s .. %s", (int)i == xd_ ? "X" : "Y", d.format(0).c_str(), d.format(d.len ? d.len - 1 : 0).c_str());
            } else if (shape[i] <= 1) {
                ImGui::TextUnformatted(d.format(0).c_str());
            } else {
                int idx = (int)pos_[i];
                ImGui::SetNextItemWidth(-FLT_MIN);
                std::string lbl = d.format(pos_[i]) + "  [" + std::to_string(idx + 1) + "/" + std::to_string(shape[i]) + "]";
                if (ImGui::SliderInt("##idx", &idx, 0, (int)shape[i] - 1, lbl.c_str())) pos_[i] = (size_t)idx;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// ------------------------------------------------------------------ toolbar

void App::drawToolbar() {
    const float fh = ImGui::GetFontSize();
    // Row 1: playback.
    bool canAnim = animd_ >= 0;
    ImGui::BeginDisabled(!canAnim);
    if (ImGui::Button("|<")) { playDir_ = 0; setFrame(0); }
    ImGui::SameLine();
    if (ImGui::Button(playDir_ < 0 ? "[<<]" : "<<")) playDir_ = playDir_ < 0 ? 0 : -1;
    ImGui::SameLine();
    if (ImGui::Button("<")) { playDir_ = 0; stepFrame(-1, true); }
    ImGui::SameLine();
    if (ImGui::Button("||")) playDir_ = 0;
    ImGui::SameLine();
    if (ImGui::Button(">")) { playDir_ = 0; stepFrame(1, true); }
    ImGui::SameLine();
    if (ImGui::Button(playDir_ > 0 ? "[>>]" : ">>")) playDir_ = playDir_ > 0 ? 0 : 1;
    ImGui::SameLine();
    if (ImGui::Button(">|") && canAnim) { playDir_ = 0; setFrame(ds_.shape(var_)[animd_] - 1); }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fh * 7);
    ImGui::SliderFloat("##fps", &fps_, 1.f, 60.f, "%.0f fps", ImGuiSliderFlags_Logarithmic);
    ImGui::EndDisabled();
    if (canAnim) {
        const DimInfo& d = ds_.dims()[ds_.vars()[var_].dims[animd_]];
        ImGui::SameLine();
        ImGui::Text("%s = %s  (%zu/%zu)", d.name.c_str(), d.format(pos_[animd_]).c_str(), pos_[animd_] + 1, d.len);
    }
    if (loading_ || loader_.busy()) {
        ImGui::SameLine();
        ImGui::TextDisabled(loading_ ? "loading..." : "reading...");
    }

    if (!isImageVar()) return;
    // Row 2: colours and range.
    ImGui::SetNextItemWidth(fh * 7);
    if (ImGui::BeginCombo("##cmap", colormaps()[rp_.cmap].name.c_str(), ImGuiComboFlags_HeightLarge)) {
        for (size_t i = 0; i < colormaps().size(); ++i) {
            ImGui::PushID((int)i);
            ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##c", (int)i == rp_.cmap, 0, ImVec2(fh * 12, fh))) { rp_.cmap = (int)i; renderDirty_ = true; }
            auto tbl = colormapTable((int)i, rp_.invert);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            float w = fh * 6;
            for (int k = 0; k < 64; ++k) {
                uint32_t c = tbl[k * 4];
                dl->AddRectFilled(ImVec2(p.x + w * k / 64, p.y + 1), ImVec2(p.x + w * (k + 1) / 64 + 1, p.y + fh - 1),
                                  IM_COL32(c & 255, (c >> 8) & 255, (c >> 16) & 255, 255));
            }
            dl->AddText(ImVec2(p.x + w + fh * 0.5f, p.y), ImGui::GetColorU32(ImGuiCol_Text), colormaps()[i].name.c_str());
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("invert", &rp_.invert)) renderDirty_ = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fh * 5);
    int tr = (int)rp_.transform;
    if (ImGui::Combo("##trans", &tr, "linear\0low\0high\0log\0")) { rp_.transform = (Transform)tr; renderDirty_ = true; }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fh * 6);
    if (ImGui::InputFloat("##min", &vmin_, 0, 0, "%.5g")) { rangeUser_ = rangeValid_ = true; renderDirty_ = true; }
    ImGui::SameLine(0, 2);
    ImGui::TextUnformatted("..");
    ImGui::SameLine(0, 2);
    ImGui::SetNextItemWidth(fh * 6);
    if (ImGui::InputFloat("##max", &vmax_, 0, 0, "%.5g")) { rangeUser_ = rangeValid_ = true; renderDirty_ = true; }
    ImGui::SameLine();
    if (ImGui::Button("auto")) { rangeUser_ = false; startScan(false); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Range from a sample of frames (a)");
    ImGui::SameLine();
    if (ImGui::Button("frame") && shown_ && shown_->anyValid) {
        vmin_ = shown_->vmin; vmax_ = shown_->vmax; rangeUser_ = true; renderDirty_ = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Range of the current frame (r)");
    ImGui::SameLine();
    if (ImGui::Button("all")) { rangeUser_ = false; startScan(true); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Exact range over all frames (background scan)");
    {
        std::lock_guard<std::mutex> lk(scanMutex_);
        if (scan_ && scan_->full && !scan_->finished) {
            ImGui::SameLine();
            ImGui::TextDisabled("scanning %zu/%zu", scan_->done, scan_->total);
        }
    }

    // Row 3: view.
    const VarInfo& vi = ds_.vars()[var_];
    bool lonlat = ds_.dims()[vi.dims[xd_]].kind == AxisKind::Lon && ds_.dims()[vi.dims[yd_]].kind == AxisKind::Lat;
    ImGui::BeginDisabled(!lonlat);
    ImGui::SetNextItemWidth(fh * 8);
    int ov = (int)overlay_;
    if (ImGui::Combo("##ov", &ov, "no overlay\0coasts\0coasts (hi-res)\0")) { overlay_ = (Overlay)ov; renderDirty_ = true; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Checkbox("flip Y", &rp_.flipY)) renderDirty_ = true;
    ImGui::SameLine();
    ImGui::Checkbox("smooth", &smooth_);
    ImGui::SameLine();
    if (ImGui::Button("fit")) fit_ = true;
    ImGui::SameLine();
    ImGui::BeginDisabled(fit_);
    if (ImGui::Button("1:1")) zoom_ = -1;   // resolved in drawCanvas
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox("plot", &showPlot_);
    ImGui::SameLine();
    ImGui::Checkbox("info", &showInfo_);
}

// ------------------------------------------------------------------ canvas

void App::drawCanvas(ImVec2 size) {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 c0 = ImGui::GetCursorScreenPos();
    const ImVec2 full = size;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    hoverValid_ = false;
    if (!shown_ || !tex_) {
        ImGui::InvisibleButton("##canvas", size);
        dl->AddRectFilled(c0, ImVec2(c0.x + size.x, c0.y + size.y), IM_COL32(40, 40, 44, 255));
        const char* msg = (shown_ && !shown_->error.empty()) ? shown_->error.c_str() : "loading...";
        dl->AddText(ImVec2(c0.x + 10, c0.y + 10), ImGui::GetColorU32(ImGuiCol_Text), msg);
        return;
    }
    const float nx = (float)shown_->nx, ny = (float)shown_->ny;
    const float fitS = std::min(full.x / nx, full.y / ny);
    if (zoom_ < 0) zoom_ = 1.0f / fitS;  // 1:1 (one data cell per pixel)
    if (fit_) { zoom_ = 1; cx_ = nx * 0.5f; cy_ = ny * 0.5f; }

    // Reserve room for scrollbars when the zoomed image is larger than the view.
    const float sb = ImGui::GetStyle().ScrollbarSize;
    bool hbar = false, vbar = false;
    for (int it = 0; it < 2; ++it) {
        hbar = nx * fitS * zoom_ > full.x - (vbar ? sb : 0) + 0.5f;
        vbar = ny * fitS * zoom_ > full.y - (hbar ? sb : 0) + 0.5f;
    }
    size = ImVec2(full.x - (vbar ? sb : 0), full.y - (hbar ? sb : 0));

    ImGui::InvisibleButton("##canvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    bool hov = ImGui::IsItemHovered();
    dl->AddRectFilled(c0, ImVec2(c0.x + size.x, c0.y + size.y), IM_COL32(40, 40, 44, 255));
    float s = fitS * zoom_;
    const ImVec2 mid(c0.x + size.x * 0.5f, c0.y + size.y * 0.5f);
    auto toScreen = [&](float u, float v) { return ImVec2(mid.x + (u - cx_) * s, mid.y + (v - cy_) * s); };

    // Zoom around the cursor.
    if (hov && io.MouseWheel != 0) {
        float u = cx_ + (io.MousePos.x - mid.x) / s, v = cy_ + (io.MousePos.y - mid.y) / s;
        zoom_ = std::clamp(zoom_ * std::pow(1.2f, io.MouseWheel), 0.05f, 4000.f);
        fit_ = false;
        s = fitS * zoom_;
        cx_ = u - (io.MousePos.x - mid.x) / s;
        cy_ = v - (io.MousePos.y - mid.y) / s;
    }
    // Drag to pan, click to plot.
    if (ImGui::IsItemActivated()) dragged_ = false;
    if (ImGui::IsItemActive()) {
        for (int b = 0; b < 3; ++b)
            if (ImGui::IsMouseDragging(b, 3.0f)) dragged_ = true;
        if (dragged_ && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
            fit_ = false;
            cx_ -= io.MouseDelta.x / s;
            cy_ -= io.MouseDelta.y / s;
        }
    }
    if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) fit_ = true;

    // Scrollbars: thumb shows the visible part of the image; drag it or click the track to move.
    auto scrollbar = [&](const char* id, bool horiz, float& center, float n) {
        const ImVec2 p0 = horiz ? ImVec2(c0.x, c0.y + size.y) : ImVec2(c0.x + size.x, c0.y);
        const float len = horiz ? size.x : size.y;
        const float view = len / s;                       // visible extent in data cells
        const float total = std::max(n, view);
        const float lo = std::clamp(center - view * 0.5f, 0.f, total - view);
        const float tLen = std::max(sb, len * view / total);
        const float tPos = (len - tLen) * (total > view ? lo / (total - view) : 0.f);
        ImGui::SetCursorScreenPos(p0);
        ImGui::InvisibleButton(id, horiz ? ImVec2(len, sb) : ImVec2(sb, len));
        const bool active = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();
        if (active) {
            fit_ = false;
            const float m = horiz ? io.MousePos.x - p0.x : io.MousePos.y - p0.y;
            const float mPrev = m - (horiz ? io.MouseDelta.x : io.MouseDelta.y);
            if (ImGui::IsItemActivated() && (mPrev < tPos || mPrev > tPos + tLen))
                center = (m / len) * total;  // jump to click on the track
            else if (len > tLen)
                center = lo + view * 0.5f + (m - mPrev) * (total - view) / (len - tLen);
        }
        const ImVec2 q0 = horiz ? ImVec2(p0.x + tPos, p0.y + 2) : ImVec2(p0.x + 2, p0.y + tPos);
        const ImVec2 q1 = horiz ? ImVec2(q0.x + tLen, p0.y + sb - 2) : ImVec2(p0.x + sb - 2, q0.y + tLen);
        dl->AddRectFilled(p0, horiz ? ImVec2(p0.x + len, p0.y + sb) : ImVec2(p0.x + sb, p0.y + len), ImGui::GetColorU32(ImGuiCol_ScrollbarBg));
        dl->AddRectFilled(q0, q1, ImGui::GetColorU32(active ? ImGuiCol_ScrollbarGrabActive : hovered ? ImGuiCol_ScrollbarGrabHovered : ImGuiCol_ScrollbarGrab),
                          ImGui::GetStyle().ScrollbarRounding);
    };
    if (hbar) scrollbar("##hscroll", true, cx_, nx);
    if (vbar) scrollbar("##vscroll", false, cy_, ny);
    ImGui::SetCursorScreenPos(c0);
    ImGui::Dummy(full);

    // Image (nearest-neighbour unless smoothing is on). Only the visible part is submitted:
    // a quad with corners far off-screen at high zoom is dropped by some renderers.
    const float d = (float)rp_.decim;
    const float iw = texW_ * d, ih = texH_ * d;   // image extent in data cells
    const float u0 = std::max(0.f, cx_ - size.x * 0.5f / s), u1 = std::min(iw, cx_ + size.x * 0.5f / s);
    const float v0 = std::max(0.f, cy_ - size.y * 0.5f / s), v1 = std::min(ih, cy_ + size.y * 0.5f / s);
    dl->PushClipRect(c0, ImVec2(c0.x + size.x, c0.y + size.y), true);
    const ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    if (u1 > u0 && v1 > v0) {
        if (!smooth_ && pio.DrawCallback_SetSamplerNearest) dl->AddCallback(pio.DrawCallback_SetSamplerNearest, nullptr);
        // Split into tiles: SDL's software renderer drops one triangle of large textured quads
        // (seen with SDL 3.4 for quads ~1500 px wide); tiles of <= 256 px render correctly.
        const ImVec2 pa = toScreen(u0, v0), pb = toScreen(u1, v1);
        const int ntx = std::max(1, (int)std::ceil((pb.x - pa.x) / 256.f));
        const int nty = std::max(1, (int)std::ceil((pb.y - pa.y) / 256.f));
        for (int j = 0; j < nty; ++j)
            for (int i = 0; i < ntx; ++i) {
                const float ua = u0 + (u1 - u0) * i / ntx, ub = u0 + (u1 - u0) * (i + 1) / ntx;
                const float va = v0 + (v1 - v0) * j / nty, vb = v0 + (v1 - v0) * (j + 1) / nty;
                dl->AddImage((ImTextureID)(intptr_t)tex_, toScreen(ua, va), toScreen(ub, vb),
                             ImVec2(ua / iw, va / ih), ImVec2(ub / iw, vb / ih));
            }
        if (!smooth_ && pio.DrawCallback_SetSamplerLinear) dl->AddCallback(pio.DrawCallback_SetSamplerLinear, nullptr);
    }

    // Hover → data cell.
    if (hov) {
        float u = cx_ + (io.MousePos.x - mid.x) / s, v = cy_ + (io.MousePos.y - mid.y) / s;
        if (u >= 0 && v >= 0 && u < nx && v < ny) {
            hx_ = (size_t)u;
            size_t row = (size_t)v;
            hy_ = rp_.flipY ? shown_->ny - 1 - row : row;
            hoverValid_ = true;
        }
    }
    if (hov && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !dragged_ && hoverValid_)
        addPlotLine(hx_, hy_, io.KeyCtrl || io.KeyShift);

    // Markers of plotted points.
    {
        std::lock_guard<std::mutex> lk(plotMutex_);
        for (size_t i = 0; i < lines_.size(); ++i) {
            float u = lines_[i]->ix + 0.5f;
            float v = (rp_.flipY ? (ny - 1 - lines_[i]->iy) : lines_[i]->iy) + 0.5f;
            ImVec2 p = toScreen(u, v);
            dl->AddCircle(p, 5, IM_COL32(0, 0, 0, 255), 0, 3);
            dl->AddCircle(p, 5, kLineColors[i % 6], 0, 1.5f);
        }
    }
    dl->PopClipRect();
}

void App::drawColorbar(float width) {
    const float fh = ImGui::GetFontSize();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float x0 = p.x + fh * 2, x1 = p.x + width - fh * 2, h = fh * 1.0f;
    auto tbl = colormapTable(rp_.cmap, rp_.invert);
    const bool trueLog = rp_.transform == Transform::Log && vmin_ > 0 && vmax_ > vmin_;
    const int N = 256;
    for (int i = 0; i < N; ++i) {
        float t = (i + 0.5f) / N;
        float tt = trueLog ? t : applyTransform(rp_.transform, t);
        uint32_t c = tbl[std::clamp(int(tt * 255 + 0.5f), 0, 255)];
        dl->AddRectFilled(ImVec2(x0 + (x1 - x0) * i / N, p.y), ImVec2(x0 + (x1 - x0) * (i + 1) / N + 1, p.y + h),
                          IM_COL32(c & 255, (c >> 8) & 255, (c >> 16) & 255, 255));
    }
    dl->AddRect(ImVec2(x0, p.y), ImVec2(x1, p.y + h), ImGui::GetColorU32(ImGuiCol_Border));
    // Ticks.
    const ImU32 tc = ImGui::GetColorU32(ImGuiCol_Text);
    auto valToX = [&](double v) {
        double t = trueLog ? (std::log(v) - std::log(vmin_)) / (std::log(vmax_) - std::log(vmin_))
                           : (v - vmin_) / (double(vmax_) - vmin_);
        return float(x0 + t * (x1 - x0));
    };
    if (vmax_ > vmin_) {
        std::vector<double> ticks;
        if (trueLog) {
            for (double e = std::floor(std::log10(vmin_)); e <= std::ceil(std::log10(vmax_)); e += 1) {
                double v = std::pow(10.0, e);
                if (v >= vmin_ && v <= vmax_) ticks.push_back(v);
            }
            if (ticks.size() < 2) ticks = {vmin_, vmax_};
        } else {
            ticks = niceTicks(vmin_, vmax_, std::max(2, int((x1 - x0) / (fh * 6))));
        }
        for (double t : ticks) {
            float x = valToX(t);
            dl->AddLine(ImVec2(x, p.y + h), ImVec2(x, p.y + h + fh * 0.3f), tc);
            std::string s = fmtG(t);
            ImVec2 ts = ImGui::CalcTextSize(s.c_str());
            dl->AddText(ImVec2(x - ts.x * 0.5f, p.y + h + fh * 0.3f), tc, s.c_str());
        }
    }
    ImGui::Dummy(ImVec2(width, h + fh * 1.5f));
}

void App::drawStatus() {
    if (var_ < 0) return;
    const VarInfo& vi = ds_.vars()[var_];
    std::string s;
    if (hoverValid_ && shown_ && isImageVar()) {
        const DimInfo& dx = ds_.dims()[vi.dims[shownKey_.xd]];
        const DimInfo& dy = ds_.dims()[vi.dims[shownKey_.yd]];
        float v = shown_->data[hy_ * shown_->nx + hx_];
        s = dx.name + " = " + dx.format(hx_) + "   " + dy.name + " = " + dy.format(hy_) + "   [" +
            std::to_string(hx_) + ", " + std::to_string(hy_) + "]   " + vi.fullName + " = " +
            (std::isnan(v) ? std::string("missing") : fmtG(v)) + (vi.units.empty() ? "" : " " + vi.units);
    } else {
        s = vi.fullName + (vi.longName.empty() ? "" : ": " + vi.longName) + (vi.units.empty() ? "" : " [" + vi.units + "]");
        if (shown_ && shown_->anyValid)
            s += "    frame min " + fmtG(shown_->vmin) + "  max " + fmtG(shown_->vmax);
    }
    ImGui::TextUnformatted(s.c_str());
}

// ------------------------------------------------------------------ windows

void App::drawPlotWindow() {
    const float fh = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(fh * 40, fh * 22), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(fh * 26, fh * 8), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Plot", &showPlot_)) { ImGui::End(); return; }
    if (!isImageVar()) { ImGui::TextDisabled("Select a 2-D (or higher) variable."); ImGui::End(); return; }
    const VarInfo& vi = ds_.vars()[var_];
    const auto shape = ds_.shape(var_);
    ImGui::SetNextItemWidth(fh * 8);
    if (ImGui::BeginCombo("along", ds_.dims()[vi.dims[plotDim_]].name.c_str())) {
        for (size_t i = 0; i < vi.dims.size(); ++i)
            if (shape[i] > 1 && ImGui::Selectable(ds_.dims()[vi.dims[i]].name.c_str(), (int)i == plotDim_)) plotDim_ = (int)i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::Checkbox("log y", &plotLogY_);
    ImGui::SameLine();
    if (ImGui::Button("clear")) { std::lock_guard<std::mutex> lk(plotMutex_); lines_.clear(); }
    ImGui::SameLine();
    ImGui::TextDisabled("click map: new line, ctrl/shift-click: add line");

    std::vector<PlotSeries> series;
    bool waiting = false;
    {
        std::lock_guard<std::mutex> lk(plotMutex_);
        for (auto& L : lines_) {
            if (L->ready) series.push_back(L->series);
            else waiting = true;
        }
    }
    const DimInfo& d = ds_.dims()[vi.dims[plotDim_]];
    PlotOptions po;
    po.xlabel = d.name + (d.units.empty() || d.time.valid() ? "" : " (" + d.units + ")");
    if (d.time.valid()) po.xfmt = [&d](double x) { return d.time.format(x).substr(0, 10); };
    po.marker = d.coord.empty() ? NAN : d.coord[pos_[plotDim_]];
    if (plotDim_ == xd_ || plotDim_ == yd_) po.marker = NAN;
    po.logY = plotLogY_;
    ImGui::TextDisabled("%s%s%s", vi.name.c_str(), vi.units.empty() ? "" : "  ", vi.units.c_str());
    if (waiting) { ImGui::SameLine(); ImGui::TextDisabled(" loading..."); }
    double cx = 0;
    if (series.empty() && !waiting) ImGui::TextDisabled("Click on the map to plot a series at that point.");
    else if (drawPlot("##plot", ImGui::GetContentRegionAvail(), series, po, &cx) && plotDim_ == animd_) {
        // Jump to the clicked frame.
        size_t best = 0;
        for (size_t i = 1; i < d.coord.size(); ++i)
            if (std::fabs(d.coord[i] - cx) < std::fabs(d.coord[best] - cx)) best = i;
        playDir_ = 0;
        setFrame(best);
    }
    ImGui::End();
}

void App::drawInfoWindow() {
    const float fh = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(fh * 34, fh * 26), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Info", &showInfo_)) { ImGui::End(); return; }
    static int cachedVar = -2;
    static std::vector<std::pair<std::string, std::string>> attrs, gattrs;
    static std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> grpAttrs;
    if (cachedVar != var_) {
        attrs = var_ >= 0 ? ds_.attributes(var_) : decltype(attrs){};
        gattrs = ds_.attributes(-1);
        // Attributes of the enclosing groups, innermost first.
        grpAttrs.clear();
        for (int g = var_ >= 0 ? ds_.vars()[var_].group : 0; g > 0; g = ds_.groups()[g].parent)
            grpAttrs.push_back({ds_.groups()[g].path, ds_.groupAttributes(g)});
        cachedVar = var_;
    }
    auto table = [](const char* id, const std::vector<std::pair<std::string, std::string>>& a) {
        if (ImGui::BeginTable(id, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) {
            for (auto& kv : a) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(kv.first.c_str());
                ImGui::TableNextColumn(); ImGui::TextWrapped("%s", kv.second.c_str());
            }
            ImGui::EndTable();
        }
    };
    if (var_ >= 0) {
        const VarInfo& vi = ds_.vars()[var_];
        std::string sig = vi.fullName + "(";
        for (size_t k = 0; k < vi.dims.size(); ++k) {
            const DimInfo& d = ds_.dims()[vi.dims[k]];
            const std::string& gp = ds_.groups()[d.group].path;
            sig += (k ? ", " : "") + (d.group != vi.group && !gp.empty() ? "/" + gp + "/" : std::string()) + d.name + "=" + std::to_string(d.len);
        }
        sig += ")";
        ImGui::SeparatorText("Variable");
        ImGui::TextWrapped("%s", sig.c_str());
        table("##va", attrs);
    }
    for (size_t i = 0; i < grpAttrs.size(); ++i) {
        if (grpAttrs[i].second.empty()) continue;
        ImGui::SeparatorText(("Group /" + grpAttrs[i].first).c_str());
        ImGui::PushID((int)i);
        table("##gr", grpAttrs[i].second);
        ImGui::PopID();
    }
    ImGui::SeparatorText("Global attributes");
    table("##ga", gattrs);
    ImGui::End();
}

} // namespace ncvpp
