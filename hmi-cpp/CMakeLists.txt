# KSP HMI flight data display for the Raspberry Pi 4 (Debian bookworm) and Linux PCs (WSL included).
#
#   cmake -S hmi-cpp -B build/hmi-cpp -DCMAKE_BUILD_TYPE=Release && cmake --build build/hmi-cpp -j4
#   ctest --test-dir build/hmi-cpp --output-on-failure
#
# Dependencies (apt): build-essential cmake pkg-config libsdl2-dev libsdl2-ttf-dev fonts-freefont-ttf.
# The kRPC wire protocol is encoded by hand (src/pb.cpp, include/pb.hpp): no libprotobuf / protoc needed.
#
# Layout: src/ holds only .cpp files, include/ only .hpp headers (./ci verify checks it); tests/ keeps the
# test programs and their helper headers.
cmake_minimum_required(VERSION 3.16)
project(ksphmi VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(SDL2TTF REQUIRED IMPORTED_TARGET SDL2_ttf)

# Pure logic: formatting, attitude and orbit geometry, config, scenes, discovery, kRPC client, telemetry sources.
add_library(ksphmi_core STATIC
  src/bootstate.cpp
  src/config.cpp
  src/discovery.cpp
  src/fmt.cpp
  src/gauges.cpp
  src/krpc.cpp
  src/log.cpp
  src/navmath.cpp
  src/netinfo.cpp
  src/orbitview.cpp
  src/pb.cpp
  src/scenes.cpp
  src/telemetry.cpp
  src/vec.cpp)
target_include_directories(ksphmi_core PUBLIC include)
target_compile_definitions(ksphmi_core PUBLIC KSPHMI_VERSION="${PROJECT_VERSION}")
target_link_libraries(ksphmi_core PUBLIC Threads::Threads)

# Drawing: SDL2 renderer (GPU when available, software for /dev/fb0 and off-screen frames) and SDL2_ttf text.
add_library(ksphmi_ui STATIC
  src/apollo.cpp
  src/canvas.cpp
  src/display.cpp
  src/fbdev.cpp
  src/navball.cpp
  src/png.cpp
  src/segments.cpp
  src/splash.cpp)
target_link_libraries(ksphmi_ui PUBLIC ksphmi_core PkgConfig::SDL2 PkgConfig::SDL2TTF)

add_executable(ksphmi src/main.cpp)
target_link_libraries(ksphmi PRIVATE ksphmi_ui)

enable_testing()
add_executable(ksphmi_tests tests/test_core.cpp)
target_link_libraries(ksphmi_tests PRIVATE ksphmi_core)
add_test(NAME core COMMAND ksphmi_tests)

add_executable(ksphmi_render_tests tests/test_render.cpp)
target_link_libraries(ksphmi_render_tests PRIVATE ksphmi_ui)
add_test(NAME render COMMAND ksphmi_render_tests)
target_compile_definitions(ksphmi_tests PRIVATE KSPHMI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(ksphmi_tests PRIVATE -Wno-missing-field-initializers)

# setup-kiosk.sh edits the Pi's boot files with python3 (present on Raspberry Pi OS): test that part when we can.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME kiosk COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_setup_kiosk.py)
endif()
