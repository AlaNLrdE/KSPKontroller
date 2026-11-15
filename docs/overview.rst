Overview
========

What it does
------------

* **Telemetry**: KSP sends vessel data (orbit, altitude, speeds, resources, attitude, stage,
  action group state ...) to the controller several times per second.
* **Controls**: the controller sends the state of its switches and throttle back, and the plugin
  applies them to the active vessel (SAS, RCS, action groups, throttle, staging ...).
* **Indicators**: the firmware turns received telemetry into external LEDs and the UNO R4's 12x8 matrix.
* **Flight display**: the Raspberry Pi C++ HMI shows navigation, orbit, resources and staging using
  kRPC telemetry, plus scene/facility context from KSPSerialIO.
* **Development without hardware**: kspsim stands in for the game; a Docker desktop can run the
  test app and real C++ HMI together in a browser. See :doc:`testing`.

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
   * - ``hmi-cpp/``
     - C++17 HMI, CMake targets, headers, source, tests and Pi service/boot files
   * - ``KSPSerialIO/KSPSerialIO/HmiContext.cs``
     - Persistent read-only UDP scene/facility context responder
   * - ``tools/kspsim/``
     - Test GUI, simulated vessel and both host-protocol stand-ins
   * - ``tools/hmi-container/``
     - Debian HMI tests, graphical desktop launcher and GUI/transport smoke check
   * - ``ci`` / ``ci.cmd``
     - Build, documentation and install helper
   * - ``docs/``
     - This documentation (Sphinx + PlantUML)

Deployment
----------

.. uml:: diagrams/deployment.puml

The mod and firmware share a packed serial contract that the simulator also implements. The HMI
uses an independent TCP kRPC path and UDP context, as described in :doc:`communication`. A serial
layout change must update all serializers; it does not automatically add a value to the Pi display.

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
     - (free) Action group 1 switch, disabled: ``CG1PIN -1``
   * - none
     - input
     - Stage push button: disabled (``STAGEPIN -1``)
   * - A0
     - analog input
     - Throttle potentiometer: not wired, disabled (``THROTTLEPIN -1``, throttle sent as 0)
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
