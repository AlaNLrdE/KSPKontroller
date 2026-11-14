// Pure geometry of the RESOURCES dials (port of hmi-pi/ksphmi/gauges.py): no SDL, unit tested in test_core.
//
// A dial sweeps SWEEP degrees clockwise from the lower left (0 %) to the lower right (100 %), with its gap at the
// bottom; the label and the amount go under it. Angles are mathematical (counter-clockwise, y up); screen points
// flip y.
#pragma once

#include <utility>
#include <vector>

namespace ksphmi::gauges {

constexpr double SWEEP = 240.0;
constexpr double START = 90.0 + SWEEP / 2;  ///< 210 degrees: the 0 % end, lower left
constexpr double BAD_BELOW = .10, WARN_BELOW = .25;  ///< the same thresholds as the colours of the values
constexpr double BELOW_CENTRE = 1.04;  ///< the text block starts this many radii below the centre, clear of the bezel

double clamp01(double fraction);
/// Angle in radians of a fraction of the scale: 0 -> 210 degrees, 0.5 -> 90 (top), 1 -> -30.
double angle(double fraction);
/// Screen point (y down) at `radius` from the centre for a fraction of the scale.
std::pair<double, double> point(double cx, double cy, double radius, double fraction);
/// Fractions splitting [f0, f1] into steps of at most `step_degrees` (both ends included).
std::vector<double> steps(double f0, double f1, double step_degrees = 4.0);

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
