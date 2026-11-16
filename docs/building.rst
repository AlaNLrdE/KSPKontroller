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
   * - ``./ci build_sketch_debug``
     - Compile the firmware with symbols and ``-Og`` into ``build/sketch-debug`` (see :doc:`debugging`).
   * - ``./ci build_KSPSerialIO``
     - Compile the plugin with msbuild, xbuild or dotnet, then ``KSPSerialIO.Hmi.dll`` (see below).
   * - ``./ci build_all``
     - Compile all three projects: the plugin, the firmware and the Pi HMI (``build_hmi``; in WSL on Windows).
   * - ``./ci deploy``
     - Build the plugin and install it into a local KSP. Lists the installs it finds and asks which one (see below).
   * - ``./ci save_log [LABEL]``
     - Copy ``KSP.log`` and ``Player.log`` of the last run to ``build/perf/LABEL/``. KSP overwrites them on every launch, so run it after each measured run, before starting the game again.
   * - ``./ci disable_mod`` / ``./ci enable_mod``
     - Move the plugin out of GameData and back, to compare performance with and without it. KSP must be closed.
   * - ``./ci deploy_all [PORT]``
     - Build the plugin and sketch, build/test/deploy the C++ HMI on the Pi, install the plugin and flash the board, then test the board. Requires KSP closed, a connected board and a reachable Pi with its dependencies and lingering configured. ``NO_TEST=1`` skips only the board test; Pi tests still run.
   * - ``./ci ksp_list``
     - List the KSP installs found on this machine.
   * - ``./ci install``
     - Install the already built plugin (same selection as ``deploy``). Needs ``./ci build_KSPSerialIO`` first.
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

For ``arduino:renesas_uno`` builds, core 1.6.0's bundled WDT library incorrectly declares
only ``architectures=renesas``. The normal and debug build commands copy that installed
library into ``build/wdt-compat/<core-version>/WDT``, add ``renesas_uno`` to its metadata
and pass the copy explicitly to arduino-cli. The watchdog implementation and Arduino15
installation remain unchanged. Cores with corrected metadata use their library directly.

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

Deploying to KSP
----------------

``./ci deploy_all`` covers all three production projects. It checks KSP, the board and Pi SSH/package/linger
prerequisites before building. After the local plugin/sketch builds, it runs ``deploy_hmi``, which copies
``hmi-cpp/`` and builds/tests it natively on the Pi before restarting ``ksphmi.service``. Only then does it
install the plugin and flash the Arduino, so an unreachable Pi, missing Pi prerequisite or failed HMI build/test
stops the command before those hardware writes. This is not a transactional deployment: a later plugin or board
failure does not roll back a successful Pi deployment. See :doc:`hmi` for the Pi's one-time setup and SSH alias.

``./ci deploy`` builds the plugin and copies ``KSPSerialIO.dll``, ``PsimaxSerial.dll`` and, the first
time, ``config.xml`` to ``GameData/KSPSerialIO`` of a local KSP install, and ``KSPSerialIO.Hmi.dll`` to
``GameData/KSPSerialIO/Plugins`` when it was built.

``KSPSerialIO.Hmi.dll`` (the Pi's VAB ship report, :doc:`plugin`) is a separate .NET 4.7.1 project,
``KSPSerialIO/KSPSerialIO.Hmi/KSPSerialIO.Hmi.csproj``, not part of ``KSPSerialIO.sln`` (which targets 3.5).
``./ci build_KSPSerialIO`` builds it right after ``KSPSerialIO.dll`` with ``-p:KSPDIR=<install>`` (chosen as for
``deploy``; set ``KSPDIR`` to skip the question) and the .NET 4.7.1 reference assemblies downloaded into
``build/refasm-net471``. It references the install's ``KSP_x64_Data/Managed`` assemblies and
``GameData/kRPC/KRPC.Core.dll``, read-only. Without an install or without kRPC it is skipped with a warning; the
main plugin builds either way. ``./ci verify`` reports it as ``ship report: installed``. An existing ``config.xml`` is
never overwritten. It refuses to run while KSP is open, because the game keeps the DLL locked.

The installs are detected, not guessed, and ``./ci ksp_list`` shows them:

* the one launched last, read from Unity's ``Player.log`` (``Mono path[0]``), listed first;
* every Steam library in ``libraryfolders.vdf``;
* common GOG and ``Games`` folders on drives C: to H:.

With one install it is used directly. With several, ``ci`` prints a numbered list and asks which to
use (Enter picks the first). Without a terminal it stops instead of choosing, so scripts must set
``KSPDIR``, which also skips the question:

.. code-block:: text

   KSP installs found:
     1) C:/Users/alanl/wk/wk-rea/Kerbal Space Program  [last launched]
     2) D:/SteamLibrary/steamapps/common/Kerbal Space Program  [Steam]
   Install into which one? [1-2, Enter = 1]:

The default ``config.xml`` ships a macOS ``DefaultPort`` (discovery still finds the board on its own) and 115200 baud. An existing ``config.xml`` is never overwritten; ``RESET_CONFIG=1 ./ci deploy`` replaces it and keeps the old one as ``config.xml.bak``. Edit
``GameData/KSPSerialIO/PluginData/KSPSerialIO/config.xml`` in the install for your port (for
example ``COM5``) and ``BaudRate`` 115200 so KSP does not have to scan every serial port.

Requirements for the documentation
----------------------------------

* **Python 3** with ``pip`` (a virtual environment is created in ``build/venv``).
* **Java** to run PlantUML. ``plantuml.jar`` is downloaded to ``build/tools`` unless the
  ``PLANTUML_JAR`` environment variable points to one.
* **Doxygen** for the mod, firmware and HMI API reference.
* **Graphviz** for graph layouts used by PlantUML and Doxygen.
* Python packages are listed in ``docs/requirements.txt``.

Diagram conventions
-------------------

Diagrams are PlantUML sources in ``docs/diagrams``, one per file, and use standard UML notation:
class, sequence, state machine, activity and deployment diagrams. They are included with the
``uml`` directive, so editing a ``.puml`` file and rebuilding is all that is needed. Class
diagrams can use PlantUML's built-in layout engine; Graphviz is installed for other graph layouts.
The authoring and strict validation workflow is documented in :doc:`documentation`.

HMI and simulator development
-----------------------------

Use ``./ci build_hmi``, ``./ci test_hmi`` and ``./ci hmi --demo`` for a supported native toolchain (Linux,
macOS, or MSYS2 UCRT64 on Windows; WSL when MSYS2 is missing, see :doc:`hmi`).
``./ci test_hmi_container`` builds and tests in Docker; ``./ci hmi_desktop`` runs the real HMI and
kspsim GUI together. ``./ci test_hmi_desktop`` validates their GUI/protocol/browser path. See
:doc:`testing` for prerequisites, architecture selection, artifact paths and coverage limits.
