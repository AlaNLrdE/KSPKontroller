"""Pure geometry of the RESOURCES dials: no pygame, so it is unit tested without a display.

A dial sweeps ``SWEEP`` degrees clockwise from the lower left (0 %) to the lower right (100 %), with its gap at
the bottom under which the label and the amount go. Angles are mathematical (counter-clockwise, y up); screen points
flip y."""
from __future__ import annotations

import math

SWEEP = 240.0
START = 90.0 + SWEEP / 2  # 210 degrees: the 0 % end, lower left
BAD_BELOW, WARN_BELOW = .10, .25  # the same thresholds as the colours of the values
BELOW_CENTRE = 1.04  # the text block starts this many radii below the dial centre, clear of the bezel


def clamp01(fraction: float) -> float:
    return 0.0 if fraction < 0 else 1.0 if fraction > 1 else float(fraction)


def gauge_angle(fraction: float) -> float:
    """Angle in radians of a fraction of the scale: 0 -> 210 degrees, 0.5 -> 90 (top), 1 -> -30."""
    return math.radians(START - SWEEP * clamp01(fraction))


def point(cx: float, cy: float, radius: float, fraction: float) -> tuple[float, float]:
    a = gauge_angle(fraction)
    return cx + radius * math.cos(a), cy - radius * math.sin(a)


def sector(cx: float, cy: float, r_out: float, r_in: float, f0: float, f1: float,
           step_degrees: float = 4.0) -> list[tuple[float, float]]:
    """Polygon of the ring between two fractions (outer edge forward, inner edge back)."""
    f0, f1 = clamp01(f0), clamp01(f1)
    steps = max(1, math.ceil(SWEEP * abs(f1 - f0) / step_degrees))
    fractions = [f0 + (f1 - f0) * i / steps for i in range(steps + 1)]
    return ([point(cx, cy, r_out, f) for f in fractions] +
            [point(cx, cy, r_in, f) for f in reversed(fractions)])


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
