VesselData reference
====================

``VesselData`` is the telemetry packet (id 1) that KSP sends to the controller. It is a packed,
little-endian struct of **62 fields and 214 bytes**. The plugin fills it in
``KSPSerialIO.Update`` and sends it at most once every ``refresh`` seconds (``config.xml``,
0.2 s by default). The firmware keeps the latest copy in the global ``VData``.

.. note::

   The struct must be identical on both sides. If you add or reorder a field, change
   ``VesselData`` in ``KSPIO.cs`` and in ``KSPIODemo9.ino`` together, and update the size
   ``static_assert`` in the sketch. The firmware drops any packet whose size does not match.

Field table
-----------

*Offset* is the byte position inside the 214-byte payload (the id byte is offset 0). *Source* is
the KSP API member the plugin reads. Resource amounts are in KSP resource *units*, not kilograms.

.. list-table::
   :header-rows: 1
   :widths: 4 6 16 8 6 28 32

   * - #
     - Offset
     - Field
     - Type
     - Unit
     - Source in KSP
     - Description
   * - 1
     - 0
     - ``id``
     - uint8
     - -
     - constant
     - Packet id, always 1.
   * - 2
     - 1
     - ``AP``
     - float32
     - m
     - orbit.ApA
     - Apoapsis altitude above sea level of the current body.
   * - 3
     - 5
     - ``PE``
     - float32
     - m
     - orbit.PeA
     - Periapsis altitude above sea level.
   * - 4
     - 9
     - ``SemiMajorAxis``
     - float32
     - m
     - orbit.semiMajorAxis
     - Orbit semi-major axis.
   * - 5
     - 13
     - ``SemiMinorAxis``
     - float32
     - m
     - orbit.semiMinorAxis
     - Orbit semi-minor axis.
   * - 6
     - 17
     - ``VVI``
     - float32
     - m/s
     - verticalSpeed
     - Vertical speed; negative while descending.
   * - 7
     - 21
     - ``e``
     - float32
     - -
     - orbit.eccentricity
     - Orbit eccentricity (0 circular, 1 parabolic).
   * - 8
     - 25
     - ``inc``
     - float32
     - deg
     - orbit.inclination
     - Orbit inclination.
   * - 9
     - 29
     - ``G``
     - float32
     - g
     - geeForce
     - Current G-force.
   * - 10
     - 33
     - ``TAp``
     - int32
     - s
     - round(orbit.timeToAp)
     - Time until apoapsis.
   * - 11
     - 37
     - ``TPe``
     - int32
     - s
     - round(orbit.timeToPe)
     - Time until periapsis.
   * - 12
     - 41
     - ``TrueAnomaly``
     - float32
     - rad
     - orbit.trueAnomaly
     - True anomaly (KSP reports radians).
   * - 13
     - 45
     - ``Density``
     - float32
     - -
     - atmDensity
     - Atmospheric density at the vessel; 0 in vacuum.
   * - 14
     - 49
     - ``period``
     - int32
     - s
     - round(orbit.period)
     - Orbital period.
   * - 15
     - 53
     - ``RAlt``
     - float32
     - m
     - min(AGL, ASL)
     - Radar altitude: the lower of altitude above terrain (ASL minus terrainAltitude) and altitude above sea level.
   * - 16
     - 57
     - ``Alt``
     - float32
     - m
     - mainBody.GetAltitude(CoM)
     - Altitude above sea level (ASL).
   * - 17
     - 61
     - ``Vsurf``
     - float32
     - m/s
     - srfSpeed
     - Speed relative to the surface.
   * - 18
     - 65
     - ``Lat``
     - float32
     - deg
     - latitude
     - Latitude.
   * - 19
     - 69
     - ``Lon``
     - float32
     - deg
     - longitude
     - Longitude.
   * - 20
     - 73
     - ``LiquidFuelTot``
     - float32
     - units
     - sum of PartResource.maxAmount "LiquidFuel"
     - Liquid fuel capacity of the whole vessel.
   * - 21
     - 77
     - ``LiquidFuel``
     - float32
     - units
     - sum of PartResource.amount "LiquidFuel"
     - Liquid fuel left in the whole vessel.
   * - 22
     - 81
     - ``OxidizerTot``
     - float32
     - units
     - sum of maxAmount "Oxidizer"
     - Oxidizer capacity of the whole vessel.
   * - 23
     - 85
     - ``Oxidizer``
     - float32
     - units
     - sum of amount "Oxidizer"
     - Oxidizer left in the whole vessel.
   * - 24
     - 89
     - ``EChargeTot``
     - float32
     - units
     - sum of maxAmount "ElectricCharge"
     - Electric charge capacity.
   * - 25
     - 93
     - ``ECharge``
     - float32
     - units
     - sum of amount "ElectricCharge"
     - Electric charge left.
   * - 26
     - 97
     - ``MonoPropTot``
     - float32
     - units
     - sum of maxAmount "MonoPropellant"
     - Monopropellant capacity.
   * - 27
     - 101
     - ``MonoProp``
     - float32
     - units
     - sum of amount "MonoPropellant"
     - Monopropellant left.
   * - 28
     - 105
     - ``IntakeAirTot``
     - float32
     - units
     - sum of maxAmount "IntakeAir"
     - Intake air capacity.
   * - 29
     - 109
     - ``IntakeAir``
     - float32
     - units
     - sum of amount "IntakeAir"
     - Intake air stored.
   * - 30
     - 113
     - ``SolidFuelTot``
     - float32
     - units
     - sum of maxAmount "SolidFuel"
     - Solid fuel capacity.
   * - 31
     - 117
     - ``SolidFuel``
     - float32
     - units
     - sum of amount "SolidFuel"
     - Solid fuel left.
   * - 32
     - 121
     - ``XenonGasTot``
     - float32
     - units
     - sum of maxAmount "XenonGas"
     - Xenon gas capacity.
   * - 33
     - 125
     - ``XenonGas``
     - float32
     - units
     - sum of amount "XenonGas"
     - Xenon gas left.
   * - 34
     - 129
     - ``LiquidFuelTotS``
     - float32
     - units
     - ProspectForResourceMax("LiquidFuel", active engines)
     - Liquid fuel capacity reachable by the active engines (stage view).
   * - 35
     - 133
     - ``LiquidFuelS``
     - float32
     - units
     - ProspectForResource("LiquidFuel", active engines)
     - Liquid fuel left that the active engines can draw. 0 when no engine is burning.
   * - 36
     - 137
     - ``OxidizerTotS``
     - float32
     - units
     - ProspectForResourceMax("Oxidizer", active engines)
     - Oxidizer capacity reachable by the active engines.
   * - 37
     - 141
     - ``OxidizerS``
     - float32
     - units
     - ProspectForResource("Oxidizer", active engines)
     - Oxidizer left that the active engines can draw.
   * - 38
     - 145
     - ``MissionTime``
     - uint32
     - s
     - round(missionTime)
     - Mission elapsed time.
   * - 39
     - 149
     - ``deltaTime``
     - float32
     - s
     - missionTime - previous missionTime
     - Mission time elapsed since the previous telemetry packet.
   * - 40
     - 153
     - ``VOrbit``
     - float32
     - m/s
     - orbit.GetVel().magnitude
     - Orbital speed.
   * - 41
     - 157
     - ``MNTime``
     - uint32
     - s
     - round(node UT - universal time)
     - Time until the first maneuver node; 0 if there is none.
   * - 42
     - 161
     - ``MNDeltaV``
     - float32
     - m/s
     - node.GetBurnVector(patch).magnitude
     - Magnitude of the first maneuver node's burn vector; 0 if there is none.
   * - 43
     - 165
     - ``Pitch``
     - float32
     - deg
     - attitude.eulerAngles.x
     - Pitch relative to the local horizon, computed from the vessel orientation (borrowed from MechJeb2).
   * - 44
     - 169
     - ``Roll``
     - float32
     - deg
     - attitude.eulerAngles.z
     - Roll, in the range -180 to 180.
   * - 45
     - 173
     - ``Heading``
     - float32
     - deg
     - attitude.eulerAngles.y
     - Compass heading, 0 to 360.
   * - 46
     - 177
     - ``ActionGroups``
     - uint16
     - bits
     - ActionGroups[...]
     - Action group state bit field, see below.
   * - 47
     - 179
     - ``SOINumber``
     - uint8
     - -
     - GetSOINumber(referenceBody.name)
     - Sphere of influence id, see below. 0 for an unknown body.
   * - 48
     - 180
     - ``MaxOverHeat``
     - uint8
     - %
     - max over parts of temperature / maxTemp
     - Highest overheat of any part, internal or skin temperature, as a percentage.
   * - 49
     - 181
     - ``MachNumber``
     - float32
     - -
     - mach
     - Mach number.
   * - 50
     - 185
     - ``IAS``
     - float32
     - m/s
     - indicatedAirSpeed
     - Indicated air speed.
   * - 51
     - 189
     - ``CurrentStage``
     - uint8
     - -
     - StageManager.CurrentStage
     - Current stage number.
   * - 52
     - 190
     - ``TotalStage``
     - uint8
     - -
     - StageManager.StageCount
     - Total number of stages.
   * - 53
     - 191
     - ``TargetDist``
     - float32
     - m
     - Vector3.Distance(target, vessel)
     - Distance to the targeted vessel; 0 if no vessel is targeted.
   * - 54
     - 195
     - ``TargetV``
     - float32
     - m/s
     - ship_tgtVelocity.magnitude
     - Velocity relative to the targeted vessel; 0 if none.
   * - 55
     - 199
     - ``NavballSASMode``
     - uint8
     - bits
     - speedDisplayMode and Autopilot.Mode
     - Combined navball and SAS mode, see below.
   * - 56
     - 200
     - ``ProgradePitch``
     - int16
     - deg x 50
     - navball prograde (srf_velocity, obt_velocity or ship_tgtVelocity, by speed mode)
     - Pitch of the prograde vector of the current navball speed mode.
   * - 57
     - 202
     - ``ProgradeHeading``
     - int16
     - deg x 50
     - navball prograde
     - Heading of the prograde vector of the current navball speed mode.
   * - 58
     - 204
     - ``ManeuverPitch``
     - int16
     - deg x 50
     - node.GetBurnVector(patch)
     - Pitch of the first maneuver node's burn vector; 0 if there is no node.
   * - 59
     - 206
     - ``ManeuverHeading``
     - int16
     - deg x 50
     - node.GetBurnVector(patch)
     - Heading of the first maneuver node's burn vector; 0 if there is no node.
   * - 60
     - 208
     - ``TargetPitch``
     - int16
     - deg x 50
     - targetObject position - vessel position
     - Pitch towards the targeted object; 0 if there is no target.
   * - 61
     - 210
     - ``TargetHeading``
     - int16
     - deg x 50
     - targetObject position - vessel position
     - Heading towards the targeted object; 0 if there is no target.
   * - 62
     - 212
     - ``NormalHeading``
     - int16
     - deg x 50
     - cross(obt_velocity, up)
     - Heading of the orbit normal. Its pitch is always 0, so there is no field for it.

The last field ends at offset 213, so the struct is 214 bytes.

Resource groups
---------------

Resources come in three flavors:

* **Whole vessel** (``LiquidFuel``, ``Oxidizer``, ``ECharge``, ``MonoProp``, ``IntakeAir``,
  ``SolidFuel``, ``XenonGas``): the sum over every part. The ``Tot`` field is the capacity and
  the field without the suffix is the amount left.
* **Active engines** (``LiquidFuelS``, ``LiquidFuelTotS``, ``OxidizerS``, ``OxidizerTotS``): only
  what the engines that are currently burning can draw. The plugin starts from each active
  engine part and walks the stack-attached parts that allow fuel cross-feed, counting every part
  once. Use these for a "fuel in the current stage" gauge.
* **Zero capacity**: when the vessel has no capacity for a resource both fields are 0.

.. warning::

   With no active engine, ``LiquidFuelS`` and ``LiquidFuelTotS`` are both 0. A percentage such
   as ``LiquidFuelS / LiquidFuelTotS * 100`` is then ``0 / 0`` (NaN), and every comparison with
   NaN is false. The sketch's fuel caution and warning therefore stay off until an engine is
   active. Guard the division if you add your own gauge.

ActionGroups bit field
----------------------

``ActionGroups`` is a 16-bit field. Bit *n* is 1 while the group is active. The plugin sets it
from ``enumAG``; the firmware reads it with ``ControlStatus(bit)``.

.. list-table::
   :header-rows: 1
   :widths: 10 25 25

   * - Bit
     - Group
     - Sketch define
   * - 0
     - SAS
     - ``AGSAS``
   * - 1
     - RCS
     - ``AGRCS``
   * - 2
     - Lights
     - ``AGLight``
   * - 3
     - Gear
     - ``AGGear``
   * - 4
     - Brakes
     - ``AGBrakes``
   * - 5
     - Abort
     - ``AGAbort``
   * - 6 to 15
     - Custom 1 to Custom 10
     - ``AGCustom01`` to ``AGCustom10``

NavballSASMode
--------------

One byte, two nibbles:

.. code-block:: text

     bit 7..4              bit 3..0
   +-----------------+-----------------+
   | navball speed   | autopilot (SAS) |
   | mode + 1        | mode + 1        |
   +-----------------+-----------------+

.. list-table::
   :header-rows: 1
   :widths: 15 35 15 35

   * - Low nibble
     - Autopilot mode
     - High nibble
     - Navball speed mode
   * - 0
     - SAS is off
     - 0
     - Not sent
   * - 1
     - Stability assist
     - 1
     - Orbit
   * - 2
     - Prograde
     - 2
     - Surface
   * - 3
     - Retrograde
     - 3
     - Target
   * - 4
     - Normal
     -
     -
   * - 5
     - Anti-normal
     -
     -
   * - 6
     - Radial in
     -
     -
   * - 7
     - Radial out
     -
     -
   * - 8
     - Target
     -
     -
   * - 9
     - Anti-target
     -
     -
   * - 10
     - Maneuver node
     -
     -

The low nibble is only filled while SAS is on. In the sketch:

.. code-block:: c

   uint8_t sasMode    = VData.NavballSASMode & 0x0F;  // 0 = SAS off
   uint8_t speedMode  = VData.NavballSASMode >> 4;    // 1 orbit, 2 surface, 3 target

Navball vector angles
---------------------

Fields 56 to 62 (``ProgradePitch`` to ``NormalHeading``, added in upstream 0.19.1) give the direction
of the navball markers relative to the local horizon. Each one is an ``int16`` holding the angle in
degrees **times 50** (the fixed-point format of upstream 0.19.2), so divide by 50:

.. code-block:: c

   float progradeHeading = VData.ProgradeHeading / 50.0f;   // 0 to 360 degrees
   float progradePitch   = VData.ProgradePitch   / 50.0f;   // -90 to 90 degrees

Pitch is positive above the horizon. Heading is the compass heading: 0 north, 90 east. A marker that
does not exist (no maneuver node, no target) reports 0 for both angles. The prograde marker follows the
navball speed mode: surface, orbit or target velocity. The orbit normal only has a heading, because
its pitch is always 0.

Sphere of influence (SOINumber)
-------------------------------

The id is decimal: the hundreds digit is 1, the tens digit identifies the planet and the units
digit the moon (0 for the Sun or the planet itself).

.. list-table::
   :header-rows: 1
   :widths: 20 10 20 10

   * - Body
     - Id
     - Body
     - Id
   * - Sun
     - 100
     - Dres
     - 150
   * - Moho
     - 110
     - Jool
     - 160
   * - Eve
     - 120
     - Laythe
     - 161
   * - Gilly
     - 121
     - Vall
     - 162
   * - Kerbin
     - 130
     - Tylo
     - 163
   * - Mun
     - 131
     - Bop
     - 164
   * - Minmus
     - 132
     - Pol
     - 165
   * - Duna
     - 140
     - Eeloo
     - 170
   * - Ike
     - 141
     -
     -

Any other body, such as one added by a mod, reports 0.

Behavior to keep in mind
------------------------

* **Hyperbolic and escape orbits**: ``AP``, ``TAp``, ``period`` and related values are not
  meaningful and can be negative, infinite or NaN.
* **Time fields are rounded integers**: ``TAp``, ``TPe``, ``period``, ``MissionTime`` and
  ``MNTime`` are rounded seconds. ``MNTime`` is only meaningful while the maneuver node is still
  ahead of the vessel.
* **MaxOverHeat** is a percentage stored in one byte. A value above 100 means a part is beyond
  its temperature limit, and values above 255 do not fit in the byte.
* **Target fields**: ``TargetDist`` and ``TargetV`` are 0 when no vessel is targeted.
* **Angles**: ``TrueAnomaly`` is in radians, while ``inc``, ``Lat``, ``Lon``, ``Pitch``, ``Roll``
  and ``Heading`` are in degrees. ``Roll`` is -180 to 180 and ``Heading`` is 0 to 360. The navball vector
  fields are degrees times 50.
* **Updates**: the firmware only replaces ``VData`` when a complete packet with a valid checksum
  arrives, so between packets (and after a dropped one) it keeps the previous values.

Using a field in the sketch
---------------------------

.. code-block:: c

   // utilities.ino: Indicators() runs every time a VesselData packet is accepted.
   caution += VData.G > GCAUTION;                       // G-force above 5
   digitalWrite(SASLED, ControlStatus(AGSAS));          // SAS state from the bit field
   float apoapsisKm = VData.AP / 1000.0f;               // meters to kilometers
