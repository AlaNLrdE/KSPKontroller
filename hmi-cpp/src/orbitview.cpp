// Body-centred inertial orbit geometry (port of orbitview.py), independent of attitude and periapsis conventions.
#include <chrono>

#include "model.hpp"

namespace ksphmi {

double monotonic() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

Axes view_axes(const Vec3& position, const Vec3& velocity) {
    auto normal = unit(cross(position, velocity));
    if (!normal) return {{1, 0, 0}, {0, 0, 1}};
    for (const Vec3& reference : {Vec3{1, 0, 0}, Vec3{0, 0, 1}}) {
        auto right = unit(reference - *normal * dot(reference, *normal));
        if (right) return {*right, cross(*normal, *right)};
    }
    return {{1, 0, 0}, {0, 0, 1}};
}

std::optional<Conic> OrbitState::conic() const {
    double radius = std::sqrt(dot(position, position));
    if (radius <= 0 || mu <= 0 || !position.finite() || !velocity.finite() || !std::isfinite(mu)) return std::nullopt;
    Vec3 h = cross(position, velocity);
    auto normal = unit(h);
    if (!normal) return std::nullopt;
    double speed2 = dot(velocity, velocity), rv = dot(position, velocity);
    Vec3 ev = (position * (speed2 - mu / radius) - velocity * rv) * (1.0 / mu);
    double e = std::sqrt(dot(ev, ev));
    std::optional<Vec3> peri;
    if (e > 1e-8) {
        peri = unit(ev);
    } else {
        // A circle has no unique periapsis: choose a stable drawing basis.
        const Vec3& right = axes.x;
        peri = unit(right - *normal * dot(right, *normal));
        if (!peri) peri = unit(position);
    }
    if (!peri) return std::nullopt;
    Conic c;
    c.p = dot(h, h) / mu;
    c.eccentricity = e;
    c.peri_direction = *peri;
    c.transverse = cross(*normal, *peri);
    return c;
}

std::optional<Vec3> Conic::position(double anomaly) const {
    double denominator = 1 + eccentricity * std::cos(anomaly);
    if (denominator <= 1e-6) return std::nullopt;
    double radius = p / denominator;
    return (peri_direction * std::cos(anomaly) + transverse * std::sin(anomaly)) * radius;
}

}  // namespace ksphmi
