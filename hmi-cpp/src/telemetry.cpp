#include "telemetry.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <tuple>

#include "fmt.hpp"
#include "log.hpp"
#include "pb.hpp"
#include "scenes.hpp"

namespace ksphmi {

using krpc::Call;
using krpc::Client;
using krpc::RpcError;
using krpc::sc;

static const char* LOG = "ksphmi.telemetry";

// ================================================================================================ Source
Source::Source(const char* name) : name_(name), latest_(std::make_shared<Snapshot>()) {}

Source::~Source() { stop(); }

void Source::start() { thread_ = std::thread([this] { run_guarded(); }); }

void Source::request_stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
}

void Source::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    on_stop();
    if (thread_.joinable()) thread_.join();
}

std::shared_ptr<const Snapshot> Source::latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
}

void Source::publish(Snapshot snap) {
    auto p = std::make_shared<const Snapshot>(std::move(snap));
    std::lock_guard<std::mutex> lock(mutex_);
    latest_ = std::move(p);
}

bool Source::wait(double seconds) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::duration<double>(std::max(0.0, seconds)), [this] { return stop_.load() || wake_; });
    wake_ = false;
    return stop_;
}

void Source::wake() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wake_ = true;
    }
    cv_.notify_all();
}

void Source::command(const Command& c) { log::info(LOG, "key " + command_name(c) + " ignored: no game link (" + name_ + ")"); }


void Source::run_guarded() {
    try {
        run();
    } catch (const std::exception& e) {  // the display must keep running and show the problem, never die silently
        log::error(LOG, std::string("telemetry thread crashed: ") + e.what());
        Snapshot s;
        s.detail = "telemetry thread crashed, see the log";
        publish(std::move(s));
    }
}

std::string short_error(const std::exception& e) {
    std::string text = e.what();
    size_t a = text.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "error";
    text = text.substr(a);
    text = text.substr(0, text.find('\n'));
    while (!text.empty() && (text.back() == ' ' || text.back() == '\r')) text.pop_back();
    return text.substr(0, 160);
}

// ================================================================================================ kRPC helpers
namespace {

uint64_t object(Client& c, const Call& call) {
    krpc::Value v = c.invoke(call);
    return v.null ? 0 : pb::dec_object(v.data);
}
double get_double(Client& c, const Call& call) { return pb::dec_double(c.invoke(call).data); }
bool get_bool(Client& c, const Call& call) { return pb::dec_bool(c.invoke(call).data); }
std::string get_string(Client& c, const Call& call) { return pb::dec_string(c.invoke(call).data); }

std::string obj(uint64_t id) { return pb::enc_object(id); }

Vec3 vec3(const std::string& data) {
    auto t = pb::dec_tuple(data);
    if (t.size() != 3) throw pb::DecodeError("expected a 3-tuple");
    return {t[0], t[1], t[2]};
}

uint64_t stream_object(Client& c, uint64_t id) {
    krpc::Value v = c.stream(id);
    return v.null ? 0 : pb::dec_object(v.data);
}

const char* const SITUATIONS[] = {"pre_launch", "orbiting", "sub_orbital", "escaping",
                                  "flying",     "landed",   "splashed",    "docked"};
const char* const SPEED_MODES[] = {"orbit", "surface", "target"};
const char* const SAS_MODES[] = {"stability_assist", "maneuver", "prograde",    "retrograde", "normal",
                                 "anti_normal",      "radial",   "anti_radial", "target",     "anti_target"};

template <size_t N>
std::string enum_name(const char* const (&names)[N], int value) {
    return value >= 0 && static_cast<size_t>(value) < N ? names[value] : std::to_string(value);
}

/// Streams owned by one panel; removed together (Python's clear()).
class Group {
public:
    explicit Group(Client& c) : c_(c) {}
    ~Group() { clear(); }
    Group(const Group&) = delete;
    Group& operator=(const Group&) = delete;
    void add(const std::string& name, const Call& call, double rate) { ids_[name] = c_.add_stream(call, rate); }
    void clear() {
        for (auto& kv : ids_) c_.remove_stream(kv.second);
        ids_.clear();
    }
    void remove(const std::string& name) {
        auto it = ids_.find(name);
        if (it == ids_.end()) return;
        c_.remove_stream(it->second);
        ids_.erase(it);
    }
    bool has(const std::string& name) const { return ids_.count(name) != 0; }
    uint64_t id(const std::string& name) const { return ids_.at(name); }
    const std::map<std::string, uint64_t>& all() const { return ids_; }

private:
    Client& c_;
    std::map<std::string, uint64_t> ids_;
};

/// Runs an optional step; a server error (RpcError: InvalidOperationException, ArgumentException...) or an
/// undecodable value calls `failed` instead. A broken link (ConnectionError) still ends the session.
template <typename Step, typename Failed>
void optional_step(Step step, Failed failed) {
    try {
        step();
    } catch (const krpc::ConnectionError&) {
        throw;
    } catch (const std::exception&) {
        failed();
    }
}

// ------------------------------------------------------------------------------------------- navigation
/// Optional navball streams, owned separately so a missing target never interrupts orbital telemetry.
class NavigationStreams {
public:
    NavigationStreams(Client& c, uint64_t vessel, double rate) : c_(c), vessel_(vessel), rate_(rate), common_(c), extra_(c) {
        frame_ = object(c, sc("Vessel_get_SurfaceReferenceFrame", {obj(vessel)}));
        uint64_t f = object(c, sc("Vessel_Flight", {obj(vessel), obj(frame_)}));
        uint64_t control = object(c, sc("Vessel_get_Control", {obj(vessel)}));
        for (const char* name : {"Rotation", "Pitch", "Heading", "Roll", "GForce"})
            common_.add(name, sc(std::string("Flight_get_") + name, {obj(f)}), rate);
        for (const char* name : {"SpeedMode", "SAS", "SASMode", "RCS", "Throttle", "Nodes"})
            common_.add(name, sc(std::string("Control_get_") + name, {obj(control)}), rate);
        for (const char* name : {"TargetDockingPort", "TargetVessel", "TargetBody"})
            common_.add(name, sc(std::string("get_") + name), rate);
        for (const char* name : {"Mass", "AvailableThrust", "SpecificImpulse"})
            common_.add(name, sc(std::string("Vessel_get_") + name, {obj(vessel)}), rate);
    }

    void invalidate() { key_valid_ = false; }

    std::optional<Navigation> sample() {
        auto f = [&](const char* n) { return static_cast<double>(pb::dec_float(c_.stream(common_.id(n)).data)); };
        auto b = [&](const char* n) { return pb::dec_bool(c_.stream(common_.id(n)).data); };
        auto rot = pb::dec_tuple(c_.stream(common_.id("Rotation")).data);
        if (rot.size() != 4) throw pb::DecodeError("expected a quaternion");
        Navigation n;
        n.rotation = {rot[0], rot[1], rot[2], rot[3]};
        n.pitch = f("Pitch"), n.heading = f("Heading"), n.roll = f("Roll");
        double g_force = f("GForce");
        int speed_mode = pb::dec_sint32(c_.stream(common_.id("SpeedMode")).data);
        n.sas = b("SAS");
        int sas_mode = pb::dec_sint32(c_.stream(common_.id("SASMode")).data);
        n.rcs = b("RCS");
        double throttle = f("Throttle");
        auto nodes = pb::dec_items(c_.stream(common_.id("Nodes")).data);
        uint64_t dock = stream_object(c_, common_.id("TargetDockingPort"));
        uint64_t target_vessel = stream_object(c_, common_.id("TargetVessel"));
        uint64_t body = stream_object(c_, common_.id("TargetBody"));
        double mass = f("Mass"), thrust = f("AvailableThrust"), isp = f("SpecificImpulse");
        for (double v : {n.rotation.x, n.rotation.y, n.rotation.z, n.rotation.w, n.pitch, n.heading, n.roll})
            if (!std::isfinite(v)) return std::nullopt;

        std::string mode = enum_name(SPEED_MODES, speed_mode);
        uint64_t target = dock ? dock : target_vessel ? target_vessel : body;
        const char* target_class = dock ? "DockingPort" : target_vessel ? "Vessel" : "CelestialBody";
        uint64_t node = nodes.empty() ? 0 : pb::dec_object(nodes.front());
        auto key = std::make_tuple(mode, target, node);
        double now = monotonic();
        if (!key_valid_ || key != key_ || (retry_frame_at_ > 0 && now >= retry_frame_at_)) {
            extra_.clear();
            key_ = key;
            key_valid_ = true;
            retry_frame_at_ = 0;
            // Each optional marker group is independent. kRPC answers InvalidOperationException, e.g. "surface
            // speed reference frame is singular" on the launch pad: the attitude must still be shown.
            optional_step(
                [&] {
                    // The speed frames follow the body/surface/target velocity. The orbital_reference_frame
                    // co-moves with the vessel and reads zero speed.
                    std::string m = mode;
                    m[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(m[0])));
                    uint64_t frame = object(c_, sc("Vessel_get_" + m + "SpeedReferenceFrame", {obj(vessel_)}));
                    if (!frame) throw RpcError("no speed reference frame");
                    uint64_t flight = object(c_, sc("Vessel_Flight", {obj(vessel_), obj(frame)}));
                    extra_.add("speed", sc("Flight_get_Speed", {obj(flight)}), rate_);
                    const std::pair<const char*, std::vector<double>> dirs[] = {
                        {"prograde", {0, 1, 0}}, {"normal", {0, 0, 1}}, {"radial", {-1, 0, 0}}};
                    for (const auto& [name, v] : dirs)
                        extra_.add(name, sc("TransformDirection", {pb::enc_tuple(v), obj(frame), obj(frame_)}), rate_);
                },
                [&] { drop({"speed", "prograde", "normal", "radial"}); });  // no target or zero relative velocity
            if (target)
                optional_step(
                    [&] {
                        extra_.add("target", sc(std::string(target_class) + "_Position", {obj(target), obj(frame_)}),
                                   rate_);
                    },
                    [&] { drop({"target"}); });
            if (node)
                optional_step(
                    [&] {
                        extra_.add("maneuver", sc("Node_BurnVector", {obj(node), obj(frame_)}), rate_);
                        extra_.add("node_remaining", sc("Node_get_RemainingDeltaV", {obj(node)}), rate_);
                        extra_.add("node_dv", sc("Node_get_DeltaV", {obj(node)}), rate_);
                        extra_.add("node_time", sc("Node_get_TimeTo", {obj(node)}), rate_);
                    },
                    [&] { drop({"maneuver", "node_remaining", "node_dv", "node_time"}); });
        }
        std::vector<std::string> failed;
        for (const auto& [name, id] : extra_.all()) {
            try {
                std::string data = c_.stream(id).data;
                if (name == "speed") n.speed = pb::dec_double(data);
                else if (name == "prograde") n.prograde = vec3(data);
                else if (name == "normal") n.normal = vec3(data);
                else if (name == "radial") n.radial = vec3(data);
                else if (name == "target") n.target = vec3(data);
                else if (name == "maneuver") n.maneuver = vec3(data);
                else if (name == "node_remaining") n.node_remaining = pb::dec_double(data);
                else if (name == "node_dv") n.node_dv = pb::dec_double(data);
                else if (name == "node_time") n.node_time = pb::dec_double(data);
            } catch (const krpc::ConnectionError&) {
                throw;
            } catch (const std::exception&) {
                failed.push_back(name);  // target/node removed or frame became singular between updates
            }
        }
        if (!failed.empty()) drop(failed);
        n.speed_mode = mode;
        n.sas_mode = enum_name(SAS_MODES, sas_mode);
        n.throttle = std::isfinite(throttle) ? std::max(0.0, std::min(1.0, throttle)) : 0;
        n.g_force = std::isfinite(g_force) ? g_force : 0;
        n.burn_seconds = burn_time(n.node_remaining.value_or(0), mass, thrust, isp);
        return n;
    }

private:
    /// Remove failing optional streams and rebuild them shortly, without a rebuild every sample.
    void drop(const std::vector<std::string>& names) {
        for (const auto& name : names) extra_.remove(name);
        retry_frame_at_ = monotonic() + .5;
    }

    Client& c_;
    uint64_t vessel_, frame_ = 0;
    double rate_;
    Group common_, extra_;
    std::tuple<std::string, uint64_t, uint64_t> key_;
    bool key_valid_ = false;
    double retry_frame_at_ = 0;
};

// ------------------------------------------------------------------------------------------- maneuver orbit
/// The orbit after the first maneuver node (Node.orbit) as state vectors at the node's UT, read with
/// Orbit.position_at / velocity_at in the body's non-rotating frame, the frame of the current orbit. Only the node
/// identity, UT and delta-v components are streamed (2 Hz); the post-burn state is fetched again only when they
/// change, or every few seconds (KSP re-plans the patch from the current orbit, e.g. during the burn).
/// Optional: any server error means no dashed orbit, retried after a second; the session never ends because of it.
class ManeuverStreams {
public:
    ManeuverStreams(Client& c, uint64_t vessel, uint64_t body, uint64_t frame)
        : c_(c), body_(body), frame_(frame), common_(c), node_(c) {
        uint64_t control = object(c, sc("Vessel_get_Control", {obj(vessel)}));
        common_.add("nodes", sc("Control_get_Nodes", {obj(control)}), 2);
        optional_step([&] { soi_ = get_double(c, sc("CelestialBody_get_SphereOfInfluence", {obj(body)})); },
                      [&] { soi_ = 0; });  // unknown: draw without the sphere of influence limit
    }

    /// Forget the node after a failure; streams are re-created after `retry` seconds.
    void invalidate(double retry = 1.0) {
        node_.clear();
        node_id_ = 0;
        key_valid_ = false;
        result_.reset();
        retry_at_ = monotonic() + retry;
    }

    std::optional<ManeuverOrbit> sample(double mu) {
        double now = monotonic();
        if (now < retry_at_) return std::nullopt;
        auto nodes = pb::dec_items(c_.stream(common_.id("nodes")).data);
        uint64_t node = nodes.empty() ? 0 : pb::dec_object(nodes.front());
        if (node != node_id_) {
            node_.clear();
            node_id_ = node;
            key_valid_ = false;
            result_.reset();
            if (node)
                for (const char* name : {"UT", "Prograde", "Normal", "Radial"})
                    node_.add(name, sc(std::string("Node_get_") + name, {obj(node)}), 2);
        }
        if (!node) return std::nullopt;
        std::array<double, 4> key{};
        int i = 0;
        for (const char* name : {"UT", "Prograde", "Normal", "Radial"})
            key[i++] = pb::dec_double(c_.stream(node_.id(name)).data);
        if (!key_valid_ || key != key_ || now >= refresh_at_) {
            std::optional<ManeuverOrbit> fresh = compute(node, key[0], mu);  // throws: caller invalidates
            // Bit-identical values keep the display's dashed-line cache.
            if (!(fresh && result_ && fresh->position == result_->position && fresh->velocity == result_->velocity &&
                  fresh->ut == result_->ut))
                result_ = fresh;
            key_ = key;
            key_valid_ = true;
            refresh_at_ = now + 2.0;
        }
        return result_;
    }

private:
    std::optional<ManeuverOrbit> compute(uint64_t node, double ut, double mu) {
        uint64_t orbit = object(c_, sc("Node_get_Orbit", {obj(node)}));
        if (!orbit) return std::nullopt;
        // A node placed after a sphere of influence change belongs to another body's frame: not drawn.
        if (object(c_, sc("Orbit_get_Body", {obj(orbit)})) != body_) return std::nullopt;
        std::vector<std::string> args{obj(orbit), pb::enc_double(ut), obj(frame_)};
        Vec3 position = vec3(c_.invoke(sc("Orbit_PositionAt", args)).data);
        Vec3 velocity = vec3(c_.invoke(sc("Orbit_VelocityAt", args)).data);
        if (!position.finite() || !velocity.finite()) return std::nullopt;
        ManeuverOrbit m{position, velocity, ut, soi_};
        if (!checked_) {  // once per vessel session: does the state agree with kRPC's own elements?
            checked_ = true;
            optional_step(
                [&] {
                    double periapsis = get_double(c_, sc("Orbit_get_Periapsis", {obj(orbit)}));
                    auto conic = OrbitState{position, velocity, mu, {}}.conic();
                    if (conic && std::fabs(conic->periapsis() - periapsis) > .01 * periapsis + 1000)
                        log::warning(LOG, fmt::str("maneuver orbit: periapsis %.0f m from state vectors, %.0f m from "
                                                   "kRPC (frame mismatch?)", conic->periapsis(), periapsis));
                    else
                        log::info(LOG, fmt::str("maneuver orbit: periapsis %.0f m agrees with kRPC", periapsis));
                },
                [] {});
        }
        return m;
    }

    Client& c_;
    uint64_t body_, frame_;
    double soi_ = 0;
    Group common_, node_;
    uint64_t node_id_ = 0;
    std::array<double, 4> key_{};
    bool key_valid_ = false, checked_ = false;
    double refresh_at_ = 0, retry_at_ = 0;
    std::optional<ManeuverOrbit> result_;
};

// ------------------------------------------------------------------------------------------- systems
/// Resources and staging at 2 Hz, independent of attitude streams; rebuilt on stage / parts changes.
class SystemsStreams {
public:
    SystemsStreams(Client& c, uint64_t vessel) : c_(c), vessel_(vessel), common_(c), resources_(c), stages_(c) {
        uint64_t control = object(c, sc("Vessel_get_Control", {obj(vessel)}));
        common_.add("current", sc("Control_get_CurrentStage", {obj(control)}), 2);
        common_.add("stages", sc("Vessel_get_Stages", {obj(vessel)}), 2);
        uint64_t parts = object(c, sc("Vessel_get_Parts", {obj(vessel)}));
        common_.add("parts", sc("Parts_get_All", {obj(parts)}), 2);
    }

    Systems sample() {
        auto current = read<int>(common_, "current", [](const std::string& d) { return pb::dec_sint32(d); });
        auto objects = read<std::vector<uint64_t>>(common_, "stages", ids);
        auto parts = read<std::vector<uint64_t>>(common_, "parts", ids);
        auto key = std::make_tuple(current, objects.value_or(std::vector<uint64_t>{}),
                                   parts.value_or(std::vector<uint64_t>{}));
        if (!key_valid_ || key != key_) {
            resources_.clear();
            stages_.clear();
            stage_numbers_.clear();
            pending_delta_v_.clear();
            delta_v_retry_at_ = 0;
            key_ = key;
            key_valid_ = true;
            try {
                // Resource collections can retain a parts set: re-resolve them after staging, docking or
                // any other vessel topology change.
                uint64_t res = object(c_, sc("Vessel_get_Resources", {obj(vessel_)}));
                for (const char* name : RESOURCE_NAMES) {
                    resources_.add(std::string(name) + ".amount", sc("Resources_Amount", {obj(res), pb::enc_string(name)}), 2);
                    resources_.add(std::string(name) + ".capacity", sc("Resources_Max", {obj(res), pb::enc_string(name)}), 2);
                }
                std::vector<std::pair<int, uint64_t>> selected;
                for (uint64_t stage : objects.value_or(std::vector<uint64_t>{}))
                    selected.emplace_back(pb::dec_sint32(c_.invoke(sc("Stage_get_Number", {obj(stage)})).data), stage);
                std::stable_sort(selected.begin(), selected.end(),
                                 [](const auto& a, const auto& b) { return a.first > b.first; });
                int count = 0;
                for (const auto& entry : selected) {
                    const int number = entry.first;
                    const uint64_t stage = entry.second;
                    if (current && number > *current) continue;
                    if (count++ == 4) break;
                    stages_.add(std::to_string(number) + ".parts", sc("Stage_get_Parts", {obj(stage)}), 2);
                    pending_delta_v_[number] = stage;
                    stage_numbers_.push_back(number);
                }
            } catch (const krpc::ConnectionError&) {
                throw;
            } catch (const std::exception&) {  // docking or staging invalidated an object during setup
                key_valid_ = false;
            }
        }
        if (key_valid_ && !pending_delta_v_.empty() && monotonic() >= delta_v_retry_at_) {
            for (auto it = pending_delta_v_.begin(); it != pending_delta_v_.end();) {
                const int number = it->first;
                const uint64_t stage = it->second;
                bool added = false;
                // KSP can reject AddStream until it has computed this stage's delta-v.
                optional_step([&] {
                    stages_.add(std::to_string(number) + ".delta_v", sc("Stage_get_DeltaV", {obj(stage)}), 2);
                    added = true;
                }, [] {});
                if (added) it = pending_delta_v_.erase(it);
                else ++it;
            }
            delta_v_retry_at_ = monotonic() + .5;  // retry only absent streams; keep working collections intact
        }
        Systems s;
        s.current_stage = current;
        auto fl = [](const std::string& d) { return static_cast<double>(pb::dec_float(d)); };
        for (const char* name : RESOURCE_NAMES)
            s.resources.push_back({name, read<double>(resources_, std::string(name) + ".amount", fl),
                                   read<double>(resources_, std::string(name) + ".capacity", fl)});
        for (int number : stage_numbers_) {
            auto p = read<std::vector<uint64_t>>(stages_, std::to_string(number) + ".parts", ids);
            auto dv = read<double>(stages_, std::to_string(number) + ".delta_v", fl);
            // Unknown delta-v is normal (KSP has not computed it, e.g. on the pad): show it as unknown without
            // rebuilding every stream each sample. Missing parts means the stage object went stale: rebuild.
            if (!p) key_valid_ = false;
            Stage st;
            st.number = number;
            if (p) st.parts = static_cast<int>(p->size());
            st.delta_v = dv;
            s.stages.push_back(st);
        }
        return s;
    }

private:
    static std::vector<uint64_t> ids(const std::string& d) {
        std::vector<uint64_t> out;
        for (const auto& item : pb::dec_items(d)) out.push_back(pb::dec_object(item));
        return out;
    }

    /// Value of a stream, nothing when it is missing or the server reports an error.
    template <typename T, typename F>
    std::optional<T> read(const Group& g, const std::string& name, F decode) {
        if (!g.has(name)) return std::nullopt;
        try {
            return decode(c_.stream(g.id(name)).data);
        } catch (const krpc::ConnectionError&) {
            throw;
        } catch (const std::exception&) {  // server error (e.g. no delta-v for a stage) or undecodable value
            return std::nullopt;
        }
    }

    Client& c_;
    uint64_t vessel_;
    Group common_, resources_, stages_;
    std::vector<int> stage_numbers_;
    std::map<int, uint64_t> pending_delta_v_;
    double delta_v_retry_at_ = 0;
    std::tuple<std::optional<int>, std::vector<uint64_t>, std::vector<uint64_t>> key_;
    bool key_valid_ = false;
};

}  // namespace

namespace {

constexpr int WARP_RAILS = 0, WARP_PHYSICS = 1;  // kRPC SpaceCenter.WarpMode (None = 2)
constexpr int PHYSICS_WARP_MAX = 3;

int get_int(Client& c, const Call& call) { return pb::dec_sint32(c.invoke(call).data); }

/// Flip one boolean Control property ("SAS", "RCS", "Gear", "Lights"); "SAS on" / "SAS off".
std::string toggle(Client& c, uint64_t control, const std::string& property) {
    const bool on = get_bool(c, sc("Control_get_" + property, {obj(control)}));
    c.invoke(sc("Control_set_" + property, {obj(control), pb::enc_bool(!on)}));
    return property + (on ? " off" : " on");
}

/// KSP's , . / keys: physics warp while it is the active mode or rails warp is not allowed here (atmosphere, low
/// altitude), rails warp otherwise. Rails 0 / physics 0 is 1x. kRPC's RailsWarpFactor setter also cuts the
/// throttle, so physics warp is never stopped through it.
std::string warp(Client& c, Command::Kind kind) {
    const int mode = get_int(c, sc("get_WarpMode"));
    const int rails = get_int(c, sc("get_RailsWarpFactor"));
    const int physics = get_int(c, sc("get_PhysicsWarpFactor"));
    auto set = [&](bool on_rails, int factor) {
        c.invoke(sc(on_rails ? "set_RailsWarpFactor" : "set_PhysicsWarpFactor", {pb::enc_sint32(factor)}));
        return fmt::str("time warp %s %d", on_rails ? "rails" : "physics", factor);
    };
    if (kind == Command::WarpStop) {
        if (mode == WARP_PHYSICS) return set(false, 0);
        if (mode == WARP_RAILS && rails > 0) return set(true, 0);
        return "time warp already 1x";
    }
    if (kind == Command::WarpDown) {
        if (mode == WARP_PHYSICS && physics > 0) return set(false, physics - 1);
        if (mode == WARP_RAILS && rails > 0) return set(true, rails - 1);
        return "time warp already 1x";
    }
    if (mode == WARP_PHYSICS && physics > 0) return set(false, std::min(PHYSICS_WARP_MAX, physics + 1));
    if (get_int(c, sc("get_MaximumRailsWarpFactor")) > 0) return set(true, rails + 1);  // kRPC clamps to the maximum
    return set(false, std::min(PHYSICS_WARP_MAX, physics + 1));
}

}  // namespace

std::string send_command(Client& c, const Command& cmd, uint64_t vessel, const std::string& scene) {
    const std::string name = command_name(cmd);
    if (!vessel) {
        const bool board_button = cmd.kind == Command::ToggleSas || cmd.kind == Command::ToggleRcs;
        if (scene == "vab" && board_button) {
            const bool sas = cmd.kind == Command::ToggleSas;
            c.invoke(Call{"KSPHMI", "Press", {pb::enc_string(sas ? "SAS" : "RCS")}});
            return std::string(sas ? "SAS" : "RCS") + " pressed in the VAB (" + (sas ? "next" : "previous") +
                   " editor screen)";
        }
        return name + " ignored in scene '" + scene + "'";
    }
    auto control = [&] { return object(c, sc("Vessel_get_Control", {obj(vessel)})); };
    switch (cmd.kind) {
        case Command::ToggleSas: return toggle(c, control(), "SAS");
        case Command::ToggleRcs: return toggle(c, control(), "RCS");
        case Command::ToggleGear: return toggle(c, control(), "Gear");
        case Command::ToggleLights: return toggle(c, control(), "Lights");
        case Command::SetBrakes:
            c.invoke(sc("Control_set_Brakes", {obj(control()), pb::enc_bool(cmd.value != 0)}));
            return name;
        case Command::Abort:
            c.invoke(sc("Control_set_Abort", {obj(control()), pb::enc_bool(true)}));
            return name;
        case Command::Stage:
            c.invoke(sc("Control_ActivateNextStage", {obj(control())}));
            return name;
        case Command::ActionGroup:
            c.invoke(sc("Control_ToggleActionGroup", {obj(control()), pb::enc_uint32(uint32_t(cmd.value))}));
            return name;
        case Command::SasMode:
            c.invoke(sc("Control_set_SASMode", {obj(control()), pb::enc_sint32(cmd.value)}));
            return name;
        case Command::ThrottleFull:
        case Command::ThrottleCut:
            c.invoke(Call{"KSPHMIFlight", "SetThrottle", {pb::enc_float(cmd.kind == Command::ThrottleFull ? 1.f : 0.f)}});
            return name;
        case Command::WarpUp:
        case Command::WarpDown:
        case Command::WarpStop: return warp(c, cmd.kind);
        case Command::Quicksave:
            c.invoke(sc("Quicksave"));
            return name;
        case Command::Quickload:
            c.invoke(sc("Quickload"));
            return name;
    }
    return name + " unknown";
}

void send_held(Client& c, const Held& h, double throttle_dt, bool axes) {
    if (axes)
        c.invoke(Call{"KSPHMIFlight", "SetAxes", {pb::enc_float(h.pitch), pb::enc_float(h.yaw), pb::enc_float(h.roll),
                                                   pb::enc_float(h.x), pb::enc_float(h.y), pb::enc_float(h.z)}});
    if (h.throttle && throttle_dt > 0)
        c.invoke(Call{"KSPHMIFlight", "NudgeThrottle", {pb::enc_float(float(h.throttle * KEY_THROTTLE_RATE * throttle_dt))}});
}

// ================================================================================================ ShipReportPoller
void ShipReportPoller::reset() {
    report_.reset();
    revision_.reset();
    retry_at_ = -1e9;
    problem_.clear();
}

void ShipReportPoller::fail(const std::string& why, bool log_warning) {
    if (why != problem_) {
        if (log_warning) log::warning(LOG, "ship report: " + why);
        else log::info(LOG, "ship report unavailable (static VAB cards): " + why);
    }
    problem_ = why;
    report_.reset();
}

std::shared_ptr<const ShipReport> ShipReportPoller::poll(Client& c, double now) {
    if (now < retry_at_) return report_;
    uint32_t revision;
    try {
        revision = pb::dec_uint32(c.invoke(Call{"KSPHMI", "Revision", {}}).data);
    } catch (const RpcError& e) {  // no such service (DLL or kRPC missing), or not in the editor
        fail(short_error(e), false);
        revision_.reset();
        retry_at_ = now + retry_seconds_;
        return nullptr;
    } catch (const pb::DecodeError& e) {
        fail(std::string("bad revision: ") + e.what(), true);
        revision_.reset();
        retry_at_ = now + retry_seconds_;
        return nullptr;
    }
    if (revision_ && *revision_ == revision) return report_;
    revision_ = revision;
    std::string text;
    try {
        ++fetches;
        text = get_string(c, Call{"KSPHMI", "ShipReport", {}});
    } catch (const RpcError& e) {
        fail(short_error(e), false);
        revision_.reset();
        retry_at_ = now + retry_seconds_;
        return nullptr;
    } catch (const pb::DecodeError& e) {
        fail(std::string("bad report value: ") + e.what(), true);
        return nullptr;
    }
    if (text.empty()) {  // the editor has not reported a ship yet
        fail("no ship reported yet", false);
        return nullptr;
    }
    std::string error;
    auto parsed = parse_ship_report(text, &error);
    if (!parsed) {  // kept until the revision changes: the same text would fail again
        fail(fmt::str("revision %u rejected: ", revision) + error, true);
        return nullptr;
    }
    // one line per newly fetched revision (the revision only moves when the report changes)
    log::info(LOG, fmt::str("ship report: revision %u, %s, %s, %d stages%s", revision, parsed->name.c_str(),
                            parsed->situation.c_str(), static_cast<int>(parsed->stages.size()),
                            parsed->delta_v_pending ? " (delta-v pending)" : ""));
    log::info(LOG, "editor report: mode " + parsed->editor_mode);
    problem_.clear();
    report_ = std::make_shared<const ShipReport>(std::move(*parsed));
    return report_;
}

// ================================================================================================ KrpcSource
KrpcSource::KrpcSource(Options options) : Source("KrpcSource"), options_(std::move(options)) {
    period_ = 1.0 / std::max(options_.rate_hz, 0.5);
}

KrpcSource::~KrpcSource() { stop(); }

std::string KrpcSource::address() const {
    std::lock_guard<std::mutex> lock(address_mutex_);
    return options_.address;
}

void KrpcSource::publish(Snapshot snap) {
    snap.server = fmt::str("kRPC %s:%d", address().c_str(), options_.rpc_port);
    // The context channel (UDP 50002) also carries the simulator's skin choice: ask about once a second in every
    // state, flight included. Bounded (0.15 s) and on this thread, never on the frame loop.
    double now = monotonic();
    if (now - context_at_ >= 1.0) {
        context_at_ = now;
        Context ctx = read_context_reply(address(), options_.context_port);
        if (ctx.skin != context_skin_)
            log::info(LOG, "context skin: " + (ctx.skin.empty() ? std::string("none (the [display] setting)") : ctx.skin));
        context_skin_ = ctx.skin;
        if (snap.state == State::NoVessel && snap.scene.empty()) snap.scene = ctx.scene;
    }
    snap.skin = context_skin_;
    Source::publish(std::move(snap));
}

void KrpcSource::command(const Command& c) {
    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        if (commands_.size() >= 16) return;  // hammered keys must not build a backlog
        commands_.push_back(c);
    }
    wake();
}

void KrpcSource::hold(const Held& h) {
    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        if (h == held_) return;
        held_ = h;
    }
    wake();
}

void KrpcSource::send_inputs(Client& conn, uint64_t vessel, const std::string& scene) {
    std::vector<Command> todo;
    Held held;
    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        todo.swap(commands_);
        held = held_;
    }
    for (const Command& cmd : todo) {
        try {
            log::info(LOG, "key: " + send_command(conn, cmd, vessel, scene));
        } catch (const RpcError& e) {  // old KSPSerialIO.Hmi.dll, scene changed, vessel gone, staging locked
            log::warning(LOG, "key " + command_name(cmd) + " not applied: " + short_error(e));
        }
    }
    if (!vessel) {  // held keys act in flight only; the plugin's 0.5 s timeout releases what was sent before
        sent_ = Held{};
        nudged_at_ = -1;
        return;
    }
    const bool axes = held.axes() || sent_.axes();  // while held, then once more with zeros
    if (!axes && !held.throttle) {
        nudged_at_ = -1;
        return;
    }
    const double now = monotonic();
    const double dt = !held.throttle ? 0 : nudged_at_ < 0 ? period_ : std::min(0.25, now - nudged_at_);
    try {
        send_held(conn, held, dt, axes);
        if (!held_problem_.empty()) log::info(LOG, "held keys applied again");
        held_problem_.clear();
    } catch (const RpcError& e) {  // no KSPHMIFlight service: KSPSerialIO.Hmi.dll older than 0.19.6.0
        const std::string why = short_error(e);
        if (why != held_problem_) log::warning(LOG, "held keys not applied: " + why);
        held_problem_ = why;
    }
    sent_ = held;
    nudged_at_ = held.throttle ? now : -1;
}

void KrpcSource::on_stop() {
    std::lock_guard<std::mutex> lock(client_mutex_);
    if (active_) active_->close();
}

std::unique_ptr<Client> KrpcSource::connect() {
    Client::Options o;
    o.address = address();
    o.rpc_port = options_.rpc_port;
    o.stream_port = options_.stream_port;
    o.name = options_.client_name;
    o.tcp_timeout = options_.tcp_timeout;
    o.handshake_timeout = options_.connect_timeout;
    return std::make_unique<Client>(o);
}

std::string KrpcSource::discover_server() { return discover(options_.context_port); }

void KrpcSource::run() {
    while (!stopping()) {
        std::string where = fmt::str("%s:%d", address().c_str(), options_.rpc_port);
        Snapshot connecting;
        connecting.detail = "connecting to kRPC at " + where;
        publish(connecting);
        std::unique_ptr<Client> conn;
        try {
            conn = connect();
        } catch (const std::exception& e) {  // refused (KSP closed), timeout, handshake rejected by the server
            log::warning(LOG, "kRPC at " + where + ": " + short_error(e));
            Snapshot s;
            s.detail = "kRPC at " + where + ": " + short_error(e);
            publish(s);
            // DHCP may have reassigned the PC. The game already answers scene requests on UDP 50002,
            // including broadcast requests from a new Pi address.
            std::string found = options_.discovery ? discover_server() : "";
            if (!found.empty() && found != address()) {
                log::info(LOG, "discovered KSP at " + found + " (previously " + address() + ")");
                std::lock_guard<std::mutex> lock(address_mutex_);
                options_.address = found;
                continue;
            }
            wait(options_.retry_seconds);
            continue;
        }
        log::info(LOG, "connected to kRPC at " + where);
        {
            std::lock_guard<std::mutex> lock(client_mutex_);
            active_ = conn.get();
        }
        if (stopping()) conn->close();
        try {
            session(*conn);
        } catch (const std::exception& e) {  // connection lost mid-session: start over
            log::warning(LOG, "kRPC session ended: " + short_error(e));
            Snapshot s;
            s.detail = "lost kRPC at " + where + ": " + short_error(e);
            publish(s);
        }
        {
            std::lock_guard<std::mutex> lock(client_mutex_);
            active_ = nullptr;
        }
        conn->close();
        conn.reset();
        wait(options_.retry_seconds);
    }
}

void KrpcSource::session(Client& conn) {
    ship_.reset();
    while (!stopping()) {
        uint64_t vessel = 0;
        std::string why = "no active vessel";
        try {
            vessel = object(conn, sc("get_ActiveVessel"));
        } catch (const RpcError& e) {  // not in the flight scene (some scenes raise)
            vessel = 0;
            why = short_error(e);
        }
        if (!vessel) {  // kRPC 0.7 at the Space Center / Tracking Station returns null instead of raising
            Snapshot s;
            s.state = State::NoVessel;
            Context ctx = read_context_reply(address(), options_.context_port);
            s.scene = ctx.scene;
            if (ctx.skin != context_skin_)
                log::info(LOG, "context skin: " + (ctx.skin.empty() ? std::string("none (the [display] setting)") : ctx.skin));
            context_skin_ = ctx.skin;
            context_at_ = monotonic();
            // The VAB shows the editor ship report when KSPSerialIO.Hmi.dll serves it; polled about twice a second.
            send_inputs(conn, 0, s.scene);
            const bool vab = s.scene == "vab";
            if (vab) s.ship = ship_.poll(conn, monotonic());
            else ship_.reset();
            s.detail = why;
            s.received = monotonic();
            publish(s);
            wait(vab ? 0.5 : 1.0);
            continue;
        }
        try {
            follow(conn, vessel);
        } catch (const RpcError& e) {  // the vessel was destroyed / the scene changed while we read it
            log::info(LOG, "vessel lost: " + short_error(e));
        }
    }
}

void KrpcSource::follow(Client& c, uint64_t vessel) {
    // Stream one vessel around one body; returns when either changes so the caller re-opens the streams.
    uint64_t orbit = object(c, sc("Vessel_get_Orbit", {obj(vessel)}));
    uint64_t body = object(c, sc("Orbit_get_Body", {obj(orbit)}));
    uint64_t body_frame = object(c, sc("CelestialBody_get_ReferenceFrame", {obj(body)}));
    uint64_t flight = object(c, sc("Vessel_Flight", {obj(vessel), obj(body_frame)}));  // surface-relative speeds
    std::string name = get_string(c, sc("Vessel_get_Name", {obj(vessel)}));
    double radius = get_double(c, sc("CelestialBody_get_EquatorialRadius", {obj(body)}));
    double atmosphere = get_bool(c, sc("CelestialBody_get_HasAtmosphere", {obj(body)}))
                            ? get_double(c, sc("CelestialBody_get_AtmosphereDepth", {obj(body)}))
                            : 0.0;
    std::string body_name = get_string(c, sc("CelestialBody_get_Name", {obj(body)}));
    uint64_t inertial = object(c, sc("CelestialBody_get_NonRotatingReferenceFrame", {obj(body)}));
    double mu = get_double(c, sc("CelestialBody_get_GravitationalParameter", {obj(body)}));
    std::optional<Axes> axes;

    const double rate = 1.0 / period_;
    Group streams(c);
    streams.add("ut", sc("get_UT"), rate);
    streams.add("name", sc("Vessel_get_Name", {obj(vessel)}), rate);  // streamed: a rename shows at once
    streams.add("situation", sc("Vessel_get_Situation", {obj(vessel)}), rate);
    streams.add("body", sc("Orbit_get_Body", {obj(orbit)}), rate);
    for (const char* f : {"MeanAltitude", "Speed", "VerticalSpeed"})
        streams.add(f, sc(std::string("Flight_get_") + f, {obj(flight)}), rate);
    for (const char* f : {"Speed", "ApoapsisAltitude", "PeriapsisAltitude", "TimeToApoapsis", "TimeToPeriapsis",
                          "Inclination", "Eccentricity", "Period", "SemiMajorAxis", "TrueAnomaly"})
        streams.add(std::string("orbit.") + f, sc(std::string("Orbit_get_") + f, {obj(orbit)}), rate);
    streams.add("position", sc("Vessel_Position", {obj(vessel), obj(inertial)}), rate);
    streams.add("velocity", sc("Vessel_Velocity", {obj(vessel), obj(inertial)}), rate);

    // Optional panels: any failure (RPC error, or a procedure missing in another kRPC version) disables that panel
    // only and never interrupts the orbital telemetry.
    std::unique_ptr<NavigationStreams> nav;
    std::unique_ptr<SystemsStreams> systems;
    std::unique_ptr<ManeuverStreams> maneuver;
    try {
        nav = std::make_unique<NavigationStreams>(c, vessel, rate);
    } catch (const RpcError& e) {
        log::warning(LOG, "navball unavailable: " + short_error(e));
    }
    try {
        maneuver = std::make_unique<ManeuverStreams>(c, vessel, body, inertial);
    } catch (const RpcError& e) {
        log::warning(LOG, "maneuver orbit unavailable: " + short_error(e));
    }
    try {
        systems = std::make_unique<SystemsStreams>(c, vessel);
    } catch (const RpcError& e) {
        log::warning(LOG, "resources/staging unavailable: " + short_error(e));
    }
    log::info(LOG, "following " + name + " around " + body_name);
    auto d = [&](const std::string& key) { return pb::dec_double(c.stream(streams.id(key)).data); };
    while (!stopping()) {
        // Also a liveness probe: throws when the socket is gone (0 = left flight).
        if (object(c, sc("get_ActiveVessel")) != vessel) return;
        send_inputs(c, vessel, "flight");
        if (stream_object(c, streams.id("body")) != body) return;  // sphere of influence change: new frames
        Snapshot s;
        s.state = State::Flight;
        s.ut = d("ut");
        s.vessel = pb::dec_string(c.stream(streams.id("name")).data);
        s.situation = enum_name(SITUATIONS, pb::dec_sint32(c.stream(streams.id("situation")).data));
        s.altitude = d("MeanAltitude");
        s.surface_speed = d("Speed");
        s.vertical_speed = d("VerticalSpeed");
        s.orbital_speed = d("orbit.Speed");
        s.apoapsis = d("orbit.ApoapsisAltitude");
        s.periapsis = d("orbit.PeriapsisAltitude");
        s.time_to_apoapsis = d("orbit.TimeToApoapsis");
        s.time_to_periapsis = d("orbit.TimeToPeriapsis");
        s.inclination = degrees_of(d("orbit.Inclination"));
        s.eccentricity = d("orbit.Eccentricity");
        s.period = d("orbit.Period");
        s.semi_major_axis = d("orbit.SemiMajorAxis");
        s.true_anomaly = d("orbit.TrueAnomaly");
        Vec3 position = vec3(c.stream(streams.id("position")).data);
        Vec3 velocity = vec3(c.stream(streams.id("velocity")).data);
        if (!axes) axes = view_axes(position, velocity);
        s.body = body_name;
        s.body_radius = radius;
        s.atmosphere_depth = atmosphere;
        if (nav) optional_step([&] { s.nav = nav->sample(); }, [&] { nav->invalidate(); });  // transient change
        // Optional resources/staging sample: a failure blanks that panel, never the session.
        if (systems)
            optional_step([&] { s.systems = systems->sample(); }, [&] { s.systems.reset(); });
        s.orbit_state = OrbitState{position, velocity, mu, *axes};
        // Optional dashed post-burn orbit: a failure only hides it (retried after 1 s).
        if (maneuver) optional_step([&] { s.maneuver_orbit = maneuver->sample(mu); }, [&] { maneuver->invalidate(); });
        s.received = monotonic();
        publish(std::move(s));
        wait(period_);
    }
}

// ================================================================================================ demo
KeplerState kepler_state(double a, double e, double mu, double mean_anomaly) {
    double m = std::fmod(mean_anomaly, 2 * PI);
    if (m < 0) m += 2 * PI;
    double ecc = e < 0.8 ? m : PI;
    for (int i = 0; i < 50; ++i) {  // Newton on Kepler's equation E - e sin E = M
        double step = (ecc - e * std::sin(ecc) - m) / (1 - e * std::cos(ecc));
        ecc -= step;
        if (std::fabs(step) < 1e-12) break;
    }
    double nu = 2 * std::atan2(std::sqrt(1 + e) * std::sin(ecc / 2), std::sqrt(1 - e) * std::cos(ecc / 2));
    double r = a * (1 - e * std::cos(ecc));
    double speed = std::sqrt(mu * (2 / r - 1 / a));
    double p = a * (1 - e * e);
    double radial = std::sqrt(mu / p) * e * std::sin(nu);
    nu = std::fmod(nu, 2 * PI);
    if (nu < 0) nu += 2 * PI;
    return {r, nu, speed, radial};
}

DemoSource::DemoSource(double speed, double rate_hz, double periapsis, double apoapsis)
    : Source("DemoSource"), speed_(speed), period_s_(1.0 / std::max(rate_hz, 0.5)) {
    double rp = KERBIN_RADIUS + periapsis, ra = KERBIN_RADIUS + apoapsis;
    a_ = (rp + ra) / 2;
    e_ = (ra - rp) / (ra + rp);
}

DemoSource::~DemoSource() { stop(); }

static double pymod(double a, double b) {
    double r = std::fmod(a, b);
    return r < 0 ? r + b : r;
}

ManeuverOrbit demo_maneuver(double a, double e, double mu, double anomaly, double prograde) {
    // A node fixed in inertial space (the demo orbit never changes): post-burn state at that true anomaly.
    double p = a * (1 - e * e), r = p / (1 + e * std::cos(anomaly)), k = std::sqrt(mu / p);
    double radial = k * e * std::sin(anomaly), tangent = k * (1 + e * std::cos(anomaly));
    Vec3 out{std::cos(anomaly), 0, std::sin(anomaly)}, along{-std::sin(anomaly), 0, std::cos(anomaly)};
    Vec3 velocity = out * radial + along * tangent;
    double speed = std::sqrt(dot(velocity, velocity));
    ManeuverOrbit m;
    m.position = out * r;
    m.velocity = velocity * ((speed + prograde) / speed);
    m.soi = KERBIN_SOI;
    return m;
}

Snapshot DemoSource::sample(double ut) const {
    double a = a_, e = e_, mu = KERBIN_MU;
    double n = std::sqrt(mu / (a * a * a));
    double m = pymod(n * ut, 2 * PI);
    KeplerState k = kepler_state(a, e, mu, m);
    double tangent = std::sqrt(std::max(0.0, k.speed * k.speed - k.radial_speed * k.radial_speed));
    Vec3 position{k.radius * std::cos(k.true_anomaly), 0, k.radius * std::sin(k.true_anomaly)};
    Vec3 velocity{k.radial_speed * std::cos(k.true_anomaly) - tangent * std::sin(k.true_anomaly), 0,
                  k.radial_speed * std::sin(k.true_anomaly) + tangent * std::cos(k.true_anomaly)};
    Snapshot s;
    s.state = State::Flight;
    s.vessel = "Demo Orbiter";
    s.situation = "orbiting";
    s.body = "Kerbin";
    s.body_radius = KERBIN_RADIUS;
    s.atmosphere_depth = KERBIN_ATMOSPHERE;
    s.ut = ut;
    s.altitude = k.radius - KERBIN_RADIUS;
    s.apoapsis = a * (1 + e) - KERBIN_RADIUS;
    s.periapsis = a * (1 - e) - KERBIN_RADIUS;
    s.time_to_apoapsis = pymod(PI - m, 2 * PI) / n;
    s.time_to_periapsis = pymod(2 * PI - m, 2 * PI) / n;
    s.inclination = 2.5;
    s.eccentricity = e;
    s.period = 2 * PI / n;
    s.semi_major_axis = a;
    s.true_anomaly = k.true_anomaly;
    s.orbital_speed = k.speed;
    s.surface_speed = std::max(k.speed - KERBIN_ROTATION_SPEED, 0.0);
    s.vertical_speed = k.radial_speed;
    Navigation nav;
    nav.rotation = attitude(14, 90, -25);
    nav.pitch = 14, nav.heading = 90, nav.roll = -25;
    nav.speed_mode = "orbit";
    nav.speed = k.speed;
    nav.prograde = unit({k.radial_speed, 0, k.speed});
    nav.normal = Vec3{0, -1, 0};
    nav.radial = Vec3{1, 0, 0};
    nav.target = unit({.1, .4, .9});
    nav.maneuver = unit({.12, .1, 1});
    nav.sas = true;
    nav.sas_mode = "prograde";
    nav.rcs = true;
    nav.throttle = .42;
    nav.g_force = .3;
    nav.node_dv = 320;
    nav.node_remaining = 114.4;
    nav.node_time = 579;
    nav.burn_seconds = 8;
    s.nav = nav;
    Systems sys;
    sys.current_stage = 2;
    sys.resources = {{"ElectricCharge", 148, 200}, {"LiquidFuel", 820, 1440}, {"Oxidizer", 1002, 1760},
                     {"MonoPropellant", 24, 30}};
    sys.stages = {{2, 7, 1430}, {1, 4, 820}, {0, 2, 0}};
    s.systems = sys;
    s.orbit_state = OrbitState{position, velocity, mu, Axes{{1, 0, 0}, {0, 0, 1}}};
    s.maneuver_orbit = demo_maneuver(a, e, mu);
    s.received = monotonic();
    return s;
}

void DemoSource::run() {
    double start = monotonic();
    while (!stopping()) {
        publish(sample((monotonic() - start) * speed_));
        wait(period_s_);
    }
}

}  // namespace ksphmi
