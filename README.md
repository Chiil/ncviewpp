# ncview++

A fast, minimal netCDF viewer in the spirit of David W. Pierce's
[ncview](https://cirrus.ucsd.edu/ncview/), rewritten on **SDL3 + Dear ImGui** so it
runs natively on **Wayland and X11** (and macOS), locally or over `ssh -X`.

View only: no editing or printing.

## Features

- Variables grouped by dimensions, filter box, attribute viewer (`info`)
- Any two dimensions as X/Y, sliders for all others, pick the animation dimension
- Playback with background prefetching (frames are never skipped)
- CF time decoding for all CF calendars (standard, proleptic_gregorian, julian, noleap, all_leap, 360_day)
- `_FillValue`, `missing_value`, `valid_range`, `scale_factor`/`add_offset`
- Multiple files concatenated along the record (unlimited) dimension: `ncview++ run_*.nc`
- Multiple files with different variables merged side by side: `ncview++ u.nc v.nc stats.nc`.
  Only variables on matching dimensions (same length and coordinates) are shown, on the time
  steps common to all files. Chosen automatically; force with `--concat` or `--merge`
- The 24 ncview colormaps, invert, linear/low/high/log transforms
- Colour range: sampled auto-range (like ncview), current frame, or exact scan of all frames
- Zoom (mouse wheel) and pan (drag), pixel-exact or smooth display, fit/1:1
- Click: time series (or profile along any dim) at a point; ctrl-click adds lines;
  click in the plot to jump to that frame
- Coastline overlays on lon/lat grids (ncview's 0.8° and 0.08° data)
- North is always up, whether latitude is stored N→S or S→N

## Performance design

- Only the displayed 2-D hyperslab is read; a dedicated I/O thread with a 1 GB LRU cache
  (`--cache MB`) prefetches the next frames, so the UI never blocks on disk.
- A 256 MB netCDF-4 chunk cache speeds up compressed data.
- Colour mapping uses a 4096-entry lookup table and is multi-threaded; an ERA5-sized
  1440×721 compressed frame reads and renders in about 5 ms (~190 fps).
- The image is one texture. Zooming and panning are done by the renderer.
- Event-driven: nothing is redrawn while idle, and frames identical to the last one are not
  re-sent. Over `ssh -X` only real changes go over the network.

## Building

Requirements: a C++17 compiler, CMake ≥ 3.16, netCDF-C. SDL3 is used from the system when
available, otherwise downloaded and built statically. Dear ImGui is bundled.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/ncview++ file.nc
```

On an HPC system:

```sh
module load cmake netcdf-c      # names vary per site
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=$HOME/.local
cmake --build build -j && cmake --install build
```

netCDF is found with its CMake package, then pkg-config, then `nc-config`. If the machine
has no internet, download the SDL3 source somewhere else and pass
`-DFETCHCONTENT_SOURCE_DIR_SDL3=/path/to/SDL-release-3.4.20`.

When SDL3 is built from source on Linux, install the X11/Wayland development headers first
(e.g. `libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libwayland-dev
libxkbcommon-dev libdecor-0-dev`), or SDL may lack those backends. Without libdecor,
windows on GNOME Wayland have no title bar.

## Running remotely

```sh
ssh -X -C user@cluster          # -C (compression) helps a lot
ncview++ output.nc
```

ncview++ switches to the software renderer automatically when it detects an ssh session.
Use `--renderer opengl` to override, e.g. under VirtualGL. To force X11 on a Wayland
desktop, set `SDL_VIDEO_DRIVER=x11`.

## Keys

| key | action |
|---|---|
| space | play / pause |
| ← / → (shift: ×10) | step frame |
| home / end | first / last frame |
| ↑ / ↓, PgUp / PgDn | previous / next variable |
| c / C | next / previous colormap |
| i | invert colormap |
| t | cycle transform |
| r / a | range = current frame / auto range |
| o | coastlines on/off |
| f, + / - | fit, zoom |
| p | plot window |
| F5 / ctrl-r | reload the files (picks up new frames; keeps frame and colour range) |
| q | quit |

Mouse: wheel zooms, drag pans, click plots a series, ctrl/shift-click adds a series,
double-click fits.

## Tests

```sh
./build/make_testdata build --big   # synthetic files incl. an ERA5-sized one
./build/test_core build             # correctness tests + benchmark
./build/ncview++ --dump out.ppm -v tas build/tas_all.nc   # headless render
```

## License

GPL-3.0 (see `COPYING`). The colormaps and coastline data come from ncview © David W. Pierce.
Dear ImGui is MIT licensed. The Roboto font is Apache-2.0.
