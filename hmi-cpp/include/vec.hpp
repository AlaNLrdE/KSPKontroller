// Vectors, quaternions and the kRPC attitude conventions of the NavBall.
// Surface axes are (up, north, east); vessel axes are right, forward, down.
#pragma once

#include <cmath>
#include <optional>

namespace ksphmi {

struct Vec3 {
    double x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    bool operator==(const Vec3& o) const { return x == o.x && y == o.y && z == o.z; }
    bool operator!=(const Vec3& o) const { return !(*this == o); }
    bool finite() const { return std::isfinite(x) && std::isfinite(y) && std::isfinite(z); }
};

struct Quat {
    double x = 0, y = 0, z = 0, w = 1;
};

inline double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

/// Unit vector, or nothing for a zero / non-finite vector.
std::optional<Vec3> unit(const Vec3& v);

/// Rotate v by q (normalised first). Throws std::invalid_argument for a zero / non-finite quaternion.
Vec3 rotate(const Quat& q, const Vec3& v);

struct Basis {
    Vec3 right, forward, up;
};

/// Vessel axes in surface coordinates: right, forward and up (= -down).
Basis basis(const Quat& q);

/// Euler angles (degrees) to a vessel-to-surface quaternion, used by the demo source and the simulator.
Quat attitude(double pitch, double heading, double roll);

/// Spherical interpolation from a (t = 0) to b (t = 1) along the shortest arc; both are normalised first.
/// Returns b for a zero / non-finite input.
Quat slerp(const Quat& a, const Quat& b, double t);

/// Rotation angle in degrees between two attitudes (0..180), or 180 for a zero / non-finite input.
double angle_between(const Quat& a, const Quat& b);

/// Burn duration for dv with the rocket equation; nothing when an input is missing, non-finite or <= 0.
std::optional<double> burn_time(double dv, double mass, double thrust, double isp);

constexpr double PI = 3.14159265358979323846;
inline double radians(double d) { return d * PI / 180.0; }
inline double degrees_of(double r) { return r * 180.0 / PI; }

}  // namespace ksphmi
