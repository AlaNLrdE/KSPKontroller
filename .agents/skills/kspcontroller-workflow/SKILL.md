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

Other Codex sessions of this repo may be using the board. Before and after `./ci upload_sketch`, SendMessage each
peer from `ListAgents` ("flashing COM5 now" / "flash done"). The firmware changes need the user's explicit OK
for each flash (AGENTS.md rule 1); a request like "disable it in the Arduino" is that OK.

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

## Never call Debug.Log from a worker thread

KSP's in-game debug console creates TextMeshPro UI objects per log line; off the main thread this crashes UnityPlayer
(Access Violation in Mesh.Internal_Create, seen in 10 crash reports). All plugin logging uses `KSPLogQueue.Log`
(direct on the main thread, queued and drained from `Update` otherwise); `./ci verify` check `no direct Debug.Log` enforces it.
In a fresh worktree copy `build/refasm` from the main checkout before `./ci build_KSPSerialIO`.
