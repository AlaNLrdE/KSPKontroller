Serial protocol
===============

Framing
-------

Every packet, in both directions, uses the same frame:

.. code-block:: text

   +------+------+------+--------------------------+----------+
   | 0xBE | 0xEF | size | payload (size bytes)     | checksum |
   +------+------+------+--------------------------+----------+
                          ^ first byte = packet id

* ``size`` is the payload length (``sizeof`` of the struct), 1 to 255.
* ``checksum`` is ``size`` XOR-ed with every payload byte.
* Values are little-endian and the structs are packed (no padding).

Packets
-------

.. list-table::
   :header-rows: 1
   :widths: 10 25 20 15 30

   * - Id
     - Name
     - Direction
     - Size
     - Purpose
   * - 0
     - ``HandShakePacket``
     - both
     - 4 bytes
     - Controller discovery
   * - 1
     - ``VesselData``
     - KSP to controller
     - 214 bytes
     - Telemetry
   * - 101
     - ``ControlPacket``
     - controller to KSP
     - 25 bytes
     - Switch and axis state
   * - 2
     - ``DisplayCommand``
     - test tool to controller
     - 2 bytes
     - Forces an LED matrix screen for 4 s (only ``tools/matrix_preview.py`` sends it)
   * - 102
     - ``DebugPacket``
     - controller to KSP
     - 52 bytes
     - Board diagnostics, once per second; the plugin only logs it

Handshake
~~~~~~~~~

KSP sends ``1, 2, 3``. A compatible controller replies ``3, 1, 4``. Any other reply is ignored.

ControlPacket
~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 10 25 20 45

   * - Offset
     - Field
     - Type
     - Meaning
   * - 0
     - ``id``
     - byte
     - 101
   * - 1
     - ``MainControls``
     - byte
     - One bit each, bit 7 down to bit 0: SAS, RCS, Lights, Gear, Brakes, Precision, Abort,
       Stage
   * - 2
     - ``Mode``
     - byte
     - 0 stage, 1 docking, 2 map
   * - 3
     - ``ControlGroup``
     - uint16
     - Custom action groups 1 to 10, bit *n* is group *n*
   * - 5
     - ``NavballSASMode``
     - byte
     - Low 4 bits: autopilot mode + 1, high 4 bits: navball speed mode + 1. Ignored when 0.
   * - 6
     - ``AdditionalControlByte1``
     - byte
     - Reserved
   * - 7
     - ``Pitch``, ``Roll``, ``Yaw``
     - int16 each
     - -1000 to 1000
   * - 13
     - ``TX``, ``TY``, ``TZ``
     - int16 each
     - RCS translation, -1000 to 1000
   * - 19
     - ``WheelSteer``
     - int16
     - -1000 to 1000
   * - 21
     - ``Throttle``
     - int16
     - 0 to 1000
   * - 23
     - ``WheelThrottle``
     - int16
     - 0 to 1000

.. warning::

   The Arduino struct must use fixed-width types (``uint16_t``, ``int16_t``). On the UNO R4 a
   plain ``int`` is 32 bits, which silently produces a 37-byte packet that the plugin decodes
   with shifted fields. The sketch has a ``static_assert`` on the 25-byte size.

DebugPacket
~~~~~~~~~~~

Sent by the board once per second while connected. The plugin does not apply it: it writes one line
per packet to ``KSP.log`` starting with ``KSPSerialIO[BOARD]``, so the last line before a freeze
shows what the firmware was doing. A drop of ``uptime_ms`` produces an extra ``REBOOT detected`` line
with the reset cause (a watchdog reset sets bit 8 or 9 of ``reset``).

.. list-table::
   :header-rows: 1
   :widths: 20 12 68

   * - Field
     - Type
     - Meaning
   * - ``uptime_ms``, ``loops``
     - uint32
     - Board uptime and completed loop iterations.
   * - ``rx_ok``, ``rx_bad_cs``, ``rx_bad_size``
     - uint16
     - Valid packets, packets with a wrong checksum, packets with a wrong size.
   * - ``tx``
     - uint32
     - Packets the board has sent.
   * - ``loop_max_us``, ``tx_max_us``
     - uint32
     - Longest firmware loop and longest single send since the previous report.
   * - ``since_rx_ms``
     - uint32
     - Time since the last valid packet from KSP.
   * - ``reset``
     - uint16
     - Reset status read at boot (bit 0 power on, 8 and 9 watchdog, 10 software).
   * - ``flags``
     - byte
     - bit 0 connected, bit 2 stage pin level, bit 3 watchdog started, bit 4 ever connected, bit 5 DTR asserted
       by the host right now.
   * - ``rx_avail``
     - int16
     - Bytes waiting in the receive buffer.
   * - ``tx_skipped``
     - uint16
     - Frames the board did not send because the host was not listening or the transmit buffer was full
       (cumulative since boot).
   * - ``a0``
     - uint16
     - Raw throttle analog reading.
   * - ``matrix``
     - 12 bytes
     - The LED matrix frame, one byte per column (bit r is row r). Logged as 24 hex digits;
       ``tools/matrix_preview.py --log KSP.log`` draws it.

VesselData
~~~~~~~~~~

The 62 fields, with offsets, units and the KSP member each one comes from, are listed in
:doc:`vesseldata`. Fields ending in ``Tot`` are capacities, and fields ending in ``S``
only count what the active engines can reach. ``ActionGroups`` is a bit field in the order
SAS, RCS, Light, Gear, Brakes, Abort, Custom01 to Custom10. ``NavballSASMode`` uses the same
nibble encoding as ``ControlPacket``.

Sequences
---------

Discovery
~~~~~~~~~

.. uml:: diagrams/seq_discovery.puml

Steady state
~~~~~~~~~~~~

.. uml:: diagrams/seq_runtime.puml

Reconnection
~~~~~~~~~~~~

The protocol has no separate reconnect message. A new session always begins with the same handshake (id 0,
bytes 1, 2, 3, answered with 3, 1, 4). The host asserts DTR when it opens the port, which the board sees as
``Serial`` being true, and drops it on close. The firmware treats each handshake as a new session (parser
reset, stale input dropped, DebugPacket sent at once), and the plugin repeats discovery every 2 s until a
port answers. Details are in :doc:`plugin` and :doc:`firmware`.

.. uml:: diagrams/state_link_recovery.puml

Receivers
---------

Both ends resynchronize on the next ``0xBE 0xEF`` after any framing or checksum error.

.. uml:: diagrams/state_parser_plugin.puml

.. uml:: diagrams/state_parser_firmware.puml
