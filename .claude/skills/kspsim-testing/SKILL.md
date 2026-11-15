---
name: kspsim-testing
description: Testing the Arduino panel and the Raspberry Pi HMI without KSP using tools/kspsim (./ci sim), and extending the simulator. Use before asking the user for a game session, when a board or HMI change needs checking, or when changing tools/kspsim.
---

## When

Prefer the simulator to a KSP session for everything it can show: the board link (handshake, VesselData,
ControlPackets, LEDs, buttons), every HMI screen and state, reconnection. Ask for the game only for what it
cannot show (real mods, real flight dynamics, plugin behaviour inside KSP). It cannot run while KSP runs: both
want COM5 and port 50000 (it checks).

## Run and drive it

- `./ci sim` (window) / `--headless --seconds N` / `--no-board` / `--no-krpc` / `--port COM5` /
  `--no-board-throttle`. `./ci test_sim` (part of `./ci verify` as `check_sim`).
- The Pi needs no change: it already points at this PC's port 50000.
- To read what it is serving, connect a second krpc client to `127.0.0.1:50000` (`krpc.connect(name="diag")`):
  this is how "the vertical speed keeps growing" was shown to be orbital motion (Ap/Pe constant = no thrust).
- Values are physics unless **Manual mode** is ticked (then frozen and sent as typed, inf/nan allowed for edge
  cases, never inf on the serial wire). "Set the current vessel" moves the same vessel onto a typed orbit.

## Facts learned (2026-11-14)

- The simulator logs to `build/kspsim.log` (rotated 3 x 2 MB) as well as the console: read it instead of asking
  the user what the window shows. A status line (board state, detail, counters, DebugPacket, kRPC clients) is
  written on every state change and every 30 s; a failed handshake logs the bytes received per attempt, and
  "port completely silent" means the USB bridge delivers nothing (see kspcontroller-workflow, bridge stall).

- The user expects the values to be controllable from the test app ("para eso es"): every value reported must be
  settable from the window. `Simulator.reported()` is the single source for both links; add new values to
  `physics.REPORTED_FIELDS` and `Vessel.report()` and the manual panel picks them up.
- The board's throttle axis read a steady ~19 % with nothing wired to A0: the engine kept burning. The plugin
  ignores it (`ThrottleEnable=0`); the firmware now has `THROTTLEPIN -1`. The window can also ignore the board's
  throttle. Do not mirror a board value into a window control and then re-apply it (the slider kept 18 %).
- On the pad Ap ≈ 0 m and Pe ≈ -598 km is correct (KSP shows the same); the HMI must not show `-0 m`.
- The krpc 0.7 client calls `KRPC.GetServices` at connect and uses its pre-generated stubs: the stand-in only has
  to declare the SpaceCenter classes and the VesselSituation enum, and answer the procedures the HMI calls (names
  and return types: `krpc/services/spacecenter.py` in the venv). An unknown call returns an error naming it.
- `KrpcTest` drives the stand-in with the stock krpc 0.7 Python client over the procedures the C++ HMI
  reads (`hmi-cpp/src/telemetry.cpp`) and the UDP 50002 context for every view in `hmi-cpp/src/scenes.cpp`.
  The C++ client itself runs against kspsim only in `./ci test_hmi --sim` (Linux/WSL): when the HMI starts
  calling a new procedure, add it to the stand-in and to `KrpcTest`.
- Test GUI code without seeing it: build `App(sim, board, None)`, schedule `root.after(...)` actions and
  `root.destroy`, collect `report_callback_exception`, print the StringVars.
- Heredocs with quotes break in the Bash tool: write edit scripts with Write and run them with the venv python.

- Attitude (2026-11-14): the vessel has a 3D attitude (`Vessel.forward/top`, inertial, north = -z because the
  planar orbit's normal r x v is south as in `TransformDirection`). It follows the launch guidance until
  *steered* (SAS on or stick > 5 % since the scenario loaded), then SAS slews (`Vessel.sas_target`, modes
  `Simulator.sas_mode` 1-10) or the stick rotates it, and the engine thrusts along the nose (in-plane part).
  `physics.attitude_axes/attitude_angles` match `attitude_rotation` exactly (checked numerically: q maps the
  vessel's (0,1,0) to forward). Drive it in tests with `Simulator.step_vessel(dt)` (sets the SAS direction
  and logs "SAS on, mode X" / "SAS X: on target"); a headless check:
  `sim.set_game(sas=True); sim.sas_mode = 4` then read `build/kspsim.log`.
- `sed -i` in Git Bash rewrites a CRLF file as LF even when nothing matches: edit with the Python helper.

## Agents

- Parallel agents on kspsim get a worktree. The Agent tool's `isolation: worktree` refused once (it reused a
  stale locked worktree based on an old commit, path case `c:` vs `C:`): create one by hand,
  `git worktree add -b NAME .claude/worktrees/NAME main`, and give the agent absolute paths there; merge with
  `tools/merge_agent.py NAME`.
- A log-monitor agent waits for KSP with a background shell loop and streams KSP.log with the Monitor tool;
  it must not be started for a session whose prerequisites are missing (see the raspberry-pi-hmi skill).
- Maneuver nodes: window tab "Maneuver node", `./ci sim --node DV,SECONDS`, or `Simulator.set_node(prograde,
  seconds, normal, radial)` (fixed UT, counts down; None deletes); `node_preview()` gives the window's Ap/Pe line. `Node.orbit` is served as a separate
  object (id uid*10000+6000, outside the uid*10 slots, all taken); `physics.propagate` is universal-variable
  Kepler, so hyperbolic post-burn orbits work. For an HMI frame against the sim from WSL, run kspsim with
  the Windows venv on PYTHONPATH (see raspberry-pi-hmi) and `ksphmi --address 127.0.0.1 --screenshot`.
