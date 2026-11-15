# Changelog

* **0.19.4.0**: 20261115
  New assembly `KSPSerialIO.Hmi.dll` (installed in `GameData/KSPSerialIO/Plugins`) with the kRPC service `KSPHMI`
  for the Raspberry Pi's VAB screen (phase 1 of the editor ship report): `Revision()` and `ShipReport()`, available in
  the editor scene. The report (text format v1: ship, situation of KSP's delta-v app, mass, cost, funds, parts, crew,
  size, engines, per-stage delta-v / TWR / burn / events, or `dv=pending`) is built on Unity's main thread on
  `onEditorShipModified`, craft load, new craft, `onDeltaVCalcsCompleted` and the delta-v app's situation change, and
  cached; RPCs only read the cache. It targets .NET 4.7.1 because kRPC 0.7 is a .NET 4 assembly, and compiles against
  the install's `KSP_x64_Data/Managed` and `GameData/kRPC/KRPC.Core.dll` (`./ci build_KSPSerialIO`). `KSPSerialIO.dll`
  is unchanged apart from the version and still has no kRPC dependency: without kRPC only the new DLL fails to load.
  Log tag `KSPSerialIO[HMI]`.

* **0.19.3.16**: 20261114
  Fix the link silence detection: it measured silence from the last queued packet, which is never older than
  200 ms while flying, so a board that stopped answering mid-flight stayed `link_state=up` for minutes
  (board-dead-1923: no rx from 19:16:26, no `lost:` line). Silence now counts from the start of the current
  sending run (or the last received byte), and pauses over 1 s (scene loads) still do not count as a loss.

* **0.19.3.15**: 20261114
  Add read-only HMI scene discovery over UDP 50002, including space-center facility UI events.
  The Pi shows dedicated preparation checklists for VAB, SPH, Mission Control, Astronaut Complex,
  R&D, Administration, Tracking Station and Space Center. The simulator supports all views.

* **0.19.3.14**: 20261114
  SAS and RCS are toggle buttons on the board (pins 2 and 9): every change of their ControlPacket bit is one press
  and now toggles the vessel's SAS / RCS (`ToggleGroup`) instead of copying the bit (`SetGroup`). Copying lost
  presses after SAS / RCS had been changed in the game (T / R), because the board's bit already matched. The first
  packet of a link or flight only seeds the bits (as for the stage button), and SAS / RCS are no longer forced to
  the board's bit when entering flight or switching vessels. Each toggle is logged as `KSPSerialIO[CTRL]`.
  Firmware: the SAS / RCS buttons flip their own bit on each press (no longer the opposite of AGSAS / AGRCS), and
  the action group 1 switch is disabled (`CG1PIN -1`; nothing is wired and an open input meant group 1 ON).

* **0.19.3.13**: 20261108
  Fix a game crash (UnityPlayer.dll access violation in Mesh.Internal_Create <- TextMeshProUGUI.Awake <-
  DebugScreenConsole.OnMemoryLogUpdated <- Debug.Log <- KSPSerialPort.LogBoardDebug / SerialWorker, 10 of 10 Unity
  crash reports). Root cause: KSP's in-game debug console creates Unity UI objects for every log line, which is
  illegal off the main thread, and the plugin called Debug.Log from its serial reader thread (and the supervisor and
  sender threads). All plugin logging now goes through `KSPLogQueue.Log`: on the main thread it writes directly, on
  any other thread it enqueues the line (bounded to 2000, drops the oldest and then logs a "dropped n lines" warning)
  and the main thread drains up to 50 lines per frame from Update and flushes the queue on scene unload and quit.
  Tags and formats are unchanged; worker-thread lines may appear a frame or so late. New `./ci verify` check
  `no direct Debug.Log`.
* **0.19.3.12**: 20261108
  Stage button hardened. The press is latched when its ControlPacket arrives (the board holds the bit only 150 ms)
  and applied on the next game frame, so a slow or stalled frame cannot skip it; a press older than 1 s is dropped
  as stale. New log lines `KSPSerialIO[CTRL] stage press received` (packet arrived) and, when applied,
  `stage button: ... age_ms=N`; a press that arrives but is never applied is now visible in the log.
* **0.19.3.11**: 20261103
  Automatic reconnection. A link supervisor thread (started with the flight scene, stopped on scene unload)
  declares the link lost when the writer fails 3 times, when the board sends nothing for 3 s, or when the port
  closes, then retries every 2 s without limit, on the last port first and then on every other non Bluetooth
  port (the board may come back under another COM number): open, 300 ms settle, handshake, 1.5 s for the reply.
  It also takes over when the first discovery at scene load finds nothing. While the link is down the main thread
  sends and applies nothing and never blocks; on reconnect telemetry and controls resume with no scene reload and
  the stage button is re-seeded so a held button cannot stage. Log lines: `KSPSerialIO[LINK] lost: ...`,
  `searching attempt n: ...`, `candidate <port>: ...`, `reconnected on <port> after n attempts, m ms down`.
  The perf line gains `link_state=<up|searching>` and `reconnects=<n>`.
  Review fixes: the silence test only counts once telemetry has been queued since the link came up (measured from
  the last queued packet, the board idles 2 s after the last valid packet) and the 10 s start grace also applies after
  a connection made in Awake, so a long vessel load no longer causes a false loss; the axes are hooked again when
  the link becomes ready (late reconnect with the same vessel id); a stopped supervisor no longer touches the
  port, reader, writer or `DisplayFound` of the next scene, and scene cleanup waits up to 300 ms for it.

* **0.19.3.10**: 20261103
  The KSPSerialIO[BOARD] line also carries the LED matrix frame (`matrix=` as 24 hex digits, one byte per column),
  so the log shows what the matrix displayed. Matches the firmware that draws splash, fuel, stage, horizon,
  vertical speed and SAS screens, and resets the board when the USB link stays dead for 20 s.
* **0.19.3.9**: 20261103
  The board sends a DebugPacket (id 102) once per second and the plugin writes it to KSP.log as a
  `KSPSerialIO[BOARD]` line (uptime, loop and send times, packet counters, buffers, reset cause), plus a
  `REBOOT detected` line when the uptime drops. The last line before a freeze shows what the firmware was doing.
* **0.19.3.8**: 20261103
  The perf line gains `rx_bytes` and `rx_age_ms` (bytes received from the board in the window and time since
  the last chunk), so a board that stops talking can be told apart from a plugin that stops reading.
  The firmware now has a 2 s watchdog: if its loop stalls the board resets itself.
* **0.19.3.7**: 20261103
  The stage button now behaves exactly like the space bar: it only calls `StageManager.ActivateNextStage()`
  and no longer toggles the Stage action group (before and 150 ms after the press), which could confuse
  other mods such as MechJeb's ascent autopilot. Each press logs a `KSPSerialIO[CTRL]` line with the stage
  and the current throttle.
* **0.19.3.6**: 20261103
  Every port open, close and shutdown step now logs a line starting with `KSPSerialIO[PORT]` (with thread
  and time), including `OnDestroy` and `OnApplicationQuit`. A `closing port` line without a following
  `closed in N ms` line means `Close()` hung. Used to find why the board stops answering after KSP exits.
* **0.19.3.5**: 20261103
  The stage button (Arduino pin 2) stages the vessel once per press; the first packet after connecting only
  seeds the button state, so a button that is already down no longer stages by itself. New throttle mode 4
  (`ThrottleEnable`): keyboard throttle works and the potentiometer takes over only when it moves. The
  default is now 0 (keyboard only) because a floating analog input read a constant 18 % that came back
  whenever the throttle was cut. The initial action group sync no longer aborts on a failing part module.
* **0.19.3.4**: 20261103
  Telemetry is written by a background thread. The first performance run showed that each
  `Port.Write` blocked the main thread for about 20 ms, five times a second, costing 9 % of every frame.
  The main thread now only queues the packet (newest wins). The perf line gains `writes`, `write_avg_ms`
  and `write_max_ms` and reads process memory with the Windows API, because Mono returned 0.
* **0.19.3.3**: 20261103
  The `PerfLog` line is now a list of `key=value` pairs for easy parsing, and adds process memory
  (working set, private), managed heap size, garbage collections per generation and process CPU usage.
* **0.19.3.2**: 20261103
  Optional performance log (`PerfLog` in config.xml): every 10 s it writes how long the plugin takes per
  frame, how much of that is vessel sampling and packet sending, and the game's own frame time. The
  harmless read error when a port is closed no longer dumps a stack trace.
* **0.19.3.1**: 20261103
  The plugin now asserts DTR on its serial port. Native-USB boards such as the Arduino UNO R4 only
  transmit while DTR is high, so without it the handshake never got a reply and no LED lit. The default
  `BaudRate` in config.xml is now 115200 to match the sketch.
* **0.19.3.0**: 20261103
  Merged the functional changes of upstream zitron-git/KSPSerialIO 0.19.0 to 0.19.3b into this
  cross-platform (PsimaxSerial) fork:
  * Seven navball vector fields in `VesselData` (prograde, maneuver, target and normal headings;
    upstream 0.19.1). The packet grows from 200 to 214 bytes, so the Arduino struct must be updated.
  * Those angles are 16-bit fixed point, degrees times 50 (upstream 0.19.2).
  * Removed the "disengage SAS while steering" block, which fought with the axis input (upstream
    0.19.3b). `SASTol` in config.xml is now unused.
  * Not ported: the `\.\COMn` port name workaround (PsimaxSerial already adds that prefix), the
    OpenNETCF serial code and the always-on checksum debug logging.
  Fork fixes since 0.18.7: single-reader serial thread and parser hardening, 25-byte ControlPacket,
  per-port isolation during discovery, Bluetooth ports skipped and bounded send failures.

* **0.20.0.0**: 20180105
  This is my first edit, removing stuff I don't need and testing the build process.
* **0.18.6.1**: 20170304
  This release just updates to the upstream 0.18.6 release.
* **0.18.2.2**: 20160809
  I'm still getting the hang of making the new serial handling code work
  properly, sorry. This release is a minor change to the serial port
  initialisation and teardown, ensuring the port is created properly every
  time the flight scene loads, and is torn down properly without memory leaks
  when the flight scene exits. This should make it more stable across
  multiple scene changes.
* **0.18.2.1**: 20160721
  * Move inbound packet processing out of worker back in to main thread.
  * Serial worker thread uses while loop instead of nested calls,
    fixing segfaults on Linux.
* **0.18.2**: 20160719
  * Initial port of upstream 0.18.2.
  * Replace original serialport library with PsiMaxSerial.
  * Replace serial event listener with threaded Stream BeginRead/EndRead loop.
