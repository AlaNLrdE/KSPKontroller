# Agent roster (DRAFT, pending the user's approval)

Canonical responsibility roster for every agent on the LAN mailbox (protocol 4, `docs/communication.rst`).
Drafted by the coordinator pi-claude on 2026-11-18 from the agents' own cards (mailbox #251-#263) and
the user's orders relayed in #244, #261, #265, #267, #271 and #273. Responsibilities belong to the
**identity**, not to the session: a relief session inherits them.

Reviewed on every roll call and whenever an agent joins or leaves. Each entry carries its review date.

## Identities

| Identity | Machine | Repo (path) | Tool / mode | Backup |
|---|---|---|---|---|
| pi-claude (coordinator) | Raspberry Pi 4 (kspKontrollerPi) | KSPKontroller (`~/wk/KSPKontroller`) | claude, headless daemon (`tools/agent_daemon.py`) | pc-claude (acting coordinator) |
| pc-claude | Windows PC Alan-TR | KSPKontroller (`C:\Users\alanl\wk\KSPKontroller`) | claude, open session | mac-claude (code/docs/hmi-cpp only) |
| mac-claude | Mac (Alans-MacBook-Air) | KSPKontroller (`/Users/alan/wk/KSPKontroller`) | claude, open session (same session as mac-workbench) | pc-claude; pi-claude for the mailbox server |
| pc-workbench | Windows PC Alan-TR | ksp-workbench (`C:\Users\alanl\wk\decompilation\ksp-workbench`) | claude, open session (ksp-workbench-89) | mac-workbench (code); pc-claude only for the ksp-gamedata lock/ledger |
| mac-workbench | Mac (Alans-MacBook-Air) | ksp-workbench (`/Users/alan/wk/ksp-workbench`) | claude, open session | pc-workbench |
| pc-redux | Windows PC Alan-TR | KSP2_decompilation (`C:/Users/alanl/wk/KSP2_decompilation`, no remote) | claude, open session | any relief session in that repo (open question Q4) |

### pi-claude (reviewed 2026-11-18)

- Owns code: none exclusively. Coordination docs (this roster, `docs/communication.rst` roles) with the
  owner of the push; probe scripts it was assigned (`tools/latency/`, request #130).
- Owns resources: none of the locks. Runs on the host of the mailbox server (`agent-mailbox.service`,
  operated by mac-claude) and of the deployed HMI (`~/ksphmi`, read-only for pi-claude).
- Can: plan, assign one owner and the locks per job, watch `overdue`, reassign; read-only Pi/ksphmi
  diagnostics; Python tests and edits in its own worktrees; measurements scheduled by the user.
- Cannot: deploy, flash, push, merge, restart ksphmi, use sudo, touch `~/ksphmi`, GameData or boot
  config, build hmi-cpp on the Pi, edit other repos.
- Skills: raspberry-pi-hmi (read-only diagnosis side), kspsim-testing.

### pc-claude (reviewed 2026-11-18)

- Owns code: KSPKontroller `KSPSerialIO/` (plugin and the KSPHMI kRPC service), `arduino-sketch/KSPIODemo9/`,
  `tools/` (verify, kspsim, merge_agent, mailbox client, windows scripts), `ci`, the docs covering them,
  `AGENTS.md` and the `.agents/skills` mirror (absorbed from pc-codex). hmi-cpp shared with mac-claude:
  pc-claude does the WSL cross builds and Pi deploys. Merges agent branches into KSPKontroller main.
- Owns resources: wk-rea `GameData/KSPSerialIO` (lock `ksp-gamedata` + "OK deploy" with pc-workbench),
  UNO R4 on COM5 (`board-flash`), Pi HMI deploy (`pi-deploy`), push of KSPKontroller main (`push-main`).
- Can / cannot: builds plugin, firmware and aarch64 hmi-cpp; `./ci sim/verify/deploy/deploy_hmi/upload_sketch`.
  GameData deploys and flashes only with KSP closed, the user's OK typed in its chat and pc-workbench's OK;
  push only with the user's OK. Cannot open KSP, no sudo on the Pi.
- Skills: kspcontroller-workflow (plugin version bump, board recovery), kspsim-testing, raspberry-pi-hmi.

### mac-claude (reviewed 2026-11-18)

- Owns code: Mac side of hmi-cpp (macOS build, full-screen/Retina, Mac-run scenes such as MechJeb),
  the redux_source/KSP2ORB1 parser side, kspsim on the Mac, `ci` portability to bash 3.2. Plugin and
  firmware only when assigned.
- Owns resources: mailbox server operation on the Pi (moves, restarts, journal integrity); push of
  KSPKontroller main under `push-main` with the user's OK typed in its chat.
- Can: `./ci test_hmi`, `./ci hmi`, `./ci sim`, cross-build ksphmi as the fallback to the PC,
  `deploy_hmi` under `pi-deploy` when assigned. Cannot: wk-rea GameData, COM5, open KSP.

### pc-workbench (reviewed 2026-11-18)

- Owns code: ksp-workbench (all mods, Shared/, Tests/, deploy scripts, verify.sh, docs/site,
  WorkbenchVersion, CHANGELOG), Windows-only mod stacks (Parallax/Scatterer/EVE).
- Owns resources: wk-rea GameData for the workbench mods (`ksp-gamedata`), third-party mod install/removal
  in wk-rea (modding owner on Windows), Ships/ crafts, the arbiter ledger
  `C:/Users/alanl/wk/wk-rea/ksp-arbiter/ledger.md` and the "OK deploy" handshake. Push only with the user's OK.
- Cannot: KSPSerialIO, COM5, the board, Pi deploys, open KSP.

### mac-workbench (reviewed 2026-11-18)

- Owns code: ksp-workbench macOS side (BodyThemes stock path, Mac builds/tests, `tools/platform.sh`).
- Owns resources: the Mac's KSP GameData (`ksp-gamedata`), third-party mod install on the Mac, the
  ksp-workbench docs site; push of ksp-workbench master only with the user's OK typed in its chat.
- Cannot: Windows or Pi installs, open KSP.

### pc-redux (reviewed 2026-11-18)

- Owns code: the whole KSP2_decompilation workbench (VersionBanner, KSPKontroller.KSP2 incl. the HMI UDP
  server on 50003, AscentGuidance, KSP1CraftImport, part-import staging, deploy-all.ps1, tools, tests, docs).
- Owns resources: the Windows KSP 2 install (its own workspace deployment lock, not a mailbox lock);
  push of KSP2_decompilation with the user's OK.
- Cannot: KSP 1 GameData, Pi deploy, board flash, KSPKontroller main push, hmi-cpp parser (upstream).
  Redux UDP field changes are agreed with the KSPKontroller owner (pc-claude / mac-claude) first.

## Overlaps and proposed splits

- **mac-workbench vs pc-workbench** (same repo): split by machine. Each owns its own machine's GameData and
  deploys; macOS-specific code is mac-workbench's, Windows-only stacks are pc-workbench's. When both change the
  same mod at once, the coordinator assigns one owner per mod.
- **hmi-cpp** (pc-claude vs mac-claude): pc-claude owns cross builds and Pi deploys; mac-claude owns the
  macOS runtime and is the cross-build fallback. Shared code changes get one owner per request.
- **KSPSerialIO MechJeb work**: bridge/procedures/version = pc-claude; formatter/tests = mac-claude.
- **Mailbox server**: operated by mac-claude; pi-claude only has local host access for read-only checks.

## Resource locks

| Lock | Resource | Owners who may take it |
|---|---|---|
| `ksp-gamedata` | a KSP 1 GameData install | pc-claude (KSPSerialIO in wk-rea), pc-workbench (wk-rea workbench mods), mac-workbench (Mac install) |
| `board-flash` | UNO R4 on COM5 | pc-claude |
| `pi-deploy` | `~/ksphmi` on the Pi | pc-claude; mac-claude when assigned |
| `push-main` | KSPKontroller origin/main | pc-claude, mac-claude (always with the user's OK typed in their own chat) |

Mailbox text is never deploy, flash or push consent.

## Routing rules

1. A request with a single owner goes to that owner directly (FYI the coordinator).
2. A multi-step or multi-owner request goes to the coordinator, which splits it into chained REQUESTs,
   one owner each (e.g. "install mod X, then update feature Y" = modding owner first, then feature owner).
3. An unclear owner: the coordinator decides.
4. A direct user order to an agent wins; that agent reports it to the coordinator.

## Rule: no dead agents (session takeover, user 2026-11-18)

1. Each repo/machine declares its identity explicitly in its CLAUDE.md and in
   `build/agent-mailbox/<identity>.json`; never infer it from the cwd the server shows.
2. Before taking over, confirm the old session is dead: server state `gone` **and** no live process of the
   same agent on that machine/folder. Only then `register --takeover`. If the lease is alive (HTTP 409),
   work without the mailbox and tell the user.
3. Continue its work: `collect` + `pending`, read its history, resume what was in progress.
4. Announce with the same name: INFO HANDOVER to all ("<identity> resumed by a new session; pending: ...").
   The identity does not change, so the roster does not change.
5. If the old session returns and gets 409, it stops and tells the user; never force.
6. A session holding several identities (e.g. the Mac session: mac-workbench + mac-claude) takes over and
   watches every identity it holds.
7. An automatic ACK (collect's receipt) is **not** a sign of life: only a recent STATUS, RESULT or INFO
   written by the agent counts.

## Rule: backup coordinator (user 2026-11-18)

pc-claude is the backup coordinator. If pi-claude has not processed work (no STATUS/RESULT, an automatic
ACK does not count) for more than 30 minutes while it has pending REQUESTs, or its daemon log
(`build/agent-mailbox/pi-claude.daemon/`) shows failed runs, pc-claude takes the coordinator role
temporarily, announces it with INFO to all, and hands it back with INFO when pi-claude answers again.

## Retired identities

| Retired | Absorbed by | Date | Reference |
|---|---|---|---|
| pc-codex | pc-claude | 2026-11-18 | #261, #264, #265 (config kept, lease released) |
| ksp-workbench-42 (SendMessage-only session) | pc-workbench (session ksp-workbench-89) | 2026-11-18 | #232, #235 |
| old mac-claude session (gone 02:53Z) | current Mac session, same identity mac-claude | 2026-11-18 | #258 |
| legacy protocol-3 inbox `mac` | mac-claude (drained with `collect --legacy-inbox`) | 2026-11-18 | #267 |
| legacy protocol-3 inbox `pc` | pc-claude (drain requested) | 2026-11-18 | #267, #281 |
| pc-workbench2 (invitation never used) | none needed | 2026-11-18 | #232 |

## Storing the roster in each repo (after approval)

Each agent stores in its own CLAUDE.md only a pointer to this file plus its own section and the two rules
above, as a LOCAL commit. Shared repos: one editor per repo, named by the coordinator (proposal:
KSPKontroller = pc-claude for pc-claude/mac-claude/pi-claude; ksp-workbench = pc-workbench for both
workbench identities; KSP2_decompilation = pc-redux). The user authorizes the pushes together at the end.
