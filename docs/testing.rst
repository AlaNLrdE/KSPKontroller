Testing and validation
======================

Use the smallest environment that exercises the changed behavior, then cross the next boundary only
when needed. Build success, simulation success, deployment and physical confirmation are separate facts.

.. uml:: diagrams/testing_boundaries.puml

Verification matrix
-------------------

.. list-table:: What each check proves
   :header-rows: 1
   :widths: 22 20 32 26

   * - Command
     - Needs
     - Covers
     - Does not establish
   * - ``./ci verify``
     - Python/tool dependencies; optional local installs
     - Layout/config/protocol consistency, available build/log evidence, simulator checks.
       Install checks use ``KSPDIR``; without it, the shared Windows install, or the first ``./ci ksp_list``
       entry on macOS/Linux. Without any install they are skipped with WARN; firewall checks are Windows only.
     - A full live game or physical display test.
   * - ``./ci build_sketch``
     - Arduino CLI and board core
     - Firmware compile for configured board.
     - Flashing or wiring.
   * - ``./ci build_KSPSerialIO``
     - .NET build tools and game references
     - C# assembly compatibility at compile time.
     - Unity execution or other-mod interactions.
   * - ``./ci test_sim``
     - Python and simulator dependencies
     - Simulated physics/state, serial codecs and stand-in kRPC procedures.
     - Running the actual mod inside KSP.
   * - ``python -m unittest discover -s tools -p test_ci_deploy.py``
     - Python and Bash; no hardware or network
     - Three-project deployment ordering, Pi preflight failures, HMI build failure, ``NO_TEST``, clean reference paths and WDT metadata overrides that preserve source bytes and compiler arguments.
     - Actual compilation, SSH availability, service restart or flashing. All such operations are stubbed.
   * - ``./ci test_hmi``
     - Native/WSL C++ toolchain, SDL, fonts
     - Core, off-screen render and kiosk transformation tests.
     - Real framebuffer or boot behavior.
   * - ``./ci test_hmi --sim``
     - Above plus Python kRPC dependencies
     - Real C++ client follows a simulated vessel and renders a PNG.
     - Real kRPC mod quirks not represented in the stand-in.
   * - ``./ci test_hmi_container``
     - Running Docker engine
     - Linux build, ctest and client integration in the chosen architecture.
     - Pi kernel/drivers or timing.
   * - ``./ci test_hmi_desktop``
     - Running Docker engine
     - Real Tk controls, UDP skin application in C++, throttle over kRPC, HTTP/WebSocket/VNC path.
     - Human readability or physical display output.
   * - ``./ci sim --no-board``
     - Python GUI environment
     - Interactive values and scenarios served to an HMI.
     - Board behavior.
   * - ``./ci hmi_desktop``
     - Docker and browser
     - Interactive test app and actual HMI together through noVNC.
     - Hardware performance or real KSP execution.

Firmware native regression tests
--------------------------------

``python3 tools/verify.py --firmware-only`` compiles and runs the actual ``SerialCOMS.ino``, ``Input.ino``
and ``HandShake.ino`` functions with fake serial, clock and indicator peripherals. Packet declarations
are extracted from the production sketch, including their size assertions. Tests cover incomplete telemetry
expiring after two seconds, the immediate next handshake (including bytes queued before expiry is checked),
and ordinary fragmentation below the timeout. Header tests deliver ``00 00 00 BE EF`` before the remainder,
split a handshake at every byte boundary, deliver all three packet types byte by byte, preserve ``BE BE EF``,
and check recovery after bad checksums, zero/mismatched lengths and expired partial headers. The fake serial
port counts empty-buffer reads and requires zero. Unknown-ID tests first receive each supported packet type,
then send a checksum-valid ID 99 frame of the same length: the previous structure and link timestamps
must remain unchanged, only the invalid counter advances, and a following handshake must work.
Recovery tests receive each supported packet type at 100 ms without drawing the matrix, disconnect before
the splash ends, and check that a reset is requested only after 30 seconds of silence. The reset call is a
counting stand-in; never-connected and invalid-packet-only cases must not arm link recovery.
Requires a native C++17 compiler; artifacts stay under
``build/firmware-tests``. The same command runs ``matrix.ino`` data-page tests with UBSan (including
float-to-int overflow checks): infinity and NaN show three dashes, finite extremes saturate before conversion,
and ordinary numbers retain decimals and unit dots. Matrix tests require a compiler with UBSan support;
the LED driver and GPIO are stand-ins. These tests do not validate physical USB, watchdog, LEDs or buttons.

Plugin receive regression tests
-------------------------------

After ``./ci build_KSPSerialIO``, run ``python3 tools/verify.py --plugin-only`` for the button tests and
``tools/plugin-receiver-tests``. The latter loads the actual built plugin and reference assemblies, invokes
the real receive and dispatch methods through reflection, and keeps logging queued without Unity lifecycle
or UI calls. It checks the ID-only control regression after a handshake, short and oversized control and
handshake payloads with correct checksums, preservation of pending valid packets, button events and
handshake state, and successful recovery with valid payloads. Requires .NET 8; stale plugin output fails
rather than testing an older assembly. Artifacts stay under ``build/plugin-receiver-tests``. This does not
validate Unity control application, serial hardware or the game.

``--plugin-only`` also runs ``tools/plugin-connection-tests`` against the real plugin assembly. Its
separate process substitutes test-only serial and KSP configuration assemblies and verifies those bindings
before invoking discovery methods; no operating-system serial port is opened. Tests cover busy/absent
configured ports in initial and supervisor modes, stale/other candidates, no-handshake success without a
probe, empty configuration, normal verified fallback and cancellation. Every test stops the plugin's reader
and sender workers. The stand-ins remain under the test project/build directories and are never deployed.

Ship report format tests
------------------------

``./ci verify`` (``check_ship_report_contract``) builds and runs ``tools/plugin-shipreport-tests`` with the .NET 8
SDK: it compiles the plugin's ``ShipReportFormat.cs`` without KSP or kRPC and requires its output for a sample ship
to equal ``hmi-cpp/tests/data/ship_report_v1.txt`` byte for byte (under a comma-decimal culture), plus escaping,
the pending report and revision rules. The same fixture is parsed by ``ship_report_parses_the_shared_sample``
(ctest) and written by kspsim's formatter (``ShipReportTest``), so the three implementations cannot drift apart.
The check also requires the service name, procedures, Editor scene and version to agree in plugin, HMI and
simulator, and ``KSPSerialIO.dll`` to stay free of kRPC. The rest of ``KSPSerialIO.Hmi.dll`` (KSP events and
``VesselDeltaV``) is only verified in the game: compare the Pi's numbers with KSP's delta-v app in the VAB.

Button event regression tests
-----------------------------

``./ci verify`` runs the standalone C# tests in ``tools/button-events-tests`` when the .NET 8 SDK is
available. They compile the production ``ButtonEvents`` source without Unity and cover a complete
STAGE pulse before consumption, repeated SAS/RCS presses, held-button baselines, reconnect reset,
receive timestamps and concurrent capture/consumption. Artifacts stay under ``build/button-events-tests``.
They do not validate Unity action group behavior or the physical serial link.

HMI test layers
---------------

``tests/test_core.cpp`` checks formatting, protobuf/client behavior, configuration, geometry, source
reconnection and optional telemetry failures using in-process test servers. ``tests/test_render.cpp``
checks panel bounds, text placement, marker/orbit geometry, skin switching and cache composition with
off-screen SDL rendering. ``tests/test_setup_kiosk.py`` exercises boot-file edits and service files;
it does not reboot a Pi or execute its display stack.

The ``core`` suite needs loopback sockets. If a sandbox blocks bind/connect, granting access to local
test sockets is an environment requirement, not evidence of a product connection bug. Render tests
need an actual font; ``KSPHMI_FONT`` can select one. Native macOS may use Arial Bold, so screenshots
can differ from Pi/Linux fonts even when layout tests pass.

Docker workflows and artifacts
------------------------------

.. code-block:: bash

   ./ci test_hmi_container
   ./ci test_hmi_desktop
   ./ci hmi_desktop

Default ``HMI_PLATFORM`` is ``linux/arm64``. ``linux/amd64`` can speed up x86 development;
``linux/arm/v7`` is an option when checking a compatible 32-bit environment. An ARM image is not proof
that the Pi's installed OS uses the same architecture: match the actual target. Emulation support is
required when the selected architecture differs from the Docker host.

Automated containers have no external runtime network and keep protocol peers on loopback. Image
builds download dependencies. The interactive desktop publishes only host ``127.0.0.1:6080``. Source
is mounted read-only and copied into a stable internal path; build output is a writable bind mount.
Package versions are resolved at image build time rather than fully locked.

Output is under ``build/hmi-container/<platform-with-dashes>``:

* ``hmi-cpp/Testing/Temporary``: ctest logs.
* ``hmi-cpp-sim/flight.png``, ``hmi.log`` and ``kspsim.log``: headless integration artifacts.
* ``desktop/hmi.log``, ``simulator.log`` and desktop-service logs: graphical session diagnostics.
* ``desktop/smoke.json``: GUI/transport test result; verification marks results older than relevant
  source changes as stale. Starting an ordinary desktop is not equivalent to passing this test.

The desktop and tests share build directories by architecture. Stop ``ksphmi-desktop`` before testing
that architecture. Repeating ``./ci hmi_desktop`` preserves an already running session and prints its
URL; stop and start again after editing source to recompile. See :doc:`hmi` for exact commands.

Hardware and game sessions
--------------------------

Board tests can exercise handshake, corrupted packets, keepalives, LEDs, matrix and reconnect while
KSP is closed. Firmware uploads require explicit approval and a fresh compile. Keep the known board
core limitations and diagnostic counters from :doc:`firmware` visible when investigating a hang.

For game testing, the user launches KSP. Verify requirements first, coordinate any shared-install
deployment, and save logs with ``./ci save_log LABEL`` before the next launch. Compare real behavior
with simulator assumptions and add a regression when a real API edge case is discovered. Never
describe a simulator-only run as a successful game session.

For Pi testing, inspect service restart counts, output selection, unblank state and logs, then ask
the user what the physical screen shows. A running service or saved PNG cannot prove that the display
is visible. Measure render optimizations on the target using the same resolution and output path.

Documentation checks
--------------------

``./ci diagrams`` renders every PlantUML source. ``./ci sphinx`` builds the Sphinx site with warnings
treated as errors and embeds Doxygen when available. ``check_documentation`` in ``tools/verify.py``
checks required guides, UML references, balanced sequence activations and API input coverage without
requiring Java or a game install. Run just that check with ``python3 tools/verify.py --docs-only``.
See :doc:`documentation` for the authoring workflow.
