#include "steamdials.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "fmt.hpp"
#include "vec.hpp"

namespace ksphmi {
std::array<SteamDial, 10> steam_dials(const Snapshot& s) {
    return {{{s.altitude, 0, 500000, 1000, "km"},
             {s.orbital_speed, 0, 5000, 1000, "km/s"},
             {s.eccentricity >= 1 ? std::numeric_limits<double>::quiet_NaN() : s.apoapsis, 0, 1000000, 1000, "km"},
             {s.periapsis, -100000, 500000, 1000, "km"},
             {s.vertical_speed, -500, 500, 1, "m/s"},
             {s.surface_speed, 0, 5000, 1000, "km/s"},
             {s.inclination, 0, 180, 1, "deg"},
             {s.eccentricity, 0, 2, 1, "e"},
             {s.eccentricity >= 1 ? std::numeric_limits<double>::quiet_NaN() : s.period, 0, 7200, 60, "min"},
             {s.semi_major_axis, 0, 2000000, 1000, "km"}}};
}
std::optional<double> steam_dial_fraction(const SteamDial& dial) {
    if (!std::isfinite(dial.value) || !std::isfinite(dial.minimum) || !std::isfinite(dial.maximum) ||
        dial.maximum <= dial.minimum || !std::isfinite(dial.maximum - dial.minimum))
        return std::nullopt;
    if (dial.value <= dial.minimum) return 0;
    if (dial.value >= dial.maximum) return 1;
    return (dial.value - dial.minimum) / (dial.maximum - dial.minimum);
}
SteamDialGeometry steam_dial_geometry(const Canvas& c, const Rect& tile) {
    const double radius = std::min(tile.w * .47, (tile.h - 4.5 * c.u) / 2);
    return {double(tile.centerx()), tile.y + 1.75 * c.u + radius, radius,
            Rect::f(tile.x + .6 * c.u, tile.bottom() - 2.65 * c.u, tile.w - 1.2 * c.u, 2.4 * c.u)};
}
void steam_dial_scale(Canvas& c, const SteamDialGeometry& g, const SteamDial& dial) {
    constexpr Color ink{240, 194, 102};
    const double r = g.r;
    for (int i = 0; i <= 20; ++i) {
        const double angle = radians(135 + i * 270.0 / 20), dx = std::cos(angle), dy = std::sin(angle);
        const double outer = .73 * r, inner = (i % 5 == 0 ? .61 : .67) * r;
        c.line({float(g.cx + dx * inner), float(g.cy + dy * inner)},
               {float(g.cx + dx * outer), float(g.cy + dy * outer)}, std::max(1.0f, float(.018 * r)), ink);
        if (i % 5 == 0) {
            const double v = (dial.minimum + (dial.maximum - dial.minimum) * i / 20) / dial.divisor;
            std::string caption = fmt::str(std::fabs(v) < 10 && v != std::floor(v) ? "%.1f" : "%.0f", v);
            const double size = std::min(2.0 * c.u, .30 * r);
            c.text(caption, float(size / c.u), ink, g.cx + dx * .47 * r, g.cy + dy * .47 * r, Anchor::Center);
        }
    }
    c.text(dial.unit, float(std::min(1.4 * c.u, .22 * r) / c.u), Color{188, 158, 100}, g.cx, g.cy + .57 * r,
           Anchor::Center);
}
void steam_dial_needle(Canvas& c, const SteamDialGeometry& g, const SteamDial& dial) {
    constexpr Color gold{255, 196, 66}, dark{45, 25, 9};
    const auto fraction = steam_dial_fraction(dial);
    if (!fraction) return;
    const double angle = radians(135 + *fraction * 270), dx = std::cos(angle), dy = std::sin(angle), tx = -dy, ty = dx;
    const double reach = g.r * .68, tail = g.r * .13, half = std::max(1.0, g.r * .025);
    c.polygon({{float(g.cx + dx * reach + 1), float(g.cy + dy * reach + 1)},
               {float(g.cx - dx * tail + tx * half * 2 + 1), float(g.cy - dy * tail + ty * half * 2 + 1)},
               {float(g.cx - dx * tail - tx * half * 2 + 1), float(g.cy - dy * tail - ty * half * 2 + 1)}},
              dark);
    c.polygon({{float(g.cx + dx * reach), float(g.cy + dy * reach)},
               {float(g.cx - dx * tail + tx * half), float(g.cy - dy * tail + ty * half)},
               {float(g.cx - dx * tail - tx * half), float(g.cy - dy * tail - ty * half)}},
              gold);
    c.circle(float(g.cx), float(g.cy), float(g.r * .115), dark);
    c.circle(float(g.cx), float(g.cy), float(g.r * .085), Color{171, 111, 36});
    c.circle(float(g.cx - g.r * .02), float(g.cy - g.r * .025), float(g.r * .045), Color{246, 220, 147});
    if (dial.value < dial.minimum || dial.value > dial.maximum)
        c.text("LIMIT", 1.0f, gold, g.cx, g.cy - g.r * .25, Anchor::Center);
    // The small optical glint sits above the pointer; scale/face highlights are cached artwork.
    c.arc(float(g.cx), float(g.cy), float(g.r * .77), radians(195), radians(235), std::max(1.0f, float(g.r * .025)),
          Color{116, 177, 174});
}
}  // namespace ksphmi
