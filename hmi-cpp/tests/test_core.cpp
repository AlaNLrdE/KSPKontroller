// Pure-logic tests (formatting, attitude, orbit geometry, resources, scenes,
// discovery, config, kRPC client, reconnect). No display, no game.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <map>
#include <mutex>
#include <cstring>
#include <sstream>
#include <fstream>
#include <thread>

#include "check.hpp"
#include "config.hpp"
#include "fake_krpc.hpp"
#include "fmt.hpp"
#include "gauges.hpp"
#include "krpc.hpp"
#include "log.hpp"
#include "model.hpp"
#include "navmath.hpp"
#include "netinfo.hpp"
#include "pb.hpp"
#include "scenes.hpp"
#include "telemetry.hpp"

using namespace ksphmi;

// ------------------------------------------------------------------------------------------- fmt
TEST(format_distance) {
    CHECK_EQ(fmt::distance(950), "950 m");
    CHECK_EQ(fmt::distance(80000), "80.0 km");
    CHECK_EQ(fmt::distance(-12400), "-12.4 km");
    CHECK_EQ(fmt::distance(12400000), "12.40 Mm");
    CHECK_EQ(fmt::distance(NAN), "--");
    CHECK_EQ(fmt::distance(-0.3), "0 m");  // no "-0 m" flicker on the pad
    CHECK_EQ(fmt::distance(-40000040), "-40.00 Mm");
    CHECK_EQ(fmt::speed(-0.04), "0.0 m/s");
    CHECK_EQ(fmt::distance(2e10), "20.00 Gm");
    CHECK_EQ(fmt::speed(12345), "12.35 km/s");
}

TEST(format_duration_uses_kerbin_days) {
    CHECK_EQ(fmt::duration(45), "45s");
    CHECK_EQ(fmt::duration(723), "12m 03s");
    CHECK_EQ(fmt::duration(7500), "2h 05m");
    CHECK_EQ(fmt::duration(6 * 3600 * 3 + 3600 * 4), "3d 4h");
    CHECK_EQ(fmt::duration(25 * 3600, 24), "1d 1h");
    CHECK_EQ(fmt::duration(INFINITY), "--");
    CHECK_EQ(fmt::duration(-90), "-1m 30s");
    CHECK_EQ(fmt::duration(2.5), "2s");  // Python round(): half to even
}

TEST(format_clock_and_situation) {
    CHECK_EQ(fmt::clock(0), "Y1 D001 00:00:00");
    CHECK_EQ(fmt::clock(6 * 3600 * 426 + 6 * 3600 + 61), "Y2 D002 00:01:01");
    CHECK_EQ(fmt::clock(-1), "--");
    CHECK_EQ(fmt::situation("sub_orbital"), "Sub-orbital");
    CHECK_EQ(fmt::situation("orbiting"), "Orbiting");
    CHECK_EQ(fmt::situation("pre_launch"), "Pre-launch");
    CHECK_EQ(fmt::degrees(2.5), "2.50\xC2\xB0");
}

// ------------------------------------------------------------------------------------------- Kepler / demo
TEST(kepler_circular_orbit) {
    double a = KERBIN_RADIUS + 100000;
    auto k = kepler_state(a, 0.0, KERBIN_MU, 1.0);
    CHECK_NEAR(k.radius, a, 1e-6);
    CHECK_NEAR(k.true_anomaly, 1.0, 1e-7);
    CHECK_NEAR(k.speed, std::sqrt(KERBIN_MU / a), 1e-7);
    CHECK_NEAR(k.radial_speed, 0.0, 1e-7);
}

TEST(kepler_apsides_and_high_eccentricity) {
    double a = 765000, e = 0.11;
    auto pe = kepler_state(a, e, KERBIN_MU, 0.0), ap = kepler_state(a, e, KERBIN_MU, PI);
    CHECK_NEAR(pe.radius, a * (1 - e), 1e-3);
    CHECK_NEAR(ap.radius, a * (1 + e), 1e-3);
    CHECK_NEAR(ap.true_anomaly, PI, 1e-9);
    CHECK(pe.speed > ap.speed);
    for (double m : {0.01, 1.0, 3.0, 6.0}) {
        double r = kepler_state(10e6, 0.97, KERBIN_MU, m).radius;
        CHECK(10e6 * 0.03 - 1 <= r && r <= 10e6 * 1.97 + 1);
    }
}

TEST(demo_sample_is_consistent) {
    DemoSource src;
    Snapshot s = src.sample(0.0);  // at periapsis
    CHECK(s.state == State::Flight);
    CHECK_NEAR(s.periapsis, 80000, 1e-3);
    CHECK_NEAR(s.apoapsis, 250000, 1e-3);
    CHECK_NEAR(s.altitude, s.periapsis, 1e-3);
    CHECK_NEAR(s.time_to_apoapsis, s.period / 2, 1e-3);
    Snapshot later = src.sample(s.period / 4);
    CHECK(later.vertical_speed > 0);  // climbing towards apoapsis
    CHECK(later.time_to_apoapsis < s.time_to_apoapsis);
    CHECK(s.nav && s.systems && s.orbit_state);
}

TEST(stale_only_in_flight) {
    double now = monotonic();
    Snapshot f;
    f.state = State::Flight;
    f.received = now;
    CHECK(!stale(f, now + 1));
    CHECK(stale(f, now + 5));
    CHECK(!stale(Snapshot{}, now + 100));  // not in flight: nothing to go stale
}

// ------------------------------------------------------------------------------------------- attitude
static void check_vector(const Vec3& a, const Vec3& b) {
    CHECK_NEAR(a.x, b.x, 1e-6);
    CHECK_NEAR(a.y, b.y, 1e-6);
    CHECK_NEAR(a.z, b.z, 1e-6);
}

TEST(attitude_compass_pitch_roll_and_poles) {
    check_vector(basis(attitude(0, 0, 0)).forward, {0, 1, 0});
    check_vector(basis(attitude(0, 90, 0)).forward, {0, 0, 1});
    check_vector(basis(attitude(90, 45, 120)).forward, {1, 0, 0});
    check_vector(basis(attitude(-90, 45, -120)).forward, {-1, 0, 0});
    check_vector(basis(attitude(0, 90, 90)).right, {-1, 0, 0});
    for (double pitch : {-90, -45, 0, 45, 90})
        for (double roll : {-180, -90, 0, 90, 180}) {
            Basis b = basis(attitude(pitch, 125, roll));
            CHECK_NEAR(dot(b.right, b.forward), 0, 1e-7);
            CHECK_NEAR(dot(b.up, b.forward), 0, 1e-7);
            for (const Vec3& axis : {b.right, b.forward, b.up}) CHECK_NEAR(dot(axis, axis), 1, 1e-7);
        }
}

TEST(krpc_rotation_axes_match_observed_flight_direction) {
    // Paired read-only Flight.rotation / Flight.direction sample from real kRPC.
    Quat q{-.2744391398, -.6535272784, -.6527750457, -.2673387262};
    check_vector(basis(q).forward, {.0096828239, -.0028641836, .9999490184});
}

TEST(invalid_vectors_and_burn_estimate) {
    CHECK(!unit({0, 0, 0}));
    CHECK(!unit({NAN, 0, 0}));
    bool threw = false;
    try {
        basis(Quat{0, 0, 0, 0});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(!burn_time(100, 1000, 0, 300));
    CHECK(!burn_time(INFINITY, 1000, 100, 300));
    CHECK_NEAR(*burn_time(.01, 1000, 100, 300), .1, 1e-5);  // small dv: dv / acceleration
    CHECK(*burn_time(1000, 1000, 100, 300) < 10000);
}

TEST(navball_labels_visible_during_roll_and_fade_smoothly_at_limb) {
    for (int roll = -180; roll <= 180; ++roll) {
        auto p = label_projection(5, 90, basis(attitude(0, 90, roll)));
        CHECK(p.has_value());
        if (p) CHECK_EQ(p->alpha, 255);
    }
    std::vector<int> alphas;
    for (int step = 0; step <= 360; ++step) {
        auto p = label_projection(5, 0, basis(attitude(0, step / 4.0, 0)));
        alphas.push_back(p ? p->alpha : 0);
    }
    int partial = 0;
    for (int a : alphas) partial += 0 < a && a < 255;
    CHECK(partial > 20);
    for (size_t i = 1; i < alphas.size(); ++i) CHECK(0 <= alphas[i - 1] - alphas[i] && alphas[i - 1] - alphas[i] <= 9);
}

TEST(front_polygon_clips_to_visible_hemisphere) {
    auto out = front_polygon({{0, 0, 1}, {1, 0, -1}, {0, 1, 1}});
    CHECK_EQ(out.size(), size_t(4));
    for (const Vec3& v : out) CHECK(v.z >= -1e-12);
    CHECK(front_polygon({{0, 0, -1}, {1, 0, -1}, {0, 1, -1}}).empty());
}

// ------------------------------------------------------------------------------------------- orbit view
TEST(rcs_can_flip_periapsis_without_moving_the_vessel) {
    double mu = 3.5316e12, radius = 700000, speed = std::sqrt(mu / radius);
    Axes axes = view_axes({radius, 0, 0}, {0, 0, speed});
    OrbitState before{{radius, 0, 0}, {0, 0, speed + .05}, mu, axes};
    OrbitState after = before;
    after.velocity = {0, 0, speed - .05};
    CHECK(dot(before.conic()->peri_direction, after.conic()->peri_direction) < -.99);
    CHECK(before.vessel_point() == after.vessel_point());
    CHECK_NEAR(before.vessel_point().first, radius, 1e-9);
    CHECK_NEAR(before.vessel_point().second, 0, 1e-9);
}

TEST(circular_and_inclined_orbits_have_stable_axes) {
    double mu = 3.5316e12, radius = 700000, v = std::sqrt(mu / radius);
    for (const Vec3& velocity : {Vec3{0, 0, v}, Vec3{0, v, 0}}) {
        OrbitState state{{radius, 0, 0}, velocity, mu, view_axes({radius, 0, 0}, velocity)};
        auto c = state.conic();
        CHECK(c.has_value());
        CHECK_NEAR(c->eccentricity, 0, 1e-7);
        CHECK_NEAR(c->periapsis(), radius, 1e-3);
        CHECK_NEAR(*c->apoapsis(), radius, 1e-3);
        CHECK_NEAR(state.vessel_point().first, radius, 1e-9);
        CHECK_NEAR(state.vessel_point().second, 0, 1e-9);
    }
    OrbitState hyper{{280000, 0, 0}, {0, 0, std::sqrt(6.5e10 * 2.5 / 280000)}, 6.5e10, Axes{}};
    CHECK(!hyper.conic()->apoapsis());
    CHECK(!hyper.conic()->position(PI));  // beyond the asymptote
}

// ------------------------------------------------------------------------------------------- resources
TEST(resource_empty_missing_and_unknown_are_distinct) {
    CHECK((Resource{"ElectricCharge", 0.0, 100.0}.fraction()) == std::optional<double>(0));
    CHECK(!(Resource{"ElectricCharge", 0.0, 0.0}.fraction()));
    CHECK(!(Resource{"ElectricCharge", std::nullopt, std::nullopt}.fraction()));
    CHECK(!(Resource{"ElectricCharge", NAN, 100.0}.fraction()));
    CHECK((Resource{"ElectricCharge", 125.0, 100.0}.fraction()) == std::optional<double>(1));
    CHECK((Resource{"ElectricCharge", -1.0, 100.0}.fraction()) == std::optional<double>(0));
}

// ------------------------------------------------------------------------------------------- scenes
TEST(scene_context_accepts_only_versioned_known_scenes) {
    for (const auto& v : views()) CHECK_EQ(parse_context(std::string("KSPHMI/1 ") + v.key), std::string(v.key));
    for (const char* reply : {"", "KSPHMI/2 vab", "KSPHMI/1 unknown", "\xff", "KSPHMI/1"})
        CHECK_EQ(parse_context(reply), "");
    CHECK_EQ(views().size(), size_t(8));
    CHECK(find_view("vab") && !find_view("flight"));
}

TEST(discovery_accepts_only_local_peers) {
    for (const char* a : {"127.0.0.1", "192.168.100.10", "10.0.0.5", "172.16.1.1", "169.254.3.4"}) CHECK(is_local(a));
    for (const char* a : {"8.8.8.8", "1.1.1.1", "not-an-ip", "192.168.1", "192.168.001.1", "1.2.3.4.5", "256.1.1.1"})
        CHECK(!is_local(a));
}

TEST(discovery_uses_scene_protocol_and_ignores_unrelated_replies) {
    int server = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(server, reinterpret_cast<sockaddr*>(&a), sizeof a);
    socklen_t len = sizeof a;
    getsockname(server, reinterpret_cast<sockaddr*>(&a), &len);
    int port = ntohs(a.sin_port);
    std::string request;
    std::thread responder([&] {
        char buf[256];
        sockaddr_in peer{};
        socklen_t plen = sizeof peer;
        ssize_t n = recvfrom(server, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (n > 0) request.assign(buf, static_cast<size_t>(n));
        for (const char* reply : {"unrelated packet", "KSPHMI/1 mainmenu"})
            sendto(server, reply, std::strlen(reply), 0, reinterpret_cast<sockaddr*>(&peer), plen);
    });
    CHECK_EQ(discover(port, 0.5, {"127.0.0.1"}), "127.0.0.1");
    responder.join();
    CHECK_EQ(request, "KSPHMI/1");
    CHECK_EQ(discover(port, 0.02, {"127.0.0.1"}), "");
    close(server);
}

TEST(read_context_is_bounded_without_a_server) {
    double t0 = monotonic();
    CHECK_EQ(read_context("127.0.0.1", 9), "");
    CHECK(monotonic() - t0 < 1.0);
}

// ------------------------------------------------------------------------------------------- netinfo
TEST(netinfo_line_names_this_machine) {
    NetInfo info("127.0.0.1");
    std::string line = info.line();
    CHECK(line.rfind("This display: ", 0) == 0);
    CHECK(line.find(info.hostname) != std::string::npos);
    CHECK_EQ(route_address("127.0.0.1"), "127.0.0.1");
    CHECK_EQ(route_address("not a host name"), "");
    CHECK(info.primary.rfind("127.", 0) != 0);  // loopback never shown as the display's address
}

// ------------------------------------------------------------------------------------------- config
TEST(config_layers_and_environment) {
    char dir[] = "/tmp/ksphmi-test-XXXXXX";
    CHECK(mkdtemp(dir) != nullptr);
    std::string app = std::string(dir), local = app + "/local.toml";
    std::ofstream(app + "/config.toml") << "source = \"krpc\"  # comment\n[krpc]\naddress = 'pc'\nrate_hz = 10\n"
                                           "[display]\nfps = 10\nfullscreen = true\n[format]\nday_hours = 6\n";
    std::ofstream(local) << "[display]\nfps = 5\n";
    setenv("KSPHMI_APP_DIR", app.c_str(), 1);
    setenv("KSPHMI_LOCAL_CONFIG", local.c_str(), 1);
    setenv("KSPHMI_KRPC_ADDRESS", "10.0.0.9", 1);
    Config cfg = load_config();
    unsetenv("KSPHMI_APP_DIR");
    unsetenv("KSPHMI_LOCAL_CONFIG");
    unsetenv("KSPHMI_KRPC_ADDRESS");
    CHECK_EQ(cfg.display.fps, 5);
    CHECK(cfg.display.fullscreen);  // untouched keys keep config.toml / defaults
    CHECK_EQ(cfg.krpc.address, "10.0.0.9");
    CHECK_EQ(cfg.krpc.rpc_port, 50000);
    CHECK_EQ(cfg.loaded.size(), size_t(2));
    Config only = load_config(local);  // --config: that file only
    CHECK_EQ(only.display.fps, 5);
    CHECK_EQ(only.krpc.address, "localhost");
    std::remove((app + "/config.toml").c_str());
    std::remove(local.c_str());
    rmdir(dir);
}

TEST(toml_subset_and_errors) {
    auto v = parse_toml("a = 1_000\n[t]\nb = \"x # y\"  # c\nc = false\nd = -2.5\n");
    CHECK_EQ(v["a"], "1000");
    CHECK_EQ(v["t.b"], "x # y");
    CHECK_EQ(v["t.c"], "false");
    CHECK_EQ(v["t.d"], "-2.5");
    for (const char* bad : {"a = [1, 2]\n", "[[x]]\n", "a = \"open\n", "= 3\n", "a = nope\n"}) {
        bool threw = false;
        try {
            parse_toml(bad);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
    }
    // the real settings file parses
    Config cfg;
    std::ifstream in(std::string(KSPHMI_SOURCE_DIR) + "/config.toml");
    std::stringstream buf;
    buf << in.rdbuf();
    apply_toml(cfg, parse_toml(buf.str()));
    CHECK_EQ(cfg.krpc.rpc_port, 50000);
    CHECK_EQ(cfg.display.output, "sdl");
}

// ------------------------------------------------------------------------------------------- protobuf / kRPC
TEST(protobuf_values_round_trip) {
    CHECK_EQ(pb::varint(300), std::string("\xAC\x02"));
    CHECK_EQ(pb::enc_sint32(-1), std::string("\x01"));
    CHECK_EQ(pb::enc_sint32(5), std::string("\x0A"));
    for (int v : {0, 1, -1, 7, -300, 2147483647, -2147483647 - 1}) CHECK_EQ(pb::dec_sint32(pb::enc_sint32(v)), v);
    CHECK_EQ(pb::dec_double(pb::enc_double(-1.25)), -1.25);
    CHECK_EQ(pb::dec_float(pb::enc_float(3.5f)), 3.5f);
    CHECK_EQ(pb::dec_string(pb::enc_string("Kerbin \xC2\xB7")), "Kerbin \xC2\xB7");
    CHECK_EQ(pb::dec_object(pb::enc_object(123456789012ULL)), 123456789012ULL);
    auto t = pb::dec_tuple(pb::enc_tuple({1, -2, 3.5}));
    CHECK(t == (std::vector<double>{1, -2, 3.5}));
    bool threw = false;
    try {
        pb::dec_double("abc");
    } catch (const pb::DecodeError&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(krpc_client_calls_errors_nulls_and_streams) {
    FakeServer server([](const std::string& proc, const std::vector<std::string>& args) -> FakeResult {
        if (proc == "get_UT") return {pb::enc_double(1234.5)};
        if (proc == "get_ActiveVessel") return {"", true};
        if (proc == "Vessel_get_Name") return {pb::enc_string("Probe " + std::to_string(pb::dec_object(args.at(0))))};
        return {"", false, "no such procedure " + proc};
    });
    krpc::Client::Options o;
    o.address = "127.0.0.1";
    o.rpc_port = server.rpc_port;
    o.stream_port = server.stream_port;
    krpc::Client c(o);
    CHECK_EQ(pb::dec_double(c.invoke(krpc::sc("get_UT")).data), 1234.5);
    CHECK(c.invoke(krpc::sc("get_ActiveVessel")).null);
    CHECK_EQ(pb::dec_string(c.invoke(krpc::sc("Vessel_get_Name", {pb::enc_object(7)})).data), "Probe 7");
    bool rpc_error = false;
    try {
        c.invoke(krpc::sc("Nope"));
    } catch (const krpc::RpcError& e) {
        rpc_error = std::string(e.what()) == "no such procedure Nope";
    }
    CHECK(rpc_error);
    uint64_t ut = c.add_stream(krpc::sc("get_UT"), 10);
    uint64_t bad = c.add_stream(krpc::sc("Nope"), 10);
    CHECK_EQ(pb::dec_double(c.stream(ut).data), 1234.5);
    rpc_error = false;
    try {
        c.stream(bad);
    } catch (const krpc::RpcError&) {
        rpc_error = true;
    }
    CHECK(rpc_error);
    c.remove_stream(ut);
}

namespace {
// Accepts connections and never answers: like kRPC with maxTimePerUpdate = 0.
struct SilentServer {
    SilentServer() {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
        listen(fd, 8);
        socklen_t len = sizeof a;
        getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
    }
    ~SilentServer() { close(fd); }
    int fd, port;
};
}  // namespace

TEST(a_server_that_never_answers_does_not_hang_the_hmi) {
    SilentServer srv;
    KrpcSource::Options o;
    o.address = "127.0.0.1";
    o.rpc_port = o.stream_port = srv.port;
    o.retry_seconds = 0.1;
    o.discovery = false;
    o.tcp_timeout = 0.3;
    o.connect_timeout = 0.5;
    KrpcSource src(o);
    src.start();
    double end = monotonic() + 5;
    while (monotonic() < end && src.latest()->detail.find("no answer to the handshake") == std::string::npos)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(src.latest()->state == State::Connecting);
    CHECK(src.latest()->detail.find("no answer to the handshake") != std::string::npos);
    src.stop();
}

namespace {
class ReconnectProbe : public KrpcSource {
public:
    using KrpcSource::KrpcSource;
    int connects = 0, discoveries = 0, sessions = 0;
    void go() { run(); }

protected:
    std::unique_ptr<krpc::Client> connect() override {
        if (++connects == 1) throw krpc::ConnectionError("address changed");
        return KrpcSource::connect();
    }
    std::string discover_server() override {
        ++discoveries;
        return "127.0.0.1";
    }
    void session(krpc::Client&) override {
        ++sessions;
        request_stop();
    }
};
}  // namespace

TEST(reconnect_discovers_server_after_configured_address_fails) {
    FakeServer server([](const std::string&, const std::vector<std::string>&) { return FakeResult{"", true}; });
    KrpcSource::Options o;
    o.address = "old-server.invalid";
    o.rpc_port = server.rpc_port;
    o.stream_port = server.stream_port;
    o.retry_seconds = 0.05;
    ReconnectProbe probe(o);
    probe.go();  // on this thread: returns once the session asked to stop
    CHECK_EQ(probe.address(), "127.0.0.1");
    CHECK_EQ(probe.latest()->server, "kRPC 127.0.0.1:" + std::to_string(server.rpc_port));
    CHECK_EQ(probe.discoveries, 1);
    CHECK_EQ(probe.connects, 2);
    CHECK_EQ(probe.sessions, 1);
}

namespace {
// A vessel stationary on the launch pad: kRPC answers InvalidOperationException for the surface speed frame
// ("singular when surface velocity is near zero") and for stage delta-v ("not available").
struct PadServer {
    std::mutex mutex;
    std::map<std::string, int> counts;
    int count(const std::string& proc) {
        std::lock_guard<std::mutex> lock(mutex);
        return counts[proc];
    }
    static std::string list(std::initializer_list<uint64_t> ids) {
        pb::Writer w;
        for (uint64_t id : ids) w.field_bytes(1, pb::enc_object(id));
        return w.out;
    }
    FakeResult answer(const std::string& proc) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++counts[proc];
        }
        static const std::map<std::string, uint64_t> objects = {
            {"get_ActiveVessel", 1}, {"Vessel_get_Orbit", 2}, {"Orbit_get_Body", 3},
            {"CelestialBody_get_ReferenceFrame", 4}, {"Vessel_get_SurfaceReferenceFrame", 4}, {"Vessel_Flight", 5},
            {"Vessel_get_Control", 6}, {"Vessel_get_Parts", 7}, {"Vessel_get_Resources", 8},
            {"CelestialBody_get_NonRotatingReferenceFrame", 11}};
        if (auto it = objects.find(proc); it != objects.end()) return {pb::enc_object(it->second)};
        if (proc == "Vessel_get_SurfaceSpeedReferenceFrame")
            return {"", false, "VesselSurfaceSpeed reference frame is singular when surface velocity is near zero"};
        if (proc == "Stage_get_DeltaV") return {"", false, "Delta-v information is not available for activation stage 1."};
        if (proc.rfind("get_Target", 0) == 0) return {"", true};
        if (proc == "Vessel_get_Name") return {pb::enc_string("Pad")};
        if (proc == "CelestialBody_get_Name") return {pb::enc_string("Kerbin")};
        if (proc == "CelestialBody_get_HasAtmosphere") return {pb::enc_bool(true)};
        if (proc == "CelestialBody_get_EquatorialRadius") return {pb::enc_double(KERBIN_RADIUS)};
        if (proc == "CelestialBody_get_AtmosphereDepth") return {pb::enc_double(KERBIN_ATMOSPHERE)};
        if (proc == "CelestialBody_get_GravitationalParameter") return {pb::enc_double(KERBIN_MU)};
        if (proc == "Vessel_Position") return {pb::enc_tuple({KERBIN_RADIUS, 0, 0})};
        if (proc == "Vessel_Velocity") return {pb::enc_tuple({0, KERBIN_ROTATION_SPEED, 0})};
        if (proc == "Flight_get_Rotation") return {pb::enc_tuple({0, 0, 0, 1})};
        if (proc == "Flight_get_Pitch" || proc == "Flight_get_Heading") return {pb::enc_float(90)};
        if (proc == "Flight_get_Roll" || proc == "Control_get_Throttle") return {pb::enc_float(0)};
        if (proc == "Flight_get_GForce") return {pb::enc_float(1)};
        if (proc == "Vessel_get_Mass") return {pb::enc_float(10000)};
        if (proc == "Vessel_get_AvailableThrust" || proc == "Vessel_get_SpecificImpulse") return {pb::enc_float(0)};
        if (proc == "Vessel_get_Situation" || proc == "Control_get_SASMode") return {pb::enc_sint32(0)};
        if (proc == "Control_get_SpeedMode" || proc == "Control_get_CurrentStage" || proc == "Stage_get_Number")
            return {pb::enc_sint32(1)};
        if (proc == "Control_get_SAS" || proc == "Control_get_RCS") return {pb::enc_bool(false)};
        if (proc == "Control_get_Nodes") return {list({})};
        if (proc == "Vessel_get_Stages") return {list({9})};
        if (proc == "Parts_get_All") return {list({10, 12})};
        if (proc == "Stage_get_Parts") return {list({10})};
        if (proc == "Resources_Amount") return {pb::enc_float(50)};
        if (proc == "Resources_Max") return {pb::enc_float(100)};
        if (proc == "get_UT" || proc.rfind("Flight_get_", 0) == 0 || proc.rfind("Orbit_get_", 0) == 0)
            return {pb::enc_double(proc == "Orbit_get_Eccentricity" ? .9 : 1000)};
        return {"", false, "no such procedure " + proc};
    }
};
}  // namespace

TEST(launch_pad_errors_drop_only_the_failing_values) {
    PadServer pad;
    FakeServer server([&](const std::string& proc, const std::vector<std::string>&) { return pad.answer(proc); });
    server.fail_add_stream = true;
    KrpcSource::Options o;
    o.address = "127.0.0.1";
    o.rpc_port = server.rpc_port;
    o.stream_port = server.stream_port;
    o.discovery = false;
    o.retry_seconds = .1;
    KrpcSource src(o);
    src.start();
    double start = monotonic(), end = start + 5;
    auto ready = [&] {
        auto s = src.latest();
        return s->state == State::Flight && s->nav && s->systems && !s->systems->stages.empty();
    };
    while (monotonic() < end && !ready()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(ready());
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));  // many samples at 10 Hz / 2 Hz
    auto snap = src.latest();
    double elapsed = monotonic() - start;
    src.stop();
    CHECK(snap->state == State::Flight);
    CHECK(snap->nav.has_value());  // the attitude survives the singular surface speed frame
    if (snap->nav) {
        CHECK_NEAR(snap->nav->pitch, 90, 1e-6);
        CHECK(!snap->nav->prograde && !snap->nav->speed);
    }
    CHECK(snap->systems.has_value());
    if (snap->systems && !snap->systems->stages.empty()) {
        CHECK(snap->systems->stages[0].parts == 1);
        CHECK(!snap->systems->stages[0].delta_v);  // unknown, not a reason to drop the panel
        CHECK_NEAR(snap->systems->resources[0].fraction().value_or(-1), .5, 1e-9);
    }
    CHECK_EQ(pad.count("Vessel_get_Orbit"), 1);  // the vessel session never restarted
    CHECK(pad.count("Stage_get_Number") <= 2);    // unknown delta-v does not force a stream rebuild
    CHECK(pad.count("Vessel_get_SurfaceSpeedReferenceFrame") <= 2 + static_cast<int>(elapsed / .5));  // retry 0.5 s
}

TEST(gauge_angle_sweeps_the_fuel_gauge_arc_from_e_to_f) {
    const double deg = 180.0 / 3.14159265358979323846;
    CHECK(gauges::SWEEP >= 100 && gauges::SWEEP <= 120);
    CHECK_NEAR(gauges::angle(0) * deg, 90 + gauges::SWEEP / 2, 1e-9);
    CHECK_NEAR(gauges::angle(.5) * deg, 90, 1e-9);
    CHECK_NEAR(gauges::angle(1) * deg, 90 - gauges::SWEEP / 2, 1e-9);
    CHECK_NEAR(gauges::angle(-1), gauges::angle(0), 1e-12);
    CHECK_NEAR(gauges::angle(7), gauges::angle(1), 1e-12);
    auto hub = gauges::pivot(100, 100, 10);
    CHECK_NEAR(hub.first, 100, 1e-9);
    CHECK(hub.second > 100);  // the hub is in the lower half of the face
    auto top = gauges::point(hub.first, hub.second, 5, .5);  // screen y grows downwards: mid-scale straight up
    CHECK_NEAR(top.first, hub.first, 1e-9);
    CHECK_NEAR(top.second, hub.second - 5, 1e-9);
    auto a = gauges::point(0, 0, 1, 0), b = gauges::point(0, 0, 1, 1);
    CHECK(a.first < 0 && b.first > 0 && a.second < 0);  // E left, F right, both above the hub
    CHECK_NEAR(a.second, b.second, 1e-9);
    auto s = gauges::steps(0, .5);
    CHECK(s.front() == 0 && s.back() == .5 && s.size() == 15);
}

TEST(gauge_scale_needle_and_window_stay_on_the_face) {
    auto hub = gauges::pivot(0, 0, 1);
    for (int step = 0; step <= 40; ++step) {
        double f = step / 40.0;
        for (double radius : {gauges::ZONE_OUT, gauges::SCALE, gauges::LETTERS}) {
            auto p = gauges::point(hub.first, hub.second, radius, f);
            CHECK(std::hypot(p.first, p.second) < gauges::FACE);
        }
        for (auto [x, y] : gauges::needle(hub.first, hub.second, 1, f)) CHECK(std::hypot(x, y) < gauges::FACE);
    }
    CHECK(std::hypot(gauges::WINDOW_W / 2, gauges::WINDOW_BOTTOM) < gauges::FACE);
    CHECK(gauges::WINDOW_TOP > gauges::PIVOT);  // the window is under the hub
}

TEST(gauge_sector_closes_between_the_two_radii) {
    auto pts = gauges::sector(0, 0, 10, 8, 0, .5);
    const size_t half = pts.size() / 2;
    CHECK(pts.size() % 2 == 0 && half == gauges::steps(0, .5).size());
    for (size_t i = 0; i < pts.size(); ++i)
        CHECK_NEAR(std::hypot(pts[i].first, pts[i].second), i < half ? 10.0 : 8.0, 1e-9);
    for (size_t i = 0; i < half; ++i) {  // point i and its mirror lie on the same ray from the hub
        auto o = pts[i], in = pts[pts.size() - 1 - i];
        CHECK_NEAR(o.first * in.second - o.second * in.first, 0, 1e-9);
    }
    CHECK_NEAR(pts.front().first, gauges::point(0, 0, 10, 0).first, 1e-12);  // starts at E, outer edge first
}

TEST(gauge_layout_fits_the_panel_at_every_resolution) {
    for (auto [width, height] : {std::pair{1260, 840}, std::pair{1920, 1080}, std::pair{2520, 1680}}) {
        double u = height / 100.0;
        double x = 2.5 * u, y = 58 * u, w = width * .44 - 3.7 * u, h = 20.4 * u, text_h = 4.0 * u, pad = .4 * u;
        for (int count = 1; count <= 6; ++count) {
            auto l = gauges::layout(x, y, w, h, count, text_h, pad);
            CHECK_EQ(l.cells.size(), size_t(count));
            CHECK(l.radius > 3.5 * u);
            for (const auto& c : l.cells) {
                CHECK(c.cx - l.radius >= x - 1e-6 && c.cx + l.radius <= x + w + 1e-6);
                CHECK(c.cy - l.radius >= y - 1e-6);
                CHECK(c.cy + gauges::BELOW_CENTRE * l.radius + text_h <= y + h + 1e-6);
            }
            for (size_t i = 1; i < l.cells.size(); ++i) CHECK(l.cells[i].cx - l.cells[i - 1].cx >= 2 * l.radius - 1e-6);
        }
    }
}

int main(int argc, char** argv) {
    log::set_quiet(true);
    return RUN_TESTS(argc, argv);
}
