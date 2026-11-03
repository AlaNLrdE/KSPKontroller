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
