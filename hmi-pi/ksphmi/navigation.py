"""Read-only navball data and attitude geometry (surface axes: up, north, east)."""
from dataclasses import dataclass
import math
import time


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def unit(v):
    length = math.sqrt(dot(v, v))
    return tuple(x / length for x in v) if length > 1e-8 and math.isfinite(length) else None


def rotate(q, v):
    x, y, z, w = q
    n = math.sqrt(dot(q, q))
    if n < 1e-8 or not math.isfinite(n):
        raise ValueError("invalid attitude quaternion")
    x, y, z, w = x / n, y / n, z / n, w / n
    vx, vy, vz = v
    tx, ty, tz = 2 * (y * vz - z * vy), 2 * (z * vx - x * vz), 2 * (x * vy - y * vx)
    return (vx + w * tx + y * tz - z * ty,
            vy + w * ty + z * tx - x * tz,
            vz + w * tz + x * ty - y * tx)


def basis(q):
    return rotate(q, (1, 0, 0)), rotate(q, (0, 1, 0)), rotate(q, (0, 0, -1))


def attitude(pitch, heading, roll):
    """Euler angles to a vessel-to-surface quaternion, used by demo and simulator."""
    p, h, r = map(math.radians, (pitch, heading, roll))
    f = (math.sin(p), math.cos(p) * math.cos(h), math.cos(p) * math.sin(h))
    right = (0, -math.sin(h), math.cos(h))
    up = (math.cos(p), -math.sin(p) * math.cos(h), -math.sin(p) * math.sin(h))
    right, up = (tuple(math.cos(r) * a - math.sin(r) * b for a, b in zip(right, up)),
                 tuple(math.sin(r) * a + math.cos(r) * b for a, b in zip(right, up)))
    # Convert a proper rotation matrix using its largest quaternion component.
    m = [[right[i], f[i], -up[i]] for i in range(3)]
    trace = sum(m[i][i] for i in range(3))
    if trace > 0:
        s = math.sqrt(trace + 1) * 2
        return ((m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s,
                (m[1][0] - m[0][1]) / s, s / 4)
    i = max(range(3), key=lambda j: m[j][j])
    j, k = (i + 1) % 3, (i + 2) % 3
    s = math.sqrt(1 + m[i][i] - m[j][j] - m[k][k]) * 2
    q = [0.0] * 4
    q[i], q[j], q[k], q[3] = s / 4, (m[j][i] + m[i][j]) / s, (m[k][i] + m[i][k]) / s, (m[k][j] - m[j][k]) / s
    return tuple(q)


def burn_time(dv, mass, thrust, isp):
    if not all(math.isfinite(v) for v in (dv, mass, thrust, isp)) or min(dv, mass, thrust, isp) <= 0:
        return None
    ve = isp * 9.80665
    return mass * ve / thrust * -math.expm1(-dv / ve)


@dataclass(frozen=True)
class Navigation:
    rotation: tuple = (0, 0, 0, 1)
    pitch: float = 0
    heading: float = 0
    roll: float = 0
    speed_mode: str = "orbit"
    speed: float | None = None
    prograde: tuple | None = None
    normal: tuple | None = None
    radial: tuple | None = None
    target: tuple | None = None
    maneuver: tuple | None = None
    sas: bool = False
    sas_mode: str = "stability_assist"
    rcs: bool = False
    throttle: float = 0
    g_force: float = 0
    node_dv: float | None = None
    node_remaining: float | None = None
    node_time: float | None = None
    burn_seconds: float | None = None


class NavigationStreams:
    """Own optional nav streams separately so a missing target never interrupts orbital telemetry."""
    def __init__(self, conn, sc, vessel, rate):
        self.conn, self.sc, self.vessel, self.rate = conn, sc, vessel, rate
        self.common, self.extra, self.key = {}, {}, None
        self.retry_frame_at = 0
        self.frame = vessel.surface_reference_frame
        f, c = vessel.flight(self.frame), vessel.control
        try:
            for name in ("rotation", "pitch", "heading", "roll", "g_force"):
                self.add(self.common, name, getattr, f, name)
            for name in ("speed_mode", "sas", "sas_mode", "rcs", "throttle", "nodes"):
                self.add(self.common, name, getattr, c, name)
            for name in ("target_docking_port", "target_vessel", "target_body"):
                self.add(self.common, name, getattr, sc, name)
            for name in ("mass", "available_thrust", "specific_impulse"):
                self.add(self.common, name, getattr, vessel, name)
        except Exception:
            self.close()
            raise

    def add(self, group, name, func, *args):
        stream = self.conn.add_stream(func, *args)
        group[name] = stream
        stream.rate = self.rate

    @staticmethod
    def clear(group):
        for stream in group.values():
            try:
                stream.remove()
            except Exception:
                pass
        group.clear()

    def close(self):
        self.clear(self.common)
        self.clear(self.extra)

    def drop(self, names):
        """Remove failing optional streams and rebuild them shortly, without a rebuild every sample."""
        self.clear({name: self.extra.pop(name) for name in names if name in self.extra})
        self.retry_frame_at = time.monotonic() + .5

    def sample(self):
        v = {k: s() for k, s in self.common.items()}
        if not all(math.isfinite(x) for x in (*v['rotation'], v['pitch'], v['heading'], v['roll'])):
            return None
        mode = v['speed_mode'].name
        target = v['target_docking_port'] or v['target_vessel'] or v['target_body']
        node = v['nodes'][0] if v['nodes'] else None
        key = (mode, target, node)
        if key != self.key or (self.retry_frame_at and time.monotonic() >= self.retry_frame_at):
            self.clear(self.extra)
            self.key = key
            self.retry_frame_at = 0
            # Each optional marker group is independent. kRPC raises plain RuntimeError
            # (InvalidOperationException), not RPCError, e.g. "surface speed reference frame
            # is singular" on the launch pad: the attitude must still be shown.
            try:
                # The speed frames follow the body/surface/target velocity. The
                # orbital_reference_frame co-moves with the vessel and reads zero speed.
                frame = getattr(self.vessel, mode + '_speed_reference_frame')
                self.add(self.extra, 'speed', getattr, self.vessel.flight(frame), 'speed')
                for name, vector in (('prograde', (0, 1, 0)), ('normal', (0, 0, 1)), ('radial', (-1, 0, 0))):
                    self.add(self.extra, name, self.sc.transform_direction, vector, frame, self.frame)
            except Exception:
                self.drop(('speed', 'prograde', 'normal', 'radial'))  # no target or zero relative velocity
            if target is not None:
                try:
                    self.add(self.extra, 'target', target.position, self.frame)
                except Exception:
                    self.drop(('target',))
            if node is not None:
                try:
                    self.add(self.extra, 'maneuver', node.burn_vector, self.frame)
                    for name, attr in (('node_remaining', 'remaining_delta_v'), ('node_dv', 'delta_v'), ('node_time', 'time_to')):
                        self.add(self.extra, name, getattr, node, attr)
                except Exception:
                    self.drop(('maneuver', 'node_remaining', 'node_dv', 'node_time'))
        extra = {}
        for key, stream in list(self.extra.items()):
            try:
                extra[key] = stream()
            except Exception:
                # Target/node removed or frame became singular between updates.
                self.drop((key,))
        v['throttle'] = max(0, min(1, v['throttle'])) if math.isfinite(v['throttle']) else 0
        v['g_force'] = v['g_force'] if math.isfinite(v['g_force']) else 0
        return Navigation(**{k: v[k] for k in ('rotation', 'pitch', 'heading', 'roll', 'sas', 'rcs', 'throttle', 'g_force')},
                          speed_mode=mode, sas_mode=v['sas_mode'].name,
                          burn_seconds=burn_time(extra.get('node_remaining', 0), v['mass'], v['available_thrust'], v['specific_impulse']),
                          **extra)
