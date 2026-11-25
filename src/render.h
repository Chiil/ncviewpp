// ncview++ — colormaps and data→RGBA rendering.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "loader.h"

namespace ncvpp {

struct Colormap {
    std::string name;
    const int* rgb;   // 256 * 3 values 0..255
};
const std::vector<Colormap>& colormaps();
int findColormap(const std::string& name);

enum class Transform { Linear, Low, High, Log };
const char* transformName(Transform t);

struct RenderParams {
    float vmin = 0, vmax = 1;
    int cmap = 0;
    bool invert = false;
    Transform transform = Transform::Linear;
    uint32_t missing = 0xFF000000u;   // packed RGBA (little endian: R lowest byte)
    bool flipY = true;                // row 0 of the data at the bottom
    int decim = 1;                    // take every decim-th point (huge grids)
};

inline uint32_t packRGBA(int r, int g, int b, int a = 255) {
    return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24);
}

// 256-entry colour table after invert, for colour bars.
std::vector<uint32_t> colormapTable(int cmap, bool invert);
// Maps normalised t in [0,1] through the transform (non-log modes).
float applyTransform(Transform t, float x);

// Renders slice into out (row-major RGBA, outW x outH). Multi-threaded for big slices.
void renderSlice(const Slice& s, const RenderParams& p, std::vector<uint32_t>& out, int& outW, int& outH);

} // namespace ncvpp
