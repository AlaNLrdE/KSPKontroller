// NavBall geometry without any drawing (from navball.py): globe points, projection and label foreshortening.
#pragma once

#include <optional>
#include <vector>

#include "vec.hpp"

namespace ksphmi {

/// Unit vector on the surface globe (up, north, east) for a pitch and heading in degrees.
Vec3 globe(double pitch, double heading);

/// Surface vector to screen space of the ball: x right, y down, z towards the viewer (> 0 = visible).
inline Vec3 projected(const Vec3& v, const Basis& b) { return {dot(v, b.right), -dot(v, b.up), dot(v, b.forward)}; }

struct LabelProjection {
    Vec3 v;             ///< projected label centre
    double width = 1;   ///< horizontal foreshortening factor
    double height = 1;  ///< vertical foreshortening factor
    double angle = 0;   ///< clockwise screen rotation in degrees
    int alpha = 255;    ///< limb fade, 0..255
};

/// Continuous label projection: no collision-dependent visibility decisions (they flicker while rolling).
std::optional<LabelProjection> label_projection(double pitch, double heading, const Basis& axes);

/// Clip a globe cell against the visible hemisphere (z >= 0) before projecting it.
std::vector<Vec3> front_polygon(const std::vector<Vec3>& points);

}  // namespace ksphmi
