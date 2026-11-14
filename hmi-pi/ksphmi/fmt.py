"""Number formatting for the screens: short, fixed-width-ish strings that do not jump around as values change."""
from __future__ import annotations

import math


def distance(m: float) -> str:
    """Metres as m / km / Mm / Gm with 3 significant-ish digits: ``950 m``, ``80.0 km``, ``12.4 Mm``."""
    if not math.isfinite(m):
        return "--"
    a = abs(m)
    if a < 10_000:
        return "%.0f m" % m
    if a < 10_000_000:
        return "%.1f km" % (m / 1e3)
    if a < 10_000_000_000:
        return "%.2f Mm" % (m / 1e6)
    return "%.2f Gm" % (m / 1e9)


def speed(ms: float) -> str:
    if not math.isfinite(ms):
        return "--"
    if abs(ms) < 10_000:
        return "%.1f m/s" % ms
    return "%.2f km/s" % (ms / 1e3)


def duration(s: float, day_hours: int = 6) -> str:
    """Seconds as ``45s``, ``12m 03s``, ``2h 05m``, ``3d 4h`` (KSP days are 6 h unless ``day_hours`` says 24)."""
    if not math.isfinite(s):
        return "--"
    sign = "-" if s < 0 else ""
    s = int(round(abs(s)))
    day = day_hours * 3600
    d, s = divmod(s, day)
    h, s = divmod(s, 3600)
    m, s = divmod(s, 60)
    if d:
        return "%s%dd %dh" % (sign, d, h)
    if h:
        return "%s%dh %02dm" % (sign, h, m)
    if m:
        return "%s%dm %02ds" % (sign, m, s)
    return "%s%ds" % (sign, s)


def clock(ut: float, day_hours: int = 6, year_days: int = 426) -> str:
    """Game universal time as the KSP calendar date ``Y1 D001 00:00:00`` (both counted from 1)."""
    if not math.isfinite(ut) or ut < 0:
        return "--"
    t = int(ut)
    day = day_hours * 3600
    y, t = divmod(t, day * year_days)
    d, t = divmod(t, day)
    h, t = divmod(t, 3600)
    m, s = divmod(t, 60)
    return "Y%d D%03d %02d:%02d:%02d" % (y + 1, d + 1, h, m, s)


def degrees(v: float) -> str:
    return "--" if not math.isfinite(v) else "%.2f°" % v


def situation(name: str) -> str:
    """kRPC VesselSituation name to a label: ``sub_orbital`` -> ``Sub-orbital``."""
    labels = {"pre_launch": "Pre-launch", "sub_orbital": "Sub-orbital", "splashed": "Splashed down"}
    return labels.get(name, name.replace("_", " ").capitalize())
