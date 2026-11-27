// ncview++ — publication-style PNG export: the full slice at native resolution, with axes,
// ticks, labels, title and colour bar, on a white background.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "dataset.h"
#include "loader.h"
#include "overlay.h"
#include "render.h"

namespace ncvpp {

struct ExportMarker {
    size_t ix = 0, iy = 0;           // data cell
    uint32_t rgba = 0xFFFFFFFFu;     // packed like packRGBA
};

struct ExportSpec {
    const Slice* slice = nullptr;
    const DimInfo* xdim = nullptr;
    const DimInfo* ydim = nullptr;
    RenderParams rp;                 // colours, range and flipY (decimation is ignored)
    std::string title, subtitle, cbarLabel;
    Overlay overlay = Overlay::None; // drawn only on lon/lat grids
    std::vector<ExportMarker> markers;
    int dpi = 300;
    int minSize = 1800;              // image is upscaled by an integer factor to at least this (px)
};

// Fills title, subtitle, colour bar label and axis dims from the dataset.
ExportSpec makeExportSpec(const Dataset& ds, const SliceKey& key, const Slice& s, const RenderParams& rp);

// Default file name: <var>[_<frame index>].png, with '/' of group paths replaced.
std::string exportFileName(const Dataset& ds, const SliceKey& key, int animDim);

bool exportPNG(const std::string& path, const ExportSpec& spec, std::string& err);

// The embedded UI font (Roboto Medium TTF), shared with the GUI.
const unsigned char* uiFontTTF(int* len);

} // namespace ncvpp
