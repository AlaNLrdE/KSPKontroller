Overview
========

What it does
------------

* **Telemetry**: KSP sends vessel data (orbit, altitude, speeds, resources, attitude, stage,
  action group state ...) to the controller several times per second.
* **Controls**: the controller sends the state of its switches and throttle back, and the plugin
  applies them to the active vessel (SAS, RCS, action groups, throttle, staging ...).
* **Indicators**: the firmware turns the received telemetry into LEDs (caution, warning, SAS,
  RCS, action group 1).

Repository layout
-----------------

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Path
     - Content
   * - ``KSPSerialIO/KSPSerialIO/KSPIO.cs``
     - The KSP plugin (C#, .NET Framework 3.5, loaded by Unity/Mono)
   * - ``KSPSerialIO/config.xml``
     - Plugin settings: port, baud rate, refresh rate, per-axis modes
   * - ``KSPSerialIO/Includes/``
     - Reference DLLs from KSP/Unity and ``PsimaxSerial.dll``
   * - ``arduino-sketch/KSPIODemo9/``
     - Arduino firmware, split across several ``.ino`` files
   * - ``ci`` / ``ci.cmd``
     - Build, documentation and install helper
   * - ``docs/``
     - This documentation (Sphinx + PlantUML)

Deployment
----------

.. uml:: diagrams/deployment.puml

The plugin and the firmware only share the serial link. Both sides define the same packed binary
structs, so any change to a packet layout must be made on both sides (see :doc:`protocol`).

Hardware used by the demo sketch
--------------------------------

.. list-table::
   :header-rows: 1
   :widths: 20 20 60

   * - Pin
     - Direction
     - Function
   * - 2
     - input (pull-up)
     - SAS push button: each press toggles SAS
   * - 9
     - input (pull-up)
     - RCS push button: each press toggles RCS
   * - 3
     - input (pull-up)
     - LED matrix screen button
   * - 10
     - input (pull-up)
     - Action group 1 switch
   * - A0
     - analog input
     - Throttle potentiometer
   * - 12
     - output
     - Green LED: link alive
   * - 6
     - output
     - Yellow LED: caution
   * - 5
     - output
     - Red LED: warning
   * - 8
     - output
     - SAS state LED
   * - 7
     - output
     - RCS state LED
   * - 11
     - output
     - Action group 1 LED
