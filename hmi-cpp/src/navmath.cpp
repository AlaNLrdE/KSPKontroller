#include "navmath.hpp"

#include <algorithm>
#include <cmath>

#include "fmt.hpp"

namespace ksphmi {

Vec3 globe(double pitch, double heading) {
    double p = radians(pitch), h = radians(heading);
    return {std::sin(p), std::cos(p) * std::cos(h), std::cos(p) * std::sin(h)};
}

std::optional<LabelProjection> label_projection(double pitch, double heading, const Basis& axes) {
    Vec3 v = projected(globe(pitch, heading), axes);
    if (v.z <= 0) return std::nullopt;
    Vec3 north = projected(globe(pitch + .5, heading), axes);
    Vec3 east = projected(globe(pitch, heading + .5), axes);
    double step = radians(.5);
    LabelProjection out;
    out.v = v;
    out.width = std::max(.08, std::hypot(east.x - v.x, east.y - v.y) / step);
    out.height = std::max(.08, std::hypot(north.x - v.x, north.y - v.y) / step);
    out.angle = degrees_of(std::atan2(north.x - v.x, -(north.y - v.y)));
    double edge = std::min(1.0, v.z / .22);
    out.alpha = static_cast<int>(fmt::round_half_even(255 * edge * edge * (3 - 2 * edge)));
    return out;
}

std::vector<Vec3> front_polygon(const std::vector<Vec3>& points) {
    std::vector<Vec3> out;
    if (points.empty()) return out;
    Vec3 previous = points.back();
    for (const Vec3& current : points) {
        if ((current.z >= 0) != (previous.z >= 0)) {
            double t = previous.z / (previous.z - current.z);
            out.push_back(previous + (current - previous) * t);
        }
        if (current.z >= 0) out.push_back(current);
        previous = current;
    }
    return out;
}

}  // namespace ksphmi
