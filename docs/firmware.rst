Firmware
========

Target: Arduino UNO R4 (Renesas RA4M1, 32-bit, native USB serial). Sources are in
``arduino-sketch/KSPIODemo9``. For scheduler/module ownership and a development workflow, see
:doc:`development/arduino`; for channel semantics, see :doc:`communication`.

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
       Incomplete frames expire after 250 ms without a consumed byte, before parsing queued bytes;
       a reconnect handshake is preserved even when the host closes mid-frame and reopens before
       the separate 2 s link-idle timeout.
   * - ``HandShake.ino``
     - Replies ``3, 1, 4`` to the plugin's probe.
   * - ``Input.ino``
     - ``input()``: dispatches received packets and runs the idle timeout.
   * - ``output.ino``
     - ``output()`` and ``controls()``: read the panel and send ``ControlPacket``.
   * - ``utilities.ino``
     - LED indicators and packet id initialization.
   * - ``matrix.ino``
     - The 12x8 LED matrix: splash, waiting / lost animations, screens, screen button.
   * - ``debug.ino``
     - ``debugReport()``: the once-per-second ``DebugPacket``.

Feature status
--------------

What the current sketch does, and how to change it. All defines are in ``KSPIODemo9.ino``. ``tools/kspsim``
exercises every item without KSP (see :doc:`simulator`, *Game state / LEDs* and *Board tests* tabs).

.. list-table::
   :header-rows: 1
   :widths: 24 12 64

   * - Feature
     - Status
     - Behaviour, and how to enable or change it
   * - SAS button, pin 2
     - enabled
     - Each press flips the SAS bit (150 ms debounce, idle level learned while disconnected); the plugin
       (0.19.3.14+) toggles SAS on every change of the bit. ``SASPIN``.
   * - RCS button, pin 9
     - enabled
     - Same as SAS, for RCS. ``RCSPIN``.
   * - Screen button, pin 3
     - enabled
     - Cycles the six matrix screens, then automatic. ``SCREENPIN``.
   * - Action group 1 switch
     - disabled
     - ``CG1PIN -1`` (was pin 10; nothing is wired and an open pull-up input read as group 1 ON). Set a pin:
       sent as a level, pin HIGH sets group 1, so an open switch means ON and a switch closed to GND means OFF.
   * - Stage button
     - disabled
     - ``STAGEPIN -1``. Set a pin number: the STAGE bit is held 150 ms per press (``STAGEHOLD``).
   * - Throttle axis
     - disabled
     - ``THROTTLEPIN -1``, throttle sent as 0. Set the analog pin (``0`` for A0); deadband ``THROTTLEDB``,
       sent on a change of ``THROTTLEDELTA``.
   * - Other controls
     - not implemented
     - Lights, gear, brakes, precision and abort bits, groups 2-10, pitch / roll / yaw, translation, wheel
       axes, ``Mode`` and ``NavballSASMode`` are always 0. Add them in ``controls()`` (see *Adding a control*).
   * - Green LED, pin 12
     - enabled
     - On with any valid packet, off after ``IDLETIMER`` (2 s) without one.
   * - Yellow LED, pin 6
     - enabled
     - ``G > GCAUTION`` (5) or stage fuel ``LiquidFuelS / LiquidFuelTotS`` below ``FUELCAUTION`` (50 %).
   * - Red LED, pin 5
     - enabled
     - ``G > GWARN`` (9) or stage fuel below ``FUELWARN`` (5 %). Also inverts the matrix at 2 Hz. A stage
       without liquid fuel (``LiquidFuelTotS`` 0) raises neither.
   * - SAS / RCS / group 1 LEDs, pins 8 / 7 / 11
     - enabled
     - Mirror ``ActionGroups`` bits 0, 1 and 6 of each ``VesselData``.
   * - Overheat (``MaxOverHeat``)
     - not used
     - Received but no LED or screen reads it. Add a condition in ``Indicators()`` to use it.
   * - LED matrix
     - enabled
     - Screens below; ``SCR_*`` in ``matrix.ino``.
   * - ``DisplayCommand`` (id 2)
     - enabled (test tools)
     - Forces a matrix screen for 4 s: 0-5 the screens (fuel, stage, horizon, vertical speed, SAS mode, data
       rotation), 16-20 the label and 32-36 the value page of Ap, Pe, inclination, altitude, speed, 64 the
       waiting animation, 255 automatic. Other values are ignored. KSP never sends it; it counts as a valid packet.
   * - ``DebugPacket`` (id 102)
     - enabled
     - Once per second while connected (see :doc:`protocol`). ``txSkipped`` is never incremented (always 0).
   * - Idle timeout
     - enabled
     - ``IDLETIMER`` 2000 ms: disconnected, LEDs off, no more ControlPackets or DebugPackets, matrix shows X.
       Before reading new bytes, discard any incomplete receive frame; unread serial bytes are preserved so
       a new handshake can reconnect immediately.
   * - Link reset
     - enabled
     - ``LINKRESETMS`` 30000 ms without a valid packet, once connected since power on: ``NVIC_SystemReset()``.
   * - Watchdog
     - enabled
     - ``WATCHDOGMS`` 2000 ms: resets the board if ``loop()`` stops.

The SAS and RCS buttons and the plugin agree on *toggles*: the board flips its bit on every press, and the plugin
(0.19.3.14+) toggles the vessel's SAS / RCS on every change of the bit, after seeding the bits from the first
packet of a link or flight. A state changed in the game (T or R) no longer swallows the next board press, which it
did while the board sent the opposite of ``AGSAS`` / ``AGRCS`` and the plugin copied the bit.

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
with a loading bar, then the letters KSP with a moon orbiting a small planet below them while it waits for
KSP. A blinking X means the link was lost.
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
   * - Flight data
     - Apoapsis, periapsis, inclination, altitude and surface speed, one after another (see below).

Flight data pages. In flight the automatic mode rotates every 3 s through the fuel bars and then the five data
fields. Three 3x5 digits are all that fit in 12 columns, so each field takes two steps: first a label for 1.2 s
(a letter A, P, I, H or V and an icon: up arrow, down arrow, slope, stacked arrows, right arrow), then the value
for the rest of the slot. The value has up to three digits, or one decimal below 10 (``1.2``), a minus sign when
negative (two digits then). Non-finite values (infinity or NaN, such as an escape-orbit apoapsis) show
``---`` without unit dots. Finite magnitudes are capped before integer conversion at 999, or 99 after
a minus sign. Dots on the bottom row give the unit: one dot is metres (or m/s), two dots
kilometres (or km/s), three dots thousands of kilometres. Inclination is in degrees and has no dots. With the
screen button the data screen cycles the same way by itself. ``tools/matrix_preview.py`` draws every label and
value page with sample numbers.

A warning (very high G or very low fuel) inverts the picture at 2 Hz. The screen button on pin 3 cycles
through the screens and back to automatic selection; wire it between pin 3 and GND.
``python tools/matrix_preview.py`` draws every screen with the real firmware, and
``python tools/matrix_preview.py --log KSP.log`` shows what the matrix displayed according to a saved log.

Recovering from a frozen board
------------------------------

A 2 s hardware watchdog (WDT) resets the board if ``loop()`` stops. The USB link has its own recovery,
described below. The ``KSPSerialIO[BOARD]`` lines in ``KSP.log`` show uptime, loop and send times,
packet counters and the reset cause, and a ``REBOOT detected`` line when it restarts.

Reconnection
------------

The plugin can reconnect at any time (see :doc:`plugin`). What the firmware does to help:

* Any valid packet, and a handshake in particular, marks the board as connected again, so a reconnecting plugin
  only has to repeat the handshake. ``everConnected`` is recorded when the receiver accepts the packet,
  independently of matrix drawing or the startup splash. A connection lost during the splash still arms
  the 30-second recovery and shows the lost-link animation once the splash ends.
* If it was connected before and now hears nothing for ``LINKRESETMS`` (30000 ms), it resets itself with
  ``NVIC_SystemReset()``. A softer USB restart is not possible: on the renesas_uno core 1.6.0 ``Serial.end()``
  is an empty function and ``Serial.begin()`` returns at once when the port is already running. The hardware
  watchdog stays active.
* It sends less. A ControlPacket goes out only when a switch, button or group changed or the throttle moved by
  ``THROTTLEDELTA`` (6 of 1000), plus a keep-alive every ``CONTROLKEEPALIVE`` (200 ms). The UNO R4 USB serial
  is known to stall at random after long steady traffic, and this cut the traffic about eight times.
* Each frame is built in a local buffer and written with one ``Serial.write()`` call instead of one call per byte.

What did not work on this core, so do not try it again: (1) calling ``Serial`` (its ``operator bool``) from
``loop()`` to read DTR, because it runs ``tud_task()`` in main context while the USB interrupt runs it too
(the handshake then took seconds or never came); (2) writing straight to TinyUSB with ``tud_cdc_write()`` (the
board sent nothing); (3) gating writes on ``Serial.availableForWrite()``, which reads 0 (every frame was
skipped). The core's own ``_SerialUSB::write()`` loops without a timeout until the transmit FIFO has room, so
if the PC closes the port while the board is inside it, the board freezes until the link reset. One call per
packet makes that window a few microseconds instead of most of the time.

After a reset the board re-enumerates on USB and Windows may give it the same COM number or another one.
That is why the plugin scans all ports instead of waiting for the old one.

.. uml:: diagrams/state_link_recovery.puml

``tools/test_arduino.py`` has a ``reopen`` test (close and reopen the port twice, then an abrupt close with
60 telemetry frames in flight) that checks the handshake is answered again after each reopen. In ten rounds
the abrupt close still freezes the board about one time in five; it comes back by itself after the link reset
(about 15 s).

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
