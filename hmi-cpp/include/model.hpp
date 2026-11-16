// Telemetry data model (ports of the dataclasses in navigation.py, systems.py, orbitview.py and telemetry.py).
// Distances in metres, speeds in m/s, times in seconds, angles as documented per field.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "shipreport.hpp"
#include "vec.hpp"
#include "vesselview.hpp"

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
    /// Native resource totals by the game's own names (KSP 2 Redux); false = the six KSP 1 slots.
    bool native_resources = false;
    std::optional<int> current_stage;
    std::vector<Resource> resources;
    std::vector<Stage> stages;
};

// ------------------------------------------------------------------------------------------- cockpit panel
/// The switches of the cockpit panel scene, in its order.
enum class CockpitSwitch { ReactionWheels, Gear, Lights, Brakes, SolarPanels, Antennas, Radiators, CargoBays, Intakes };
constexpr int COCKPIT_SWITCHES = 9;

/// One switch as the game reports it. `on` empty = unknown; `fitted` false = the vessel has no such part (shown
/// grey, never as OFF), empty = unknown.
struct SwitchState {
    std::optional<bool> on, fitted;
};

/// ParachuteState values (kRPC): a bit per state any parachute is in.
enum ChuteState { CHUTE_STOWED, CHUTE_ARMED, CHUTE_SEMI_DEPLOYED, CHUTE_DEPLOYED, CHUTE_CUT, CHUTE_STATES };

/// What only the cockpit panel scene shows, read over kRPC while that scene is visible (telemetry.cpp
/// CockpitStreams). Every value is optional: a failing stream blanks its control, never the panel.
struct Cockpit {
    std::array<SwitchState, COCKPIT_SWITCHES> switches{};
    std::array<std::optional<bool>, 10> groups{};  ///< custom action groups 1-10 ([9] = group 10)
    std::optional<bool> abort, stage_lock;
    std::optional<int> chutes;           ///< parachute parts; 0 = none fitted
    unsigned chute_states = 0;           ///< bit (1 << ChuteState) per state of any parachute
    std::optional<int> chute_safety;     ///< worst kRPC ParachuteSafeState of the stowed/armed ones (0 safe, 1 risky, 2 unsafe)
    /// Part titles the next stage activates, with their counts, in part order; empty with `next_known` = nothing.
    std::vector<std::pair<std::string, int>> next_stage;
    bool next_known = false;
    std::optional<double> warp_rate;  ///< current time warp multiplier (1 = real time)
    const SwitchState& at(CockpitSwitch s) const { return switches[static_cast<size_t>(s)]; }
    SwitchState& at(CockpitSwitch s) { return switches[static_cast<size_t>(s)]; }
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

/// True when `anomaly` (any turn) lies inside the span.
bool span_contains(const ConicSpan& span, double anomaly);

/// `span` ended at the first point `end` (a sphere of influence entry) after `start` in the direction of motion. A
/// closed span becomes the open arc from `start` to `end`; an open one keeps its start and stops at `end` when that
/// comes first.
ConicSpan span_until(const Conic& conic, const ConicSpan& span, double start, const Vec3& end);

/// The target (SpaceCenter.TargetVessel or TargetBody) when it orbits the active vessel's body: its state now in the
/// same body non-rotating frame as OrbitState, so its conic shares the view axes. Read about once a second.
struct TargetOrbit {
    std::string name;
    bool body = false;    ///< a celestial body (drawn as a disc), else a vessel
    Vec3 position, velocity;
    double radius = 0;    ///< body equatorial radius; 0 for a vessel
    std::optional<Conic> conic(const OrbitState& frame) const {
        return OrbitState{position, velocity, frame.mu, frame.axes}.conic();
    }
};

/// KSP's two closest approach markers: where the vessel (on the post-maneuver orbit when a node exists) and the
/// target are at the time of closest approach (kRPC Orbit.NextClosestApproach), in the OrbitState frame.
struct ClosestApproach {
    double ut = 0;
    bool planned = false;  ///< on the post-maneuver orbit
    Vec3 vessel, target;
    double distance() const {
        Vec3 d = target - vessel;
        return std::sqrt(dot(d, d));
    }
};

/// A sphere of influence entry into a moon of the current body (Orbit.NextOrbit), on the current orbit or on the
/// post-maneuver orbit. As in KSP's map view, the patch is drawn around the moon's position at the entry time.
struct Encounter {
    std::string body;
    double radius = 0, mu = 0, soi = 0;
    double ut = 0;            ///< sphere of influence entry
    bool planned = false;     ///< on the post-maneuver orbit
    Vec3 center;              ///< the moon at `ut`, in the OrbitState frame
    Vec3 position, velocity;  ///< the vessel relative to the moon at `ut` (same axes)
    Vec3 entry() const { return center + position; }
    std::optional<Conic> conic() const;
    /// The arc inside the moon's sphere of influence: from the entry to the exit, or the whole ellipse once captured.
    std::optional<ConicSpan> span() const;
    /// Where the patch leaves the sphere of influence, relative to the moon; nothing when it stays inside.
    std::optional<Vec3> exit() const;
    /// Periapsis altitude above the moon's surface (negative: impact).
    std::optional<double> periapsis_altitude() const;
};

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
    std::string game;    ///< "ksp1" (kRPC answered) or "ksp2" (KSP2 Redux UDP answered); "" before any game
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
    std::optional<TargetOrbit> target_orbit;      ///< only when the target orbits the same body (KSP 1 kRPC)
    std::optional<ClosestApproach> approach;      ///< closest approach to that target
    std::vector<Encounter> encounters;            ///< moon SOI entries: current orbit and/or planned orbit
    /// VAB scene only: the editor ship report (KSPHMI kRPC service); null = unavailable, the static cards are shown.
    std::shared_ptr<const ShipReport> ship;
    /// Flight only: the VESSEL panel's side view with fuel and heat (KSPHMIFlight); null = unavailable.
    std::shared_ptr<const VesselView> vessel_view;
    /// Flight only, while the cockpit panel scene is shown: switch states, parachutes, next stage (kRPC).
    std::optional<Cockpit> cockpit;
    double received = 0;  ///< monotonic seconds when the sample was taken
};

/// True when a FLIGHT snapshot has not been refreshed for `limit` seconds (frozen game, stalled link).
inline bool stale(const Snapshot& s, double now, double limit = 3.0) {
    return s.state == State::Flight && now - s.received > limit;
}

/// Monotonic clock in seconds.
double monotonic();

}  // namespace ksphmi
