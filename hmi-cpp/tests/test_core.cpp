// Pure-logic tests (formatting, attitude, orbit geometry, resources, scenes,
// discovery, config, kRPC client, reconnect). No display, no game.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <map>
#include <mutex>
#include <cstring>
#include <sstream>
#include <fstream>
#include <limits>
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
#include "shipreport.hpp"
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

TEST(format_clock_rejects_unrepresentable_times) {
    const double limit = std::ldexp(1.0, std::numeric_limits<long long>::digits);
    for (double ut : {1e100, std::numeric_limits<double>::max(), limit,
                      std::nextafter(limit, INFINITY), static_cast<double>(std::numeric_limits<long long>::max()),
                      double(INFINITY), double(-INFINITY), double(NAN), -1.0})
        CHECK_EQ(fmt::clock(ut), "--");
    CHECK(fmt::clock(std::nextafter(limit, 0.0)) != "--");
    CHECK_EQ(fmt::clock(61.9), "Y1 D001 00:01:01");
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

TEST(attitude_slerp_and_navball_smoothing) {
    Quat a = attitude(0, 0, 0), b = attitude(0, 90, 0);
    CHECK_NEAR(angle_between(a, b), 90, 1e-6);
    CHECK_NEAR(angle_between(a, slerp(a, b, 0.5)), 45, 1e-6);
    CHECK_NEAR(angle_between(slerp(a, b, 0), a), 0, 1e-6);
    CHECK_NEAR(angle_between(slerp(a, b, 1), b), 0, 1e-6);
    Quat neg{-b.x, -b.y, -b.z, -b.w};  // same attitude, opposite sign: still the short 45 degree way
    CHECK_NEAR(angle_between(a, slerp(a, neg, 0.5)), 45, 1e-6);
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

// ------------------------------------------------------------------------------------------- maneuver orbit
TEST(maneuver_conic_passes_through_the_node_and_follows_the_burn) {
    double rp = KERBIN_RADIUS + 80000, ra = KERBIN_RADIUS + 250000, a = (rp + ra) / 2, e = (ra - rp) / (ra + rp);
    OrbitState frame{{rp, 0, 0}, {0, 0, std::sqrt(KERBIN_MU * (2 / rp - 1 / a))}, KERBIN_MU, Axes{}};
    Conic current = *frame.conic();
    // No burn: the same conic as the current orbit; the node lies on it at its own true anomaly.
    ManeuverOrbit none = demo_maneuver(a, e, KERBIN_MU, 2.3, 0);
    Conic same = *none.conic(frame);
    CHECK_NEAR(same.eccentricity, current.eccentricity, 1e-9);
    CHECK_NEAR(same.p, current.p, 1e-3);
    CHECK_NEAR(anomaly_of(same, none.position), 2.3, 1e-9);
    // Prograde raises the opposite apsis; the node stays on the new conic.
    ManeuverOrbit burn = demo_maneuver(a, e, KERBIN_MU, 2.3, 300);
    Conic next = *burn.conic(frame);
    CHECK(*next.apoapsis() > ra + 100000);
    auto on = next.position(anomaly_of(next, burn.position));
    CHECK(on.has_value());
    if (on) CHECK(std::sqrt(dot(*on - burn.position, *on - burn.position)) < 1e-3);
    // An escape burn is open: no apoapsis.
    CHECK(!demo_maneuver(a, e, KERBIN_MU, 0, 1200).conic(frame)->apoapsis());
}

TEST(conic_span_stops_at_the_sphere_of_influence) {
    Conic ellipse{1e6 * (1 - .25), .5, {1, 0, 0}, {0, 1, 0}};  // a = 1e6: rp 5e5, ra 1.5e6
    auto closed = conic_span(ellipse, 1.0, 2e6);
    CHECK(closed && closed->closed);
    CHECK(conic_span(ellipse, 1.0, 0)->closed);  // unknown sphere of influence: unbounded
    auto open = conic_span(ellipse, 1.0, 1e6);   // apoapsis beyond the limit: from the node to the boundary
    CHECK(open && !open->closed);
    if (open) {
        CHECK_NEAR(open->from, 1.0, 1e-12);
        CHECK_NEAR(std::sqrt(dot(*ellipse.position(open->to), *ellipse.position(open->to))), 1e6, 1e-3);
    }
    CHECK(!conic_span(ellipse, 3.0, 1e6));  // the node itself outside the limit
    CHECK(!conic_span(ellipse, 0, 4e5));    // periapsis beyond the limit

    Conic hyperbola{1e6, 2, {1, 0, 0}, {0, 1, 0}};
    auto free = conic_span(hyperbola, -0.5, 0);
    CHECK(free && !free->closed && free->to < std::acos(-.5) && free->to > std::acos(-.5) - 1e-3);
    auto soi = conic_span(hyperbola, -0.5, 5e6);
    CHECK(soi && soi->to < free->to);
    if (soi) CHECK_NEAR(std::sqrt(dot(*hyperbola.position(soi->to), *hyperbola.position(soi->to))), 5e6, 1e-2);
    CHECK(!conic_span(Conic{0, 0, {1, 0, 0}, {0, 1, 0}}, 0, 0));  // degenerate

    Axes axes;  // default x/z view plane: a conic in the x/y plane projects onto its x axis, still bounded
    auto track = conic_track(ellipse, *closed, Axes{{1, 0, 0}, {0, 1, 0}}, 100);
    CHECK_EQ(track.size(), size_t(101));
    CHECK(track.front() == track.back());
    for (const auto& p : conic_track(hyperbola, *soi, axes, 200)) CHECK(std::hypot(p.first, p.second) <= 5e6 + 1);
}

TEST(dashes_keep_their_length_along_corners) {
    auto length = [](const std::vector<Point2>& d) {
        double l = 0;
        for (size_t i = 1; i < d.size(); ++i) l += std::hypot(d[i].first - d[i - 1].first, d[i].second - d[i - 1].second);
        return l;
    };
    auto straight = dash_polyline({{0, 0}, {10, 0}}, 1, 1);
    CHECK_EQ(straight.size(), size_t(5));
    for (const auto& d : straight) CHECK_NEAR(length(d), 1, 1e-9);
    CHECK_NEAR(straight[1].front().first, 2, 1e-9);
    // A dash across a corner keeps the corner vertex and its full length.
    auto corner = dash_polyline({{0, 0}, {2.5, 0}, {2.5, 10}}, 1, 1);
    CHECK(corner.size() >= 6);
    for (size_t i = 0; i + 1 < corner.size(); ++i) CHECK_NEAR(length(corner[i]), 1, 1e-9);
    CHECK_EQ(corner[1].size(), size_t(3));
    // The pattern phase and degenerate inputs.
    auto shifted = dash_polyline({{0, 0}, {10, 0}}, 1, 1, 1.5);
    CHECK(!shifted.empty() && std::fabs(shifted[0].front().first - .5) < 1e-9);
    CHECK(dash_polyline({{0, 0}}, 1, 1).empty());
    CHECK(dash_polyline({{0, 0}, {1, 1}}, 0, 1).empty());
    CHECK(dash_polyline({{0, 0}, {0, 0}, {0, 0}}, 1, 1).empty());
    CHECK_EQ(dash_polyline({{0, 0}, {5, 0}}, 1, 0).size(), size_t(5));  // no gap: abutting dashes, a solid line
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

TEST(scene_context_carries_an_optional_skin) {
    Context c = parse_context_reply("KSPHMI/1 vab skin=apollo");
    CHECK_EQ(c.scene, "vab");
    CHECK_EQ(c.skin, "apollo");
    CHECK_EQ(parse_context("KSPHMI/1 vab skin=apollo"), "vab");
    c = parse_context_reply("KSPHMI/1 flight skin=classic");  // flight is no facility view, the skin still counts
    CHECK_EQ(c.scene, "");
    CHECK_EQ(c.skin, "classic");
    CHECK_EQ(parse_context_reply("KSPHMI/1 vab").skin, "");  // KSPSerialIO: no field, the config decides
    for (const char* reply : {"KSPHMI/2 vab skin=apollo", "KSPHMI/1 vab skin=", "KSPHMI/1 vab skin=a;b", "skin=apollo"})
        CHECK_EQ(parse_context_reply(reply).skin, "");
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
    char hostname[256] = {0};
    gethostname(hostname, sizeof hostname - 1);
    CHECK(line.find(hostname) != std::string::npos);
    CHECK_EQ(route_address("127.0.0.1"), "127.0.0.1");
    CHECK_EQ(route_address("not a host name"), "");
    double end = monotonic() + 2;
    while (monotonic() < end && line.find("looking up") != std::string::npos) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        line = info.line();
    }
    CHECK(line.find("looking up") == std::string::npos);
    CHECK(line.find("This display: 127.") == std::string::npos);
}

TEST(netinfo_slow_query_does_not_block_cached_reads_or_destruction) {
    struct Gate {
        std::mutex mutex;
        std::condition_variable wake;
        bool entered = false, released = false, finished = false;
    };
    auto gate = std::make_shared<Gate>();
    auto query = [gate](const std::string&) {
        std::unique_lock<std::mutex> lock(gate->mutex);
        gate->entered = true;
        gate->wake.notify_all();
        gate->wake.wait(lock, [&] { return gate->released; });
        gate->finished = true;
        gate->wake.notify_all();
        return std::string("This display: 10.0.0.42 (resolved)");
    };
    auto info = std::make_unique<NetInfo>("slow.example", 15, query);
    {
        std::unique_lock<std::mutex> lock(gate->mutex);
        CHECK(gate->wake.wait_for(lock, std::chrono::seconds(2), [&] { return gate->entered; }));
    }
    double start = monotonic();
    for (int i = 0; i < 100; ++i) CHECK(info->line().find("looking up") != std::string::npos);
    CHECK(monotonic() - start < 1);
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        gate->released = true;
        gate->wake.notify_all();
    }
    double end = monotonic() + 2;
    while (monotonic() < end && info->line().find("resolved") == std::string::npos)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK_EQ(info->line(), "This display: 10.0.0.42 (resolved)");
    info.reset();
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        gate->entered = gate->released = gate->finished = false;
    }
    info = std::make_unique<NetInfo>("slow.example", 15, query);
    {
        std::unique_lock<std::mutex> lock(gate->mutex);
        CHECK(gate->wake.wait_for(lock, std::chrono::seconds(2), [&] { return gate->entered; }));
    }
    start = monotonic();
    info.reset();  // DNS is still blocked; its worker must not retain or block the display
    CHECK(monotonic() - start < 1);
    {
        std::unique_lock<std::mutex> lock(gate->mutex);
        gate->released = true;
        gate->wake.notify_all();
        CHECK(gate->wake.wait_for(lock, std::chrono::seconds(2), [&] { return gate->finished; }));
    }
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

TEST(config_integer_fields_reject_invalid_conversions) {
    for (const char* key : {"display.fps", "display.width", "display.height", "krpc.rpc_port", "krpc.stream_port"}) {
        for (const char* value : {"1e100", "-1e100", "inf", "-inf", "nan", "1.5", "-0.5",
                                 "2147483648", "-2147483649"}) {
            Config cfg;
            bool rejected = false;
            try {
                const std::string name(key);
                const size_t dot = name.find('.');
                apply_toml(cfg, parse_toml("[" + name.substr(0, dot) + "]\n" + name.substr(dot + 1) + " = " + value));
            } catch (const std::runtime_error& e) {
                rejected = std::string(e.what()).find(std::string(key) + " must be an integer within the int range") == 0;
            }
            CHECK(rejected);
        }
    }
    for (int value : {std::numeric_limits<int>::min(), -1, 0, 60, std::numeric_limits<int>::max()}) {
        Config cfg;
        apply_toml(cfg, parse_toml("[display]\nwidth = " + std::to_string(value)));
        CHECK_EQ(cfg.display.width, value);
    }
    Config cfg;
    apply_toml(cfg, parse_toml("[display]\nfps = 60.0"));
    CHECK_EQ(cfg.display.fps, 60);
    char dir[] = "/tmp/ksphmi-int-XXXXXX";
    CHECK(mkdtemp(dir) != nullptr);
    const std::string path = std::string(dir) + "/invalid.toml";
    std::ofstream(path) << "[display]\nfps = 1e100\n";
    bool descriptive = false;
    try {
        load_config(path);
    } catch (const std::runtime_error& e) {
        descriptive = std::string(e.what()).find(path + ": display.fps must be an integer within the int range") == 0;
    }
    CHECK(descriptive);
    std::remove(path.c_str());
    rmdir(dir);
}

TEST(config_day_hours_requires_a_positive_representable_integer) {
    for (const char* value : {"0", "-1", "0.5", "6.5", "inf", "-inf", "nan", "2147483648", "1e300"}) {
        Config cfg;
        bool rejected = false;
        try {
            apply_toml(cfg, parse_toml(std::string("[format]\nday_hours = ") + value));
        } catch (const std::runtime_error& e) {
            rejected = std::string(e.what()).find("format.day_hours must be a positive integer") != std::string::npos;
        }
        CHECK(rejected);
        CHECK_EQ(cfg.format.day_hours, 6);  // invalid input never reaches the integer conversion
    }
    for (int hours : {1, 6, 24, 2147483647}) {
        Config cfg;
        apply_toml(cfg, parse_toml("[format]\nday_hours = " + std::to_string(hours)));
        CHECK_EQ(cfg.format.day_hours, hours);
        CHECK_EQ(fmt::duration(static_cast<double>(hours) * 3600, hours), "1d 0h");
        CHECK_EQ(fmt::clock(static_cast<double>(hours) * 3600, hours), "Y1 D002 00:00:00");
    }
    char dir[] = "/tmp/ksphmi-config-XXXXXX";
    CHECK(mkdtemp(dir) != nullptr);
    const std::string path = std::string(dir) + "/invalid.toml";
    std::ofstream(path) << "[format]\nday_hours = 0\n";
    bool descriptive = false;
    try {
        load_config(path);
    } catch (const std::runtime_error& e) {
        descriptive = std::string(e.what()).find(path + ": format.day_hours must be a positive integer") == 0;
    }
    CHECK(descriptive);
    std::remove(path.c_str());
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

TEST(krpc_stream_disconnect_overrides_cached_value) {
    FakeServer server([](const std::string&, const std::vector<std::string>&) -> FakeResult {
        return {pb::enc_double(1234.5)};
    });
    krpc::Client::Options o;
    o.address = "127.0.0.1";
    o.rpc_port = server.rpc_port;
    o.stream_port = server.stream_port;
    krpc::Client c(o);
    uint64_t ut = c.add_stream(krpc::sc("get_UT"), 10);
    CHECK_EQ(pb::dec_double(c.stream(ut).data), 1234.5);
    server.disconnect_stream();
    bool disconnected = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        try {
            c.stream(ut);
        } catch (const krpc::ConnectionError& e) {
            disconnected = std::string(e.what()).find("stream connection lost:") != std::string::npos;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(disconnected);
    CHECK_EQ(pb::dec_double(c.invoke(krpc::sc("get_UT")).data), 1234.5);
}

TEST(krpc_silent_stream_channel_expires_cached_values_while_rpc_stays_alive) {
    FakeServer server([](const std::string&, const std::vector<std::string>&) -> FakeResult {
        return {pb::enc_double(1234.5)};
    });
    krpc::Client::Options o;
    o.address = "127.0.0.1";
    o.rpc_port = server.rpc_port;
    o.stream_port = server.stream_port;
    krpc::Client c(o);
    uint64_t ut = c.add_stream(krpc::sc("get_UT"), 10);
    CHECK_EQ(pb::dec_double(c.stream(ut).data), 1234.5);
    // Identical values arriving for longer than the idle limit are still live, even without reads.
    std::this_thread::sleep_for(std::chrono::milliseconds(3200));
    CHECK_EQ(pb::dec_double(c.stream(ut).data), 1234.5);
    server.pause_streams = true;
    CHECK_EQ(pb::dec_double(c.stream(ut).data), 1234.5);  // a recent cached value remains valid
    std::this_thread::sleep_for(std::chrono::milliseconds(3200));
    CHECK_EQ(pb::dec_double(c.invoke(krpc::sc("get_UT")).data), 1234.5);
    bool expired = false;
    try {
        c.stream(ut);
    } catch (const krpc::ConnectionError& e) {
        expired = std::string(e.what()).find("stream channel inactive") != std::string::npos;
    }
    CHECK(expired);
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

TEST(stage_delta_v_recovers_after_stream_creation_is_rejected) {
    PadServer pad;
    std::atomic<bool> available{false};
    FakeServer server([&](const std::string& proc, const std::vector<std::string>&) {
        auto result = pad.answer(proc);
        if (proc == "Stage_get_DeltaV" && available) return FakeResult{pb::enc_float(1250)};
        return result;
    });
    server.fail_add_stream = true;
    KrpcSource::Options o;
    o.address = "127.0.0.1";
    o.rpc_port = server.rpc_port;
    o.stream_port = server.stream_port;
    o.discovery = false;
    KrpcSource src(o);
    src.start();
    auto ready = [&] {
        auto s = src.latest();
        return s->state == State::Flight && s->systems && !s->systems->stages.empty();
    };
    double end = monotonic() + 5;
    while (monotonic() < end && !ready()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(ready());
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    int rejected = pad.count("Stage_get_DeltaV");
    CHECK(rejected >= 2);
    CHECK(rejected <= 5);  // retry is bounded, not one AddStream per 10 Hz sample
    available = true;  // same stage and parts; KSP has now computed delta-v
    auto recovered = [&] {
        auto s = src.latest();
        return ready() && s->systems && !s->systems->stages.empty() && s->systems->stages[0].delta_v.has_value();
    };
    end = monotonic() + 3;
    while (monotonic() < end && !recovered()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto snapshot = src.latest();
    CHECK(recovered());
    src.stop();
    if (snapshot->systems && !snapshot->systems->stages.empty()) {
        CHECK_NEAR(snapshot->systems->stages[0].delta_v.value_or(-1), 1250, 1e-6);
        CHECK(snapshot->systems->stages[0].parts == 1);
        CHECK_NEAR(snapshot->systems->resources[0].fraction().value_or(-1), .5, 1e-9);
    }
    CHECK_EQ(pad.count("Vessel_get_Orbit"), 1);  // no session restart
    CHECK_EQ(pad.count("Stage_get_Number"), 1);  // no topology rebuild
    CHECK_EQ(pad.count("Vessel_get_Resources"), 1);  // working resource streams remain intact
}

namespace {
/// The pad vessel with one maneuver node; Node.orbit answers in the body's non-rotating frame (object 11).
struct NodeServer : PadServer {
    std::atomic<double> prograde{100};
    std::atomic<bool> broken{false};  ///< Node.orbit raises, as for a node kRPC cannot resolve
    std::atomic<int> frame_errors{0};
    FakeResult node(const std::string& proc, const std::vector<std::string>& args) {
        if (proc == "Control_get_Nodes") return {list({20})};
        if (proc == "Node_get_UT") return {pb::enc_double(5000)};
        if (proc == "Node_get_Prograde") return {pb::enc_double(prograde)};
        if (proc == "Node_get_Normal" || proc == "Node_get_Radial") return {pb::enc_double(0)};
        if (proc == "Node_get_Orbit") {
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++counts[proc];
            }
            if (broken) return {"", false, "Node has no orbit"};
            return {pb::enc_object(21)};
        }
        if (proc == "CelestialBody_get_SphereOfInfluence") return {pb::enc_double(KERBIN_SOI)};
        if (proc == "Orbit_PositionAt" || proc == "Orbit_VelocityAt") {
            std::lock_guard<std::mutex> lock(mutex);
            ++counts[proc];
            if (args.size() != 3 || pb::dec_object(args[2]) != 11 || pb::dec_double(args[1]) != 5000) ++frame_errors;
            if (proc == "Orbit_PositionAt") return {pb::enc_tuple({700000, 0, 0})};
            return {pb::enc_tuple({0, 0, 2245.0 + prograde})};
        }
        return answer(proc);
    }
};
}  // namespace

TEST(maneuver_orbit_is_read_once_per_node_change_and_fails_alone) {
    NodeServer pad;
    FakeServer server([&](const std::string& proc, const std::vector<std::string>& args) { return pad.node(proc, args); });
    KrpcSource::Options o;
    o.address = "127.0.0.1";
    o.rpc_port = server.rpc_port;
    o.stream_port = server.stream_port;
    o.discovery = false;
    o.retry_seconds = .1;
    KrpcSource src(o);
    src.start();
    auto wait_for = [&](auto ready) {
        double end = monotonic() + 5;
        while (monotonic() < end && !ready()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return ready();
    };
    CHECK(wait_for([&] { return src.latest()->maneuver_orbit.has_value(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));  // ~12 samples, the node unchanged
    auto snap = src.latest();
    CHECK(snap->maneuver_orbit.has_value());
    if (snap->maneuver_orbit) {
        CHECK_NEAR(snap->maneuver_orbit->position.x, 700000, 1e-9);
        CHECK_NEAR(snap->maneuver_orbit->velocity.z, 2345, 1e-9);
        CHECK_NEAR(snap->maneuver_orbit->ut, 5000, 1e-9);
        CHECK_NEAR(snap->maneuver_orbit->soi, KERBIN_SOI, 1e-6);
    }
    CHECK(pad.count("Orbit_PositionAt") <= 2);  // not every sample (10 Hz), at most the 2 s refresh
    CHECK_EQ(pad.frame_errors.load(), 0);       // at the node UT, in the non-rotating frame of the current orbit

    pad.prograde = 300;  // the node was edited: read again
    CHECK(wait_for([&] {
        auto s = src.latest();
        return s->maneuver_orbit && std::fabs(s->maneuver_orbit->velocity.z - 2545) < 1e-9;
    }));

    pad.broken = true;  // a failing node hides the dashed orbit only
    CHECK(wait_for([&] { return !src.latest()->maneuver_orbit.has_value(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    snap = src.latest();
    src.stop();
    CHECK(snap->state == State::Flight && snap->nav.has_value() && !snap->maneuver_orbit);
    CHECK_EQ(pad.count("Vessel_get_Orbit"), 1);  // the vessel session never restarted
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

// ------------------------------------------------------------------------------------------- VAB ship report
namespace {
std::string golden_ship_report() {
    std::ifstream in(std::string(KSPHMI_SOURCE_DIR) + "/tests/data/ship_report_v1.txt");
    std::stringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

bool rejects(const std::string& text, const std::string& reason) {
    std::string error;
    bool rejected = !parse_ship_report(text, &error);
    if (!rejected || error.find(reason) == std::string::npos)
        check::fail(__FILE__, __LINE__, "'" + text + "' gave '" + error + "', expected '" + reason + "'");
    return rejected;
}
}  // namespace

TEST(ship_report_parses_the_shared_sample) {
    // The same file is the expected output of the plugin's formatter (tools/plugin-shipreport-tests) and of kspsim.
    const std::string text = golden_ship_report();
    CHECK(!text.empty());
    std::string error;
    auto r = parse_ship_report(text, &error);
    CHECK(r.has_value());
    if (!r) return;
    CHECK_EQ(error, "");
    CHECK_EQ(r->revision, 57u);
    CHECK_EQ(r->facility, "vab");
    CHECK_EQ(r->name, "Kerbal X");  // %20 decoded
    CHECK_EQ(r->situation, "kerbin_asl");
    CHECK_EQ(r->situation_label(), "KERBIN \xC2\xB7 SEA LEVEL");
    CHECK_EQ(*r->mass, 18.4);
    CHECK_EQ(*r->dry, 6.1);
    CHECK_EQ(*r->cost, 23400.0);
    CHECK_EQ(*r->funds, 41200.0);
    CHECK_EQ(*r->parts, 42);
    CHECK_EQ(*r->crew, 1);
    CHECK_EQ(*r->seats, 3);
    CHECK_EQ(*r->height, 11.2);
    CHECK_EQ(*r->width, 3.8);
    CHECK_EQ(*r->engines, 5);
    CHECK(!r->delta_v_pending);
    CHECK_EQ(r->stages.size(), size_t(3));
    const StageReport& launch = r->stages.at(0);
    CHECK_EQ(launch.stage, 2);
    CHECK_EQ(launch.delta_v, 1720.0);
    CHECK_EQ(launch.delta_v_vac, 1960.0);
    CHECK_EQ(launch.twr, 1.63);
    CHECK_EQ(launch.burn, 48.0);
    CHECK_EQ(launch.events.size(), size_t(2));
    CHECK_EQ(launch.events.at(0).kind, "engines");
    CHECK_EQ(launch.events.at(0).count, 3);
    CHECK_EQ(launch.events.at(1).kind, "clamps");
    CHECK_EQ(r->stages.at(2).events.size(), size_t(3));
    CHECK_EQ(events_text(launch), "3 ENG  CLAMP");
    CHECK_EQ(events_text(r->stages.at(2)), "ENG  DECOUPLE  CHUTE");
    CHECK_EQ(events_text(StageReport{}), "");
    CHECK_EQ(r->total_delta_v(), 1720.0 + 1890 + 1210);
    CHECK_EQ(r->total_delta_v_vac(), 1960.0 + 2010 + 1340);
    // CRLF line endings and a missing final newline parse the same
    std::string crlf;
    for (char c : text) crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
    auto again = parse_ship_report(crlf.substr(0, crlf.size() - 2));
    CHECK(again && again->stages.size() == 3 && again->name == "Kerbal X");
}

TEST(ship_report_pending_delta_v_and_missing_values) {
    auto r = parse_ship_report("v=1 rev=3 facility=vab name=Probe sit=kerbin_vac\n"
                               "mass=0.53 dry=0.40 cost=nan parts=4 crew=0 seats=0 size=1.2,1.3 engines=1\n"
                               "dv=pending\n");
    CHECK(r.has_value());
    if (!r) return;
    CHECK(r->delta_v_pending);
    CHECK(r->stages.empty());
    CHECK(!r->cost.has_value());   // nan = unknown
    CHECK(!r->funds.has_value());  // no funds field outside career
    CHECK_EQ(r->total_delta_v(), 0.0);
    CHECK_EQ(r->situation_label(), "KERBIN \xC2\xB7 VACUUM");
    // a stage with an unknown TWR still parses; totals skip unknown values
    auto st = parse_ship_report("v=1 rev=4 name=A sit=mun_alt\nst=1,nan,300,nan,10\nst=0,200,250,1.2,5\n");
    CHECK(st && st->stages.size() == 2 && std::isnan(st->stages[0].twr) && st->total_delta_v() == 200);
    CHECK(st && st->situation_label() == "MUN \xC2\xB7 ALTITUDE");
}

TEST(ship_report_rejects_unknown_versions_and_malformed_lines) {
    rejects("", "empty");
    rejects("rev=1 name=x\n", "no version");
    rejects("v=2 rev=1 facility=vab\nst=1,1,1,1,1\n", "unsupported version 2");
    rejects("v=one rev=1\n", "bad v");
    rejects("v=1 rev=-1\n", "bad rev");
    rejects("v=1 rev=1 name=Bad%2\n", "bad name");
    rejects("v=1 rev=1 name=Bad%zz\n", "bad name");
    rejects("v=1 rev=1\nmass=heavy\n", "bad mass");
    rejects("v=1 rev=1\nmass=inf\n", "bad mass");
    rejects("v=1 rev=1\nparts=4.5\n", "bad parts");
    rejects("v=1 rev=1\nsize=12\n", "bad size");
    rejects("v=1 rev=1\nst=1,2,3\n", "bad st");
    rejects("v=1 rev=1\nst=1,2,3,4,5,engines\n", "bad st");
    rejects("v=1 rev=1\nst=1,2,3,4,5,engines:x\n", "bad st");
    rejects("v=1 rev=1\ndv=soon\n", "bad dv");
    rejects("v=1 rev=1\ndv=pending\nst=1,2,3,4,5\n", "pending together");
    rejects("v=1 rev=1  name=double-space\n", "bad group");
    rejects("v=1 rev=1\nnot a group\n", "bad group");
    // later phases add line kinds and keys: ignored, not rejected
    auto r = parse_ship_report("v=1 rev=9 name=X future=1\nchk=control:ok,power:warn\nlim=parts:42/45\n");
    CHECK(r && r->revision == 9 && r->name == "X" && r->stages.empty());
}

TEST(config_vab_destinations_and_target) {
    Config cfg;
    std::ifstream in(std::string(KSPHMI_SOURCE_DIR) + "/config.toml");
    std::stringstream buf;
    buf << in.rdbuf();
    apply_toml(cfg, parse_toml(buf.str()));
    CHECK_EQ(cfg.vab.target, "mun_landing");
    CHECK(cfg.vab.destinations.size() >= 4 && static_cast<int>(cfg.vab.destinations.size()) <= MAX_DESTINATIONS);
    std::map<std::string, double> by_key;
    for (const auto& d : cfg.vab.destinations) by_key[d.key] = d.delta_v;
    CHECK_EQ(by_key["kerbin_orbit"], 3400.0);
    CHECK_EQ(by_key["mun_flyby"], 4310.0);
    CHECK_EQ(by_key["minmus_landing"], 4670.0);
    CHECK_EQ(by_key["mun_landing"], 5150.0);
    for (size_t i = 1; i < cfg.vab.destinations.size(); ++i)
        CHECK(cfg.vab.destinations[i - 1].delta_v <= cfg.vab.destinations[i].delta_v);  // sorted for display
    // a local file adds a destination, changes another and picks it as the target
    apply_toml(cfg, parse_toml("[vab]\ntarget = \"duna_landing\"\n[vab.destinations]\nduna_landing = 5900\nmun_flyby = 4300\n"));
    CHECK_EQ(cfg.vab.target, "duna_landing");
    CHECK_EQ(cfg.vab.destinations.back().key, "duna_landing");
    by_key.clear();
    for (const auto& d : cfg.vab.destinations) by_key[d.key] = d.delta_v;
    CHECK_EQ(by_key["mun_flyby"], 4300.0);
    // dotted table names are parsed part by part
    CHECK_EQ(parse_toml("[ vab . destinations ]\nx = 1\n").count("vab.destinations.x"), size_t(1));
    for (const char* bad : {"[vab]\ntarget = \"nowhere\"\n", "[vab.destinations]\nmoon = 0\n",
                            "[vab.destinations]\nmoon = -5\n", "[vab.destinations]\nmoon = \"far\"\n",
                            "[vab.destinations]\nMoon = 100\n", "[vab.destinations]\nmoon = 1e9\n"}) {
        Config c2 = cfg;
        bool threw = false;
        try {
            apply_toml(c2, parse_toml(bad));
        } catch (const std::runtime_error& e) {
            threw = std::string(e.what()).find("vab.") != std::string::npos;  // the error names the key
        }
        CHECK(threw);
    }
    Config many;
    std::string thirteen = "[vab.destinations]\n";
    for (int i = 0; i <= MAX_DESTINATIONS; ++i) thirteen += "d" + std::to_string(i) + " = " + std::to_string(1000 + i) + "\n";
    bool threw = false;
    try {
        apply_toml(many, parse_toml(thirteen));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(Config().vab.destinations.empty());  // no config: the report shows no destinations, nothing breaks
}

TEST(ship_report_poller_fetches_on_revision_change_and_survives_errors) {
    std::atomic<uint32_t> revision{5};
    std::atomic<int> mode{0};  // 0 serve, 1 no service, 2 malformed text, 3 empty text
    std::atomic<int> reports{0};
    const std::string good = golden_ship_report();
    FakeServer server([&](const std::string& proc, const std::vector<std::string>&) -> FakeResult {
        if (mode == 1) return {"", false, "No such service KSPHMI"};
        if (proc == "Revision") return {pb::enc_uint32(revision)};
        if (proc == "ShipReport") {
            ++reports;
            if (mode == 2) return {pb::enc_string("v=1 rev=1\nst=broken\n")};
            if (mode == 3) return {pb::enc_string("")};
            return {pb::enc_string(good)};
        }
        return {"", false, "no such procedure " + proc};
    });
    krpc::Client::Options o;
    o.address = "127.0.0.1";
    o.rpc_port = server.rpc_port;
    o.stream_port = server.stream_port;
    krpc::Client c(o);
    ShipReportPoller poller(1.0);
    auto r = poller.poll(c, 0);
    CHECK(r && r->name == "Kerbal X" && r->stages.size() == 3);
    CHECK(poller.poll(c, .5) == r);  // same revision: no new ShipReport call, same object
    CHECK_EQ(reports.load(), 1);
    revision = 6;
    auto r2 = poller.poll(c, 1);
    CHECK(r2 && r2 != r);
    CHECK_EQ(reports.load(), 2);
    // the DLL or kRPC is missing: no report (static cards), the session goes on, retried after the back-off
    mode = 1;
    revision = 7;
    CHECK(!poller.poll(c, 2));
    CHECK(poller.problem().find("No such service") != std::string::npos);
    mode = 0;
    CHECK(!poller.poll(c, 2.5));  // still backing off
    auto r3 = poller.poll(c, 3.1);
    CHECK(r3 && poller.problem().empty());
    // a malformed report is dropped and not fetched again for the same revision
    mode = 2;
    revision = 8;
    int before = reports;
    CHECK(!poller.poll(c, 4));
    CHECK(!poller.poll(c, 4.5));
    CHECK_EQ(reports.load(), before + 1);
    CHECK(poller.problem().find("revision 8 rejected") != std::string::npos);
    // an editor without a ship yet answers ""
    mode = 3;
    revision = 9;
    CHECK(!poller.poll(c, 5));
    CHECK_EQ(poller.problem(), "no ship reported yet");
    mode = 0;
    revision = 10;
    CHECK(poller.poll(c, 5.5) != nullptr);
    poller.reset();
    CHECK(poller.problem().empty());
}

int main(int argc, char** argv) {
    log::set_quiet(true);
    return RUN_TESTS(argc, argv);
}
