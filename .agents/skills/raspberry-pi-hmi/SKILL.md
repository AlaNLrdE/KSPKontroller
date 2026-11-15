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

- If AddStream itself rejects stage delta-v, no subscription exists to recover later. Retain the stage object
  for a bounded retry (0.5 s), remove successful subscriptions from the pending set, and clear that set when
  stage/parts topology changes. Do not rebuild working resource or stage streams just to retry delta-v.

- A conic can have a mathematical apoapsis while position(PI) is unavailable (1 - eccentricity <= 1e-6).
  Check the returned position optional before drawing current or maneuver apoapsis markers. The render
  regression uses e = 0.9999995 in both skins; libc++ debug hardening catches unchecked optional access.

- An open stream socket can stop delivering data while RPC stays healthy. Track monotonic arrival of stream
  results and reject cached values after 3 s of channel inactivity. Equal values still count as new arrivals;
  cache reads do not. The fake server can pause updates without closing either socket to test this case.

- Waiting-screen network identity must be cached: getaddrinfo has no portable application deadline and
  must not run under the render thread or its cache mutex. NetInfo refreshes on a worker with shared state;
  destruction signals stop without joining a pending DNS call. Test with a blocked injected query, including
  cached reads, publication after release and destruction before release.

- Validate format.day_hours as finite, positive, integral and within int range before casting configuration
  numbers. A zero day reaches integer division in both clock and duration; generic to_int truncates floats
  and cannot safely reject non-finite or out-of-range input. Config errors must identify the key and file.

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

- The RPC and stream sockets can fail independently. Check the stream-reader error before returning cached
  values; otherwise fresh snapshot timestamps can keep frozen telemetry marked LIVE. Test by closing only
  the fake server stream socket after receiving a value, while verifying RPC still answers.

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

## Testing when the Pi is unavailable

- `./ci test_hmi_container` builds Debian Bookworm + SDL2 and runs ctest plus the C++ client against kspsim,
  headlessly with no external runtime network. Requires a running Docker engine; default `linux/arm64`,
  override `HMI_PLATFORM=linux/amd64` or `linux/arm/v7`. Sources are read-only; artifacts and separate
  architecture caches live in `build/hmi-container/<platform-with-dashes>/`.
- Use this for logic, rendering and protocol checks. It cannot validate fbdev hardware, kiosk boot or Pi
  performance. Do not claim container tests passed unless Docker actually ran them.
- Docker Desktop on macOS may expose its CLI only under `~/.docker/bin`; the container target adds it
  to PATH when needed. A context skin can arrive before `Display` exists: require the telemetry context
  message and the selected display skin, rather than assuming the display logs `(context)` instead of
  `(config)`. Keep discovery disabled in the isolated simulator run.

## Interactive desktop without the Pi

- `./ci hmi_desktop` starts the actual Tk simulator and C++ SDL window together, accessed through
  `http://localhost:6080/vnc.html?autoconnect=true&resize=scale` (noVNC). Docker publishes only host
  loopback 6080; kRPC 50300/50301 and context UDP 50002 stay inside the container. Discovery is disabled.
- Stop with `docker stop ksphmi-desktop`; rerun the command after source changes to rebuild. Stop the
  desktop before automated tests for that architecture: they share `build/hmi-container/<platform>`.
- `./ci test_hmi_desktop` invokes actual Tk skin/throttle widgets with the real C++ HMI running, checks
  the skin was applied over UDP and the throttle changed over kRPC. `check_hmi_desktop` in verify.py
  reports `desktop/smoke.json` and detects stale results. This covers desktop interaction, not fbdev.
- Container source copies must preserve timestamps for incremental CMake builds. Desktop packages
  (Xvfb, Openbox, x11vnc, noVNC, websockify, python3-tk) belong in the image, not on the Mac.
- `hmi_desktop` is idempotent for a running session: print its URL before building or creating another
  container. A second invocation must not fail with a Docker name conflict or silently replace the session.

## Native macOS builds

- Keep C++17 portable: include `<algorithm>` for min/max and do not capture structured bindings in
  lambdas (C++20). Use ordinary local variables when a lambda needs a pair's members.
- `fbdev.cpp` compiles its Linux implementation only on Linux; other platforms throw a clear error
  for fbdev while supporting SDL and off-screen output. `config.cpp` uses `_NSGetExecutablePath` on
  macOS instead of `/proc/self/exe`; Canvas can use system Arial Bold when Linux fonts are absent.
- Validate native support with `./ci test_hmi` and `./ci hmi --demo --screenshot build/hmi-macos-demo.png`.
  Core tests need loopback sockets; a sandbox that blocks binds can leave the fake server waiting.

## VAB ship report (2026-11-15, phase 1)

- In the `vab` scene `ShipReportPoller` (telemetry.cpp) polls `KSPHMI.Revision` at 2 Hz on the session's RPC
  connection and fetches `KSPHMI.ShipReport` only on a new revision. Any `RpcError` (no KSPSerialIO.Hmi.dll, no
  kRPC, other scene) or a rejected text leaves `Snapshot.ship` null: the static cards stay and the session lives.
  Log: `ship report unavailable (static VAB cards): ...` once per reason, `ship report: revision N, ...`.
- Format v1 lives in four places that must agree: the plugin's `ShipReportFormat.cs`, `src/shipreport.cpp`,
  `tools/kspsim/ships.py` and the fixture `hmi-cpp/tests/data/ship_report_v1.txt` (tested by all three;
  `check_ship_report_contract`). Bump `v` for anything an old Pi would misread; new line kinds are ignored.
- Apollo's VAB art is keyed only by size and `[vab]` config (fixed 8 stage rows, the target ring is art), never by
  the ship. `KSPHMI_SHOTS=DIR` writes `classic_vab*.png` / `apollo_vab*.png` from the fixture.
- End to end without the game: kspsim `--scene vab --ship heavy|kerbal_x|pending|none` and
  `ksphmi --address 127.0.0.1 --config <toml with discovery = false and the sim ports> --screenshot`.
- The TOML subset now accepts dotted tables (`[vab.destinations]`); destinations are `name = m/s`, max 12.
- TWR colours (2026-11-15): `twr_level()` in shipreport.cpp is the one rule for both skins and the apollo LAUNCH TWR /
  UPPER TWR lamps (`[vab] launch_twr_min/launch_twr_warn/upper_twr_warn`); only stages with delta-v are judged. The
  apollo VAB header uses its own lamp table (`VAB_LAMPS`, fitted on its own so the flight lamps keep their size);
  `ApolloSkin::on_lamp` reports every lamp evaluated live for tests.
