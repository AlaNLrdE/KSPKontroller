"""A KSP 2 Redux stand-in for the orbit report only: ``KSP2ORB1 <nonce>`` on UDP 50003 (docs/hmi.rst "KSP 2 Redux
source"), built from the simulated orbit with the encoding rules of the KSP2 workbench's OrbitReport.EncodeBody.

It answers nothing else (no KSP2HMI snapshot), so the HMI's Redux source keeps preferring kRPC: the responder exists
to exercise the wire format (tests, a script probing the HMI parser). ``./ci sim --redux-orbit`` starts it.

Frame: like Redux's Zup body-inertial frame, the spin axis is +z. kspsim's kRPC frame is y-up (the equatorial plane
is x/z), so every vector is sent as (x, z, y): a reflection, which ORBIT's view axes from r x v absorb.
"""
from __future__ import annotations

import logging
import math
import re
import socket
import threading
from decimal import Decimal

from . import krpcserver as ks
from . import physics

log = logging.getLogger("kspsim.redux")

REDUX_PORT = 50003
REQUEST = re.compile(r"KSP2ORB1 ([0-9]{1,16})")
MAX_REPLY = 4096


def number(x: float) -> str:
    """C# double "R" invariant: 1000.5, -358.8, 3531600000000, 1E-05, 1.5E+20. Never NaN or infinity."""
    x = float(x)
    if not math.isfinite(x):
        raise ValueError("non-finite value in the orbit report")
    if x == 0:
        return "0"  # also -0
    if x.is_integer() and abs(x) < 1e15:
        return str(int(x))
    if abs(x) >= 1e15:  # .NET writes an exponent from 1E+15 on; Python's repr only from 1e+16
        sign, digits, exponent = Decimal(repr(x)).normalize().as_tuple()
        text = "".join(map(str, digits))
        mantissa = text[0] + ("." + text[1:] if len(text) > 1 else "")
        return "%s%sE+%02d" % ("-" if sign else "", mantissa, exponent + len(text) - 1)
    text = repr(x)
    if "e" in text:
        mantissa, exponent = text.split("e")
        if mantissa.endswith(".0"):
            mantissa = mantissa[:-2]
        sign = "-" if exponent.startswith("-") else "+"
        return "%sE%s%02d" % (mantissa, sign, int(exponent.lstrip("+-")))
    return text


def hexname(name: str) -> str:
    """Lowercase hex of the UTF-8 text (at most 256 bytes); "-" for an empty one."""
    data = name.encode("utf-8")[:256]
    return data.hex() if data else "-"


def unhexname(token: str) -> str:
    if token == "-":
        return ""
    if len(token) % 2 or len(token) > 512 or token != token.lower():
        raise ValueError("bad hex name")
    return bytes.fromhex(token).decode("utf-8")


def encode_body(report: dict | None) -> str:
    """The reply body (the text after "KSP2ORB1 <nonce> "), or "-" without orbit data. `report` keys: ut, body, mu,
    position, velocity and the optional maneuver / target / approach dicts and encounters list (see decode_body)."""
    if report is None:
        return "-"
    out = [number(report["ut"]), hexname(report["body"]), number(report["mu"])]
    out += [number(c) for c in report["position"]] + [number(c) for c in report["velocity"]]
    m, t, a = report.get("maneuver"), report.get("target"), report.get("approach")
    if a and not t:
        raise ValueError("a closest approach needs a target")
    out.append(str((1 if m else 0) | (2 if t else 0) | (4 if a else 0)))
    if m:
        out += [number(m["ut"]), number(m["soi"])] + [number(c) for c in m["position"] + m["velocity"]]
    if t:
        out += [hexname(t["name"]), "1" if t["body"] else "0", number(t["radius"])]
        out += [number(c) for c in t["position"] + t["velocity"]]
    if a:
        out += [number(a["ut"]), "1" if a["planned"] else "0"] + [number(c) for c in a["vessel"] + a["target"]]
    encounters = report.get("encounters", [])
    if len(encounters) > 4:
        raise ValueError("at most 4 encounters")
    out.append(str(len(encounters)))
    for e in encounters:
        out += [hexname(e["body"])] + [number(e[k]) for k in ("radius", "mu", "soi", "ut")] + ["1" if e["planned"] else "0"]
        out += [number(c) for c in e["center"] + e["position"] + e["velocity"]]
    body = " ".join(out)
    if len(body) > MAX_REPLY:
        raise ValueError("orbit report over 4096 bytes")
    return body


def decode_body(body: str) -> dict | None:
    """The inverse of encode_body, as strict as the HMI's parser (hmi-cpp parse_redux_orbit). Raises ValueError."""
    if len(body) > MAX_REPLY:
        raise ValueError("oversized")
    if body == "-":
        return None
    tokens = body.split(" ")
    if any(not t or not t.isprintable() for t in tokens):
        raise ValueError("bad spacing")
    it = iter(tokens)

    def take():
        try:
            return next(it)
        except StopIteration:
            raise ValueError("truncated") from None

    def real():
        t = take()
        if not re.fullmatch(r"[0-9+\-.eE]{1,40}", t):
            raise ValueError("bad number %r" % t)
        v = float(t)
        if not math.isfinite(v):
            raise ValueError("non-finite")
        return v

    def flag():
        t = take()
        if t not in ("0", "1"):
            raise ValueError("bad flag")
        return t == "1"

    def vec():
        return (real(), real(), real())

    def digit(most):
        t = take()
        if len(t) != 1 or not "0" <= t <= str(most):
            raise ValueError("bad count")
        return int(t)

    r = {"ut": real(), "body": unhexname(take()), "mu": real(), "position": vec(), "velocity": vec()}
    mask = digit(7)
    if not r["body"] or not r["mu"] > 0 or (mask & 4 and not mask & 2):
        raise ValueError("bad report")
    if mask & 1:
        r["maneuver"] = {"ut": real(), "soi": real(), "position": vec(), "velocity": vec()}
    if mask & 2:
        r["target"] = {"name": unhexname(take()), "body": flag(), "radius": real(), "position": vec(), "velocity": vec()}
    if mask & 4:
        r["approach"] = {"ut": real(), "planned": flag(), "vessel": vec(), "target": vec()}
    r["encounters"] = []
    for _ in range(digit(4)):
        e = {"body": unhexname(take()), "radius": real(), "mu": real(), "soi": real(), "ut": real(), "planned": flag(),
             "center": vec(), "position": vec(), "velocity": vec()}
        r["encounters"].append(e)
    if next(it, None) is not None:
        raise ValueError("trailing tokens")
    return r


def zup(v) -> tuple[float, float, float]:
    """kspsim's y-up kRPC frame -> Redux's Zup frame."""
    return (float(v[0]), float(v[2]), float(v[1]))


def _sub(a, b):
    return tuple(a[i] - b[i] for i in range(3))


def report_from_sim(sim, events=ks.path_events) -> dict | None:
    """The simulated orbit as a KSP2ORB1 report: the vessel around Kerbin, the post-burn state at the node, the Mun
    as the target with its closest approach, and Mun encounters on the current and planned paths (kRPC's own
    stand-in values, so both sources agree). None outside flight."""
    if sim.scene != "flight" or sim.vessel is None:
        return None
    r = sim.reported()
    ut = sim.ut
    pos = tuple(r["position_" + a] for a in "xyz")
    vel = tuple(r["velocity_" + a] for a in "xyz")
    report = {"ut": ut, "body": "Kerbin", "mu": physics.MU, "position": zup(pos), "velocity": zup(vel), "encounters": []}
    paths = [(False, ut, pos, vel)]
    if r.get("node_enabled"):
        npos, nvel = physics.maneuver_state(r)
        node_ut = ut + r["node_time"]
        report["maneuver"] = {"ut": node_ut, "soi": physics.SOI_RADIUS, "position": zup(npos), "velocity": zup(nvel)}
        paths.append((True, node_ut, npos, nvel))
    if r.get("target_enabled"):
        mun_pos, mun_vel = ks.mun_state(ut)
        report["target"] = {"name": "Mun", "body": True, "radius": ks.MUN_RADIUS, "position": zup(mun_pos),
                            "velocity": zup(mun_vel)}
    for planned, t0, p0, v0 in paths:
        found = events(t0, p0, v0)
        if r.get("target_enabled") and planned == bool(r.get("node_enabled")):  # approach on the post-burn patch
            when = found["approach"]
            vessel = physics.propagate3(p0, v0, when - t0)[0]
            report["approach"] = {"ut": when, "planned": planned, "vessel": zup(vessel),
                                  "target": zup(ks.mun_state(when)[0])}
        if "soi" in found:
            when = found["soi"]
            vp, vv = physics.propagate3(p0, v0, when - t0)
            mp, mv = ks.mun_state(when)
            report["encounters"].append({"body": "Mun", "radius": ks.MUN_RADIUS, "mu": ks.MUN_MU, "soi": ks.MUN_SOI,
                                         "ut": when, "planned": planned, "center": zup(mp),
                                         "position": zup(_sub(vp, mp)), "velocity": zup(_sub(vv, mv))})
    return report


def reply(request: bytes, sim, events=ks.path_events) -> bytes | None:
    """The datagram answering `request`, or None (malformed, another request: no reply, as the plugin)."""
    if len(request) > 160:
        return None
    try:
        match = REQUEST.fullmatch(request.decode("ascii"))
    except UnicodeDecodeError:
        return None
    if not match:
        return None
    try:
        body = encode_body(report_from_sim(sim, events))
    except ValueError as e:  # the encoder refuses non-finite values: no report, as the plugin
        log.warning("no orbit report: %s", e)
        return None
    return ("KSP2ORB1 %s %s" % (match.group(1), body)).encode("ascii")


class ReduxOrbitServer:
    """UDP KSP2ORB1 responder, one thread; `port` 0 picks a free one (tests)."""

    def __init__(self, sim, address: str = "0.0.0.0", port: int = REDUX_PORT) -> None:
        self.sim, self.address, self.port = sim, address, port
        self._sock = None
        self._cache = (None, None)

    def _events(self, t0, r, v):
        key = (t0, tuple(r), tuple(v))  # one search per path and tick, however often it is asked
        if self._cache[0] != key:
            self._cache = (key, ks.path_events(t0, r, v))
        return self._cache[1]

    def start(self) -> None:
        if self._sock is not None:
            return
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind((self.address, self.port))
        self.port = sock.getsockname()[1]
        sock.settimeout(0.2)
        self._sock = sock
        threading.Thread(target=self._serve, args=(sock,), daemon=True, name="redux-orbit").start()
        log.info("KSP2ORB1 orbit report on UDP %s:%d", self.address, self.port)

    def _serve(self, sock) -> None:
        while self._sock is sock:
            try:
                data, peer = sock.recvfrom(256)
            except socket.timeout:
                continue
            except OSError:
                return
            out = reply(data, self.sim, self._events)
            if out is not None:
                sock.sendto(out, peer)

    def stop(self) -> None:
        sock, self._sock = self._sock, None
        if sock is not None:
            sock.close()
