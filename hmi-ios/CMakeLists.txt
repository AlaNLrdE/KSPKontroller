# KSP HMI for iPhone: the hmi-cpp flight display and touch cockpit as an iOS app (design: docs/development/ios.rst).
# Builds for the iOS Simulator with Xcode 27.2 (2026-11-18). Xcode must be the developer directory:
# export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer when xcode-select still names the CLT.
#
#   cmake -S hmi-ios -B build/hmi-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DKSPHMI_IOS_FETCH=ON \
#         -DKSPHMI_IOS_TEAM=<personal team id> -DKSPHMI_IOS_BUNDLE_ID=<unique id>
#   cmake --build build/hmi-ios --config Debug -- -sdk iphonesimulator      # iOS Simulator
#   cmake --build build/hmi-ios --config Debug -- -sdk iphoneos             # device, free Apple ID signing
#
# KSPHMI_IOS_FETCH downloads the pinned SDL2 and SDL2_ttf release sources into build/hmi-ios/_deps (ignored):
# it stays OFF until the user approves that download. The versions match Homebrew's on the Mac (2026-11-18).
# Shared sources come from hmi-cpp/cmake/sources.cmake; nothing here changes the Pi, Mac, Linux or Windows builds.
cmake_minimum_required(VERSION 3.24)
project(ksphmi_ios VERSION 0.1.0 LANGUAGES C CXX OBJCXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
  message(FATAL_ERROR "hmi-ios builds only for iOS (-G Xcode -DCMAKE_SYSTEM_NAME=iOS); "
                      "other platforms build hmi-cpp/ directly")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OSX_DEPLOYMENT_TARGET 15.0 CACHE STRING "Minimum iOS version")
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

get_filename_component(HMI_CPP "${CMAKE_CURRENT_SOURCE_DIR}/../hmi-cpp" ABSOLUTE)

set(KSPHMI_IOS_TEAM "" CACHE STRING "Apple development team id (a free Apple ID's personal team works)")
set(KSPHMI_IOS_BUNDLE_ID "local.ksphmi.iphone" CACHE STRING
    "Bundle identifier; free signing needs one no other account has registered")

# --- SDL2 and SDL2_ttf from pinned release sources (static) ---------------------------------------------------
option(KSPHMI_IOS_FETCH "Download the pinned SDL2 / SDL2_ttf sources (only with the user's OK)" OFF)
set(KSPHMI_SDL2_VERSION 2.32.10)
set(KSPHMI_SDL2_TTF_VERSION 2.24.0)
# Pinned from the approved download of 2026-11-18 (shasum -a 256). SDL2_ttf matches Homebrew's formula checksum;
# SDL2's was taken over HTTPS from the official release (its GPG .sig not checked: no gpg on the Mac).
set(KSPHMI_SDL2_SHA256 "5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165")
set(KSPHMI_SDL2_TTF_SHA256 "0b2bf1e7b6568adbdbc9bb924643f79d9dedafe061fa1ed687d1d9ac4e453bfd")

if(NOT KSPHMI_IOS_FETCH)
  message(FATAL_ERROR "SDL2 ${KSPHMI_SDL2_VERSION} and SDL2_ttf ${KSPHMI_SDL2_TTF_VERSION} must be downloaded: "
                      "pass -DKSPHMI_IOS_FETCH=ON once the user has approved it")
endif()

include(FetchContent)
function(ksphmi_fetch name url sha)
  if(sha)
    FetchContent_Declare(${name} URL "${url}" URL_HASH SHA256=${sha} DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  else()
    message(WARNING "${name}: no SHA256 pinned yet, record the downloaded archive's hash in hmi-ios/CMakeLists.txt")
    FetchContent_Declare(${name} URL "${url}" DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  endif()
endfunction()
ksphmi_fetch(sdl2
  "https://github.com/libsdl-org/SDL/releases/download/release-${KSPHMI_SDL2_VERSION}/SDL2-${KSPHMI_SDL2_VERSION}.tar.gz"
  "${KSPHMI_SDL2_SHA256}")
ksphmi_fetch(sdl2_ttf
  "https://github.com/libsdl-org/SDL_ttf/releases/download/release-${KSPHMI_SDL2_TTF_VERSION}/SDL2_ttf-${KSPHMI_SDL2_TTF_VERSION}.tar.gz"
  "${KSPHMI_SDL2_TTF_SHA256}")

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
# The release archive carries FreeType and HarfBuzz under external/ (checked 2026-11-18).
set(SDL2TTF_VENDORED ON CACHE BOOL "" FORCE)
set(SDL2TTF_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDL2TTF_INSTALL OFF CACHE BOOL "" FORCE)
# CMake 4 dropped compatibility with cmake_minimum_required() below 3.5, which the vendored FreeType still
# declares; scoped to the fetched projects only.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(sdl2 sdl2_ttf)
unset(CMAKE_POLICY_VERSION_MINIMUM)

# --- The shared HMI libraries, same sources as every other platform -------------------------------------------
include("${HMI_CPP}/cmake/sources.cmake")
list(TRANSFORM KSPHMI_CORE_SOURCES PREPEND "${HMI_CPP}/")
list(TRANSFORM KSPHMI_UI_SOURCES PREPEND "${HMI_CPP}/")
find_package(Threads REQUIRED)

add_library(ksphmi_core STATIC ${KSPHMI_CORE_SOURCES})
target_include_directories(ksphmi_core PUBLIC "${HMI_CPP}/include")
target_compile_definitions(ksphmi_core PUBLIC KSPHMI_VERSION="${PROJECT_VERSION}-ios")
target_link_libraries(ksphmi_core PUBLIC Threads::Threads)

add_library(ksphmi_ui STATIC ${KSPHMI_UI_SOURCES})
target_link_libraries(ksphmi_ui PUBLIC ksphmi_core SDL2::SDL2-static SDL2_ttf::SDL2_ttf-static)

# --- The app bundle ------------------------------------------------------------------------------------------
# hmi-cpp's main.cpp unchanged: SDL.h renames main() to SDL_main() on iOS and SDL2main's UIApplicationMain calls
# it. src/platform.mm runs before both (static constructor) and prepares settings, paths and SDL hints.
add_executable(ksphmi_ios MACOSX_BUNDLE "${HMI_CPP}/src/main.cpp" src/platform.mm)
target_link_libraries(ksphmi_ios PRIVATE ksphmi_ui SDL2::SDL2main "-framework Foundation" "-framework UIKit")

# config.toml and assets/ keep their hmi-cpp layout under the bundle's hmi/ folder (find_app_dir through
# KSPHMI_APP_DIR, set by platform.mm); Settings.bundle must sit at the bundle root for the Settings app.
# "Resources/..." lands at the root of the flat iOS bundle (checked 2026-11-18); platform.mm accepts both.
set(bundle_files "${HMI_CPP}/config.toml")
set_source_files_properties("${HMI_CPP}/config.toml" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/hmi")
file(GLOB_RECURSE asset_files RELATIVE "${HMI_CPP}" CONFIGURE_DEPENDS "${HMI_CPP}/assets/*")
list(FILTER asset_files EXCLUDE REGEX "\\.(mp4|md)$")  # the Pi's boot intro video and notes stay out
foreach(rel IN LISTS asset_files)
  get_filename_component(dir "${rel}" DIRECTORY)
  set_source_files_properties("${HMI_CPP}/${rel}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/hmi/${dir}")
  list(APPEND bundle_files "${HMI_CPP}/${rel}")
endforeach()
file(GLOB settings_files CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Settings.bundle/*")
set_source_files_properties(${settings_files} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/Settings.bundle")
target_sources(ksphmi_ios PRIVATE ${bundle_files} ${settings_files})

set_target_properties(ksphmi_ios PROPERTIES
  OUTPUT_NAME "KSPHMI"
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
  MACOSX_BUNDLE_GUI_IDENTIFIER "${KSPHMI_IOS_BUNDLE_ID}"
  MACOSX_BUNDLE_BUNDLE_NAME "KSP HMI"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
  MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
  XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${KSPHMI_IOS_BUNDLE_ID}"
  XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${KSPHMI_IOS_TEAM}"
  XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
  XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1"        # iPhone only
  XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}"
  XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC "YES")
