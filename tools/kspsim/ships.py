"""Sample editor ships for the KSPHMI kRPC service (KSPSerialIO.Hmi.dll's ship report), text format version 1.

``report_text`` writes exactly what the plugin's ShipReportFormat writes (KSPSerialIO/KSPSerialIO.Hmi): the test
compares both with hmi-cpp/tests/data/ship_report_v1.txt, which the Pi's parser reads too.
"""
from __future__ import annotations

import math

VERSION = 1
# kspsim's ship choices: "" = no KSPHMI service at all (KSPSerialIO.Hmi.dll or kRPC missing: the Pi keeps its cards)
NAMES = ("", "kerbal_x", "pending", "heavy", "empty")


def escape(text: str) -> str:
    """Percent-encode every UTF-8 byte outside A-Z a-z 0-9 . _ ~ - (spaces, '=', ',' and '%' included)."""
    plain = set(b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._~-")
    return "".join(chr(b) if b in plain else "%%%02X" % b for b in text.encode("utf-8"))


def number(v, decimals: int) -> str:
    if v is None or not math.isfinite(v):
        return "nan"
    s = "%.*f" % (decimals, v)
    return s[1:] if s.startswith("-") and float(s) == 0 else s


def report_text(revision: int, ship: dict) -> str:
    """The whole ShipReport() answer for one revision."""
    lines = ["v=%d rev=%d facility=%s name=%s sit=%s" % (VERSION, revision, escape(ship["facility"]),
                                                          escape(ship["name"]), escape(ship["situation"]))]
    summary = "mass=%s dry=%s cost=%s" % (number(ship["mass"], 2), number(ship["dry"], 2), number(ship["cost"], 0))
    if ship.get("funds") is not None:
        summary += " funds=%s" % number(ship["funds"], 0)
    summary += " parts=%d crew=%d seats=%d size=%s,%s engines=%d" % (
        ship["parts"], ship["crew"], ship["seats"], number(ship["height"], 1), number(ship["width"], 1), ship["engines"])
    lines.append(summary)
    if ship.get("pending"):
        lines.append("dv=pending")
    else:
        for st in ship["stages"]:
            fields = [str(st["stage"]), number(st["dv"], 0), number(st["vac"], 0), number(st["twr"], 2),
                      number(st["burn"], 0)]
            fields += ["%s:%d" % (escape(kind), n) for kind, n in st["events"]]
            lines.append("st=" + ",".join(fields))
        if ship["stages"]:  # Kerbin sea-level delta-v per stage (plugin 0.19.4.1+); the samples are kerbin_asl
            lines.append("kasl=" + ",".join("%d:%s" % (st["stage"], number(st.get("kasl", st["dv"]), 0))
                                            for st in ship["stages"]))
    return "\n".join(lines) + "\n"


def _kerbal_x() -> dict:
    return dict(facility="vab", name="Kerbal X", situation="kerbin_asl", mass=18.4, dry=6.1, cost=23400, funds=41200,
                parts=42, crew=1, seats=3, height=11.2, width=3.8, engines=5, stages=[
                    dict(stage=2, dv=1720, vac=1960, twr=1.63, burn=48, events=[("engines", 3), ("clamps", 1)]),
                    dict(stage=1, dv=1890, vac=2010, twr=1.41, burn=71, events=[("engines", 1), ("decouplers", 1)]),
                    dict(stage=0, dv=1210, vac=1340, twr=0.62, burn=52,
                         events=[("engines", 1), ("decouplers", 1), ("chutes", 1)])])


def ship(name: str) -> dict | None:
    """A sample ship by name (see NAMES); None for "" (no service)."""
    if name == "kerbal_x":  # reaches Minmus but is 330 m/s short of a Mun landing
        return _kerbal_x()
    if name == "pending":  # just modified: KSP has not finished its delta-v calculation
        return dict(_kerbal_x(), pending=True, stages=[])
    if name == "heavy":  # sandbox (no funds), launch TWR below 1: it cannot leave the pad
        return dict(facility="vab", name="Heavy Lifter", situation="kerbin_asl", mass=96.2, dry=31.5, cost=84200,
                    funds=None, parts=118, crew=3, seats=3, height=24.6, width=7.5, engines=9, stages=[
                        dict(stage=3, dv=2150, vac=2480, twr=0.74, burn=95, events=[("engines", 7), ("clamps", 4)]),
                        dict(stage=2, dv=1980, vac=2120, twr=0.95, burn=120, events=[("engines", 1), ("decouplers", 6)]),
                        dict(stage=1, dv=1630, vac=1700, twr=0.58, burn=140, events=[("engines", 1), ("decouplers", 1)]),
                        dict(stage=0, dv=0, vac=0, twr=0, burn=0, events=[("decouplers", 1), ("chutes", 3)])])
    if name == "empty":  # New in the editor: no parts, KSP never computes delta-v (the Pi shows EMPTY CRAFT)
        return dict(facility="vab", name="Untitled Space Craft", situation="kerbin_asl", mass=0, dry=0, cost=0,
                    funds=41200, parts=0, crew=0, seats=0, height=0, width=0, engines=0, pending=True, stages=[])
    if name == "":
        return None
    raise ValueError("unknown sample ship %r (choose from %s)" % (name, ", ".join(n or '""' for n in NAMES)))
