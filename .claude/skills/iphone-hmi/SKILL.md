---
name: iphone-hmi
description: Building, running and testing the iPhone variant of the HMI (hmi-ios/, hmi-cpp built for iOS with SDL2) on the Mac: Xcode/CMake setup, the iOS Simulator against kspsim, simctl workarounds and how to hand the branch to the PC. Use before any work in hmi-ios/ or with the iOS Simulator.
---

## What it is

`hmi-ios/` is an iOS-only CMake project that builds the shared hmi-cpp sources (`hmi-cpp/cmake/sources.cmake`)
and `hmi-cpp/src/main.cpp` unchanged into `KSPHMI.app`. iOS specifics live only in `hmi-ios/` (Info.plist,
Settings.bundle, `src/platform.mm` start-up glue). Design and status: `docs/development/ios.rst`. Owner:
mac-iphonehmi; shared hmi-cpp changes are reviewed by mac-claude / pc-claude and need the PC's aarch64 cross
build + ctest before merge.

## Build (Mac)

- Xcode must be the developer directory. After installing Xcode, `xcode-select -p` may still name the Command
  Line Tools: `xcrun --sdk iphoneos` then fails and CMake says "iphoneos is not an iOS SDK". Without sudo,
  prefix every command with `export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer`.
- Configure + build for the simulator:
  `cmake -S hmi-ios -B build/hmi-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DKSPHMI_IOS_FETCH=ON` then
  `cmake --build build/hmi-ios --config Debug --target ksphmi_ios -- -sdk iphonesimulator -arch arm64 CODE_SIGNING_ALLOWED=NO`.
  `KSPHMI_IOS_FETCH` downloads pinned SDL2/SDL2_ttf (SHA256 in the CMakeLists): new versions need the user's OK.
- CMake 4 refuses the vendored FreeType's old `cmake_minimum_required`: keep `CMAKE_POLICY_VERSION_MINIMUM 3.5`
  scoped around `FetchContent_MakeAvailable` only.
- `Info.plist.in` goes through `configure_file`: a `${...}` that is not a plain variable, even inside an XML
  comment, is a CMake syntax error at generate time.
- Only two warnings from our code are expected (the same as macOS); everything else comes from SDL/FreeType/HarfBuzz.
- The iOS Simulator runtime is one per Mac (several GB, `xcodebuild -downloadPlatform iOS`, user's OK): check
  `xcrun simctl list runtimes` and the other Mac agents before starting another download.

## Run in the iOS Simulator against kspsim

- kspsim on loopback only, so the real Pi never discovers it: from `tools/`,
  `<main checkout>/build/venv/bin/python -m kspsim --no-board --address 127.0.0.1 --scenario orbit --headless --seconds 1800`
  (`--address` binds both kRPC 50000/50001 and the UDP 50002 context). The simulator shares the Mac's
  loopback, so the app's PC address is `127.0.0.1`.
- **One booted simulator at a time on the Mac** (8 GB RAM, 2026-11-19): two iOS 27.2 simulators booting together
  filled 6.7 GB of swap (load ~35); iOS's launch watchdog killed apps (0x8BADF00D, `mach_vm_allocate_kernel
  failed`) and simctl install/launch hung. Serialize with the mailbox lock `ios-simulator` and ask the other
  agent to shut its device down first; never shut down another agent's device yourself.
- macOS has no `timeout`: wrap every simctl call, e.g. `T(){ perl -e 'alarm shift; exec @ARGV' "$@"; }; T 60 xcrun simctl ...`.
  simctl calls can hang forever.
- A device can stay half booted (only CoreSimulatorBridge under its launchd_sim, no SpringBoard):
  `simctl shutdown`, `simctl boot`, then `simctl bootstatus <udid>` (about 4 min on the first boot) until Finished.
- `simctl spawn <udid> defaults write ...` hung: write the app's settings straight into its data container,
  `plutil -replace pc_host -string 127.0.0.1 "$(xcrun simctl get_app_container <udid> local.ksphmi.iphone data)/Library/Preferences/local.ksphmi.iphone.plist"`
  (create it first with `plutil -create xml1`). Keys: pc_host, source, skin, flight_scene (Settings.bundle).
- Sign the unsigned simulator build ad hoc before installing: `codesign --force --sign - --timestamp=none KSPHMI.app`.
- Launch with logs to files: `simctl launch --terminate-running-process --stdout=F --stderr=G <udid> local.ksphmi.iphone`.

## App lifecycle

- Backgrounding is handled in `hmi-cpp/src/main.cpp`'s event loop (no `#ifdef`): SDL_APP_WILLENTERBACKGROUND /
  DIDENTERBACKGROUND stop drawing and presenting, DIDENTERFOREGROUND resumes with a size check and full redraw.
  Log lines: `app entering background: drawing paused`, `app back in foreground: drawing resumed`.
- SDL2 on iOS delivers these from UIKit callbacks only while SDL_PumpEvents runs, so polling sees WILLENTER
  before the loop can present again. If iOS still kills the app on backgrounding, move the pause flag into an
  `SDL_AddEventWatch` callback (synchronous) as SDL's docs recommend. Unverified on simulator/device so far.

## Handing the branch to the PC

The mailbox cannot carry files (no upload; evidence is served only from the server host, MAX_BODY 16 KB per
message). Pushing only the `agent/hmi-ios` branch (never main) needs the user's explicit OK in this chat; verify
the remote ref equals HEAD with `git ls-remote` before sending RESULT. Headless pc-claude runs cannot build,
test or merge: they wait for an interactive PC session.
