"""A small flight model so the board and the HMI see believable, changing data without KSP.

The vessel moves in the plane of its orbit around Kerbin (2D, inertial frame, body centre at the origin):
gravity, an exponential atmosphere with drag, a rotating surface to land on, and one engine whose thrust
follows a simple ascent guidance (straight up, a gradual turn to the horizon, then prograde). The board's
throttle drives the engine; its SAS / RCS / lights / gear / brakes / action group bits are kept as vessel
state and echoed back in VesselData, so the LEDs follow them exactly as with the real plugin.
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
NAV_DEFAULTS = dict(roll=0.0, speed_mode=0.0, target_enabled=0.0, target_up=0.0, target_north=20.0,
                    target_east=40.0, relative_up=0.0, relative_north=0.0, relative_east=12.0,
                    node_enabled=0.0, node_dv=120.0, node_remaining=114.0, node_time=300.0,
                    burn_up=.2, burn_north=0.0, burn_east=1.0,
                    mass=10000.0, available_thrust=160000.0, specific_impulse=300.0)
REPORTED_FIELDS += tuple((key, key.replace('_', ' ').title(), '', 1.0)
                        for key in ('pitch', 'heading', *NAV_DEFAULTS))


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
        pitch, heading = self.attitude()
        return dict(NAV_DEFAULTS,
            pitch=pitch, heading=heading,
            name=self.name, situation=self.situation(), altitude=self.altitude, vertical_speed=self.vertical_speed,
            orbital_speed=self.orbital_speed, surface_speed=self.surface_speed, apoapsis=el.apoapsis,
            periapsis=el.periapsis, time_to_apoapsis=el.time_to_apoapsis, time_to_periapsis=el.time_to_periapsis,
            inclination=self.inclination, eccentricity=el.e, period=el.period, semi_major_axis=el.a,
            true_anomaly=el.nu, g_force=self.g_force())

    def g_force(self) -> float:
        return 1.0 if self.landed and self.thrust_accel == 0 else (self.thrust_accel + self.drag_accel) / G0

    def attitude(self) -> tuple[float, float]:
        """(pitch above the horizon, heading) in degrees of the thrust direction the guidance holds."""
        ux, uy = self.thrust_direction()
        rx, ry = self.x / self.radius, self.y / self.radius
        up = ux * rx + uy * ry
        return math.degrees(math.asin(max(-1.0, min(1.0, up)))), 90.0

    # ------------------------------------------------------------------------------------- dynamics
    def thrust_direction(self) -> tuple[float, float]:
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
