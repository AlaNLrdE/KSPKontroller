System architecture
===================

KSPKontroller has three production projects: the Arduino panel firmware, the KSPSerialIO game mod,
and the C++ Raspberry Pi HMI. The game is the authority for vessel state. The panel supplies physical
control input; the HMI presents telemetry and does not issue flight-control commands.

System boundary
---------------

.. uml:: diagrams/system_components.puml
   :caption: Production components and their communication boundaries.

There are two independent telemetry paths. KSPSerialIO samples KSP into a fixed-size serial packet for
the Arduino. The HMI obtains richer data directly from the separate kRPC mod. It does not decode the
Arduino packet or depend on the Arduino being plugged in. KSPSerialIO also supplies a small UDP scene
context service, because kRPC's active vessel alone cannot identify Space Center building dialogs.

.. list-table:: Runtime ownership
   :header-rows: 1
   :widths: 23 32 45

   * - Project
     - Owns
     - Does not own
   * - ``arduino-sketch/KSPIODemo9``
     - Pin sampling, button debounce, control packets, LEDs, matrix and board recovery.
     - KSP vessel state or the Pi display.
   * - ``KSPSerialIO``
     - Serial transport, Unity-side telemetry and control application, scene context.
     - The kRPC server, which is a separate mod.
   * - ``hmi-cpp``
     - kRPC client, immutable display snapshots, instrument geometry, skins and output.
     - Arduino switches or writes to the vessel's controls.
   * - ``tools/kspsim`` (development support)
     - Simulated vessel state, test GUI and stand-ins for both host protocols.
     - Unity, real flight physics, the Pi's drivers or hardware timing.

Deployment views
----------------

.. uml:: diagrams/deployment.puml
   :caption: Physical deployment in the game setup.

The mod is a .NET Framework 3.5 assembly loaded by Unity/Mono. Its serial library is
``PsimaxSerial.dll``. The firmware is built for the UNO R4 WiFi's Renesas RA4M1. The Pi runs the
``ksphmi`` C++17 executable under a systemd user service; its output can use SDL kmsdrm/OpenGL or
fall back to Linux fbdev. Configuration and deployment commands are in :doc:`building` and :doc:`hmi`.

.. uml:: diagrams/deployment_desktop.puml
   :caption: Development deployment when KSP, the panel and the Pi are unavailable.

The Docker desktop runs the real Tk test app and C++ executable on a virtual X11 screen. noVNC
transports their pixels and user input to the browser. It is not a web rewrite of the HMI. Both apps
share container loopback; only browser port 6080 is published on the host's loopback address.

Data paths
----------

Panel input to vessel
~~~~~~~~~~~~~~~~~~~~~

#. Firmware samples enabled pins and updates ``ControlPacket``. SAS/RCS button presses flip bits;
   the bits represent toggle events across successive packets rather than authoritative game states.
#. The serial reader assembles and validates frames, then hands received data to the main thread.
#. The plugin decodes control state, detects relevant edges and applies changes on Unity's main thread.
#. ``AxisInput`` merges analog values with KSP's input according to each configured axis mode.
#. Later telemetry reflects the actual resulting game state back to the panel LEDs.

The startup baseline matters: a held stage button or a seeded SAS/RCS bit must not become a new press
just because a session reconnects. See :doc:`development/plugin` for those safeguards.

Vessel to HMI
~~~~~~~~~~~~~

#. ``KrpcSource`` connects the RPC and stream sockets to the kRPC mod.
#. A vessel session registers streams and reads navigation, systems and orbit data.
#. The source publishes a ``shared_ptr<const Snapshot>`` through its synchronized latest-value slot.
#. The display thread reads that snapshot without waiting for network I/O, selects the screen and skin,
   and draws through ``Canvas``.
#. ``Output`` presents through SDL, copies software pixels to fbdev, or saves an off-screen PNG.

The HMI has its own model; ``Snapshot`` is not a wire struct. Optional values distinguish unavailable
data from valid zero values. A failed optional marker or stage delta-v must not tear down a healthy
vessel session. A flight snapshot older than three seconds is considered stale by the model helper.

Editor ship to HMI
~~~~~~~~~~~~~~~~~~

#. In the VAB/SPH, ``KSPSerialIO.Hmi.dll`` builds a text report of the ship on editor and delta-v events,
   on Unity's main thread, and caches it with a revision number.
#. The kRPC mod serves it as the ``KSPHMI`` service (editor scene only); its procedures only read the cache.
#. While the UDP context says ``vab``, the HMI's source thread polls ``KSPHMI.Revision`` twice a second over
   the same RPC connection, fetches ``KSPHMI.ShipReport`` when the revision changes and publishes the parsed
   report in ``Snapshot.ship``.
#. The display draws it instead of the VAB cards; without it (no DLL, no kRPC, parse error) the cards stay.

Execution and synchronization
-----------------------------

.. list-table:: Execution contexts
   :header-rows: 1
   :widths: 25 43 32

   * - Context
     - Work
     - Boundary
   * - Unity main thread
     - Game API access, control application, received-packet dispatch, UDP context replies.
     - Background logs are marshalled through ``KSPLogQueue``.
   * - Serial reader worker
     - One outstanding read, parser progression, receive-time button edges and discovery handshake handling.
     - ``SerialMutex`` protects packet handoff; ``ButtonEvents`` locks its event queue.
   * - Serial sender worker
     - Blocking serial writes and write-failure accounting.
     - Latest pending telemetry frame, protected by ``sendLock``.
   * - Serial link supervisor
     - Failure detection, candidate enumeration and reconnect attempts.
     - Generation and stop event prevent obsolete workers from reconnecting.
   * - Arduino foreground loop
     - Receive, controls, debug reporting and matrix updates.
     - Library interrupts handle hardware work; there is no application worker-thread model.
   * - HMI source thread
     - Synchronous RPC calls, source state transitions and bounded UDP context reads.
     - Publishes immutable snapshots.
   * - kRPC stream reader
     - Stores the latest result for each stream ID and wakes waiters.
     - Mutex, condition variable and shutdown flags.
   * - HMI network identity worker
     - Resolves the configured server and enumerates local interfaces every 15 seconds.
     - Publishes cached text under a mutex; owns shared state until any pending DNS lookup ends.
   * - HMI display thread
     - SDL events, interpolation, drawing and presentation.
     - Owns rendering resources; no RPC calls in the frame loop.

Latest-value slots bound backlog: an obsolete telemetry frame or display snapshot is less useful than
the newest state. They are not durable event queues. STAGE rising edges and SAS/RCS bit transitions
use a separate receive-time event queue so replacing the latest controls cannot erase presses.
STAGE events expire after one second; this is not a general command acknowledgement mechanism.

Failure domains
---------------

The serial link, kRPC session and UDP context are separate failure domains. A missing Arduino should
not prevent HMI telemetry; an unavailable context reply should not stop drawing a valid vessel. A
missing kRPC connection cannot be repaired merely by successfully receiving UDP context.

Firmware has three recovery layers: a two-second input idle timeout, a two-second hardware watchdog
when the foreground loop stops, and a 30-second no-valid-packet reset after it has connected before.
The host serial supervisor uses its own silence and retry timers. The Pi service restarts a crashed
process; ``KrpcSource`` reconnects a lost server inside the still-running process. These mechanisms
solve different failures and must not be conflated.

Compatibility and design constraints
------------------------------------

* Serial packet layouts are shared contracts across C#, firmware and simulator. There is no negotiated
  field schema: changing a layout requires coordinated changes and size checks.
* The kRPC protocol version is 1 for the supported 0.7.0 client/server pairing. The C++ implementation
  encodes protobuf directly and uses the subset needed by the HMI.
* The ship report text has its own version (``v=1``); the Pi rejects other versions and ignores unknown line
  kinds, so fields can be added without breaking an older display. Only ``KSPSerialIO.Hmi.dll`` depends on kRPC.
* The serial transport can control the game. The HMI's UDP service is read-only context/discovery.
  The kRPC server is a wider external API; the HMI's read-only behavior does not restrict other clients.
* Rendering must fit the target hardware budget. Static art and glyphs are cached; target measurements
  decide whether full-frame or layer-cache rendering is faster.
* Simulation proves logic and protocol behavior. It cannot prove electrical wiring, USB-driver behavior,
  Unity integration, physical display output or Pi performance.

Detailed design
---------------

Read :doc:`communication` for the channel contracts, :doc:`protocol` and :doc:`vesseldata` for the serial
layout, and the individual development guides for implementation details:

* :doc:`development/arduino`
* :doc:`development/plugin`
* :doc:`development/raspberry-pi`
* :doc:`testing`
