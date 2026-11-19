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
prerequisites before building. After the local plugin/sketch builds, it runs ``deploy_hmi``, which
cross-compiles the HMI here, copies it to the Pi and runs its tests there before restarting ``ksphmi.service``
(see `Cross-compiling the Pi HMI`_). Only then does it
install the plugin and flash the Arduino, so an unreachable Pi, missing Pi prerequisite or failed HMI build/test
stops the command before those hardware writes. This is not a transactional deployment: a later plugin or board
failure does not roll back a successful Pi deployment. See :doc:`hmi` for the Pi's one-time setup and SSH alias.

Cross-compiling the Pi HMI
--------------------------

Deploys from the PC or the Mac cross-compile the HMI (user, 2026-11-17: the Pi builds far slower than the PC);
on the Pi itself ``./ci install_hmi`` builds natively from its ``~/dev`` checkout (user, 2026-11-19, see
:doc:`hmi`). This PC (inside WSL) or a Linux machine builds ``ksphmi`` and its test programs for the Pi's
aarch64 Debian 13:

``./ci hmi_sysroot [--force]``
   Copies the Pi's headers, gcc runtime, crt objects, pkg-config files and the libraries SDL2, SDL2_ttf and
   libstdc++ need (``ldd`` closure, about 60 MB, about 30 s) into ``build/pi-sysroot``.
   ``hmi-cpp/cmake/pi-sysroot-files.sh`` lists them on the Pi. Absolute links are made relative, and ``lib``
   points to ``usr/lib`` (merged ``/usr``). The copy is made again only when ``build/pi-sysroot/.stamp``
   changes: the sha256 of the Pi's ``dpkg-query -W`` list plus the checksum of the file list.
``./ci build_hmi_pi``
   Runs ``hmi_sysroot``, then builds with ``hmi-cpp/cmake/aarch64-pi.cmake`` (``CMAKE_SYSROOT``, pkg-config
   limited to the sysroot) and ``-DKSPHMI_TEST_SOURCE_DIR=.``, so that the tests read ``config.toml`` and
   ``tests/data`` from their working directory on the Pi. The compiler is ``aarch64-linux-gnu-g++`` when it is
   installed. Otherwise Ubuntu 24.04's cross g++ 13 is unpacked without root (``apt-get download``), and so is
   Ninja. ccache is used when installed. On WSL the build tree, the toolchain and a mirror of the sysroot live
   on WSL's ext4 (``~/.cache/ksphmi-cross/<cksum of the checkout path>``), because NTFS through ``/mnt/c`` is
   about five times slower (108 s instead of 22 s cold, and 26 s instead of 0.1 s for a no-op). Only the
   programs are copied to ``build/hmi-cpp-pi``.

   Measured on this PC: 22 s cold, 6 s after touching one ``.cpp``, 4 s with nothing to do.

   It then checks with ``readelf -V`` that the Pi's own ``libc``/``libstdc++``/``libgcc_s`` (their copies in the
   sysroot) define every ``GLIBC_``/``GLIBCXX_``/``CXXABI_``/``GCC_`` version the programs need. A cross g++
   newer than the Pi's gcc 14 would build fine and then fail at start on the Pi. g++ 13 needs at most
   ``GLIBCXX_3.4.30``, and the Pi has ``3.4.33``.

Once installed, the toolchain is one line in WSL or Ubuntu: ``sudo apt install -y g++-aarch64-linux-gnu cmake
ninja-build``. Without sudo the unpacked copy is enough.

``./ci deploy_hmi`` uses this path whenever a cross compiler is available. It takes a lock on the Pi
(``~/dev/run/ksphmi/.deploy.lock``, so only one deploy runs at a time; a lock older than 30 minutes is taken over
with a warning). It then cross-builds, copies the programs and the hmi-cpp files (``config.toml``, assets, systemd,
test data) to ``~/.local/share/ksphmi.next`` and runs ``ksphmi_tests``, ``ksphmi_redux_tests``,
``ksphmi_intro_tests``, ``ksphmi_render_tests``, ``test_setup_kiosk.py`` and ``test_screenshots.py`` there. Only
when all of them pass does it install: ``~/.local/bin/ksphmi`` is replaced by a rename, the previous binary stays
as ``~/.local/bin/ksphmi.prev``, and the staged files (without the test programs) replace
``~/.local/share/ksphmi``. Then it restarts the service. A failed test leaves the running HMI untouched and the
new one in ``ksphmi.next`` for inspection. Without a cross compiler it prints a warning and falls back to an
on-Pi CMake build in ``~/.cache/ksphmi-build``.

``./ci install_hmi`` (on the Pi itself, Linux/aarch64 only) builds the same targets natively with the same
options (``-DKSPHMI_TEST_SOURCE_DIR=.``, Ninja when installed) into ``build/hmi-cpp-pi`` of that checkout, then
runs the same lock, staging, tests, install, restart and kiosk steps with local commands.

macOS (not implemented yet): clang is already a cross compiler. ``./ci hmi_sysroot`` works there. A Darwin
branch of ``build_hmi_pi_here`` would pass ``--target=aarch64-linux-gnu``, ``-fuse-ld=lld`` (Homebrew
``lld``) and the sysroot, and would use the Pi's own gcc 14 libstdc++ headers and crt files from
``usr/lib/gcc/aarch64-linux-gnu/14`` in the sysroot (already copied). The ABI check is then trivially
satisfied.

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
kspsim GUI together. ``./ci test_hmi_desktop`` validates their GUI/protocol/browser path. On macOS with
Xcode, ``./ci ios_build`` builds the iPhone HMI for the iOS Simulator and ``./ci ios_sim`` runs it there
against kspsim on loopback (:doc:`development/ios`). See
:doc:`testing` for prerequisites, architecture selection, artifact paths and coverage limits.
