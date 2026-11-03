Architecture
============

Plugin classes
--------------

The plugin is three Unity ``MonoBehaviour`` components plus a few data structures. Settings are
loaded once at the main menu, the other two components live in the flight scene.

.. uml:: diagrams/classes_plugin.puml

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Class
     - Responsibility
   * - ``SettingsNStuff``
     - Reads ``config.xml`` into static fields (port, baud rate, refresh rate, handshake
       delay, per-axis modes, SAS tolerance).
   * - ``KSPSerialPort``
     - Finds the controller, owns the port and the reader thread, parses and builds packets,
       and holds the shared state (``VData``, ``CPacket``, ``VControls``).
   * - ``KSPSerialIO``
     - Samples the active vessel into ``VData`` and sends it, applies received controls to the
       vessel, and injects the analog axes through ``AxisInput``.

Firmware structure
------------------

The Arduino IDE concatenates every ``.ino`` file of the sketch into one translation unit, so the
files below share all globals. They are split by responsibility.

.. uml:: diagrams/classes_firmware.puml

Threads
-------

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Thread
     - Work
   * - Unity main thread
     - ``KSPSerialPort.Awake`` (discovery), ``KSPSerialIO.Update`` (telemetry and controls),
       ``KSPSerialPort.Update`` (dispatches a completed packet).
   * - Serial worker thread
     - ``SerialWorker`` reads from the port one read at a time and runs the packet state
       machine (``ReceivedDataEvent``). Valid packets are copied to ``NewPacketBuffer`` under
       ``SerialMutex``.

During discovery the handshake reply is handled on the worker thread, because it arrives while
the scene is still loading and ``Update`` has not run yet.
