Building and documentation
==========================

Everything goes through the ``ci`` script at the repository root (``./ci`` in Git Bash,
``.\ci.cmd`` in PowerShell or cmd).

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Command
     - Action
   * - ``./ci build_sketch``
     - Compile the firmware with arduino-cli (downloads it and the UNO R4 core if missing).
   * - ``./ci build_KSPSerialIO``
     - Compile the plugin with msbuild, xbuild or dotnet.
   * - ``./ci build_all``
     - Both of the above.
   * - ``./ci install``
     - Copy the plugin and ``config.xml`` to ``$KSPDIR/GameData/KSPSerialIO``.
   * - ``./ci docs``
     - Doxygen API reference into ``build/docs/html``.
   * - ``./ci diagrams``
     - Render every PlantUML diagram to SVG in ``build/diagrams``.
   * - ``./ci sphinx``
     - Build this site into ``build/sphinx`` (includes the Doxygen output under ``api/``).
   * - ``./ci upload_sketch [PORT]``
     - Upload the compiled sketch to the board (run ``build_sketch`` first). Finds the board by its FQBN when PORT is missing.
   * - ``./ci test_arduino [PORT]``
     - Compile, upload and test the board over USB serial (see below).
   * - ``./ci clean``
     - Remove build output.

Testing the board
-----------------

``./ci test_arduino`` compiles the sketch, uploads it to the board and then runs
``tools/test_arduino.py``, which talks to the Arduino with the
real wire protocol, without KSP. It needs Python 3; ``pyserial`` is installed into
``build/venv`` automatically.

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - Test
     - Passes when
   * - handshake
     - The plugin's probe ``1, 2, 3`` is answered with ``3, 1, 4``.
   * - telemetry
     - After one ``VesselData`` packet the board streams at least 3 ``ControlPacket`` frames per
       second (id 101, exactly 25 bytes, valid checksum) with a throttle in 0 to 1000.
   * - idle timeout
     - With no more telemetry the board stops sending after about 2 seconds.

Port selection: pass a port (``./ci test_arduino COM5``) or set ``ARDUINO_PORT``. If the board does
not answer there, or no port was given, every USB serial port is tried, Arduino boards (vendor id
``0x2341``) first. Bluetooth and other non-USB COM ports are skipped. The exit code is 0 when all
tests pass, 1 when one fails and 2 when no board answered.

Every run flashes the board. Set ``NO_UPLOAD=1`` to test the firmware that is already on it.
The board is found by the ``FQBN`` setting (UNO R4 WiFi by default), or pass the port. Close the Arduino
IDE's serial monitor and KSP first, since only one program can hold the port.

Requirements for the documentation
----------------------------------

* **Python 3** with ``pip`` (a virtual environment is created in ``build/venv``).
* **Java** to run PlantUML. ``plantuml.jar`` is downloaded to ``build/tools`` unless the
  ``PLANTUML_JAR`` environment variable points to one.
* **Doxygen** for the API reference.
* Python packages are listed in ``docs/requirements.txt``.

Diagram conventions
-------------------

Diagrams are PlantUML sources in ``docs/diagrams``, one per file, and use standard UML notation:
class, sequence, state machine, activity and deployment diagrams. They are included with the
``uml`` directive, so editing a ``.puml`` file and rebuilding is all that is needed. Class
diagrams use PlantUML's built-in layout engine, so Graphviz is not required.
