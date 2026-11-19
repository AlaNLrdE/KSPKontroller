iPhone HMI development
======================

Status: **design note and build skeleton** (mac-iphonehmi, 2026-11-18). Nothing in this page has run on
iOS yet: Xcode was still installing when it was written, so every item under "Unverified assumptions"
must be checked on the first Xcode build before it is described as working.

Scope (confirmed with the user, 2026-11-18)
-------------------------------------------

The iPhone app is the hmi-cpp HMI (C++17, SDL2, SDL2_ttf) built for iOS, not a new client:

- **First delivery**: the existing flight view and the clickable cockpit panel, with touch as the input.
  The cockpit already accepts clicks and touch screens' synthesized mouse events (``CockpitInput``,
  :doc:`../hmi` "Cockpit panel"), so actions reach the game exactly as on the Pi: through the source's
  command queue to the KSPHMI / KSPHMIFlight services. Held axes and throttle go through KSPHMIFlight,
  editor and facility keys through ``KSPHMI.Press``, never kRPC ``Control`` axes or throttle; kRPC calls
  stay on the telemetry thread, never in the frame loop. KSP 2 Redux works where the Redux source already
  does (read-only telemetry over UDP 50003).
- **Screen**: full screen at the native pixel size, in both portrait and landscape, rebuilt on rotation.
- **Network**: the PC address is typed in the iOS Settings app. UDP broadcast discovery (port 50002) is
  off on iOS: it needs the ``com.apple.developer.networking.multicast`` entitlement, which a free Apple ID
  cannot obtain.
- **Signing**: a free Apple ID's personal team. Apps signed that way expire after 7 days and are
  reinstalled from Xcode. Development starts in the iOS Simulator against ``./ci sim``; the device comes
  after that.
- **Dependencies**: CMake generates the Xcode project and fetches pinned SDL2 / SDL2_ttf release sources
  into the ignored ``build/hmi-ios/_deps``, only after the user approves that download.

Layout and ownership
--------------------

.. list-table::
   :header-rows: 1
   :widths: 34 46 20

   * - Path
     - Content
     - Owner
   * - ``hmi-ios/CMakeLists.txt``
     - iOS-only CMake project: pinned SDL2 2.32.10 / SDL2_ttf 2.24.0 (static), the two shared libraries
       from ``hmi-cpp/cmake/sources.cmake``, the ``KSPHMI.app`` bundle and its Xcode signing attributes.
     - mac-iphonehmi
   * - ``hmi-ios/Info.plist.in``
     - Bundle keys: orientations, hidden status bar, local-network usage text, Files app access to
       ``Documents/``.
     - mac-iphonehmi
   * - ``hmi-ios/Settings.bundle/``
     - Settings app page: PC address, game (kRPC, Redux, automatic, demo), skin, first flight scene.
     - mac-iphonehmi
   * - ``hmi-ios/src/platform.mm``
     - Start-up glue (static constructor before SDL's ``UIApplicationMain``): sets ``KSPHMI_APP_DIR``,
       writes ``Documents/config.local.toml`` from the settings (discovery off) and sets SDL's iOS hints.
     - mac-iphonehmi
   * - ``hmi-cpp/cmake/sources.cmake``
     - The ``ksphmi_core`` / ``ksphmi_ui`` source lists, moved out of ``hmi-cpp/CMakeLists.txt`` so both
       projects build the same files. A pure refactor: no other platform changes.
     - shared (review: pc-claude / mac-claude)

``hmi-ios/`` lives outside ``hmi-cpp/`` on purpose: ``./ci deploy_hmi`` ships every ``hmi-cpp/`` folder to the
Pi and ``check_hmi_cpp_layout`` keeps that list complete, while nothing of the iOS app belongs on the Pi.

How the shared code runs on iOS
-------------------------------

``hmi-cpp/src/main.cpp`` is compiled unchanged. On iOS ``SDL.h`` renames ``main`` to ``SDL_main``;
SDL2main's ``main`` starts ``UIApplicationMain`` and calls it, and SDL pumps the UIKit run loop from
``SDL_PollEvent``, so the existing frame loop keeps its structure. Everything iOS-specific reaches the
shared code through the environment variables every platform already reads:

- ``KSPHMI_APP_DIR`` points ``find_app_dir()`` at the bundle's ``hmi/`` folder, which keeps the
  ``config.toml`` + ``assets/`` layout (fonts, skins, body art). The Pi's boot intro video and the asset
  READMEs are left out of the bundle; font licence files stay in.
- ``KSPHMI_LOCAL_CONFIG`` is ``Documents/config.local.toml``, regenerated at every launch from the
  Settings page. The bundle is read-only, and a generated file keeps one source of truth (Settings).
- SDL hints in their environment form: ``SDL_IOS_ORIENTATIONS`` (both orientations),
  ``SDL_IOS_IDLE_TIMER_DISABLED`` (the screen stays on in flight) and ``SDL_IOS_HIDE_HOME_INDICATOR``.

Touch already works through the mouse path: SDL synthesizes left-button events from touches, and
``Output::to_canvas`` maps window points to drawable pixels, the same mapping macOS Retina uses.

Planned shared changes
----------------------

Each item is a small change behind an iOS guard (``TARGET_OS_IOS``) or a neutral default elsewhere,
sent for review to pc-claude / mac-claude, with ``./ci test_hmi`` and the aarch64 cross build plus ctest
green. None is implemented yet.

1. **Background and foreground**: on ``SDL_APP_WILLENTERBACKGROUND`` stop presenting frames (iOS ends an
   app that renders in the background) and resume on ``SDL_APP_DIDENTERFOREGROUND``; sockets dropped while
   suspended go through the existing reconnect path.
2. **Safe area**: the notch / Dynamic Island and the home indicator must not cover instruments or buttons.
   SDL2 has no safe-area API: ``platform.mm`` reads ``UIWindow.safeAreaInsets`` and the display lays out
   inside the inset rectangle (zero insets on every other platform).
3. **Portrait layout**: ``flight_layout()`` was designed for landscape screens; a portrait iPhone
   (about 9:19.5) needs its own arrangement of the same panels, approved by the user from renderer
   captures, before it is implemented.
4. **Touch flight controls beyond the cockpit buttons**: a throttle slider and attitude pad for held
   axes, through KSPHMIFlight like the keyboard, after the first delivery.
5. **In-app scene and skin switching**: the keyboard shortcuts (Ctrl+1..5, Ctrl+"+"/"-") have no
   equivalent without a keyboard; a touch gesture or on-screen control is needed.

Unverified assumptions
----------------------

To check on the first Xcode build (each one recorded here as confirmed or corrected):

- The SDL2_ttf 2.24.0 release archive carries FreeType and HarfBuzz for ``SDL2TTF_VENDORED``.
- The target names ``SDL2::SDL2-static``, ``SDL2::SDL2main`` and ``SDL2_ttf::SDL2_ttf-static`` and the
  system frameworks the static SDL2 target brings with it.
- Where ``MACOSX_PACKAGE_LOCATION "Resources/..."`` lands in a flat iOS bundle (``platform.mm`` accepts
  ``hmi/`` at the bundle root or under ``Resources/``) and that ``Settings.bundle`` ends at the root.
- That every shared source compiles for iOS: the Linux-only paths (framebuffer, boot intro) are expected
  to fall back as on macOS, and ``config.cpp`` already uses ``_NSGetExecutablePath`` on Apple platforms.
- The SHA256 of both archives, pinned in ``hmi-ios/CMakeLists.txt`` after the approved download.

Testing plan
------------

1. iOS Simulator against ``./ci sim`` on the Mac (simulator address ``localhost``): connection, all
   flight scenes, cockpit touches arriving at kspsim, rotation, background/foreground.
2. Renderer captures at iPhone sizes in both orientations for the layout review: off-screen frames from
   the Mac build (``--screenshot`` with ``--size``) before any shared layout change.
3. The iPhone with free signing, against ``./ci sim`` and then the game, only for what the simulator
   cannot show (the local-network permission prompt, real touch, Wi-Fi reconnects).

Building
--------

Needs Xcode (not only the Command Line Tools) and the user's OK for the SDL download::

  cmake -S hmi-ios -B build/hmi-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DKSPHMI_IOS_FETCH=ON \
        -DKSPHMI_IOS_TEAM=<personal team id> -DKSPHMI_IOS_BUNDLE_ID=<unique bundle id>
  cmake --build build/hmi-ios --config Debug -- -sdk iphonesimulator

Without ``-DKSPHMI_IOS_FETCH=ON`` the configure step stops and names the download it needs.
