cmake_minimum_required(VERSION 3.16)
project(ncviewpp VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(NCVPP_FETCH_SDL3 "Download and build SDL3 statically if it is not found" ON)
option(NCVPP_BUILD_TESTS "Build the test-data generator and tests" ON)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------- SDL3
find_package(SDL3 CONFIG QUIET)
if(SDL3_FOUND)
  message(STATUS "ncview++: using system SDL3 ${SDL3_VERSION}")
  set(NCVPP_SDL_TARGET SDL3::SDL3)
elseif(NCVPP_FETCH_SDL3)
  message(STATUS "ncview++: SDL3 not found, building it from source (static)")
  include(FetchContent)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  # Offline machines: pass -DFETCHCONTENT_SOURCE_DIR_SDL3=/path/to/SDL3-src
  FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.20.tar.gz)
  FetchContent_MakeAvailable(SDL3)
  set(NCVPP_SDL_TARGET SDL3::SDL3-static)
else()
  message(FATAL_ERROR "SDL3 not found. Set SDL3_DIR or enable NCVPP_FETCH_SDL3.")
endif()

# ---------------------------------------------------------------- netCDF
# Order: CMake config package, pkg-config, nc-config (what HPC modules provide).
find_package(netCDF CONFIG QUIET)
if(netCDF_FOUND AND TARGET netCDF::netcdf)
  set(NCVPP_NC_TARGET netCDF::netcdf)
else()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(NETCDF QUIET IMPORTED_TARGET netcdf)
  endif()
  if(NETCDF_FOUND)
    set(NCVPP_NC_TARGET PkgConfig::NETCDF)
  else()
    find_program(NC_CONFIG nc-config)
    if(NOT NC_CONFIG)
      message(FATAL_ERROR "netCDF-C not found (no CMake config, pkg-config or nc-config). Load your netcdf module.")
    endif()
    execute_process(COMMAND ${NC_CONFIG} --includedir OUTPUT_VARIABLE NC_INC OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${NC_CONFIG} --libs OUTPUT_VARIABLE NC_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
    separate_arguments(NC_LIBS UNIX_COMMAND "${NC_LIBS}")
    add_library(ncvpp_netcdf INTERFACE)
    target_include_directories(ncvpp_netcdf INTERFACE ${NC_INC})
    target_link_libraries(ncvpp_netcdf INTERFACE ${NC_LIBS})
    set(NCVPP_NC_TARGET ncvpp_netcdf)
  endif()
endif()

# ---------------------------------------------------------------- Dear ImGui (vendored)
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(imgui PUBLIC ${NCVPP_SDL_TARGET})

# ---------------------------------------------------------------- core (no GUI)
add_library(ncvpp_core STATIC
  src/cftime.cpp src/dataset.cpp src/loader.cpp src/render.cpp src/overlay.cpp)
target_include_directories(ncvpp_core PUBLIC src)
target_link_libraries(ncvpp_core PUBLIC ${NCVPP_NC_TARGET} Threads::Threads)

# ---------------------------------------------------------------- app
add_executable(ncview++ src/main.cpp src/app.cpp src/plot.cpp)
target_link_libraries(ncview++ PRIVATE ncvpp_core imgui)
target_compile_definitions(ncview++ PRIVATE NCVPP_VERSION="${PROJECT_VERSION}")
install(TARGETS ncview++ RUNTIME DESTINATION bin)

if(NCVPP_BUILD_TESTS)
  enable_testing()
  add_executable(make_testdata tests/make_testdata.cpp)
  target_link_libraries(make_testdata PRIVATE ${NCVPP_NC_TARGET})
  add_executable(test_core tests/test_core.cpp)
  target_link_libraries(test_core PRIVATE ncvpp_core)
  add_test(NAME core COMMAND test_core ${CMAKE_CURRENT_BINARY_DIR})
  set_tests_properties(core PROPERTIES DEPENDS make_testdata)
endif()
