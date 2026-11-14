Raspberry Pi HMI
================

The Arduino panel keeps KSPSerialIO and its serial port for buttons, LEDs and potentiometers. A Raspberry Pi 4
with a monitor shows the flight data next to it: apoapsis, periapsis, altitude, speeds, orbital elements and a
drawing of the orbit. The code lives in ``hmi-pi/``.

Why kRPC and not KSPSerialIO
----------------------------

KSPSerialIO serves exactly one serial port (the first board that answers the handshake), and the Pi would have
to pretend to be a USB serial device. Instead the Pi talks over the LAN to the `kRPC <https://krpc.github.io/krpc/>`_
mod running inside KSP. kRPC gives far more than ``VesselData`` (any vessel, orbit, body or part value) and can
also send commands, so touch controls can be added later without touching the plugin or the firmware.

.. code-block:: text

   KSP (PC) --- KSPSerialIO --- USB serial (COM5) --- Arduino UNO R4: buttons, LEDs, LED matrix
            \-- kRPC server --- TCP 50000/50001 over the LAN --- Raspberry Pi 4: HDMI monitor

How it works
------------

``python -m ksphmi`` runs two threads:

* a **source** (``ksphmi/telemetry.py``) publishes immutable ``Snapshot`` objects. ``KrpcSource`` connects to
  kRPC, opens streams on the active vessel and re-opens them when the vessel or the sphere of influence changes;
  it reconnects forever, so starting the Pi before KSP is fine. ``DemoSource`` flies an 80 x 250 km Kerbin
  orbit, so the screens can be worked on without the game.
* the **display** (``ksphmi/display.py``, pygame) draws the latest snapshot at ``fps`` frames per second. The
  layout is in units of the screen height, so it fits the 2520x1680 monitor, a TV or a window on the PC.

The header shows ``LIVE``, ``NOT IN FLIGHT`` (space center, editors), ``OFFLINE`` (no kRPC) or ``NO DATA``
(connected, but no new sample for 3 s: the game froze or the link stalled). A periapsis inside the atmosphere is
orange and one below the surface red.

On the Pi there is no desktop: SDL draws straight on the HDMI output with its ``kmsdrm`` driver.

.. important::

   The pygame wheels from pip bundle an SDL **without** ``kmsdrm`` (``pygame.error: kmsdrm not available``).
   The Pi uses Debian's ``python3-pygame`` (pygame 2.6.1 on the system SDL) through a venv created with
   ``--system-site-packages``; only ``krpc`` comes from pip. The PC venv pins the same pygame 2.6.1.

Settings
--------

``hmi-pi/config.toml`` (deployed) holds the defaults: kRPC ports and sample rate, ``fps``, screen size
(0 = the monitor's own) and ``day_hours`` (6 for the KSP calendar). On the Pi, ``~/ksphmi/config.local.toml``
overrides any key and is never touched by a deploy; ``~/ksphmi/env`` holds ``KSPHMI_KRPC_ADDRESS``, which
``./ci deploy_hmi`` sets to the PC's address as the Pi sees it (or to ``HMI_KRPC_ADDRESS`` if set).

One-time setup
--------------

On the Pi (Raspberry Pi OS Lite 64-bit, wired to the same LAN), as your user:

.. code-block:: bash

   sudo apt install -y python3-pygame       # pygame with the kmsdrm-capable system SDL
   sudo loginctl enable-linger $USER        # start the user service at boot, nobody logged in

On the PC: an SSH key copied to the Pi and a ``Host ksphmi`` entry in ``~/.ssh/config`` (``HostName`` = the Pi's
address, ``User``, ``IdentityFile``). The repo never stores the address or the user name; ``HMI_HOST`` overrides
the alias. A DHCP reservation on the router keeps the Pi's address fixed.

In KSP (with KSP closed, coordinated with the other session that shares the install): install the kRPC mod into
``GameData/kRPC``. In the game, open the kRPC window, set the server address to the PC's LAN address (not
``127.0.0.1``), tick auto-start and auto-accept, and allow TCP 50000-50001 in the Windows firewall.

Commands
--------

==========================  ===========================================================================
``./ci hmi --demo``         Run the HMI on the PC in a window with the simulated orbit
``./ci hmi``                Same against KSP on this PC (``--address`` for another one)
``./ci test_hmi``           Unit tests (formatting, Kepler solver, demo source, settings layers)
``./ci deploy_hmi``         Tests, copies ``hmi-pi`` to ``~/ksphmi/app``, updates the venv, installs and
                            restarts the ``ksphmi`` user service, prints its log
``./ci hmi_logs [N]``       Last N lines of the service log on the Pi
``./ci verify``             Includes ``check_hmi``: unit tests, service state, kRPC address still this PC
==========================  ===========================================================================

``python -m ksphmi --screenshot out.png --size 2520x1680 --demo`` with ``SDL_VIDEODRIVER=dummy`` renders a frame
without any window, which is how screen changes are reviewed.
