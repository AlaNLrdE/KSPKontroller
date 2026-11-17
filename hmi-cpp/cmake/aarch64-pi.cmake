# CMake toolchain: cross-compile hmi-cpp for the Raspberry Pi 4 (Debian 13 trixie, arm64) on a PC or Mac.
# ./ci build_hmi_pi uses it; ./ci hmi_sysroot copies the Pi's headers and libraries into build/pi-sysroot first.
#
#   cmake -S hmi-cpp -B build/hmi-cpp-pi -DCMAKE_TOOLCHAIN_FILE=hmi-cpp/cmake/aarch64-pi.cmake \
#         -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ -DKSPHMI_TEST_SOURCE_DIR=.
#
# KSPHMI_PI_SYSROOT (cache or environment) overrides the sysroot; the default is build/pi-sysroot of this checkout.
# The compiler is GNU aarch64-linux-gnu-g++ (Ubuntu: g++-aarch64-linux-gnu). Its own libstdc++ headers are used:
# they must not be newer than the Pi's libstdc++.so.6, which ./ci build_hmi_pi checks on the binaries (readelf -V).
# A clang build (macOS) would instead pass --target=aarch64-linux-gnu, -fuse-ld=lld and the Pi's gcc 14 headers
# from the sysroot (docs/building.rst); not implemented yet.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT KSPHMI_PI_SYSROOT)
  if(DEFINED ENV{KSPHMI_PI_SYSROOT})
    set(KSPHMI_PI_SYSROOT "$ENV{KSPHMI_PI_SYSROOT}")
  else()
    get_filename_component(KSPHMI_PI_SYSROOT "${CMAKE_CURRENT_LIST_DIR}/../../build/pi-sysroot" ABSOLUTE)
  endif()
endif()
if(NOT EXISTS "${KSPHMI_PI_SYSROOT}/usr/include/SDL2/SDL.h")
  message(FATAL_ERROR "No Raspberry Pi sysroot at ${KSPHMI_PI_SYSROOT}: run ./ci hmi_sysroot")
endif()
# try_compile projects re-read this file without the cache: hand them the sysroot
set(ENV{KSPHMI_PI_SYSROOT} "${KSPHMI_PI_SYSROOT}")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES KSPHMI_PI_SYSROOT)

set(CMAKE_SYSROOT "${KSPHMI_PI_SYSROOT}")
if(NOT CMAKE_CXX_COMPILER)
  set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
endif()

# Programs (python3 for the kiosk/screenshot tests) from this machine; headers, libraries and packages only from
# the Pi's sysroot.
set(CMAKE_FIND_ROOT_PATH "${KSPHMI_PI_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# pkg-config (SDL2, SDL2_ttf) reads only the Pi's .pc files and prefixes their paths with the sysroot.
set(ENV{PKG_CONFIG_DIR} "")
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_LIBDIR} "${KSPHMI_PI_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:${KSPHMI_PI_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${KSPHMI_PI_SYSROOT}")
