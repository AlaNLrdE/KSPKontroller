// Flight data sources (port of telemetry.py, with NavigationStreams and SystemsStreams).
//
// Every source runs in its own thread and publishes immutable Snapshots; the display thread only calls latest(),
// so a slow or lost connection never blocks drawing.
//  * KrpcSource reads the active vessel from the kRPC mod running inside KSP.
//  * DemoSource flies a Kepler orbit around Kerbin, so the screens can be developed without the game.
#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "keyboard.hpp"
#include "krpc.hpp"
#include "model.hpp"

namespace ksphmi {

/// KSP's own Shift / Ctrl throttle rate: the whole range in two seconds (FlightInputHandler).
constexpr double KEY_THROTTLE_RATE = 0.5;

/// Send one keyboard command the way the game's own key acts. In flight (vessel != 0): SAS, RCS, gear, lights,
/// brakes, abort, staging, action groups and SAS modes through kRPC Control (one-shot changes, like the keys and
/// the board's edge toggles); throttle full / cut through KSPHMIFlight.SetThrottle (KSPSerialIO.Hmi.dll: a
/// one-shot change, so the PC's own Shift / Ctrl keep working); time warp and quicksave / quickload through
/// SpaceCenter. In the VAB only SAS / RCS, through KSPHMI.Press, the board's editor navigation (SAS next screen,
/// RCS previous). Any other scene ignores keys. Returns what happened, for the log. Errors propagate.
/// `control`: the vessel's cached Control object (0 = look it up).
std::string send_command(krpc::Client& c, const Command& cmd, uint64_t vessel, const std::string& scene,
                         uint64_t control = 0);

/// The held keys, in flight: KSPHMIFlight.SetAxes with the six axes when `axes` (every tick while one is held, then
/// once with zeros; the plugin drops values older than 0.5 s and the board's joystick wins over them) and
/// KSPHMIFlight.NudgeThrottle(direction * KEY_THROTTLE_RATE * dt) while Shift / Ctrl are held.
void send_held(krpc::Client& c, const Held& h, double throttle_dt, bool axes);

class Source {
public:
    virtual ~Source();
    void start();
    void stop();
    std::shared_ptr<const Snapshot> latest() const;
    const char* name() const { return name_; }
    /// A keyboard command from the display thread. Never blocks: a source with a game link queues it for its own
    /// thread, the others only log it.
    virtual void command(const Command& c);
    /// The held keys now (display thread, on every change). Ignored without a game link.
    virtual void hold(const Held&) {}
    /// Whether the cockpit panel scene is on screen (display thread): its extra kRPC streams exist only then.
    virtual void set_cockpit(bool) {}
    /// Whether the MechJeb scene is on screen (display thread): MechJeb is polled only then.
    virtual void set_mechjeb(bool) {}

protected:
    explicit Source(const char* name);
    virtual void run() = 0;
    virtual void publish(Snapshot snap);
    virtual void on_stop() {}
    /// Sleep up to `seconds` or until wake(); true when stop() was called.
    bool wait(double seconds);
    /// End the current wait() early (a queued keyboard press), without stopping.
    void wake();
    bool stopping() const { return stop_; }
    /// Ask the thread to finish without joining it (callable from the thread itself).
    void request_stop();

private:
    void run_guarded();
    const char* name_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::shared_ptr<const Snapshot> latest_;
    std::atomic<bool> stop_{false};
    bool wake_ = false;  ///< guarded by mutex_
    std::thread thread_;
};

/// First line of an error, at most 160 characters (telemetry.py _short).
std::string short_error(const std::exception& e);

/// The VAB ship report over kRPC: KSPHMI.Revision() on every poll (about twice a second), KSPHMI.ShipReport()
/// only when the revision changes. A server error (no KSPHMI service: KSPSerialIO.Hmi.dll or kRPC missing, wrong
/// scene) or a report that does not parse clears the report, so the display keeps its static cards; a server error
/// is retried after `retry_seconds`. Link errors (krpc::ConnectionError) propagate: the session reconnects as usual.
class ShipReportPoller {
public:
    explicit ShipReportPoller(double retry_seconds = 5.0) : retry_seconds_(retry_seconds) {}
    std::shared_ptr<const ShipReport> poll(krpc::Client& c, double now);
    /// Forget the report and the revision (scene left, new connection).
    void reset();
    /// Why there is no report ("" while there is one or before the first poll).
    const std::string& problem() const { return problem_; }
    int fetches = 0;  ///< ShipReport calls made (tests)

private:
    void fail(const std::string& why, bool log_warning);
    double retry_seconds_;
    std::shared_ptr<const ShipReport> report_;
    std::optional<uint32_t> revision_;
    double retry_at_ = -1e9;
    std::string problem_;
};

/// The VESSEL panel over kRPC: KSPHMIFlight.VesselState() every `period` seconds, VesselShape() only when the
/// state's revision is not the shape held (new vessel, staging, docking). A server error (no KSPHMIFlight service:
/// KSPSerialIO.Hmi.dll older than 0.19.7.0 or kRPC missing) or text that does not parse clears the view and is
/// retried after `retry_seconds`, logged once per reason. Link errors (krpc::ConnectionError) propagate.
class VesselViewPoller {
public:
    explicit VesselViewPoller(double period = 0.5, double retry_seconds = 5.0)
        : period_(period), retry_seconds_(retry_seconds) {}
    std::shared_ptr<const VesselView> poll(krpc::Client& c, double now);
    /// Forget the view (new vessel session, new connection).
    void reset();
    int shape_fetches = 0;  ///< VesselShape calls made (tests)

private:
    void fail(const std::string& why, double now);
    double period_, retry_seconds_;
    std::optional<VesselView> shape_;
    std::shared_ptr<const VesselView> view_;
    double next_ = -1e9;
    std::string problem_;
};

/// The MechJeb scene over kRPC: KSPHMIFlight.MechJebRevision() every `period` seconds, MechJebReport() only when
/// the revision changes. A server error (no such procedure: KSPSerialIO.Hmi.dll without MechJeb support, scene
/// changing) or a report that does not parse clears the report (the scene says "no data") and is retried after
/// `retry_seconds`, logged once per reason. Link errors (krpc::ConnectionError) propagate.
class MechJebPoller {
public:
    explicit MechJebPoller(double period = 0.5, double retry_seconds = 5.0)
        : period_(period), retry_seconds_(retry_seconds) {}
    std::shared_ptr<const MechJebReport> poll(krpc::Client& c, double now);
    /// Forget the report (scene hidden, new vessel session, new connection).
    void reset();
    /// Why there is no report ("" while there is one or before the first poll).
    const std::string& problem() const { return problem_; }
    int fetches = 0;  ///< MechJebReport calls made (tests)

private:
    void fail(const std::string& why, double now, bool retry);
    double period_, retry_seconds_;
    std::shared_ptr<const MechJebReport> report_;
    std::optional<uint32_t> revision_;
    double next_ = -1e9;
    std::string problem_;
};

/// KSPHMIFlight.MechJebSet(key, text) or MechJebAction(key) for a MechJeb command: the plugin's reply ("ok",
/// "unknown key", "read only", "invalid value", "unavailable"). Errors propagate.
std::string send_mechjeb(krpc::Client& c, const Command& cmd);

class KrpcSource : public Source {
public:
    struct Options {
        std::string address = "localhost";
        int rpc_port = 50000, stream_port = 50001;
        double rate_hz = 10, retry_seconds = 3;
        std::string client_name = "KSP HMI";
        bool discovery = true;
        double tcp_timeout = 5;      ///< reaching the port (a firewall on a Public network drops rather than refuses)
        double connect_timeout = 20;  ///< the whole handshake, longer than tcp_timeout so the two causes stay apart
        int context_port = 50002;
    };
    explicit KrpcSource(Options options);
    ~KrpcSource() override;
    std::string address() const;
    void command(const Command& c) override;
    void hold(const Held& h) override;
    void set_cockpit(bool shown) override;
    void set_mechjeb(bool shown) override;

protected:
    void run() override;
    void publish(Snapshot snap) override;
    void on_stop() override;
    // Overridable for tests (reconnect / discovery without a server).
    virtual std::unique_ptr<krpc::Client> connect();
    virtual void session(krpc::Client& conn);
    virtual std::string discover_server();
    void follow(krpc::Client& conn, uint64_t vessel);
    /// Send the queued keyboard commands and the held keys (telemetry thread). An RpcError is logged and drops
    /// that command only; a failing held-key call is logged once per distinct error.
    void send_inputs(krpc::Client& conn, uint64_t vessel, const std::string& scene);
    /// In flight, a command thread sends each queued command as soon as it arrives, on the same connection (one
    /// RPC at a time), so a click never waits for the rest of a telemetry iteration. Returns when `stop` is set.
    void command_loop(krpc::Client& conn, uint64_t vessel, uint64_t control, const std::atomic<bool>& stop);
    /// Send one command and log what happened, with its time in the queue and in kRPC.
    void send_one(krpc::Client& conn, const Command& cmd, uint64_t vessel, const std::string& scene, uint64_t control);
    /// A MechJeb command: logged with its reply and timing ("mechjeb: ... -> ok (queued 2 ms, kRPC 48 ms)"); the
    /// reply is published with the next snapshot (Snapshot::mechjeb_reply).
    void send_mechjeb_one(krpc::Client& conn, const Command& cmd);

    Options options_;
    double period_;

private:
    mutable std::mutex address_mutex_;
    std::mutex client_mutex_;
    krpc::Client* active_ = nullptr;
    /// Skin field of the last context reply, refreshed about once a second in every state (telemetry thread).
    std::string context_skin_;
    double context_at_ = -1e9;
    ShipReportPoller ship_;
    VesselViewPoller vessel_view_;
    std::mutex input_mutex_;
    std::vector<Command> commands_;  ///< keyboard commands waiting for the telemetry thread (input_mutex_)
    std::condition_variable input_cv_;  ///< a command was queued, or the command thread must stop
    bool sender_active_ = false;        ///< in flight the command thread sends commands_ (input_mutex_)
    Held held_;                      ///< held keys as the display thread last saw them (input_mutex_)
    std::atomic<bool> cockpit_{false};  ///< the cockpit panel scene is shown: follow() keeps CockpitStreams
    std::atomic<bool> mechjeb_{false};  ///< the MechJeb scene is shown: follow() polls MechJeb
    MechJebPoller mechjeb_poller_;
    std::optional<MechJebReply> mechjeb_reply_;  ///< the plugin's last MechJeb reply (input_mutex_)
    Held sent_;                      ///< held keys last sent (telemetry thread)
    double nudged_at_ = -1;          ///< last NudgeThrottle, -1 while no throttle key is held
    std::string held_problem_;       ///< last held-key error logged
};

constexpr double KERBIN_MU = 3.5316e12;
constexpr double KERBIN_RADIUS = 600000.0;
constexpr double KERBIN_ATMOSPHERE = 70000.0;
constexpr double KERBIN_ROTATION_SPEED = 174.94;  // equatorial surface speed, m/s

struct KeplerState {
    double radius, true_anomaly, speed, radial_speed;
};

/// Elliptic orbit (0 <= e < 1) at a mean anomaly.
KeplerState kepler_state(double a, double e, double mu, double mean_anomaly);

constexpr double KERBIN_SOI = 84159286.0;

/// The demo's maneuver node: a prograde burn of `prograde` m/s at true anomaly `anomaly` (radians) of the orbit
/// (a, e), in the demo's frame (orbit in the x/z plane).
ManeuverOrbit demo_maneuver(double a, double e, double mu, double anomaly = 2.3, double prograde = 150);

/// A ship on an 80 x 250 km Kerbin orbit, time accelerated by `speed`. No game or network needed.
class DemoSource : public Source {
public:
    explicit DemoSource(double speed = 20.0, double rate_hz = 10.0, double periapsis = 80000.0,
                        double apoapsis = 250000.0);
    ~DemoSource() override;
    Snapshot sample(double ut) const;

protected:
    void run() override;

private:
    double speed_, period_s_, a_, e_;
};

}  // namespace ksphmi
