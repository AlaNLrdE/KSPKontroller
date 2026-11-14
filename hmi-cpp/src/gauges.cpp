#include "gauges.hpp"

#include <algorithm>
#include <cmath>

namespace ksphmi::gauges {

namespace {
constexpr double PI = 3.14159265358979323846;
}

double clamp01(double fraction) { return fraction < 0 ? 0.0 : fraction > 1 ? 1.0 : fraction; }

double angle(double fraction) { return (START - SWEEP * clamp01(fraction)) * PI / 180.0; }

std::pair<double, double> point(double cx, double cy, double radius, double fraction) {
    double a = angle(fraction);
    return {cx + radius * std::cos(a), cy - radius * std::sin(a)};
}

std::vector<double> steps(double f0, double f1, double step_degrees) {
    f0 = clamp01(f0), f1 = clamp01(f1);
    int n = std::max(1, static_cast<int>(std::ceil(SWEEP * std::fabs(f1 - f0) / step_degrees)));
    std::vector<double> out;
    out.reserve(n + 1);
    for (int i = 0; i <= n; ++i) out.push_back(f0 + (f1 - f0) * i / n);
    return out;
}

Layout layout(double x, double y, double w, double h, int count, double text_h, double pad) {
    Layout out;
    if (count <= 0) return out;
    double best = -1e300, cell_w = w, cell_h = h;
    int cols = 1;
    for (int c = 1; c <= count; ++c) {
        int rows = (count + c - 1) / c;
        double cw = w / c, ch = h / rows;
        double r = std::min((cw - 2 * pad) / 2, (ch - 2 * pad - text_h) / (1 + BELOW_CENTRE));
        if (r > best + 1e-9) best = r, cols = c, cell_w = cw, cell_h = ch;
    }
    out.radius = std::max(best, 0.0);
    double block = out.radius * (1 + BELOW_CENTRE) + text_h;
    for (int index = 0; index < count; ++index) {
        int row = index / cols, col = index % cols;
        int in_row = std::min(cols, count - row * cols);  // centre a short last row
        double cell_x = x + (w - in_row * cell_w) / 2 + col * cell_w;
        double cy = y + row * cell_h + (cell_h - block) / 2 + out.radius;
        out.cells.push_back({cell_x + cell_w / 2, cy, cell_x, cell_w});
    }
    return out;
}

}  // namespace ksphmi::gauges
