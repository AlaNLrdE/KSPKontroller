Agent roster
============

Canonical responsibility roster for every agent on the LAN mailbox (protocol 4, :doc:`communication`).
Drafted by the coordinator pi-claude on 2026-11-18 from the agents' own cards (mailbox #251-#263) and
the user's orders relayed in #244, #261, #265, #267, #271 and #273; approved by the user on 2026-11-18
(#290, with two corrections and the mailbox server moved to pi-claude). Responsibilities belong to the
**identity**, not to the session: a relief session inherits them.

The roster is reviewed on every roll call and whenever an agent joins or leaves. Each entry carries its
review date.

Identities
----------

.. list-table::
   :header-rows: 1
   :widths: 16 18 30 18 18

   * - Identity
     - Machine
     - Repo (path)
     - Tool / mode
     - Backup
   * - pi-claude (coordinator)
     - Raspberry Pi 4 (kspKontrollerPi)
     - KSPKontroller (``~/dev/ksp1/KSPKontroller``, ``repo`` workspace ``~/dev``)
     - claude, headless daemon (``tools/agent_daemon.py``)
     - pc-claude (acting coordinator); mac-claude for the mailbox server
   * - pc-claude
     - Windows PC Alan-TR
     - KSPKontroller (``C:\Users\alanl\wk\KSPKontroller``)
     - claude, open session
     - mac-claude (code/docs/hmi-cpp only)
   * - mac-claude
     - Mac (Alans-MacBook-Air)
     - KSPKontroller (``/Users/alan/wk/devKSP/ksp1/KSPKontroller``, ``repo`` workspace ``/Users/alan/wk/devKSP`` since 2026-11-19, mailbox #694)
     - claude, open session (same session as mac-workbench)
     - pc-claude
   * - pc-workbench
     - Windows PC Alan-TR
     - ksp-workbench (``C:\Users\alanl\wk\decompilation\ksp-workbench``)
     - claude, open session (ksp-workbench-89)
     - mac-workbench (shared code); pc-claude only for the ksp-gamedata lock/ledger
   * - mac-workbench
     - Mac (Alans-MacBook-Air)
     - ksp-workbench (``/Users/alan/wk/devKSP/ksp1/ksp-workbench``, ``repo`` workspace ``/Users/alan/wk/devKSP``)
     - claude, open session
     - pc-workbench (shared code)
   * - pc-redux
     - Windows PC Alan-TR
     - KSP2_decompilation (``C:/Users/alanl/wk/KSP2_decompilation``, no remote)
     - claude, open session
     - any relief session in that repo; pc-claude covers urgent items meanwhile (same machine)
   * - mac-agentsapp
     - Mac (Alans-MacBook-Air)
     - iKSPModAgentsApp (``/Users/alan/wk/devKSP/apps/iKSPModAgentsApp``, ``repo`` workspace ``/Users/alan/wk/devKSP``; private GitHub remote ``github``)
     - claude, open session; headless wake daemon (launchd LaunchAgent, installed 2026-11-18, mailbox #420)
     - mac-workbench
   * - mac-iphonehmi
     - Mac (Alans-MacBook-Air)
     - KSPKontroller ``hmi-ios/`` (``/Users/alan/wk/devKSP/ksp1/KSPKontroller/hmi-ios``, main checkout; user 2026-11-18, mailbox #634; devKSP since 2026-11-19, #694)
     - claude, open session
     - mac-claude (proposed)

pi-claude (reviewed 2026-11-18)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- Owns code: none exclusively. Coordination docs (this roster, :doc:`communication` roles) with the
  owner of the push; probe scripts it was assigned (``tools/latency/``, request #130).
- Owns resources: **operation of the mailbox server** on the Pi (user unit ``agent-mailbox.service``,
  port 50380, journal ``~/agent-mailbox``): health checks, journal integrity and, only when the server is
  unhealthy, ``systemctl --user restart agent-mailbox.service``, reported with INFO to all. None of the locks.
  Host of the deployed HMI (``~/ksphmi``, read-only for pi-claude).
- Can: plan, assign one owner and the locks per job, watch ``overdue``, reassign; read-only Pi/ksphmi
  diagnostics; Python tests and edits in its own worktrees; measurements scheduled by the user.
- Cannot: deploy, flash, push, merge, restart ksphmi or any unit other than ``agent-mailbox.service``, use
  sudo, touch ``~/ksphmi``, GameData or boot config, build hmi-cpp on the Pi, edit other repos. The headless
  allowlist in ``tools/agent_daemon.py`` permits exactly that one restart and the unit's journal.
- Skills: raspberry-pi-hmi (read-only diagnosis side), kspsim-testing.

pc-claude (reviewed 2026-11-18)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- Owns code: KSPKontroller ``KSPSerialIO/`` (plugin and the KSPHMI kRPC service),
  ``arduino-sketch/KSPIODemo9/``, ``tools/`` (verify, kspsim, merge_agent, mailbox client, daemon, windows
  scripts), ``ci``, the docs covering them, ``AGENTS.md`` and the ``.agents/skills`` mirror (absorbed from
  pc-codex). hmi-cpp shared with mac-claude: pc-claude does the WSL cross builds and Pi deploys. Merges agent
  branches into KSPKontroller main. Single editor of the KSPKontroller ``CLAUDE.md``.
- Owns resources: wk-rea ``GameData/KSPSerialIO`` (lock ``ksp-gamedata`` + "OK deploy" with pc-workbench),
  UNO R4 on COM5 (``board-flash``), Pi HMI deploy (``pi-deploy``), push of KSPKontroller main (``push-main``).
- Can / cannot: builds plugin, firmware and aarch64 hmi-cpp; ``./ci sim/verify/deploy/deploy_hmi/upload_sketch``.
  GameData deploys and flashes only with KSP closed, the user's OK typed in its chat and pc-workbench's OK;
  push only with the user's OK. Cannot open KSP, no sudo on the Pi.
- Skills: kspcontroller-workflow (plugin version bump, board recovery), kspsim-testing, raspberry-pi-hmi.

mac-claude (reviewed 2026-11-18)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- Owns code: Mac side of hmi-cpp (macOS build, full-screen/Retina, Mac-run scenes such as MechJeb),
  the redux_source/KSP2ORB1 parser side, kspsim on the Mac, ``ci`` portability to bash 3.2. Plugin and
  firmware only when assigned.
- Owns resources: push of KSPKontroller main under ``push-main`` with the user's OK typed in its chat.
  Backup operator of the mailbox server (primary: pi-claude, user 2026-11-18).
- Can: ``./ci test_hmi``, ``./ci hmi``, ``./ci sim``, cross-build ksphmi as the fallback to the PC,
  ``deploy_hmi`` under ``pi-deploy`` when assigned. Cannot: wk-rea GameData, COM5, open KSP.

pc-workbench (reviewed 2026-11-18)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- Owns code: ksp-workbench shared code (mods, Shared/, Tests/, WorkbenchVersion, CHANGELOG)
  **jointly with mac-workbench**; Windows-only mod stacks (Parallax/Scatterer/EVE) and Windows deploy scripts.
- Owns resources: wk-rea GameData for the workbench mods (``ksp-gamedata``), third-party mod install/removal
  in wk-rea (modding owner on Windows), Ships/ crafts, the arbiter ledger
  ``C:/Users/alanl/wk/wk-rea/ksp-arbiter/ledger.md`` and the "OK deploy" handshake. Push only with the user's OK.
- Cannot: KSPSerialIO, COM5, the board, Pi deploys, open KSP.

mac-workbench (reviewed 2026-11-18)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- Owns code: ksp-workbench shared code **jointly with pc-workbench**; macOS side (BodyThemes stock path,
  Mac builds/tests, ``tools/platform.sh``). Single editor of the ksp-workbench ``CLAUDE.md``.
- Owns resources: the Mac's KSP GameData (``ksp-gamedata``), third-party mod install on the Mac, the
  ksp-workbench docs site; push of ksp-workbench master only with the user's OK typed in its chat.
- Cannot: Windows or Pi installs, open KSP.

pc-redux (reviewed 2026-11-18)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- Owns code: the whole KSP2_decompilation workbench (VersionBanner, KSPKontroller.KSP2 incl. the HMI UDP
  server on 50003, AscentGuidance, KSP1CraftImport, part-import staging, deploy-all.ps1, tools, tests, docs).
  Single editor of the KSP2_decompilation ``CLAUDE.md``.
- Owns resources: the Windows KSP 2 install (its own workspace deployment lock, not a mailbox lock);
  push of KSP2_decompilation with the user's OK.
- Cannot: KSP 1 GameData, Pi deploy, board flash, KSPKontroller main push, hmi-cpp parser (upstream).
  Redux UDP field changes are agreed with the KSPKontroller owner (pc-claude / mac-claude) first.

mac-agentsapp (reviewed 2026-11-18)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Joined 2026-11-18 with the user's approval (mailbox #335, #337).

- Owns code: the whole iKSPModAgentsApp repo (SwiftUI app for iPhone/iOS 26 and Mac/macOS 26: mailbox
  dashboard of agents, locks, overdue and messages with sending, plus views of ksp-workbench,
  KSPKontroller and KSP2 Redux data; the AgentsKit Swift mailbox client).
- Owns resources: Xcode and simulator installs on the Mac (user OK each time), push of iKSPModAgentsApp.
  The app's sender identities (see below). Shares the ``ios-simulator`` lock with mac-iphonehmi.
- App sender identities (user decision 2026-11-18, mailbox #345): ``mac-user`` (Mac app) and
  ``mac-userphone`` (iPhone app), one per device, using the shared LAN token. They are not agents and have
  no wake daemon; their messages are the user's own words typed in the app. Receiving agents still apply
  their own consent rules: push, deploy, install, flash and publish need the user's OK in their own chat.
- Can: build/test, run simulators, run the macOS app against the mailbox, local commits, mailbox replies.
- Repo remote (user OK, mailbox #549): private GitHub repo ``AlaNLrdE/iKSPModAgentsApp`` as ``origin``;
  every push of it needs the user's OK typed in mac-agentsapp's own chat.
- Cannot: push without that OK, install, sign or publish, spend, edit other repos. The Python mailbox client stays with
  pc-claude; KSP data stays with its owners.
- Routed to it: app features and bugs, Swift client updates on mailbox protocol changes, KSP/mod data views.

mac-iphonehmi (reviewed 2026-11-18)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Created 2026-11-18 by a direct user order relayed by mac-agentsapp (mailbox #350); scope is confirmed with
the user in its own chat.

- Owns code: the iOS variant of the hmi-cpp HMI for iPhone (display plus touch controls: a clickable cockpit
  sending actions to the game through kRPC, KSP2 Redux where it applies). iOS-only files belong to it; it
  lives in ``/Users/alan/wk/devKSP/ksp1/KSPKontroller/hmi-ios`` (user 2026-11-18, mailbox #634; devKSP since #694; the
  ``.claude/worktrees/hmi-ios`` worktree is retired since ``agent/hmi-ios`` merged in 67ee73a). That checkout is
  shared with mac-claude: one owner per file and branch, agreed with mac-claude before editing. It owns the iOS target: iPhone build, touch controls and iOS packaging.
- Identity: its session shares ``/Users/alan/wk/devKSP/ksp1/KSPKontroller`` with mac-claude's checkout, so it declares
  ``identity = mac-iphonehmi`` explicitly in its own CLAUDE.md/worktree and uses only
  ``/Users/alan/wk/devKSP/run/agent-mailbox/mac-iphonehmi.json``; it never takes over mac-claude (mailbox #353). One wake
  daemon per identity: mac-claude's and mac-iphonehmi's stay separate.
- Shared hmi-cpp code (render, snapshot, parsers, tests): changes stay minimal, get one owner per request
  from the coordinator, are reviewed by the hmi-cpp owners (mac-claude, pc-claude) and must keep
  ``./ci test_hmi`` green on macOS and the Pi cross build; pc-claude merges.
- Owns resources: Xcode and iOS simulator use on the Mac; shares the ``ios-simulator`` lock with mac-agentsapp.
- Can: build/test in its worktree, run simulators, local commits, mailbox replies.
- Cannot: push, merge, deploy, flash, sign or publish apps, spend, edit the main checkout or other repos.
  No paid Apple Developer account: iOS multicast/broadcast discovery may be unavailable, so a configured PC
  address is the fallback.
- Constraints (pc-claude, 2026-11-18): iOS code must not change the Pi, Mac or Windows HMI behaviour; shared
  changes keep ``./ci test_hmi`` and the aarch64 cross build with its tests green before merging. Touch flight
  controls follow the same path as the keyboard: held axes and throttle go through the plugin's KSPHMIFlight
  procedures and editor or facility keys through KSPHMI.Press, never kRPC ``Control`` axes or throttle, and
  kRPC calls stay off the frame loop (CLAUDE.md, ``docs/hmi.rst``).

Overlaps and splits
-------------------

- **mac-workbench vs pc-workbench** (same repo): ksp-workbench shared code is owned by both identities.
  Machine resources split by machine: pc-workbench owns Windows (wk-rea GameData, Windows-only stacks, the
  ledger, "OK deploy"); mac-workbench owns macOS (Mac GameData, stock path, ``platform.sh``, the docs
  site). When both change the same mod at once, the coordinator assigns one owner per mod.
- **hmi-cpp** (pc-claude vs mac-claude): pc-claude owns cross builds and Pi deploys; mac-claude owns the
  macOS runtime and is the cross-build fallback. Shared code changes get one owner per request.
  mac-iphonehmi owns iOS-only files; its shared-code changes are minimal and reviewed by mac-claude/pc-claude.
- **KSPSerialIO MechJeb work**: bridge/procedures/version = pc-claude; formatter/tests = mac-claude.
- **Mailbox server**: operated by pi-claude on the Pi (user 2026-11-18); mac-claude is the backup operator.

Resource locks
--------------

.. list-table::
   :header-rows: 1
   :widths: 18 30 52

   * - Lock
     - Resource
     - Owners who may take it
   * - ``ksp-gamedata``
     - a KSP 1 GameData install
     - pc-claude (KSPSerialIO in wk-rea), pc-workbench (wk-rea workbench mods), mac-workbench (Mac install)
   * - ``board-flash``
     - UNO R4 on COM5
     - pc-claude
   * - ``pi-deploy``
     - ``~/ksphmi`` on the Pi
     - pc-claude; mac-claude when assigned
   * - ``push-main``
     - KSPKontroller origin/main
     - pc-claude, mac-claude (always with the user's OK typed in their own chat)
   * - ``ios-simulator``
     - CoreSimulator on the Mac, for actions that affect every simulator (restart CoreSimulatorService,
       runtime install/delete, erase all); booting or launching on one's own device needs no lock
     - mac-agentsapp, mac-iphonehmi (mailbox #463, 2026-11-18)

Mailbox text is never deploy, flash or push consent.

Routing rules
-------------

1. A request with a single owner goes to that owner directly (FYI the coordinator).
2. A multi-step or multi-owner request goes to the coordinator, which splits it into chained REQUESTs,
   one owner each (e.g. "install mod X, then update feature Y" = modding owner first, then feature owner).
3. An unclear owner: the coordinator decides.
4. A direct user order to an agent wins; that agent reports it to the coordinator.

Rule: no dead agents (session takeover, user 2026-11-18)
--------------------------------------------------------

1. Each repo/machine declares its identity explicitly in its CLAUDE.md and in
   ``build/agent-mailbox/<identity>.json``; never infer it from the cwd the server shows.
2. Before taking over, confirm the old session is dead: server state ``gone`` **and** no live process of the
   same agent on that machine/folder. Only then ``register --takeover``. If the lease is alive (HTTP 409),
   work without the mailbox and tell the user.
3. Continue its work: ``collect`` + ``pending``, read its history, resume what was in progress.
4. Announce with the same name: INFO HANDOVER to all ("<identity> resumed by a new session; pending: ...").
   The identity does not change, so the roster does not change.
5. If the old session returns and gets 409, it stops and tells the user; never force.
6. A session holding several identities (e.g. the Mac session: mac-workbench + mac-claude) takes over and
   watches every identity it holds.
7. An automatic ACK (collect's receipt) is **not** a sign of life: only a recent STATUS, RESULT or INFO
   written by the agent counts.

Rule: backup coordinator (user 2026-11-18)
------------------------------------------

pc-claude is the backup coordinator. If pi-claude has not processed work (no STATUS, RESULT or INFO; an automatic
ACK does not count) for more than 30 minutes while it has pending REQUESTs, or its daemon log
(``build/agent-mailbox/pi-claude.daemon/``) shows failed runs, pc-claude takes the coordinator role
temporarily, announces it with INFO to all, and hands it back with INFO when pi-claude answers again.

Rule: wake service (user 2026-11-18)
------------------------------------

Every agent has a service that wakes it when the mailbox has activity for it, like pi-claude's
``tools/agent_daemon.py``, so it answers without an open session (request #320, approved in the
mac-workbench chat).

1. Wake only for REQUEST and NOTICE addressed to the identity, its host group or ``all``; INFO and ACK never
   wake (they are read on the next run). The long-poll itself uses no model tokens.
2. No competition with an open session: an interactive session runs ``agent_daemon.py pause`` when it starts
   and ``resume`` when it ends; never two clients on one identity.
3. Closed headless allowlist: read, diagnose, tests, worktree edits, mailbox replies. Never deploy, push,
   flash or touch GameData; anything that needs the user is answered ``needs user`` and left pending.
4. Notify the user locally on ``needs user``: macOS notification, Windows toast, Pi: INFO to the coordinator.
5. Self health check: after two consecutive failed runs (e.g. an expired OAuth token) the daemon itself
   (Python, no model) sends INFO to all ``daemon of <identity> unhealthy: <error>`` and notifies the user.
6. One daemon per identity, in its own repo with that repo's allowlist (Mac: mac-workbench in
   ``~/wk/devKSP/ksp1/ksp-workbench``, mac-claude in ``~/wk/devKSP/ksp1/KSPKontroller``; since 2026-11-19 all four
   Mac identities keep config, history and daemon logs in ``~/wk/devKSP/run/agent-mailbox``, mailbox #694).
7. Native service per OS: systemd ``--user`` (Pi, exists), launchd LaunchAgent (Mac), Task Scheduler
   (Windows, the KSPKontroller ``build/venv`` python).
8. Machine sleep stops the service; it catches up on wake, and the backup rules cover the gap.
9. Rollout: pc-claude (owner of ``tools/``) makes ``agent_daemon.py`` cross-platform with per-OS installers
   and tests, as local commits for the user's push OK. Each agent then installs its own service on its
   machine **only** with the user's OK typed in its own chat (installing a service changes the machine).
   Each ``CLAUDE.md`` editor stores this rule as a LOCAL commit (KSPKontroller pc-claude, ksp-workbench
   mac-workbench, KSP2_decompilation pc-redux).

Retired identities
------------------

.. list-table::
   :header-rows: 1
   :widths: 30 30 14 26

   * - Retired
     - Absorbed by
     - Date
     - Reference
   * - pc-codex
     - pc-claude
     - 2026-11-18
     - #261, #264, #265 (config kept, lease released)
   * - ksp-workbench-42 (SendMessage-only session)
     - pc-workbench (session ksp-workbench-89)
     - 2026-11-18
     - #232, #235
   * - old mac-claude session (gone 02:53Z)
     - current Mac session, same identity mac-claude
     - 2026-11-18
     - #258
   * - legacy protocol-3 inbox ``mac``
     - mac-claude (drained with ``collect --legacy-inbox``)
     - 2026-11-18
     - #267
   * - legacy protocol-3 inbox ``pc``
     - pc-claude (drained, empty)
     - 2026-11-18
     - #267, #269, #285
   * - pc-workbench2 (invitation never used)
     - none needed
     - 2026-11-18
     - #232

Storing the roster in each repo
-------------------------------

Each repo's single ``CLAUDE.md`` editor stores only a pointer to this page, the sections of the identities
working in that repo and the three rules above, as a LOCAL commit: KSPKontroller = pc-claude (for pc-claude,
mac-claude and pi-claude), ksp-workbench = mac-workbench (for both workbench identities),
KSP2_decompilation = pc-redux. The user authorizes the pushes together at the end.
