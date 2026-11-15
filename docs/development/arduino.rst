Arduino firmware development
============================

Scope and entry points
----------------------

The production sketch is ``arduino-sketch/KSPIODemo9`` and targets the Arduino UNO R4 WiFi
(``arduino:renesas_uno:unor4wifi``). Its RA4M1 runs the panel logic; the sketch does not use the
board's Wi-Fi connection. The application communicates through native USB serial at 115200 baud.
The built-in 12x8 LED matrix complements the external status LEDs.

:doc:`../firmware` is the operational reference for pins, thresholds, screens and recovery.
:doc:`../protocol` and :doc:`../vesseldata` define the binary interface. This guide explains how to
change the implementation while preserving those contracts.

Module map
----------

.. uml:: ../diagrams/classes_firmware.puml
   :caption: Logical modules; Arduino concatenates the sketch files, rather than compiling classes per file.

``KSPIODemo9.ino`` defines packed packet types, pin/bit constants, timers and shared state.
``SerialCOMS.ino`` owns frame assembly and serialization. ``HandShake.ino`` constructs the discovery
reply. ``Input.ino`` dispatches inbound messages and monitors silence. ``output.ino`` samples inputs,
debounces buttons and decides when to send controls. ``utilities.ino`` maps telemetry to external LEDs.
``matrix.ino`` owns screen selection and pixel composition; ``debug.ino`` exports board health.

There is no per-file encapsulation: these files share globals after Arduino preprocessing. A rename
or type change can affect the entire translation unit. Protocol data, GPIO policy and drawing are
logical boundaries, not independent runtime services.

Initialization and scheduler
----------------------------

``setup()`` opens serial, records reset status, starts the watchdog, initializes LEDs and packet IDs,
configures enabled inputs, and initializes the matrix. Pins with a negative configured number are
disabled and must not be passed to GPIO/ADC APIs.

The foreground scheduler is deliberately small:

.. uml:: ../diagrams/activity_firmware_loop.puml

#. Record the maximum elapsed loop interval and increment diagnostic loop count.
#. Refresh the hardware watchdog.
#. ``input()`` consumes at most one completed packet and updates connection/idle state.
#. ``output()`` samples and sends controls when due.
#. ``debugReport()`` sends diagnostics when due and connected.
#. ``matrixUpdate()`` selects and composes the current frame.

Use elapsed ``millis()``/``micros()`` comparisons for periodic work. Adding an unbounded wait or long
animation delay stalls reception, button sampling and watchdog refresh. Matrix hardware refresh uses
the library's interrupt machinery; foreground drawing must still complete quickly.

Packet and state ownership
--------------------------

* ``VData`` is the latest accepted telemetry packet. Indicators read it; it is not an outgoing command.
* ``CPacket`` is outgoing physical control state. ``lastSent`` in the control path suppresses redundant
  frames until the keepalive deadline.
* ``HPacket`` is the handshake structure; ``DCmd`` is a test-tool display selection.
* ``Connected`` gates normal control/diagnostic traffic. A valid inbound packet updates the receive
  timestamp and turns the link LED on.

The receiver selects a known destination structure, accumulates partial input, checks the expected
size and XOR, and only then copies into shared state. Keep the C# layout and simulator serializer in
step with the sketch. ``int`` is 32 bits on this board, so wire fields use explicit widths and packed
layout. A ``ControlPacket`` must remain 25 bytes unless all peers are deliberately migrated.

Inputs and event semantics
--------------------------

The current physical controls are SAS on pin 2, RCS on pin 9 and the matrix screen button on pin 3.
SAS/RCS use pull-ups and debounce; idle level is learned while disconnected. A press flips the relevant
packet bit. The plugin uses bit changes to toggle game state, allowing keyboard input between presses.
The screen button stays local to the board and does not command the game.

Stage, custom group 1 and analog throttle are disabled by default with ``-1`` pin definitions. The
protocol supports more axes and flags than this particular panel implements. Enabling an axis also
requires a compatible plugin axis mode; wiring a potentiometer alone does not enable host control.

Sampling uses ``CONTROLREFRESH`` (25 ms). A changed control or throttle delta of at least six units
causes a send, with a 200 ms keepalive otherwise. Throttle uses the 0..1000 wire scale. Do not replace
button toggles with copies of the game's current SAS/RCS state: that loses presses after in-game changes.

Indicators and matrix
---------------------

External SAS/RCS/group LEDs show game-confirmed action-group bits. Caution/warning combine G-force
and stage fuel thresholds, guarded against absent fuel capacity. They do not report command delivery.
The matrix chooses connected flight screens, startup/waiting animation or link-loss indication; a
test ``DisplayCommand`` can temporarily override selection. The warning state can invert the frame.

Maintain one mapping from screen IDs to behavior. If adding a page, update ``matrix.ino``,
``tools/matrix_preview.py`` and the test app's board controls. Check overflow, negative values and
small display constraints; the panel has only 96 pixels.

Recovery and diagnostics
------------------------

.. uml:: ../diagrams/state_link_recovery.puml

The idle timeout (2 s), watchdog (2 s) and post-connection link reset (30 s) are independent. Idle
means the foreground loop still runs but no valid packet has arrived; the watchdog addresses a
stalled loop. A valid packet restores connected state. A hardware reset re-enumerates USB, which is
why the host must rescan rather than assume the COM number is permanent.

``DebugPacket`` exposes uptime, loop/send maxima, accepted/rejected packet counters, reset reason
and the matrix frame. Compare timestamps and reboot markers in ``KSPSerialIO[BOARD]`` logs before
changing watchdog thresholds. The current serial send builds one contiguous frame and writes it once;
its trailing 100 microsecond delay is intentional. USB core behaviors and unsuccessful recovery
approaches are recorded in :doc:`../firmware`.

Build and change workflow
-------------------------

.. code-block:: bash

   ./ci build_sketch
   ./ci build_sketch_debug
   ./ci test_sim

``build_sketch_debug`` produces symbols and ``-Og`` for the SWD workflow in :doc:`../debugging`.
Builds do not flash. ``./ci upload_sketch`` uploads the last compiled artifact; compile first, get
explicit approval for flashing, and follow the shared-install coordination rules in ``CLAUDE.md``.

For a new button, define/configure the pin, implement debounce and its intended level/edge semantics,
populate an existing control field where possible, and test its host interpretation in kspsim. For a
new packet field, update all three serializers and the field documentation before touching hardware.

Validation ladder
-----------------

#. Run consistency and simulator tests (:doc:`../testing`).
#. Exercise malformed frames, idle recovery and control behavior using the test app and a board.
#. Inspect ``DebugPacket`` counters and matrix previews; confirm real LEDs and button wiring.
#. Test real KSP only for behavior that simulation cannot prove, such as the real plugin's control
   application or USB-driver reconnection under game load.

A successful simulator test is not evidence that an unwired pin, electrical circuit or board watchdog
works. A successful firmware build is not evidence that the new artifact has been flashed.
