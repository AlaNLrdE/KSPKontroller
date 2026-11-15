// Pure geometry of the RESOURCES dials: no SDL, unit tested in test_core.
//
// Each dial looks like a car fuel gauge: a chrome bezel around a light face, and a SWEEP-degree arc across the
// upper half of the face, E (0 %) at the left end and F (100 %) at the right end. The needle pivots on a hub in the
// lower centre of the face (PIVOT radii below the dial centre), so the arc is centred on the hub. All radii are
// fractions of the dial radius (the outside of the bezel). Angles are mathematical (counter-clockwise, y up);
// screen points flip y.
#pragma once

#include <utility>
#include <vector>

namespace ksphmi::gauges {

constexpr double SWEEP = 110.0;
constexpr double START = 90.0 + SWEEP / 2;  ///< 145 degrees: the E end, upper left of the hub
constexpr double BAD_BELOW = .10, WARN_BELOW = .25;  ///< the same thresholds as the colours of the values
constexpr double BELOW_CENTRE = 1.04;  ///< the text block starts this many radii below the centre, clear of the bezel

// Radii, in dial radii. Bezel from 1 down to FACE; the scale is measured from the hub.
constexpr double FACE = .80;                    ///< the light face inside the chrome
constexpr double PIVOT = .22;                   ///< the hub sits this far below the dial centre
constexpr double SCALE = .78;                   ///< outer end of the ticks, from the hub
constexpr double ZONE_IN = .80, ZONE_OUT = .86;  ///< the red/amber marks outside the scale, from the hub
constexpr double NEEDLE = .80;                  ///< needle tip, from the hub
constexpr double LETTERS = .52;                 ///< centre of the E and F letters, from the hub
constexpr double WINDOW_TOP = .35, WINDOW_BOTTOM = .63;  ///< odometer window, below the dial centre
constexpr double WINDOW_W = .64;                ///< width of the window

double clamp01(double fraction);
/// Angle in radians of a fraction of the scale, seen from the hub: 0 -> 145 degrees, 0.5 -> 90 (straight up),
/// 1 -> 35.
double angle(double fraction);
/// Screen position of the needle hub of a dial centred at (cx, cy).
std::pair<double, double> pivot(double cx, double cy, double radius);
/// Screen point (y down) at `radius` from (px, py) (normally the hub) for a fraction of the scale.
std::pair<double, double> point(double px, double py, double radius, double fraction);
/// Tapered needle (convex hexagon) from a short tail behind the hub to a narrow tip at NEEDLE radii.
std::vector<std::pair<double, double>> needle(double px, double py, double radius, double fraction);
/// Fractions splitting [f0, f1] into steps of at most `step_degrees` (both ends included).
std::vector<double> steps(double f0, double f1, double step_degrees = 4.0);
/// Ring between two radii around (cx, cy) from fraction f0 to f1: the outer edge forward, then the inner edge back
/// (points i and size-1-i are on the same ray, so consecutive pairs form convex quads).
std::vector<std::pair<double, double>> sector(double cx, double cy, double r_out, double r_in, double f0, double f1,
                                              double step_degrees = 4.0);

struct Cell {
    double cx, cy, cell_x, cell_w;
};
struct Layout {
    double radius = 0;
    std::vector<Cell> cells;
};
/// Places `count` dials in the rectangle; the column count with the largest radius wins. Each dial needs 2 r
/// across and r (1 + BELOW_CENTRE) + text_h down, plus `pad` around; blocks are centred in their cells.
Layout layout(double x, double y, double w, double h, int count, double text_h, double pad);

}  // namespace ksphmi::gauges
