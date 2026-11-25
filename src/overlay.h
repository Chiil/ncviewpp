// ncview++ — coastline overlays (data from the original ncview).
#pragma once
#include <cstdint>
#include <vector>

#include "dataset.h"

namespace ncvpp {

enum class Overlay { None, Coast, CoastHiRes };

// Data-grid cells (packed iy * nx + ix) hit by the overlay for a lon (x) / lat (y) grid.
std::vector<uint32_t> overlayCells(Overlay o, const DimInfo& xdim, const DimInfo& ydim);

} // namespace ncvpp
