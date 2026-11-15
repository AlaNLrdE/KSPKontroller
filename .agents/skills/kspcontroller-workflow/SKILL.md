---
name: kspcontroller-workflow
description: Step by step workflow for changing the KSPSerialIO plugin or the Arduino firmware, deploying to the shared wk-rea KSP install, flashing the UNO R4 and diagnosing a frozen board from KSP.log. Use before any deploy, flash, game test or board hang investigation in the KSPKontroller repo.
---

## Change, build, check

1. Edit (CRLF-safe Python script), bump `AssemblyInfo.cs` and the top `CHANGELOG.md` entry together.
2. `./ci build_all` (plugin + sketch compile). `./ci verify` must show no new FAIL.
3. Commit locally. Push only when the user says so.

## Deploy and flash (KSP closed, other session informed)

1. `tasklist | grep -ci KSP_x64` is 0. SendMessage `ksp-workbench-*` and wait for "OK deploy".
2. `KSPDIR="C:/Users/alanl/wk/wk-rea/Kerbal Space Program" ./ci deploy` then `./ci verify` (installed DLL == build, 163 mods intact).
3. Tell the peer it is done. Flash: `./ci build_sketch && ./ci upload_sketch COM5`, then `./ci verify --board --port COM5`.
4. If the upload says "No device found": the board is frozen. Double-tap RESET (orange LED pulses), or unplug USB 10 s.

## Before the user opens KSP (the test must be able to work)

The user's time in the game is the expensive part. When they say they will open KSP, first check, then answer:
1. `./ci verify` with KSP closed: 0 FAIL, and read every WARN that concerns the test (`kRPC:`, `hmi:`, install).
2. Say plainly what this session can and cannot test. Missing mod, firewall rule or Pi setup: stop them and fix
   it (or hand them the admin / sudo command) before they launch. On 2026-11-14 a whole session was wasted
   because kRPC was not installed and that was said only after the game was open.
3. Remind them: the board and the HMI get data only **in flight** (not main menu / space center), and the
   simulator (`./ci sim`) cannot run at the same time (COM5 and port 50000).
4. Only then start a log monitor agent (waits for KSP_x64, streams KSP.log, `save_log` at exit).

## Flashing while other sessions exist

Other Claude sessions of this repo may be using the board. Before and after `./ci upload_sketch`, SendMessage each
peer from `ListAgents` ("flashing COM5 now" / "flash done"). The firmware changes need the user's explicit OK
for each flash (CLAUDE.md rule 1); a request like "disable it in the Arduino" is that OK.

## Reading a game session

- `./ci save_log LABEL` first (KSP overwrites the log). Then `./ci verify --log build/perf/LABEL/KSP.log`.
- `perf` lines: `write_avg_ms` ~0.6 is healthy; ~19.7 together with `rx_bytes=0` means the board stopped talking.
- `KSPSerialIO[BOARD]` lines are the board's own diagnostics once per second (uptime, loops, packet counters,
  `reset=` cause, `matrix=` frame). The last line before a silence shows the board state; a uptime drop is a reboot.
- `tools/matrix_preview.py --log LOG` draws the LED matrix as it was.

## Facts learned (with where)

- A serial write blocked the main thread ~20 ms five times a second; moved to a background writer thread (0.19.3.4).
- The UNO R4 only transmits while the host asserts DTR, and only answers at 115200 baud (0.19.3.1).
- The board froze three times with USB still enumerated and Windows seeing COM5; once the staging was *not* the
  trigger (silence 10 s before it). The 2 s hardware watchdog does not fire if `loop()` keeps running with a wedged
  USB stack, hence the link reset and the plugin reconnect supervisor. The link reset must stay above the longest scene
  load: 12 s reset the board in the middle of a flight scene load (the plugin sends nothing while loading); it is 30 s now.
- Verified in a real flight (session_reconnect1): unplugging the USB for 8 s mid-flight, the supervisor logged
  `lost: send failures`, searched 5 times and reconnected on COM5 without reloading the scene, then 9.5 minutes of flight with no loss.
- A floating analog input (no potentiometer) read ~18.7 % and, with `ThrottleEnable=2`, overrode a cut throttle (and MechJeb's).
- The stage button must call only `StageManager.ActivateNextStage()`; toggling the Stage action group confused
  MechJeb's ascent autopilot.
- `Serial.availableForWrite()` always reads 0 on this core: do not trust it.
- On the UNO R4 **WiFi** (`unor4wifi` defines `-DNO_USB`) `Serial` is `_UART1_` at 115200 to the **ESP32-S3 USB
  bridge**, which owns COM5; the RA4M1 native USB is not used (pin 21 is the USB mux). So "wedged USB" means the
  bridge: `NVIC_SystemReset()`/watchdog reset only the RA4M1 and do not recover it (stage_dead: board rebooted 30 s
  into a silence, link still dead 5 min). Only unplugging USB (power-cycles the ESP32-S3) recovers it.
- 2026-11-14 19:50: a variant that unplugging did NOT fix: the sketch ran (matrix X, RX/TX LEDs blinking,
  screens appeared while kspsim sent) but the PC received 0 bytes (kspsim log "port completely silent",
  `verify --board` no handshake). `./ci upload_sketch` alone failed ("No device found on COM5"); double-tap
  RESET (orange LED pulsing) then `./ci upload_sketch` succeeded and all `verify --board` checks passed.
  Escalation: unplug 10 s -> double-tap RESET + upload_sketch -> ESP32-S3 bridge firmware update.
- Bad frames board -> PC (kspsim "bad frame" lines, ~1 in 180): each was exactly ONE byte short, the checksum,
  always where a ControlPacket and a DebugPacket were written in the same loop(). renesas_uno 1.6.0
  `UART::write()` returns while the last byte is still shifting out. Fix c81d663: `delayMicroseconds(100)`
  after each frame (not `Serial.flush()`, which spins without a timeout); 0 bad frames afterwards. The plugin
  does not count PC-side bad frames, so check them with kspsim (`build/kspsim.log`).
  The signature on the host: `write_avg_ms` ~19.7 with `rx_bytes=0` and COM5 still present.
- Until 0.19.3.16 `CheckLink` measured silence from the last queued packet, so it never fired while flying
  (board-dead-1923: `link_state=up` with `rx_age_ms` 400000+). `./ci verify` check `log: link loss detected`.
- SAS (pin 2) and RCS (pin 9) are push buttons: each press sets the bit to the opposite of the reported AGSAS /
  AGRCS. A switch-level read (old RCS code) only held RCS on while pressed (fixed 2026-11-14).
- Disabled inputs use `-1` pin defines with `#if PIN >= 0` around every use (STAGEPIN, THROTTLEPIN), and the
  docs pin table says so; `check_pins` only parses non-negative pins.
- The plugin applies SAS/RCS/lights/... bits only when they differ from the previous packet (`VControlsOld`), and the
  firmware's SAS/RCS button sends `!AGSAS`/`!AGRCS`. So after a toggle in the game (T/R) the next board press repeats the
  same bit and is ignored. `tools/kspsim` models this (test `test_board_buttons_toggle_like_the_plugin`).
- The CG1 switch (pin 10, pull-up) is sent as the raw level: open or unwired means action group 1 ON.
- `sed -i` in Git Bash rewrites a CRLF file with LF endings: use the Edit tool or a Python helper instead.
- `BoardLink._session(ser, name)` accepts any object with `read`/`write`/`reset_input_buffer`: link behaviour can be
  unit tested with a fake board (see `FakeBoard` in `tools/kspsim/test_kspsim.py`).

- When the firmware idle timeout expires, reset partial receive state before parsing new bytes. Clearing
  Connected alone leaves rx_len active and consumes a reconnect handshake as telemetry. Keep unread serial
  bytes intact. `python3 tools/verify.py --firmware-only` tests the actual receive/dispatch/handshake code
  with fake peripherals, including a handshake already queued when expiry is first observed.

- Matrix data pages can receive infinity/NaN (escape apoapsis) or finite values beyond int range. Check
  isfinite before numeric drawing and saturate finite magnitudes before casting, not afterwards. Missing
  data shows three dashes without unit dots. `--firmware-only` runs the real matrix renderer with fake
  peripherals under UBSan, including float-cast-overflow checks across all five data fields.

- Reject unsupported receive IDs explicitly before accepting any payload. Leaving address/structSize from
  the prior packet lets a checksum-valid unknown ID of the same length overwrite that destination. Clear
  the receive state, count it in dbgRxBadSize, and do not renew valid-packet counters or link timestamps.
  Native receiver tests cover all three destinations plus the next handshake.

- Serial availability before skipping garbage does not guarantee that header, size and ID bytes remain.
  Preserve explicit header/length/ID/payload/checksum states and read only one available byte per transition.
  Keep overlapping BE BE EF headers, and reset header state on timeout too. Native tests split handshakes
  at every byte boundary after garbage, send all packet types byte by byte, and reject empty-buffer reads.

- everConnected belongs to valid packet acceptance, never matrix drawing. A packet at 100 ms can time out
  before the 2500 ms splash ends; if rendering owns this flag, the 30 s link reset is never armed. Native
  tests receive all three packet types without drawing, check the reset deadline, and ensure no-valid-packet
  and invalid-packet-only boots keep waiting without link resets. Hardware reset calls are counted stubs.

- Plugin packet length checks must guard publication and decoding, not just Buttons.Capture: require
  exactly 25 control bytes and 4 handshake bytes before copying to NewPacketBuffer. An ID-only control
  packet otherwise reuses handshake magic and can assert Abort. `--plugin-only` tests the actual built
  assembly via reflection, with log queue main-thread identity unset so no Unity UI/native calls execute.

- HandshakeDisable=1 trusts only the exact configured DefaultPort: bypass enumeration and guard TryConnect
  before constructing any other candidate. Missing/busy ports must retry that port without fallback; empty
  configuration must open nothing. Connection tests load the real plugin with separate test-only serial
  and KSP configuration assemblies (the real settings initializer calls Unity). Assert test bindings first
  and close real plugin workers after each scenario; never deploy the stand-in DLLs.

- An abrupt host close can leave any receive state incomplete while the port reopens before IDLETIMER.
  Use a separate 250 ms inter-byte deadline, checked before consuming queued bytes, without renewing link
  liveness. Native regressions truncate telemetry at every byte boundary; the hardware test also deliberately
  truncates header/payload/checksum and requires the first reconnect probe to succeed. A passing native test
  does not prove the ESP32-S3 bridge or the 60-frame abrupt-close stress case is fixed: repeat hardware runs.

## Never call Debug.Log from a worker thread

KSP's in-game debug console creates TextMeshPro UI objects per log line; off the main thread this crashes UnityPlayer
(Access Violation in Mesh.Internal_Create, seen in 10 crash reports). All plugin logging uses `KSPLogQueue.Log`
(direct on the main thread, queued and drained from `Update` otherwise); `./ci verify` check `no direct Debug.Log` enforces it.
In a fresh worktree copy `build/refasm` from the main checkout before `./ci build_KSPSerialIO`.

For a direct Windows Sphinx invocation, set `PLANTUML_JAR` to a forward-slash path (as `./ci sphinx` does).
The default backslash path is shell-quoted by `docs/conf.py` and Java can receive literal single quotes.

## KSPSerialIO.Hmi.dll and kRPC (2026-11-15)

- kRPC 0.7.0's DLLs are built for the .NET 4 runtime (mscorlib 4.0, checked with `dnfile`): anything referencing
  `KRPC.Core.dll` cannot target .NET 3.5. KSPSerialIO.Hmi targets v4.7.1 (`build/refasm-net471`) and compiles
  against the install's `KSP_x64_Data/Managed` (the repo's `Includes/` are pre-1.7: no `VesselDeltaV`).
- kRPC 0.7.0 has `GameScene.Editor` (= EditorVAB | EditorSPH); service attributes are in `KRPC.Core.dll`
  (`KRPC.Service.Attributes`). kRPC declares no `KSPAssembly`, so no `KSPAssemblyDependency` on it. Its own
  validation can be run outside KSP: load the DLL and call `KRPC.Service.Scanner.Scanner.GetServices(errors)` by
  reflection under mono (it accepted KSPHMI: Editor scene, Revision/ShipReport). Leave the service `Id` unset.
- KSP 1.12 delta-v app (seen live 2026-11-15, read from Assembly-CSharp metadata): the app's body and situation
  are `DeltaVGlobals.fetch.deltaVAppValues.body/.situation` (enum SeaLevel, Altitude, Vaccum). The app shows
  `DeltaVStageInfo.GetSituationDeltaV(opt)` = deltaVatASL / deltaVActual / deltaVinVac (same for TWR*):
  `deltaVActual` is the *altitude* value and does not follow Vacuum (Duna vac read 2328 for 2452). Fuel flow is
  pressure-independent, so dv at a pressure = dv_vac * flow-weighted Isp(p) / Isp(vac) (`ShipMath.AtPressure`).
- Editor facts: loading a craft from the other building's tab calls `EditorLogic.SwitchEditor` and moves the
  player to that building (`EditorDriver.editorFacility` changes, kRPC logs a scene switch, no KSP scene change).
  After New the crew manifest (`ShipConstruction.ShipManifest`) still holds the previous ship's crew. The staging
  list is `StageManager.Instance.Stages` (`StageGroup.inverseStageIndex`, `.Icons[i].Part`).
- To inspect KSP's API without decompilers: a tiny .NET 8 console using `System.Reflection.Metadata` (in the
  shared framework, no NuGet) lists a type's fields/methods and an IL dump (ldfld/call targets) of one method.
  Read the install's DLLs read-only; never write into the install.
- Windows Python opens text as cp1252 by default: pass `encoding='utf-8'` in edit scripts (a non-cp1252
  character makes `open(p,'w').write()` fail after truncating the file; restore it with `git checkout`).
- Linux VM without root: `apt-get -o Dir::State::Lists=... -o Dir::Cache=... update/download` + `dpkg -x` gives
  SDL2 dev files and `mono-mcs`; `pip install --user cmake`. The repo is LF in git (`text=auto`): a Linux checkout
  is LF, so new files must be LF there (CRLF exists only in the Windows working tree). The VM has no git identity:
  pass `GIT_AUTHOR_*`/`GIT_COMMITTER_*` from `git log -1` instead of changing config.
- `tools/verify.py` content checks must be newline-agnostic: the main checkout is CRLF (core.autocrlf), some
  worktrees/CI checkouts are LF. `read()` uses universal newlines (it once used `newline=""`, so `^VERSION = 1$`
  with re.M failed only in the main checkout). Read repo text through `read()`; `tools/test_verify.py` checks both.
