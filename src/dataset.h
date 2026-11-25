// ncview++ — netCDF dataset access (one or more files concatenated along the record dimension).
#pragma once
#include <string>
#include <utility>
#include <vector>

#include "cftime.h"

namespace ncvpp {

enum class AxisKind { Other, Lon, Lat, Vertical, Time };

struct GroupInfo {
    std::string path;                // "" for the root group, else e.g. "thermo/budget"
    std::string name;                // last path component ("" for root)
    int parent = -1;                 // index into Dataset::groups(), -1 for root
    std::vector<int> children;       // indices into Dataset::groups()
    std::vector<int> ncid;           // group ncid per file (-1 if absent)
};

struct DimInfo {
    std::string name;
    int group = 0;                   // index into Dataset::groups() where the dim is defined
    size_t len = 0;
    bool isRecord = false;           // aggregated across files
    bool hasCoordVar = false;
    std::vector<double> coord;       // coordinate values (indices if none)
    std::string units, longName;
    CFTime time;
    AxisKind kind = AxisKind::Other;
    bool monotonic = true;
    std::string format(size_t idx) const;      // pretty value at index
    std::string formatValue(double v) const;   // pretty value of a coordinate value
};

struct VarInfo {
    std::string name, longName, units;
    std::string fullName;            // group path + "/" + name ("name" in the root group)
    int group = 0;                   // index into Dataset::groups()
    std::vector<int> varid;          // per file
    std::vector<int> ncid;           // group ncid per file
    std::vector<int> dims;           // indices into Dataset::dims()
    int type = 0;                    // nc_type
    bool hasFill = false;  float fill = 0;
    bool hasMissing = false; float missing = 0;
    bool hasValidMin = false; float validMin = 0;
    bool hasValidMax = false; float validMax = 0;
    double scale = 1.0, offset = 0.0;
    bool isRecordVar = false;        // first dim is the aggregated record dim
    bool isCoordVar = false;
    std::string coordinatesAttr;     // CF "coordinates" attribute (informational)
};

class Dataset {
public:
    Dataset() = default;
    ~Dataset();
    Dataset(const Dataset&) = delete;
    Dataset& operator=(const Dataset&) = delete;

    bool open(const std::vector<std::string>& paths, std::string& err);

    const std::vector<GroupInfo>& groups() const { return groups_; }
    const std::vector<DimInfo>& dims() const { return dims_; }
    const std::vector<VarInfo>& vars() const { return vars_; }
    const std::vector<std::string>& paths() const { return paths_; }
    std::vector<size_t> shape(int v) const;
    // Finds by full path ("grp/sub/var", leading '/' optional), else by bare name (first match).
    int findVar(const std::string& name) const;

    // Thread-safe hyperslab read in the variable's dimension order. Missing values
    // (fill, missing_value, outside valid range, non-finite) become NaN; scale/offset applied.
    bool read(int v, const std::vector<size_t>& start, const std::vector<size_t>& count,
              std::vector<float>& out, std::string* err = nullptr) const;

    // "name = value" attribute strings for a variable (v = -1: global attributes).
    std::vector<std::pair<std::string, std::string>> attributes(int v) const;
    // Attributes of a group (0 = root, i.e. the global attributes).
    std::vector<std::pair<std::string, std::string>> groupAttributes(int g) const;

private:
    struct File { std::string path; int ncid = -1; size_t recStart = 0, recLen = 0; };
    bool readRaw(int v, const std::vector<size_t>& start, const std::vector<size_t>& count,
                 float* fout, double* dout, std::string* err) const;
    void loadCoords();
    void scanGroup(int ncid, int parent, const std::string& name);
    static std::vector<std::pair<std::string, std::string>> attrsOf(int ncid, int varid);

    std::vector<std::string> paths_;
    std::vector<File> files_;
    std::vector<GroupInfo> groups_;
    std::vector<DimInfo> dims_;
    std::vector<VarInfo> vars_;
    int recDim_ = -1;
};

} // namespace ncvpp
