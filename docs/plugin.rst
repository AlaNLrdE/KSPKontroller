KSP plugin
==========

For component ownership, synchronization and extension guidance, see :doc:`development/plugin`.
The channel inventory and UDP scene-context contract are in :doc:`communication`.

Lifecycle
---------

#. **Main menu**: ``SettingsNStuff.Awake`` loads ``config.xml``.
#. **Flight scene load**: ``KSPSerialPort.Awake`` looks for the controller (see the discovery
   diagram in :doc:`protocol`). If it was already found in an earlier flight, only the port object
   and the reader thread are recreated.
#. **Flight scene start**: ``KSPSerialIO.Start`` opens the port, hooks ``AxisInput`` into the
   active vessel and pushes the current switch state to it.
#. **Every frame**: ``KSPSerialIO.Update`` (below) and ``KSPSerialPort.Update``.
#. **Scene unload**: ``KSPSerialIO.OnDestroy`` calls ``PortCleanup`` to stop the reader thread
   and close the port.

.. uml:: diagrams/activity_plugin_update.puml

Settings (``config.xml``)
-------------------------

.. list-table::
   :header-rows: 1
   :widths: 25 15 60

   * - Key
     - Type
     - Meaning
   * - ``DefaultPort``
     - string
     - Port tried first, for example ``COM3``. Other ports are then scanned.
   * - ``BaudRate``
     - int
     - Serial baud rate. It must be 115200 to match the sketch: the UNO R4 did not answer at other rates.
   * - ``refresh``
     - double
     - Minimum seconds between telemetry packets.
   * - ``HandshakeDelay``
     - int (ms)
     - Wait after opening a port before talking, for boards that reset on open.
   * - ``HandshakeDisable``
     - int
     - ``1`` skips the handshake and trusts only ``DefaultPort``, for both initial connection and retries.
       An empty, busy or absent configured port never falls back to another device.
   * - ``PerfLog``
     - int
     - ``1`` logs the plugin's cost per frame every 10 s (see below). Default 0 when the key is missing.
   * - ``SASTol``
     - double
     - Obsolete since 0.19.3. It is still read so existing files load, but SAS is no longer switched off while steering.
   * - ``PitchEnable``, ``RollEnable``, ``YawEnable``, ``TXEnable``, ``TYEnable``,
       ``TZEnable``, ``WheelSteerEnable``, ``ThrottleEnable``, ``WheelThrottleEnable``
     - int
     - Axis mode, see below.

Axis modes
~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 10 90

   * - Value
     - Behavior
   * - 0
     - Always use KSP's own value; the controller is ignored.
   * - 1
     - Always use the controller's value.
   * - 2
     - Use KSP's value unless it is zero, then use the controller's.
   * - 3
     - Use the controller's value unless it is zero, then use KSP's.

Writing without blocking the game
---------------------------------

A serial write can block for tens of milliseconds: the first performance run measured about 20 ms per
telemetry packet, five times a second, which took 9 % of every frame. ``KSPSerialPort.TrySendPacket``
therefore only builds the frame and queues it. A background thread (``SenderWorker``) writes it. Only the
newest frame is kept, because an older telemetry packet is useless. After three consecutive failed writes
the thread closes the port and sets ``LinkFailed``. The handshake still writes synchronously, because it
runs once during scene load.

Reading the port safely
-----------------------

``SerialWorker`` issues one read at a time and waits for it to complete, so the parser state is
only touched by one thread. The parser rejects zero-length packets, stops at the announced
size, and any exception in the worker is logged and resets the parser instead of leaving it
stuck. Before capture or publication, checksum-valid controls must be exactly 25 bytes and handshakes
exactly 4 bytes. Wrong-size packets are discarded without changing buttons, controls, pending valid packets
or handshake state; fixed-size decoding must never reuse bytes left in the shared buffer. A completed,
validated packet is handed to the main thread through ``NewPacketBuffer`` guarded by
``SerialMutex``. Before replacing that latest-value buffer, the reader captures STAGE rising edges and
both SAS/RCS transitions in ``ButtonEvents`` with their receive timestamps. The first packet after a
reset establishes the baseline; pending presses are cleared on reconnect or flight initialization.
Unity consumes every queued press in order, while analog axes and level switches use the latest packet.
STAGE requests older than one second are discarded using their actual receive time.

Port discovery and failures
---------------------------

With handshake enabled, discovery probes the last known port when present, then ``DefaultPort`` and
remaining ports. Each candidate is verified in isolation: an open failure, write timeout or missing reply
closes that port before moving on. Bluetooth modem ports (``\Device\BthModem*``) are skipped because they
open but never answer, and candidates are not probed twice.

With ``HandshakeDisable = 1``, discovery does not enumerate ports: it tries only the configured
``DefaultPort``, even if enumeration would not list it. Empty or whitespace-only configuration produces
no candidates. Connection and reconnection also reject any other candidate before constructing or
opening it, so failure of the trusted port cannot send telemetry to an unrelated device.

Telemetry is only sent while a controller was found (``DisplayFound``). If sending fails three times in
a row, ``LinkFailed`` stops main-thread send/application until the background supervisor restores
connectivity. It closes/reprobes the link and retries during the same flight, logging transitions
rather than blocking and throwing on every frame. The write timeout is 250 ms.

Measuring the plugin's cost
---------------------------

With ``PerfLog`` set to 1 in ``config.xml`` (the default file enables it) the plugin writes one line to
``KSP.log`` every 10 seconds while a vessel is active. It is a list of ``key=value`` pairs so that a
script or a profiler can parse it:

.. code-block:: text

   KSPSerialIO: perf window_s=10.0 parts=142 frames=600 frame_ms=16.67 fps=60.0 upd_avg_ms=0.021 upd_max_ms=0.85 upd_share_pct=0.126 samples=50 sample_avg_ms=0.310 sample_max_ms=0.85 send_avg_ms=0.004 send_max_ms=0.05 writes=50 write_avg_ms=19.80 write_max_ms=20.4 ws_mb=27700 private_mb=28100 gc_heap_mb=1200 gc0=12 gc1=3 gc2=0 proc_cpu_pct=96

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Key
     - Meaning
   * - ``window_s``, ``parts``, ``frames``
     - Length of the window, parts of the active vessel and frames rendered in it.
   * - ``frame_ms``, ``fps``
     - Average frame time of the whole game in the window.
   * - ``upd_avg_ms``, ``upd_max_ms``
     - Time spent in ``KSPSerialIO.Update`` per frame. It includes sampling the vessel and sending the
       packet, but not ``AxisInput`` or the reader thread.
   * - ``upd_share_pct``
     - The plugin's Update time as a percentage of the window: the fraction of every frame it takes.
   * - ``samples``, ``sample_avg_ms``, ``sample_max_ms``
     - Telemetry samples taken and the time to read the vessel into ``VData``. This is the only part
       that grows with the number of parts.
   * - ``send_avg_ms``, ``send_max_ms``
     - Time the main thread spends queueing the packet for the writer thread. Should be a few microseconds.
   * - ``writes``, ``write_avg_ms``, ``write_max_ms``
     - Writes done by the background writer thread and how long each took. This time is **not** spent
       in the game's frame; a value near the 250 ms write timeout means the board is not reading.
   * - ``ws_mb``, ``private_mb``
     - Working set and private memory of the whole KSP process, in MB (read with the Windows API;
       Mono's ``Process.WorkingSet64`` returns 0).
   * - ``gc_heap_mb``, ``gc0``, ``gc1``, ``gc2``
     - Managed heap size and the garbage collections of each generation during the window.
   * - ``proc_cpu_pct``
     - CPU used by the whole process in the window, where 100 is one full core.

To compare the game with and without the plugin, fly the same craft in the same place both times:

#. ``./ci deploy`` and fly for a minute; the ``perf`` lines are written to ``KSP.log``. Then ``./ci save_log a_all`` keeps that log, because KSP overwrites it on the next launch.
#. Close KSP, run ``./ci disable_mod``, fly the same craft again and ``./ci save_log b_no_plugin``.
#. ``./ci enable_mod`` puts the plugin back.

The log line only exists while the plugin runs, so the frame rate of the run without the plugin has to
come from outside the plugin: an external overlay (the NVIDIA or Steam counter) or a profiler such as
KSPPerf.

Automatic reconnection
----------------------

A link supervisor runs on its own background thread (never the main thread) and brings the link back
without reloading the scene. It treats the link as lost when any of these happens:

* the writer thread reports ``LinkFailed`` after repeated send failures;
* no bytes arrive from the board for ``LinkTimeoutMs`` (3000 ms) while the link was up;
* the port is closed or throws;
* the initial discovery found nothing.

While in the flight scene it then repeats, every ``RetryIntervalMs`` (2000 ms) and without an attempt limit:

#. Close the old port.
#. Build the candidate list: the last known port first, then every non-Bluetooth name of
   ``SerialPort.GetPortNames()``. The list is rebuilt on each attempt because the board may come back
   under a different COM number after a reset.
#. For each candidate: open it with ``DtrEnable = true``, wait 300 ms, send the handshake (id 0, bytes 1, 2, 3)
   and wait up to 1500 ms for 3, 1, 4. If there is no reply, close it and try the next one.

On a reply the link resumes at once. The firmware treats any valid packet, a handshake included, as a reconnection
(see :doc:`firmware`), so nothing else needs to be reset. Only the handshake proves that a port belongs to
the board: USB has no "reconnect" request, a CDC serial port simply disappears and reappears with the bus.

.. uml:: diagrams/state_link_recovery.puml

The supervisor logs with the tag ``KSPSerialIO[LINK]``:

.. list-table::
   :header-rows: 1
   :widths: 45 55

   * - Line
     - Meaning
   * - ``lost: <reason> | port=<name>``
     - The link was declared lost and why.
   * - ``searching attempt <n>: <candidates>``
     - Start of attempt ``n`` with the ports that will be tried.
   * - ``candidate <name>: opened``
     - The port opened.
   * - ``candidate <name>: open failed: <msg>``
     - The port could not be opened (busy, gone).
   * - ``candidate <name>: no handshake reply``
     - Opened but it is not the board (or the board is still booting).
   * - ``candidate <name>: handshake ok``
     - The board answered 3, 1, 4.
   * - ``reconnected on <name> after <n> attempts, <ms> ms down``
     - The link is up again.

``./ci verify`` counts these lines in ``KSP.log`` (see ``check_reconnect_log`` in ``tools/verify.py``).

State exposed to other code
---------------------------

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Member
     - Meaning
   * - ``KSPSerialPort.DisplayFound``
     - A controller answered the handshake.
   * - ``KSPSerialPort.VData``
     - Telemetry that is sent on every refresh.
   * - ``KSPSerialPort.VControls``
     - Latest decoded controls. ``VControlsOld`` holds what was already applied.
   * - ``KSPSerialPort.ControlReceived``
     - A new control packet is waiting to be applied.
