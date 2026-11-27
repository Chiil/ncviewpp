// ncview++ — application state and UI.
#pragma once
#include <SDL3/SDL.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "dataset.h"
#include "loader.h"
#include "overlay.h"
#include "plot.h"
#include "render.h"

namespace ncvpp {

// Accumulated seconds spent in updateTexture stages (read by the NCVPP_PROFILE report in main).
inline double g_profRender = 0, g_profUpload = 0;

struct Options {
    std::vector<std::string> files;
    std::string var, cmap;
    bool haveMin = false, haveMax = false;
    float vmin = 0, vmax = 0;
    size_t cacheMB = 1024;
    std::string renderer;          // SDL render driver override
    std::string screenshot;        // test hook: save window to BMP after a few frames
    std::string dump;              // test hook: render the first slice to PPM and exit
    bool coasts = false;           // start with coastline overlay
    int frame = 0;                 // initial frame index along the animation dim
    MultiFile multi = MultiFile::Auto;   // how several files are combined
};

class App {
public:
    App(SDL_Renderer* ren, Dataset& ds, const Options& opt);
    ~App();

    void frame();                         // build the UI for one frame
    bool quit() const { return quit_; }
    int waitTimeoutMs() const;            // how long the main loop may sleep (-1 = until event)
    uint64_t version() const { return version_; }
    std::string title() const;

private:
    struct PlotLine {
        size_t ix = 0, iy = 0;
        std::vector<size_t> pos;
        int var = -1, dim = -1;
        PlotSeries series;
        bool ready = false;
    };
    struct ScanState {
        int gen = 0;
        bool full = false;
        SliceKey base;
        std::vector<int> dims;             // var dims iterated
        std::vector<size_t> lens;
        size_t total = 0, next = 0, done = 0;
        float lo = 0, hi = 0;
        bool any = false, finished = false;
    };

    void selectVar(int v);
    void pickAnimDim();
    void load1d();
    void reload();
    void exportImage();
    void defaultAxes();
    void autoFlip();
    SliceKey currentKey() const;
    SliceKey keyWithAnim(size_t idx) const;
    void updateTexture();
    void stepFrame(int delta, bool wrap);
    void setFrame(size_t idx);
    void startScan(bool full);
    void scanStep(std::shared_ptr<ScanState> st);
    void addPlotLine(size_t ix, size_t iy, bool append);
    void reloadPlotLines();
    void handleKeys();
    bool isImageVar() const;

    void drawSidebar(float width);
    void drawToolbar();
    void drawCanvas(ImVec2 size);
    void drawColorbar(float width);
    void drawStatus();
    void drawPlotWindow();
    void drawInfoWindow();

    SDL_Renderer* ren_;
    Dataset& ds_;
    Loader loader_;
    Options opt_;
    bool quit_ = false;
    uint64_t version_ = 1;

    // Selection
    int var_ = -1;
    std::vector<size_t> pos_;
    std::map<int, size_t> dimPos_;       // remembered index per global dim
    int xd_ = -1, yd_ = -1, animd_ = -1;  // positions within var dims

    // Playback
    int playDir_ = 0;
    float fps_ = 10.f;
    double lastAdvance_ = 0;

    // Range
    float vmin_ = 0, vmax_ = 1;
    bool rangeValid_ = false, rangeUser_ = false;
    int scanGen_ = 0;
    std::mutex scanMutex_;
    std::shared_ptr<ScanState> scan_;

    // Display
    RenderParams rp_;
    Overlay overlay_ = Overlay::None;
    bool smooth_ = false;
    bool fit_ = true;
    float zoom_ = 1.f, cx_ = 0, cy_ = 0;
    bool dragged_ = false;
    SDL_Texture* tex_ = nullptr;
    int texW_ = 0, texH_ = 0, maxTex_ = 8192;
    SliceKey shownKey_;
    SliceP shown_;
    bool renderDirty_ = true;
    std::vector<uint32_t> pixels_;
    std::vector<uint32_t> ovCells_;
    std::string ovKey_;
    bool loading_ = false;

    // Hover
    bool hoverValid_ = false;
    size_t hx_ = 0, hy_ = 0;

    // Plot window
    bool showPlot_ = false;
    int plotDim_ = -1;
    bool plotLogY_ = false;
    std::mutex plotMutex_;
    std::vector<std::shared_ptr<PlotLine>> lines_;
    std::shared_ptr<PlotLine> line1d_;   // whole-variable plot for 1-D variables

    // Transient status message (reload, export)
    std::string statusMsg_;
    double statusMsgT_ = 0;

    // UI
    bool showInfo_ = false;
    bool showCoordVars_ = false;
    char filter_[128] = "";
    float sidebarW_ = 0;
};

} // namespace ncvpp
