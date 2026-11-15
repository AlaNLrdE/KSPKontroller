Simulator (testing without KSP)
===============================

``tools/kspsim`` stands in for KSP and both mods, so the Arduino panel and the Raspberry Pi HMI can be tested
without starting the game:

.. code-block:: text

   kspsim (PC) --- KSPSerialIO protocol, USB serial ------------ Arduino UNO R4 (unchanged firmware)
               \-- kRPC protocol, TCP 50000/50001 on the LAN --- Raspberry Pi HMI (unchanged code)

Both sides run the code they run with the game: the board gets the same handshake and 214-byte ``VesselData``
every 200 ms (the plugin's default ``refresh``), and the Pi's ``KrpcSource`` talks to a server that answers
the kRPC protocol (version 1, krpc 0.7 client). Nothing has to be reconfigured: the Pi already points at the
PC on port 50000. KSP must be closed (it owns the serial port and the kRPC ports); the simulator checks.

Running it
----------

.. code-block:: bash

   ./ci sim                      # window; first Arduino USB port; kRPC on all interfaces
   ./ci sim --port COM5          # a given serial port
   ./ci sim --no-board           # only the Pi        (--no-krpc: only the board)
   ./ci sim --headless           # no window, one status line every 2 s
   ./ci sim --node 250,900       # a 250 m/s prograde maneuver node 900 s ahead (the HMI's dashed orbit)
   ./ci sim --scene vab --ship heavy  # start in the VAB with a sample editor ship (KSPHMI service)
   ./ci test_sim                 # tests, also part of ./ci verify

The first time Python listens on the LAN, Windows asks whether to allow it: allow it on **private** networks,
or the Pi cannot connect.

The window
----------

* **Game**: *Flight*; *Space center* (the board gets no telemetry and goes idle, the HMI shows
  ``NOT IN FLIGHT``); *KSP closed* (kRPC ports closed: the HMI shows ``OFFLINE`` and reconnects when the
  game "starts" again). *HMI skin* (``config`` / ``classic`` / ``apollo``, or ``--skin``) adds
  ``skin=<name>`` to the UDP 50002 context reply: the Pi HMI switches its look at once (``config`` sends no
  field, as KSPSerialIO, so the HMI's own setting decides). Changes are logged as ``HMI skin ...``.
  *VAB ship* picks what the ``KSPHMI`` kRPC service (``KSPSerialIO.Hmi.dll`` in the game) reports in the VAB and
  SPH: ``kerbal_x`` (330 m/s short of a Mun landing), ``pending`` (delta-v not computed yet), ``heavy`` (launch
  TWR 0.74, sandbox: no funds), ``empty`` (no parts, as after New: the Pi shows EMPTY CRAFT) or ``none`` (no service, as without the DLL: the Pi keeps its VAB cards). Each
  choice is a new revision; outside the editor scenes the service answers kRPC's "not available in game scene"
  error. Same as ``--ship`` / ``Simulator.set_ship``; the samples are in ``tools/kspsim/ships.py``.
* **Scenario** loads a new vessel, as switching vessels in KSP does (the HMI re-opens its streams):
  on the pad, an 80 x 250 km orbit, a sub-orbital arc falling back through the atmosphere, an escape
  trajectory (hyperbolic, the HMI shows ``Escape``).
* **Time warp** 1x to 1000x, or *Pause* to hold every value still.
* **Set the current vessel**: apoapsis, periapsis (km), inclination, position on the orbit (degrees from
  the periapsis: 0 = at Pe, 180 = at Ap), name and stage fuel. *Apply* moves the same vessel onto that orbit
  (no vessel switch) and it keeps flying from there; impossible requests (Ap below Pe, a point under the
  surface, beyond the sphere of influence) are refused with the reason. *Read current* fills the fields
  from the vessel, to change one value and keep the rest.
* **Maneuver node**: prograde, normal and radial-out delta-v (m/s) and the time to the node (s); *Create /
  update node* and *Delete node* change what kRPC ``Node`` / ``Node.orbit`` answer at once (the HMI's dashed
  orbit and NavBall node marker follow within about 2 s), and the tab shows the node's dV, T- and the
  predicted Ap/Pe after the burn. ``--node DV,SECONDS`` creates one at start. Changes are logged to
  ``build/kspsim.log`` (``maneuver node created / updated / deleted``).
* **Manual values**: tick *Manual mode* and the physics freezes; every value the board and the HMI receive
  (altitude, vertical / orbital / surface speed, Ap, Pe, times to Ap / Pe, inclination, eccentricity, period,
  semi-major axis, true anomaly, G, situation) starts from its current value and is sent exactly as typed,
  even when the set makes no physical sense. ``inf`` and ``nan`` are accepted, to test how the screens cope;
  the board never receives them (VesselData carries 0, as the plugin would). Untick to resume the physics
  from where the vessel was. Without manual mode every value comes from the flight model, so they change
  as the vessel moves (on the 80 x 250 km orbit the vertical speed swings between about +250 and -250 m/s).
* On the pad the apoapsis is about 0 m and the periapsis about -598 km: that is the "orbit" of a point
  turning with the surface, and KSP shows the same values.
* **Controls**: while ControlPackets arrive the board drives the vessel and the window only mirrors what it
  sends (throttle, SAS, RCS, lights, gear, brakes, action groups, pitch/roll/yaw). Without the board, or
  3 s after it goes quiet, the window's own throttle, switches, STAGE button and *Stick (hold)* buttons
  (pitch / yaw / roll at full deflection while held) take over. The *Vessel* box shows the attitude
  (pitch, heading, roll) and what steers it: the launch guidance, SAS off, or the SAS mode and its error.
  *Use the board's throttle* (on by default; ``--no-board-throttle``) can be unticked: the engine then
  follows the window's slider while everything else still comes from the board. A throttle axis that
  does not rest at 0 % (the panel's reads about 19 %) otherwise keeps the engine burning, as it would in KSP.
* **Game state / LEDs** tab: what KSP reports as active in ``VesselData.ActionGroups`` (SAS, RCS, lights,
  gear, brakes, abort, action groups 1-10) and the SAS mode (the icon of the SAS matrix screen). Change it as
  keys in the game would, without touching the board: the SAS, RCS and group 1 LEDs follow. The board's
  bits are applied the way KSPSerialIO 0.19.3.14 does: a change of the SAS / RCS bit is one button press and
  toggles the state (the board flips its bit on each press), the other bits set their level when they change,
  and the first packet of a link only seeds them. So a state changed in this tab survives the keep-alives and
  the next board press still toggles it (see :doc:`firmware`, *Feature status*).
  The *caution / warning triggers* pin G force, stage fuel and ``MaxOverHeat`` (overheat is sent, but the
  firmware does not use it) over the flight model and manual mode; the firmware thresholds are shown next
  to them, and a line below says which LEDs the board should light with the values it is receiving.
* **Board tests** tab: one button per LED matrix screen sends a ``DisplayCommand`` (id 2), which forces that
  screen for 4 s (*Keep it* repeats it every 3 s; *Automatic* hands back). *Pause telemetry* stops every frame
  to the board: it should go idle after 2 s (its last packet arrives about 2 s into the pause) and reset
  itself after 30 s; the link then reconnects and ends the pause. Untick earlier and the next ``VesselData``
  reconnects it. *Bad checksum* and *wrong size* frames should raise ``rx_bad_cs`` and ``rx_bad_size`` in the
  next DebugPacket; *Re-handshake* repeats the handshake on the open port. The event list shows what was sent
  and what the board reported (counters that grew, a reboot and its cause), and the matrix picture is the one
  in the last DebugPacket.
* Link panels: board state, packets each way, bad frames, the board's DebugPacket decoded (uptime, loops,
  loop and send maxima, receive counters, reset cause, flags); the kRPC clients connected with their calls
  and streams.

The flight model
----------------

``physics.py`` is deliberately small: a planar orbit around Kerbin (real radius, gravitational parameter,
rotation and 70 km atmosphere), RK4 integration, exponential air density with drag, a rotating surface to
stand on and land on, and three identical stages of about 4.8 km/s each. The STAGE bit stages on its rising
edge, like the plugin.

Attitude is a full 3D orientation (the vessel's forward and top axes in the inertial frame) even though the
orbit is planar. The rule for what points the vessel and its engine:

* Until the vessel is **steered** it follows a fixed ascent guidance (straight up, a gradual turn to the
  horizon by 45 km, then prograde), so a launch needs only the throttle: full throttle from the pad reaches
  orbit unattended. The vessel becomes steered, until the next scenario load, as soon as SAS is on (game
  state) or a pitch / yaw / roll input exceeds 5 % (board axes or the window's stick buttons).
* Once steered, the engine thrusts along the nose. Only the part in the orbital plane acts, so a normal or
  anti-normal burn changes nothing but the attitude (the inclination is reported, not modelled).
* **SAS on** slews to the selected mode's direction at up to 20 deg/s, accelerating and braking at 20 deg/s2
  (a 180 degree turn takes about 10 s), and keeps the roll. Stability assist stops the rotation and holds.
  Prograde / retrograde follow the NavBall speed mode (``speed_mode`` 0 orbit, 1 surface, 2 target:
  ``relative_*``); normal (south for these eastward orbits, as ``TransformDirection``) and radial follow that
  prograde. Target / anti-target use ``target_*`` and maneuver uses the node's burn vector; without a target or
  node they hold. Mode numbers are ``Simulator.sas_mode`` 1-10 (KSP ``AutopilotMode`` + 1), mapped to the kRPC
  ``SASMode`` enum by ``Control.get_SASMode``.
* **Stick input** (overrides SAS while held, as in KSP) applies 40 deg/s2 per full axis; without SAS the
  rotation is damped (about 27 deg/s at full input, stopping within 2 s after release). Landed, the attitude
  turns with the surface; on rails (a tick longer than 10 s) there is no rotation and SAS snaps to its
  direction.
* ``Vessel.report()`` gives pitch / heading / roll relative to the surface (NavBall); kRPC's
  ``Flight.rotation`` is ``attitude_rotation`` of them (vessel axes right, forward, down in the surface
  frame up, north, east) and VesselData carries them in ``Pitch`` / ``Roll`` / ``Heading``, with
  ``ProgradePitch/Heading`` (speed-mode prograde), ``NormalHeading``, ``ManeuverPitch/Heading`` and
  ``MNTime`` / ``MNDeltaV`` (node) and ``TargetPitch/Heading`` / ``TargetDist`` / ``TargetV`` as the plugin
  fills them. SAS on/off, mode changes, "on target" (within 1 degree) and the switch from guidance to
  steering are logged to ``build/kspsim.log``.

SAS, RCS, lights, gear, brakes, abort and the custom action groups go into ``ActionGroups`` from the game state
(above), which is what the board's LEDs show.

Not modelled: other bodies and sphere of influence changes, torque from RCS or reaction wheels (one fixed
rotation rate), real targets (``target_*`` values only), electric charge use beyond a slow drain. A maneuver node has a fixed UT (it
counts down) and prograde / normal / radial-out delta-v; its ``Node.orbit`` is the reported state coasted to
the node by 3D Kepler propagation (``physics.propagate3``) plus that delta-v along KSP's node axes.

Extending it
------------

* Every value sent comes from ``Simulator.reported()`` (the flight model's ``Vessel.report()``, overlaid with the
  manual values, then the game state (``action_groups``, ``sas_mode``) and the forced values). A new reported
  value goes into ``REPORTED_FIELDS`` and ``report()``; the window's manual panel picks it up by itself.
* Board-side test packets (``display_frame``, ``bad_checksum_frame``, ``decode_debug``) and the firmware
  thresholds (``GCAUTION`` ... ``LINKRESET_S``) are in ``arduino.py``; keep them in step with ``KSPIODemo9.ino``.
  ``BoardLink._session`` takes any object with ``read``/``write``, so ``test_kspsim.py`` runs it against a
  fake board.
* A new HMI value read through kRPC: add the procedure to ``KrpcServer._space_center`` (names and types are
  in the krpc client stubs, ``krpc/services/spacecenter.py``) and to ``test_hmi_client_end_to_end``.
* A new ``VesselData`` field or packet change: ``VESSEL_DATA`` / ``VESSEL_FIELDS`` in ``arduino.py`` mirror the
  firmware struct; ``test_sizes_match_the_firmware`` fails when they drift.


Editor modes
------------

``./ci sim --scene vab --editor-mode construction|actions|crew|cargo`` serves the selected VAB mode through the
existing KSPHMI report. The window's editor-mode and action-group selectors update its revision. In the editor
the board still receives a neutral 5 Hz heartbeat: SAS advances and RCS reverses the four modes, while stage,
axes and action-group levels leave simulated flight controls unchanged. A new serial link seeds retained bits
instead of treating them as presses. Reports include sample named stage components, actions, seats and inventory
data shared with ``hmi-cpp/tests/data/editor_report_v1.txt``. The scene switch resets the input baseline.
