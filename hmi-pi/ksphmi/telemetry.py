"""Flight data sources for the HMI.

Every source runs in its own thread and publishes immutable :class:`Snapshot` objects; the display thread only
calls :meth:`Source.latest`, so a slow or lost connection never blocks drawing.

* :class:`KrpcSource` reads the active vessel from the kRPC mod running inside KSP.
* :class:`DemoSource` flies a Kepler orbit around Kerbin, so the screens can be developed without the game.
"""
from __future__ import annotations

import logging
import math
import threading
import time
from dataclasses import dataclass, replace
from .navigation import Navigation, NavigationStreams, attitude, unit

log = logging.getLogger(__name__)

CONNECTING = "connecting"  #: no kRPC connection (KSP closed, kRPC server stopped, network down)
NO_VESSEL = "no_vessel"    #: connected, but not in the flight scene (space center, VAB, tracking station)
FLIGHT = "flight"          #: the fields below describe the active vessel


@dataclass(frozen=True)
class Snapshot:
    """One sample of the active vessel. Distances in metres, speeds in m/s, times in seconds."""
    state: str = CONNECTING
    detail: str = ""             #: why we are not in FLIGHT, shown on the waiting screen
    scene: str = ""             #: KSPSerialIO scene context, empty if unavailable
    server: str = ""            #: current endpoint, including a server found after DHCP changes
    vessel: str = ""
    situation: str = ""          #: kRPC VesselSituation name: pre_launch, flying, sub_orbital, orbiting...
    body: str = ""
    body_radius: float = 0.0
    atmosphere_depth: float = 0.0  #: 0 for bodies without atmosphere
    ut: float = 0.0              #: game universal time
    altitude: float = 0.0        #: above sea level
    apoapsis: float = 0.0        #: altitude above sea level (kRPC gives a negative value on hyperbolic orbits)
    periapsis: float = 0.0
    time_to_apoapsis: float = 0.0
    time_to_periapsis: float = 0.0
    inclination: float = 0.0     #: degrees
    eccentricity: float = 0.0
    period: float = 0.0
    semi_major_axis: float = 0.0  #: from the body centre; negative for hyperbolic orbits
    true_anomaly: float = 0.0    #: radians, 0 at periapsis
    orbital_speed: float = 0.0
    surface_speed: float = 0.0
    vertical_speed: float = 0.0
    nav: Navigation | None = None  #: optional navball telemetry; None means unavailable
    received: float = 0.0        #: time.monotonic() when the sample was taken


class Source:
    """Base class: a daemon thread that keeps :attr:`_latest` up to date until :meth:`stop`."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._latest = Snapshot()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run_guarded, name=type(self).__name__, daemon=True)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=3)

    def latest(self) -> Snapshot:
        with self._lock:
            return self._latest

    def _publish(self, snap: Snapshot) -> None:
        with self._lock:
            self._latest = snap

    def _run_guarded(self) -> None:
        try:
            self._run()
        except Exception:  # the display must keep running and show the problem, never die silently
            log.exception("telemetry thread crashed")
            self._publish(Snapshot(state=CONNECTING, detail="telemetry thread crashed, see the log"))

    def _run(self) -> None:
        raise NotImplementedError


# ------------------------------------------------------------------------------------------------- kRPC
class KrpcSource(Source):
    """Polls the kRPC server. Reconnects forever; re-opens its streams when the vessel or the body changes."""

    def __init__(self, address: str, rpc_port: int = 50000, stream_port: int = 50001,
                 rate_hz: float = 10.0, retry_seconds: float = 3.0, client_name: str = "KSP HMI",
                 discovery: bool = True) -> None:
        super().__init__()
        self.address = address
        self.context_port = 50002
        self.rpc_port = rpc_port
        self.stream_port = stream_port
        self.period = 1.0 / max(rate_hz, 0.5)
        self.retry = retry_seconds
        self.client_name = client_name
        self.discovery = discovery

    def _publish(self, snap: Snapshot) -> None:
        super()._publish(replace(snap, server="kRPC %s:%d" % (self.address, self.rpc_port)))

    def _run(self) -> None:
        import krpc  # imported here so the demo source and the tests do not need it

        while not self._stop.is_set():
            where = "%s:%d" % (self.address, self.rpc_port)
            self._publish(Snapshot(state=CONNECTING, detail="connecting to kRPC at " + where))
            try:
                conn = self._connect(krpc)
            except Exception as e:  # refused (KSP closed), timeout, handshake rejected by the server
                log.warning("kRPC at %s: %s", where, _short(e))
                self._publish(Snapshot(state=CONNECTING, detail="kRPC at %s: %s" % (where, _short(e))))
                # DHCP may have reassigned the PC. The game already answers scene
                # requests on UDP 50002, including broadcast requests from a new Pi IP.
                from .discovery import discover
                discovered = discover(self.context_port) if self.discovery else ""
                if discovered and discovered != self.address:
                    log.info("discovered KSP at %s (previously %s)", discovered, self.address)
                    self.address = discovered
                    continue
                self._stop.wait(self.retry)
                continue
            log.info("connected to kRPC at %s", where)
            try:
                self._session(conn)
            except Exception as e:  # connection lost mid-session: start over
                log.warning("kRPC session ended: %s", _short(e))
                self._publish(Snapshot(state=CONNECTING, detail="lost kRPC at %s: %s" % (where, _short(e))))
            finally:
                try:
                    conn.close()
                except Exception:
                    pass
            self._stop.wait(self.retry)

    TCP_TIMEOUT = 5.0       # reaching the port (a firewall on a Public network drops, rather than refuses)
    CONNECT_TIMEOUT = 20.0  # the whole handshake, longer than TCP_TIMEOUT so the two causes stay apart

    def _connect(self, krpc):
        """krpc.connect() with a deadline on the whole handshake. Its own ``timeout`` only covers the TCP connect:
        a server that accepts but never answers (kRPC with maxTimePerUpdate = 0 did exactly that) hung the HMI
        forever. The stuck attempt is left to a daemon thread; if it ever completes, its connection is closed."""
        box: dict = {}

        def attempt() -> None:
            try:
                box["conn"] = krpc.connect(name=self.client_name, address=self.address, rpc_port=self.rpc_port,
                                           stream_port=self.stream_port, timeout=self.TCP_TIMEOUT)
            except Exception as e:
                box["error"] = e
            if box.get("abandoned") and "conn" in box:
                try:
                    box["conn"].close()
                except Exception:
                    pass

        t = threading.Thread(target=attempt, name="krpc-connect", daemon=True)
        t.start()
        t.join(self.CONNECT_TIMEOUT)
        if t.is_alive():
            box["abandoned"] = True
            raise TimeoutError("connected but no answer to the handshake within %.0f s (kRPC busy, paused or "
                               "misconfigured)" % self.CONNECT_TIMEOUT)
        if "error" in box:
            raise box["error"]
        return box["conn"]

    def _session(self, conn) -> None:
        from krpc.error import RPCError

        sc = conn.space_center
        while not self._stop.is_set():
            try:
                vessel = sc.active_vessel
            except RPCError as e:  # not in the flight scene (some scenes raise)
                vessel, why = None, _short(e)
            else:
                why = "no active vessel"
            if vessel is None:  # kRPC 0.7 at the Space Center / Tracking Station returns null instead of raising
                from .scenes import read_context
                scene = read_context(self.address, self.context_port)
                self._publish(Snapshot(state=NO_VESSEL, scene=scene, detail=why, received=time.monotonic()))
                self._stop.wait(1.0)
                continue
            try:
                self._follow(conn, sc, vessel)
            except RPCError as e:  # the vessel was destroyed / the scene changed while we read it
                log.info("vessel lost: %s", _short(e))

    def _follow(self, conn, sc, vessel) -> None:
        """Stream one vessel around one body; returns when either changes so the caller re-opens the streams."""
        orbit = vessel.orbit
        body = orbit.body
        flight = vessel.flight(body.reference_frame)  # surface-relative speeds
        name, radius = vessel.name, body.equatorial_radius
        atmosphere = body.atmosphere_depth if body.has_atmosphere else 0.0
        body_name = body.name

        fields = {
            "ut": (sc, "ut"),
            "name": (vessel, "name"),  # streamed: a rename shows without switching vessels
            "situation": (vessel, "situation"),
            "body_obj": (orbit, "body"),
            "altitude": (flight, "mean_altitude"),
            "surface_speed": (flight, "speed"),
            "vertical_speed": (flight, "vertical_speed"),
            "orbital_speed": (orbit, "speed"),
            "apoapsis": (orbit, "apoapsis_altitude"),
            "periapsis": (orbit, "periapsis_altitude"),
            "time_to_apoapsis": (orbit, "time_to_apoapsis"),
            "time_to_periapsis": (orbit, "time_to_periapsis"),
            "inclination": (orbit, "inclination"),
            "eccentricity": (orbit, "eccentricity"),
            "period": (orbit, "period"),
            "semi_major_axis": (orbit, "semi_major_axis"),
            "true_anomaly": (orbit, "true_anomaly"),
        }
        streams = {}
        navstreams = None
        try:
            for key, (obj, attr) in fields.items():
                s = conn.add_stream(getattr, obj, attr)
                try:
                    s.rate = 1.0 / self.period
                except Exception:  # very old servers have no per-stream rate
                    pass
                streams[key] = s
            try:
                navstreams = NavigationStreams(conn, sc, vessel, 1.0 / self.period)
            except RPCError as e:
                log.warning("navball unavailable: %s", _short(e))
            log.info("following %s around %s", name, body_name)
            while not self._stop.is_set():
                if sc.active_vessel != vessel:  # also a liveness probe: raises when the socket is gone (None = left flight)
                    return
                v = {k: s() for k, s in streams.items()}
                if v["body_obj"] != body:  # sphere of influence change: new reference frame needed
                    return
                nav = None
                if navstreams is not None:
                    try:
                        nav = navstreams.sample()
                    except RPCError:
                        navstreams.key = None  # transient target, node or speed frame change
                self._publish(Snapshot(
                    state=FLIGHT, vessel=v["name"], situation=getattr(v["situation"], "name", str(v["situation"])),
                    body=body_name, body_radius=radius, atmosphere_depth=atmosphere, ut=v["ut"],
                    altitude=v["altitude"], apoapsis=v["apoapsis"], periapsis=v["periapsis"],
                    time_to_apoapsis=v["time_to_apoapsis"], time_to_periapsis=v["time_to_periapsis"],
                    inclination=math.degrees(v["inclination"]), eccentricity=v["eccentricity"],
                    period=v["period"], semi_major_axis=v["semi_major_axis"], true_anomaly=v["true_anomaly"],
                    orbital_speed=v["orbital_speed"], surface_speed=v["surface_speed"],
                    vertical_speed=v["vertical_speed"], nav=nav,
                    received=time.monotonic()))
                self._stop.wait(self.period)
        finally:
            if navstreams is not None:
                navstreams.close()
            for s in streams.values():
                try:
                    s.remove()
                except Exception:
                    pass


def _short(e: BaseException) -> str:
    text = str(e).strip().splitlines()[0] if str(e).strip() else type(e).__name__
    return text[:160]


# ------------------------------------------------------------------------------------------------- demo
KERBIN_MU = 3.5316e12      # m^3/s^2
KERBIN_RADIUS = 600_000.0  # m
KERBIN_ATMOSPHERE = 70_000.0
KERBIN_ROTATION_SPEED = 174.94  # equatorial surface speed, m/s


def kepler_state(a: float, e: float, mu: float, mean_anomaly: float) -> tuple[float, float, float, float]:
    """Elliptic orbit (0 <= e < 1) at a mean anomaly: returns (radius, true anomaly, speed, radial speed)."""
    m = mean_anomaly % (2 * math.pi)
    ecc = m if e < 0.8 else math.pi
    for _ in range(50):  # Newton on Kepler's equation E - e sin E = M
        step = (ecc - e * math.sin(ecc) - m) / (1 - e * math.cos(ecc))
        ecc -= step
        if abs(step) < 1e-12:
            break
    nu = 2 * math.atan2(math.sqrt(1 + e) * math.sin(ecc / 2), math.sqrt(1 - e) * math.cos(ecc / 2))
    r = a * (1 - e * math.cos(ecc))
    speed = math.sqrt(mu * (2 / r - 1 / a))
    p = a * (1 - e * e)
    radial = math.sqrt(mu / p) * e * math.sin(nu)
    return r, nu % (2 * math.pi), speed, radial


class DemoSource(Source):
    """A ship on an 80 x 250 km Kerbin orbit, time accelerated by ``speed``. No game or network needed."""

    def __init__(self, speed: float = 20.0, rate_hz: float = 10.0,
                 periapsis: float = 80_000.0, apoapsis: float = 250_000.0) -> None:
        super().__init__()
        self.speed = speed
        self.period_s = 1.0 / max(rate_hz, 0.5)
        rp, ra = KERBIN_RADIUS + periapsis, KERBIN_RADIUS + apoapsis
        self.a = (rp + ra) / 2
        self.e = (ra - rp) / (ra + rp)

    def sample(self, ut: float) -> Snapshot:
        a, e, mu = self.a, self.e, KERBIN_MU
        n = math.sqrt(mu / a ** 3)
        m = (n * ut) % (2 * math.pi)
        r, nu, speed, radial = kepler_state(a, e, mu, m)
        return Snapshot(
            state=FLIGHT, vessel="Demo Orbiter", situation="orbiting", body="Kerbin",
            body_radius=KERBIN_RADIUS, atmosphere_depth=KERBIN_ATMOSPHERE, ut=ut,
            altitude=r - KERBIN_RADIUS,
            apoapsis=a * (1 + e) - KERBIN_RADIUS, periapsis=a * (1 - e) - KERBIN_RADIUS,
            time_to_apoapsis=((math.pi - m) % (2 * math.pi)) / n,
            time_to_periapsis=((2 * math.pi - m) % (2 * math.pi)) / n,
            inclination=2.5, eccentricity=e, period=2 * math.pi / n, semi_major_axis=a, true_anomaly=nu,
            orbital_speed=speed, surface_speed=max(speed - KERBIN_ROTATION_SPEED, 0.0), vertical_speed=radial,
            nav=Navigation(rotation=attitude(14, 90, -25), pitch=14, heading=90, roll=-25,
                           speed_mode='orbit', speed=speed, prograde=unit((radial, 0, speed)),
                           normal=(0, -1, 0), radial=(1, 0, 0), target=unit((.1, .4, .9)),
                           maneuver=unit((.12, .1, 1)), sas=True, sas_mode='prograde', rcs=True,
                           throttle=.42, g_force=.3, node_dv=320, node_remaining=114.4,
                           node_time=579, burn_seconds=8),
            received=time.monotonic())

    def _run(self) -> None:
        start = time.monotonic()
        while not self._stop.is_set():
            self._publish(self.sample((time.monotonic() - start) * self.speed))
            self._stop.wait(self.period_s)


def stale(snap: Snapshot, now: float, limit: float = 3.0) -> bool:
    """True when a FLIGHT snapshot has not been refreshed for ``limit`` seconds (frozen game, stalled link)."""
    return snap.state == FLIGHT and now - snap.received > limit


__all__ = ["Snapshot", "Source", "KrpcSource", "DemoSource", "kepler_state", "stale",
           "CONNECTING", "NO_VESSEL", "FLIGHT"]
