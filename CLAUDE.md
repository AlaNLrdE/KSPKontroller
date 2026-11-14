# KSPKontroller: shared contract for every Claude session and agent

Arduino UNO R4 WiFi hardware controller for Kerbal Space Program. Two projects in one repo:
`KSPSerialIO/` (C# KSP plugin, .NET 3.5 / Mono in Unity) and `arduino-sketch/KSPIODemo9/` (firmware).
Docs: `docs/*.rst` (Sphinx + Doxygen + PlantUML, published to GitHub Pages). Build/test tool: `./ci help`.

Read only what the task needs (grep + offsets, not whole files or logs). Docs are not imported here:

| Task | Read |
|---|---|
| Wire protocol, packet sizes | `docs/protocol.rst`, `docs/vesseldata.rst` |
| Plugin behaviour, settings, perf/port logs | `docs/plugin.rst` |
| Firmware, pins, LED matrix, recovery | `docs/firmware.rst`, `docs/overview.rst` |
| Raspberry Pi HMI (`hmi-pi/`, kRPC, `./ci deploy_hmi`) | `docs/hmi.rst` |
| Testing board/HMI without KSP (`./ci sim`) | `docs/simulator.rst` |
| Build, deploy, flash | `docs/building.rst`, `./ci help` |
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

## Tool gotchas (Windows, Git Bash)

- Repo text files are CRLF. Edit multi-line text with a small Python script that keeps CRLF (see the `edit()` helper
  pattern in `tools/` docs/skill) and `assert s.count(old) == 1`; anchors with `\n` silently fail otherwise.
- Heredocs containing quotes break in the Bash tool: write scripts with the Write tool, run them with
  `build/venv/Scripts/python.exe` (system `python` is not installed). `Write` needs a prior Read on existing files.
- `./ci upload_sketch` uploads the last compiled build: run `./ci build_sketch` first.
- A board frozen with the orange LED not pulsing: double-tap RESET (orange pulses), or unplug USB for 10 s, then
  `./ci upload_sketch`. A plain RESET often does not recover it.
- Raspberry Pi HMI: reach it only as `ssh ksphmi` (alias in `~/.ssh/config`; never put its address or user in
  the repo). pip's pygame has no `kmsdrm`: the Pi uses Debian `python3-pygame`; anything needing `sudo` there
  is run by the user. `hmi-pi/` is LF (`.gitattributes`), not CRLF.
- The DLL is UTF-16 inside: grep the installed DLL with `s.encode('utf-16-le')`, not plain `grep`.
