KSP mod development
===================

Scope and artifacts
-------------------

``KSPSerialIO`` is a C# .NET Framework 3.5 plugin loaded by KSP's Unity/Mono runtime. The main source
is ``KSPSerialIO/KSPSerialIO/KSPIO.cs``; ``HmiContext.cs`` adds the independent UDP scene service.
The project builds ``KSPSerialIO.dll`` and uses ``PsimaxSerial.dll`` for serial I/O. ``config.xml``
is the deployable settings template. :doc:`../plugin` documents its keys and diagnostic log formats.

The kRPC mod is a separate dependency for the Pi path. Adding HMI telemetry generally changes the
C++ kRPC client, not the serial ``VesselData`` structure. KSPSerialIO's UDP context fills a scene
identity gap; it does not forward vessel telemetry to the HMI.

``KSPSerialIO/KSPSerialIO.Hmi/`` builds a second artifact, ``KSPSerialIO.Hmi.dll`` (.NET 4.7.1, installed in
``GameData/KSPSerialIO/Plugins``): the kRPC service ``KSPHMI`` with the editor ship report for the Pi's VAB
screen. It is the only code that references kRPC; keep it that way, so a missing kRPC disables only that DLL.
:doc:`../plugin` describes its events, data sources and build constraints.

Components and lifecycle
------------------------

.. uml:: ../diagrams/classes_plugin.puml

``SettingsNStuff`` initializes settings at the main menu. ``KSPSerialPort`` owns serial connection
state and workers in flight. ``KSPSerialIO`` samples vessels and applies controls in flight.
``HmiContext`` persists from the main menu, tracks facility events and services UDP on the main
thread. ``KSPLogQueue`` marshals worker log messages back to Unity's thread.

#. ``KSPSerialPort.Awake`` stops an obsolete supervisor, resets static link flags, initializes or
   recreates the port, attempts initial discovery where necessary, and starts supervision.
#. ``KSPSerialIO.Start`` opens the selected port if necessary, hooks ``AxisInput`` and synchronizes
   level-based switches. Initial loading can include synchronous waits.
#. Per-frame ``Update`` runs packet dispatch, guarded telemetry sampling and control application.
   A late reconnect idempotently attaches the axis hook to the active vessel.
#. Vessel changes move control handling to the current vessel; settings are not reloaded per packet.
#. Flight destruction closes the port and stops link supervision. Static state outlives individual
   scene components, so reset/cleanup cannot rely on instance destruction alone.
#. ``HmiContext.OnDestroy`` removes every subscribed facility event and closes its UDP socket.

Thread model and bounded queues
-------------------------------

.. uml:: ../diagrams/plugin_threads.puml

Game objects and control APIs belong to Unity's thread. The reader, sender and supervisor must not
call arbitrary Unity APIs. Worker diagnostics go through ``KSPLogQueue``. The reader serializes parser
access by issuing one read at a time and hands completed packets over under ``SerialMutex``. The
handshake path can handle a reply before normal per-frame dispatch has started.

``TrySendPacket`` constructs a frame on the caller and replaces the pending frame under ``sendLock``.
``SenderWorker`` performs the potentially blocking write. A newer telemetry frame supersedes an older
unsent frame; this prevents an increasing backlog from representing an old vessel state. The handshake
uses synchronous transmission during connection establishment instead of that steady-state queue.

The supervisor performs the retry loop outside normal frame processing. It uses a generation/stop
mechanism to invalidate previous supervisors. Port/worker replacement happens while the link is down.
Review locks, stop signals and cleanup paths together whenever changing connection ownership.

Telemetry sampling
------------------

``KSPSerialIO.Update`` sends only with an active vessel and ``LinkReady``. The configured refresh
interval limits expensive sampling; it is not the physics update frequency. Sampling reads orbital,
surface, resource, action-group and navigation state into packed ``VData``. Resource aggregation is
one of the costs that depends on vessel parts. Angle units differ from native KSP values where the
wire contract explicitly uses fixed-point degrees times 50.

Treat :doc:`../vesseldata` as the field/unit reference. A change to packing, field order or numerical
scale is a protocol change even if a C# consumer still compiles. Simulator encoders and the Arduino
must match it byte for byte.

Control application
-------------------

The receive parser validates the exact payload size (25 bytes for controls, 4 for handshakes) before
capturing events or copying into ``NewPacketBuffer``. A valid checksum alone does not make a truncated
packet safe: the shared buffer retains old bytes, and struct decoding reads its entire fixed size.
Diagnostics follow their own validated length check and do not enter this shared buffer.

``VesselControls`` decodes the latest control packet into values convenient for the game. The main
thread compares current and prior values before applying changes. SAS/RCS are event-like bit changes;
lights, gear and other level switches use their new state. The initial control packet establishes a
baseline, so entering flight or reconnecting does not manufacture a button press.

The reader captures STAGE rising edges and both SAS/RCS bit transitions in ``ButtonEvents`` before
replacing ``NewPacketBuffer``. Its synchronized queue preserves multiple presses during a stalled frame;
Unity drains the events in order, independently of the latest analog values. STAGE events retain their
receive timestamp, and only requests up to one second old activate the next stage. Reconnect and flight
initialization clear pending events and reset the receive baseline, preventing a held startup button
from staging. Do not turn STAGE into an unconditional action on every packet whose bit is set.

``AxisInput`` runs through ``OnPostAutopilotUpdate`` and merges controller axes into ``FlightCtrlState``
using the per-axis enable mode. Mode zero preserves KSP input; modes one to three choose or combine
controller and game values as documented in :doc:`../plugin`. Wire axis units are normalized during
decoding. Disabled panel axes can still be fields in the protocol without controlling the vessel.

Connection supervision
-----------------------

When ``HandshakeDisable`` is 1, ``BuildCandidates`` returns only the non-empty configured ``DefaultPort``
without reading the registry or enumerating devices. ``TryConnect`` independently rejects other names
before constructing a serial port. This applies to initial discovery and supervisor retries; a missing or
busy configured port remains a failed attempt, while normal handshake-verified scanning is unchanged.

``LinkReady`` requires link-up, a compatible controller, an open port and no write failure. Three
consecutive failed writes mark the link failed. Supervision can also detect silence or a closed port.
The normal silence threshold is 3000 ms, retry delay 2000 ms, candidate settle delay 300 ms, handshake
reply wait 1500 ms, and scene-start grace 10000 ms. Sending-gap tracking avoids confusing a pause in
host telemetry with a controller that failed to answer an active sending run.

Candidate order prefers the last known port and configured default among present ports, then other
eligible names. Bluetooth modem ports are skipped where registry metadata identifies them. The list
is rebuilt on retries because a reset can change USB enumeration. The compatibility handshake, not
the COM number, establishes board identity. See :doc:`../protocol` for the reconnect state diagram.

Editor ship report service
--------------------------

``EditorShipReport`` (``KSPAddon.Startup.EditorAny``) subscribes in ``Start`` and unsubscribes every event in
``OnDestroy``, which also clears ``ShipReportCache``. It reads KSP objects only inside event handlers on the main
thread; ``HmiService``'s procedures run in kRPC's update and only read the cache under its lock, so the RPC path
never touches ``ShipConstruct`` or ``VesselDeltaV``. A failed build of the report is logged once per distinct
message and leaves the previous report in place. ``ShipReportFormat`` holds the text format (version 1) and has no
game types; change it together with ``hmi-cpp/src/shipreport.cpp``, ``tools/kspsim/ships.py`` and the shared
fixture ``hmi-cpp/tests/data/ship_report_v1.txt``, and bump ``v`` for anything an old Pi would misread.

.. uml:: ../diagrams/seq_vab_report.puml
   :caption: Editor ship report from editor events to the Pi display.

Scene context service
---------------------

``HmiContext`` binds UDP 50002 with a non-blocking socket. It accepts only the exact ASCII request
``KSPHMI/1`` and handles at most four pending datagrams per frame. Facility spawn/despawn subscriptions
distinguish Mission Control, Astronaut Complex, Research and Administration from the surrounding
Space Center scene. Editor facility selects VAB or SPH. It replies to the requesting endpoint.

This service is independent of serial discovery and the flight-only components. A missing Arduino
does not disable it. A bind failure is logged and leaves context unavailable rather than preventing
the game from running. The service is read-only and does not implement HMI skin control; only the
simulator adds that extension. The complete contract is in :doc:`../communication`.

Build, deployment and diagnosis
-------------------------------

.. code-block:: bash

   ./ci build_KSPSerialIO
   ./ci verify
   ./ci test_sim

The build needs the game's/Unity's reference assemblies and the serial dependency; see
:doc:`../building`. Builds and simulator tests do not prove the plugin works inside Unity. For an
actual deploy, close KSP, coordinate the shared installation, verify before/after and change only
``GameData/KSPSerialIO``. Every deployed plugin change requires matching assembly/changelog versions.

Use ``[PORT]`` for serial setup, ``[CTRL]`` for applied controls, ``[BOARD]`` for firmware diagnostics,
``[LINK]`` for supervision and ``[HMI]`` for scene-service failures and editor ship report changes. Performance lines distinguish
main-thread queueing from worker write duration. Save game logs before a new launch overwrites them.

When extending the mod
----------------------

* New physical controls need a documented edge/level policy and a safe startup/reconnect baseline.
* New serial fields need coordinated C#/firmware/simulator layout changes and field documentation.
* New facility events need symmetrical subscribe/unsubscribe, simulator support and an HMI view.
* New ship report fields go into all three implementations and the shared fixture; kRPC types stay in
  ``KSPSerialIO.Hmi``.
* New worker behavior needs explicit ownership, bounded work, shutdown behavior and main-thread logging.

Keep tests at the correct boundary: kspsim validates compatible protocol peers, but runs a stand-in
server rather than the C# assembly in Unity. Actual KSP lifecycle, other mods and action-group side
effects require a user-run game session after the lower-level checks pass.
