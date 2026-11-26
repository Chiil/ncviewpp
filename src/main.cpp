// ncview++ — a fast netCDF viewer for X11 and Wayland (SDL3 + Dear ImGui).
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#ifndef _WIN32
#include <csignal>
#include <pthread.h>
#include <unistd.h>
#endif

#include "app.h"
#include "data/font_roboto.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#ifndef NCVPP_VERSION
#define NCVPP_VERSION "dev"
#endif

using namespace ncvpp;

static void usage() {
    std::printf(
        "ncview++ %s — fast netCDF viewer\n\n"
        "usage: ncview++ [options] file.nc [more.nc ...]\n"
        "  Multiple files are concatenated along the record (unlimited) dimension when their\n"
        "  times follow each other, and merged otherwise: variables of all files side by side,\n"
        "  showing only those on matching dimensions and the time steps common to all files.\n\n"
        "options:\n"
        "  -v, --var NAME        variable to show first\n"
        "  -c, --cmap NAME       colormap (3gauss, detail, ssec, bright, jet, bw, ...)\n"
        "      --min V --max V   fixed colour range\n"
        "  -f, --frame N         start at frame N of the animation dimension\n"
        "      --concat          always concatenate multiple files along the record dimension\n"
        "      --merge           always merge multiple files (variables side by side)\n"
        "      --coasts          start with the coastline overlay on\n"
        "      --cache MB        slice cache size (default 1024)\n"
        "      --software        force software rendering (default over ssh)\n"
        "      --renderer NAME   SDL render driver (opengl, vulkan, metal, gpu, software)\n"
        "      --dump OUT.ppm    render the first frame to a PPM image and exit (no display needed)\n"
        "  -h, --help            this help\n\n"
        "keys: space play/pause, left/right step (shift: x10), home/end, up/down variable,\n"
        "      c/C colormap, i invert, t transform, r range=frame, a auto range, o coasts,\n"
        "      f fit, +/- zoom, p plot window, F5/ctrl-r reload, q quit. Mouse: wheel zoom, drag pan,\n"
        "      click plot, ctrl-click add line, double-click fit.\n",
        NCVPP_VERSION);
}

static bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "ncview++: %s needs a value\n", what); std::exit(2); }
            return argv[++i];
        };
        if (a == "-h" || a == "--help" || a == "-help") { usage(); std::exit(0); }
        else if (a == "--version") { std::printf("ncview++ %s\n", NCVPP_VERSION); std::exit(0); }
        else if (a == "-v" || a == "--var") o.var = next("--var");
        else if (a == "-c" || a == "--cmap") o.cmap = next("--cmap");
        else if (a == "--min") { o.vmin = std::strtof(next("--min"), nullptr); o.haveMin = true; }
        else if (a == "--max") { o.vmax = std::strtof(next("--max"), nullptr); o.haveMax = true; }
        else if (a == "-f" || a == "--frame") o.frame = std::atoi(next("--frame"));
        else if (a == "--cache") o.cacheMB = (size_t)std::atol(next("--cache"));
        else if (a == "--coasts") o.coasts = true;
        else if (a == "--concat") o.multi = MultiFile::Concat;
        else if (a == "--merge") o.multi = MultiFile::Merge;
        else if (a == "--software") o.renderer = "software";
        else if (a == "--renderer") o.renderer = next("--renderer");
        else if (a == "--dump") o.dump = next("--dump");
        else if (a == "--screenshot") o.screenshot = next("--screenshot");
        else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "ncview++: unknown option %s (try --help)\n", a.c_str()); return false; }
        else o.files.push_back(a);
    }
    if (o.files.empty()) { usage(); return false; }
    return true;
}

// Headless: render the first frame of a variable to a binary PPM.
static int dumpPPM(Dataset& ds, const Options& o) {
    int v = o.var.empty() ? -1 : ds.findVar(o.var);
    for (size_t i = 0; v < 0 && i < ds.vars().size(); ++i)
        if (ds.vars()[i].dims.size() >= 2 && !ds.vars()[i].isCoordVar) v = (int)i;
    if (v < 0) { std::fprintf(stderr, "ncview++: no 2-D variable to dump\n"); return 1; }
    auto shape = ds.shape(v);
    SliceKey k;
    k.var = v;
    k.xd = (int)shape.size() - 1;
    k.yd = (int)shape.size() - 2;
    k.pos.assign(shape.size(), 0);
    if (shape.size() > 2) k.pos[0] = std::min<size_t>(o.frame, shape[0] - 1);
    k.pos[k.xd] = k.pos[k.yd] = 0;
    SliceP s = readSlice(ds, k);
    if (!s->error.empty()) { std::fprintf(stderr, "ncview++: %s\n", s->error.c_str()); return 1; }
    RenderParams p;
    p.vmin = o.haveMin ? o.vmin : s->vmin;
    p.vmax = o.haveMax ? o.vmax : s->vmax;
    if (!o.cmap.empty() && findColormap(o.cmap) >= 0) p.cmap = findColormap(o.cmap);
    std::vector<uint32_t> px;
    int w, h;
    renderSlice(*s, p, px, w, h);
    FILE* f = std::fopen(o.dump.c_str(), "wb");
    if (!f) { std::perror(o.dump.c_str()); return 1; }
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (uint32_t c : px) { unsigned char rgb[3] = {(unsigned char)(c & 255), (unsigned char)((c >> 8) & 255), (unsigned char)((c >> 16) & 255)}; std::fwrite(rgb, 1, 3, f); }
    std::fclose(f);
    std::printf("%s: %s %dx%d min=%g max=%g\n", o.dump.c_str(), ds.vars()[v].fullName.c_str(), w, h, s->vmin, s->vmax);
    return 0;
}

// FNV-1a over the draw lists: lets us skip presenting identical frames (big win over ssh -X).
static uint64_t hashDrawData(const ImDrawData* dd) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {
        const unsigned char* b = (const unsigned char*)p;
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    };
    for (const ImDrawList* l : dd->CmdLists) {
        mix(l->VtxBuffer.Data, l->VtxBuffer.Size * sizeof(ImDrawVert));
        mix(l->IdxBuffer.Data, l->IdxBuffer.Size * sizeof(ImDrawIdx));
        for (const ImDrawCmd& c : l->CmdBuffer) {
            ImTextureID t = c.GetTexID();
            mix(&t, sizeof t);
            mix(&c.ClipRect, sizeof c.ClipRect);
            mix(&c.UserCallback, sizeof c.UserCallback);
        }
    }
    return h;
}

// SDL's own SIGINT handler only sets a flag that is checked when events are pumped, so while the
// main loop sleeps in SDL_WaitEvent, Ctrl+C is not noticed until some other event arrives. Instead,
// block SIGINT/SIGTERM in all threads and handle them on a dedicated thread that pushes a quit
// event (which wakes the event loop immediately). A second signal exits right away.
static void installSignalThread() {
#ifndef _WIN32
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);  // inherited by every thread created afterwards
    std::thread([set] {
        int sig = 0, count = 0;
        while (sigwait(&set, &sig) == 0) {
            if (++count > 1 || !SDL_WasInit(SDL_INIT_EVENTS)) _exit(128 + sig);
            SDL_Event e{};
            e.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&e);
        }
    }).detach();
#endif
}

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) return 2;

    Dataset ds;
    std::string err;
    if (!ds.open(opt.files, err, opt.multi)) { std::fprintf(stderr, "ncview++: %s\n", err.c_str()); return 1; }
    for (const auto& n : ds.notes()) std::fprintf(stderr, "ncview++: %s\n", n.c_str());
    if (ds.vars().empty()) { std::fprintf(stderr, "ncview++: no displayable variables\n"); return 1; }
    if (!opt.dump.empty()) return dumpPPM(ds, opt);

    // Over ssh (X11 forwarding) GPU rendering is usually unavailable or slow: use software.
    // A DISPLAY with a host part (e.g. "localhost:10.0") is a remote/forwarded X server, even when
    // the ssh variables are not visible (sudo, screen/tmux started elsewhere, job launchers).
    const char* display = std::getenv("DISPLAY");
    bool remoteX = display && display[0] != ':' && display[0] != '/' && std::strchr(display, ':');
    bool remote = remoteX || std::getenv("SSH_CONNECTION") || std::getenv("SSH_CLIENT");
    if (opt.renderer.empty() && remote && !std::getenv("WAYLAND_DISPLAY")) opt.renderer = "software";
    if (opt.renderer == "software") {
        // Keep SDL away from GLX entirely: a failing GLX call over forwarded X aborts the process
        // with an X error, so it cannot be caught and fallen back from.
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
        SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");
    }
    SDL_SetHint(SDL_HINT_APP_NAME, "ncview++");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
    installSignalThread();

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "ncview++: cannot open display: %s\n"
                     "  (no $DISPLAY / $WAYLAND_DISPLAY? use 'ssh -X' or 'ssh -Y'; or --dump for headless output)\n",
                     SDL_GetError());
        return 1;
    }
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale <= 0) scale = 1;
    SDL_Window* win = SDL_CreateWindow("ncview++", int(1280 * scale), int(820 * scale),
                                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!win) { std::fprintf(stderr, "ncview++: SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    SDL_Renderer* ren = SDL_CreateRenderer(win, opt.renderer.empty() ? nullptr : opt.renderer.c_str());
    if (!ren && !opt.renderer.empty()) {
        std::fprintf(stderr, "ncview++: renderer '%s' failed (%s), using default\n", opt.renderer.c_str(), SDL_GetError());
        ren = SDL_CreateRenderer(win, nullptr);
    }
    if (!ren) { std::fprintf(stderr, "ncview++: SDL_CreateRenderer: %s\n", SDL_GetError()); return 1; }
    SDL_SetRenderVSync(ren, 1);
    SDL_ShowWindow(win);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // never write imgui.ini into the user's data directory
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.FrameRounding = 3;
    style.GrabRounding = 3;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF((void*)roboto_medium_ttf, (int)roboto_medium_ttf_len, 15.0f, &fc);
    ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer3_Init(ren);

    {
        App app(ren, ds, opt);
        std::string lastTitle;
        uint64_t lastHash = 0, lastVersion = 0;
        int settle = 3;           // ImGui needs a few frames after input to reach a steady state
        int frames = 0;
        bool forcePresent = true;
        while (!app.quit()) {
            if (!opt.screenshot.empty()) settle = 3;   // test hook: keep rendering
            SDL_Event e;
            bool got = false;
            if (settle > 0) got = SDL_PollEvent(&e);
            else {
                int to = app.waitTimeoutMs();
                got = to < 0 ? SDL_WaitEvent(&e) : SDL_WaitEventTimeout(&e, to);
            }
            bool quit = false;
            while (got) {
                ImGui_ImplSDL3_ProcessEvent(&e);
                if (e.type == SDL_EVENT_QUIT) quit = true;
                if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(win)) quit = true;
                if (e.type == SDL_EVENT_WINDOW_EXPOSED || e.type == SDL_EVENT_WINDOW_RESIZED ||
                    e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || e.type == SDL_EVENT_WINDOW_SHOWN)
                    forcePresent = true;
                settle = 3;
                got = SDL_PollEvent(&e);
            }
            if (quit) break;
            if (SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED) { SDL_Delay(20); continue; }

            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            app.frame();
            ImGui::Render();
            if (settle > 0) --settle;

            std::string t = app.title();
            if (t != lastTitle) { SDL_SetWindowTitle(win, t.c_str()); lastTitle = t; }

            uint64_t h = hashDrawData(ImGui::GetDrawData());
            ++frames;
            bool shot = !opt.screenshot.empty() && frames == 40;
            if (h == lastHash && app.version() == lastVersion && !forcePresent && !shot) continue;
            lastHash = h;
            lastVersion = app.version();
            forcePresent = false;

            SDL_SetRenderScale(ren, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
            SDL_SetRenderDrawColor(ren, 30, 30, 34, 255);
            SDL_RenderClear(ren);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
            if (shot) {
                SDL_Surface* s = SDL_RenderReadPixels(ren, nullptr);
                if (s) { SDL_SaveBMP(s, opt.screenshot.c_str()); SDL_DestroySurface(s); }
                SDL_RenderPresent(ren);
                break;
            }
            SDL_RenderPresent(ren);
        }
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
