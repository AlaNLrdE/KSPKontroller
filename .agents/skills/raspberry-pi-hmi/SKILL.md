---
name: raspberry-pi-hmi
description: How to reach, deploy, diagnose and change the Raspberry Pi 4 flight data display (hmi-pi/, kRPC client, framebuffer output) and the kRPC server it needs inside KSP. Use before any work on hmi-pi, the Pi itself, kRPC in GameData, or when the Pi screen shows nothing or the wrong thing.
---

## Reach it

- Only as `ssh ksphmi` (alias in `~/.ssh/config`, key `~/.ssh/id_ed25519`). Always `-o BatchMode=yes`. Never put the
  Pi's address or user name in the repo (it is public); `HMI_HOST` overrides the alias.
- `sudo` asks for a password: anything needing root is a command you hand to the user, one block, with why.
- Logs: `./ci hmi_logs` (= `journalctl _SYSTEMD_USER_UNIT=ksphmi.service`). `journalctl --user` finds nothing:
  Raspberry Pi OS keeps no per-user journal. The user is in `adm`, so the system journal is readable.

## Deploy and check

1. `./ci test_hmi`, then `./ci deploy_hmi` (refuses and prints the one-time `sudo apt ...` / linger command if the
   Pi lacks a package; warns with the cursor command). It requires `active` **and** `NRestarts=0` after 8 s:
   `Restart=always` makes a crash loop look `active` at a glance.
2. `./ci verify` → `hmi:` lines: tests, service, kRPC address (= this PC as the Pi sees it), screen (framebuffer
   not blanked), console cursor rule; `kRPC:` lines: mod version vs the Pi client, server settings, firewall rule.
3. You cannot see the monitor. Say so, and ask the user what the screen shows after any display change.

## Display facts (learned the hard way, 2026-11-14)

- pip's pygame / pygame-ce wheels bundle an SDL **without** `kmsdrm`. Debian's `python3-pygame` has it, but SDL
  kmsdrm also needs Mesa EGL/GLES (Lite lacks them: "EGL not initialized") and even with them it set the mode and
  left the screen **black**. Final design: draw into a pygame Surface, copy to `/dev/fb0` (RGB565, ~21 ms per
  2520x1680 frame, user in group `video`). Service sets `KSPHMI_OUTPUT=fbdev`.
- `/sys/class/graphics/fb0/blank` = 4 (powered down) hides every frame while the service and kRPC link look
  healthy: the user saw a dark screen for an hour. The HMI unblanks with the `FBIOBLANK` ioctl (no root) at start
  and every 30 s; `check_hmi` reports it.
- The text console (fbcon, tty1) blinks its cursor into the framebuffer. Only root can stop it: tmpfiles rule
  `w /sys/class/graphics/fbcon/cursor_blink - - - - 0` in `/etc/tmpfiles.d/ksphmi-cursor.conf`. The live
  `cursor_blink` value is no proof: it reads 0 whenever fbcon's timer is stopped (e.g. after an unblank).
- Render screens on the PC to review them: `SDL_VIDEODRIVER=dummy python -m ksphmi --demo --size 2520x1680
  --screenshot out.png` (from hmi-pi/, with build/hmi-venv), then look at the PNG.
- Format numbers through `ksphmi/fmt.py` (no `-0 m`); a value that can be renamed in game (vessel name) must be a
  stream, not read once.

## kRPC (server in KSP, client on the Pi)

- kRPC **0.7.0** for KSP 1.12.5 speaks protocol version 1; the krpc 0.7 Python client refuses older servers
  (0.5.x = protocol 0). Mod version and `hmi-pi/requirements.txt` must match (`check_krpc`). Web search results
  were stale (said 0.5.4 was latest): check `api.github.com/repos/krpc/krpc/releases` instead.
- The mod ships an empty `PluginData/settings.cfg` (defaults: 127.0.0.1 only). Install the complete template
  `tools/krpc/settings.cfg` (every key, kRPC's own format, server `address = 0.0.0.0`, ports 50000/50001).
  Confirm on the first run from KSP.log `[kRPC]` lines which address it really bound.
- **Never hand-write a partial settings.cfg** (2026-11-14): `ConfigurationFile.AfterLoad` copies every persistent
  field, so missing keys become 0/False, not defaults. `maxTimePerUpdate = 0` = server accepts the client but never
  answers any RPC (HMI hung a whole session). `check_krpc` FAILs on maxTimePerUpdate/recvTimeout missing or 0 and
  WARNs on any difference from the template.
- The krpc Python client's `timeout` only covers the TCP connect, not the handshake/first RPC: the HMI uses a
  5 s TCP timeout plus its own 20 s deadline on the whole connect, and logs connect failures.
- The wired LAN is a **Public** network in Windows and the wk-rea KSP has no firewall rule: the user must add
  "KSP kRPC (wk-rea)" in an administrator PowerShell (command in docs/hmi.rst). Creating it yourself is blocked.
- Installing into `GameData/kRPC` is a GameData change: KSP closed, peer `ksp-workbench-*` informed, snapshot
  refreshed with `./ci verify --snapshot`.

## Before the user opens KSP to test the HMI

Run `./ci verify` and read every `hmi:` / `kRPC:` line **before** they launch. If anything the test needs is
missing (mod, firewall, cursor rule), stop them and say so first; do not start monitors for a session that cannot
work. Remind them the board and the HMI only get data once a vessel is **in flight**.

## Real kRPC vs the stand-in

- Real kRPC 0.7 returns **null** for `SpaceCenter.ActiveVessel` at the Space Center (no RPC error): the HMI read
  `None.orbit` and reconnected in a loop (seen live 2026-11-14). Handle None as NOT IN FLIGHT; kspsim now answers
  null too. Any behaviour seen only in the real game goes into `tools/kspsim/krpcserver.py` and a test.
