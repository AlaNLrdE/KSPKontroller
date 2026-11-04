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
  USB stack, hence the 20 s (later 12 s) link reset and the plugin reconnect supervisor.
- A floating analog input (no potentiometer) read ~18.7 % and, with `ThrottleEnable=2`, overrode a cut throttle (and MechJeb's).
- The stage button must call only `StageManager.ActivateNextStage()`; toggling the Stage action group confused
  MechJeb's ascent autopilot.
- `Serial.availableForWrite()` always reads 0 on this core: do not trust it.
