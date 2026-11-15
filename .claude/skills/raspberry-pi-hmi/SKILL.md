---
name: raspberry-pi-hmi
description: How to reach, deploy, diagnose and change the Raspberry Pi 4 flight data display (hmi-cpp/, C++ kRPC client, framebuffer output) and the kRPC server it needs inside KSP. Use before any work on hmi-cpp, the Pi itself, kRPC in GameData, or when the Pi screen shows nothing or the wrong thing.
---

## Reach it

- Only as `ssh ksphmi` (alias in `~/.ssh/config`, key `~/.ssh/id_ed25519`). Always `-o BatchMode=yes`. Never put the
  Pi's address or user name in the repo (it is public); `HMI_HOST` overrides the alias.
- `sudo` asks for a password: anything needing root is a command you hand to the user, one block, with why.
- Logs: `./ci hmi_logs` (= `journalctl _SYSTEMD_USER_UNIT=ksphmi.service`). `journalctl --user` finds nothing:
  Raspberry Pi OS keeps no per-user journal. The user is in `adm`, so the system journal is readable.

## Layout on the Pi

- `~/ksphmi/cpp`: the `hmi-cpp/` sources and `config.toml` (replaced by every deploy), `build/` (kept: incremental
  CMake build), binary `~/ksphmi/cpp/build/ksphmi`. `~/ksphmi/env` (`KSPHMI_KRPC_ADDRESS`, written by the deploy),
  `~/ksphmi/config.local.toml` (the user's, never touched).
- User unit `ksphmi.service` (from `hmi-cpp/systemd/`): `KSPHMI_OUTPUT=fbdev`, `KSPHMI_APP_DIR=%h/ksphmi/cpp`,
  `KSPHMI_LOCAL_CONFIG=%h/ksphmi/config.local.toml`, `EnvironmentFile=-%h/ksphmi/env`.
- History (2026-11-14): a Python/pygame runtime (`hmi-pi/`, `~/ksphmi/app`, `~/ksphmi/venv`) ran first, then the
  C++ port ran as `ksphmi-cpp.service`. Measured on the Pi: Python ~91 % of a core, C++ ~43 %. Python was dropped;
  the deploy removes the old unit, `app/` and `venv/`. Tests and tooling (`tools/kspsim`, `verify.py`) stay Python.

## Deploy and check

1. `./ci test_hmi` (WSL here), then `./ci deploy_hmi`: package/linger check (prints the one-time `sudo apt ...`
   command), copy to `~/ksphmi/cpp`, `cmake` Release + `cmake --build -j4` + `ctest` **on the Pi** (a failure stops
   before the restart, the old binary keeps running), install the unit, restart. It requires `active` **and**
   `NRestarts=0` after 8 s: `Restart=always` makes a crash loop look `active` at a glance.
2. `./ci verify` -> `hmi:` lines: views and resources, local ctest build current, service, C++ unit (ExecStart,
   no leftover `ksphmi-cpp.service`), kRPC address (= this PC as the Pi sees it), screen (framebuffer not
   blanked), console cursor rule, kiosk boot, Pi render budget (`ksphmi --bench 5 --offscreen` over SSH);
   `kRPC:` lines: mod version vs `tools/requirements.txt`, server settings, firewall rule.
3. You cannot see the monitor. Say so, and ask the user what the screen shows after any display change.

## Display facts (learned the hard way, 2026-11-14)

- SDL's `kmsdrm` driver (tried through Debian's pygame) needs Mesa EGL/GLES (Lite lacks them: "EGL not
  initialized") and even with them it set the mode and left the screen **black**. Final design: SDL software
  renderer, changed rows copied to `/dev/fb0` (RGB565/XRGB8888, user in group `video`). The unit sets
  `KSPHMI_OUTPUT=fbdev`; `KSPHMI_OUTPUT=sdl SDL_VIDEODRIVER=kmsdrm` is the opt-in GPU path.
- `/sys/class/graphics/fb0/blank` = 4 (powered down) hides every frame while the service and kRPC link look
  healthy: the user saw a dark screen for an hour. The HMI unblanks with the `FBIOBLANK` ioctl (no root) at start
  and every 30 s; `check_hmi` reports it.
- The text console (fbcon, tty1) blinks its cursor into the framebuffer. Only root can stop it: tmpfiles rule
  `w /sys/class/graphics/fbcon/cursor_blink - - - - 0` in `/etc/tmpfiles.d/ksphmi-cursor.conf`. The live
  `cursor_blink` value is no proof: it reads 0 whenever fbcon's timer is stopped (e.g. after an unblank).
- Kiosk boot (2026-11-14, `hmi-cpp/systemd/setup-kiosk.sh`, run by the user with sudo, `--undo` reverts): a
  system oneshot `ksphmi-splash.service` runs `ksphmi --splash` after `dev-fb0.device` (needs the udev rule
  `TAG+="systemd"` on fb0, else the device unit never appears) and before `systemd-user-sessions.service`, so the
  HMI's first frame always comes after it. Removing `console=tty1` alone is not enough: with no `console=` left the
  kernel falls back to tty0 (the screen), and on a Pi 4 without `enable_uart=1` `serial0` may not exist; so
  `console=ttynull` goes **first** (the last `console=` becomes `/dev/console`). Never add `fbcon=map:`/`nodefer`:
  with DRM fbdev emulation the mode is set through fbcon's set_par; the splash re-applies it with
  `FBIOPUT_VSCREENINFO` + `FB_ACTIVATE_FORCE`. Unverified until the first reboot: ask the user what they saw.
- Render screens on the PC to review them: `./ci hmi --demo --size 2520x1680 --screenshot out.png` (in WSL; the
  path is relative to the repo), then look at the PNG.
- Static instrument art (dial faces: chrome bezel, shaded face, ticks, E/F, icon, window) is drawn once per
  size, state and resource into a cached texture. Per frame draw only the dynamic needle polygon, the hub and
  text.
- Chrome without images: a disc shaded row by row through grey stops (bright top, dark band just below the
  middle, lighter bottom), a smaller reversed-gradient disc as the inner bevel, a dark seat ring and a short
  white arc highlight top-left. Face shading: concentric circles from a grey rim to white at ~45% inwards.
  Car-gauge needles pivot on a hub below the dial centre: keep the needle tail shorter than the hub radius or
  it pokes into the window underneath.
- `Canvas::polygon` fills convex shapes only: split concave icons such as the lightning bolt into triangles, and
  ring sectors into quads (`gauges::sector` points i and size-1-i share a ray). Render tests and screenshots need
  the `build/hmi-cpp-deps` libraries on `LD_LIBRARY_PATH` and `KSPHMI_FONT` (see `hmi_env` in `ci`).
- Several captions in a row (dial labels): fit one common font size to the narrowest cell, computed when the
  layout changes; a per-frame fit makes the text size jitter.
- Format numbers through `src/fmt.cpp` (no `-0 m`); a value that can be renamed in game (vessel name) must be a
  stream, not read once.
- Optional panels (NavBall marker groups, resources/staging) must fail on their own: on the launch pad kRPC
  raises InvalidOperationException for the singular surface speed frame and for a stage without delta-v. Catch
  per value, retry after 0.5 s, never end the vessel session (`launch_pad_errors_drop_only_the_failing_values`).

- Maneuver orbit (2026-11-14): kRPC 0.7 has `Orbit_PositionAt` / `Orbit_VelocityAt(orbit, ut, frame)`: read
  `Node.orbit` at `Node.ut` in the body's non-rotating frame, so the dashed conic shares ORBIT's view axes. Stream
  only the node key (UT, prograde/normal/radial), refetch on change or every 2 s; cache the dashes per state and
  quantised scale. Not yet seen in the real game: check the log line `maneuver orbit: periapsis ... agrees`.
- Apollo skin (2026-11-14, proposal, `--skin apollo` / `[display] skin`): all static art (brushed grey written
  straight into the surface pixels, frames, labels, lamp housings, segment windows with ghost segments, meter
  scales, bezels) is one full-screen texture per size and screen kind; per frame only lit segments, lit lamps,
  pointers and the plots. Measured 7.1 ms vs 8.6 ms classic (`--bench`, WSL). Segment glow is pre-mixed with the
  glass colour (opaque): translucent triangles are slow on the software renderer. The NavBall keeps its whole
  zone (an inset rect pushed HDG under the mode buttons): its bezel goes around it, in the gaps between zones.
  `KSPHMI_SHOTS=DIR ./ci test_hmi` saves facility / no-vessel / caution screenshots that `--demo` cannot reach.
- Skins switch at runtime (2026-11-14): `Snapshot.skin` comes from an optional ` skin=<name>` field of the UDP
  50002 context reply (kspsim only; KSPSerialIO never sends it), read about once a second in every state from
  `KrpcSource::publish` (telemetry thread). Known context skin > `[display] skin`. `ApolloSkin` is created the
  first time it is shown and kept, so switching back reuses its panel texture (`builds` stays 1). Navball has
  `chrome` (classic panel, readouts, buttons): apollo turns it off and draws its own around
  `Navball::globe_geometry` (`globe` is already a free function in navball.cpp). `./ci test_hmi --sim` runs
  kspsim with `--skin apollo` and requires `skin: apollo (context` in the HMI log.
- WSL can fail with `Wsl/Service/E_UNEXPECTED` for minutes: wait with a background loop on
  `wsl.exe -d Ubuntu -e true`; never `wsl --shutdown` (other agents use it). No PIL in the venv: crop PNGs
  with a stdlib zlib script.
- ORBIT's plot area is short (about 12 % of the screen height at 3:2): labels belong beside markers (side
  margins are reserved for them); labels above/below a marker at the view's edge leave the clip.

## kRPC (server in KSP, client on the Pi)

- kRPC **0.7.0** for KSP 1.12.5 speaks protocol version 1, as the HMI's own client (`src/krpc.cpp`) and the
  krpc 0.7 Python client kspsim's tests use (0.5.x = protocol 0). Mod version and `tools/requirements.txt` must
  match (`check_krpc`). Web search results were stale (said 0.5.4 was latest): check
  `api.github.com/repos/krpc/krpc/releases` instead.
- The mod ships an empty `PluginData/settings.cfg` (defaults: 127.0.0.1 only). Install the complete template
  `tools/krpc/settings.cfg` (every key, kRPC's own format, server `address = 0.0.0.0`, ports 50000/50001).
  Confirm on the first run from KSP.log `[kRPC]` lines which address it really bound.
- **Never hand-write a partial settings.cfg** (2026-11-14): `ConfigurationFile.AfterLoad` copies every persistent
  field, so missing keys become 0/False, not defaults. `maxTimePerUpdate = 0` = server accepts the client but never
  answers any RPC (HMI hung a whole session). `check_krpc` FAILs on maxTimePerUpdate/recvTimeout missing or 0 and
  WARNs on any difference from the template.
- A TCP connect timeout does not cover the handshake/first RPC: the HMI uses a 5 s TCP timeout plus its own 20 s
  deadline on the whole connect, and logs connect failures.
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
  a null vessel and reconnected in a loop (seen live 2026-11-14). Handle null as NOT IN FLIGHT; kspsim answers
  null too. Any behaviour seen only in the real game goes into `tools/kspsim/krpcserver.py` and a test.

## Building on this PC (WSL)

- This Windows PC has no C++ compiler; WSL Ubuntu 24.04 has g++/cmake but no sudo. `./ci build_hmi` / `hmi` /
  `test_hmi` re-run themselves in WSL and unpack SDL2/SDL2_ttf with `apt-get download` + `dpkg -x` into
  `build/hmi-cpp-deps` (PKG_CONFIG_SYSROOT_DIR, `-rpath-link`, LD_LIBRARY_PATH, KSPHMI_FONT). SDL2_ttf.pc names
  harfbuzz/glib include dirs: CMake refuses imported targets whose include dirs do not exist, so ci creates them.
  `build_hmi_cpp` / `test_hmi_cpp` are kept as aliases.
- Building on `/mnt/c` from WSL is slow (minutes); for quick iterations build into `~/` inside WSL.
- kspsim runs inside WSL with the Windows venv: `PYTHONPATH=<repo>/build/venv/Lib/site-packages
  PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION=python` (pure-Python protobuf), or set `KSPSIM_PYTHON`. Then
  `./ci test_hmi --sim`. The WSL system python3 has no krpc: `--sim` dies early without one of those.
- SDL's **software** renderer rasterises each blended triangle through a temporary surface: opaque untextured
  batches must use `SDL_BLENDMODE_NONE`, rectangles `SDL_RenderFillRect`, glyphs `SDL_RenderCopy` (40 -> 9 ms
  per 2520x1680 frame on the PC). The GPU path keeps batched `SDL_RenderGeometry` with feathered edges.
- Never type `cat > file` without a heredoc in a Bash call: it waits on stdin until the tool times out.
- `hmi-cpp/` is LF (`.gitattributes`): it is built on the Pi; the kiosk script and the unit must stay LF.
