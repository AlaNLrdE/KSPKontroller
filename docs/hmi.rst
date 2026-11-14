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

On the Pi there is no desktop: frames are drawn in memory and copied to the Linux framebuffer ``/dev/fb0``
(``output = fbdev``, set by the service through ``KSPHMI_OUTPUT``). SDL's ``kmsdrm`` driver set the HDMI mode
but left the screen black on the Pi 4; fbdev needs no EGL and no DRM master, costs about 21 ms per
2520x1680 frame, and when the HMI stops the text console shows again.
The framebuffer can be left *blanked* (``/sys/class/graphics/fb0/blank`` = 4, powered down; it happened after
the kmsdrm attempts): frames are then written but nothing shows, while the HMI log looks healthy. The HMI
unblanks it (``FBIOBLANK`` ioctl, no root needed) at start and every 30 s; ``./ci verify`` checks the value.

.. important::

   The pygame wheels from pip bundle an SDL **without** ``kmsdrm`` (``pygame.error: kmsdrm not available``).
   The Pi uses Debian's ``python3-pygame`` (pygame 2.6.1 on the system SDL) through a venv created with
   ``--system-site-packages``; only ``krpc`` comes from pip. The PC venv pins the same pygame 2.6.1.
   SDL's ``kmsdrm`` also needs Mesa EGL/GLES (``pygame.error: EGL not initialized`` without them), which
   Raspberry Pi OS Lite does not install; ``./ci deploy_hmi`` checks for every package it needs.
   The service log is read with ``journalctl _SYSTEMD_USER_UNIT=ksphmi.service`` (``./ci hmi_logs``): Raspberry
   Pi OS keeps no per-user journal, so ``journalctl --user`` finds nothing.

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

   # pygame with the kmsdrm-capable system SDL, plus the Mesa EGL/GLES libraries kmsdrm needs (not in Lite)
   sudo apt install -y python3-pygame libegl1 libegl-mesa0 libgles2 libgl1-mesa-dri
   sudo loginctl enable-linger $USER        # start the user service at boot, nobody logged in

On the PC: an SSH key copied to the Pi and a ``Host ksphmi`` entry in ``~/.ssh/config`` (``HostName`` = the Pi's
address, ``User``, ``IdentityFile``). The repo never stores the address or the user name; ``HMI_HOST`` overrides
the alias. A DHCP reservation on the router keeps the Pi's address fixed.

In KSP (with KSP closed): kRPC **0.7.0** (built for KSP 1.12.5, protocol version 1, matching ``krpc==0.7.0``
on the Pi; the 0.5.x mods speak protocol 0 and the 0.7 client refuses them) from
`github.com/krpc/krpc/releases <https://github.com/krpc/krpc/releases>`_, ``GameData/kRPC`` copied as is. The
shipped ``PluginData/settings.cfg`` is empty (kRPC writes defaults on first start, listening on ``127.0.0.1``
only), so it is replaced with:

.. code-block:: text

   KRPCConfiguration
   {
       autoStartServers = True
       autoAcceptConnections = True
       servers { item { name = KSPKontroller HMI  protocol = ProtocolBuffersOverTCP
                        settings { item { key = address  value = 0.0.0.0 }
                                   item { key = rpc_port  value = 50000 }
                                   item { key = stream_port  value = 50001 } } } }
   }

(one value per line in the real file). The wired LAN is a **Public** network in Windows, and the KSP install
has no firewall rule of its own, so an administrator PowerShell must allow the ports once:

.. code-block:: powershell

   New-NetFirewallRule -DisplayName "KSP kRPC (wk-rea)" -Direction Inbound -Action Allow -Protocol TCP `
     -LocalPort 50000,50001 -RemoteAddress LocalSubnet `
     -Program "C:\Users\alanl\wk\wk-rea\Kerbal Space Program\KSP_x64.exe"

``./ci verify`` (``check_krpc``) checks the mod version against the Pi's client, the server settings (auto-start,
auto-accept, not localhost) and the firewall rule. ``tools/kspsim`` must not run while KSP does: both want port
50000 (and COM5).

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
