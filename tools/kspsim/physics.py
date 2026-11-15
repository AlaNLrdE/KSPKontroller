"""A small flight model so the board and the HMI see believable, changing data without KSP.

The vessel moves in the plane of its orbit around Kerbin (2D, inertial frame, body centre at the origin):
gravity, an exponential atmosphere with drag, a rotating surface to land on, and one engine. The board's
throttle drives the engine; its SAS / RCS / lights / gear / brakes / action group bits are kept as vessel
state and echoed back in VesselData, so the LEDs follow them exactly as with the real plugin.

The orbit stays planar but the attitude is a full 3D orientation (forward and top axes in the inertial frame).
Until the vessel is *steered* (SAS on, or a pitch / yaw / roll input, since the scenario loaded) it points
along a simple ascent guidance (straight up, a gradual turn to the horizon, then prograde), so the launch
scenarios still reach orbit unattended. Once steered, SAS slews it towards the selected mode's direction and
the stick axes apply angular acceleration; the engine then thrusts along the vessel's nose (only the part in
the orbital plane acts: an out-of-plane burn changes nothing but the attitude).
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field

MU = 3.5316e12           # Kerbin gravitational parameter, m^3/s^2
RADIUS = 600_000.0       # m
ATMOSPHERE = 70_000.0    # m
SCALE_HEIGHT = 5_600.0   # m
SEA_DENSITY = 1.225      # kg/m^3
ROTATION_PERIOD = 21_549.425  # s
OMEGA = 2 * math.pi / ROTATION_PERIOD
SOI_RADIUS = 84_159_286.0
G0 = 9.80665

MAX_ACCEL = 16.0         # m/s^2 at full throttle (TWR about 1.6 on the pad)
DRAG_K = 1.2e-4          # drag acceleration = DRAG_K * density * v^2
LIQUID_FUEL = 1440.0     # units per stage
OXIDIZER = 1760.0
BURN_TIME = 300.0        # seconds of full throttle per stage (about 4.8 km/s)
STAGES = 3


# Attitude model (radians, seconds)
SAS_RATE = math.radians(20)      # SAS slew rate limit
SAS_ACCEL = math.radians(20)     # SAS angular acceleration limit (smooth start and stop)
SAS_GAIN = 2.0                   # 1/s, final approach rate per radian of error
STICK_ACCEL = math.radians(40)   # angular acceleration at full pitch / yaw / roll input
STICK_DAMPING = 1.5              # 1/s, rotation damping without SAS (full input settles at about 27 deg/s)
STICK_DEADZONE = 0.05            # axis inputs below this are noise, not steering
ATTITUDE_STEP = 0.05             # s, attitude integration step
RAILS_STEPS = 200                # more attitude steps than this per tick = on rails: SAS snaps, no rotation
HOLD = "hold"                    # sas_target(): keep the current attitude (stability assist, or no direction)
# Simulator.sas_mode 1-10 (KSP AutopilotMode + 1)
SAS_MODES = ("stability assist", "prograde", "retrograde", "normal", "anti-normal", "radial in", "radial out",
             "target", "anti-target", "maneuver")

SITUATIONS = ("pre_launch", "orbiting", "sub_orbital", "escaping", "flying", "landed", "splashed", "docked")

# What the simulator reports to the board and the HMI: (key, label, unit shown in the window, unit -> SI factor).
# Values are SI internally: metres, m/s, seconds, radians for the true anomaly; the inclination is in degrees.
REPORTED_FIELDS = (
    ("altitude", "Altitude", "km", 1e3),
    ("vertical_speed", "Vertical speed", "m/s", 1.0),
    ("orbital_speed", "Orbital speed", "m/s", 1.0),
    ("surface_speed", "Surface speed", "m/s", 1.0),
    ("apoapsis", "Apoapsis", "km", 1e3),
    ("periapsis", "Periapsis", "km", 1e3),
    ("time_to_apoapsis", "Time to Ap", "s", 1.0),
    ("time_to_periapsis", "Time to Pe", "s", 1.0),
    ("inclination", "Inclination", "deg", 1.0),
    ("eccentricity", "Eccentricity", "", 1.0),
    ("period", "Period", "s", 1.0),
    ("semi_major_axis", "Semi-major axis", "km", 1e3),
    ("true_anomaly", "True anomaly", "deg", math.pi / 180),
    ("g_force", "G force", "g", 1.0),
)

# Additional HMI values are numeric so the existing manual-value editor exposes every one.
NAV_DEFAULTS = dict(speed_mode=0.0, target_enabled=0.0, target_up=0.0, target_north=20.0,
                    target_east=40.0, relative_up=0.0, relative_north=0.0, relative_east=12.0,
                    node_enabled=0.0, node_dv=120.0, node_remaining=114.0, node_time=300.0,
                    node_prograde=120.0, node_normal=0.0, node_radial=0.0,
                    burn_up=.2, burn_north=0.0, burn_east=1.0,
                    mass=10000.0, available_thrust=160000.0, specific_impulse=300.0)
REPORTED_FIELDS += tuple((key, key.replace('_', ' ').title(), '', 1.0)
                        for key in ('pitch', 'heading', 'roll', *NAV_DEFAULTS))
RESOURCE_KEYS = ('electric_charge', 'electric_capacity', 'liquid_fuel', 'liquid_capacity',
                 'oxidizer', 'oxidizer_capacity', 'monopropellant', 'monoprop_capacity',
                 'solid_fuel', 'solid_capacity', 'xenon_gas', 'xenon_capacity',
                 'stage_number', 'stage_delta_v', 'stage_parts')
REPORTED_FIELDS += tuple((key, key.replace('_', ' ').title(), '', 1.0) for key in RESOURCE_KEYS)
REPORTED_FIELDS += tuple((key, key.replace('_', ' ').title(), unit, 1.0)
                        for prefix, unit in (('position', 'm'), ('velocity', 'm/s'))
                        for key in (prefix + '_x', prefix + '_y', prefix + '_z'))


def attitude_rotation(pitch, heading, roll):
    def multiply(a, b):
        x, y, z, w = a
        u, v, t, s = b
        return (w*u + x*s + y*t - z*v, w*v - x*t + y*s + z*u,
                w*t + x*v - y*u + z*s, w*s - x*u - y*v - z*t)
    p, h, r = (math.radians(a) / 2 for a in (pitch, heading, roll))
    q = multiply((math.sin(h), 0, 0, math.cos(h)), (0, -math.sqrt(.5), 0, math.sqrt(.5)))
    q = multiply(q, (-math.sin(p), 0, 0, math.cos(p)))
    return multiply(q, (0, -math.sin(r), 0, math.cos(r)))


def attitude_axes(pitch, heading, roll):
    """(forward, top) unit vectors in surface components (up, north, east) of the attitude in degrees; the
    same orientation as attitude_rotation (vessel axes right, forward, down; top = -down)."""
    p, h, r = (math.radians(a) for a in (pitch, heading, roll))
    forward = (math.sin(p), math.cos(p) * math.cos(h), math.cos(p) * math.sin(h))
    top0 = (math.cos(p), -math.sin(p) * math.cos(h), -math.sin(p) * math.sin(h))
    right0 = (0.0, -math.sin(h), math.cos(h))
    return forward, tuple(top0[i] * math.cos(r) + right0[i] * math.sin(r) for i in range(3))


def attitude_angles(forward, top):
    """(pitch, heading 0-360, roll -180..180) in degrees of surface-component axes; inverse of attitude_axes.
    Pointing straight up or down the heading comes from the top axis and the roll is 0."""
    pitch = math.asin(max(-1.0, min(1.0, forward[0])))
    if math.hypot(forward[1], forward[2]) > 1e-6:
        h = math.atan2(forward[2], forward[1])
        top0 = (math.cos(pitch), -math.sin(pitch) * math.cos(h), -math.sin(pitch) * math.sin(h))
        right0 = (0.0, -math.sin(h), math.cos(h))
        roll = math.atan2(_dot(top, right0), _dot(top, top0))
    else:
        s = 1.0 if forward[0] > 0 else -1.0
        h, roll = math.atan2(-top[2] * s, -top[1] * s), 0.0
    return math.degrees(pitch), math.degrees(h) % 360.0, math.degrees(roll)


def _rotate(v, axis, angle):
    """Rodrigues rotation of v about the unit axis."""
    c, s = math.cos(angle), math.sin(angle)
    k = _cross(axis, v)
    d = _dot(axis, v) * (1 - c)
    return tuple(v[i] * c + k[i] * s + axis[i] * d for i in range(3))


def _angle(a, b) -> float:
    return math.atan2(math.sqrt(_dot(_cross(a, b), _cross(a, b))), _dot(a, b))


@dataclass
class Controls:
    """What the board asks for, decoded from its ControlPacket."""
    sas: bool = False
    rcs: bool = False
    lights: bool = False
    gear: bool = False
    brakes: bool = False
    precision: bool = False
    abort: bool = False
    stage: bool = False
    groups: int = 0            # custom action groups, bit n = group n (1-10)
    throttle: float = 0.0      # 0..1
    pitch: float = 0.0         # -1..1
    roll: float = 0.0
    yaw: float = 0.0
    mode: int = 0


@dataclass
class Elements:
    a: float
    e: float
    nu: float                  # true anomaly, radians 0..2pi
    apoapsis: float            # altitudes above sea level (m)
    periapsis: float
    period: float              # inf when not elliptic
    time_to_apoapsis: float    # inf when not elliptic
    time_to_periapsis: float


def elements(x: float, y: float, vx: float, vy: float, mu: float = MU) -> Elements:
    r = math.hypot(x, y)
    v2 = vx * vx + vy * vy
    rv = x * vx + y * vy
    h = x * vy - y * vx
    energy = v2 / 2 - mu / r
    a = -mu / (2 * energy) if abs(energy) > 1e-9 else float("inf")
    ex = ((v2 - mu / r) * x - rv * vx) / mu
    ey = ((v2 - mu / r) * y - rv * vy) / mu
    e = math.hypot(ex, ey)
    theta = math.atan2(y, x)
    arg_pe = math.atan2(ey, ex) if e > 1e-9 else theta
    sign = 1.0 if h >= 0 else -1.0
    nu = ((theta - arg_pe) * sign) % (2 * math.pi)
    inf = float("inf")
    if e < 1 and a > 0:
        ecc = 2 * math.atan2(math.sqrt(1 - e) * math.sin(nu / 2), math.sqrt(1 + e) * math.cos(nu / 2))
        m = (ecc - e * math.sin(ecc)) % (2 * math.pi)
        n = math.sqrt(mu / a ** 3)
        period = 2 * math.pi / n
        return Elements(a, e, nu, a * (1 + e) - RADIUS, a * (1 - e) - RADIUS, period,
                        ((math.pi - m) % (2 * math.pi)) / n, ((2 * math.pi - m) % (2 * math.pi)) / n)
    # hyperbolic (or parabolic): no apoapsis, time to periapsis negative once past it
    nu_signed = nu if nu <= math.pi else nu - 2 * math.pi
    t_pe = inf
    if e > 1 and a < 0:
        f = 2 * math.atanh(math.sqrt((e - 1) / (e + 1)) * math.tan(nu_signed / 2))
        m = e * math.sinh(f) - f
        t_pe = -m / math.sqrt(mu / (-a) ** 3)
    return Elements(a, e, nu, a * (1 + e) - RADIUS, a * (1 - e) - RADIUS, inf, inf, t_pe)


def _stumpff(z: float) -> tuple[float, float]:
    """Stumpff functions (C, S) of the universal Kepler equation."""
    if z > 1e-8:
        s = math.sqrt(z)
        return (1 - math.cos(s)) / z, (s - math.sin(s)) / s ** 3
    if z < -1e-8:
        s = math.sqrt(-z)
        return (math.cosh(s) - 1) / -z, (math.sinh(s) - s) / s ** 3
    return 0.5, 1 / 6


def _dot(a, b) -> float:
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _cross(a, b) -> tuple[float, float, float]:
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _unit(a) -> tuple[float, float, float]:
    n = math.sqrt(_dot(a, a)) or 1.0
    return (a[0] / n, a[1] / n, a[2] / n)


def propagate3(r, v, dt: float, mu: float = MU):
    """Kepler motion (any conic, universal variable) of a 3D state over dt seconds; no drag or thrust."""
    r0 = math.sqrt(_dot(r, r))
    if dt == 0 or r0 <= 0:
        return tuple(r), tuple(v)
    vr0 = _dot(r, v) / r0
    alpha = 2 / r0 - _dot(v, v) / mu
    sq = math.sqrt(mu)
    chi = sq * abs(alpha) * dt if abs(alpha) > 1e-12 else sq * dt / r0
    for _ in range(60):
        z = alpha * chi * chi
        c, s = _stumpff(z)
        f = r0 * vr0 / sq * chi * chi * c + (1 - alpha * r0) * chi ** 3 * s + r0 * chi - sq * dt
        df = r0 * vr0 / sq * chi * (1 - z * s) + (1 - alpha * r0) * chi * chi * c + r0
        step = f / df
        chi -= step
        if abs(step) < 1e-9 * max(1.0, abs(chi)):
            break
    z = alpha * chi * chi
    c, s = _stumpff(z)
    f = 1 - chi * chi / r0 * c
    g = dt - chi ** 3 * s / sq
    nr = tuple(f * r[i] + g * v[i] for i in range(3))
    rn = math.sqrt(_dot(nr, nr))
    fdot = sq / (rn * r0) * (alpha * chi ** 3 * s - chi)
    gdot = 1 - chi * chi / rn * c
    return nr, tuple(fdot * r[i] + gdot * v[i] for i in range(3))


def propagate(x: float, y: float, vx: float, vy: float, dt: float, mu: float = MU) -> tuple[float, float, float, float]:
    """Planar form of propagate3 (the flight model's x/y plane)."""
    (nx, ny, _), (nvx, nvy, _) = propagate3((x, y, 0.0), (vx, vy, 0.0), dt, mu)
    return nx, ny, nvx, nvy


def maneuver_state(values: dict):
    """(position, velocity) right after the node, in the kRPC frame of the reported Position/Velocity: the reported
    state coasted node_time seconds, plus node_prograde along the velocity, node_normal along r x v and node_radial
    outwards, perpendicular to the velocity (KSP's maneuver node axes)."""
    r, v = propagate3(tuple(values["position_" + a] for a in "xyz"), tuple(values["velocity_" + a] for a in "xyz"),
                      values["node_time"])
    pro = _unit(v)
    normal = _unit(_cross(r, v))
    radial = _cross(pro, normal)
    dv = (values["node_prograde"], values["node_normal"], values["node_radial"])
    return r, tuple(v[i] + dv[0] * pro[i] + dv[1] * normal[i] + dv[2] * radial[i] for i in range(3))


def extremes(r, v, mu: float = MU) -> tuple[float, float, float, float]:
    """(apoapsis altitude, periapsis altitude, eccentricity, semi-major axis) of a 3D state; Ap inf when open."""
    rn = math.sqrt(_dot(r, r))
    energy = _dot(v, v) / 2 - mu / rn
    a = -mu / (2 * energy) if abs(energy) > 1e-9 else float("inf")
    h = _cross(r, v)
    e = math.sqrt(max(0.0, 1 + 2 * energy * _dot(h, h) / mu ** 2))
    p = _dot(h, h) / mu
    ap = a * (1 + e) - RADIUS if e < 1 and a > 0 else float("inf")
    return ap, p / (1 + e) - RADIUS, e, a


def density(altitude: float) -> float:
    return 0.0 if altitude >= ATMOSPHERE else SEA_DENSITY * math.exp(-max(altitude, 0.0) / SCALE_HEIGHT)


@dataclass
class Vessel:
    name: str
    x: float
    y: float
    vx: float
    vy: float
    landed: bool = False
    inclination: float = 0.0     # degrees, reported only (the model is planar)
    stage: int = STAGES - 1      # KSP counts stages down to 0
    liquid_fuel: float = LIQUID_FUEL
    oxidizer: float = OXIDIZER
    spent_fuel: float = 0.0      # fuel of the stages already dropped (for the vessel totals)
    monoprop: float = 30.0
    electric: float = 150.0
    mission_time: float = 0.0
    controls: Controls = field(default_factory=Controls)
    thrust_accel: float = 0.0    # last applied engine acceleration, m/s^2
    drag_accel: float = 0.0
    heat: float = 0.0            # 0..1, rises with aerodynamic heating
    _stage_was: bool = False
    # attitude, in the inertial frame (x, y orbital plane, z = r x v of a prograde orbit = south)
    forward: tuple | None = None
    top: tuple | None = None
    omega: tuple = (0.0, 0.0, 0.0)   # angular velocity, rad/s
    steered: bool = False            # SAS or stick input since the scenario loaded: guidance off
    autopilot: object = None         # set by the Simulator each tick: None = SAS off, HOLD, or a unit vector
    sas_error: float = float("nan")  # degrees between the nose and the SAS direction

    # ------------------------------------------------------------------------------------- derived
    @property
    def radius(self) -> float:
        return math.hypot(self.x, self.y)

    @property
    def altitude(self) -> float:
        return self.radius - RADIUS

    @property
    def orbital_speed(self) -> float:
        return math.hypot(self.vx, self.vy)

    def surface_velocity(self) -> tuple[float, float]:
        return self.vx + OMEGA * self.y, self.vy - OMEGA * self.x

    @property
    def surface_speed(self) -> float:
        return math.hypot(*self.surface_velocity())

    @property
    def vertical_speed(self) -> float:
        return (self.x * self.vx + self.y * self.vy) / self.radius

    def elements(self) -> Elements:
        return elements(self.x, self.y, self.vx, self.vy)

    def situation(self) -> str:
        if self.landed:
            return "pre_launch" if self.mission_time == 0 else "landed"
        el = self.elements()
        if el.e >= 1:
            return "escaping"
        if self.altitude < ATMOSPHERE and el.periapsis < ATMOSPHERE:
            return "flying"
        return "orbiting" if el.periapsis > ATMOSPHERE else "sub_orbital"

    def report(self) -> dict:
        """Every value sent to the board and the HMI, computed from the flight model (see REPORTED_FIELDS)."""
        el = self.elements()
        pitch, heading, roll = self.attitude()
        return dict(NAV_DEFAULTS,
            pitch=pitch, heading=heading, roll=roll,
            electric_charge=self.electric, electric_capacity=150, monopropellant=self.monoprop, monoprop_capacity=30,
            liquid_fuel=self.liquid_fuel + LIQUID_FUEL * self.stage, liquid_capacity=LIQUID_FUEL * (self.stage + 1),
            oxidizer=self.oxidizer + OXIDIZER * self.stage, oxidizer_capacity=OXIDIZER * (self.stage + 1),
            solid_fuel=0, solid_capacity=0, xenon_gas=0, xenon_capacity=0,
            stage_number=self.stage, stage_delta_v=MAX_ACCEL * BURN_TIME * self.liquid_fuel / LIQUID_FUEL,
            stage_parts=8,
            position_x=self.x, position_y=0, position_z=self.y,
            velocity_x=self.vx, velocity_y=0, velocity_z=self.vy,
            name=self.name, situation=self.situation(), altitude=self.altitude, vertical_speed=self.vertical_speed,
            orbital_speed=self.orbital_speed, surface_speed=self.surface_speed, apoapsis=el.apoapsis,
            periapsis=el.periapsis, time_to_apoapsis=el.time_to_apoapsis, time_to_periapsis=el.time_to_periapsis,
            inclination=self.inclination, eccentricity=el.e, period=el.period, semi_major_axis=el.a,
            true_anomaly=el.nu, g_force=self.g_force())

    def g_force(self) -> float:
        return 1.0 if self.landed and self.thrust_accel == 0 else (self.thrust_accel + self.drag_accel) / G0

    def frame(self):
        """(up, north, east) unit vectors at the vessel, in the inertial frame. North is -z: the planar orbits
        are prograde equatorial, so their normal (r x v, +z) points south, as krpcserver.TransformDirection."""
        r = self.radius or 1.0
        rx, ry = self.x / r, self.y / r
        return (rx, ry, 0.0), (0.0, 0.0, -1.0), (-ry, rx, 0.0)

    def to_une(self, v) -> tuple[float, float, float]:
        return tuple(_dot(v, b) for b in self.frame())

    def from_une(self, v) -> tuple[float, float, float]:
        u, n, e = self.frame()
        return tuple(v[0] * u[i] + v[1] * n[i] + v[2] * e[i] for i in range(3))

    def axes(self):
        """(forward, top) in the inertial frame: the stored attitude once steered, else the guidance's."""
        if self.steered and self.forward is not None:
            return self.forward, self.top
        ux, uy = self.guidance_direction()
        u, _, _ = self.frame()
        pitch = math.degrees(math.asin(max(-1.0, min(1.0, ux * u[0] + uy * u[1]))))
        f, t = attitude_axes(pitch, 90.0, 0.0)
        return self.from_une(f), self.from_une(t)

    def attitude(self) -> tuple[float, float, float]:
        """(pitch above the horizon, heading, roll) in degrees, relative to the local surface (NavBall)."""
        f, t = self.axes()
        return attitude_angles(self.to_une(f), self.to_une(t))

    def sas_target(self, mode: int, r: dict):
        """The inertial unit vector SAS mode ``mode`` (Simulator.sas_mode 1-10) points at, or HOLD. ``r`` is
        Simulator.reported(): speed_mode (0 orbit, 1 surface, 2 target) picks the prograde of the NavBall as in
        KSP, normal and radial follow it; target and maneuver use target_* / burn_* (up, north, east) and hold
        when there is no target or node."""
        if mode == 1:
            return HOLD
        if mode in (8, 9):
            if not r.get("target_enabled"):
                return HOLD
            d = self.from_une(tuple(r["target_" + a] for a in ("up", "north", "east")))
            n = math.sqrt(_dot(d, d))
            return tuple((1 if mode == 8 else -1) * c / n for c in d) if n > 1e-9 else HOLD
        if mode == 10:
            if not r.get("node_enabled"):
                return HOLD
            d = self.from_une(tuple(r["burn_" + a] for a in ("up", "north", "east")))
            n = math.sqrt(_dot(d, d))
            return tuple(c / n for c in d) if n > 1e-9 else HOLD
        speed_mode = int(r.get("speed_mode", 0))
        if speed_mode == 2 and r.get("target_enabled"):
            pro = self.from_une(tuple(r["relative_" + a] for a in ("up", "north", "east")))
        elif speed_mode == 1:
            pro = (*self.surface_velocity(), 0.0)
        else:
            pro = (self.vx, self.vy, 0.0)
        if math.sqrt(_dot(pro, pro)) < 0.1:
            return HOLD
        pro = _unit(pro)
        normal = _cross((self.x, self.y, 0.0), pro)
        if _dot(normal, normal) < 1e-12:  # straight up or down: no orbital plane
            return pro if mode == 2 else tuple(-c for c in pro) if mode == 3 else HOLD
        normal = _unit(normal)
        radial = _cross(pro, normal)  # radial out, perpendicular to the velocity
        d = {2: pro, 3: tuple(-c for c in pro), 4: normal, 5: tuple(-c for c in normal),
             6: tuple(-c for c in radial), 7: radial}.get(mode)
        return d if d is not None else HOLD

    def _steer(self, dt: float) -> None:
        """Advance the attitude: stick input first (it overrides SAS, as in KSP), else SAS, else damping."""
        c = self.controls
        stick = (c.pitch, c.yaw, c.roll)
        active = any(abs(a) > STICK_DEADZONE for a in stick)
        if not self.steered and (active or self.autopilot is not None):
            self.forward, self.top = self.axes()  # take over from the guidance where it points
            self.steered = True
        if not self.steered or dt <= 0:
            return
        if self.landed:  # held by the ground: turns with the surface
            self.forward, self.top = (_rotate(v, (0.0, 0.0, 1.0), OMEGA * dt) for v in (self.forward, self.top))
            self.omega = (0.0, 0.0, 0.0)
            self.sas_error = float("nan")
            return
        n = max(1, int(math.ceil(dt / ATTITUDE_STEP)))
        if n > RAILS_STEPS:  # time warp on rails: no rotation; SAS points at its direction
            self.omega = (0.0, 0.0, 0.0)
            d = self.autopilot
            if isinstance(d, tuple):
                axis = _cross(self.forward, d)
                if _dot(axis, axis) > 1e-18:
                    angle = _angle(self.forward, d)
                    axis = _unit(axis)
                    self.forward, self.top = (_rotate(v, axis, angle) for v in (self.forward, self.top))
                self.sas_error = 0.0
            return
        for _ in range(n):
            self._attitude_step(dt / n, stick if active else None)

    def _attitude_step(self, h: float, stick) -> None:
        f, t = self.forward, self.top
        w = self.omega
        d = self.autopilot
        if stick is not None:
            pitch, yaw, roll = stick
            right = _cross(t, f)
            axes = (_cross(f, t), _cross(f, right), tuple(-x for x in f))  # nose up, nose right, right wing down
            acc = tuple(STICK_ACCEL * (pitch * axes[0][i] + yaw * axes[1][i] + roll * axes[2][i]) for i in range(3))
            w = tuple(w[i] + (acc[i] - STICK_DAMPING * w[i]) * h for i in range(3))
            self.sas_error = _angle(f, d) * 180 / math.pi if isinstance(d, tuple) else float("nan")
        elif d is None:
            k = max(0.0, 1 - STICK_DAMPING * h)
            w = tuple(x * k for x in w)
            self.sas_error = float("nan")
        else:
            desired = (0.0, 0.0, 0.0)
            self.sas_error = 0.0
            if isinstance(d, tuple):
                theta = _angle(f, d)
                axis = _cross(f, d)
                if _dot(axis, axis) < 1e-18:
                    axis = _cross(f, t) if theta > 1 else (0.0, 0.0, 0.0)  # opposite: turn nose up
                if _dot(axis, axis) > 0:
                    rate = min(SAS_RATE, math.sqrt(1.6 * SAS_ACCEL * theta), SAS_GAIN * theta)
                    desired = tuple(rate * x for x in _unit(axis))  # no roll rate: SAS keeps the roll
                self.sas_error = math.degrees(theta)
            dw = tuple(desired[i] - w[i] for i in range(3))
            size = math.sqrt(_dot(dw, dw))
            limit = SAS_ACCEL * h
            if size > limit:
                dw = tuple(x * limit / size for x in dw)
            w = tuple(w[i] + dw[i] for i in range(3))
        self.omega = w
        rate = math.sqrt(_dot(w, w))
        if rate > 0:
            axis = tuple(x / rate for x in w)
            f, t = _rotate(f, axis, rate * h), _rotate(t, axis, rate * h)
        f = _unit(f)
        t = _unit(tuple(t[i] - f[i] * _dot(t, f) for i in range(3)))
        self.forward, self.top = f, t

    # ------------------------------------------------------------------------------------- dynamics
    def thrust_direction(self) -> tuple[float, float]:
        """Where the engine pushes in the orbital plane: the vessel's nose once steered (its in-plane part, so a
        normal burn has no effect on the planar orbit), else the guidance."""
        if self.steered and self.forward is not None:
            return self.forward[0], self.forward[1]
        return self.guidance_direction()

    def guidance_direction(self) -> tuple[float, float]:
        """Straight up below 1 km, a gradual turn to the horizon until 45 km, then along the orbital velocity."""
        r = self.radius
        rx, ry = self.x / r, self.y / r
        tx, ty = -ry, rx  # horizontal, eastward (the way the surface turns)
        alt = self.altitude
        if alt > 45_000 or not self.landed and self.situation() in ("orbiting", "escaping"):
            v = self.orbital_speed
            if v > 1:
                return self.vx / v, self.vy / v
        turn = 0.0 if alt < 1_000 else min(1.0, ((alt - 1_000) / 44_000) ** 0.6)
        ang = turn * math.pi / 2  # 0 = up, pi/2 = horizontal
        return rx * math.cos(ang) + tx * math.sin(ang), ry * math.cos(ang) + ty * math.sin(ang)

    def apply_controls(self, c: Controls) -> None:
        if c.stage and not self._stage_was and self.stage > 0:  # rising edge, like the plugin
            self.spent_fuel += self.liquid_fuel
            self.stage -= 1
            self.liquid_fuel, self.oxidizer = LIQUID_FUEL, OXIDIZER
        self._stage_was = c.stage
        self.controls = c

    def _accel(self, x, y, vx, vy, throttle):
        r = math.hypot(x, y)
        ax, ay = -MU * x / r ** 3, -MU * y / r ** 3
        thrust = MAX_ACCEL * throttle if self.liquid_fuel > 0 else 0.0
        ux, uy = self.thrust_direction()
        ax, ay = ax + thrust * ux, ay + thrust * uy
        rho = density(r - RADIUS)
        drag = 0.0
        if rho > 0:
            sx, sy = vx + OMEGA * y, vy - OMEGA * x
            s = math.hypot(sx, sy)
            if s > 0:
                drag = DRAG_K * rho * s * s
                ax, ay = ax - drag * sx / s, ay - drag * sy / s
        return ax, ay, thrust, drag

    def step(self, dt: float) -> None:
        """Advance ``dt`` seconds of game time (RK4, at most 0.5 s per sub-step)."""
        throttle = max(0.0, min(1.0, self.controls.throttle))
        self._steer(dt)
        n = max(1, int(math.ceil(dt / 0.5)))
        h = dt / n
        for _ in range(n):
            if self.landed:
                if throttle * MAX_ACCEL <= G0 or self.liquid_fuel <= 0:
                    self._rest_on_surface(h)
                    continue
                self.landed = False
            self._rk4(h, throttle)
            self.mission_time += h
            if throttle > 0 and self.liquid_fuel > 0:
                burn = throttle * h / BURN_TIME
                self.liquid_fuel = max(0.0, self.liquid_fuel - burn * LIQUID_FUEL)
                self.oxidizer = max(0.0, self.oxidizer - burn * OXIDIZER)
            if self.radius <= RADIUS:  # touchdown: stick to the rotating surface
                self.landed = True
                self._rest_on_surface(0.0)
        rho = density(self.altitude)
        heating = rho * self.surface_speed ** 3 / 4e8
        self.heat = max(0.0, min(1.0, self.heat + (heating - 0.05) * dt * 0.02))
        self.electric = max(0.0, self.electric - 0.01 * dt)

    def _rest_on_surface(self, dt: float) -> None:
        ang = math.atan2(self.y, self.x) + OMEGA * dt
        self.x, self.y = RADIUS * math.cos(ang), RADIUS * math.sin(ang)
        self.vx, self.vy = -OMEGA * self.y, OMEGA * self.x
        self.thrust_accel = self.drag_accel = 0.0
        if self.mission_time > 0:
            self.mission_time += dt

    def _rk4(self, h: float, throttle: float) -> None:
        x, y, vx, vy = self.x, self.y, self.vx, self.vy
        a1x, a1y, thrust, drag = self._accel(x, y, vx, vy, throttle)
        a2x, a2y, _, _ = self._accel(x + vx * h / 2, y + vy * h / 2, vx + a1x * h / 2, vy + a1y * h / 2, throttle)
        a3x, a3y, _, _ = self._accel(x + (vx + a1x * h / 2) * h / 2, y + (vy + a1y * h / 2) * h / 2,
                                     vx + a2x * h / 2, vy + a2y * h / 2, throttle)
        a4x, a4y, _, _ = self._accel(x + (vx + a2x * h / 2) * h, y + (vy + a2y * h / 2) * h,
                                     vx + a3x * h, vy + a3y * h, throttle)
        self.x += h * (vx + h / 6 * (a1x + a2x + a3x))
        self.y += h * (vy + h / 6 * (a1y + a2y + a3y))
        self.vx += h / 6 * (a1x + 2 * a2x + 2 * a3x + a4x)
        self.vy += h / 6 * (a1y + 2 * a2y + 2 * a3y + a4y)
        self.thrust_accel, self.drag_accel = thrust, drag


def orbit_vessel(name: str, periapsis: float, apoapsis: float, inclination: float = 0.0,
                 at_apoapsis: bool = False) -> Vessel:
    """A vessel on a prograde orbit with the given altitudes, placed at its periapsis (or apoapsis)."""
    rp, ra = RADIUS + periapsis, RADIUS + apoapsis
    a = (rp + ra) / 2
    r = ra if at_apoapsis else rp
    v = math.sqrt(MU * (2 / r - 1 / a))
    return Vessel(name, r, 0.0, 0.0, v, inclination=inclination, mission_time=600.0)


def set_orbit(v: Vessel, apoapsis: float, periapsis: float, inclination: float, true_anomaly_deg: float) -> None:
    """Put an existing vessel on the orbit with these altitudes (m) at this true anomaly (0 = periapsis).
    Raises ValueError with a message for the window when the request makes no sense."""
    if apoapsis < periapsis:
        raise ValueError("the apoapsis must not be below the periapsis")
    if periapsis <= -RADIUS:
        raise ValueError("the periapsis must be above -%d km (the centre of Kerbin)" % (RADIUS / 1e3))
    if apoapsis + RADIUS >= SOI_RADIUS:
        raise ValueError("the apoapsis must be below the edge of Kerbin's sphere of influence (%d km)"
                         % ((SOI_RADIUS - RADIUS) / 1e3))
    rp, ra = RADIUS + periapsis, RADIUS + apoapsis
    a, e = (rp + ra) / 2, (ra - rp) / (ra + rp)
    nu = math.radians(true_anomaly_deg) % (2 * math.pi)
    p = a * (1 - e * e)
    r = p / (1 + e * math.cos(nu))
    if r <= RADIUS:
        raise ValueError("at %.0f degrees from the periapsis that orbit is %.1f km below the surface: "
                         "choose an angle nearer the apoapsis (180)" % (true_anomaly_deg, (RADIUS - r) / 1e3))
    k = math.sqrt(MU / p)
    v.x, v.y = r * math.cos(nu), r * math.sin(nu)
    v.vx, v.vy = -k * math.sin(nu), k * (e + math.cos(nu))
    v.landed = False
    v.inclination = inclination
    if v.mission_time == 0:
        v.mission_time = 1.0  # off the pad: no longer "pre-launch"


def scenario(name: str) -> Vessel:
    """The ready-made situations the simulator window offers."""
    if name == "pad":
        v = Vessel("Kerbal X", RADIUS, 0.0, 0.0, OMEGA * RADIUS, landed=True)
        return v
    if name == "orbit":
        return orbit_vessel("Orbiter", 80_000, 250_000, inclination=0.4)
    if name == "suborbital":
        v = orbit_vessel("Hopper", -150_000, 120_000, at_apoapsis=True)
        v.stage = 0
        return v
    if name == "escape":
        rp = RADIUS + 100_000
        v = Vessel("Voyager", rp, 0.0, 0.0, math.sqrt(2 * MU / rp) * 1.08, inclination=1.2, mission_time=3_000.0)
        v.stage = 0
        return v
    raise ValueError("unknown scenario %r" % name)


SCENARIOS = ("pad", "orbit", "suborbital", "escape")
