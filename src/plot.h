// ncview++ — minimal, fast line plot drawn with ImDrawList.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "imgui.h"

namespace ncvpp {

struct PlotSeries {
    std::vector<double> x, y;   // y may contain NaN (gaps)
    std::string label;
    ImU32 color = 0xFFFFFFFF;
};

struct PlotOptions {
    std::string xlabel, ylabel;
    std::function<std::string(double)> xfmt;   // optional tick label formatter
    double marker = NAN;                       // vertical marker at this x (current frame)
    bool logY = false;
};

// Draws the plot filling `size`. Returns true when clicked; *clickedX is the x value.
bool drawPlot(const char* id, ImVec2 size, const std::vector<PlotSeries>& series,
              const PlotOptions& opt, double* clickedX);

// "Nice" tick positions covering [lo, hi].
std::vector<double> niceTicks(double lo, double hi, int target);

} // namespace ncvpp
