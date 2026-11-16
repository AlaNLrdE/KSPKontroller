// Pure-logic tests (formatting, attitude, orbit geometry, resources, scenes,
// discovery, config, kRPC client, reconnect). No display, no game.
#if !defined(_WIN32)
#include <unistd.h>
#endif

#include <atomic>
#include <cstdlib>
#include <map>
#include <mutex>
#include <cstring>
#include <sstream>
#include <fstream>
#include <limits>
#include <thread>

#include "bootstate.hpp"
#include "check.hpp"
#include "config.hpp"
#include "fake_krpc.hpp"
#include "fmt.hpp"
#include "gauges.hpp"
#include "keyboard.hpp"
#include "krpc.hpp"
#include "log.hpp"
#include "model.hpp"
#include "navmath.hpp"
#include "net.hpp"
#include "netinfo.hpp"
#include "pb.hpp"
#include "scenes.hpp"
#include "shipreport.hpp"
#include "telemetry.hpp"
#include "vesselview.hpp"

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

TEST(span_until_ends_at_the_entry_in_the_direction_of_motion) {
    const double r = 1e6, v = std::sqrt(KERBIN_MU / r);
    OrbitState state{{r, 0, 0}, {0, 0, v}, KERBIN_MU, {}};
    auto circle = state.conic();
    CHECK(circle.has_value());
    if (!circle) return;
    double start = anomaly_of(*circle, state.position);
    // A quarter turn ahead: the vessel moves towards +z.
    ConicSpan quarter = span_until(*circle, ConicSpan{-PI, PI, true}, start, Vec3{0, 0, r});
    CHECK(!quarter.closed);
    CHECK_NEAR(quarter.to - quarter.from, PI / 2, 1e-9);
    CHECK(span_contains(quarter, start + .1) && span_contains(quarter, start + 2 * PI + .1));
    CHECK(!span_contains(quarter, start - .1) && !span_contains(quarter, start + 2));
    CHECK(span_contains(ConicSpan{-PI, PI, true}, 123.0));
    // Three quarters: the end behind the start is reached going forwards, never backwards.
    ConicSpan most = span_until(*circle, ConicSpan{-PI, PI, true}, start, Vec3{0, 0, -r});
    CHECK_NEAR(most.to - most.from, 3 * PI / 2, 1e-9);
    // An open span keeps its start and stops at whichever comes first.
    Conic flat{1e6, .5, {1, 0, 0}, {0, 0, 1}};
    ConicSpan open{-1, 1, false};
    CHECK_NEAR(span_until(flat, open, -1, *flat.position(.5)).to, .5, 1e-12);
    CHECK_NEAR(span_until(flat, open, -1, *flat.position(2)).to, 1, 1e-12);
    CHECK_NEAR(span_until(flat, open, -1, *flat.position(2)).from, -1, 1e-12);
}

namespace {
/// A flyby of Ike as the live game showed it (2026-11-16): hyperbolic, periapsis `rp`, entering at the sphere.
Encounter ike_flyby(double rp, double e) {
    Encounter x;
    x.body = "Ike", x.radius = 130000, x.mu = 1.8568369e10, x.soi = 1049598.5;
    const double p = rp * (1 + e), nu = -std::acos((p / x.soi - 1) / e), h = std::sqrt(x.mu * p);
    const double vr = x.mu / h * e * std::sin(nu), vt = x.mu / h * (1 + e * std::cos(nu));
    const Vec3 P{1, 0, 0}, T{0, 0, 1};
    x.position = (P * std::cos(nu) + T * std::sin(nu)) * x.soi;
    x.velocity = (P * std::cos(nu) + T * std::sin(nu)) * vr + (T * std::cos(nu) - P * std::sin(nu)) * vt;
    x.center = {3.2e6, 0, 0};
    return x;
}
}  // namespace

TEST(encounter_patch_enters_and_leaves_the_sphere_of_influence) {
    Encounter flyby = ike_flyby(130000 + 27781, 1.195);
    auto conic = flyby.conic();
    auto span = flyby.span();
    CHECK(conic && span && !span->closed);
    if (!conic || !span) return;
    CHECK_NEAR(conic->eccentricity, 1.195, 1e-6);
    CHECK_NEAR(flyby.periapsis_altitude().value_or(0), 27781, 1e-3);
    CHECK(span_contains(*span, 0));  // the periapsis lies between entry and exit
    CHECK_NEAR(span->from, anomaly_of(*conic, flyby.position), 1e-12);
    auto out = flyby.exit();
    CHECK(out.has_value());
    if (out) CHECK_NEAR(std::sqrt(dot(*out, *out)), flyby.soi, 3);
    CHECK_NEAR(flyby.entry().x, 3.2e6 + flyby.position.x, 1e-6);
    // Captured (after a capture burn, well inside the sphere): the whole ellipse and no exit.
    Encounter captured = flyby;
    captured.position = {.5 * captured.soi, 0, 0}, captured.velocity = {0, 0, std::sqrt(captured.mu / (.5 * captured.soi))};
    CHECK(captured.span() && captured.span()->closed);
    CHECK(!captured.exit());
    // Impact: a periapsis below the surface reads negative.
    CHECK(ike_flyby(100000, 1.3).periapsis_altitude().value_or(1) < 0);
    Encounter broken = flyby;
    broken.mu = 0;
    CHECK(!broken.conic() && !broken.span() && !broken.exit());
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
    net::socket_t server = net::open_socket(SOCK_DGRAM);
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
        long n = recvfrom(server, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (n > 0) request.assign(buf, static_cast<size_t>(n));
        for (const char* reply : {"unrelated packet", "KSPHMI/1 mainmenu"})
            sendto(server, reply, std::strlen(reply), 0, reinterpret_cast<sockaddr*>(&peer), plen);
    });
    CHECK_EQ(discover(port, 0.5, {"127.0.0.1"}), "127.0.0.1");
    responder.join();
    CHECK_EQ(request, "KSPHMI/1");
    CHECK_EQ(discover(port, 0.02, {"127.0.0.1"}), "");
    net::close_socket(server);
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
    CHECK(line.find(net::host_name()) != std::string::npos);
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
    const std::string dir = check::make_temp_dir("ksphmi-test");
    CHECK(!dir.empty());
    std::string app = dir, local = app + "/local.toml";
    std::ofstream(app + "/config.toml") << "source = \"krpc\"  # comment\n[krpc]\naddress = 'pc'\nrate_hz = 10\n"
                                           "[display]\nfps = 10\nlayer_cache = \"on\"\n[format]\nday_hours = 6\n";
    std::ofstream(local) << "[display]\nfps = 5\n";
    check::set_env("KSPHMI_APP_DIR", app);
    check::set_env("KSPHMI_LOCAL_CONFIG", local);
    check::set_env("KSPHMI_KRPC_ADDRESS", "10.0.0.9");
    Config cfg = load_config();
    check::unset_env("KSPHMI_APP_DIR");
    check::unset_env("KSPHMI_LOCAL_CONFIG");
    check::unset_env("KSPHMI_KRPC_ADDRESS");
    CHECK_EQ(cfg.display.fps, 5);
    CHECK_EQ(cfg.display.layer_cache, "on");  // untouched keys keep config.toml / defaults
    CHECK_EQ(cfg.krpc.address, "10.0.0.9");
    CHECK_EQ(cfg.krpc.rpc_port, 50000);
    CHECK_EQ(cfg.loaded.size(), size_t(2));
    Config only = load_config(local);  // --config: that file only
    CHECK_EQ(only.display.fps, 5);
    CHECK_EQ(only.krpc.address, "localhost");
    std::remove((app + "/config.toml").c_str());
    std::remove(local.c_str());
    check::remove_dir(dir);
}

TEST(config_integer_fields_reject_invalid_conversions) {
    for (const char* key : {"display.fps", "krpc.rpc_port", "krpc.stream_port"}) {
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
        apply_toml(cfg, parse_toml("[krpc]\nrpc_port = " + std::to_string(value)));
        CHECK_EQ(cfg.krpc.rpc_port, value);
    }
    Config cfg;
    apply_toml(cfg, parse_toml("[display]\nfps = 60.0"));
    CHECK_EQ(cfg.display.fps, 60);
    const std::string dir = check::make_temp_dir("ksphmi-int");
    CHECK(!dir.empty());
    const std::string path = dir + "/invalid.toml";
    std::ofstream(path) << "[display]\nfps = 1e100\n";
    bool descriptive = false;
    try {
        load_config(path);
    } catch (const std::runtime_error& e) {
        descriptive = std::string(e.what()).find(path + ": display.fps must be an integer within the int range") == 0;
    }
    CHECK(descriptive);
    std::remove(path.c_str());
    check::remove_dir(dir);
}

TEST(config_monitor_picks_the_secondary_display) {
    CHECK_EQ(Config{}.display.monitor, std::string("auto"));
    CHECK_EQ(monitor_index("auto", 0), 0);  // SDL error or no display
    CHECK_EQ(monitor_index("auto", 1), 0);  // the Pi, or a PC with one monitor
    CHECK_EQ(monitor_index("auto", 2), 1);  // the game keeps display 0
    CHECK_EQ(monitor_index("auto", 3), 2);
    CHECK_EQ(monitor_index("0", 2), 0);
    CHECK_EQ(monitor_index("1", 3), 1);
    CHECK_EQ(monitor_index("5", 2), 0);  // unplugged monitor: back to the primary one
    for (const char* bad : {"", "-1", "1.5", "123", "second", "AUTO"}) CHECK(!valid_monitor(bad));
    Config cfg;
    apply_toml(cfg, parse_toml("[display]\nmonitor = \"1\""));
    CHECK_EQ(cfg.display.monitor, std::string("1"));
    bool rejected = false;
    try {
        apply_toml(cfg, parse_toml("[display]\nmonitor = \"left\""));
    } catch (const std::runtime_error& e) {
        rejected = std::string(e.what()).find("display.monitor must be") != std::string::npos;
    }
    CHECK(rejected);
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
    const std::string dir = check::make_temp_dir("ksphmi-config");
    CHECK(!dir.empty());
    const std::string path = dir + "/invalid.toml";
    std::ofstream(path) << "[format]\nday_hours = 0\n";
    bool descriptive = false;
    try {
        load_config(path);
    } catch (const std::runtime_error& e) {
        descriptive = std::string(e.what()).find(path + ": format.day_hours must be a positive integer") == 0;
    }
    CHECK(descriptive);
    std::remove(path.c_str());
    check::remove_dir(dir);
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

TEST(vessel_view_text_round_trips_and_rejects_bad_input) {
    VesselView demo = demo_vessel_view(0);
    demo.revision = 7;
    auto shape = parse_vessel_shape(format_vessel_shape(demo));
    CHECK(shape.has_value());
    CHECK_EQ(shape->revision, 7u);
    CHECK_EQ(shape->parts.size(), demo.parts.size());
    CHECK_EQ(shape->parts[0].fuel, -1);  // a shape carries no state
    CHECK(apply_vessel_state(*shape, format_vessel_state(demo)));
    for (size_t i = 0; i < demo.parts.size(); ++i) {
        CHECK_EQ(shape->parts[i].fuel, demo.parts[i].fuel);
        CHECK_EQ(shape->parts[i].heat, demo.parts[i].heat);
    }
    CHECK(!apply_vessel_state(*shape, "v=1 rev=8\ns=" + std::string("0:0")));  // another revision
    CHECK(!apply_vessel_state(*shape, "v=1 rev=7\ns=0:0"));                       // wrong part count
    VesselView one;
    one.revision = 3;
    one.parts.push_back(VesselPart{{{0, 0}, {1, 0}, {0, 1}}, 0, -1, 0});
    CHECK(apply_vessel_state(one, "v=1 rev=3\ns=250:-5"));  // clamped
    CHECK_EQ(one.parts[0].fuel, 100);
    CHECK_EQ(one.parts[0].heat, 0);
    CHECK(parse_vessel_shape("v=1 rev=0 parts=0\n").has_value());  // no vessel
    for (const char* bad : {"", "v=2 rev=1 parts=0", "v=1 rev=1 parts=1", "v=1 rev=1 parts=1\np=0,0:0,1:0",
                            "v=1 rev=1 parts=1\np=0,0:0,1:0,x:1", "v=1 rev=-1 parts=0", "v=1 rev=1 parts=1\nq=0,0:0,1:0,1:1"})
        CHECK(!parse_vessel_shape(bad).has_value());
    // The fixture the plugin's formatter and kspsim share
    std::ifstream shape_in(std::string(KSPHMI_SOURCE_DIR) + "/tests/data/vessel_shape_v1.txt");
    std::ifstream state_in(std::string(KSPHMI_SOURCE_DIR) + "/tests/data/vessel_state_v1.txt");
    std::stringstream shape_text, state_text;
    shape_text << shape_in.rdbuf();
    state_text << state_in.rdbuf();
    auto fixture = parse_vessel_shape(shape_text.str());
    CHECK(fixture.has_value());
    if (fixture) {
        CHECK(apply_vessel_state(*fixture, state_text.str()));
        CHECK_EQ(fixture->parts.size(), size_t(3));
        CHECK_EQ(fixture->parts[1].fuel, 48);
        CHECK_EQ(fixture->parts[2].heat, 93);
    }
}

TEST(vessel_view_draw_order_and_fuel_clip) {
    VesselView v;
    v.parts.push_back(VesselPart{{{0, 0}, {1, 0}, {0, 1}}, 0.5, -1, 0});
    v.parts.push_back(VesselPart{{{0, 0}, {1, 0}, {0, 1}}, 2.0, -1, 0});
    v.parts.push_back(VesselPart{{{0, 0}, {1, 0}, {0, 1}}, 0.5, -1, 0});
    const auto order = vessel_draw_order(v);
    CHECK(order == (std::vector<size_t>{1, 0, 2}));  // farthest first, ties in shape order
    const std::vector<Point2> square{{0, 0}, {10, 0}, {10, 10}, {0, 10}};  // screen y down
    auto lower = clip_below(square, 4);
    CHECK_EQ(lower.size(), size_t(4));
    for (const Point2& p : lower) CHECK(p.second >= 4);
    CHECK(clip_below(square, 11).empty());
    CHECK_EQ(clip_below(square, -1).size(), size_t(4));
}

TEST(vessel_view_poller_fetches_the_shape_only_on_a_new_revision) {
    std::mutex m;
    int shape_calls = 0, state_calls = 0;
    uint32_t revision = 1;
    bool missing = false;
    VesselView base = demo_vessel_view(0);
    FakeServer server([&](const std::string& proc, const std::vector<std::string>&) -> FakeResult {
        std::lock_guard<std::mutex> lock(m);
        if (missing) return {"", false, "Procedure \"VesselState\" not found, in service \"KSPHMIFlight\""};
        VesselView v = base;
        v.revision = revision;
        if (proc == "VesselState") return ++state_calls, FakeResult{pb::enc_string(format_vessel_state(v))};
        if (proc == "VesselShape") return ++shape_calls, FakeResult{pb::enc_string(format_vessel_shape(v))};
        return {"", false, "no such procedure " + proc};
    });
    krpc::Client::Options o;
    o.address = "127.0.0.1";
    o.rpc_port = server.rpc_port;
    o.stream_port = server.stream_port;
    krpc::Client c(o);
    VesselViewPoller poller(0.5, 5.0);
    auto view = poller.poll(c, 0);
    CHECK(view && view->parts.size() == base.parts.size());
    CHECK_EQ(view->parts[3].fuel, base.parts[3].fuel);
    CHECK(poller.poll(c, 0.2) == view);  // within the period: no call
    poller.poll(c, 1.0);
    poller.poll(c, 2.0);
    CHECK_EQ(poller.shape_fetches, 1);  // same revision: state only
    {
        std::lock_guard<std::mutex> lock(m);
        revision = 2;  // staging: new shape
    }
    poller.poll(c, 3.0);
    CHECK_EQ(poller.shape_fetches, 2);
    {
        std::lock_guard<std::mutex> lock(m);
        missing = true;  // KSPSerialIO.Hmi.dll older than 0.19.7.0
    }
    CHECK(!poller.poll(c, 4.0));
    CHECK(!poller.poll(c, 5.0));  // retried only after 5 s
    {
        std::lock_guard<std::mutex> lock(m);
        missing = false;
    }
    CHECK(poller.poll(c, 9.1) != nullptr);
}

TEST(keyboard_maps_ksp_default_flight_keys) {
    Keyboard kb;
    auto key = [](Key k) { return KeyEvent{k}; };
    auto digit = [](int d, bool ctrl = false, bool alt = false) { return KeyEvent{Key::Digit, d, ctrl, alt}; };
    auto only = [](const KeyResult& r, Command c) { return r.commands.size() == 1 && r.commands[0] == c; };
    // One-shot keys, as KSP binds them
    CHECK(only(kb.down(key(Key::T), 0), Command{Command::ToggleSas}));
    CHECK(kb.down(key(Key::T), 0).commands.empty());  // still held: auto-repeat is no new press
    kb.up(key(Key::T), 0);
    CHECK(only(kb.down(key(Key::R), 0), Command{Command::ToggleRcs}));
    CHECK(only(kb.down(key(Key::Z), 0), Command{Command::ThrottleFull}));
    CHECK(only(kb.down(key(Key::X), 0), Command{Command::ThrottleCut}));
    CHECK(only(kb.down(key(Key::Space), 0), Command{Command::Stage}));
    CHECK(only(kb.down(key(Key::Backspace), 0), Command{Command::Abort}));
    CHECK(only(kb.down(key(Key::Period), 0), Command{Command::WarpUp}));
    CHECK(only(kb.down(key(Key::Slash), 0), Command{Command::WarpStop}));
    CHECK(only(kb.down(key(Key::F5), 0), Command{Command::Quicksave}));
    CHECK(only(kb.down(key(Key::M), 0), Command{Command::ToggleMap}));
    // Held B = brakes while held; held F = SAS inverted while held
    CHECK(only(kb.down(key(Key::B), 0), Command{Command::SetBrakes, 1}));
    CHECK(only(kb.up(key(Key::B), 0), Command{Command::SetBrakes, 0}));
    CHECK(only(kb.down(key(Key::F), 0), Command{Command::ToggleSas}));
    CHECK(only(kb.up(key(Key::F), 0), Command{Command::ToggleSas}));
    // Digits: action groups (0 = group 10); Alt+digit SAS modes in the HMI button order; Ctrl+1/2 skins
    CHECK(only(kb.down(digit(1), 0), Command{Command::ActionGroup, 1}));
    CHECK(only(kb.down(digit(0), 0), Command{Command::ActionGroup, 0}));
    CHECK_EQ(command_name(Command{Command::ActionGroup, 0}), "action group 10");
    CHECK(only(kb.down(digit(2, false, true), 0), Command{Command::SasMode, 2}));  // PRO = kRPC Prograde
    CHECK(only(kb.down(digit(0, false, true), 0), Command{Command::SasMode, 1}));  // NODE = kRPC Maneuver
    CHECK(only(kb.down(digit(6, false, true), 0), Command{Command::SasMode, 6}));  // R+ = kRPC Radial
    CHECK_EQ(kb.down(digit(1, true), 0).skin, "classic");
    CHECK_EQ(kb.down(digit(2, true), 0).skin, "apollo");
    CHECK_EQ(kb.down(digit(3, true), 0).skin, "steampunk");
    CHECK_EQ(kb.down(digit(4, true), 0).skin, "ksp1");
    CHECK_EQ(kb.down(digit(5, true), 0).skin, "ksp2");
    CHECK(kb.down(digit(4, true), 0).commands.empty());
    CHECK(kb.down(digit(5, true), 0).commands.empty());
    CHECK(kb.down(digit(3, true), 0).commands.empty());
    CHECK(kb.down(digit(2, true), 0).commands.empty());
    CHECK(kb.down(digit(0, true), 0).vessel_mode);  // Ctrl+0: VESSEL fuel / heat
    CHECK(kb.down(digit(0, true), 0).commands.empty());
    CHECK(kb.down(key(Key::Escape), 0).quit);
    CHECK(kb.down(key(Key::Q), 0).commands.empty() && !kb.down(key(Key::Q), 0).quit);  // Q is roll left, not quit
    kb.release_all();
    // Axes with KSP's FlightCtrlState signs
    kb.down(key(Key::W), 0), kb.down(key(Key::D), 0), kb.down(key(Key::E), 0);
    kb.down(key(Key::L), 0), kb.down(key(Key::K), 0), kb.down(key(Key::H), 0);
    Held h = kb.held(0);
    CHECK_EQ(h.pitch, -1.f);
    CHECK_EQ(h.yaw, 1.f);
    CHECK_EQ(h.roll, 1.f);
    CHECK_EQ(h.x, -1.f);  // L = translate right
    CHECK_EQ(h.y, 1.f);   // K = translate up (I is down)
    CHECK_EQ(h.z, -1.f);  // H = translate forward
    kb.down(key(Key::S), 0);
    CHECK_EQ(kb.held(0).pitch, 0.f);  // W and S cancel, as in KSP
    CHECK(kb.release_all().commands.empty());
    CHECK(!kb.held(0).axes());
    // Throttle ramp: Shift at once, Ctrl after a short delay, never for a Ctrl+digit chord
    kb.down(key(Key::LeftShift), 0);
    CHECK_EQ(kb.held(0).throttle, 1);
    kb.up(key(Key::LeftShift), 0);
    kb.down(key(Key::LeftCtrl), 10);
    CHECK_EQ(kb.held(10.1).throttle, 0);
    CHECK_EQ(kb.held(10 + Keyboard::CTRL_RAMP_DELAY).throttle, -1);
    kb.up(key(Key::LeftCtrl), 11);
    kb.down(key(Key::LeftCtrl), 20);
    kb.down(digit(1, true), 20.05);
    CHECK_EQ(kb.held(25).throttle, 0);
    kb.up(key(Key::LeftCtrl), 25);
    // F9 quickloads only after being held for a second, once
    kb.down(key(Key::F9), 30);
    CHECK(kb.tick(30.5).commands.empty());
    CHECK(only(kb.tick(31), Command{Command::Quickload}));
    CHECK(kb.tick(32).commands.empty());
    kb.up(key(Key::F9), 32);
    kb.down(key(Key::F9), 40);
    kb.up(key(Key::F9), 40.3);
    CHECK(kb.tick(42).commands.empty());
    // Losing focus releases held brakes and SAS hold
    kb.down(key(Key::B), 50), kb.down(key(Key::F), 50);
    CHECK_EQ(kb.release_all().commands.size(), size_t(2));
}

TEST(keyboard_commands_reach_krpc_like_the_game_keys) {
    std::mutex m;
    std::vector<std::string> calls;
    std::map<std::string, std::string> last_args;  // procedure -> encoded arguments after the object
    bool sas = true, rcs = false, gear = true;
    int warp_mode = 2, rails = 0, physics = 0, max_rails = 7, camera_mode = 0;
    FakeServer server([&](const std::string& proc, const std::vector<std::string>& args) -> FakeResult {
        std::lock_guard<std::mutex> lock(m);
        calls.push_back(proc);
        std::string rest;
        for (size_t i = proc.rfind("Control_", 0) == 0 ? 1 : 0; i < args.size(); ++i) rest += args[i];
        last_args[proc] = rest;
        if (proc == "Vessel_get_Control") return {pb::enc_object(42)};
        if (proc == "Control_get_SAS") return {pb::enc_bool(sas)};
        if (proc == "Control_get_RCS") return {pb::enc_bool(rcs)};
        if (proc == "Control_get_Gear") return {pb::enc_bool(gear)};
        if (proc == "Control_set_SAS") return sas = pb::dec_bool(args.at(1)), FakeResult{""};
        if (proc == "Control_set_RCS") return rcs = pb::dec_bool(args.at(1)), FakeResult{""};
        if (proc == "Control_set_Gear") return gear = pb::dec_bool(args.at(1)), FakeResult{""};
        if (proc == "get_WarpMode") return {pb::enc_sint32(warp_mode)};
        if (proc == "get_Camera") return {pb::enc_object(77)};
        if (proc == "Camera_get_Mode") return {pb::enc_sint32(camera_mode)};
        if (proc == "Camera_set_Mode") return camera_mode = pb::dec_sint32(args.at(1)), FakeResult{""};
        if (proc == "get_RailsWarpFactor") return {pb::enc_sint32(rails)};
        if (proc == "get_PhysicsWarpFactor") return {pb::enc_sint32(physics)};
        if (proc == "get_MaximumRailsWarpFactor") return {pb::enc_sint32(max_rails)};
        if (proc == "set_RailsWarpFactor") return rails = pb::dec_sint32(args.at(0)), warp_mode = 0, FakeResult{""};
        if (proc == "set_PhysicsWarpFactor") return physics = pb::dec_sint32(args.at(0)), warp_mode = 1, FakeResult{""};
        for (const char* ok : {"Press", "SetThrottle", "SetAxes", "NudgeThrottle", "Control_set_Brakes",
                               "Control_set_Abort", "Control_ActivateNextStage", "Control_ToggleActionGroup",
                               "Control_set_SASMode", "Quicksave", "Quickload"})
            if (proc == ok) return {""};
        return {"", false, "no such procedure " + proc};
    });
    krpc::Client::Options o;
    o.address = "127.0.0.1";
    o.rpc_port = server.rpc_port;
    o.stream_port = server.stream_port;
    krpc::Client c(o);
    auto args_of = [&](const std::string& proc) {
        std::lock_guard<std::mutex> lock(m);
        return last_args[proc];
    };
    // Flight: one-shot changes through kRPC Control, like the keys and the board's edge toggles
    CHECK_EQ(send_command(c, {Command::ToggleSas}, 7, "flight"), "SAS off");
    CHECK_EQ(send_command(c, {Command::ToggleRcs}, 7, "flight"), "RCS on");
    CHECK_EQ(send_command(c, {Command::ToggleGear}, 7, "flight"), "Gear off");
    CHECK(!sas && rcs && !gear);
    send_command(c, {Command::SetBrakes, 1}, 7, "flight");
    CHECK_EQ(args_of("Control_set_Brakes"), pb::enc_bool(true));
    send_command(c, {Command::ActionGroup, 0}, 7, "flight");
    CHECK_EQ(args_of("Control_ToggleActionGroup"), pb::enc_uint32(0));
    send_command(c, {Command::SasMode, 6}, 7, "flight");
    CHECK_EQ(args_of("Control_set_SASMode"), pb::enc_sint32(6));
    CHECK_EQ(send_command(c, {Command::Stage}, 7, "flight"), "stage");
    // Throttle keys go through the plugin (one-shot), never kRPC's latched Control.Throttle
    send_command(c, {Command::ThrottleFull}, 7, "flight");
    CHECK_EQ(args_of("SetThrottle"), pb::enc_float(1));
    // Time warp: rails where allowed, physics otherwise; physics is stopped through its own factor
    CHECK_EQ(send_command(c, {Command::WarpUp}, 7, "flight"), "time warp rails 1");
    CHECK_EQ(send_command(c, {Command::WarpDown}, 7, "flight"), "time warp rails 0");
    max_rails = 0, warp_mode = 2;
    CHECK_EQ(send_command(c, {Command::WarpUp}, 7, "flight"), "time warp physics 1");
    CHECK_EQ(send_command(c, {Command::WarpUp}, 7, "flight"), "time warp physics 2");
    CHECK_EQ(send_command(c, {Command::WarpStop}, 7, "flight"), "time warp physics 0");
    // M: map view and back, through kRPC's camera mode
    CHECK_EQ(send_command(c, {Command::ToggleMap}, 7, "flight"), "map view");
    CHECK_EQ(camera_mode, 6);
    CHECK_EQ(send_command(c, {Command::ToggleMap}, 7, "flight"), "flight view");
    CHECK_EQ(camera_mode, 0);
    // Held keys: SetAxes with six floats, NudgeThrottle at KSP's rate
    Held h;
    h.pitch = -1, h.z = 1, h.throttle = -1;
    send_held(c, h, 0.1, true);
    CHECK_EQ(args_of("SetAxes"), pb::enc_float(-1) + pb::enc_float(0) + pb::enc_float(0) + pb::enc_float(0) +
                                     pb::enc_float(0) + pb::enc_float(1));
    CHECK_EQ(args_of("NudgeThrottle"), pb::enc_float(float(-KEY_THROTTLE_RATE * 0.1)));
    // VAB: only SAS / RCS, through the board's editor navigation
    CHECK_EQ(send_command(c, {Command::ToggleSas}, 0, "vab"), "SAS pressed in the VAB (next editor screen)");
    CHECK_EQ(args_of("Press"), pb::enc_string("SAS"));
    CHECK_EQ(send_command(c, {Command::ToggleRcs}, 0, "vab"), "RCS pressed in the VAB (previous editor screen)");
    CHECK_EQ(args_of("Press"), pb::enc_string("RCS"));
    // Elsewhere nothing reaches the game
    {
        std::lock_guard<std::mutex> lock(m);
        calls.clear();
    }
    CHECK(send_command(c, {Command::Stage}, 0, "vab").find("ignored") != std::string::npos);
    CHECK(send_command(c, {Command::ToggleSas}, 0, "spacecenter").find("ignored") != std::string::npos);
    {
        std::lock_guard<std::mutex> lock(m);
        CHECK(calls.empty());
    }
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
        fd = net::open_socket(SOCK_STREAM);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
        listen(fd, 8);
        socklen_t len = sizeof a;
        getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
    }
    ~SilentServer() { net::close_socket(fd); }
    net::socket_t fd;
    int port;
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

namespace {
/// The pad vessel with the Mun as its target body and a Mun encounter ahead on the current orbit (object 2).
struct TargetServer : PadServer {
    std::atomic<bool> broken_approach{false};
    void counted(const std::string& proc) {
        std::lock_guard<std::mutex> lock(mutex);
        ++counts[proc];
    }
    FakeResult target(const std::string& proc, const std::vector<std::string>& args) {
        auto id = [&](size_t i) { return args.size() > i ? pb::dec_object(args[i]) : 0; };
        const uint64_t MUN = 30, MUN_ORBIT = 31, NEXT = 41, APPROACH = 50;
        if (proc == "get_TargetBody") return {pb::enc_object(MUN)};
        if (proc == "CelestialBody_get_Orbit") return id(0) == MUN ? FakeResult{pb::enc_object(MUN_ORBIT)} : FakeResult{"", true};
        if (proc == "Orbit_get_Body") return {pb::enc_object(id(0) == NEXT ? MUN : 3)};
        if (id(0) == MUN) {
            if (proc == "CelestialBody_get_Name") return {pb::enc_string("Mun")};
            if (proc == "CelestialBody_get_EquatorialRadius") return {pb::enc_double(200000)};
            if (proc == "CelestialBody_get_GravitationalParameter") return {pb::enc_double(6.5138398e10)};
            if (proc == "CelestialBody_get_SphereOfInfluence") return {pb::enc_double(2429559)};
        }
        if (proc == "CelestialBody_get_SphereOfInfluence") return {pb::enc_double(KERBIN_SOI)};
        if (proc == "Orbit_get_TimeToSOIChange") return {pb::enc_double(id(0) == 2 ? 600 : NAN)};
        if (proc == "Orbit_get_NextOrbit") return id(0) == 2 ? FakeResult{pb::enc_object(NEXT)} : FakeResult{"", true};
        if (proc == "Orbit_PositionAt" || proc == "Orbit_VelocityAt") {
            counted(proc);
            double ut = pb::dec_double(args.at(1));
            bool position = proc == "Orbit_PositionAt";
            if (id(0) == MUN_ORBIT) return {pb::enc_tuple(position ? std::vector<double>{12e6, 0, ut} : std::vector<double>{0, 0, 543})};
            return {pb::enc_tuple(position ? std::vector<double>{11e6, 0, ut} : std::vector<double>{0, 0, 900})};
        }
        if (proc == "Orbit_NextClosestApproach" || proc == "Orbit_TimeOfClosestApproach") {
            counted(proc);
            if (broken_approach) return {"", false, "No closest approach"};
            return {pb::enc_object(APPROACH)};
        }
        if (proc == "ClosestApproach_get_UT") return {pb::enc_double(1234)};
        if (proc == "ClosestApproach_Position") return {pb::enc_tuple({1e6, 0, 0})};
        if (proc == "ClosestApproach_TargetPosition") return {pb::enc_tuple({1e6, 0, 5000})};
        return answer(proc);
    }
};
}  // namespace

TEST(target_orbit_approach_and_encounter_are_read_at_low_rate_and_fail_alone) {
    TargetServer pad;
    FakeServer server([&](const std::string& proc, const std::vector<std::string>& args) { return pad.target(proc, args); });
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
    CHECK(wait_for([&] { auto s = src.latest(); return s->target_orbit && s->approach && !s->encounters.empty(); }));
    int before = pad.count("Orbit_NextClosestApproach");
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));  // ~15 samples at 10 Hz
    auto snap = src.latest();
    CHECK(pad.count("Orbit_NextClosestApproach") - before <= 3);  // about once a second, not every sample
    CHECK(snap->target_orbit.has_value());
    if (snap->target_orbit) {
        CHECK_EQ(snap->target_orbit->name, std::string("Mun"));
        CHECK(snap->target_orbit->body);
        CHECK_NEAR(snap->target_orbit->radius, 200000, 1e-9);
        CHECK_NEAR(snap->target_orbit->position.x, 12e6, 1e-9);
        CHECK_NEAR(snap->target_orbit->position.z, 1000, 1e-9);  // at the current UT
    }
    if (snap->approach) {
        CHECK_NEAR(snap->approach->ut, 1234, 1e-9);
        CHECK_NEAR(snap->approach->distance(), 5000, 1e-6);
        CHECK(!snap->approach->planned);
    }
    CHECK_EQ(snap->encounters.size(), size_t(1));
    if (!snap->encounters.empty()) {
        const Encounter& e = snap->encounters.front();
        CHECK_EQ(e.body, std::string("Mun"));
        CHECK(!e.planned);
        CHECK_NEAR(e.ut, 1600, 1e-9);                  // UT + time to the SOI change
        CHECK_NEAR(e.center.z, 1600, 1e-9);            // the Mun at the entry time
        CHECK_NEAR(e.position.x, 11e6 - 12e6, 1e-9);   // vessel minus Mun
        CHECK_NEAR(e.velocity.z, 900 - 543, 1e-9);
        CHECK_NEAR(e.soi, 2429559, 1e-9);
    }
    pad.broken_approach = true;  // only the closest approach disappears
    CHECK(wait_for([&] { return !src.latest()->approach.has_value(); }));
    snap = src.latest();
    src.stop();
    CHECK(snap->state == State::Flight && snap->target_orbit.has_value() && !snap->encounters.empty());
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

// ------------------------------------------------------------------------------------- boot splash state
namespace ksphmi {
std::ostream& operator<<(std::ostream& o, Step s) { return o << static_cast<int>(s); }
}  // namespace ksphmi

TEST(boot_clock_counts_minutes_seconds_and_tenths) {
    CHECK_EQ(boot_clock(0), std::string("T+ 00:00.0"));
    CHECK_EQ(boot_clock(75.36), std::string("T+ 01:15.3"));
    CHECK_EQ(boot_clock(6000), std::string("T+ 100:00.0"));
    CHECK_EQ(boot_clock(-3), std::string("T+ 00:00.0"));
}

TEST(boot_view_follows_the_boot_stages) {
    BootView v = boot_view(boot_demo_inputs("system", 2520, 1680));
    CHECK_EQ(v.rows[0].step, Step::Done);
    CHECK_EQ(v.rows[1].step, Step::Done);
    CHECK_EQ(v.rows[1].detail, std::string("HDMI 2520x1680 \xC2\xB7 /dev/fb0"));
    CHECK_EQ(v.rows[2].step, Step::Active);
    CHECK_EQ(v.rows[2].detail, std::string("Mounting filesystems"));
    CHECK_EQ(v.rows[3].step, Step::Pending);
    CHECK_EQ(v.rows[4].step, Step::Pending);
    CHECK_NEAR(v.progress, 0.35, 1e-9);
    CHECK_EQ(v.message, std::string("Starting system services"));
    CHECK(!v.failed && !v.done);
    CHECK_EQ(v.clock, std::string("T+ 00:07.8"));

    v = boot_view(boot_demo_inputs("network", 2520, 1680));
    CHECK_EQ(v.rows[2].step, Step::Done);
    CHECK_EQ(v.rows[2].detail, std::string("Filesystems and services up"));
    CHECK_EQ(v.rows[3].step, Step::Active);
    CHECK(v.rows[3].detail.find("waiting for address") != std::string::npos);
    CHECK_NEAR(v.progress, 0.62, 1e-9);
    CHECK_EQ(v.message, std::string("Connecting to the network"));

    v = boot_view(boot_demo_inputs("hmi", 2520, 1680));
    CHECK_EQ(v.rows[3].step, Step::Done);
    CHECK_EQ(v.rows[3].detail, std::string("wlan0 192.168.1.20"));
    CHECK_EQ(v.rows[4].step, Step::Active);
    CHECK_EQ(v.rows[4].detail, std::string("Starting ksphmi"));
    CHECK_NEAR(v.progress, 0.86, 1e-9);
    CHECK_EQ(v.message, std::string("Launching the flight display"));

    v = boot_view(boot_demo_inputs("ready", 2520, 1680));
    CHECK(v.done && !v.failed);
    CHECK_EQ(v.rows[4].step, Step::Done);
    CHECK_EQ(v.rows[4].detail, std::string("First frame drawn"));
    CHECK_NEAR(v.progress, 1.0, 1e-9);
    CHECK_EQ(v.message, std::string("Orbit reached"));
    CHECK(v.footer.find("ksphmi") == 0);
}

TEST(boot_view_gives_up_on_network_and_hmi_without_blocking) {
    BootInputs in = boot_demo_inputs("network", 1920, 1080);
    in.iface.clear();
    in.seconds = NETWORK_GIVE_UP - 0.1;
    CHECK_EQ(boot_view(in).rows[3].step, Step::Active);
    CHECK(boot_view(in).rows[3].detail.find("network") == 0);
    in.seconds = NETWORK_GIVE_UP;
    BootView v = boot_view(in);
    CHECK_EQ(v.rows[3].step, Step::Warn);
    CHECK(v.rows[3].detail.find("offline") != std::string::npos);
    CHECK_EQ(v.rows[4].step, Step::Active);  // the HMI starts without a network
    CHECK_NEAR(v.progress, 0.86, 1e-9);

    in = boot_demo_inputs("hmi", 1920, 1080);
    in.seconds = HMI_GIVE_UP - 0.1;
    CHECK(!boot_view(in).failed);
    in.seconds = HMI_GIVE_UP;
    v = boot_view(in);
    CHECK(v.failed && !v.done);
    CHECK_EQ(v.rows[4].step, Step::Failed);
    CHECK_EQ(v.rows[4].detail, std::string("ksphmi did not start"));
    CHECK(v.message.find("ssh ksphmi") == 0);
    CHECK(v.message.find("journalctl --user -u ksphmi") != std::string::npos);
    in.ready = true;  // a late HMI still ends the boot normally
    CHECK(boot_view(in).done && !boot_view(in).failed);

    // The system row is never skipped: a network that is up early is shown, the progress waits for the system.
    in = boot_demo_inputs("system", 1920, 1080);
    in.address = "10.0.0.3";
    v = boot_view(in);
    CHECK_EQ(v.rows[3].step, Step::Done);
    CHECK_EQ(v.rows[4].step, Step::Pending);
    CHECK_NEAR(v.progress, 0.35, 1e-9);

    bool threw = false;
    try {
        boot_demo_inputs("nope", 10, 10);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(ready_file_follows_xdg_runtime_dir) {
    const char* old = std::getenv("XDG_RUNTIME_DIR");
    const std::string saved = old ? old : "";
    check::unset_env("XDG_RUNTIME_DIR");
#if defined(_WIN32)
    const std::string fallback = ready_file_path();  // %TEMP%\ksphmi.ready: no systemd runtime directory
    CHECK(fallback.size() > 13 && fallback.compare(fallback.size() - 13, 13, "\\ksphmi.ready") == 0);
#else
    CHECK_EQ(ready_file_path(), "/run/user/" + std::to_string(getuid()) + "/ksphmi.ready");
#endif
    const std::string dir = check::make_temp_dir("ksphmi-ready");
    CHECK(!dir.empty());
    check::set_env("XDG_RUNTIME_DIR", dir);
    CHECK_EQ(ready_file_path(), dir + "/ksphmi.ready");
    CHECK(!file_exists(ready_file_path()));
    CHECK(write_ready_file());
    CHECK(file_exists(ready_file_path()));
    remove_ready_file();
    CHECK(!file_exists(ready_file_path()));
    check::remove_dir(dir);
    if (old) check::set_env("XDG_RUNTIME_DIR", saved);
    else check::unset_env("XDG_RUNTIME_DIR");
}

TEST(boot_probes_answer_without_systemd) {
    CHECK(boot_seconds() > 0);
    CHECK(!hmi_process_running() || true);  // must not crash; the answer depends on the machine
    CHECK(!unit_active("ksphmi-no-such-unit.target"));
    NetInterface n = primary_interface();
    CHECK(n.address.empty() || !n.name.empty());
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

TEST(ship_report_twr_levels_follow_the_vab_thresholds) {
    // launch stage = first in firing order with delta-v; the clamp-only stage before it is not judged
    auto twr_report = [](double launch, double upper, double last) {
        return *parse_ship_report(fmt::str("v=1 rev=1\nst=4,0,0,0.00,0,clamps:1\nst=3,1500,1700,%.2f,60\n"
                                           "st=2,1800,1900,%.2f,90\nst=1,900,950,%.2f,40\nst=0,0,0,0.00,0,chutes:1\n",
                                           launch, upper, last));
    };
    const TwrLimits limits;  // 1.0 red / 1.2 amber at launch, 0.8 amber above
    ShipReport r = twr_report(0.99, 0.79, 0.80);
    CHECK_EQ(launch_stage_index(r), 1);
    CHECK(twr_level(r, 0, limits) == TwrLevel::None);  // no delta-v: no thrust to judge
    CHECK(twr_level(r, 1, limits) == TwrLevel::Bad);
    CHECK(twr_level(r, 2, limits) == TwrLevel::Warn);
    CHECK(twr_level(r, 3, limits) == TwrLevel::Ok);    // 0.80 is not below 0.8
    CHECK(twr_level(r, 4, limits) == TwrLevel::None);
    CHECK(twr_level(r, 9, limits) == TwrLevel::None);
    CHECK(launch_twr_level(r, limits) == TwrLevel::Bad);
    CHECK(upper_twr_level(r, limits) == TwrLevel::Warn);
    r = twr_report(1.00, 0.80, 2.5);  // the limits themselves pass the lower bands
    CHECK(launch_twr_level(r, limits) == TwrLevel::Warn);
    CHECK(upper_twr_level(r, limits) == TwrLevel::Ok);
    r = twr_report(1.20, 1.0, 1.0);
    CHECK(launch_twr_level(r, limits) == TwrLevel::Ok);
    // configurable: a stricter pad rule turns the same ship red
    TwrLimits strict{1.3, 1.5, 1.1};
    CHECK(launch_twr_level(r, strict) == TwrLevel::Bad);
    CHECK(upper_twr_level(r, strict) == TwrLevel::Warn);
    // the shared sample: launch 1.63 fine, last stage 0.62 amber; a pending report judges nothing
    auto sample = parse_ship_report(golden_ship_report());
    CHECK(sample && launch_twr_level(*sample, limits) == TwrLevel::Ok && twr_level(*sample, 2, limits) == TwrLevel::Warn);
    sample->delta_v_pending = true;
    CHECK(launch_twr_level(*sample, limits) == TwrLevel::None && upper_twr_level(*sample, limits) == TwrLevel::None);
}

TEST(config_vab_twr_thresholds_are_validated) {
    Config cfg;
    std::ifstream in(std::string(KSPHMI_SOURCE_DIR) + "/config.toml");
    std::stringstream buf;
    buf << in.rdbuf();
    apply_toml(cfg, parse_toml(buf.str()));
    CHECK_EQ(cfg.vab.twr.launch_bad, 1.0);
    CHECK_EQ(cfg.vab.twr.launch_warn, 1.2);
    CHECK_EQ(cfg.vab.twr.upper_warn, 0.8);
    apply_toml(cfg, parse_toml("[vab]\nlaunch_twr_min = 1.1\nlaunch_twr_warn = 1.4\nupper_twr_warn = 0.5\n"));
    CHECK_EQ(cfg.vab.twr.launch_bad, 1.1);
    CHECK_EQ(cfg.vab.twr.launch_warn, 1.4);
    CHECK_EQ(cfg.vab.twr.upper_warn, 0.5);
    for (const char* bad : {"[vab]\nlaunch_twr_min = 0\n", "[vab]\nupper_twr_warn = -1\n", "[vab]\nlaunch_twr_warn = \"high\"\n",
                            "[vab]\nupper_twr_warn = 1000\n", "[vab]\nlaunch_twr_min = 2\nlaunch_twr_warn = 1.5\n"}) {
        Config c2 = cfg;
        bool threw = false;
        try {
            apply_toml(c2, parse_toml(bad));
        } catch (const std::runtime_error& e) {
            threw = std::string(e.what()).find("vab.") != std::string::npos;
        }
        CHECK(threw);
        CHECK_EQ(c2.vab.twr.launch_bad, 1.1);  // a rejected file leaves the thresholds as they were
    }
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
    CHECK_EQ(static_cast<int>(cfg.vab.destinations.size()), MAX_DESTINATIONS);
    CHECK_EQ(cfg.vab.atmosphere_dv, 1200.0);
    // community delta-v map, cumulative from the pad, no aerobraking (config.toml comment, docs/hmi.rst)
    CHECK_EQ(by_key["kerbin_orbit"], 3400.0);
    CHECK_EQ(by_key["mun_orbit"], 4570.0);
    CHECK_EQ(by_key["mun_landing"], 5150.0);
    CHECK_EQ(by_key["minmus_orbit"], 4490.0);
    CHECK_EQ(by_key["minmus_landing"], 4670.0);
    CHECK_EQ(by_key["duna_orbit"], 5090.0);
    CHECK_EQ(by_key["duna_landing"], 6540.0);
    // every body other than Kerbin has an orbit and, except Eve (aerobraking), a landing for the app-driven target
    for (const char* body : {"mun", "minmus", "duna", "ike", "dres"})
        CHECK(by_key.count(std::string(body) + "_orbit") && by_key.count(std::string(body) + "_landing"));
    for (size_t i = 1; i < cfg.vab.destinations.size(); ++i)
        CHECK(cfg.vab.destinations[i - 1].delta_v <= cfg.vab.destinations[i].delta_v);  // sorted for display
    // a local file adds a destination, changes another and picks it as the target
    apply_toml(cfg, parse_toml("[vab]\ntarget = \"duna_landing\"\natmosphere_dv = 900\n[vab.destinations]\n"
                               "duna_landing = 9000\nmun_orbit = 4300\n"));
    CHECK_EQ(cfg.vab.target, "duna_landing");
    CHECK_EQ(cfg.vab.atmosphere_dv, 900.0);
    CHECK_EQ(cfg.vab.destinations.back().key, "duna_landing");
    by_key.clear();
    for (const auto& d : cfg.vab.destinations) by_key[d.key] = d.delta_v;
    CHECK_EQ(by_key["mun_orbit"], 4300.0);
    // dotted table names are parsed part by part
    CHECK_EQ(parse_toml("[ vab . destinations ]\nx = 1\n").count("vab.destinations.x"), size_t(1));
    for (const char* bad : {"[vab]\ntarget = \"nowhere\"\n", "[vab.destinations]\nmoon = 0\n",
                            "[vab.destinations]\nmoon = -5\n", "[vab.destinations]\nmoon = \"far\"\n",
                            "[vab.destinations]\nMoon = 100\n", "[vab.destinations]\nmoon = 1e9\n",
                            "[vab]\natmosphere_dv = -1\n", "[vab]\natmosphere_dv = 100001\n", "[vab]\natmosphere_dv = nan\n",
                            "[vab]\natmosphere_dv = \"low\"\n"}) {
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

namespace {
std::optional<ShipReport> fixture_report(const std::string& name) {
    std::ifstream in(std::string(KSPHMI_SOURCE_DIR) + "/tests/data/" + name);
    std::stringstream buf;
    buf << in.rdbuf();
    return parse_ship_report(buf.str());
}
}  // namespace

// The live Kerbal X (KSP 1.12.5, VAB): the app's total stays KSP's, the mission delta-v counts the ascent at
// Kerbin sea level up to [vab] atmosphere_dv and the upper stages in vacuum.
TEST(mission_delta_v_counts_the_ascent_at_sea_level_and_upper_stages_in_vacuum) {
    auto asl = fixture_report("ship_report_kerbal_x.txt");
    CHECK(asl && asl->has_kerbin_asl && asl->stages.size() == 8);
    CHECK_EQ(asl->total_delta_v(), 4328.0);
    CHECK_EQ(asl->stages.at(5).delta_v_kerbin_asl, 631.0);
    MissionDeltaV m = mission_delta_v(*asl, 1200);
    CHECK_EQ(m.total, 257.0 + 304 + 372 + 2764 + 2452);  // 6149
    CHECK(m.kerbin_asl && m.per_stage.size() == 8 && m.per_stage.at(5) == 2452 && m.per_stage.at(3) == 2764);
    CHECK_EQ(mission_basis(*asl, m), "ASCENT KERBIN ASL, UPPER STAGES VAC");
    CHECK_EQ(events_text(asl->stages.back()), "CHUTE");
    // another body in the app (Duna, vacuum): same mission delta-v from the kasl= line
    auto duna = fixture_report("ship_report_kerbal_x_duna.txt");
    CHECK(duna && duna->total_delta_v() == 6527);  // KSP shows 6528 (it sums unrounded stages)
    CHECK_EQ(mission_delta_v(*duna, 1200).total, 6149.0);
    // an older plugin without kasl=: the app's values stand in for the ascent, and the basis says so
    ShipReport old = *duna;
    old.has_kerbin_asl = false;
    for (auto& st : old.stages) st.delta_v_kerbin_asl = std::numeric_limits<double>::quiet_NaN();
    MissionDeltaV mo = mission_delta_v(old, 1200);
    CHECK(!mo.kerbin_asl && mo.total == 6527);
    CHECK(mission_basis(old, mo).find("NO KERBIN ASL DATA") != std::string::npos);
    // ...but Kerbin sea level in the app is the ascent figure itself
    ShipReport old_asl = *asl;
    old_asl.has_kerbin_asl = false;
    for (auto& st : old_asl.stages) st.delta_v_kerbin_asl = std::numeric_limits<double>::quiet_NaN();
    CHECK(mission_delta_v(old_asl, 1200).kerbin_asl && mission_delta_v(old_asl, 1200).total == 6149);
    // atmosphere_dv 0: every stage in vacuum; huge: everything at sea level (the app's total)
    CHECK_EQ(mission_delta_v(*asl, 0).total, asl->total_delta_v_vac());
    CHECK_EQ(mission_delta_v(*asl, 100000).total, 4328.0);
    // pending: nothing
    ShipReport pending = *asl;
    pending.delta_v_pending = true;
    CHECK_EQ(mission_delta_v(pending, 1200).total, 0.0);
    // malformed kasl= is rejected like any known field
    CHECK(!parse_ship_report("v=1 rev=1\nst=1,1,1,1,1\nkasl=1:x\n"));
    CHECK(!parse_ship_report("v=1 rev=1\nst=1,1,1,1,1\nkasl=12\n"));
}

TEST(vab_target_follows_the_delta_v_app_body) {
    std::vector<Destination> d = {{"kerbin_orbit", 3400}, {"mun_orbit", 4570}, {"mun_landing", 5150},
                                  {"duna_orbit", 5090}, {"eve_orbit", 5850}, {"ike_landing", 5330}};
    auto pick = [&](const std::string& sit) {
        ShipReport r;
        r.situation = sit;
        TargetChoice t = choose_target(&r, d, "mun_landing");
        return (t.destination ? t.destination->key : std::string("-")) + (t.from_app ? "@app" : "@config");
    };
    CHECK_EQ(pick("kerbin_asl"), "mun_landing@config");
    CHECK_EQ(pick("kerbin_vac"), "mun_landing@config");
    CHECK_EQ(pick("mun_asl"), "mun_landing@app");
    CHECK_EQ(pick("mun_alt"), "mun_landing@app");
    CHECK_EQ(pick("mun_vac"), "mun_orbit@app");
    CHECK_EQ(pick("duna_vac"), "duna_orbit@app");
    CHECK_EQ(pick("duna_asl"), "duna_orbit@app");  // no duna_landing configured: its orbit
    CHECK_EQ(pick("eve_asl"), "eve_orbit@app");
    CHECK_EQ(pick("ike_vac"), "ike_landing@app");
    CHECK_EQ(pick("jool_vac"), "mun_landing@config");  // nothing for Jool: the configured target
    CHECK_EQ(pick("odd"), "mun_landing@config");
    CHECK(choose_target(nullptr, d, "nowhere").destination == nullptr);
    ShipReport r;
    r.situation = "sun_vac";
    CHECK(r.situation_body() == "sun" && r.situation_mode() == "vac");
    r.situation = "vac";
    CHECK(r.situation_body().empty() && r.situation_mode().empty());
    // an empty craft (New in the editor) is not "calculating"
    auto empty = parse_ship_report("v=1 rev=25 facility=vab name=Untitled%20Space%20Craft sit=kerbin_vac\n"
                                   "mass=0.00 dry=0.00 cost=0 parts=0 crew=0 seats=0 size=0.0,0.0 engines=0\ndv=pending\n");
    CHECK(empty && empty->empty_craft() && empty->delta_v_pending);
    CHECK(!fixture_report("ship_report_kerbal_x.txt")->empty_craft());
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

TEST(editor_report_contract_and_page_models) {
    std::ifstream file(std::string(KSPHMI_SOURCE_DIR) + "/tests/data/editor_report_v1.txt");
    std::stringstream text;
    text << file.rdbuf();
    auto report = parse_ship_report(text.str());
    CHECK(report.has_value());
    CHECK(report->has_editor);
    CHECK_EQ(report->editor_mode, "actions");
    CHECK_EQ(report->selected_group, "custom1");
    CHECK_EQ(report->actions.size(), size_t(1));
    CHECK_EQ(report->manifest.size(), size_t(3));
    CHECK_EQ(report->manifest[0].name, "Valentina Kerman");
    CHECK(report->manifest[1].name.empty());
    CHECK_EQ(report->inventories.size(), size_t(1));
    CHECK_EQ(report->cargo.size(), size_t(2));
    CHECK_EQ(report->stages[0].items[0].count, 3);
    CHECK(stage_detail_lines(report->stages[0])[0].find("Engine x3") != std::string::npos);
    auto actions = editor_page(*report);
    CHECK_EQ(actions.left.size(), size_t(17));
    CHECK_EQ(actions.right.back().detail, "Toggle antenna");
    report->selected_group = "sas";
    actions = editor_page(*report);
    CHECK_EQ(actions.right[1].detail, "Toggle stability assist");
    report->editor_mode = "crew";
    auto crew = editor_page(*report);
    CHECK_EQ(crew.metrics[0].second, "1");
    CHECK_EQ(crew.metrics[2].second, "2");
    report->editor_mode = "cargo";
    auto cargo = editor_page(*report);
    CHECK_EQ(cargo.metrics[0].second, "3");
    CHECK_EQ(cargo.metrics[1].second, "22.0");
    CHECK_EQ(cargo.metrics[2].second, "84.0");
    report->inventories[0].volume_capacity = -1;
    CHECK(editor_page(*report).right[0].detail.find("NO LIMIT") != std::string::npos);
    report->inventories[0].volume_capacity.reset();
    CHECK(editor_page(*report).right[0].detail.find("UNKNOWN") != std::string::npos);
    CHECK(parse_ship_report(golden_ship_report())->editor_mode == "construction");
    CHECK(rejects(golden_ship_report() + "editor=invalid,\n", "bad editor"));
    CHECK(rejects(golden_ship_report() + "seat=1,Pod,0,A,Pilot,NaN\n", "bad seat"));
    CHECK(rejects(golden_ship_report() + "inv=1,Pod,3,inf,1,2,3\n", "bad inv"));
    CHECK(rejects(golden_ship_report() + "sp=999,engines,Engine,1\n", "without a stage"));
    CHECK(rejects(golden_ship_report() + "cargo=missing,0,Kit,1,1,1\n", "without an inventory"));
}

TEST(editor_pagination_visits_every_row_without_unbounded_casts) {
    CHECK_EQ(page_slice(0, 0, 0).pages, size_t(1));
    CHECK_EQ(page_slice(21, 10, 0).end, size_t(10));
    CHECK_EQ(page_slice(21, 10, 8).begin, size_t(10));
    CHECK_EQ(page_slice(21, 10, 16).end, size_t(21));
    CHECK_EQ(page_slice(21, 10, 24).begin, size_t(0));
    CHECK_EQ(page_slice(21, 10, std::numeric_limits<double>::infinity()).page, size_t(0));
    CHECK(page_slice(21, 10, 1e300).page < 3);
}

int main(int argc, char** argv) {
    log::set_quiet(true);
    return RUN_TESTS(argc, argv);
}
