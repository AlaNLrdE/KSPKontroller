KSP plugin
==========

Lifecycle
---------

#. **Main menu**: ``SettingsNStuff.Awake`` loads ``config.xml``.
#. **Flight scene load**: ``KSPSerialPort.Awake`` looks for the controller (see the discovery
   diagram in :doc:`protocol`). If it was already found in an earlier flight, only the port object
   and the reader thread are recreated.
#. **Flight scene start**: ``KSPSerialIO.Start`` opens the port, hooks ``AxisInput`` into the
   active vessel and pushes the current switch state to it.
#. **Every frame**: ``KSPSerialIO.Update`` (below) and ``KSPSerialPort.Update``.
#. **Scene unload**: ``KSPSerialIO.OnDestroy`` calls ``PortCleanup`` to stop the reader thread
   and close the port.

.. uml:: diagrams/activity_plugin_update.puml

Settings (``config.xml``)
-------------------------

.. list-table::
   :header-rows: 1
   :widths: 25 15 60

   * - Key
     - Type
     - Meaning
   * - ``DefaultPort``
     - string
     - Port tried first, for example ``COM3``. Other ports are then scanned.
   * - ``BaudRate``
     - int
     - Serial baud rate. It must be 115200 to match the sketch: the UNO R4 did not answer at other rates.
   * - ``refresh``
     - double
     - Minimum seconds between telemetry packets.
   * - ``HandshakeDelay``
     - int (ms)
     - Wait after opening a port before talking, for boards that reset on open.
   * - ``HandshakeDisable``
     - int
     - ``1`` skips the handshake and uses ``DefaultPort`` directly.
   * - ``PerfLog``
     - int
     - ``1`` logs the plugin's cost per frame every 10 s (see below). Default 0 when the key is missing.
   * - ``SASTol``
     - double
     - Obsolete since 0.19.3. It is still read so existing files load, but SAS is no longer switched off while steering.
   * - ``PitchEnable``, ``RollEnable``, ``YawEnable``, ``TXEnable``, ``TYEnable``,
       ``TZEnable``, ``WheelSteerEnable``, ``ThrottleEnable``, ``WheelThrottleEnable``
     - int
     - Axis mode, see below.

Axis modes
~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 10 90

   * - Value
     - Behavior
   * - 0
     - Always use KSP's own value; the controller is ignored.
   * - 1
     - Always use the controller's value.
   * - 2
     - Use KSP's value unless it is zero, then use the controller's.
   * - 3
     - Use the controller's value unless it is zero, then use KSP's.

Reading the port safely
-----------------------

``SerialWorker`` issues one read at a time and waits for it to complete, so the parser state is
only touched by one thread. The parser rejects zero-length packets, stops at the announced
size, and any exception in the worker is logged and resets the parser instead of leaving it
stuck. A completed packet is handed to the main thread through ``NewPacketBuffer`` guarded by
``SerialMutex``.

Port discovery and failures
---------------------------

Discovery probes ``DefaultPort`` first and then every COM port in the registry. Each port is probed
in isolation: an error opening it, a write timeout or a missing reply only closes that port and moves
on to the next one. Bluetooth modem ports (``\Device\BthModem*``) are skipped because they open but
never answer, and a port equal to ``DefaultPort`` is not probed twice.

Telemetry is only sent while a controller was found (``DisplayFound``). If sending fails three times in
a row, the port is closed, ``LinkFailed`` stops further attempts for the rest of the flight and a single
line per failure is logged, instead of blocking the main thread and logging a stack trace on every
packet. The write timeout is 250 ms.

Measuring the plugin's cost
---------------------------

With ``PerfLog`` set to 1 in ``config.xml`` (the default file enables it) the plugin writes one line to
``KSP.log`` every 10 seconds while a vessel is active:

.. code-block:: text

   KSPSerialIO: perf [10 s, 142 parts] frame 16.7 ms (60 fps) | plugin Update avg 0.021 ms max 0.85 ms = 0.13% of frame | sampling 50 x avg 0.310 ms max 0.85 ms | send avg 0.004 ms max 0.05 ms

* **frame**: average frame time of the whole game in that window.
* **plugin Update**: time spent in ``KSPSerialIO.Update`` per frame, and what share of the frame that is.
  It includes sampling the vessel and sending the packet, but not ``AxisInput`` or the reader thread.
* **sampling**: reading the vessel into ``VData``. This is the only part that grows with the number of parts.
* **send**: writing the packet to the port. A value near the 250 ms write timeout means the board is not
  reading.

To compare the game with and without the plugin, fly the same craft in the same place both times:

#. ``./ci deploy`` and fly for a minute; note the ``perf`` lines and the frame rate shown by an external overlay.
#. Close KSP, run ``./ci disable_mod`` and fly the same craft again, reading the frame rate with the overlay.
#. ``./ci enable_mod`` puts the plugin back.

The frame time in the log only exists while the plugin runs, so the external overlay (the NVIDIA or Steam
counter, for example) is what gives the number for the run without the plugin.

State exposed to other code
---------------------------

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Member
     - Meaning
   * - ``KSPSerialPort.DisplayFound``
     - A controller answered the handshake.
   * - ``KSPSerialPort.VData``
     - Telemetry that is sent on every refresh.
   * - ``KSPSerialPort.VControls``
     - Latest decoded controls. ``VControlsOld`` holds what was already applied.
   * - ``KSPSerialPort.ControlReceived``
     - A new control packet is waiting to be applied.
