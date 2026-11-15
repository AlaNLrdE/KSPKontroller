# KSPKontroller: shared contract for every Claude session and agent

Arduino UNO R4 WiFi hardware controller and Raspberry Pi flight display for Kerbal Space Program.
Three production projects: `KSPSerialIO/` (C# mod, .NET 3.5 / Mono in Unity),
`arduino-sketch/KSPIODemo9/` (UNO R4 firmware) and `hmi-cpp/` (C++17/SDL2 HMI).
Docs: `docs/*.rst` (Sphinx + Doxygen + PlantUML, published to GitHub Pages). Build/test tool: `./ci help`.

Read only what the task needs (grep + offsets, not whole files or logs). Docs are not imported here:

| Task | Read |
|---|---|
| Wire protocol, packet sizes | `docs/protocol.rst`, `docs/vesseldata.rst` |
| Plugin behaviour, settings, perf/port logs | `docs/plugin.rst` |
| Firmware, pins, LED matrix, recovery | `docs/firmware.rst`, `docs/overview.rst` |
| Raspberry Pi HMI (`hmi-cpp/`, C++, kRPC, `./ci deploy_hmi`) | `docs/hmi.rst` |
| Testing board/HMI without KSP (`./ci sim`) | `docs/simulator.rst` |
| Build, deploy, flash | `docs/building.rst`, `./ci help` |
| Architecture, channels, per-project internals | `docs/architecture.rst`, `docs/communication.rst`, `docs/development/*.rst` |
| Test coverage and documentation authoring | `docs/testing.rst`, `docs/documentation.rst` |
| Past decisions | `KSPSerialIO/CHANGELOG.md` (top entries), `git log -10` |

## Non-negotiable rules

1. **KSP must be closed** before deploying into `GameData` or probing the board with scripts (it owns COM5):
   `tasklist | grep -i KSP_x64`. Never touch `GameData` while it runs. **Flashing the sketch is allowed with KSP
   open** (user, 2026-11-14); it still needs the user's explicit OK. KSP may lose the port while the board resets:
   check the log afterwards (the plugin reconnects by itself).
2. **The `wk-rea` KSP install is shared with another Claude session** (`ksp-workbench-*`, owns the other
   mods). Before `./ci deploy` or flashing: SendMessage it, wait for "OK deploy", tell it when done.
   We only write `GameData/KSPSerialIO`, never `--remove`, never touch other mods. `./ci verify` checks it.
3. **Test without the game first:** `./ci sim` (tools/kspsim) drives the board and serves the Pi HMI with the
   real protocols; ask for a KSP session only for what the simulator cannot show.
   **The user runs the game; you read the logs.** KSP overwrites `KSP.log` at each launch: save it with
   `KSPDIR="C:/Users/alanl/wk/wk-rea/Kerbal Space Program" ./ci save_log LABEL` before it is lost.
   Useful tags in the log: `KSPSerialIO[PORT]`, `[CTRL]`, `[BOARD]`, `[LINK]`, and the `perf` lines.
4. **Run `./ci verify` before and after** deploys, flashes and game sessions. Every new kind of check goes into
   `tools/verify.py` as a `check_*` function (stdlib only), never as a one-off manual grep.
5. **Commit only after a successful build**; capture exit codes (`out=$(cmd 2>&1); rc=$?`), never
   `cmd | grep && git commit`. **Never push without the user's explicit OK for that push** (Pages publishes the docs).
6. Commit messages end with the co-author trailer the harness gives.
7. Reply to the user in Spanish; code, comments, docs and commits in English.
8. Bump the plugin version (`AssemblyInfo.cs` + top `CHANGELOG.md` entry, they must match) for every change that is deployed.

## Working with agents (learned from the ksp-workbench project)

- Parallel agents get **their own git worktree and branch** (`isolation: worktree`) and commit only on that branch.
  The coordinator merges with `build/venv/Scripts/python.exe tools/merge_agent.py <branch>` and then runs `./ci verify`.
  Agents never push, merge, deploy, flash or touch KSP. Without a worktree, give each agent disjoint files.
- Agent type: `kspcontroller-dev` (`.claude/agents/kspcontroller-dev.md`). Reports are short (under 150 words):
  what changed, what was verified, what could not be (needs the board or the game).
- Do not repeat what the other session or an agent already investigated: ask first (SendMessage), it costs less.
- Waiting for KSP to close: a background shell loop `while tasklist | grep -qi KSP_x64; do sleep 3; done`
  (the harness notifies when it ends). Never an agent polling.
- Every lesson learned goes into a skill (`.claude/skills/*/SKILL.md`) or this file before the task is finished.
  Session handoffs (what is deployed, what is pending) go in commit messages or the docs, not in this file.
- `AGENTS.md` (Codex) only points here: keep a single contract in this file.

## Documentation maintenance

- Sphinx/reStructuredText is the narrative source; UML lives in `docs/diagrams/*.puml`, included with `.. uml::`.
  Update affected architecture, channel and project guides with implementation changes. Use English.
  Sequence UML must declare lifelines and show explicit, balanced activations, returns and async messages;
  distinguish method execution from worker lifetime.
- README screenshots must be captured from the actual apps, labelled with demo/simulator provenance, and
  stored under `docs/images/` with regeneration notes. Lossless recompression may preserve pixels; never
  present mockups as runtime evidence. `check_documentation` validates local README image references.
- Validate `./ci diagrams` and `./ci sphinx` (warnings are errors). Doxygen inputs must cover both mod sources,
  the firmware and C++ HMI headers/sources. Keep generated SVG/HTML under `build/`, out of commits.
- `python3 tools/verify.py --docs-only` runs `check_documentation` for guide/UML/API coverage and balanced
  sequence activations without a game install. The documentation CI runs it and renders all PUML before Sphinx.
  On macOS a Java stub can exist without a working runtime; `ensure_plantuml` detects Homebrew's keg-only JDK.

## Tool gotchas (Windows, Git Bash)

- Repo text files are CRLF. Edit multi-line text with a small Python script that keeps CRLF (see the `edit()` helper
  pattern in `tools/` docs/skill) and `assert s.count(old) == 1`; anchors with `\n` silently fail otherwise.
- Heredocs containing quotes break in the Bash tool: write scripts with the Write tool, run them with
  `build/venv/Scripts/python.exe` (system `python` is not installed). `Write` needs a prior Read on existing files.
- `./ci upload_sketch` uploads the last compiled build: run `./ci build_sketch` first.
- Core 1.6.0 WDT metadata says `renesas` instead of `renesas_uno`: `compile_sketch` copies the installed
  library into `build/wdt-compat/` and fixes metadata only for normal/debug builds. Do not edit Arduino15
  or suppress compiler diagnostics; corrected upstream metadata bypasses the workaround.
- Helpers captured with command substitution must keep stdout machine-readable: `ensure_refassemblies`
  returns only the absolute reference-assembly path; download/status messages go to stderr, even on a cold cache.
- `./ci deploy_all` includes the Pi: preflight SSH/packages/linger, build plugin/sketch, then `deploy_hmi`
  (native C++ build + ctest + restart on the Pi), then plugin install and board flash/test. Pi failure stops
  before plugin install or flash; `NO_TEST=1` skips only the board test, never the Pi build/tests.
- A failed abrupt-close/reopen hardware test is a real reconnection failure even if the later idle-timeout
  test passes: silence alone does not prove the board is alive. Rerun against the installed firmware with KSP
  closed before changing it; intermittent failures may disappear on repeated tests and must not be called fixed.
- A board frozen with the orange LED not pulsing: double-tap RESET (orange pulses), or unplug USB for 10 s, then
  `./ci upload_sketch`. A plain RESET often does not recover it.
- HMI facility screens need KSPSerialIO UDP 50002 context as well as kRPC: stock kRPC active_vessel is null
  outside flight and does not distinguish the Space Center building dialogs. Use facility spawn/despawn events.
- HMI reconnects discover the PC by broadcasting the same context request on UDP 50002 after the saved address
  fails. Disable discovery in isolated client tests so an open real game cannot redirect their connections.
- Pi kiosk boot setup is `sudo sh ~/ksphmi/cpp/systemd/setup-kiosk.sh` (run by the user): hide boot text and
  firmware splash, mask tty1 getty, persist cursor settings; user-service lingering starts the HMI without login.
- On Windows, `hmi_ssh` prefers native Windows OpenSSH: Git Bash/MSYS SSH can abort long remote builds
  with `poll: No error`. Keep stdin available for piped tar transfers; do not blindly retry deployments.
- Raspberry Pi HMI: reach it only as `ssh ksphmi` (alias in `~/.ssh/config`; never put its address or user in
  the repo). Anything needing `sudo` there is run by the user. `hmi-cpp/` is LF (`.gitattributes`), not CRLF.
- The HMI is C++ only (`hmi-cpp/`, SDL2; the Python/pygame runtime was dropped 2026-11-14). The Pi builds it:
  `./ci deploy_hmi` copies it to `~/ksphmi/cpp`, runs cmake/ctest there and restarts `ksphmi.service`. On this PC
  `./ci build_hmi`/`hmi`/`test_hmi` run inside WSL (no Windows C++ toolchain). Tests and tools stay Python.
- HMI `Canvas::polygon` fills convex shapes only: split concave icons and ring sectors into triangles/quads.
- The DLL is UTF-16 inside: grep the installed DLL with `s.encode('utf-16-le')`, not plain `grep`.
- NavBall speed uses `orbit_speed_reference_frame` / `surface_speed_reference_frame` /
  `target_speed_reference_frame`: `orbital_reference_frame` co-moves with the vessel and reads zero speed.
  Attitude uses the surface-frame quaternion; vessel axes are right, forward, down, surface axes up, north, east.
- NavBall markings use bold compass bearings only on the 0 and +/-45 degree pitch bands, and regular pitch
  labels every 10 degrees. Lift horizon bearings 5 degrees into the blue hemisphere so their backgrounds
  do not cover the brown ground. The fixed yellow boresight includes a center dot. Marker
  edge margins must account for the complete glyph size, so larger symbols cannot spill outside the globe.
- NavBall volume lighting is a cached low-resolution alpha map, scaled once per size. Shade the globe and
  graduations before drawing navigation markers; avoid per-pixel work inside the Pi's frame loop (software
  renderer: opaque batches with `SDL_BLENDMODE_NONE`, static art cached per size).
- ORBIT must clip its contents to the panel interior and restore the caller's clip on every exit path (exceptions too):
  hyperbolic curves and distant vessel markers can extend arbitrarily far beyond the diagram scale.
- NavBall degree labels must not be hidden by screen-space rectangle collision tests: rolling changes rounded
  rectangles and causes flicker. Use continuous foreshortening and limb fading; pitch ladders have 10-degree
  major ticks and 5-degree minor ticks, with stroke widths scaled to display height.
- The +/-45 degree NavBall parallels use thin, long horizontal segments interrupted at the compass-bearing
  columns; keep these bands in addition to the local five-degree pitch ladder ticks.
- HMI geometry uses the plain `Point2` coordinate type in `vec.hpp` instead of returning
  `std::pair<double, double>`: GCC on ARM emits historical C++14/C++17 ABI notes for the latter.
  Rebuild all HMI targets after coordinate type changes; do not disable `-Wpsabi` globally.
- HMI numeric casts must validate before conversion: all integer configuration fields require finite, integral
  values within int range; clock UT must be below 2^63. `double(LLONG_MAX)` rounds to 2^63, so use an
  exclusive power-of-two bound and test the adjacent representable double with float-cast-overflow enabled.
- Keep the ten flight metric tiles in the upper-left block below 40% of screen height. Resources are vessel
  totals in KSP units (0 capacity = absent, 0 amount = empty, missing data = unknown); staging is read-only.
  Rebuild resource/stage collections after stage or parts changes, including docking and decoupling.
- kRPC server exceptions (InvalidOperationException, ArgumentException) reach Python as plain `RuntimeError`,
  not `krpc.error.RPCError`. On the launch pad `surface_speed_reference_frame` is singular and stage `delta_v`
  is unavailable: optional HMI streams catch `Exception`, drop only the failing marker or value and retry
  later, never rebuild every sample and never end the vessel session.
- ORBIT's live vessel marker must use Cartesian position in the body's non-rotating reference frame, with
  fixed view axes per vessel/body. Never anchor the camera or live marker to periapsis/true anomaly:
  near-circular RCS impulses can flip apsis direction without moving the vessel's position.


- Only `KSPSerialIO.Hmi.dll` (kRPC service KSPHMI, the Pi's VAB report) may reference kRPC; it targets .NET 4.7.1
  because kRPC 0.7 is a .NET 4 assembly, and builds against the install's Managed + GameData/kRPC (read-only). The
  report format v1 has four copies that must agree (plugin, hmi-cpp, kspsim, shared fixture): see the skills.
- Facility screens (VAB, Tracking Station, Astronaut Complex) follow an approved roadmap and order:
  `docs/hmi.rst` "Facility screens roadmap". All extend the one kRPC service `KSPHMI`; never a second service DLL.
- Commands for the user run in **PowerShell**: `$env:KSPDIR = "C:/Users/alanl/wk/wk-rea/Kerbal Space Program"`
  then `.\ci.cmd <target>`. Never bash `VAR=x ./ci`, never placeholders like `KSPDIR=...`.
- Agent branches live in `.claude/worktrees/<name>`: the user builds, deploys (`deploy`, `deploy_hmi`) and tests a
  branch from that folder. `./ci deploy_all` from the main checkout deploys `main`, not the branch.
- The only way to reach the `ksp-workbench` session is the user: `SendMessage` reaches only agents of the current
  session. Give the user a ready-to-paste "OK deploy?" request before a deploy and a notice afterwards.
- Cowork/VM shell (`device_bash`): the main checkout shows ~76 modified files that are only CRLF noise; never
  `git add -A`/`commit -a` there, commit explicit paths from a worktree. Git needs deletion permission once per
  session (`device_request_delete_permission`). No mono/cmake by default, no `ssh ksphmi`, no KSP: Windows builds,
  Pi deploys and game sessions are always the user's.
- `./ci verify` `log:` checks read the last saved KSP.log: after a deploy they stay FAIL (old plugin version, old
  link windows) until a new game session; judge only `installed == built` and `ship report: installed` then.

- Control packets use a latest-value buffer, but STAGE/SAS/RCS presses are events: capture their edges and
  receipt times on the serial reader before replacing that buffer. Decode/apply game controls only on Unity's
  thread; a latch in the main-thread decoder cannot recover packets already overwritten by the reader.
