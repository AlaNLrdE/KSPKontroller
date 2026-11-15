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

#include "krpc.hpp"
#include "model.hpp"

namespace ksphmi {

class Source {
public:
    virtual ~Source();
    void start();
    void stop();
    std::shared_ptr<const Snapshot> latest() const;
    const char* name() const { return name_; }

protected:
    explicit Source(const char* name);
    virtual void run() = 0;
    virtual void publish(Snapshot snap);
    virtual void on_stop() {}
    /// Sleep up to `seconds`; true when stop() was called.
    bool wait(double seconds);
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
    std::thread thread_;
};

/// First line of an error, at most 160 characters (telemetry.py _short).
std::string short_error(const std::exception& e);

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

protected:
    void run() override;
    void publish(Snapshot snap) override;
    void on_stop() override;
    // Overridable for tests (reconnect / discovery without a server).
    virtual std::unique_ptr<krpc::Client> connect();
    virtual void session(krpc::Client& conn);
    virtual std::string discover_server();
    void follow(krpc::Client& conn, uint64_t vessel);

    Options options_;
    double period_;

private:
    mutable std::mutex address_mutex_;
    std::mutex client_mutex_;
    krpc::Client* active_ = nullptr;
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
