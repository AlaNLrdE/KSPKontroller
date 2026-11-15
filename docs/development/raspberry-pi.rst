Raspberry Pi HMI development
============================

Scope and build graph
---------------------

``hmi-cpp`` is the production HMI, implemented in C++17 with SDL2 and SDL2_ttf. It reads KSP through
the kRPC mod and the KSPSerialIO UDP context responder. It is read-only with respect to vessel controls.
Python remains in tests and development tools; there is no Python/pygame production HMI runtime.

.. uml:: ../diagrams/components_hmi.puml

``CMakeLists.txt`` separates ``ksphmi_core`` (model support, geometry, configuration, network/client and
sources), ``ksphmi_ui`` (SDL drawing, skins, output and PNG) and the ``ksphmi`` executable. Unit tests
link the core; rendering tests link the UI; optional Python kiosk tests inspect boot-file transformations.
Although core does not link SDL, the current top-level CMake configuration still requires the SDL
development packages to configure the project.

Source map
----------

.. list-table:: Modules and responsibilities
   :header-rows: 1
   :widths: 26 44 30

   * - Module
     - Responsibility
     - Boundary to preserve
   * - ``main.cpp``
     - CLI parsing, source/output lifetime, SDL events, frame cadence and benchmark/screenshot modes.
     - Rendering resources stay on this thread.
   * - ``config.hpp`` / ``config.cpp``
     - Supported TOML-like settings, application-directory lookup and environment overrides.
     - This is a small project parser, not a full TOML implementation.
   * - ``net.hpp``, ``netinfo.*``, ``discovery.cpp``, ``scenes.*``
     - Socket RAII, IPv4 lookup, interface information, broadcast discovery and UDP context.
     - Source socket waits stay off the render thread. ``NetInfo`` uses a worker for DNS/interface lookup;
       ``line()`` reads cached text only, and destruction signals shared worker state without joining DNS.
   * - ``pb.*`` / ``krpc.*``
     - Protobuf wire codecs, paired TCP connections, procedures and stream-result table.
     - Protocol errors and transport errors remain distinct.
   * - ``telemetry.*`` / ``model.hpp``
     - Live/demo sources, stream groups and immutable display snapshots.
     - No SDL calls from telemetry workers.
   * - ``vec.*``, ``navmath.*``, ``orbitview.cpp``
     - Quaternions, frame transforms and conic projection/geometry.
     - Explicit frame and unit conventions.
   * - ``canvas.*`` / ``fmt.*``
     - Primitives, fonts, texture/clip helpers and value formatting.
     - Restore rendering state; centralize units and negative-zero handling.
   * - ``display.*`` / ``apollo.*`` / ``segments.*``
     - Screen composition, skins, cached instrument art and digital readouts.
     - Consume snapshots without network calls.
   * - ``navball.*`` / ``gauges.*``
     - Navigation globe, marker projection, smoothing and gauge geometry.
     - Cache static work; distinguish orientation from speed reference frame.
   * - ``fbdev.*``, ``png.*``, ``splash.*``, ``bootstate.*``
     - Linux framebuffer transfer, PNG artifacts, the animated boot splash and its pure boot-stage logic
       (``boot_view``) with the probes and the ``ksphmi.ready`` hand-over file.
     - Hardware output differs from off-screen testing. Probes run on a worker at 2 Hz; drawing only reads
       ``BootView``. ``SplashPainter`` must keep animated drawing inside ``Layout::dynamic()`` rectangles.

Sources, state and concurrency
------------------------------

.. uml:: ../diagrams/classes_hmi.puml

``Source`` owns a worker and publishes a synchronized ``shared_ptr<const Snapshot>``. Consumers keep
a stable sample while another sample is built. ``latest()`` returns the newest published object; it
does not wait for a fresh network response or offer a history of every sample.

``KrpcSource`` runs connection/retry and vessel sessions. ``krpc::Client`` additionally owns a stream
reader that records the latest encoded result per stream ID. The source thread performs synchronous
RPC calls and decodes/assembles snapshots; the render thread only observes published state. Shutdown
sets stop flags, closes active sockets to wake blocked work and joins workers through their lifetimes.

.. uml:: ../diagrams/state_hmi.puml

``Connecting`` means no working live session yet. ``NoVessel`` is a connected non-flight condition
and can carry facility context; a null active vessel is not a broken connection. ``Flight`` carries
vessel data. A stale flight snapshot (default age greater than 3 s) is a quality condition, not an
additional enum member. The display can indicate stale data while the source diagnoses/reconnects.

``DemoSource`` generates an accelerated Kepler orbit and navigation/systems data locally. It is useful
for graphics and numerical checks but does not exercise sockets. Use kspsim for real client protocol
coverage. :doc:`../communication` describes the connect sequence and error types.

Telemetry groups and invalidation
---------------------------------

Mandatory session data identifies the vessel and its orbit/body. Navigation and systems add optional
markers, resources and staging. Missing target, a singular surface speed frame or unavailable stage
delta-v must suppress only the affected value; optional setup retries later rather than recreating
every stream on every sample. Transport failures still end the connection and trigger reconnection.

Resource values are vessel totals in KSP units. Zero capacity means absent, zero amount with capacity
means empty, and an unavailable optional value is unknown. Stage/resource collections must be rebuilt
after stage or topology changes such as docking and decoupling. Vessel names are live values, not
immutable labels fetched once at connection time.

Maneuver rendering uses the node's post-burn orbit sampled at the node time in the body's non-rotating
frame. Node key changes and periodic refresh invalidate its cached state. It does not estimate the
post-burn trajectory from a screen-space arrow.

Navigation conventions
----------------------

Model distances are metres, speeds m/s and times seconds. Snapshot inclination is degrees and true
anomaly is radians. The serial protocol's fixed-point angles are not used by this model.

The attitude quaternion maps vessel axes (right, forward, down) into surface axes (up, north, east).
NavBall speed markers use the selected orbit/surface/target **speed** reference frame, not a vessel
co-moving orbital frame that would make the measured speed zero. Keep these conventions next to new
vector math and exercise limiting attitudes, roll and hidden-hemisphere clipping.

ORBIT uses Cartesian vessel position/velocity in the body's non-rotating frame with view axes fixed
for the vessel/body. A small RCS impulse in a nearly circular orbit can rotate apsides without moving
the spacecraft; therefore neither camera nor live marker is anchored to periapsis direction. Curves,
labels and markers are clipped to the panel interior, including hyperbolic or distant geometry.

Frame pipeline and caching
--------------------------

.. uml:: ../diagrams/activity_hmi_frame.puml

Each frame processes SDL input, reads the newest snapshot, chooses its skin/screen, draws and presents.
``Canvas`` scales layout with screen height. Fonts/glyph atlases and static instrument art are cached;
needles, live values, orbit markers and the navigation globe update dynamically. ``Canvas::polygon``
accepts convex polygons: split concave icons and ring sectors into convex pieces.

The optional layer cache stores a full frame and the background under the globe; ``animate()`` can
then redraw only the NavBall between telemetry samples. Any changed data/skin/screen requires a full
draw. ``Canvas`` wrappers restore target/clip state because SDL state is target-dependent. The default
configuration disables this cache: measurements on the Pi GPU found it slower than a full redraw.
Do not infer target performance from CPU rendering in Docker.

Outputs and service lifecycle
-----------------------------

* SDL window mode runs on a development desktop; on the Pi the service selects kmsdrm and requests
  automatic output, trying the accelerated path before fbdev fallback.
* fbdev renders to an ARGB8888 software surface and copies changed rows to Linux ``/dev/fb0`` in its
  supported format. It unblanks the display at startup and periodically; console cursor behavior is
  a separate OS configuration concern.
* Off-screen mode uses software surfaces for tests, benchmarks and PNG screenshots, without a monitor.
  It cannot prove mode setting, scanout or physical visibility.
* macOS supports SDL and off-screen rendering, system font fallback and native executable lookup.
  Selecting Linux fbdev there raises an explicit error.

The Pi user unit ``ksphmi.service`` executes ``~/ksphmi/cpp/build/ksphmi`` and restarts after process
exit. A separate system splash service from ``setup-kiosk.sh`` animates the boot until the HMI writes its ready
file after the first presented frame (see :doc:`../hmi`).
Lingering allows the user service to start at boot. Root-required kiosk changes are run by the user.
Use ``ssh ksphmi`` (configured locally); do not embed the Pi's address or account in source/docs.

Configuration layers and operations
-----------------------------------

The deployed defaults live in ``~/ksphmi/cpp/config.toml``. The unit points
``KSPHMI_LOCAL_CONFIG`` at ``~/ksphmi/config.local.toml`` for persistent user overrides, and loads
``~/ksphmi/env`` for the deploy-provided server address. ``KSPHMI_APP_DIR`` fixes application-directory
lookup. Explicit CLI switches override their corresponding effective options; a recognized runtime
context skin can override configured skin selection. See :doc:`../hmi` for the full CLI and precedence.

Build and test on a supported host:

.. code-block:: bash

   ./ci build_hmi
   ./ci test_hmi
   ./ci hmi --demo --size 1280x800 --screenshot build/hmi-review.png
   ./ci test_hmi_container
   ./ci hmi_desktop

Windows native targets run through WSL. Docker targets use their own Linux environment; Windows
Docker usage has not been validated in this session. Stop a desktop before automated tests sharing
its architecture's build directory. The complete verification matrix is in :doc:`../testing`.

Adding a displayed value
------------------------

#. Define its units, reference frame and unavailable state in the model.
#. Add the call/stream to telemetry, using optional failure isolation when appropriate.
#. Add the consumed procedure to kspsim and make the value controllable in the test app.
#. Format centrally and render in the appropriate panel without per-frame RPC or expensive static work.
#. Add meaningful value/geometry or clipping tests; inspect screenshots at supported sizes and both skins.
#. Run the actual C++ client against the stand-in. Use real KSP for API behavior the simulator cannot
   establish, and the real Pi for driver behavior and performance.
