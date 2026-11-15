Raspberry Pi HMI
================

The Arduino panel keeps KSPSerialIO and its serial port for buttons, LEDs and potentiometers. A Raspberry Pi 4
with a monitor shows the flight data next to it: apoapsis, periapsis, altitude, speeds, orbital elements and a
drawing of the orbit. The code lives in ``hmi-cpp/`` (C++17, SDL2); the Pi builds it from source.

For module boundaries, threads, telemetry models and extension guidance, see
:doc:`development/raspberry-pi`. The complete TCP/UDP contracts are in :doc:`communication`.

Why kRPC and not KSPSerialIO
----------------------------

KSPSerialIO serves exactly one serial port (the first board that answers the handshake), and the Pi would have
to pretend to be a USB serial device. Instead the Pi talks over the LAN to the `kRPC <https://krpc.github.io/krpc/>`_
mod running inside KSP. kRPC gives far more than ``VesselData`` (any vessel, orbit, body or part value) and can
also send commands, so touch controls can be added later without touching the plugin or the firmware.

.. code-block:: text

   KSP (PC) --- KSPSerialIO --- USB serial (COM5) --- Arduino UNO R4: buttons, LEDs, LED matrix
            \-- kRPC server --- TCP 50000/50001 over the LAN --- Raspberry Pi 4: HDMI monitor

How it works
------------

``ksphmi`` separates source and display execution; a live kRPC client also owns a stream-reader thread.
The two application roles are:

* a **source** (``src/telemetry.cpp``) publishes immutable ``Snapshot`` objects. ``KrpcSource`` connects to
  kRPC, opens streams on the active vessel and re-opens them when the vessel or the sphere of influence changes;
  a stream transport failure ends the session even when cached values and the RPC channel remain available.
  It reconnects forever, so starting the Pi before KSP is fine. ``DemoSource`` flies an 80 x 250 km Kerbin
  orbit, so the screens can be worked on without the game.
* the **display** (``src/display.cpp``, SDL2 + SDL2_ttf) draws the latest snapshot at ``fps`` frames per second.
  The layout is in units of the screen height, so it fits the 2520x1680 monitor, a TV or a window on the PC.

The kRPC wire protocol (protocol version 1, the same as the krpc 0.7 clients) is encoded by hand in
``src/pb.cpp`` / ``src/krpc.cpp`` instead of libprotobuf: a few hundred lines, no ``protoc`` step, no library
version coupling. Shapes are batched into ``SDL_RenderGeometry`` calls, text comes from per-size glyph atlases
(FreeSansBold), the NavBall labels are pre-rendered once into a sprite atlas and drawn as foreshortened, rotated
quads with limb fading, and the NavBall lighting is one texture per size. A frame is redrawn only when a new
snapshot arrives (or the screen animates: waiting view, stale counter). The HMI replaced a Python/pygame version
in November 2026: on the Pi 4 it uses about 43 % of one core against 91 % for the Python one.

Between two 10 Hz samples only the NavBall moves (its attitude is smoothed at ``fps``). With the **layer cache**
(``[display] layer_cache``: ``auto`` = on with a GPU renderer, ``on``, ``off``; ``--layer-cache``) every new
snapshot, skin or screen is drawn into a full-screen render target, and the pixels under the globe square are
kept just before the globe is drawn. An in-between frame copies that layer, puts those pixels back and redraws,
clipped to the globe square, only what depends on the smoothed attitude: globe, lighting, labels, markers and
whatever is drawn over them (rim, boresight, the classic speed box). Readouts and lamps stay in the layer. Render
tests require the composed frame to equal a full redraw pixel for pixel in both skins. Without render targets
(``fbdev``, software) every frame is a full redraw, as before. The ``perf:`` log line (every 60 s) splits the
frames: ``full N at X ms, navball-only M at Y ms``, and gives the CPU time spent inside ``present`` next to its
wall time (a long present with little CPU is the wait for the page flip, not work).
The shipped ``config.toml`` sets ``layer_cache = "off"``: on the Pi GPU, copying the cached layer
cost more than a full redraw. ``auto`` describes an available selection mode, not the shipped setting.

The ten flight metrics occupy a compact two-column block in the upper left (less than 40% of screen height),
leaving resource gauges and a read-only staging queue below them. Each resource is a classic car fuel gauge:
a chrome bezel around a white face, a 110-degree arc across the upper half from E (left) to F (right) with
major ticks at E, 1/4, 1/2 (longest), 3/4 and F, a red mark below 10% and an amber mark below 25%, a red
needle on a charcoal hub in the lower centre, a dark-grey resource symbol (lightning bolt, fuel pump, O2
droplet, MP tank, solid booster, Xe), and an odometer-style window with the percentage (light digits, amber or
red when low). The resource label and the vessel-wide amount/capacity go under the dial, for ElectricCharge,
LiquidFuel, Oxidizer and MonoPropellant; SolidFuel and XenonGas appear when installed. Zero capacity means not
installed (dark-grey face, no needle, ``N/A``), an empty installed tank shows the needle at E and 0%, and
unavailable telemetry shows a light-grey face, no needle and ``--``. The static faces are drawn once per size,
state and resource (``src/gauges.cpp`` holds the pure geometry); each frame draws only the needle, the hub,
the window digits and the labels. Quantities use KSP resource units. The queue shows the current stage number
and up to four remaining activation stages with part counts and KSP's stage delta-v. Resource and stage streams
refresh at 2 Hz, and their collections are rebuilt when the vessel's parts or staging queue changes (including
docking/decoupling). These displays do not activate stages or transfer resources.

The flight layout places compact orbital data on the left, a smaller ORBIT diagram at the upper right,
and a stock-inspired NavBall below it. ``src/navball.cpp`` renders the blue/brown sphere and graduations
from kRPC's surface-frame attitude quaternion, including inverted and vertical flight. Fixed boresight,
heading, pitch/roll, Orbit/Surface/Target speed, SAS/RCS, throttle, G-force, all six orbital direction markers,
target/anti-target and maneuver markers follow the game's live telemetry. SAS mode buttons are read-only
status indicators. Zero-velocity frames and missing targets/nodes hide their undefined markers.
The maneuver readout includes remaining delta-v, countdown, progress and an estimated burn duration/start
using current mass, available thrust and specific impulse; this estimate assumes a constant engine configuration
and is not a staging-aware copy of KSP's burn prediction. Missing navigation telemetry leaves the orbital data
usable. On the launch pad kRPC refuses some values (the surface speed frame is singular, a stage may have no
delta-v): each optional value fails on its own and is retried, the attitude and the session stay up.
The simulator's manual fields include attitude, speed mode (0 orbit, 1 surface, 2 target), target vectors and
node/engine parameters.

Outside flight, dedicated preparation checklists are shown for Space Center, VAB, SPH, Mission Control,
Astronaut Complex, Research & Development, Administration and Tracking Station. These are contextual guides,
not live contract or crew lists. Flight data resumes automatically after launching or switching to a vessel.

KSPSerialIO 0.19.3.15 adds a read-only UDP context responder on port 50002. The Pi sends ``KSPHMI/1`` once per
non-flight sample; the response is ``KSPHMI/1 <scene>``. Facility spawn/despawn events distinguish buildings
that all use KSP's Space Center scene. Deploy both the plugin and HMI to enable these views, and allow UDP
50002 from the Pi through the PC firewall. No serial port is used. A missing responder, unknown scene or
blocked port falls back to the generic waiting screen; flight telemetry still uses kRPC unchanged.
The simulator serves the same context protocol and offers all buildings in its scene selector.

The header shows ``LIVE``, ``CONNECTED`` (outside flight), ``OFFLINE`` (no kRPC) or ``NO DATA``
(connected, but no new sample for 3 s: the game froze or the link stalled). A periapsis inside the atmosphere is
orange and one below the surface red.

ORBIT's white ring is the measured vessel position relative to the body's centre, projected into a fixed
inertial view plane. The source streams ``Vessel.position`` and ``Vessel.velocity`` in the body's
``non_rotating_reference_frame`` and fixes the view axes while following that vessel/body. The conic is
derived from the same position, velocity and gravitational parameter, so it rotates together with its apsides
without rotating the camera. This avoids apparent vessel jumps during RCS docking in near-circular orbits,
where true anomaly and the periapsis direction are ill-conditioned. Ap/Pe are hidden for effectively circular
orbits without a meaningful apsis direction. This is a 2D inertial projection, not a surface-longitude map;
switching vessels or bodies initializes a new view. The clip still contains unbounded escape trajectories. The
scale and offset fit the drawn extent (body, vessel, current and planned conics, with exact projected ellipse
bounds) to the plot area's full width and height; the axes never move. ORBIT is 27u tall (22u before), which
shrinks the NavBall globe by about 8 % (634 to 582 px at 2520x1680) and keeps it the largest element.

With a maneuver node, ORBIT also draws the orbit that results from it, as KSP's map view does: an orange
dashed line (dash and gap 1.2% of the panel height, constant on screen) with its own small orange Ap/Pe
markers and a diamond at the burn point; the current orbit stays solid. The source reads the first node's
``Node.orbit`` with ``Orbit.position_at`` / ``Orbit.velocity_at`` at the node's UT in the same
``non_rotating_reference_frame``, so the dashed conic shares the fixed view axes. Only the node's identity, UT
and prograde/normal/radial delta-v are streamed (2 Hz); the post-burn state is fetched again when they change,
and every 2 s while the node exists (KSP re-plans it from the current orbit, e.g. during the burn). The dashed
polyline is cached per node state and (0.5%-quantised) scale. A post-burn orbit that leaves the sphere of
influence or escapes is drawn from the node to the SOI boundary (``CelestialBody.sphere_of_influence``) or
towards the asymptote, inside the clip. The view widens to include the planned orbit, at most 20 times the
current one. A node placed after an SOI change (another body), or any kRPC error, hides only the dashed orbit
(retried after 1 s). On the first node of a session the log says ``maneuver orbit: periapsis ... agrees with
kRPC``, or warns ``frame mismatch?`` when the state vectors disagree with kRPC's own periapsis.
``./ci sim --node 250,900`` adds a 250 m/s prograde node 900 s ahead in the simulator; ``--demo`` has one.

On the Pi there is no desktop. The current service sets ``KSPHMI_OUTPUT=auto`` and
``SDL_VIDEODRIVER=kmsdrm``: it first tries an accelerated SDL renderer and falls back to Linux fbdev when
that path cannot open. The GPU path needs the working Mesa EGL/GLES stack and DRM access. The old
pygame attempt left the screen black; that historical result does not describe the current C++ GPU path.

With fbdev selected, SDL draws into an ARGB8888 software surface and only changed rows are copied to
``/dev/fb0`` (RGB565 or XRGB8888). This fallback needs neither EGL nor DRM master. Explicit
``KSPHMI_OUTPUT=fbdev`` forces it; ``KSPHMI_OUTPUT=sdl SDL_VIDEODRIVER=kmsdrm`` selects SDL output.
The framebuffer can be left *blanked* (``/sys/class/graphics/fb0/blank`` = 4, powered down; it happened after
the kmsdrm attempts): frames are then written but nothing shows, while the HMI log looks healthy. The HMI
unblanks it (``FBIOBLANK`` ioctl, no root needed) at start and every 30 s; ``./ci verify`` checks the value.
The console underneath (fbcon, tty1) still blinks its cursor into the framebuffer between frames; only
root can turn that off, hence the tmpfiles rule below. With blinking off the cursor is never redrawn and the
next frame covers it. The waiting screens show this display's own address (the one it uses to reach the
kRPC server first) and the server it looks for, to find the Pi without a keyboard.

The service log is read with ``journalctl _SYSTEMD_USER_UNIT=ksphmi.service`` (``./ci hmi_logs``): Raspberry
Pi OS keeps no per-user journal, so ``journalctl --user`` finds nothing.

Settings
--------

``hmi-cpp/config.toml`` (deployed to ``~/ksphmi/cpp``) holds the defaults: kRPC ports and sample rate, ``fps``,
screen size (0 = the monitor's own) and ``day_hours`` (6 for the KSP calendar). The binary finds it through
``KSPHMI_APP_DIR`` (the unit sets ``~/ksphmi/cpp``), else next to or one level above itself, so a build tree
under ``build/hmi-cpp`` uses the repo's file. On the Pi, ``~/ksphmi/config.local.toml``
(``KSPHMI_LOCAL_CONFIG``) overrides any key and is never touched by a deploy; ``~/ksphmi/env`` holds
``KSPHMI_KRPC_ADDRESS``, which ``./ci deploy_hmi`` sets to the PC's address as the Pi sees it (or to
``HMI_KRPC_ADDRESS`` if set). Options: ``--demo --address --windowed --size WxH --screenshot FILE --seconds S
--config FILE --fps N --bench N [--bench-anim] --offscreen --skin NAME --layer-cache auto|on|off``
(``ksphmi --help``). ``--bench N --bench-anim`` keeps one sample and turns only the attitude, so it times
NavBall-only frames (full frames when the cache is off).

``[display] skin`` picks the look: ``apollo`` (the default since 2026-11-14) or ``classic`` (the previous flat
dark panels, kept for switching back). Apollo draws the same zones as a 1960s spacecraft panel: grey panel with painted labels and group lines,
seven-segment readouts (``segments.cpp``), caution and warning lamps (UPLINK, NO DATA, NO LINK, SAS, RCS,
PE ATMO, PE SURF, LOW PWR, LOW FUEL, ESCAPE), edgewise resource meters and bezels around ORBIT and the
NavBall (round metal bezel, segment readouts for speed, heading, pitch, roll, throttle, G and the maneuver
node, SAS modes as lamp tiles). Its static art is one texture per screen size and screen kind; per frame only
lit segments, lamps, pointers and the two plots are drawn. Try it with ``./ci hmi --demo --skin apollo``.

The skin can change while the HMI runs: the context reply on UDP 50002 may carry an optional field,
``KSPHMI/1 <scene> skin=<name>``. Only the simulator sends it (its *HMI skin* selector, or ``--skin``);
KSPSerialIO never does, so with the game ``[display] skin`` decides. The HMI asks for the context about once a
second in every state, flight included (telemetry thread, 0.15 s timeout). Known skin in the reply > the
setting; each skin keeps its caches, built the first time it is shown. Changes are logged
(``ksphmi.display: skin: apollo (context, was classic)``). The default skin is the one line ``skin = ...`` in
``hmi-cpp/config.toml``.

The Pi initiates all connections, so a DHCP change of its own address does not affect the display.
If the configured PC address stops working, the HMI broadcasts ``KSPHMI/1`` on UDP 50002 to the local
IPv4 network and retries kRPC at the responding KSPSerialIO server; only private, link-local or loopback
peers are accepted. This reuses the scene responder;
no additional plugin or Pi address registry is needed. Both machines must be on the same broadcast network,
with UDP 50002 and kRPC TCP 50000/50001 allowed. With multiple KSP PCs the first responder is selected.
If broadcast is blocked, the configured address remains the fallback; use a DHCP reservation for that case.
Set ``krpc.discovery = false`` in the local configuration, or use ``--address``, to pin a specific server.
The SSH alias is separate: use a DHCP reservation or a resolvable hostname to keep administrative access stable.

One-time setup
--------------

On the Pi (Raspberry Pi OS Lite 64-bit, bookworm, wired to the same LAN), as your user:

.. code-block:: bash

   # compiler, CMake and the SDL2 / SDL2_ttf development packages, plus the font the HMI draws with
   sudo apt install -y build-essential cmake pkg-config libsdl2-dev libsdl2-ttf-dev fonts-freefont-ttf
   sudo loginctl enable-linger $USER        # start the user service at boot, nobody logged in
   # stop the text console's cursor blinking over the HMI frames (now and at every boot)
   echo 'w /sys/class/graphics/fbcon/cursor_blink - - - - 0' | sudo tee /etc/tmpfiles.d/ksphmi-cursor.conf
   sudo systemd-tmpfiles --create /etc/tmpfiles.d/ksphmi-cursor.conf

After deploying the HMI, enable the kiosk boot once on the Pi (``--undo`` reverts all of it except lingering):

.. code-block:: bash

   sudo sh ~/ksphmi/cpp/systemd/setup-kiosk.sh && sudo reboot

The script is idempotent and keeps SSH, the network and the serial console. It:

* edits ``cmdline.txt`` (backup ``cmdline.txt.ksphmi-backup``, written once): removes ``console=tty1`` and puts
  ``console=ttynull`` first, so kernel, systemd and cloud-init text never reach the screen (``serial0``, if
  enabled, stays the last console and so ``/dev/console``); adds ``quiet loglevel=3 logo.nologo
  vt.global_cursor_default=0 consoleblank=0 systemd.show_status=false rd.systemd.show_status=false
  plymouth.enable=0``. Everything else (``root=``, ``ds=nocloud``, the Wi-Fi country) is kept;
* adds ``disable_splash=1`` (no rainbow) and ``avoid_warnings=1`` (no under-voltage icon over the HMI) to
  ``config.txt`` (backup ``config.txt.ksphmi-backup``);
* writes the fbcon ``cursor_blink`` tmpfiles rule and masks ``getty@tty1`` (no login prompt);
* installs the udev rule ``/etc/udev/rules.d/99-ksphmi-fb.rules`` (makes ``dev-fb0.device`` visible to systemd)
  and the **system** unit ``ksphmi-splash.service`` (from ``hmi-cpp/systemd/``, with the user and home filled
  in): ``Type=oneshot``, ``DefaultDependencies=no``, ``After=dev-fb0.device``, ``WantedBy=sysinit.target``,
  ``Before=systemd-user-sessions.service`` (capped by ``JobTimeoutSec=20``), running
  ``~/ksphmi/cpp/build/ksphmi --splash`` as the HMI user in group ``video``;
* enables lingering and warns when ``ksphmi.service`` is not enabled yet (``./ci deploy_hmi`` does that).

What the screen shows at boot: black while the firmware and kernel start (no rainbow, no logo, no text), then the
loading screen ("KSP HMI", "Starting...") as soon as ``/dev/fb0`` exists, held by the framebuffer; then the user
unit ``ksphmi.service`` (lingering, ``WantedBy=default.target``, ``Restart=always``, ``StartLimitIntervalSec=0``)
paints its waiting screen over it and connects when KSP and kRPC are up. ``ksphmi --splash`` reads no settings
and touches no network; it re-applies the video mode (``FBIOPUT_VSCREENINFO``) because with a silent console
nothing else may have set it. Without the font it still fills the screen with the HMI background. Review the
picture with ``./ci hmi --splash --screenshot build/splash.png --size 2520x1680``. The HMI opens ``/dev/fb0``
without clearing it and rewrites every row at each unblank (start, then every 30 s), so neither the splash nor
console text can survive behind its frames. ``./ci verify`` reports each missing piece in ``hmi: kiosk boot``.
Do not cut power to shut down Linux; use ``sudo poweroff`` first to avoid filesystem damage.

On the PC: an SSH key copied to the Pi and a ``Host ksphmi`` entry in ``~/.ssh/config`` (``HostName`` = the Pi's
address, ``User``, ``IdentityFile``). The repo never stores the address or the user name; ``HMI_HOST`` overrides
the alias. A DHCP reservation on the router keeps the Pi's address fixed.

In KSP (with KSP closed): kRPC **0.7.0** (built for KSP 1.12.5, protocol version 1, as the HMI's client and
``krpc==0.7.0`` in ``tools/requirements.txt`` for kspsim; the 0.5.x mods speak protocol 0) from
`github.com/krpc/krpc/releases <https://github.com/krpc/krpc/releases>`_, ``GameData/kRPC`` copied as is. The
shipped ``PluginData/settings.cfg`` is empty (kRPC writes defaults on first start, listening on ``127.0.0.1``
only), so it is replaced by copying the repo's complete ``tools/krpc/settings.cfg`` over it:

.. code-block:: bash

   cp tools/krpc/settings.cfg "$KSPDIR/GameData/kRPC/PluginData/settings.cfg"

The template holds every key kRPC 0.7.0 writes, in its own format, with kRPC's defaults plus our choices:
``autoStartServers = True``, ``autoAcceptConnections = True``, ``confirmRemoveClient = False`` and one server
``KSPKontroller HMI`` (``ProtocolBuffersOverTCP``, ``address = 0.0.0.0``, ``rpc_port = 50000``,
``stream_port = 50001``).

.. warning::

   Never hand-write a partial file. kRPC's ``ConfigurationFile.AfterLoad`` copies **every** persistent field, so
   a missing key becomes ``0``/``False`` instead of its default. With ``maxTimePerUpdate`` missing (0) the server
   accepts the Pi's connection but never answers a single RPC: the HMI hung for a whole game session this way.
   The real defaults (``core/src/Configuration.cs``) are ``maxTimePerUpdate = 5000``, ``recvTimeout = 1000``,
   ``tickHoldTimeout = 1000000``, ``adaptiveRateControl = True``, ``blockingRecv = True``,
   ``oneRPCPerUpdate = False``, ``verboseErrors = True``, ``pauseServerWithGame = False``.

The wired LAN is a **Public** network in Windows, and the KSP install
has no firewall rule of its own, so an administrator PowerShell must allow the ports once:

.. code-block:: powershell

   New-NetFirewallRule -DisplayName "KSP kRPC (wk-rea)" -Direction Inbound -Action Allow -Protocol TCP `
     -LocalPort 50000,50001 -RemoteAddress LocalSubnet `
     -Program "C:\Users\alanl\wk\wk-rea\Kerbal Space Program\KSP_x64.exe"

``./ci verify`` (``check_krpc``) checks the mod version against ``tools/requirements.txt``, the server settings
(auto-start, auto-accept, not localhost), fails if ``maxTimePerUpdate`` or ``recvTimeout`` is missing or 0, warns
if ``adaptiveRateControl``/``blockingRecv`` are False or if any key differs from ``tools/krpc/settings.cfg``
(window positions ignored), and checks the firewall rule. ``tools/kspsim`` must not run while KSP does: both want
port 50000 (and COM5).

Deploy
------

``./ci deploy_hmi`` (KSP may be running; it only talks to the Pi over SSH):

#. checks the apt packages above and lingering on the Pi (prints the one-time ``sudo`` command if missing) and
   warns when the console cursor rule is missing;
#. copies ``hmi-cpp/`` (``CMakeLists.txt``, ``config.toml``, ``src``, ``tests``, ``systemd``) to ``~/ksphmi/cpp``,
   replacing everything there except ``build/`` (kept so CMake rebuilds incrementally);
#. on the Pi: ``cmake -S . -B build -DCMAKE_BUILD_TYPE=Release``, ``cmake --build build -j4`` and ``ctest`` in
   ``build``. A failure stops here and the running service keeps its old binary;
#. writes ``~/ksphmi/env``, disables and removes a leftover ``ksphmi-cpp.service`` (the trial unit) and the old
   Python ``~/ksphmi/app`` and ``~/ksphmi/venv``, installs ``hmi-cpp/systemd/ksphmi.service`` into
   ``~/.config/systemd/user``, enables and restarts it;
#. waits 8 s, requires ``active`` with ``NRestarts=0`` (``Restart=always`` hides a crash loop behind
   ``active``) and prints the last 15 log lines.

Interactive HMI and test app without the Pi
-------------------------------------------

Run both application windows on a Linux desktop, viewed in your browser::

   ./ci hmi_desktop

Open ``http://localhost:6080/vnc.html?autoconnect=true&resize=scale``. Wait for the initial build;
``docker logs -f ksphmi-desktop`` shows startup progress. The left window is the real kspsim test app;
the right window is the actual C++ HMI, with an SDL window on a virtual X11 screen.
The browser displays the desktop through noVNC, rather than reimplementing either app.

Use the test app's scenario buttons, scene selector, HMI skin selector, throttle, resources and manual
fields as usual. Both apps share container loopback: kRPC TCP 50300/50301, scene/skin context UDP 50002.
Discovery is disabled so the HMI stays connected to this simulator. No Arduino or real KSP is needed.
Only the browser port is published, bound to the host's loopback; no kRPC or VNC port is exposed.

The desktop is 3000x1800, scaled to the browser viewport. Use browser fullscreen or noVNC scaling options
for small labels. Move or resize either window with its title bar. Closing either app ends the container.
Logs are under ``build/hmi-container/<platform>/desktop``. Stop and restart after editing code::

   docker stop ksphmi-desktop
   ./ci hmi_desktop

If the desktop is already running, the command prints its URL and keeps the current session.
A new session copies the current source and rebuilds it. This is not hot reload. The desktop and automated
tests share a build directory: stop the desktop before running tests for the same architecture.
The container is removed when stopped; build outputs and logs remain on the host.

``./ci test_hmi_desktop`` exercises the actual Tk skin and throttle controls with both windows running,
checks that the C++ display selects the skin received over UDP and reads the changed throttle over kRPC.
It also checks the noVNC HTTP/WebSocket path to VNC. It runs without publishing browser ports, exits
automatically, and saves ``desktop/smoke.json``.
``tools/verify.py`` reports this result and whether it predates the relevant sources.

Testing without a Raspberry Pi (Docker)
---------------------------------------

Install and start Docker with Linux containers, then run from the repository root::

   ./ci test_hmi_container

This builds a Debian Bookworm environment (the Pi's Debian generation), compiles the actual C++ HMI,
runs all ctest suites and connects the executable to kspsim over loopback. SDL uses its dummy video
backend and software rendering: no display, Pi, Arduino or KSP is required. The source mount is read-only;
the test container has no external network. Only the image build downloads dependencies.

The default platform is ``linux/arm64``. On an ARM64 Mac this avoids CPU emulation; on an x86 PC Docker
needs ARM emulation support (included in Docker Desktop). For faster x86 development, or a 32-bit target::

   HMI_PLATFORM=linux/amd64 ./ci test_hmi_container
   HMI_PLATFORM=linux/arm/v7 ./ci test_hmi_container

Choose the architecture matching the Pi OS when checking ARM compatibility. Builds have separate caches
under ``build/hmi-container/linux-arm64`` (or ``linux-amd64`` / ``linux-arm-v7``). Inspect
``hmi-cpp-sim/flight.png``, ``hmi.log`` and ``kspsim.log`` there; ctest details are under
``hmi-cpp/Testing/Temporary``. A failing build or test makes the command fail.

This checks C++ logic, rendering and the real client wire protocol, including an ARM build when selected.
It does not reproduce the Pi kernel, firmware, systemd boot, physical framebuffer, display blanking or
hardware performance. Test those on the Pi when available. Container timings are not Pi performance
measurements. A full Raspberry Pi OS VM is an optional system integration environment, rather than a
requirement for everyday HMI development. Debian package and transitive Python versions are resolved at
image build time; this environment is repeatable tooling, not a bit-for-bit dependency lock.

Building on macOS
-----------------

With CMake, a C++17 compiler, pkg-config, SDL2 and SDL2_ttf installed, ``./ci hmi`` runs a native Mac
window. ``./ci hmi --demo`` displays simulated data without KSP. Run ``./ci test_hmi`` for native tests;
``./ci hmi --demo --screenshot build/hmi-macos-demo.png --size 1280x800`` saves an off-screen frame.
Native rendering falls back to macOS Arial Bold when the Linux fonts are absent; ``KSPHMI_FONT`` can
select the same font used on the Pi for visual comparisons. Executable-relative configuration lookup
uses the macOS API. Linux fbdev is unavailable on macOS and gives an explicit error; SDL and off-screen
output work. For the simulator and HMI together in the browser, use ``./ci hmi_desktop``.

Building on this PC
-------------------

Windows has no C++ toolchain here: ``./ci build_hmi``, ``./ci hmi`` and ``./ci test_hmi`` re-run themselves inside
WSL. Without root there, the SDL2 packages are unpacked into ``build/hmi-cpp-deps`` with ``apt-get download``
(Ubuntu 24.04 names). ``./ci test_hmi --sim`` also starts ``tools/kspsim --no-board --headless`` on
127.0.0.1:50300 and checks that the HMI follows the simulated vessel and saves a frame to
``build/hmi-cpp-sim/flight.png`` (needs ``python3`` with ``krpc`` and ``protobuf``; set ``KSPSIM_PYTHON`` to
choose the interpreter).

Tests (``hmi-cpp/tests``, ctest): ``core`` (formatting, Kepler solver, demo source, attitude quaternions and
burn time, label projection and limb fade, front-hemisphere clipping, inertial orbit geometry (RCS cannot move
the vessel ring), resource fractions, gauge geometry and layout, scene replies, discovery address filter and
broadcast, settings layers, protobuf values, the kRPC client against an in-process server, the handshake timeout,
reconnect discovery and the launch-pad errors), ``render`` (off-screen layout checks: ten metric tiles below 40 %
of the height, text inside its tile, dials inside the resources panel, ORBIT and NavBall never drawing outside
their panels, apsis labels, facility views, the boot splash, the software frame time) and ``kiosk`` (the boot
file edit of ``setup-kiosk.sh`` and the unit files, when ``python3`` is found).

Measured: about 9 ms per 2520x1680 flight frame on the PC (x86-64, WSL, software renderer).
``ksphmi --bench 20 --offscreen --size 2520x1680`` prints ``HMI_RENDER_MS`` without touching the screen;
``./ci verify`` runs it on the Pi (``hmi: Pi render budget``).

Commands
--------

==========================  ===========================================================================
``./ci hmi --demo``         Build the HMI and run it on the PC in a window with the simulated orbit
``./ci hmi``                Same against KSP on this PC (``--address`` for another one)
``./ci build_hmi``          Build into ``build/hmi-cpp`` (on Windows inside WSL)
``./ci test_hmi [--sim]``   Build and run ctest; ``--sim`` adds the run against ``tools/kspsim``
``./ci deploy_hmi``         Copy ``hmi-cpp`` to ``~/ksphmi/cpp``, build and test it on the Pi, install and
                            restart the ``ksphmi`` user service, print its log
``./ci hmi_logs [N]``       Last N lines of the service log on the Pi
``./ci verify``             ``hmi:`` lines: views and resources, local test build current, service state and
                            unit, kRPC address still this PC, screen, cursor, kiosk, Pi render budget
==========================  ===========================================================================

``build_hmi_cpp`` and ``test_hmi_cpp`` are old names of ``build_hmi`` and ``test_hmi`` and still work.
``ksphmi --demo --size 2520x1680 --screenshot out.png`` renders a frame without any window, which is how
screen changes are reviewed (inside WSL, with the environment ``./ci`` sets up: ``hmi_env`` in ``ci``).
