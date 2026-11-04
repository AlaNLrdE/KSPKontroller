Firmware
========

Target: Arduino UNO R4 (Renesas RA4M1, 32-bit, native USB serial). Sources are in
``arduino-sketch/KSPIODemo9``.

Files
-----

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - File
     - Content
   * - ``KSPIODemo9.ino``
     - Pin and bit defines, the packet structs, global state, ``setup()`` and ``loop()``.
   * - ``SerialCOMS.ino``
     - ``KSPBoardReceiveData`` (non-blocking frame receiver) and ``KSPBoardSendData``.
   * - ``HandShake.ino``
     - Replies ``3, 1, 4`` to the plugin's probe.
   * - ``Input.ino``
     - ``input()``: dispatches received packets and runs the idle timeout.
   * - ``output.ino``
     - ``output()`` and ``controls()``: read the panel and send ``ControlPacket``.
   * - ``utilities.ino``
     - LED indicators and packet id initialization.

Main loop
---------

.. uml:: diagrams/activity_firmware_loop.puml

Link state
----------

The firmware only transmits controls while it is connected. Any valid packet from KSP connects
it, and 2 seconds without one disconnects it and turns every LED off.

.. uml:: diagrams/state_connection.puml

Indicators
----------

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - LED
     - Condition
   * - Green
     - Lit while packets arrive.
   * - Yellow
     - G-force above 5, or remaining stage fuel below 50 percent.
   * - Red
     - G-force above 9, or remaining stage fuel below 5 percent.
   * - SAS, RCS, group 1
     - Mirror the action group state reported by KSP.

LED matrix
----------

The 12x8 matrix of the UNO R4 WiFi is drawn by ``matrix.ino`` into an 8x12 buffer and refreshed by the
library from a timer interrupt, so drawing never blocks ``loop()``. At power on it shows a "KSP" splash
with a loading bar, then a scanning dot while it waits for KSP. A blinking X means the link was lost.
The pixel at the top right is a heartbeat that blinks four times a second: if it stops, ``loop()`` is stuck.

While connected one of these screens is shown. The first four columns are bars, the other screens use the
whole matrix:

.. list-table::
   :header-rows: 1
   :widths: 15 85

   * - Screen
     - Content
   * - Fuel
     - Four 2-pixel bars: liquid fuel, oxidizer, monopropellant, electric charge (a single dot if the
       vessel has no such tank). The default in flight.
   * - Stage
     - Current stage as a number, one dot per stage below it. Shown on the pad and for 1.5 s after staging.
   * - Horizon
     - Artificial horizon: tilts with roll, moves with pitch, the ground is dotted.
   * - Vertical speed
     - A bar growing up or down from the middle in four steps (2, 10, 50, 200 m/s).
   * - SAS mode
     - One icon per autopilot mode, or a dotted line when SAS is off.

A warning (very high G or very low fuel) inverts the picture at 2 Hz. The screen button on pin 3 cycles
through the screens and back to automatic selection; wire it between pin 3 and GND.
``python tools/matrix_preview.py`` draws every screen with the real firmware, and
``python tools/matrix_preview.py --log KSP.log`` shows what the matrix displayed according to a saved log.

Recovering from a frozen board
------------------------------

Two protections are active. A 2 s hardware watchdog resets the board if ``loop()`` stops. If ``loop()``
keeps running but the USB link has been silent for 20 s after working (a wedged USB stack, which the watchdog
cannot see), ``Input.ino`` resets the board too. The ``KSPSerialIO[BOARD]`` lines in ``KSP.log`` show uptime,
loop and send times, packet counters and the reset cause, and a ``REBOOT detected`` line when it restarts.

Adding a control
----------------

#. Define the pin and, for a toggle, its bit in ``KSPIODemo9.ino``.
#. Configure the pin in ``controlsInit()``.
#. Read it in ``controls()`` and call ``MainControls()``, ``ControlGroups()`` or assign an axis
   field of ``CPacket``.
#. Do not change the size or order of ``ControlPacket`` without changing ``KSPIO.cs`` too.

USB serial requirements (UNO R4)
--------------------------------

The UNO R4 has native USB, and its ``Serial`` only transmits while the host asserts the **DTR** line.
A program that opens the port without DTR never receives anything, which looks exactly like a dead
board (no handshake reply, no LEDs). The plugin therefore sets ``DtrEnable`` on its port. The
baud rate must also match the sketch (``Serial.begin(115200)``): in tests the board only answered at
115200, so ``BaudRate`` in ``config.xml`` has to be 115200.
