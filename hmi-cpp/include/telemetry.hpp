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

#include "krpc.hpp"
#include "model.hpp"

namespace ksphmi {

/// The board buttons the HMI keyboard can press (T = SAS, R = RCS), forwarded as if they came from the board.
enum class Button { Sas, Rcs };
/// "SAS" or "RCS": the KSPHMI.Press argument and the log name.
const char* button_name(Button b);

/// Send one press to the game the way KSPSerialIO applies a board press. In flight (vessel != 0) it toggles the
/// vessel's SAS / RCS (Control.SAS / Control.RCS, the board's edge toggle). In the VAB it calls KSPHMI.Press, which
/// feeds the same EditorNavigation as the board (SAS next screen, RCS previous). In any other scene nothing is
/// sent. Returns what happened, for the log. RpcError and ConnectionError propagate.
std::string send_press(krpc::Client& c, Button b, uint64_t vessel, const std::string& scene);

class Source {
public:
    virtual ~Source();
    void start();
    void stop();
    std::shared_ptr<const Snapshot> latest() const;
    const char* name() const { return name_; }
    /// A keyboard press from the display thread. Never blocks: a source with a game link queues it for its own
    /// thread, the others only log it.
    virtual void press(Button b);

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
    void press(Button b) override;

protected:
    void run() override;
    void publish(Snapshot snap) override;
    void on_stop() override;
    // Overridable for tests (reconnect / discovery without a server).
    virtual std::unique_ptr<krpc::Client> connect();
    virtual void session(krpc::Client& conn);
    virtual std::string discover_server();
    void follow(krpc::Client& conn, uint64_t vessel);
    /// Send the queued keyboard presses (telemetry thread). An RpcError is logged and drops that press only.
    void send_presses(krpc::Client& conn, uint64_t vessel, const std::string& scene);

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
    std::mutex press_mutex_;
    std::vector<Button> presses_;  ///< keyboard presses waiting for the telemetry thread
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
