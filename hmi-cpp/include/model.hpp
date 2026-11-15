// Telemetry data model (ports of the dataclasses in navigation.py, systems.py, orbitview.py and telemetry.py).
// Distances in metres, speeds in m/s, times in seconds, angles as documented per field.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "vec.hpp"

namespace ksphmi {

// ------------------------------------------------------------------------------------------- navigation
struct Navigation {
    Quat rotation{0, 0, 0, 1};
    double pitch = 0, heading = 0, roll = 0;
    std::string speed_mode = "orbit";
    std::optional<double> speed;
    std::optional<Vec3> prograde, normal, radial, target, maneuver;
    bool sas = false;
    std::string sas_mode = "stability_assist";
    bool rcs = false;
    double throttle = 0, g_force = 0;
    std::optional<double> node_dv, node_remaining, node_time, burn_seconds;
};

// ------------------------------------------------------------------------------------------- systems
inline const std::array<const char*, 6> RESOURCE_NAMES = {"ElectricCharge", "LiquidFuel", "Oxidizer",
                                                          "MonoPropellant", "SolidFuel",  "XenonGas"};
inline const std::array<const char*, 6> RESOURCE_LABELS = {"ELECTRICITY", "LIQUID FUEL", "OXIDIZER",
                                                           "MONOPROPELLANT", "SOLID FUEL", "XENON GAS"};

/// Vessel total in KSP units. 0 capacity = not installed, 0 amount = empty, missing = unknown.
struct Resource {
    std::string name;
    std::optional<double> amount, capacity;
    std::optional<double> fraction() const {
        if (!amount || !capacity || *capacity <= 0) return std::nullopt;
        if (!std::isfinite(*amount) || !std::isfinite(*capacity)) return std::nullopt;
        return std::max(0.0, std::min(1.0, *amount / *capacity));
    }
};

struct Stage {
    int number = 0;
    std::optional<int> parts;
    std::optional<double> delta_v;
};

/// Vessel totals and the read-only activation-stage queue.
struct Systems {
    std::optional<int> current_stage;
    std::vector<Resource> resources;
    std::vector<Stage> stages;
};

// ------------------------------------------------------------------------------------------- orbit view
struct Axes {
    Vec3 x{1, 0, 0}, y{0, 0, 1};
};

/// Choose a fixed inertial view plane once per vessel/body, never from the eccentricity vector.
Axes view_axes(const Vec3& position, const Vec3& velocity);

struct Conic {
    double p = 0, eccentricity = 0;
    Vec3 peri_direction, transverse;
    double periapsis() const { return p / (1 + eccentricity); }
    std::optional<double> apoapsis() const {
        if (eccentricity < 1) return p / (1 - eccentricity);
        return std::nullopt;
    }
    std::optional<Vec3> position(double anomaly) const;
};

/// Measured position/velocity in the body's non-rotating frame, plus the fixed view axes.
struct OrbitState {
    Vec3 position, velocity;
    double mu = 0;
    Axes axes;
    Point2 project(const Vec3& v) const { return {dot(v, axes.x), dot(v, axes.y)}; }
    /// The marker is always the measured Cartesian position. Never reconstruct it from true anomaly:
    /// periapsis is ill-conditioned in circular orbits (RCS impulses flip it).
    Point2 vessel_point() const { return project(position); }
    std::optional<Conic> conic() const;
};

/// The orbit that results from the next maneuver node (kRPC Node.orbit), as the post-burn state at the node's UT
/// in the same body non-rotating frame as OrbitState, so it shares the view axes. Recomputed only when the node
/// changes (and every few seconds while it exists), never per frame; unchanged values stay bit-identical, which
/// keeps the display's dashed-line cache valid.
struct ManeuverOrbit {
    Vec3 position, velocity;  ///< post-burn state at the burn point
    double ut = 0;
    double soi = 0;  ///< sphere of influence radius of the body; <= 0 or non-finite = unbounded
    std::optional<Conic> conic(const OrbitState& frame) const {
        return OrbitState{position, velocity, frame.mu, frame.axes}.conic();
    }
};

/// Where a conic is drawn: the true-anomaly interval from `from` to `to` (radians, increasing = direction of
/// motion). `closed` when it is the whole ellipse inside `limit`.
struct ConicSpan {
    double from = 0, to = 0;
    bool closed = false;
};

/// The part of a conic to draw inside radius `limit` (<= 0 or non-finite = unbounded): the whole ellipse when its
/// apoapsis fits, otherwise from `start` (the burn point's true anomaly) forward to the limit or the asymptote,
/// as KSP draws a patch that leaves the sphere of influence. Nothing when `start` is already outside the limit.
std::optional<ConicSpan> conic_span(const Conic& conic, double start, double limit);

/// True anomaly of a position on a conic.
double anomaly_of(const Conic& conic, const Vec3& position);

/// Sample a span into projected points (metres in the view plane); a closed span returns an open list whose last
/// point equals the first.
std::vector<Point2> conic_track(const Conic& conic, const ConicSpan& span, const Axes& axes, int samples);

/// Cut a polyline into dashes of `dash` length separated by `gap`, measured along the line so the pattern keeps a
/// constant length on screen. Each dash is a short polyline. `phase` offsets the pattern start.
std::vector<std::vector<Point2>> dash_polyline(const std::vector<Point2>& points, double dash, double gap,
                                               double phase = 0);

// ------------------------------------------------------------------------------------------- snapshot
enum class State { Connecting, NoVessel, Flight };

/// One sample of the active vessel; immutable once published.
struct Snapshot {
    State state = State::Connecting;
    std::string detail;  ///< why we are not in FLIGHT, shown on the waiting screen
    std::string scene;   ///< KSPSerialIO scene context, empty if unavailable
    std::string server;  ///< current endpoint, including a server found after DHCP changes
    std::string skin;    ///< HMI look requested by the context reply (the simulator), "" = the [display] setting
    std::string vessel, situation, body;
    double body_radius = 0, atmosphere_depth = 0, ut = 0, altitude = 0;
    double apoapsis = 0, periapsis = 0, time_to_apoapsis = 0, time_to_periapsis = 0;
    double inclination = 0;  ///< degrees
    double eccentricity = 0, period = 0, semi_major_axis = 0;
    double true_anomaly = 0;  ///< radians, 0 at periapsis
    double orbital_speed = 0, surface_speed = 0, vertical_speed = 0;
    std::optional<Navigation> nav;
    std::optional<Systems> systems;
    std::optional<OrbitState> orbit_state;
    std::optional<ManeuverOrbit> maneuver_orbit;  ///< only with a node in the current sphere of influence
    double received = 0;  ///< monotonic seconds when the sample was taken
};

/// True when a FLIGHT snapshot has not been refreshed for `limit` seconds (frozen game, stalled link).
inline bool stale(const Snapshot& s, double now, double limit = 3.0) {
    return s.state == State::Flight && now - s.received > limit;
}

/// Monotonic clock in seconds.
double monotonic();

}  // namespace ksphmi
