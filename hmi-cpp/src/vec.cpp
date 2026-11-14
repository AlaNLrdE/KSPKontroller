#include "vec.hpp"

#include <stdexcept>

namespace ksphmi {

std::optional<Vec3> unit(const Vec3& v) {
    double length = std::sqrt(dot(v, v));
    if (length > 1e-8 && std::isfinite(length)) return v * (1.0 / length);
    return std::nullopt;
}

Vec3 rotate(const Quat& q, const Vec3& v) {
    double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n < 1e-8 || !std::isfinite(n)) throw std::invalid_argument("invalid attitude quaternion");
    double x = q.x / n, y = q.y / n, z = q.z / n, w = q.w / n;
    double tx = 2 * (y * v.z - z * v.y), ty = 2 * (z * v.x - x * v.z), tz = 2 * (x * v.y - y * v.x);
    return {v.x + w * tx + y * tz - z * ty, v.y + w * ty + z * tx - x * tz, v.z + w * tz + x * ty - y * tx};
}

Basis basis(const Quat& q) { return {rotate(q, {1, 0, 0}), rotate(q, {0, 1, 0}), rotate(q, {0, 0, -1})}; }

Quat attitude(double pitch, double heading, double roll) {
    double p = radians(pitch), h = radians(heading), r = radians(roll);
    Vec3 f{std::sin(p), std::cos(p) * std::cos(h), std::cos(p) * std::sin(h)};
    Vec3 right0{0, -std::sin(h), std::cos(h)};
    Vec3 up0{std::cos(p), -std::sin(p) * std::cos(h), -std::sin(p) * std::sin(h)};
    Vec3 right = right0 * std::cos(r) - up0 * std::sin(r);
    Vec3 up = right0 * std::sin(r) + up0 * std::cos(r);
    // Proper rotation matrix (columns right, forward, down) to a quaternion via its largest component.
    double m[3][3] = {{right.x, f.x, -up.x}, {right.y, f.y, -up.y}, {right.z, f.z, -up.z}};
    double trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0) {
        double s = std::sqrt(trace + 1) * 2;
        return {(m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s, s / 4};
    }
    int i = 0;
    for (int j = 1; j < 3; ++j)
        if (m[j][j] > m[i][i]) i = j;
    int j = (i + 1) % 3, k = (i + 2) % 3;
    double s = std::sqrt(1 + m[i][i] - m[j][j] - m[k][k]) * 2;
    double q[4] = {0, 0, 0, 0};
    q[i] = s / 4;
    q[j] = (m[j][i] + m[i][j]) / s;
    q[k] = (m[k][i] + m[i][k]) / s;
    q[3] = (m[k][j] - m[j][k]) / s;
    return {q[0], q[1], q[2], q[3]};
}

std::optional<double> burn_time(double dv, double mass, double thrust, double isp) {
    for (double v : {dv, mass, thrust, isp})
        if (!std::isfinite(v) || v <= 0) return std::nullopt;
    double ve = isp * 9.80665;
    return mass * ve / thrust * -std::expm1(-dv / ve);
}

}  // namespace ksphmi
