# KSP HMI flight data display for the Raspberry Pi 4 (Debian bookworm), Linux PCs (WSL included) and Windows
# (MSYS2 UCRT64 gcc: ./ci build_hmi builds it natively into build/hmi-cpp-win).
#
#   cmake -S hmi-cpp -B build/hmi-cpp -DCMAKE_BUILD_TYPE=Release && cmake --build build/hmi-cpp -j4
#   ctest --test-dir build/hmi-cpp --output-on-failure
#
# Dependencies (apt): build-essential cmake pkg-config libsdl2-dev libsdl2-ttf-dev fonts-freefont-ttf.
# Dependencies (MSYS2 pacman): mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,SDL2,SDL2_ttf}.
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

if(WIN32)
  # Winsock and windows.h without the min/max macros and GDI names (ERROR, ...) that clash with our code.
  add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX NOGDI _WIN32_WINNT=0x0601)
  # sdl2.pc links SDL2main and -mwindows (a GUI program without a console, entered through WinMain): ksphmi
  # and the tests keep their own main() (SDL_MAIN_HANDLED) and stay console programs that log to stdout.
  foreach(target PkgConfig::SDL2 PkgConfig::SDL2TTF)
    foreach(property INTERFACE_LINK_OPTIONS INTERFACE_LINK_LIBRARIES)
      get_target_property(values ${target} ${property})
      if(values)
        list(FILTER values EXCLUDE REGEX "^-mwindows$|SDL2main")
        set_target_properties(${target} PROPERTIES ${property} "${values}")
      endif()
    endforeach()
  endforeach()
endif()

# Pure logic: formatting, attitude and orbit geometry, config, scenes, discovery, kRPC client, telemetry sources.
add_library(ksphmi_core STATIC
  src/bootstate.cpp
  src/cockpitinput.cpp
  src/config.cpp
  src/discovery.cpp
  src/fmt.cpp
  src/intro.cpp
  src/editorpage.cpp
  src/gauges.cpp
  src/keyboard.cpp
  src/krpc.cpp
  src/log.cpp
  src/mechjeb.cpp
  src/mechjebinput.cpp
  src/navmath.cpp
  src/net.cpp
  src/netinfo.cpp
  src/orbitview.cpp
  src/pb.cpp
  src/redux_source.cpp
  src/scenes.cpp
  src/shipreport.cpp
  src/telemetry.cpp
  src/vec.cpp
  src/vesselview.cpp)
target_include_directories(ksphmi_core PUBLIC include)
target_compile_definitions(ksphmi_core PUBLIC KSPHMI_VERSION="${PROJECT_VERSION}")
target_link_libraries(ksphmi_core PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(ksphmi_core PUBLIC ws2_32 iphlpapi)
endif()

# Drawing: SDL2 renderer (GPU when available, software for /dev/fb0 and off-screen frames) and SDL2_ttf text.
add_library(ksphmi_ui STATIC
  src/apollo.cpp
  src/canvas.cpp
  src/cockpit.cpp
  src/display.cpp
  src/fbdev.cpp
  src/flightdeck.cpp
  src/gameskin.cpp
  src/mechjebpage.cpp
  src/navball.cpp
  src/orbitart.cpp
  src/png.cpp
  src/segments.cpp
  src/steampunk.cpp
  src/steamdials.cpp
  src/splash.cpp)
target_link_libraries(ksphmi_ui PUBLIC ksphmi_core PkgConfig::SDL2 PkgConfig::SDL2TTF)
if(WIN32)
  target_compile_definitions(ksphmi_ui PUBLIC SDL_MAIN_HANDLED)
endif()

add_executable(ksphmi src/main.cpp)
target_link_libraries(ksphmi PRIVATE ksphmi_ui)

enable_testing()
add_executable(ksphmi_tests tests/test_core.cpp)
target_link_libraries(ksphmi_tests PRIVATE ksphmi_core)
add_test(NAME core COMMAND ksphmi_tests)

add_executable(ksphmi_redux_tests tests/test_redux.cpp)
target_link_libraries(ksphmi_redux_tests PRIVATE ksphmi_core)
target_compile_definitions(ksphmi_redux_tests PRIVATE KSPHMI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME redux COMMAND ksphmi_redux_tests)

# Boot intro hand-over against a fake mpv IPC socket (AF_UNIX: Linux only, a single skip test elsewhere).
add_executable(ksphmi_intro_tests tests/test_intro.cpp)
target_link_libraries(ksphmi_intro_tests PRIVATE ksphmi_core)
add_test(NAME intro COMMAND ksphmi_intro_tests)

add_executable(ksphmi_render_tests tests/test_render.cpp)
target_link_libraries(ksphmi_render_tests PRIVATE ksphmi_ui)
add_test(NAME render COMMAND ksphmi_render_tests)
target_compile_definitions(ksphmi_tests PRIVATE KSPHMI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(ksphmi_render_tests PRIVATE KSPHMI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(ksphmi_tests PRIVATE -Wno-missing-field-initializers)

# setup-kiosk.sh edits the Pi's boot files with python3 (present on Raspberry Pi OS): test that part when we can.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME kiosk COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_setup_kiosk.py)
  add_test(NAME screenshots COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_screenshots.py $<TARGET_FILE:ksphmi>)
  set_tests_properties(screenshots PROPERTIES TIMEOUT 90)
endif()
