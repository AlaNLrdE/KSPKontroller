# Source lists of the two HMI libraries, shared by hmi-cpp/CMakeLists.txt (Pi, Linux, macOS, Windows) and
# hmi-ios/CMakeLists.txt (iPhone app). Paths are relative to hmi-cpp/: prefix them with the caller's path to it.
# Adding a source here adds it to every platform; keep the iOS-only glue in hmi-ios/.

# Pure logic: formatting, attitude and orbit geometry, config, scenes, discovery, kRPC client, telemetry sources.
set(KSPHMI_CORE_SOURCES
  src/bootstate.cpp
  src/cockpitinput.cpp
  src/config.cpp
  src/discovery.cpp
  src/endpoint.cpp
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

# Drawing: SDL2 renderer (GPU when available, software for /dev/fb0 and off-screen frames) and SDL2_ttf text.
set(KSPHMI_UI_SOURCES
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
