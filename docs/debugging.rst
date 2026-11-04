Debugging the firmware
======================

Two ways exist. The serial way needs nothing more than the USB cable. The hardware way (breakpoints,
stepping, variables) needs an external SWD probe, because the UNO R4 WiFi has no debug probe on board
(its USB port is the RA4M1 native USB, used for the sketch and the serial link, not for SWD).

Without extra hardware
----------------------

* ``KSPSerialIO[BOARD]`` lines in ``KSP.log``: the board sends a DebugPacket (id 102) once per second
  (:doc:`protocol`). The last line before a freeze shows uptime, loop counters, worst loop time, receive
  errors, the reset cause and the LED matrix frame. ``REBOOT detected`` marks an uptime drop.
  ``./ci save_log LABEL`` keeps a log before KSP overwrites it, ``tools/verify.py --log PATH`` analyses it.
* ``build/venv/Scripts/python.exe tools/matrix_preview.py [PORT]`` feeds test telemetry and prints the
  LED matrix as ASCII art (KSP closed). ``--log KSP.log`` decodes the ``matrix=`` field of a saved log.
* ``./ci verify --board`` (KSP closed) compiles nothing but talks to the board with the real protocol and
  checks handshake, packet sizes and checksums. ``./ci test_arduino`` compiles, flashes and tests.
* To add a new probe value, add a field to the DebugPacket (sketch ``debug.ino``, plugin decoder, ``docs/protocol.rst``
  and the size constants that ``tools/verify.py`` cross-checks).

With a debug probe (SWD, breakpoints)
-------------------------------------

What the Arduino core provides (checked in core 1.6.0): ``platform.txt`` ``debug.*`` and ``boards.txt``
``unor4wifi.debug.*`` for ``arduino-cli debug``, the OpenOCD script ``debugger/R7FA4M1AB.cfg``, the SVD
``svd/R7FA4M1AB.svd``, programmer definitions for CMSIS-DAP, ST-Link (hla) and J-Link, OpenOCD 0.11.0-arduino2
and ``arm-none-eabi-gdb`` 7-2017q4.

Hardware to add
~~~~~~~~~~~~~~~

* A probe: a CMSIS-DAP probe (for example a Raspberry Pi Debug Probe or any DAPLink) or a Segger J-Link. An ST-Link
  only works through OpenOCD's hla mode, which is not what this repo configures.
* Wires from the probe to the SWD pads of the board: SWDIO (RA4M1 pin P108), SWCLK (P300), reset, GND and
  a 3.3 V reference. Find the pads in the official UNO R4 WiFi schematic; they are unpopulated, so soldering
  is needed. This could not be checked here: confirm against the schematic before wiring.

Steps
~~~~~

1. Install the VS Code extensions recommended by ``.vscode/extensions.json`` (Cortex-Debug, C/C++).
2. Run the task ``Sketch: build debug`` (``./ci build_sketch_debug``): symbols, ``-Og``, output in
   ``build/sketch-debug`` (the normal ``build/sketch`` is not touched).
3. Run the task ``Sketch: upload debug build`` (normal USB upload, KSP closed). It is an upload of the debug
   binary; OpenOCD 0.11 has no flash driver for the RA4M1, so gdb cannot load the program itself.
4. Connect the probe and press F5 with ``UNO R4 WiFi: attach (CMSIS-DAP probe)`` or the J-Link variant.
   The configuration attaches, halts and runs to ``main``.

Things to expect
~~~~~~~~~~~~~~~~

* Halting the CPU stops the USB stack: the PC drops the serial port and the plugin sees the board vanish.
  Debug with the plugin not running, or accept the reconnection logic firing.
* The sketch starts the watchdog (``WATCHDOGMS``); if it keeps running while halted, the board resets.
  The RA4M1 normally pauses the watchdog counter in debug state, but this was not verified on hardware.
* ``-Og`` changes timing: do not judge loop times from a debug build.
* IntelliSense uses ``build/sketch-debug/compile_commands.json``; run the debug build once to create it.
