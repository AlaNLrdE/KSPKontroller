"""Pure geometry of the RESOURCES dials: no pygame, so it is unit tested without a display.

Each dial looks like a car fuel gauge: a chrome bezel around a light face, and a ``SWEEP``-degree arc across the
upper half of the face, E (0 %) at the left end and F (100 %) at the right end. The needle pivots on a hub in the
lower centre of the face (``PIVOT`` radii below the dial centre), so the arc is centred on the hub, not on the
dial. All radii here are fractions of the dial radius (the outside of the bezel). Angles are mathematical
(counter-clockwise, y up); screen points flip y."""
from __future__ import annotations

import math

SWEEP = 110.0
START = 90.0 + SWEEP / 2  # 145 degrees: the E end, upper left of the hub
BAD_BELOW, WARN_BELOW = .10, .25  # the same thresholds as the colours of the values
BELOW_CENTRE = 1.04  # the text block starts this many radii below the dial centre, clear of the bezel

# Radii, in dial radii. Bezel from 1 down to FACE; the scale is measured from the hub.
FACE = .80          # the light face inside the chrome
PIVOT = .22         # the hub sits this far below the dial centre
SCALE = .78         # outer end of the ticks, from the hub
ZONE = (.80, .86)   # the red/amber marks along the outside of the scale, from the hub
NEEDLE = .80        # needle tip, from the hub
LETTERS = .52       # centre of the E and F letters, from the hub, at the two ends of the scale
WINDOW = (.35, .63)  # top and bottom of the odometer window, below the dial centre
WINDOW_W = .64      # width of the window


def clamp01(fraction: float) -> float:
    return 0.0 if fraction < 0 else 1.0 if fraction > 1 else float(fraction)


def gauge_angle(fraction: float) -> float:
    """Angle in radians of a fraction of the scale, seen from the hub: 0 -> 145 degrees, 0.5 -> 90 (straight
    up), 1 -> 35."""
    return math.radians(START - SWEEP * clamp01(fraction))


def pivot(cx: float, cy: float, radius: float) -> tuple[float, float]:
    """Screen position of the needle hub of a dial centred at (cx, cy)."""
    return cx, cy + PIVOT * radius


def point(px: float, py: float, radius: float, fraction: float) -> tuple[float, float]:
    """Point at ``radius`` from (px, py) (normally the hub) in the direction of a fraction of the scale."""
    a = gauge_angle(fraction)
    return px + radius * math.cos(a), py - radius * math.sin(a)


def sector(cx: float, cy: float, r_out: float, r_in: float, f0: float, f1: float,
           step_degrees: float = 4.0) -> list[tuple[float, float]]:
    """Polygon of the ring around (cx, cy) between two fractions (outer edge forward, inner edge back)."""
    f0, f1 = clamp01(f0), clamp01(f1)
    steps = max(1, math.ceil(SWEEP * abs(f1 - f0) / step_degrees))
    fractions = [f0 + (f1 - f0) * i / steps for i in range(steps + 1)]
    return ([point(cx, cy, r_out, f) for f in fractions] +
            [point(cx, cy, r_in, f) for f in reversed(fractions)])


def needle(px: float, py: float, radius: float, fraction: float) -> list[tuple[float, float]]:
    """Tapered needle polygon from a short tail behind the hub to a narrow tip at ``NEEDLE`` radii."""
    a = gauge_angle(fraction)
    dx, dy = math.cos(a), -math.sin(a)
    nx, ny = -dy, dx
    tail, tip = -.10 * radius, NEEDLE * radius
    base, point_w, tail_w = .048 * radius, .012 * radius, .032 * radius
    return [(px + dx * tail + nx * tail_w, py + dy * tail + ny * tail_w),
            (px + nx * base, py + ny * base),
            (px + dx * tip + nx * point_w, py + dy * tip + ny * point_w),
            (px + dx * tip - nx * point_w, py + dy * tip - ny * point_w),
            (px - nx * base, py - ny * base),
            (px + dx * tail - nx * tail_w, py + dy * tail - ny * tail_w)]


def gauge_layout(x: float, y: float, w: float, h: float, count: int, text_h: float,
                 pad: float) -> tuple[float, list[tuple[float, float, float, float]]]:
    """Places ``count`` dials in the rectangle; returns (radius, [(cx, cy, cell_x, cell_w)]).

    The column count with the largest radius wins. Each dial needs ``2 r`` across and
    ``r + BELOW_CENTRE r + text_h`` down (its top, the gap and the text block), plus ``pad`` around it;
    the blocks are centred in their cells."""
    if count <= 0:
        return 0.0, []
    best = None
    for cols in range(1, count + 1):
        rows = math.ceil(count / cols)
        cell_w, cell_h = w / cols, h / rows
        radius = min((cell_w - 2 * pad) / 2, (cell_h - 2 * pad - text_h) / (1 + BELOW_CENTRE))
        if best is None or radius > best[0] + 1e-9:
            best = (radius, cols, rows, cell_w, cell_h)
    radius, cols, rows, cell_w, cell_h = best
    radius = max(radius, 0.0)
    block = radius * (1 + BELOW_CENTRE) + text_h
    cells = []
    for index in range(count):
        row, col = divmod(index, cols)
        in_row = min(cols, count - row * cols)  # centre a short last row
        offset = (w - in_row * cell_w) / 2
        cell_x = x + offset + col * cell_w
        cy = y + row * cell_h + (cell_h - block) / 2 + radius
        cells.append((cell_x + cell_w / 2, cy, cell_x, cell_w))
    return radius, cells
