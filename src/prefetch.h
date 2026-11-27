// ncview++ — warm the OS page cache with the coordinate variables of netCDF-4/HDF5 files.
#pragma once
#include <string>
#include <vector>

namespace ncvpp {

// Reads the stored chunks of all dimension-scale (coordinate) variables of the given files
// with many parallel preads, so the serial netCDF reads that follow come from memory.
// A record coordinate written one step per chunk (e.g. time, chunk size 1) is otherwise
// hundreds of scattered tiny reads per file, each a full round trip on a parallel file
// system. Not thread-safe with respect to netCDF/HDF5: call with the netCDF lock held.
// Non-HDF5 files are skipped; does nothing when built without HDF5.
void prefetchCoords(const std::vector<std::string>& paths);

} // namespace ncvpp
