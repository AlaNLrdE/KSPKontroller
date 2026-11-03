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
     - 200 bytes
     - Telemetry
   * - 101
     - ``ControlPacket``
     - controller to KSP
     - 25 bytes
     - Switch and axis state

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

VesselData
~~~~~~~~~~

The 55 fields, with offsets, units and the KSP member each one comes from, are listed in
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

Receivers
---------

Both ends resynchronize on the next ``0xBE 0xEF`` after any framing or checksum error.

.. uml:: diagrams/state_parser_plugin.puml

.. uml:: diagrams/state_parser_firmware.puml
