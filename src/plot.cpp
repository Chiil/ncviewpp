#include "plot.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ncvpp {

static std::string fmtTick(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

bool drawPlot(const char* id, ImVec2 size, const std::vector<PlotSeries>& series,
              const PlotOptions& opt, double* clickedX) {
    ImGui::PushID(id);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (size.x < 50) size.x = 50;
    if (size.y < 50) size.y = 50;
    ImGui::InvisibleButton("##plot", size);
    bool hovered = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const float fh = ImGui::GetFontSize();
    const ImU32 axisCol = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 gridCol = ImGui::GetColorU32(ImGuiCol_Border);

    // Data bounds.
    double xmin = INFINITY, xmax = -INFINITY, ymin = INFINITY, ymax = -INFINITY;
    for (const auto& s : series)
        for (size_t i = 0; i < s.x.size(); ++i) {
            xmin = std::min(xmin, s.x[i]); xmax = std::max(xmax, s.x[i]);
            double y = s.y[i];
            if (std::isfinite(y) && (!opt.logY || y > 0)) { ymin = std::min(ymin, y); ymax = std::max(ymax, y); }
        }
    bool empty = !(xmax >= xmin) || !(ymax >= ymin);
    if (!empty) {
        if (xmax == xmin) { xmin -= 0.5; xmax += 0.5; }
        if (ymax == ymin) { double d = ymin == 0 ? 1 : std::fabs(ymin) * 0.05; ymin -= d; ymax += d; }
        double pad = (ymax - ymin) * 0.05;
        if (!opt.logY) { ymin -= pad; ymax += pad; }
    }
    auto ty = [&](double y) { return opt.logY ? std::log10(std::max(y, 1e-300)) : y; };
    double tymin = ty(ymin), tymax = ty(ymax);

    // Plot area with room for labels.
    float left = fh * 5.0f, bottom = fh * 2.6f, top = fh * 0.6f, right = fh * 1.0f;
    ImVec2 a(p0.x + left, p0.y + top), b(p0.x + size.x - right, p0.y + size.y - bottom);
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), ImGui::GetColorU32(ImGuiCol_FrameBg));
    if (empty) {
        dl->AddText(ImVec2(a.x + 10, a.y + 10), axisCol, "no valid data");
        ImGui::PopID();
        return false;
    }
    auto X = [&](double x) { return float(a.x + (x - xmin) / (xmax - xmin) * (b.x - a.x)); };
    auto Y = [&](double y) { return float(b.y - (ty(y) - tymin) / (tymax - tymin) * (b.y - a.y)); };

    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    // Grid + tick labels.
    int nxt = std::max(2, int((b.x - a.x) / (fh * 9)));
    for (double t : niceTicks(xmin, xmax, nxt)) {
        float x = X(t);
        dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), gridCol);
        std::string s = opt.xfmt ? opt.xfmt(t) : fmtTick(t);
        ImVec2 ts = ImGui::CalcTextSize(s.c_str());
        dl->AddText(ImVec2(x - ts.x * 0.5f, b.y + 3), axisCol, s.c_str());
    }
    int nyt = std::max(2, int((b.y - a.y) / (fh * 2.5f)));
    if (opt.logY) {
        for (double e = std::floor(tymin); e <= std::ceil(tymax); e += 1) {
            if (e < tymin || e > tymax) continue;
            float y = float(b.y - (e - tymin) / (tymax - tymin) * (b.y - a.y));
            dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), gridCol);
            std::string s = "1e" + fmtTick(e);
            ImVec2 ts = ImGui::CalcTextSize(s.c_str());
            dl->AddText(ImVec2(a.x - ts.x - 4, y - ts.y * 0.5f), axisCol, s.c_str());
        }
    } else {
        for (double t : niceTicks(ymin, ymax, nyt)) {
            float y = Y(t);
            dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), gridCol);
            std::string s = fmtTick(t);
            ImVec2 ts = ImGui::CalcTextSize(s.c_str());
            dl->AddText(ImVec2(a.x - ts.x - 4, y - ts.y * 0.5f), axisCol, s.c_str());
        }
    }
    dl->AddRect(a, b, axisCol);
    if (!opt.xlabel.empty()) {
        ImVec2 ts = ImGui::CalcTextSize(opt.xlabel.c_str());
        dl->AddText(ImVec2((a.x + b.x - ts.x) * 0.5f, b.y + fh * 1.35f), axisCol, opt.xlabel.c_str());
    }
    if (std::isfinite(opt.marker) && opt.marker >= xmin && opt.marker <= xmax)
        dl->AddLine(ImVec2(X(opt.marker), a.y), ImVec2(X(opt.marker), b.y), IM_COL32(255, 80, 80, 200), 1.5f);

    // Series (polyline segments split at NaNs, markers if few points).
    dl->PushClipRect(a, b, true);
    std::vector<ImVec2> pts;
    for (const auto& s : series) {
        pts.clear();
        auto flush = [&] {
            if (pts.size() >= 2) dl->AddPolyline(pts.data(), (int)pts.size(), s.color, 1.5f);
            pts.clear();
        };
        bool markers = s.x.size() <= 64;
        for (size_t i = 0; i < s.x.size(); ++i) {
            double y = s.y[i];
            if (!std::isfinite(y) || (opt.logY && y <= 0)) { flush(); continue; }
            ImVec2 p(X(s.x[i]), Y(y));
            pts.push_back(p);
            if (markers) dl->AddCircleFilled(p, 2.5f, s.color);
        }
        flush();
    }
    dl->PopClipRect();

    // Legend.
    float ly = a.y + 4;
    for (const auto& s : series) {
        if (s.label.empty()) continue;
        ImVec2 ts = ImGui::CalcTextSize(s.label.c_str());
        float lx = b.x - ts.x - 24;
        dl->AddLine(ImVec2(lx, ly + ts.y * 0.5f), ImVec2(lx + 16, ly + ts.y * 0.5f), s.color, 2.0f);
        dl->AddText(ImVec2(lx + 20, ly), axisCol, s.label.c_str());
        ly += ts.y + 2;
    }

    // Hover readout: nearest point of the first series.
    double mx = xmin + (ImGui::GetIO().MousePos.x - a.x) / (b.x - a.x) * (xmax - xmin);
    if (hovered && !series.empty() && !series[0].x.empty()) {
        float mxs = ImGui::GetIO().MousePos.x;
        dl->AddLine(ImVec2(mxs, a.y), ImVec2(mxs, b.y), IM_COL32(200, 200, 200, 90));
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(opt.xfmt ? opt.xfmt(mx).c_str() : fmtTick(mx).c_str());
        for (const auto& s : series) {
            if (s.x.empty()) continue;
            size_t best = 0;
            for (size_t i = 1; i < s.x.size(); ++i)
                if (std::fabs(s.x[i] - mx) < std::fabs(s.x[best] - mx)) best = i;
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(s.color), "%s %s", s.label.c_str(),
                               std::isfinite(s.y[best]) ? fmtTick(s.y[best]).c_str() : "missing");
        }
        ImGui::EndTooltip();
    }
    dl->PopClipRect();
    if (clicked && clickedX) *clickedX = mx;
    ImGui::PopID();
    return clicked;
}

} // namespace ncvpp
