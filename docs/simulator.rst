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
   ./ci test_sim                 # tests, also part of ./ci verify

The first time Python listens on the LAN, Windows asks whether to allow it: allow it on **private** networks,
or the Pi cannot connect.

The window
----------

* **Game**: *Flight*; *Space center* (the board gets no telemetry and goes idle, the HMI shows
  ``NOT IN FLIGHT``); *KSP closed* (kRPC ports closed: the HMI shows ``OFFLINE`` and reconnects when the
  game "starts" again).
* **Scenario** loads a new vessel, as switching vessels in KSP does (the HMI re-opens its streams):
  on the pad, an 80 x 250 km orbit, a sub-orbital arc falling back through the atmosphere, an escape
  trajectory (hyperbolic, the HMI shows ``Escape``).
* **Time warp** 1x to 1000x, or *Pause* to hold every value still.
* **Set the current vessel**: apoapsis, periapsis (km), inclination, position on the orbit (degrees from
  the periapsis: 0 = at Pe, 180 = at Ap), name and stage fuel. *Apply* moves the same vessel onto that orbit
  (no vessel switch) and it keeps flying from there; impossible requests (Ap below Pe, a point under the
  surface, beyond the sphere of influence) are refused with the reason. *Read current* fills the fields
  from the vessel, to change one value and keep the rest.
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
  3 s after it goes quiet, the window's own throttle, switches and STAGE button take over.
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
stand on and land on, and three identical stages of about 4.8 km/s each. Thrust follows a fixed ascent
guidance (straight up, a gradual turn to the horizon by 45 km, then prograde), so a launch needs only the
throttle: full throttle from the pad reaches orbit. The STAGE bit stages on its rising edge, like the plugin.
SAS, RCS, lights, gear, brakes, abort and the custom action groups go into ``ActionGroups`` from the game state
(above), which is what the board's LEDs show.

Not modelled: other bodies and sphere of influence changes, attitude (pitch/roll/yaw are shown, not
applied), maneuver nodes and targets (sent as 0), electric charge use beyond a slow drain.

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
