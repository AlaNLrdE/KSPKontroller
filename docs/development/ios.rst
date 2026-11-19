iPhone HMI development
======================

Status: **runs in the iOS 27.2 Simulator against kspsim** (mac-iphonehmi, 2026-11-18). The design below was written
before Xcode was installed; "Build assumptions checked" records what the first build confirmed.

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
     - Bundle keys: the one-scene manifest iOS 27 requires, orientations, hidden status bar, local-network usage text, Files app access to
       ``Documents/``.
     - mac-iphonehmi
   * - ``hmi-ios/Settings.bundle/``
     - Settings app page: PC address, game (kRPC, Redux, automatic, demo), skin, first flight scene.
     - mac-iphonehmi
   * - ``hmi-ios/src/platform.mm``
     - Start-up glue (static constructor before SDL's ``UIApplicationMain``): sets ``KSPHMI_APP_DIR``,
       writes ``Documents/config.local.toml`` from the settings (discovery off) and sets SDL's iOS hints;
       ``KSPHMISceneDelegate`` gives SDL's window its ``UIWindowScene``.
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

UIScene lifecycle (iOS 27)
--------------------------

iOS 27 requires the UIScene lifecycle: the first simulator run (iOS 27.2, 2026-11-18) died about one second
after launch with ``EXC_BREAKPOINT`` in ``_UIApplicationEvaluateRuntimeIssueForNoSceneLifecycleAdoption``,
reached from ``-[UIApplication workspace:didCreateScene:...]``. SDL 2.32 only implements the app-delegate
lifecycle, so the fix lives in ``hmi-ios/`` without patching SDL:

- ``Info.plist.in`` declares ``UIApplicationSceneManifest``: one scene (``UIApplicationSupportsMultipleScenes``
  false), configuration ``Default`` for ``UIWindowSceneSessionRoleApplication`` with
  ``UISceneDelegateClassName`` ``KSPHMISceneDelegate``.
- ``platform.mm`` defines ``KSPHMISceneDelegate`` (a global Objective-C class, outside the C++ anonymous
  namespace). Under scenes a ``UIWindow`` without a ``windowScene`` is never shown, and SDL creates its window
  with ``-[SDL_uikitwindow initWithFrame:]`` from ``SDL_main`` (started by ``postFinishLaunch`` after
  ``didFinishLaunching``), so both orders are handled: ``scene:willConnectToSession:`` remembers the scene and
  attaches a window SDL already created (SDL's app delegate answers ``-window``), and for later windows an
  ``-initWithFrame:`` override is added to ``SDL_uikitwindow`` only (``class_addMethod``: SDL's class does not
  override it, so UIKit's own windows are untouched). It calls the inherited initializer, then sets the scene.
  No notification fires between SDL's window creation and its ``makeKeyAndVisible``, so an observer cannot do this.
- Background/foreground: SDL listens to the ``UIApplication`` notifications (``DidBecomeActive``,
  ``WillResignActive``, ``DidEnterBackground``, ``WillEnterForeground``), not the app-delegate methods, and
  UIKit still posts those for a single-scene app, so the scene delegate forwards nothing (forwarding would
  send every ``SDL_APP_*`` event twice). Rotation still arrives as ``SDL_WINDOWEVENT_SIZE_CHANGED`` from SDL's
  view controller.

Checked on the iOS 27.2 simulator (iPhone 18 Pro, 2026-11-18): the app stays alive, draws the cockpit panel at
1206x2622 and connects to kspsim on ``127.0.0.1`` (live orbit data, CONNECTED lamp). The scene connected
*after* SDL had created its window (log ``KSP HMI: attached the existing SDL window to the scene``).
Backgrounding (launching Settings) and returning logged ``app entering background: drawing paused`` /
``app back in foreground: drawing resumed``, so the UIApplication notifications still reach SDL. Rotation was
not tested (simctl has no rotate command). Harmless console notes: "You need
UIApplicationSupportsIndirectInputEvents in your Info.plist for mouse support" and "Unbalanced calls to
begin/end appearance transitions for SDL_uikitviewcontroller" right after the scene attach.

Planned shared changes
----------------------

Each item is a small change behind an iOS guard (``TARGET_OS_IOS``) or a neutral default elsewhere,
sent for review to pc-claude / mac-claude, with ``./ci test_hmi`` and the aarch64 cross build plus ctest
green. Items 1 and 2 are implemented; the others are not yet.

1. **Background and foreground** (implemented, ``hmi-cpp/src/main.cpp`` event loop, no ``#ifdef``: desktop
   platforms never send these events): on ``SDL_APP_WILLENTERBACKGROUND`` or ``SDL_APP_DIDENTERBACKGROUND``
   the loop releases held keys and cockpit buttons, then stops drawing and presenting (iOS ends an app that
   renders in the background); it keeps pumping events every 100 ms. ``SDL_APP_DIDENTERFOREGROUND`` resumes
   with a size check and a full redraw. Each transition logs one info line (``app entering background:
   drawing paused`` / ``app back in foreground: drawing resumed``). The telemetry source keeps its own
   thread: sockets dropped while suspended go through the existing reconnect path. Not yet run on the
   simulator or a device.
2. **Safe area** (implemented, ``hmi-cpp/include/safearea.hpp`` + ``Output`` in ``hmi-cpp/src/main.cpp``):
   the notch / Dynamic Island and the home indicator must not cover instruments or buttons. SDL2 has no
   safe-area API: ``platform.mm`` defines ``ksphmi_platform_safe_insets()``, which reads the SDL window's
   ``safeAreaInsets`` at call time and returns drawable pixels (points times the screen's ``nativeScale``,
   rounded up). Every other platform gets an inline default returning zeros, with no new source or link
   change. ``Output`` gives Canvas and Display the drawable minus the insets, sets the renderer viewport at
   the insets' offset, fills the margins black before each present and subtracts the offset from touch
   positions. The insets are read again at start, after every size change, on foreground resume and once
   per frame (a turn between the two landscape orientations moves the island without a size change). Zero
   insets never touch the viewport, so other platforms behave as before. Log line on every change:
   ``safe area insets left L top T right R bottom B (drawable WxH)``. Layout code is unchanged: the
   portrait arrangement is item 3.
3. **Portrait layout**: ``flight_layout()`` was designed for landscape screens; a portrait iPhone
   (about 9:19.5) needs its own arrangement of the same panels, approved by the user from renderer
   captures, before it is implemented.
4. **Touch flight controls beyond the cockpit buttons**: a throttle slider and attitude pad for held
   axes, through KSPHMIFlight like the keyboard, after the first delivery.
5. **In-app scene and skin switching**: the keyboard shortcuts (Ctrl+1..5, Ctrl+"+"/"-") have no
   equivalent without a keyboard; a touch gesture or on-screen control is needed.

Build assumptions checked
-------------------------

First iOS Simulator build (Xcode 27.2, CMake 4.3.2, arm64, unsigned, 2026-11-18): ``BUILD SUCCEEDED``.

- Confirmed: the SDL2_ttf 2.24.0 release archive carries FreeType and HarfBuzz under ``external/``; the vendored
  FreeType still declares an old ``cmake_minimum_required``, which CMake 4 refuses, so ``hmi-ios`` sets
  ``CMAKE_POLICY_VERSION_MINIMUM`` 3.5 around ``FetchContent_MakeAvailable`` only.
- Confirmed: the targets ``SDL2::SDL2-static``, ``SDL2::SDL2main`` and ``SDL2_ttf::SDL2_ttf-static`` link with
  the frameworks the static SDL2 target brings; no extra framework list was needed.
- Confirmed: every shared source compiles for iOS unchanged; the only warnings from our code are the two the
  macOS build already shows (``canvas.hpp`` unused ``renderer_``, ``intro.cpp`` unused ``LOG``).
- Confirmed: ``MACOSX_PACKAGE_LOCATION "Resources/..."`` lands at the root of the flat iOS bundle
  (``KSPHMI.app/hmi/`` and ``KSPHMI.app/Settings.bundle``).
- Pinned: SHA256 of both archives in ``hmi-ios/CMakeLists.txt``. SDL2_ttf's matches Homebrew's formula; SDL2's
  was taken over HTTPS from the official release (its GPG signature was not checked: no gpg on the Mac).
- Note: the Debug bundle is 126 MB, mostly body art and skin bitmaps; trimming them is a later item.
- Not yet run: the app itself (the simulator runtime was still downloading) and the device build (signing).

Testing plan
------------

1. iOS Simulator against ``./ci sim`` on the Mac (simulator address ``localhost``): connection, all
   flight scenes, cockpit touches arriving at kspsim, rotation, background/foreground.
   ``./ci ios_sim [--device NAME] [--seconds N] [-- KSPSIM_ARGS]`` does the whole round: ``ios_build``,
   kspsim on ``127.0.0.1`` only (``--no-board --headless``, 1800 s by default), boots the device (default
   "iPhone 18 Pro") and waits for ``bootstatus``, installs the app, writes ``pc_host 127.0.0.1``,
   ``source krpc`` and ``flight_scene cockpit`` into the app's preferences plist in its data container
   (``simctl spawn ... defaults write`` hangs), launches it and after 15 s saves
   ``build/hmi-ios-sim/screen.png``; the app's stdout/stderr and the kspsim log are in the same folder.
   Every ``simctl`` call has a timeout and a timeout stops with a message; it warns when the 1-minute
   load average is above 15. kspsim stops when the target exits. It only uses its own device, so the
   mailbox lock ``ios-simulator`` (for actions affecting all simulators) is not needed.
2. Renderer captures at iPhone sizes in both orientations for the layout review: off-screen frames from
   the Mac build (``--screenshot`` with ``--size``) before any shared layout change.
3. The iPhone with free signing, against ``./ci sim`` and then the game, only for what the simulator
   cannot show (the local-network permission prompt, real touch, Wi-Fi reconnects).

Building
--------

Needs Xcode (not only the Command Line Tools) and the user's OK for the SDL download. On the Mac the
active developer directory was still the Command Line Tools after installing Xcode 27.2 (2026-11-18):
``xcrun --sdk iphoneos`` then fails and CMake reports "iphoneos is not an iOS SDK". Without sudo, export
``DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer`` for each command (or the user runs
``sudo xcode-select -s /Applications/Xcode.app``). The iOS Simulator also needs its runtime, a separate
download (``xcodebuild -downloadPlatform iOS``) that needs the user's OK as well::

  cmake -S hmi-ios -B build/hmi-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DKSPHMI_IOS_FETCH=ON \
        -DKSPHMI_IOS_TEAM=<personal team id> -DKSPHMI_IOS_BUNDLE_ID=<unique bundle id>
  cmake --build build/hmi-ios --config Debug -- -sdk iphonesimulator

Without ``-DKSPHMI_IOS_FETCH=ON`` the configure step stops and names the download it needs.

``./ci ios_build`` runs the simulator build on the Mac: it exports ``DEVELOPER_DIR`` for Xcode when
``xcode-select`` still names the Command Line Tools, configures ``build/hmi-ios`` once (with
``-DKSPHMI_IOS_FETCH=ON``), builds ``ksphmi_ios`` for ``-sdk iphonesimulator -arch arm64`` without
signing (full log in ``build/hmi-ios/build.log``), signs ``KSPHMI.app`` ad hoc and prints its path.
Off macOS both iOS targets stop with a message.
