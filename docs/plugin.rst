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
