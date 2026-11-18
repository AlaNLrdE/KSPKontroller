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
  a stream transport failure or three seconds without incoming stream results ends the session, even when
  cached values and the RPC channel remain available. Identical values arriving normally count as activity;
  repeatedly reading the cache does not refresh the channel's receive timestamp.
  It reconnects forever, so starting the Pi before KSP is fine. ``DemoSource`` flies an 80 x 250 km Kerbin
  orbit, so the screens can be worked on without the game.
* the **display** (``src/display.cpp``, SDL2 + SDL2_ttf) draws the latest snapshot at ``fps`` frames per second.
  The layout is in units of the screen height, so it fits the 2520x1680 monitor, a TV, a Mac or a PC screen.
  The HMI is always full screen at the display's native pixel resolution, read from SDL at start-up on every
  platform (no size settings; HiDPI pixels on macOS). When the window or display changes size while it
  runs, the canvas and every size-dependent cache are rebuilt for the new size. ``--size`` only sizes
  off-screen frames (``--screenshot``, ``--bench``, ``--offscreen``) and ``--windowed`` exists only for the
  virtual-desktop test harnesses (``tools/hmi-container``).
* **keyboard** (``src/keyboard.cpp``): the HMI's keyboard is a remote KSP keyboard with the game's default
  flight bindings, on any platform (Pi, PC, Mac, Linux). See :ref:`hmi-keyboard`.

.. _hmi-keyboard:

Remote keyboard
---------------

Keys follow KSP's default flight bindings (read from ``GameSettings`` in ``Assembly-CSharp.dll``), letters and
digits by the character on the user's layout, ``, . /`` also by physical position:

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Key
     - Action (flight unless noted)
   * - ``T`` / ``R``
     - SAS / RCS toggle (kRPC ``Control``). In the VAB: ``KSPHMI.Press``, the board's editor navigation (SAS next
       screen, RCS previous)
   * - ``F`` (held)
     - SAS inverted while held (toggle on press and release)
   * - ``Z`` / ``X``
     - throttle full / cut (``KSPHMIFlight.SetThrottle``)
   * - Left Shift / Left Ctrl (held)
     - throttle up / down at KSP's rate, 50 % per second (``KSPHMIFlight.NudgeThrottle``); Ctrl starts after
       0.25 s and never for a Ctrl+digit chord
   * - ``W S`` / ``A D`` / ``Q E`` (held)
     - pitch / yaw / roll
   * - ``H N`` / ``I K`` / ``J L`` (held)
     - translate forward-back / down-up / left-right
   * - Space, Backspace
     - stage (``Control.ActivateNextStage``), abort
   * - ``G`` / ``U`` / ``B`` (held)
     - gear toggle / lights toggle / brakes while held
   * - ``1`` ... ``0``
     - custom action groups 1 ... 10
   * - Alt (Option) + ``1`` ... ``0``
     - SAS mode in the HMI's SAS MODE order: HOLD, PRO, RET, N+, N-, R+, R-, TGT, ANTI, NODE
   * - ``,`` / ``.`` / ``/``
     - time warp down / up / to 1x: physics warp while it is active or rails warp is not allowed, rails
       otherwise (kRPC's rails setter also cuts the throttle, so physics warp is stopped through its own factor)
   * - ``M``
     - map view toggle (kRPC ``Camera.Mode``: ``Map`` enters it, any other mode only leaves it, keeping the
       flight camera)
   * - F5 / F9 (held 1 s)
     - quicksave / quickload
   * - Ctrl+1 / Ctrl+2 / Ctrl+3 / Ctrl+4 / Ctrl+5
     - classic / apollo / steampunk / ksp1 / ksp2 skin until the HMI exits, over the context reply and ``[display] skin``
   * - Ctrl+0
     - VESSEL panel: fuel or heat (``--vessel-heat`` starts on heat)
   * - Ctrl+"+" / Ctrl+"-"
     - next / previous flight scene: instruments, the clickable :ref:`cockpit panel <hmi-cockpit>` or the
       :ref:`MechJeb scene <hmi-mechjeb>`. The "+" of a Spanish layout, "=" of a US one and the keypad +/- all work;
       the chord never ramps the throttle
   * - 0-9, ``.``, ``-``, Backspace, Enter, Esc
     - while the MechJeb scene's numeric keypad is open: type into it, delete, SET, CANCEL (Esc then only closes the
       keypad and never quits the HMI)
   * - Esc
     - quit the HMI (``q`` is roll left; on macOS Cmd+Q also quits)

Auto-repeat is never a new press. Losing window focus releases every held key. The display thread only queues
commands (at most 16) and publishes the held-key state; the telemetry thread sends them on its own kRPC
connection and logs ``key: ...`` or why a key was not applied. Held axes are repeated with
``KSPHMIFlight.SetAxes`` every telemetry tick and once with zeros on release; the plugin drops values older
than 0.5 s and applies them only where the board's joystick is neutral, so the joystick always wins (see
:doc:`plugin`). kRPC's own ``Control`` axes and throttle are not used: kRPC applies them in its fly-by-wire
callback, where that priority cannot be enforced, and its throttle stays latched while the client is
connected. Outside flight only ``T`` / ``R`` in the VAB do anything. An older ``KSPSerialIO.Hmi.dll`` lacks
``KSPHMIFlight`` (0.19.6.0) or ``KSPHMI.Press`` (0.19.5.1): those keys are logged as not applied.

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
docking/decoupling). If KSP temporarily rejects creation of a stage delta-v stream, only the missing
streams are retried every 0.5 seconds; working resource and stage streams remain subscribed. These displays
do not activate stages or transfer resources.

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
not live contract or crew lists; in the VAB a live ship report replaces them when the game serves one
(`VAB ship report`_). Flight data resumes automatically after launching or switching to a vessel.

KSPSerialIO 0.19.3.15 adds a read-only UDP context responder on port 50002. The Pi sends ``KSPHMI/1`` once per
non-flight sample; the response is ``KSPHMI/1 <scene>``. Facility spawn/despawn events distinguish buildings
that all use KSP's Space Center scene. Deploy both the plugin and HMI to enable these views, and allow UDP
50002 from the Pi through the PC firewall. No serial port is used. A missing responder, unknown scene or
blocked port falls back to the generic waiting screen; flight telemetry still uses kRPC unchanged.
The simulator serves the same context protocol and offers all buildings in its scene selector.

The header shows ``LIVE``, ``CONNECTED`` (outside flight), ``OFFLINE`` (no kRPC) or ``NO DATA``
(connected, but no new sample for 3 s: the game froze or the link stalled). A periapsis inside the atmosphere is
orange and one below the surface red.

Apoapsis markers are omitted when the conic cannot supply a finite position near the parabolic limit,
for both the current orbit and the maneuver preview.

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
(Apollo CRT: dimmer green)
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

Target, closest approach and encounters (KSP 1 kRPC only; the Redux source leaves these snapshot fields empty).
When ``SpaceCenter.target_vessel`` or ``target_body`` orbits the same body as the active vessel (a moon of the
current body counts: Ike while orbiting Duna; a vessel around another planet does not), ORBIT draws its orbit as
a solid line in the target colour, built from ``Orbit.position_at`` / ``velocity_at`` at the current UT in the
same non-rotating frame and fixed view axes, with the NavBall's target mark (a ring with four ticks) at its
position and its name; a target body is also drawn as a small disc. The closest approach comes from
``Orbit.next_closest_approach(target.orbit)`` on the post-maneuver orbit when a node exists (as KSP), else the
current one: the ``ClosestApproach`` object's ``ut``, ``position`` and ``target_position`` in the same frame
(an older kRPC without it falls back to ``Orbit.time_of_closest_approach`` and ``position_at``). As in the map
view, two chevrons point at each other, the vessel's (blue, or orange on the planned orbit) from above and the
target's from below, joined by a dotted line and labelled with the separation. This install's kRPC has no
``time_of_closest_approach`` / ``distance_at_closest_approach``.

An encounter is a sphere of influence entry into a moon of the current body: ``Orbit.time_to_soi_change`` (NaN
without one) and ``Orbit.next_orbit`` of the current orbit, and of ``Node.orbit`` for the planned path. KSP's
``position_at`` of a future patch around another body adds that body's *current* position, so the patch is not
read from ``next_orbit``: the HMI reads the vessel's and the moon's states at the entry time from their orbits
around the current body and builds the patch as a conic of the moon's gravitational parameter. The current or
dashed planned orbit ends at the entry; the patch inside the sphere is drawn (solid, or dashed when planned) in
the encounter colour around the moon's outline at the entry time, with KSP-like SOI entry/exit icons (a ring with
a notch pointing in or out) and the encounter periapsis labelled ``Pe <altitude>`` (amber below the surface).
With an encounter of the target, the closest approach markers are hidden, as in KSP. Skins set
``Display::orbit_style`` (ksp1/ksp2: the map view's violet target and warm encounter colours; a CRT-styled ORBIT
maps them to dim phosphor green). These values are read on the telemetry thread about once a second, and at once
when the target or the node list changes; each failing part is dropped alone and logged once
(``target orbit unavailable: ...``, ``closest approach unavailable: ...``, ``encounter unavailable: ...``).
In the simulator the target body is the Mun (``target_enabled``), on a 12 Mm circular orbit, with closest
approaches and Mun encounters computed from the coasting vessel or node orbit.

The view fit has hysteresis, so optional content that comes and goes (a node, a target, an encounter) does not
rescale ORBIT abruptly: the held extent grows at once to contain new content and shrinks only after the content
has filled less than 60% of it for 2 s (snapshot time). A new body, vessel axes or panel size refits at once.
The scale stays quantised in 0.5% steps; the geometry itself is never smoothed.

On the Pi there is no desktop. The current service sets ``KSPHMI_OUTPUT=auto`` and
``SDL_VIDEODRIVER=kmsdrm``: it first tries an accelerated SDL renderer and falls back to Linux fbdev when
that path cannot open. The GPU path needs the working Mesa EGL/GLES stack and DRM access. The old
pygame attempt left the screen black; that historical result does not describe the current C++ GPU path.

With fbdev selected, SDL draws into an ARGB8888 software surface and only changed rows are copied to
``/dev/fb0`` (RGB565 or XRGB8888). This fallback needs neither EGL nor DRM master. Explicit
``KSPHMI_OUTPUT=fbdev`` forces it; ``KSPHMI_OUTPUT=sdl SDL_VIDEODRIVER=kmsdrm`` selects SDL output.
The framebuffer can be left *blanked* (``/sys/class/graphics/fb0/blank`` = 4, powered down; it happened after
the kmsdrm attempts): frames are then written but nothing shows, while the HMI log looks healthy. The HMI
unblanks it (``FBIOBLANK`` ioctl, no root needed) at start and every 30 s. ``./ci verify`` (``hmi: screen``)
reads the output from the HMI's latest ``screen WxH (...)`` journal line: for fbdev it checks that value;
for SDL KMSDRM ``fb0/blank`` stays 4 by design, so it checks that an HDMI connector
(``/sys/class/drm/card*-HDMI-*``) is ``connected`` with ``dpms`` ``On``; an unknown output is a warning.
The console underneath (fbcon, tty1) still blinks its cursor into the framebuffer between frames; only
root can turn that off, hence the tmpfiles rule below. With blinking off the cursor is never redrawn and the
next frame covers it. The waiting screens show this display's own address (the one it uses to reach the
kRPC server first) and the server it looks for, to find the Pi without a keyboard. Each network identity
cache refreshes on a background worker every 15 seconds. Drawing reads only the cached text, initially
showing a lookup message, so slow DNS cannot stall waiting-screen animation or SDL events. Destruction
signals the worker without waiting for a system DNS lookup; its shared state survives until the lookup ends.

``kill -USR1 <pid>`` makes a running HMI save its next frame, read back from the screen's own renderer
(GPU included), to ``$XDG_RUNTIME_DIR/ksphmi-shot.png`` (``/tmp`` without it) and log ``live screenshot:``.
``--screenshot`` renders off-screen in software, which can differ from what the Pi's GPU shows.

The service log is read with ``journalctl _SYSTEMD_USER_UNIT=ksphmi.service`` (``./ci hmi_logs``): Raspberry
Pi OS keeps no per-user journal, so ``journalctl --user`` finds nothing.


.. _hmi-cockpit:

Cockpit panel
-------------

A second flight scene turns the screen into a clickable switch panel (approved 2026-11-16; design proposal with
mockups, in Spanish: `Tablero de cabina <https://claude.ai/artifact/EE5hSyVGXevZa6o73wNuYS>`_). ``Ctrl+"+"`` /
``Ctrl+"-"`` cycle Instruments and Cockpit panel, as do the two tabs of the ribbon below the header. The scene is
kept while the HMI runs (also across screen-size rebuilds); ``[display] flight_scene`` or ``--flight-scene
cockpit`` choose the first one. Outside flight every skin shows its usual facility, VAB or waiting screens.

Every skin draws the same layout (``cockpit_layout`` in ``src/cockpit.cpp``, five groups and a bottom strip) with
its own materials through a ``CockpitPainter``: Apollo's lamp tiles, white group lines, segment windows and a bat
toggle on a hex nut; Steampunk's bulbs and brass plates; Classic's cards; the KSP 1 / KSP 2 buttons and title
bars. Static art (frames, housings, unlit legends, key caps) is cached with the skin's panel; lit lamps, lever
positions, values and click feedback are drawn live.

.. list-table::
   :header-rows: 1
   :widths: 22 40 38

   * - Group
     - Controls (KSP key engraved under each)
     - Shown state (kRPC)
   * - Attitude control
     - SAS ``T``, RCS ``R``, reaction wheels; the ten SAS modes (``Alt+1`` ... ``Alt+0``); speed reference
       ORBIT / SURFACE / TARGET
     - ``Control.SAS``, ``SASMode`` (white when the mode is set but SAS is off), ``ReactionWheels``,
       ``SpeedMode``; TGT / ANTI / NODE and TARGET are veiled without a target or node
   * - Staging & abort
     - ARM, then STAGE (``Space``); guarded ABORT (``Backspace``)
     - current stage, the part titles the next stage activates (``Part.Stage == current - 1``), ``StageLock``,
       ``Abort``
   * - Recovery
     - DEPLOY (``Control.Parachutes = true``), guarded CUT (``Parachute.Cut`` on open chutes)
     - STOWED / ARMED / OPEN / CUT from every ``Parachute.State``; SAFE / RISKY / UNSAFE from KSP's own
       ``Parachute.SafeState`` (worst of the stowed or armed chutes); the parachute count
   * - Systems
     - gear ``G``, lights ``U``, brakes ``B`` (held), solar panels, antennas, radiators, cargo bays, intakes
     - the ``Control`` property; grey "NOT FITTED" when ``Parts`` has no such part (legs + wheels for gear)
   * - Action groups
     - custom groups 1-10 (keys ``1`` ... ``0``)
     - ``Control.GetActionGroup``
   * - Throttle · time warp · view
     - FULL ``Z`` / CUT ``X``, ``,`` ``/`` ``.``, MAP ``M``, QUICK SAVE ``F5``, guarded QUICK LOAD (``F9``)
     - throttle %, ``SpaceCenter.WarpRate``

**A click is the key.** ``CockpitInput`` (``src/cockpitinput.cpp``, no SDL) turns a click into the same
``Command`` the key produces, into the same queue (at most 16); only the telemetry thread calls kRPC. New commands:
``ToggleControl`` (``ReactionWheels``, ``SolarPanels``, ``Antennas``, ``Radiators``, ``CargoBays``, ``Intakes``),
``SpeedMode``, ``DeployChutes`` and ``CutChutes``. In flight a command thread sends each queued command as soon as
it arrives, on the telemetry connection (``krpc::Client::invoke`` serializes calls) with the vessel's ``Control``
object looked up once, so a click waits at most for the one call in progress, not for a telemetry iteration. A
click on a switch or action group already knows the state its lamp shows and writes the other state in one call
(``Command::set``); the keys still read the state first. The log shows each command's time in the queue and in
kRPC (``key: Lights on (queued 2 ms, kRPC 48 ms)``). Over Wi-Fi a kRPC call takes about 50 ms (up to about 250 ms)
against a few ms over Ethernet: a wired Mac or PC answers clicks fastest. Mouse coordinates are converted to
canvas pixels (HiDPI);
a touch screen's synthesized mouse events work the same way. BRAKES act while the button is held; losing focus,
changing scene or leaving flight releases them.

**Lamps follow the game, never the click.** A pressed control shows a "sent" outline until the snapshot reports
the change (switch flipped, mode selected, stage number changed, chute state changed). No change within 2.5 s
blinks the control amber for 1.2 s (for example a SAS mode KSP refused); the log says why (``key ... not
applied``). Controls whose result the game does not report (throttle, warp, map, saves) only flash "sent".

**Dangerous controls.** STAGE needs ARM first (a 5 s window, one stage per arming; STAGE without ARM blinks the
ARM lamp). ABORT, CUT and QUICK LOAD have a guard: the first click lifts it, the second within 5 s acts. The keys
(``Space``, ``Backspace``, ``F9``) act exactly as in KSP.

**Data only while shown, never blocking.** ``Source::set_cockpit`` tells the telemetry thread whether the panel is
on screen; ``CockpitStreams`` (``src/telemetry.cpp``) exist only then. Everything read per sample is a stream:
switch states, groups, abort, stage lock, warp rate, the part lists (which systems are fitted, the parachutes, all
parts) and the current stage, so a sample makes no synchronous call. The work that needs calls (adding those
streams, one State and SafeState stream per parachute, at most 16, and reading ``Part.Stage`` / ``Part.Title``
when the current stage or the part set changes) is queued and run within 40 ms per sample. From a laptop on Wi-Fi
a kRPC call takes about 50 ms: the first version scanned 90 parts in one go and delayed the first sample and every
queued click by seconds. Every value is optional: a failing call blanks that control, never the session.

**Mouse on the Pi.** The Pi screen has no touch input; plug a USB mouse. Under SDL KMSDRM SDL reads it through
evdev. The full-screen HMI hides the cursor; moving the mouse shows it until 3 s without motion. The fbdev
fallback output has no SDL input events: there the panel is operated with the engraved keys. On KSP 2 Redux the
new commands are logged as not supported (no ``KSP2KEY`` verb yet); the other controls work.

.. uml:: diagrams/seq_cockpit_click.puml

Tests: ``ctrl_plus_and_minus_step_the_flight_scene_without_moving_the_throttle``, ``cockpit_clicks_*``,
``cockpit_stage_needs_arm_and_guards_need_two_clicks``, ``cockpit_lamps_follow_the_game_never_the_click``,
``cockpit_commands_reach_krpc`` (core); ``cockpit_layout_fits_every_screen_without_overlaps`` (3:2, 16:9, 4:3
and the Mac's 2940x1846; every control placed, no overlaps, hit test) and
``cockpit_renders_inside_its_groups_in_every_skin`` (render). ``KSPHMI_SHOTS=DIR`` also writes
``SKIN_cockpit.png`` and ``SKIN_cockpit_clicks.png`` (armed, guard lifted, sent and rejected) for review;
``--demo --flight-scene cockpit`` shows the demo vessel's panel. ``tools/kspsim`` serves every procedure above
(``KrpcTest.test_cockpit_panel_procedures``).


.. _hmi-mechjeb:

MechJeb scene (approved, in progress)
-------------------------------------

Approved 2026-11-16 (design with mockups, in Spanish: `Escena MechJeb
<https://claude.ai/artifact/Bz6w131YJtZfeRaCCtNcKG>`_): a third flight scene in the ``Ctrl+"+"`` / ``Ctrl+"-"``
cycle (Instruments, Cockpit panel, MechJeb). It opens on a menu of MechJeb modules; a click on **Ascent Guidance**
opens its page, which mirrors that module's options and has a **< MECHJEB** button back to the menu. The page shown
is kept when the scene is left. Decisions: our own kRPC service (no KRPC.MechJeb), ENGAGE needs ARM first (the
cockpit panel's 5 s window), values are edited with an on-screen numeric keypad (the physical keyboard types into
it too) or − / + steps. Phase 2 modules, in order: Maneuver Planner + Node Executor, Landing Guidance, Rendezvous,
Docking, Translatron, Spaceplane, Smart A.S.S. last.

**Service.** ``KSPSerialIO.Hmi.dll`` reads and changes MechJeb by reflection (``MuMech.MechJebCore`` of the active
vessel), so MechJeb stays an optional mod: without it the scene says so and shows no values. Procedures of
``KSPHMIFlight`` (flight scene, Unity's thread):

* ``uint MechJebRevision()``: increments when the report text changes (refreshed twice a second).
* ``string MechJebReport()``: the report below.
* ``string MechJebSet(string key, string value)``: ``ok``, ``unknown key``, ``read only``, ``invalid value`` or
  ``unavailable`` (no MechJeb / no core). Keys and units are the report's.
* ``string MechJebAction(string action)``: ``ascent.engage``, ``ascent.disengage``, ``ascent.launch_rendezvous``,
  ``ascent.launch_target_plane``, ``ascent.abort_timed_launch``, ``all.disengage``; same replies. Actions and
  settings are applied on Unity's thread (kRPC runs procedures there).

**Report format v1** (fixture ``hmi-cpp/tests/data/mechjeb_report_v1.txt``, shared by the plugin's format tests,
the HMI parser and kspsim). The first line is ``v=1 rev=N mj=STATE`` plus ``version=X`` when installed; ``mj`` is
``ok``, ``missing`` (MechJeb not installed), ``nocore`` (no MechJeb part or core on the vessel) or
``error:TEXT``. Every other line is one ``key=value``; a key the installed MechJeb lacks is omitted and its control
stays unknown, never zero. Values are numbers in SI units (metres, m/s, seconds, degrees, pascals, fractions 0-1,
flags 0/1) or percent-encoded text. Readers ignore unknown keys, so later modules add lines without a new version.

On the plugin side ``MechJebFormat.cs`` (``KSPSerialIO.Hmi``, plain C# without game or MechJeb types) holds the
key list with each key's kind (flag, number, integer, text) and read-only mark, the actions, the report text,
number formatting (invariant culture, at most six decimals, trailing zeros trimmed, ``nan`` when not finite) and
the parsing of ``MechJebSet`` values; the reflection bridge only reads and writes MechJeb members.
``tools/plugin-shipreport-tests`` (``./ci verify``, *ship report: plugin format*) requires its keys to match the
fixture's in order and its report to reproduce the fixture byte for byte.

Keys map to MechJeb 2.15.3 members (reflected on the shared install, 2026-11-17): ``AS`` is
``MechJebCore.AscentSettings`` (``MechJebModuleAscentSettings``), ``TC`` ``MechJebCore.Thrust``
(``MechJebModuleThrustController``), ``SC`` ``MechJebCore.Staging`` (``MechJebModuleStagingController``), ``AP``
``MechJebCore.Ascent`` (``MechJebModuleAscentBaseAutopilot``). ``Editable*`` fields carry their SI value in
``.Val``; the display multiplier only scales ``.Text``. RO = read only.

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Keys
     - MechJeb 2.15.3 member
   * - ``ascent.enabled`` (RO), ``ascent.status`` (RO, text), ``ascent.t_minus`` (RO, s, only while a timed
       launch is armed)
     - ``AP`` enabled state, ``AP.Status``, ``AP.TMinus`` (reported only while ``AP.TimedLaunch`` is true; absent
       otherwise, which the HMI reads as "no countdown", never 0)
   * - ``ascent.path`` (0 Classic, 1 GravityTurn, 2 PVG)
     - ``AS.AscentTypeInteger``
   * - ``ascent.altitude``, ``ascent.inclination``, ``ascent.lan``
     - ``AS.DesiredOrbitAltitude``, ``AS.DesiredInclination``, ``AS.DesiredLan``
   * - ``ascent.launch_phase_angle``, ``ascent.launch_lan_difference``, ``ascent.warp_count_down``
     - ``AS.LaunchPhaseAngle``, ``AS.LaunchLANDifference``, ``AS.WarpCountDown``
   * - ``ascent.classic.auto``, ``ascent.classic.auto_turn_start_altitude`` (RO)
     - ``AS.AutoPath``, ``AS.AutoTurnStartAltitude``
   * - ``ascent.classic.turn_start_altitude`` / ``_velocity``, ``turn_end_altitude``, ``turn_end_angle``,
       ``turn_shape_exponent``
     - ``AS.TurnStartAltitude``, ``TurnStartVelocity``, ``TurnEndAltitude``, ``TurnEndAngle``,
       ``TurnShapeExponent`` (GravityTurn uses the same start altitude / velocity)
   * - ``ascent.pvg.desired_apoapsis``, ``desired_attach_alt``, ``desired_fpa``, ``pitch_start_velocity``,
       ``pitch_rate``
     - ``AS.DesiredApoapsis``, ``DesiredAttachAlt``, ``DesiredFPA``, ``PitchStartVelocity``, ``PitchRate``
   * - ``ascent.force_roll``, ``vertical_roll``, ``turn_roll``, ``roll_altitude``
     - ``AS.ForceRoll``, ``VerticalRoll``, ``TurnRoll``, ``RollAltitude``
   * - ``ascent.limit_aoa``, ``max_aoa``, ``limit_qa``, ``max_qa`` (Pa·deg)
     - ``AS.LimitAoA``, ``MaxAoA``, ``LimitQaEnabled``, ``LimitQa``
   * - ``ascent.corrective_steering``, ``corrective_steering_gain``
     - ``AS.CorrectiveSteering``, ``CorrectiveSteeringGain``
   * - ``ascent.autostage``, ``last_stage``, ``autodeploy_solar_panels``, ``autodeploy_antennas``,
       ``skip_circularization``
     - ``AS.Autostage`` (property), ``LastStage``, ``AutodeploySolarPanels``, ``AutoDeployAntennas``,
       ``SkipCircularization``
   * - ``thrust.limit_q`` / ``max_q``, ``limit_overheat``, ``limit_acceleration`` / ``max_acceleration``,
       ``limit_throttle`` / ``max_throttle``, ``limit_min_throttle`` / ``min_throttle``, ``smooth_throttle``
     - ``TC.LimitDynamicPressure`` / ``MaxDynamicPressure``, ``LimitToPreventOverheats``, ``LimitAcceleration`` /
       ``MaxAcceleration``, ``LimitThrottle`` / ``MaxThrottle``, ``LimiterMinThrottle`` / ``MinThrottle``,
       ``SmoothThrottle``
   * - ``thrust.prevent_flameout`` / ``flameout_safety_pct`` (fraction 0..1, 0.05 = 5 %),
       ``prevent_unstable_ignition``, ``differential_throttle``
     - ``TC.LimitToPreventFlameout`` / ``FlameoutSafetyPct`` (a percent in MechJeb: the bridge divides by 100 on
       read and multiplies by 100 on ``MechJebSet``), ``LimitToPreventUnstableIgnition``, ``DifferentialThrottle``
   * - ``staging.autostage_limit``, ``pre_delay``, ``post_delay``, ``clamp_thrust_pct``, ``autostaging_once``
     - ``SC.AutostageLimit``, ``AutostagePreDelay``, ``AutostagePostDelay``, ``ClampAutoStageThrustPct``,
       ``AutostagingOnce``
   * - ``staging.hot_staging`` / ``hot_staging_lead_time``, ``drop_solids`` / ``drop_solids_lead_time``
     - ``SC.HotStaging`` / ``HotStagingLeadTime``, ``DropSolids`` / ``DropSolidsLeadTime``
   * - ``staging.fairing_max_q``, ``fairing_min_altitude``, ``fairing_max_flux``
     - ``SC.FairingMaxDynamicPressure``, ``FairingMinAltitude``, ``FairingMaxAerothermalFlux``

GravityTurn-only settings (pitch at turn start, intermediate apoapsis, hold time) are added as ``ascent.gt.*`` once
their 2.15.3 members are confirmed by reflection, under the same rule.

MechJeb scene: HMI
^^^^^^^^^^^^^^^^^^

``FlightScene::MechJeb`` is the third scene of the cycle (``--flight-scene mechjeb``, ``[display] flight_scene``)
and has the third ribbon tab; the ribbon (``ribbon_layout`` / ``paint_ribbon`` in ``src/cockpit.cpp``) is shared with
the cockpit panel. ``--mechjeb-page ascent`` opens on the Ascent Guidance page instead of the menu (screenshots).

**Model** (``include/mechjeb.hpp``, core): ``parse_mechjeb_report`` reads report v1: the first line's ``v``,
``rev``, ``mj`` and ``version``; every other line one ``key=value``, percent-decoded; CRLF and unknown keys are
accepted, any other version or a line without ``=`` rejects the report. ``MechJebReport::number`` / ``flag`` return
nothing for a missing key, text or ``nan``: a control without a value shows an empty window or an unlit lamp, never
0. ``demo_mechjeb_report()`` serves the fixture's values to ``--demo`` (a core test keeps it equal to the fixture).

**Controls** (``include/mechjebinput.hpp``, core, no SDL): ``mj_fields()`` gives each key its label, display unit
and scale (km, %, degrees), step, decimals and range. ``MechJebInput`` keeps the page (menu / Ascent Guidance) for
as long as the HMI runs; leaving the scene disarms, closes the keypad and forgets pending changes, never the page.
A click becomes ``Command::MechJebSet`` (``key``, SI ``text``) or ``Command::MechJebAction`` in the same queue and
command thread as the keys. Steppers move one step per left click and ten per right click (not Shift: the left
Shift key ramps the throttle in flight; user, 2026-11-17), clamped to the range and rounded to the field's decimals;
a right click anywhere else does nothing; flags write the other
state; the path selector writes ``ascent.path``. ENGAGE AUTOPILOT and the two timed launches need ARM first (5 s,
one action per arming); DISENGAGE and ABORT TIMED LAUNCH do not. A change shows "sent" until a report carries the
value (``ascent.enabled`` for engage/disengage), "rejected" after 3 s without it, and at once when the plugin's
reply is not ``ok``: ``KrpcSource`` publishes the last reply in ``Snapshot::mechjeb_reply``, and the page shows it
for 5 s (``ALTITUDE: INVALID VALUE``). A click on a value opens the keypad (``Keypad``): its first digit replaces
the shown value, ``-`` toggles the sign, DEL / CANCEL / SET; SET checks the range (and whole numbers) before
sending and otherwise says why. While it is open it is modal, and the physical keys 0-9, ``.``, ``-``,
Backspace, Enter and Esc type into it (Esc only closes it); every other key stays a KSP key.

**Pages** (``include/mechjebpage.hpp``, ``src/mechjebpage.cpp``): ``mechjeb_layout`` places everything from the
screen size, page and ascent path alone, ``mechjeb_hit`` finds the control under a click (only keypad keys while
it is open) and ``paint_mechjeb`` draws it with the skin's ``CockpitPainter``: Classic cards, Apollo lamp tiles
and segment windows, Steampunk bulbs and brass, the KSP 1 / KSP 2 buttons. The menu is a 3 x 3 grid of module
tiles in the approved order (Ascent Guidance first, the phase 2 modules veiled as "PHASE 2 · COMING LATER", Smart
A.S.S. last). The Ascent Guidance page has a page bar (**< MECHJEB**, title, MechJeb's state) and the groups
TARGET ORBIT, ASCENT PATH (CLASSIC / GRAVITY TURN / PVG and the chosen path's settings), GUIDANCE, THRUST,
STAGING & FINISH and LAUNCH (ARM, ENGAGE AUTOPILOT, DISENGAGE, the timed launches, status with ``T-``, and the
flight's apoapsis, periapsis, altitude and vertical speed from the snapshot). Without data (``mj=missing``,
``nocore``, ``error``, or no report: an older plugin, the scene just opened) the bar says so, every setting is
veiled and no value is drawn. Apollo and the KSP skins cache the static art per screen size, page and path.

**Telemetry**: ``Source::set_mechjeb`` tells the telemetry thread whether the scene is shown; only then
``MechJebPoller`` calls ``KSPHMIFlight.MechJebRevision`` every 0.5 s and ``MechJebReport`` only for a new revision
(one or two calls per 0.5 s, never a loop per setting). A kRPC error or a rejected report clears the report (the
scene says "no data" and why) and is retried after 5 s, logged once per reason (``mechjeb: no data (...)``); a new
report state is logged as ``mechjeb: report revision N, ok, MechJeb 2.15.3.0``, each command as ``mechjeb:
MechJeb ascent.altitude=90000 -> ok (queued 1 ms, kRPC 48 ms)``.

Tests: ``mechjeb_report_*``, ``mechjeb_fields_format_step_and_validate``, ``mechjeb_keypad_*``,
``mechjeb_clicks_send_settings_and_wait_for_the_report``, ``mechjeb_engage_needs_arm_and_keypad_sets_values``,
``mechjeb_poller_fetches_on_revision_change_and_commands_reach_krpc`` (core);
``mechjeb_layout_fits_every_screen_and_every_control_is_hit`` (both pages, every path, the keypad, at 1260x840,
1920x1080, 2520x1680, 2940x1846 and 1024x768) and ``mechjeb_renders_inside_its_groups_in_every_skin`` (render).
``KSPHMI_SHOTS=DIR`` writes ``SKIN_mechjeb_menu.png``, ``SKIN_mechjeb_ascent.png`` and ``SKIN_mechjeb_keypad.png``;
``--demo --flight-scene mechjeb`` shows the fixture's values. ``tools/kspsim`` simulates MechJeb
(``KrpcTest.test_mechjeb_procedures``, :doc:`simulator`).

VESSEL panel
------------

The flight screen's right column comes from ``flight_layout`` in ``display.cpp`` for every skin and test.
The Classic/Steampunk profile uses a 34u ORBIT / VESSEL row, a 30u-wide VESSEL panel and the NavBall below.
The approved Apollo Flight Deck profile uses a 33u top row in the screen's right half, with 43 percent of
that row's width assigned to VESSEL and the remaining height assigned to the NavBall.
VESSEL shows a side view of the active vessel, one convex outline per part, in one of two views that
Ctrl+0 switches. **FUEL** fills each tank from the bottom to its propellant level (every resource except
ElectricCharge, Ablator, IntakeAir and Ore); parts without tanks are outlines only. **HEAT** colours each
part blue, green, yellow, orange and red by the hotter of its internal and skin temperature, as a percentage
of its maximum. Parts are drawn farthest first; the panel clips to its well and restores the caller's clip.
Without data it says ``NO VESSEL DATA``. On the shorter NavBall panel the apollo readout rows close up
above the SAS MODE lamps, and the classic globe moves and then shrinks so the speed box and the HDG caption
fit (``Navball::globe_geometry``).

``KSPSerialIO.Hmi.dll`` (0.19.7.0) measures parts the way Vessel Viewer does (MIT): each part's enabled model
meshes (at most 256 vertices per mesh, the bounding box of an unreadable mesh; since 0.19.7.1 not the renderers
whose materials all use effect shaders, ``Waterfall/``, particle or additive: engine plumes reached tens of metres
below the engines) projected on the root part's
right/up plane (forward/up when the vessel is wider that way, 0.19.7.2), then a convex hull of at most 32 points.
Launch clamps and towers are skipped, and so is a mesh reaching well outside its part's colliders (logged once as
``vessel view: skipped mesh``). ``VesselViewReport`` rebuilds that shape only when
the active vessel, its part count or ``onVesselWasModified`` change, and refreshes fuel and heat twice a
second; ``KSPHMIFlight.VesselShape`` / ``VesselState`` only read its cache. The telemetry thread
(``VesselViewPoller``) calls ``VesselState`` every 0.5 s and ``VesselShape`` only when the state's revision
differs from the shape it holds. A missing service (older DLL) or text that does not parse empties the panel,
is logged once per reason and retried after 5 s; the session continues. Text format version 1, two decimals,
invariant culture::

   v=1 rev=12 parts=3
   p=0.00,-1.30:18.60,1.30:18.60,0.00:20.00          depth, then x:y points (metres)
   p=0.25,-1.80:9.50,1.80:9.50,1.80:13.30,-1.80:13.30
   ...
   v=1 rev=12
   s=-1:30,48:20,-1:93                                fuel:heat per part, shape order (fuel -1 = no tanks)

``hmi-cpp/tests/data/vessel_shape_v1.txt`` and ``vessel_state_v1.txt`` are the shared fixture: the plugin's
formatter must write them exactly (``tools/plugin-shipreport-tests``), the HMI parses them and kspsim serves
them. ``./ci verify`` (*vessel view: contract*) checks that the three agree. ``--demo`` flies a three-stage
rocket with two boosters.

VAB ship report
---------------

In the VAB the static cards are replaced by a live engineering report of the ship being built, when the game
serves one. It comes from ``KSPSerialIO.Hmi.dll`` (KSPSerialIO 0.19.4.0), a separate assembly that adds the kRPC
service ``KSPHMI`` (see :doc:`plugin`). The Pi uses its existing kRPC connection with no new network port; the Arduino optionally navigates editor modes.

.. uml:: diagrams/seq_vab_report.puml
   :caption: Editor events build and cache the report; the Pi polls the revision and fetches the text on change.

* ``KSPHMI.Revision() -> uint`` increases whenever the report's contents change. While the UDP context says
  ``vab``, the telemetry thread calls it about twice a second (``ShipReportPoller``) and calls
  ``KSPHMI.ShipReport() -> string`` only when it differs from the revision last fetched.
* ``KSPHMI.Press(string button)`` (0.19.5.1) is the keyboard's T / R in the VAB: ``"SAS"`` or ``"RCS"``, any
  other value is an ``ArgumentException``. An older ``KSPSerialIO.Hmi.dll`` lacks it: the key is logged as not
  applied and nothing else changes.
* The procedures exist only in the editor scene (kRPC ``GameScene.Editor``). A missing service (DLL not
  installed, no kRPC), another scene or a report that does not parse leaves ``Snapshot.ship`` empty: the static
  VAB cards stay, the session is not affected, and server errors are retried after 5 s. The log says
  ``ship report unavailable (static VAB cards): ...`` once per distinct reason and
  ``ship report: revision N, <name>, <situation>, K stages`` at INFO for every newly fetched revision (one line per
  revision, so a live session leaves a trace of each change). The SPH keeps its cards until phase 3.

Report text, format version 1 (``hmi-cpp/include/shipreport.hpp``; the shared fixture is
``hmi-cpp/tests/data/ship_report_v1.txt``):

.. code-block:: text

   v=1 rev=57 facility=vab name=Kerbal%20X sit=kerbin_asl
   mass=18.40 dry=6.10 cost=23400 funds=41200 parts=42 crew=1 seats=3 size=11.2,3.8 engines=5
   st=2,1720,1960,1.63,48,engines:3,clamps:1
   st=1,1890,2010,1.41,71,engines:1,decouplers:1
   st=0,1210,1340,0.62,52,engines:1,decouplers:1,chutes:1

* One ``key=value`` group per field, separated by single spaces; one line per group kind. Line 1: version,
  revision, facility, name, delta-v situation (``<body>_<asl|alt|vac>``). Line 2: wet and dry mass (t), cost,
  funds (absent outside career games), parts, crew and seats, height and width (m), engines.
* ``st=`` lines in firing order: stage number, delta-v in the situation of KSP's editor delta-v app, delta-v in
  vacuum (m/s), TWR in that situation, burn time (s), then ``kind:count`` for what the stage fires (engines,
  decouplers, chutes, clamps, fairings). Until KSP finishes its first delta-v calculation the report has a single
  ``dv=pending`` line instead, and the screen says it is calculating.
* ``kasl=stage:dv,...`` (plugin 0.19.4.1 and later, optional): each stage's delta-v at Kerbin sea level, computed
  independently of the situation selected in KSP's delta-v app, so the mission delta-v below never depends on the
  body the player is looking at. Older plugins do not send it; a malformed ``kasl=`` rejects the report.
* Text values are percent-encoded UTF-8: every byte outside ``A-Z a-z 0-9 . _ ~ -`` becomes ``%XX``, so spaces,
  ``=``, ``,`` and ``%`` never appear raw (``Kerbal X`` is ``Kerbal%20X``). Numbers use ``.`` as the decimal
  separator; ``nan`` is an unknown value.
* The Pi rejects any ``v`` other than 1 and any malformed known field (the whole report: a half-read report
  could mislead), and ignores keys and line kinds it does not know: phase 2 adds ``chk=`` and ``lim=`` lines.

The screen (both skins): the delta-v total in the report's situation (also in the header), exactly as KSP's own
app adds the stages, so it can be compared with the game; the stage table (delta-v, vacuum delta-v, TWR, burn time,
what fires); the destinations; and the vehicle summary.

**Mission delta-v.** Summing every stage in one situation badly understates upper stages (the Kerbal X's stage 2
has 631 m/s at sea level and 2452 m/s in vacuum), so destinations, the target margin and the target tick use a
mission delta-v instead: stages in firing order count at Kerbin sea level while the sea-level delta-v already
counted is below ``[vab] atmosphere_dv`` (1200 m/s, the ascent through the lower atmosphere), and in vacuum after
that. For the stock Kerbal X: 257 + 304 + 372 + 2764 (sea level) + 2452 (vacuum) = 6149 m/s, against KSP's 4328 at
sea level. The sea-level figures come from the ``kasl=`` line, whatever body the app shows; with an older plugin
(no ``kasl=``) the app's situation values stand in, which is exact on Kerbin sea level, and the screen says
``NO KERBIN ASL DATA``. Both skins label the basis (``MISSION ΔV 6149 · ASCENT KERBIN ASL, UPPER STAGES VAC``);
apollo shows it as the MISSION ΔV readout beside TOTAL ΔV and paints the stage-stack bands as each stage's share
of it, classic as a line beside the vacuum total and above the destinations.

**Target.** It follows KSP's delta-v app: on Kerbin it is the configured ``[vab] target``; on another body it is
that body's ``<body>_landing`` for sea level or altitude and ``<body>_orbit`` for vacuum, else the other one of that
body, else the configured target. The screen says why (``TARGET DUNA ORBIT · FROM KSP ΔV APP`` or
``· FROM CONFIG``). Reachable destinations are green, the others red, the target outlined.

**Empty craft and calculating.** KSP never computes delta-v for a ship without parts (the editor after New), so
``parts=0`` with ``dv=pending`` shows ``EMPTY CRAFT · add parts to see the delta-v report`` instead of a
calculating state that would never end. While KSP computes a real ship, a framed amber ``CALCULATING ΔV`` plate
covers the stage table (and apollo lights its ΔV CALC lamp).

The apollo skin keeps its static art (four detailed stage rows, destination lamp housings, the target ring and the target
caption) in the cached panel texture, rebuilt only when the size, the ``[vab]`` settings or the chosen target
change.

TWR colours follow the ``[vab]`` thresholds. Only stages with delta-v are judged; the launch stage is the first one
in firing order with delta-v. Launch stage: red below ``launch_twr_min`` (1.0, it cannot leave the pad), amber below
``launch_twr_warn`` (1.2). Upper stages: amber below ``upper_twr_warn`` (0.8). Apollo shows them as red/amber
segment digits and, in the VAB header, as the LAUNCH TWR and UPPER TWR lamps. The flight annunciators (SAS, RCS,
PE ATMO, LOW FUEL, ...) are not shown in the VAB: only UPLINK / NO DATA / NO LINK and the two TWR lamps; the other
housings stay blank for the phase-2 checklist lamps.

Destinations and the highlighted target are settings (``[vab]`` in ``config.toml``; ``config.local.toml`` may add
destinations or change their delta-v):

.. code-block:: toml

   [vab]
   target = "mun_landing"      # while the app shows Kerbin
   launch_twr_min = 1.0        # launch stage red below
   launch_twr_warn = 1.2       # launch stage amber below
   upper_twr_warn = 0.8        # upper stages amber below
   atmosphere_dv = 1200        # mission delta-v: sea-level ascent before stages count in vacuum

   [vab.destinations]          # m/s from the Kerbin launch pad, at most 12
   kerbin_orbit = 3400
   minmus_orbit = 4490
   mun_orbit = 4570
   minmus_landing = 4670
   ike_orbit = 4940
   duna_orbit = 5090
   mun_landing = 5150
   ike_landing = 5330
   eve_orbit = 5850
   dres_orbit = 6260
   duna_landing = 6540
   dres_landing = 6690

Names use ``a-z``, ``0-9`` and ``_`` (shown upper case, ``_`` as a space); values must be between 0 and 100000, as
must ``atmosphere_dv`` (0 counts every stage in vacuum). A target that is not a destination is a configuration
error naming the key and the file, as is a TWR threshold that is not a number between 0 and 100 or a
``launch_twr_min`` above ``launch_twr_warn``.

The shipped values are cumulative from the pad, from the widely used community delta-v map of the Kerbol system
(KSP 1.x, by u/Kowgan and contributors), **without aerobraking**: low Kerbin orbit 3400; Mun transfer 860, low orbit
310, landing 580; Minmus transfer 930, low orbit 160, landing 180; Kerbin escape 950, then Duna transfer 130,
capture 250, low orbit 360, landing 1450 (Ike from the Duna capture: 30, low orbit 180, landing 390); Eve transfer
90, capture 80, low orbit 1330; Dres transfer 610, low orbit 1300, landing 430. Aerobraking at Duna or Eve saves most
of their capture and landing figures. Each body other than Kerbin has an orbit and a landing entry (Eve only its
orbit: landing there means aerobraking), so the app-driven target always finds one. The earlier table's
``mun_flyby = 4310`` did not match the map (3400 + 860 = 4260); the flybys and keostationary were dropped to stay
within the twelve destinations both skins lay out.

Without the game: ``./ci sim --scene vab --ship kerbal_x`` (or ``pending``, ``heavy``, ``empty``, ``none``) serves sample ships
(:doc:`simulator`). Review screenshots of both skins: ``KSPHMI_SHOTS=DIR ./ci test_hmi`` writes
``classic_vab*.png`` and ``apollo_vab*.png`` from the shared sample and from the live stock Kerbal X reports in
``hmi-cpp/tests/data/ship_report_kerbal_x*.txt`` (app on Kerbin sea level and on Duna vacuum; the latter with the
values KSP's app showed), plus the empty-craft and calculating states.

Editor mode screens
-------------------

The approved Apollo and Classic skins retain their existing colours, lettering, frames, annunciators and
readouts. The design mockups approved on 2026-11-15 describe information and navigation, not a replacement skin.
KSPSerialIO 0.19.5.0 adds four VAB screens:

* **Construction:** the existing delta-v report, with component titles and quantities below each stage's
  metrics (engines, decouplers, parachutes, clamps and fairings). KSP's staging list remains authoritative;
  unstaged parts are excluded, and stages without delta-v, including parachutes, remain visible.
* **Action groups:** stock Stage, Gear, Lights, Brakes, Abort, SAS and RCS, and custom groups 1-10, with assigned
  action counts and the panel control that drives them in flight. The detail panel follows the group selected
  in KSP and lists its part/module actions. Only SAS and RCS currently have enabled physical inputs; the stage
  and custom-group-1 pins remain disabled. The display does not execute these actions in the editor.
* **Crew:** each current-ship part and seat, assigned kerbal, stock role and experience level, or an empty seat;
  assigned/free totals and the role counts. Seat indices come from the manifest, not a compact crew list.
* **Cargo:** stored part stacks, quantity, slot and owning inventory, mass and packed volume, plus each stock
  inventory's used mass/volume, capacities and slots. Inventory totals are KSP's own cached totals; item mass
  comes from its stored snapshot. Unknown data, an unlimited capacity and an empty inventory are distinct.

The Pi follows ``onEditorScreenChange``. The Arduino **SAS** button requests the next stock editor panel and
**RCS** the previous one, cyclically: Construction, Action groups, Crew, Cargo. Requests run on Unity's thread,
are queued across UI transitions, and only the stock screen-change event acknowledges them. A rejected request
times out after two seconds without changing the reported mode. Selecting a panel directly in KSP is always
authoritative. Lists rotate every eight seconds with a page indicator; construction stage rows also rotate,
and long component lists rotate within their stage. No extra panel input is needed to reach overflow rows.

``EditorSerialLink`` owns the existing serial transport in the editor, starts discovery on the supervisor
thread, and sends a neutral VesselData heartbeat at 5 Hz. Receive-time SAS/RCS edges go to ``EditorNavigation``;
STAGE, analog axes and action-group levels do not control the editor. Presses older than one second are dropped.
Leaving the editor closes the port; scene initialization clears axes, control levels, publication flags and
pending presses before flight can consume them. The firmware and serial packet layouts do not change.
**KSP now owns the serial port in the editor as well as in flight: close KSP before external board probes.**

The existing ``KSPHMI.Revision`` / ``ShipReport`` procedures carry optional format-v1 lines (old readers ignore
them; a new reader defaults to Construction for old reports):

.. code-block:: text

   editor=actions,custom1
   ac=custom1,Communotron%2016,Toggle%20antenna
   seat=1,Mk1-3%20Pod,0,Valentina%20Kerman,Pilot,1
   seat=1,Mk1-3%20Pod,1,,,0
   inv=1%3A0,Mk1-3%20Pod,3,0.022000,0.050000,84.000,120.000
   cargo=1%3A0,0,Repair%20kit,2,0.010000,20.000
   sp=2,engines,LV-T30%20Reliant,3

``editor`` contains mode and selected-group code. ``ac`` contains group, part title and action caption.
``seat`` contains part id, part title, zero-based seat, name (empty for free), role and level. ``inv`` contains
part-id/module-index key, title, slot count, used mass, mass capacity, used volume and volume capacity.
``cargo`` contains inventory key, zero-based slot, title, quantity, stack mass and stack volume. ``sp`` contains
stage number, event kind, part title and quantity. Mass is in tonnes, volume in litres; ``nan`` is unknown and
capacity ``-1`` means unlimited. The parser rejects malformed known fields and dangling inventory/stage links.
Crew, action and inventory lines remain available while delta-v is pending. They are collected at 2 Hz because
not every editor dialog sends ``onEditorShipModified``; the revision advances only when the text changes.

``hmi-cpp/tests/data/editor_report_v1.txt`` is shared by plugin formatting, Pi parsing and simulator tests.
``./ci sim --scene vab --editor-mode actions`` also exposes mode/group controls in its window. Its serial link
sends the same neutral heartbeat in the editor and routes SAS/RCS to mode changes without changing flight state.
``KSPHMI_SHOTS=DIR ./ci test_hmi`` writes ``classic_editor_*.png`` and ``apollo_editor_*.png`` from the real
renderers with simulator fixture data. The native Windows build (MSYS2) sees the variable directly; with the
WSL fallback set it inside WSL (the Windows wrapper does not forward arbitrary environment variables).

Facility screens roadmap
------------------------

Three facility screens were designed and approved on 2026-11-15 (proposals with mockups of both skins, written
in Spanish: `VAB <https://claude.ai/artifact/5FMkLW25dQJmWzRf6r291g>`_, `Tracking Station
<https://claude.ai/artifact/8KnQXpYxJKR2x4R8dCPerd>`_, `Astronaut Complex
<https://claude.ai/artifact/BMZVd6PmfMXHEiUBtaFhsv>`_). All three extend the same kRPC service ``KSPHMI`` in
``KSPSerialIO.Hmi.dll``; each report carries a format version and a revision, and the Pi falls back to the static
cards when a procedure is missing. Implement them in this order, each merged before the next starts:

#. **VAB, phase 1** (this section above). Phase 2: live pre-launch checklist (control, power, parachute, antenna,
   decouplers, TWR) and facility limits (parts, mass, height, width vs. VAB/launch pad level; cost vs. funds),
   with the apollo lamps NO CHUTE / UPPER TWR / PARTS LIMIT. Phase 3: thrust offset and CoM shift full to dry,
   and the SPH variant (CoM vs. CoL).
#. **VAB screens per editor mode** (decided by the user 2026-11-15; mockups of both skins and the named staging
   component breakdown were approved; keep the real Apollo and Classic skin artwork). One Pi screen per KSP editor mode: *Construction* (the current delta-v report),
   *Action groups* (custom groups 1-10 and the stock groups Stage, Gear, Lights, Brakes, Abort, SAS, RCS with
   their actions, and which Arduino panel control drives each in flight), *Crew* (seats per part, assigned kerbals
   with role and level, free seats) and *Cargo* (inventories: stored parts, mass and volume against capacity).
   Navigation: the Pi follows the editor mode automatically (EditorLogic's editor screen), and the Arduino SAS and
   RCS buttons step to the next / previous screen while in the VAB. That needs the plugin to open the serial link
   in the editor too (``EditorSerialLink`` in 0.19.5.0), so it must be tested with the board. Order:
   after VAB phase 1 is merged, before Tracking Station phase 1.
#. **Tracking Station, phase 1**: ``FleetRevision``/``FleetReport`` (next events of every vessel by absolute UT:
   maneuver nodes with delta-v, Ap/Pe, SOI changes, stock alarms; fleet alerts: Pe in atmosphere or under the
   terrain, no CommNet link, low or empty EC, stranded crew; debris excluded) and
   ``SelectionRevision``/``SelectionReport`` (selected vessel with the ORBIT diagram). The Pi computes
   countdowns from the game clock, so the report does not change every second. Phase 2: selected body, transfer
   windows, fleet counts, delta-v left only when KSP has computed it. Thresholds, event count and transfer
   targets in ``config.toml``. To confirm in game: SOI patches of unloaded vessels and the selection event.
#. **Astronaut Complex, phase 1**: ``CrewRevision``/``CrewReport`` in the Space Center scene (roster with role,
   level, XP to next level and status or vessel; free crew by role; best available per role with a fixed table of
   what each stock level unlocks; capacity vs. complex level, next hire cost, applicants; lamps CREW LOST,
   CREW CAP, NO PILOT). Phase 2: kerbals close to the next level, XP pending recovery, suggested mission.

KSP 2 Redux source
------------------

The same HMI also shows KSP 2 Redux, through the KSPKontroller plugin of the KSP2 Redux workbench (a separate
repository; its ``docs/HMI-REDUX.md`` is the plugin side of this contract). It is not kRPC: the plugin answers
UDP requests on port 50003 of the game PC, one datagram per request, each echoing the request's nonce.
``source = "redux"`` (or ``KSPHMI_SOURCE=redux``, which older Pi drop-ins set) uses only Redux;
``source = "auto"`` also runs the kRPC source and shows it whenever Redux has not answered for a second, so
one binary serves both games. The PC address is ``krpc.address`` / ``KSPHMI_KRPC_ADDRESS`` unless
``KSPHMI_REDUX_ADDRESS`` is set; there is no discovery. Code: ``hmi-cpp/src/redux_source.cpp``, tests
``hmi-cpp/tests/test_redux.cpp`` (ctest ``redux``, loopback only).

=============================== =====================================================================
Request                         Reply
=============================== =====================================================================
``KSP2HMI1|KSP2HMI2 N``         snapshot; v2 adds NavBall axes/markers, throttle, SAS mode and native
                                resource totals (names hex-encoded)
``KSP2SHIP N``                  ``\n`` + the VAB ship report v1 (this page's format), or ``-``
``KSP2VSHP N`` / ``KSP2VSTA N`` ``\n`` + VESSEL shape / state v1 (``tests/data/vessel_*_v1.txt``), or ``-``
``KSP2KEY N name value``        ``queued``, ``disabled``, ``invalid`` or ``busy``
``KSP2HOLD N p y r x y z t``    ``queued``, ``disabled`` or ``invalid``
``KSP2ORB1 N``                  ``KSP2ORB1 N`` + space + orbit report v1 (ORBIT state vectors), or ``-``;
                                no reply at all from older plugins or with stale game data
=============================== =====================================================================

The game answers only while its snapshot is younger than 500 ms, and the HMI drains late datagrams before
each request, so one delayed reply never shifts later nonces. The VAB report is asked for at 2 Hz in the VAB;
the VESSEL state at 2 Hz in flight, and the shape only when the state's revision differs from the shape held.

The remote keyboard sends each command once (``sas rcs stage gear lights brakes abort group sasmode
throttle_full throttle_cut warp_up warp_down warp_stop quicksave quickload map``; ``group`` is 1-10, ``sasmode``
the kRPC ``SASMode`` number) and never resends a lost one. Held keys go every tick while held, then once with
zeros: six axes in FlightCtrlState signs and the throttle ramp direction (-1, 0, 1). The plugin applies them
only with its opt-in ``HmiCommands`` setting (``disabled`` otherwise), on Unity's main thread, drops commands
older than 1 s and releases held keys 0.5 s after the last one. In the VAB, T / R step the Pi's editor page as
the board's SAS / RCS do. Redux has no public quickload: F9 is logged as not available. While Redux is silent,
keys go to the kRPC fallback exactly as without Redux.

**Orbit report (KSP2ORB1 v1).** In flight the HMI asks ``KSP2ORB1 N`` about once a second (the plugin samples
the report at 2 Hz; ``ReduxOrbitTracker::PERIOD``) and takes only the reply echoing its nonce. The reply is
one line of at most 4096 bytes, single spaces, no trailing space: ``ut body mu px py pz vx vy vz mask``, then
the groups the mask bits select, in this order: 1 maneuver (``ut soi px py pz vx vy vz``, the post-burn state
at the node; ``soi`` <= 0 = unbounded), 2 target (``name isBody radius px py pz vx vy vz``), 4 closest
approach (``ut planned vessel_xyz target_xyz``, only with a target), then ``n`` (0-4) encounters of
``body radius mu soi ut planned cx cy cz px py pz vx vy vz`` (``c`` the moon centre at the SOI entry, ``p``/``v``
the vessel relative to the moon). Names are lowercase hex of UTF-8 (``-`` = empty), flags are ``0``/``1``,
numbers are C# ``R`` invariant text (an exponent such as ``1E-05`` may appear) and always finite; absent
groups are omitted, never zero-filled. ``-`` means no orbit data. ``parse_redux_orbit`` rejects anything
else (token counts, hex, flags, mask or ``n`` out of range, trailing tokens, oversize) and the exchange keeps
waiting, so a bad datagram never ends the source thread. The tracker fills ``orbit_state``,
``maneuver_orbit``, ``target_orbit``, ``approach`` and ``encounters`` of each snapshot from the last good
report while it is younger than 3 s (``STALE``) and for the snapshot's vessel and body; a newer vessel or body,
``-``, leaving flight or losing the link drops it at once, and ORBIT returns to its scalar view. No reply never
affects the ``KSP2HMI`` snapshot. The view axes are fixed per vessel/body from ``r x v``, as in the kRPC source.

The vectors are in Redux's ``UniverseModel.Zup`` frame, inertial and centred on the current body (spin axis
+z by its name; its handedness is not verified at runtime), and the HMI does not convert them: ORBIT only
projects on view axes derived from ``n = r x v``, and for a reflection ``M`` the cross product gives
``(Mr) x (Mv) = -M n``, so the axes follow the mirror and the projected picture is unchanged. The render test
``redux_orbit_report_draws_congruently_in_a_reflected_frame`` proves it: mirroring z or y, or swapping y and z
(kRPC's y-up), draws identical pixels; mirroring x or swapping x and y turns the view plane in itself and keeps
every marker distance. The golden body from the KSP2 workbench's spec is ``hmi-cpp/tests/data/redux_orb1_v1.txt``
(``redux_orb1_maneuver_v1.txt`` the maneuver-only example); kspsim's encoder round-trips both
(``./ci sim --redux-orbit`` answers KSP2ORB1 from the simulated orbit, :doc:`simulator`).
``KSPHMI_REDUX_PORT`` moves the UDP port for loopback tests only.

Settings
--------

``hmi-cpp/config.toml`` (deployed to ``~/ksphmi/cpp``) holds the defaults: kRPC ports and sample rate, ``fps``,
screen size (0 = the monitor's own) and ``day_hours`` (6 for the KSP calendar, 24 for Earth time).
``format.day_hours`` must be a finite positive integer within the C++ ``int`` range; zero, negative,
fractional and out-of-range values are rejected with the configuration filename before the display starts.
The binary finds it through
``KSPHMI_APP_DIR`` (the unit sets ``~/ksphmi/cpp``), else next to or one level above itself, so a build tree
under ``build/hmi-cpp`` uses the repo's file. On the Pi, ``~/ksphmi/config.local.toml``
(``KSPHMI_LOCAL_CONFIG``) overrides any key and is never touched by a deploy; ``~/ksphmi/env`` holds
``KSPHMI_KRPC_ADDRESS``, which ``./ci deploy_hmi`` sets to the PC's mDNS name (``<hostname>.local``)
when the Pi resolves it to the address the SSH session comes from, so a DHCP change needs no
redeploy; otherwise to the PC's address as the Pi sees it, or to ``HMI_KRPC_ADDRESS`` if set. The HMI resolves
the name again at every reconnect (user, 2026-11-16: Mac, PC and Pi reach each other by name). Options: ``--demo --address --windowed --desktop --monitor auto|N --size WxH
--screenshot FILE --seconds S --config FILE --fps N --bench N [--bench-anim] --offscreen --skin NAME
--layer-cache auto|on|off`` (``ksphmi --help``). ``--bench N --bench-anim`` keeps one sample and turns only the attitude, so it times
NavBall-only frames (full frames when the cache is off).

**PC on Ethernet and Wi-Fi (prefer_mac).** A PC connected both ways announces its mDNS name with one address
at a time (2026-11-18: ``Alan-TR.local`` answered its Ethernet ``.17`` and Wi-Fi ``.29`` in turn, ``getent`` on
the Pi returns a single address), so re-resolving the name flipped the endpoint
(``Redux endpoint ... is .29 (was .17)`` and back). ``[krpc] prefer_mac = "aa:bb:cc:dd:ee:ff"`` (or
``KSPHMI_PREFER_MAC``; ``:`` or ``-`` separators, any case) names the PC's **Ethernet** MAC. Then, when the
configured address is a name, the HMI (``hmi-cpp/src/endpoint.cpp``) reads the Linux neighbour table
``/proc/net/arp`` after each resolution, skipping incomplete rows (flags ``0x0``, ``00:00:00:00:00:00``):

#. the address shown with that MAC on the same /24 as the name's answers wins (``(preferred MAC, was ...)``)
   and is remembered;
#. while the remembered Ethernet address is in use and still answers, it stays even when its row expired and
   mDNS points at Wi-Fi (``remembered preferred MAC``);
#. otherwise the address in use stays while it answers (the link worked recently, or the table holds a
   complete row for it) or while the name still returns it (``kept``): equivalent answers never flip it;
#. only when it neither answers nor is returned any more does the HMI move to the name's answer
   (``resolved``), e.g. Ethernet unplugged or a new LAN.

The kernel only learns a MAC after traffic to that address, so the HMI sends one empty UDP datagram (port 9,
discard) to every other answer, and the Ethernet address wins at the next resolution once mDNS has named it.
Without ``/proc/net/arp`` (macOS, Windows), with a numeric address or with ``prefer_mac`` unset the old rule
applies: the resolver's first answer. The rule covers the Redux source and kRPC (``kRPC endpoint NAME is IP
(...)``, also used for the UDP 50002 context requests); a discovered kRPC number gives way to the configured name
as soon as a connection to it fails, so a LAN change never keeps a stale number. The MAC is private to each
installation: never commit it. On the Pi put it in ``~/ksphmi/config.local.toml``::

    [krpc]
    prefer_mac = "aa:bb:cc:dd:ee:ff"

and restart ``ksphmi.service`` (``systemctl --user restart ksphmi``). Not in ``~/ksphmi/env``: every
``./ci deploy_hmi`` rewrites that file with only ``KSPHMI_KRPC_ADDRESS``. ``KSPHMI_PREFER_MAC`` suits a Mac or
PC shell. The log line ``KSP 2 Redux UDP source at ...,
preferring the PC's Ethernet MAC`` confirms it. Reading the MAC: on the PC, PowerShell ``Get-NetAdapter``
(``MacAddress`` of the Ethernet adapter); or on the Pi ``ping -c1 <Ethernet IP>`` then ``ip neigh show``.

On macOS, the launcher opens full screen at the selected display native resolution, with Retina
rendering enabled. The Windows desktop and WSLg behavior below remains platform-specific.

**Running the HMI on the game PC.** ``./ci hmi`` builds the HMI and starts it with ``--desktop``:
``[display] monitor`` (``auto`` by default, or a display index; ``--monitor``) picks the SDL display, and
``auto`` takes the last one when several are connected, so KSP1 or KSP2 keeps the primary monitor
(display 0). On Windows the HMI is built natively with MSYS2 UCRT64 (see "Building on this PC"): a
secondary monitor is filled with an ``SDL_WINDOW_FULLSCREEN_DESKTOP`` window (no video mode change; the
game keeps its own screen), a single monitor gets a plain window, and the game's kRPC server is
``127.0.0.1``. The log line ``display N of M (monitor ...), full-screen desktop`` shows the choice.

Without MSYS2, ``./ci hmi`` falls back to WSL and sets ``KSPHMI_WSLG=1``: there ``--desktop`` always
opens a plain window, because WSLg (2026-11-15, secondary monitor at 175% scale) showed nothing for
full-screen, borderless or maximized X11 windows. WSLg reports that monitor in physical pixels while
Windows places windows in logical units, so the window is anchored at the display origin and ``./ci hmi``
passes the monitor's logical work area minus the title bar (``HMI_TITLE_BAR``, 32) as ``--size``. A new
WSLg window may open behind the active Windows window: click its taskbar icon. WSL uses NAT networking
here, so ``127.0.0.1`` is WSL itself: without ``--address`` or ``--demo``, ``./ci hmi`` targets the Windows
host (the default route's gateway) and exports it as ``KSPHMI_REDUX_ADDRESS`` for the KSP2 Redux backend.
kRPC listens on ``0.0.0.0`` (50000/50001, context UDP 50002) and the Redux endpoint binds UDP 50003 on all
interfaces; Windows Firewall must allow inbound traffic from the WSL subnet. WSLg's multi-monitor support
is limited: the log line ``display N of M`` shows what SDL actually sees. On the Pi a single HDMI display
keeps index 0.

``steampunk`` (approved 2026-11-16) uses generated brass-and-iron artwork, nine-sliced
copper pipe frames, dark petrol enamel, convex-glass instrument faces, an ornate bronze
NavBall bezel, and bundled Cinzel headings / Crimson Pro figures (SIL Open Font License).
The revised reference concept is stored at ``docs/design/steampunk-v2/concept.png``;
it is artwork, not a capture of the running HMI. Ten circular instruments form two rows
of five, with telemetry-driven gold needles and exact numerical readouts underneath.
Each dial has a fixed labelled scale; an out-of-range needle stops at the scale end and
shows ``LIMIT`` without clamping the numerical readout. Missing/non-finite measurements
have no needle, and escaping orbits have no apoapsis/period needle. A compact header
uses amber jewel lamps and a priority warning indicator. Glass sight tubes show vessel
resource fractions, followed by a live altitude/speed history and read-only staging. History keeps up to 180 seconds of received telemetry and resets when
the vessel/body changes or UT goes backwards; before two samples it says it is collecting.
The shared right-column layout keeps ORBIT and VESSEL above the NavBall. Editor and facility
screens use the same artwork. Classic keeps its original appearance; Apollo has its own Flight Deck materials.
Select it with Ctrl+3 or ``--skin steampunk``.

Waiting and generic no-vessel screens share the flight themes' planet materials:
Apollo uses green CRT phosphor; Steampunk uses the colour globe and bronze bezel.
The animated reticle is explicitly labelled ``LINK SEARCH / ILLUSTRATION`` and does
not imply live orbital telemetry. Facility checklist cards use each skin's generated
panel material. Steampunk's changing caution legend is drawn only in the live pass,
with an unavailable-clock placeholder outside flight. Critical warnings remain red,
and VAB destination jewels retain green/red reachability and a circular selection ring.

``KSPHMI_SHOTS=DIR ./ci test_hmi`` now captures the three skins' waiting/no-vessel
screens, every facility, VAB report variants, editor modes and flight cautions. These
are fixture-driven renderer captures, not game sessions; review them together when
changing shared artwork rather than checking only the flight screen.

The source PNGs, runtime BMPs, typefaces, licenses and generation notes live under
``hmi-cpp/assets/`` and travel with ``config.toml`` during deployment. BMP loading uses
SDL2 itself (no new runtime image library). Static art is baked once per size/screen;
normal telemetry frames draw the live values and plots only. Font atlases are keyed by
family and size. See ``assets/steampunk/README.md`` for provenance and demo capture commands.

``[display] skin`` selects ``auto`` (shipped default), ``apollo``, ``steampunk``, ``classic``,
``ksp1`` or ``ksp2``. ``auto`` follows the game that answers (user, 2026-11-16): ``ksp1`` (KSP1 Stock) when
kRPC answers for KSP 1, ``ksp2`` (KSP2 Redux) when the Redux UDP endpoint answers, and ``ksp1`` before any game.
Each source marks its snapshots (``Snapshot::game``); a ``source = "auto"`` HMI switches with the game.

KSP1 Stock and KSP2 Redux themes
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Approved concepts live in ``docs/design/ksp-stock-themes``. ``ksp1`` (Ctrl+4) uses
moulded grey frames, raised/inset controls, green condensed figures, a live mechanical
altitude odometer, resource pictograms and a generated silver/graphite NavBall bezel.
``ksp2`` (Ctrl+5) uses dark navy cards, blue-violet title bands, technical Chakra Petch
lettering, cyan units, green active states and a segmented dark NavBall ring.
Both retain the current orbital physics and show the shared body catalog in colour.
Both use the shared complete stock-style degree grid, with twelve meridians around
the zenith and compass bearings between columns on broken meridians.

The full scene families use their own chrome: flight metrics/resources/history/staging,
link search and no-vessel state, all eight facility checklists, VAB construction, actions,
crew and cargo. Flight-only instruments are absent outside flight. Editor mode strips
show the game-selected mode; the existing keyboard and report service remain unchanged.
The construction report preserves the actual app and mission delta-v, destination
selection, TWR warning thresholds, named stage events and capacity/cost figures.

Skin and game source are independent: either skin works with KSP1 kRPC or Redux UDP.
Resource lists use actual Redux names instead of inventing LiquidFuel/Oxidizer slots;
more than six native resources page at eight-second intervals. Missing, absent and empty
resources remain distinct. Static cache identity includes resource names/page, scene,
editor mode and display size, but never live values or selected action-group contents.
Typeface and orbit hooks are restored on exit, including exceptional exits.

Bundled OFL fonts and generated source/runtime bezels live under ``assets/fonts``,
``assets/ksp1`` and ``assets/ksp2``; exact production prompts are recorded there. Canvas
builds the panel chrome once per cache key. Normal frames update live values/plots and
navigation geometry. Renderer tests capture both skins in every scene family and check
cache reuse, native resource names, keyboard shortcuts and NavBall-only animation.
Apollo Flight Deck
^^^^^^^^^^^^^^^^^^

Apollo Flight Deck (approved 2026-11-16) implements proposal A's light painted aluminum
mounting plates, steel fasteners, charcoal inset modules, smoked instrument windows,
Barlow Condensed headings and green seven-segment readings, combined with proposal B's
black/stainless NavBall bezel. The globe carries the same full markings as Classic and
Steampunk (bearing columns with their pitch ladders, compass labels and parallels; the single local ladder
with sparse meridians of the first version looked incomplete and fanned out with the nose straight up,
user 2026-11-16), quaternion-driven attitude, volume lighting and live markers.
Its side instruments are speed, heading, pitch, roll, throttle and G-force, plus the next
maneuver node's remaining delta-v and time to node (empty without a node), as Classic and Steampunk show it. SAS mode lamps
run along the bottom. Flight warnings use the header's priority caution window:
stale data, surface/atmosphere periapsis, low power, escape, then fuel.

``flightdeck.cpp`` composes the flight screen with equal-width halves and ten metric
modules in two rows. It uses the Apollo profile of ``flight_layout()`` for ORBIT / VESSEL
and the NavBall. Resources remain vessel totals in KSP units; staging is read-only.
The altitude/speed plot records up to 180 seconds of received telemetry, shows a collecting
state before two samples, and resets on a different vessel name/body or UT rewind.
Static art is cached per size/screen. The same materials and heading face extend to
waiting, facility and editor screens. Select it with Ctrl+2 or ``--skin apollo``.

ORBIT retains the inertial view axes, live Cartesian ship position, physically scaled
body radius, conics and maneuver dashes of the shared renderer. Apollo supplies a separate
surface-material callback. All three themes select detailed artwork by the snapshot's
body name from the shared ``assets/bodies`` catalog. Classic and Steampunk show the
colour material; Apollo draws the
whole ORBIT screen as a green phosphor CRT, including the planet, atmosphere, orbital and
maneuver curves, markers and annotations. The maneuver retains dashes and a dimmer green
intensity. Green vector under-strokes supply glow; a size-cached scanline/vignette mask
suggests the tube face. The green planet texture is converted once from source luminance,
with no image readback or per-pixel colour conversion in the telemetry loop.

The catalog covers all 17 original-system bodies, including the Sun, planets and moons.
``Sun`` / ``Kerbol`` and ``Mun`` / ``The Mun`` share their corresponding material. The
cache includes the body key and CRT mode, so changing sphere of influence switches the
image without retaining the previous world. Each cache holds only the most recent body
per colour mode to bound Pi memory use.

Custom systems can supply ``assets/bodies/<key>.bmp`` (lowercase ASCII name; spaces
become underscores). An unknown body without supplied artwork uses a neutral shaded
sphere, never Kerbin geography; path separators and traversal names are rejected. Stock
assets missing from a package produce a clear error. Source PNGs, 512px runtime BMPs,
production prompts and generation notes live in ``assets/bodies``. These are illustrations,
not exact cartographic terrain maps or a live cloud/lighting sensor. Body name, Ap/Pe altitude,
semi-major axis, eccentricity and inclination come from the current snapshot. Material
callbacks, the CRT palette and clipping restore the caller's state on every exit.

Assets, source PNGs, opaque runtime BMPs, font license, generation prompts and capture
commands are recorded in ``hmi-cpp/assets/apollo/README.md``. The approved source renders
are stored under ``docs/design/apollo-flight-deck/`` as concept art, never runtime evidence.
Try ``./ci hmi --demo --skin apollo --size 1920x1080 --seconds 3 --screenshot build/apollo-flight-deck.png``.

The skin can change while the HMI runs: the context reply on UDP 50002 may carry an optional field,
``KSPHMI/1 <scene> skin=<name>``. Only the simulator sends it (its *HMI skin* selector, or ``--skin``);
KSPSerialIO never does, so with the game ``[display] skin`` decides (``auto``: the game's own look). The HMI asks for the context about once a
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

``./ci deploy_hmi`` applies the kiosk boot at its end whenever the deployed ``setup-kiosk.sh`` differs from the
one last applied (``/etc/ksphmi-kiosk.applied`` holds its sha256): it runs the script over ``ssh -t`` and sudo
asks for the Pi user's password. Without a terminal, or with ``NO_KIOSK=1``, it only prints the command. It never
reboots the Pi; do that yourself so the boot changes take effect. By hand (``--undo`` reverts all of it except
lingering):

.. code-block:: bash

   sudo sh ~/ksphmi/cpp/systemd/setup-kiosk.sh && sudo reboot

The script is idempotent and keeps SSH, the network and the serial console. It:

* edits ``cmdline.txt`` (backup ``cmdline.txt.ksphmi-backup``, written once): keeps ``console=tty1`` and adds
  ``quiet loglevel=3 logo.nologo vt.global_cursor_default=0 consoleblank=0 systemd.show_status=false
  rd.systemd.show_status=false plymouth.enable=0``, which keep kernel and systemd text off the screen. Everything
  else (``root=``, ``ds=nocloud``, the Wi-Fi country) is kept. **Never use** ``console=ttynull``: on the Pi 4 it
  hung the boot before the journal was written (black screen with the backlight on, no network, nothing to read
  afterwards; 2026-11-15). The script removes it if an older version added it;
* makes the journal persistent (``/etc/systemd/journald.conf.d/50-ksphmi-persistent.conf``; Raspberry Pi OS keeps
  it in RAM by default), so ``journalctl -b -1`` shows a boot that failed before SSH came up;
* adds ``disable_splash=1`` (no rainbow) and ``avoid_warnings=1`` (no under-voltage icon over the HMI) to
  ``config.txt`` (backup ``config.txt.ksphmi-backup``);
* writes the fbcon ``cursor_blink`` tmpfiles rule and masks ``getty@tty1`` (no login prompt);
* installs ``mpv`` (``apt-get install -y --no-install-recommends mpv``, only when it is missing; without network
  it warns and the boot shows the fallback loading screen until the script runs again). ``/etc/ksphmi-mpv.installed``
  records that the script installed it, so ``--undo`` removes it again;
* installs the udev rule ``/etc/udev/rules.d/99-ksphmi-fb.rules`` (tags only the DRM framebuffer
  ``ATTR{name}=="vc4drmfb"``: the firmware's ``simplefb`` is ``fb0`` from about 4.5 s and is replaced by
  ``vc4drmfb`` at about 6.4 s) and the **system** unit ``ksphmi-splash.service`` (from ``hmi-cpp/systemd/``, with
  the user and home filled in): ``Type=simple``, ``DefaultDependencies=no``, ``After=local-fs.target``,
  ``WantedBy=sysinit.target``, no ``Before=`` on the boot or the logins and no device dependency (with a monitor
  detected late, ``dev-fb0.device`` timed out and the old unit never ran), ``RuntimeDirectory=ksphmi-intro``,
  ``RuntimeMaxSec=660``, ``TimeoutStopSec=3``, running ``~/ksphmi/cpp/systemd/ksphmi-intro.sh`` as the HMI user
  with the groups ``video``, ``render`` and ``audio``;
* enables lingering and warns when ``ksphmi.service`` is not enabled yet (``./ci deploy_hmi`` does that).

What the screen shows at boot: black while the firmware and kernel start (no rainbow, no logo, no text), then the
intro video ``hmi-cpp/assets/intro/KSPKontroller_pi_Intro.mp4`` (10 s, 1276x720 H.264 with AAC sound) once, with
its sound on the HDMI monitor, and its last frame until the HMI's first frame.

``ksphmi-intro.sh`` writes ``/run/ksphmi-intro/starting``, waits up to 20 s for a connected HDMI connector
(``/sys/class/drm/card*-HDMI-A-*/status``; a 4K monitor is often detected late), then starts mpv on the KMS device
(``/dev/dri/by-path/platform-gpu-card``, else ``/dev/dri/card1``) with ``--vo=gpu --gpu-context=drm`` (V3D scales
the video to the monitor's native mode; ``--vo=drm`` would scale on the CPU), ``--profile=fast``
``--hwdec=v4l2m2m-copy`` (first boot, 2026-11-16: with the default high-quality scaling to 4096x2160 and
software decoding the picture ended ~3 s after its sound; with these it ends 0.2 s after), 
``--audio-device=alsa/hdmi:CARD=vc4hdmi0,DEV=0`` (``--audio-fallback-to-null=yes``: no sound card, still the
video), ``--keep-open=always``, ``--no-terminal`` (its log goes to ``~/ksphmi/intro-mpv.log``) and
``--input-ipc-server=/run/ksphmi-intro/mpv.sock``, and removes ``starting`` once the socket exists. Settings in
``~/ksphmi/intro.env`` (read by the unit): ``KSPHMI_INTRO_VIDEO``, ``KSPHMI_INTRO_AUDIO``, ``KSPHMI_INTRO_DRM``, ``KSPHMI_INTRO_HWDEC``,
``KSPHMI_INTRO_VO``, ``KSPHMI_INTRO_LOG``.

Hand-over from the video: mpv is DRM master while it holds the last frame, so SDL's kmsdrm cannot open the display
until mpv quits. Before SDL video starts, ``ksphmi`` (Linux kiosk run only: not ``--splash``, ``--screenshot``,
``--offscreen``, ``--bench``, ``--windowed`` or ``--desktop``) checks ``/run/ksphmi-intro`` (``intro.cpp``): it
waits for ``mpv.sock`` while ``starting`` exists (40 s at most), asks ``eof-reached`` every 0.2 s until it is true
(15 s at most, so the video is never cut unless something hangs), sends ``quit`` and waits up to 2 s for mpv's
socket and the unit's directory to disappear. It logs ``boot intro finished, taking the display`` or a warning with
the reason; any failure lets the HMI go on. The script exits when mpv does (or kills it when ``ksphmi.ready``
appears, or after 10 minutes), and systemd removes the directory. Between mpv's exit and the HMI's first frame the
screen is black for a moment (the console is silent).

.. uml:: diagrams/seq_hmi_intro.puml
   :caption: mpv plays the intro and holds its last frame; the HMI waits for the end, makes mpv quit, then opens
      the display.

Fallback loading screen: without mpv or the video, ``ksphmi-intro.sh`` waits up to 30 s for the DRM framebuffer
(``/sys/class/graphics/fb0/name`` is ``vc4drmfb``) and runs ``ksphmi --splash``, the animated loading screen, until
the HMI has drawn its first frame:

* header: "KSPKontroller", "FLIGHT DISPLAY · RASPBERRY PI" and, top right, ``T+ mm:ss.s`` since power-on
  (``CLOCK_BOOTTIME``, so it includes the firmware and kernel time);
* left: the planet with an ascent trajectory that climbs from the surface to the dashed target orbit as the boot
  progresses (35 % while the system services start, 62 % while waiting for the network, 86 % while the HMI
  launches, 100 % "Orbit reached"), the percentage and a status message;
* right: the BOOT SEQUENCE card, one row per stage with a detail and a status pill (OK, a pulsing "···" for the
  active stage, "--" pending, OFF, FAIL): KERNEL, DISPLAY (``HDMI WxH · /dev/fb0``), SYSTEM (until
  ``multi-user.target`` is active), NETWORK (interface and IPv4 address; "offline" in amber after 40 s, which
  never blocks the boot) and HMI (``ksphmi.service`` waiting, starting, first frame drawn);
* bottom: a five-segment bar and a footer with the host name and "SSH available while booting".

If the HMI has not drawn a frame 45 s after power-on its row turns to FAIL, the percentage reads "HMI FAILED" and
the message shows the command to read its log (``ssh <hostname>  →  journalctl --user -u ksphmi``). The splash
keeps drawing, so a late HMI still takes over. Review every stage without a Pi with
``./ci hmi --splash --screenshot build/splash.png --size 2520x1680 --splash-state STAGE`` (``system``,
``network``, ``offline``, ``hmi``, ``ready``, ``failed``).

The splash redraws about 8 times a second with the software renderer; the static parts are drawn once per size
and kept in a copy, each frame restores and redraws only the animated regions, and the framebuffer write skips
unchanged rows. Probes run on a worker at 2 Hz (``systemctl is-active``, ``getifaddrs``, ``/proc``), never per
frame; no D-Bus library, no extra packages. ``ksphmi --splash`` reads no settings; it re-applies the video mode
(``FBIOPUT_VSCREENINFO``) because with a silent console nothing else may have set it, and it reopens ``/dev/fb0``
if the device node or the driver behind it changes. Without the font it still fills the screen with the HMI
background. Its journal lines (``journalctl -u ksphmi-splash``) give the start time, the network address and a
final ``splash done (HMI ready) ... draw X ms, write Y ms per frame``.

Hand-over: the user unit ``ksphmi.service`` (lingering, ``WantedBy=default.target``, ``Restart=always``,
``StartLimitIntervalSec=0``) deletes a stale ``ksphmi.ready`` at start and writes it right after its first
presented frame, in ``$XDG_RUNTIME_DIR`` (``/run/user/<uid>/ksphmi.ready`` for a system unit without that
variable). The splash runs as the same user, looks for the file every frame and once more right before each
framebuffer write, and exits within about 125 ms, so both never paint at once; it also exits on ``SIGTERM`` and
after 10 minutes, leaving its last picture. The HMI removes the file when it stops cleanly, and a splash started
while the file exists exits at once. The HMI opens ``/dev/fb0`` without clearing it, rewrites every row one second
after its first frame and at each unblank (start, then every 30 s), so neither the splash nor console text can
survive behind its frames. ``./ci verify`` reports each missing piece in ``hmi: kiosk boot``.

.. uml:: diagrams/seq_hmi_boot.puml
   :caption: The fallback boot splash (no mpv or no video) animates until the HMI writes its ready file after the
      first presented frame.

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
#. takes the Pi's deploy lock (``~/ksphmi/.deploy.lock``: one ``deploy_hmi`` at a time, from the Mac or the PC);
#. cross-compiles ``ksphmi`` and its test programs here (``./ci build_hmi_pi``, see :doc:`building`; the Pi never
   compiles), copies them with the ``hmi-cpp/`` files (``config.toml``, ``assets``, ``systemd``, ``tests``,
   sources) to ``~/ksphmi/cpp.next`` and runs the C++ tests and the kiosk and screenshot tests there. Only when
   they pass does it replace ``~/ksphmi/cpp``, keeping the previous binary as ``build/ksphmi.prev``. A failure
   stops here and the running service keeps its old binary. Without a cross compiler it warns and builds on
   the Pi instead (``cmake`` Release, ``-j4``, ``ctest``), which is much slower;
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

With the Xcode command line tools and ``brew install cmake pkg-config sdl2 sdl2_ttf``, ``./ci hmi`` runs the
HMI full screen at the Mac display's native resolution (Esc or ``q`` quits; ``build_hmi`` names exactly these packages when one is missing). ``./ci hmi --demo`` displays simulated data without KSP. Run ``./ci test_hmi`` for native tests;
``./ci hmi --demo --screenshot build/hmi-macos-demo.png --size 1280x800`` saves an off-screen frame.
Native rendering falls back to macOS Arial Bold when the Linux fonts are absent; ``KSPHMI_FONT`` can
select the same font used on the Pi for visual comparisons. Executable-relative configuration lookup
uses the macOS API. Linux fbdev is unavailable on macOS and gives an explicit error; SDL and off-screen
output work. With a scaled Retina mode the size is the full-screen area macOS gives the app in backing
pixels (for example 2940x1846 for "looks like 1470x956" minus the notch band), drawn 1:1 with the
compositor; the boot-splash ready file is Linux only. For the simulator and HMI together in the browser, use ``./ci hmi_desktop``.

Building on this PC
-------------------

On Windows the HMI builds natively with MSYS2's UCRT64 toolchain. Install it once (PowerShell, then the
MSYS2 UCRT64 shell)::

   winget install MSYS2.MSYS2
   pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,SDL2,SDL2_ttf}

When ``C:/msys64/ucrt64/bin/g++.exe`` exists (``MSYS2_UCRT64`` overrides the prefix), ``./ci build_hmi``,
``./ci hmi`` and ``./ci test_hmi`` put ``C:/msys64/ucrt64/bin`` first on ``PATH`` (its compilers and the DLLs
``ksphmi.exe`` loads) and build with CMake and Ninja into ``build/hmi-cpp-win``. pacman may install
``sdl2-compat`` (the SDL2 API on SDL3) for ``SDL2``; the HMI uses only the SDL2 API and works on either.
MSYS2 ships no FreeSans: ``ksphmi`` falls back to ``C:/Windows/Fonts/arialbd.ttf`` unless ``KSPHMI_FONT`` is set,
and ``./ci`` sets it to the Pi's FreeSansBold when a WSL build has unpacked it into ``build/hmi-cpp-deps``.

The port keeps one source tree. ``net.hpp`` / ``net.cpp`` wrap the socket differences (Winsock started once,
``closesocket``, ``select`` for waits, millisecond ``SO_RCVTIMEO``, no ``SIO_UDP_CONNRESET`` errors on the
discovery socket); ``netinfo.cpp`` enumerates adapters with ``GetAdaptersAddresses``; the executable
directory comes from ``GetModuleFileNameW``. The Pi-only parts compile as stubs: ``--splash`` without
``--screenshot`` and the framebuffer fail with a clear message, output ``fbdev`` and ``auto`` draw through SDL,
the boot probes report no systemd and the ready file goes to ``%TEMP%``. CMake links ``ws2_32`` and ``iphlpapi``,
defines ``SDL_MAIN_HANDLED`` and strips ``SDL2main`` and ``-mwindows`` from ``sdl2.pc`` so ``ksphmi.exe`` and
the tests stay console programs with their own ``main``.

Without MSYS2 the same targets re-run themselves inside WSL (``./ci`` prints the install commands above).
Without root there, the SDL2 packages are unpacked into ``build/hmi-cpp-deps`` with ``apt-get download``
(Ubuntu 24.04 names). ``./ci test_hmi --sim`` also starts ``tools/kspsim --no-board --headless`` on
127.0.0.1:50300 and checks that the HMI follows the simulated vessel and saves a frame to
``build/hmi-cpp-sim/flight.png`` (needs ``python3`` with ``krpc`` and ``protobuf``; set ``KSPSIM_PYTHON`` to
choose the interpreter). That end-to-end run is Linux only: on Windows ``./ci test_hmi --sim`` runs the native
tests and then ``./ci test_hmi --sim`` inside WSL.

Tests (``hmi-cpp/tests``, ctest): ``core`` (formatting, Kepler solver, demo source, attitude quaternions and
burn time, label projection and limb fade, front-hemisphere clipping, inertial orbit geometry (RCS cannot move
the vessel ring), resource fractions, gauge geometry and layout, scene replies, discovery address filter and
broadcast, settings layers, protobuf values, the kRPC client against an in-process server, the handshake timeout,
reconnect discovery, launch-pad errors and recovery of temporarily rejected delta-v streams), ``render`` (off-screen layout checks: ten metric tiles below 40 %
of the height, text inside its tile, dials inside the resources panel, ORBIT and NavBall never drawing outside
their panels, apsis labels, facility views, the boot splash, the software frame time) and ``kiosk`` (the boot
file edit of ``setup-kiosk.sh`` and the unit files, when ``python3`` is found).

Orbit projection, orbit tracks and gauge geometry share the plain ``Point2`` coordinate type
from ``vec.hpp``. This avoids GCC ARM ABI compatibility notes for returned
``std::pair<double, double>`` values without disabling compiler diagnostics. Rebuild all
HMI targets together when changing this internal coordinate type.

Integer configuration fields reject non-finite, fractional and out-of-int-range values before
conversion; load errors identify the file and key. Clock telemetry outside the signed 64-bit
range displays ``--``. The core suite checks the exclusive 2^63 boundary and its previous
representable double; the render suite exercises extreme UT in both skins. To check casts
with GCC or Clang on a Linux host with the SDL development packages installed::

   cmake -S hmi-cpp -B build/hmi-ubsan -DCMAKE_BUILD_TYPE=Debug \
     -DCMAKE_CXX_FLAGS="-fsanitize=undefined,float-cast-overflow -fno-sanitize-recover=all" \
     -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=undefined,float-cast-overflow"
   cmake --build build/hmi-ubsan -j4
   ctest --test-dir build/hmi-ubsan --output-on-failure

Measured: about 9 ms per 2520x1680 flight frame on the PC (x86-64, WSL, software renderer).
``ksphmi --bench 20 --offscreen --size 2520x1680`` prints ``HMI_RENDER_MS`` without touching the screen;
``./ci verify`` runs it on the Pi (``hmi: Pi render budget``).

Commands
--------

==========================  ===========================================================================
``./ci hmi --demo``         Build the HMI and run it full screen on this computer with the simulated orbit
``./ci hmi``                Same against KSP on this PC (``--address`` for another one)
``./ci build_hmi``          Build into ``build/hmi-cpp`` (``build/hmi-cpp-win`` with MSYS2 on Windows)
``./ci test_hmi [--sim]``   Build and run ctest; ``--sim`` adds the run against ``tools/kspsim``
``./ci build_hmi_pi``       Cross-compile it for the Pi into ``build/hmi-cpp-pi`` (``./ci hmi_sysroot`` first)
``./ci deploy_hmi``         Cross-build, test the binaries on the Pi in ``~/ksphmi/cpp.next``, swap them into
                            ``~/ksphmi/cpp``, restart the ``ksphmi`` user service, print its log
``./ci hmi_logs [N]``       Last N lines of the service log on the Pi
``./ci verify``             ``hmi:`` lines: views and resources, local test build current, service state and
                            unit, kRPC address still this PC, screen, cursor, kiosk, Pi render budget
==========================  ===========================================================================

``build_hmi_cpp`` and ``test_hmi_cpp`` are old names of ``build_hmi`` and ``test_hmi`` and still work.
``ksphmi --demo --size 2520x1680 --screenshot out.png`` renders a frame without any window, which is how
screen changes are reviewed (``build/hmi-cpp-win/ksphmi.exe`` with ``C:/msys64/ucrt64/bin`` on ``PATH``
on Windows; inside WSL with the environment ``./ci`` sets up: ``hmi_env`` in ``ci``).
