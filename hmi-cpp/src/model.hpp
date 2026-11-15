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
    std::pair<double, double> project(const Vec3& v) const { return {dot(v, axes.x), dot(v, axes.y)}; }
    /// The marker is always the measured Cartesian position. Never reconstruct it from true anomaly:
    /// periapsis is ill-conditioned in circular orbits (RCS impulses flip it).
    std::pair<double, double> vessel_point() const { return project(position); }
    std::optional<Conic> conic() const;
};

// ------------------------------------------------------------------------------------------- snapshot
enum class State { Connecting, NoVessel, Flight };

/// One sample of the active vessel; immutable once published.
struct Snapshot {
    State state = State::Connecting;
    std::string detail;  ///< why we are not in FLIGHT, shown on the waiting screen
    std::string scene;   ///< KSPSerialIO scene context, empty if unavailable
    std::string server;  ///< current endpoint, including a server found after DHCP changes
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
    double received = 0;  ///< monotonic seconds when the sample was taken
};

/// True when a FLIGHT snapshot has not been refreshed for `limit` seconds (frozen game, stalled link).
inline bool stale(const Snapshot& s, double now, double limit = 3.0) {
    return s.state == State::Flight && now - s.received > limit;
}

/// Monotonic clock in seconds.
double monotonic();

}  // namespace ksphmi
