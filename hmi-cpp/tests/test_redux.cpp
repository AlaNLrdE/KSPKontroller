// KSP 2 Redux source: wire decoding, facility scenes, ship/VESSEL text replies, remote keyboard requests and the
// nonce matching of the UDP exchange (loopback only, no game).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#include "check.hpp"
#include "redux_source.hpp"
#include "resource_ui.hpp"
#include "scenes.hpp"

using namespace ksphmi;

namespace {

const std::string PACKET =
    "KSP2HMI1 42 1 466c6967687456696577 54657374 4b657262696e 4f72626974696e67 123 600000 70000 80000 250000 80000 20 30 2 0.1 "
    "2000 765000 1.5 2200 100 2 0 90 0 1 3 1 0 1";
const std::string EXTENSION =
    " 255 0 1 0 1 0 0 0 1 0 1 0 0 0 0 1 0 -1 0 1 0 0 0.25 2200 7 150 60 2 2 4d657468616e65 5 10 4f786964697a6572 15 30";

std::string v2() {
    std::string p = PACKET;
    p[7] = '2';
    return p;
}

std::string read_file(const std::string& name) {
    std::ifstream in(std::string(KSPHMI_SOURCE_DIR) + "/tests/data/" + name, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

bool throws(const std::function<void()>& f) {
    try {
        f();
    } catch (...) {
        return true;
    }
    return false;
}

}  // namespace

TEST(redux_v1_mapping_and_validation) {
    auto s = decode_redux(PACKET, "42", 100);
    CHECK(s.state == State::Flight);
    CHECK_EQ(s.vessel, std::string("Test"));
    CHECK_EQ(s.body, std::string("Kerbin"));
    CHECK(s.altitude == 80000 && s.inclination == 2 && s.received == 100);
    CHECK(!s.nav && !s.systems && !s.orbit_state);
    for (const auto& p : {PACKET + " extra", PACKET.substr(0, PACKET.size() - 2), std::string("bad"), std::string(4097, 'x')})
        CHECK(throws([&] { decode_redux(p, "42", 100); }));
    CHECK(throws([&] { decode_redux(PACKET, "41", 100); }));
}

TEST(redux_v2_navigation_and_native_resources) {
    auto e = decode_redux(v2() + EXTENSION, "42", 100);
    CHECK(e.nav && e.systems && e.systems->native_resources);
    CHECK_EQ(e.systems->resources.size(), size_t(2));
    CHECK_EQ(e.systems->resources[0].name, std::string("Methane"));
    CHECK(e.systems->resources[0].amount == 5 && e.systems->resources[0].capacity == 10);
    CHECK(e.nav->throttle == .25 && e.nav->speed == 2200 && e.nav->sas && e.nav->rcs);
    CHECK_EQ(e.nav->sas_mode, std::string("radial"));
    CHECK(e.nav->node_time == 60 && e.systems->current_stage == 2 && e.nav->prograde && e.nav->maneuver);
    auto axes = basis(e.nav->rotation);
    CHECK_NEAR(axes.forward.y, 1, 1e-9);
    CHECK_NEAR(axes.up.x, 1, 1e-9);
    CHECK_NEAR(axes.right.z, 1, 1e-9);
    const Vec3 directions[][2] = {{{0, 0, 1}, {1, 0, 0}}, {{1, 0, 0}, {0, 1, 0}}, {{-1, 0, 0}, {0, 0, 1}}, {{0, 1, 0}, {0, 0, 1}}};
    for (const auto& d : directions) {
        auto a = basis(redux_rotation(d[0], d[1]));
        CHECK(dot(a.forward, d[0]) > .999999 && dot(a.up, d[1]) > .999999);
    }
    CHECK(hmi_resource_labels(e, RESOURCE_LABELS.data()) == (std::vector<std::string>{"METHANE", "OXIDIZER"}));
    CHECK_EQ(hmi_resource_names(decode_redux(PACKET, "42", 100)).size(), size_t(6));  // KSP 1 slots unchanged
    auto unavailable = decode_redux(v2() + EXTENSION.substr(0, EXTENSION.find(" 2 2 4d")) + " 2 -1", "42", 100);
    CHECK(unavailable.nav && !unavailable.systems);
    std::string unknown_mode = EXTENSION;
    unknown_mode.replace(unknown_mode.find("2200 7 150"), 10, "2200 0 150");
    CHECK_EQ(decode_redux(v2() + unknown_mode, "42", 100).nav->sas_mode, std::string("unknown"));
    CHECK(throws([] { redux_rotation({1, 0, 0}, {1, 0, 0}); }));
    CHECK(throws([] { decode_redux(v2() + EXTENSION + " extra", "42", 100); }));
}

TEST(redux_facility_scenes) {
    auto at_scene = [](const std::string& hex) {
        std::string p = PACKET;
        p.replace(p.find(" 1 466c"), 3, " 0 ");
        return decode_redux(p.replace(p.find("466c6967687456696577"), 20, hex), "42", 100);
    };
    auto ksc = at_scene("4b657262616c537061636543656e746572");
    auto vab = at_scene("56656869636c65417373656d626c794275696c646572");
    auto map = at_scene("4d6170334456696577");
    CHECK_EQ(ksc.scene, std::string("space_center"));
    CHECK_EQ(vab.scene, std::string("vab"));
    CHECK_EQ(map.scene, std::string("tracking_station"));
    CHECK(ksc.state == State::NoVessel && find_view(ksc.scene) && find_view(vab.scene) && find_view(map.scene));
    CHECK_EQ(redux_scene("Map3DView", true), std::string("Map3DView"));
}

TEST(redux_ship_report) {
    // Same independent golden fixture as the workbench's C# encoder test.
    const std::string fixture = "v=1 rev=57 facility=vab name=Kerbal%20X sit=kerbin_asl\nmass=18.4 dry=6.1 parts=42 size=11.2,3.8 engines=5\n"
                                "st=2,1720,1960,1.63,48\nst=1,1890,2010,2.5,60\nkasl=2:1720,1:1890\n";
    auto ship = decode_redux_ship("KSP2SHIP 9\n" + fixture, "9");
    CHECK(ship && ship->name == "Kerbal X" && ship->revision == 57 && ship->parts && *ship->parts == 42);
    CHECK(ship->stages.size() == 2 && ship->stages[0].stage == 2 && ship->stages[0].twr == 1.63 && ship->has_kerbin_asl);
    CHECK_NEAR(ship->total_delta_v(), 3610, 1e-9);
    CHECK(!decode_redux_ship("KSP2SHIP 9\n-", "9"));
    auto actions = decode_redux_ship("KSP2SHIP 9\nv=1 rev=4 facility=vab name=x sit=kerbin_asl\nparts=2\neditor=actions,\n"
                                     "ac=brakes,Pod%2C%20Mk1,Toggle%20Brakes\n", "9");
    CHECK(actions && actions->has_editor && actions->editor_mode == "actions" && actions->selected_group.empty());
    CHECK(actions->actions.size() == 1 && actions->actions[0].part == "Pod, Mk1");
    for (const auto& p : {"KSP2SHIP 8\n" + fixture, std::string("KSP2SHIP 9\nv=2 rev=1"), std::string("KSP2SHIP 9 ") + fixture})
        CHECK(throws([&] { decode_redux_ship(p, "9"); }));
}

TEST(redux_vessel_view_text_uses_the_shared_fixtures) {
    const std::string shape_text = read_file("vessel_shape_v1.txt"), state_text = read_file("vessel_state_v1.txt");
    CHECK(!shape_text.empty() && !state_text.empty());
    auto shape = decode_redux_text("KSP2VSHP 5\n" + shape_text, "KSP2VSHP", "5");
    auto state = decode_redux_text("KSP2VSTA 6\n" + state_text, "KSP2VSTA", "6");
    CHECK(shape && state);
    auto view = parse_vessel_shape(*shape);
    CHECK(view && view->parts.size() == 3);
    CHECK(apply_vessel_state(*view, *state));
    CHECK(view->parts[1].fuel == 48 && view->parts[2].heat == 93);
    CHECK(!decode_redux_text("KSP2VSTA 6\n-", "KSP2VSTA", "6"));
    CHECK(throws([&] { decode_redux_text("KSP2VSTA 7\n" + state_text, "KSP2VSTA", "6"); }));
    CHECK(throws([&] { decode_redux_text("KSP2VSHP 6\n" + state_text, "KSP2VSTA", "6"); }));
}

TEST(redux_keyboard_requests) {
    CHECK_EQ(redux_key_request({Command::ActionGroup, 0}, "7"), std::string("KSP2KEY 7 group 10"));
    CHECK_EQ(redux_key_request({Command::ActionGroup, 3}, "7"), std::string("KSP2KEY 7 group 3"));
    CHECK_EQ(redux_key_request({Command::SetBrakes, 1}, "7"), std::string("KSP2KEY 7 brakes 1"));
    CHECK_EQ(redux_key_request({Command::SasMode, 2}, "7"), std::string("KSP2KEY 7 sasmode 2"));
    CHECK_EQ(redux_key_request({Command::ToggleMap}, "7"), std::string("KSP2KEY 7 map 0"));
    CHECK_EQ(redux_key_request({Command::ThrottleFull}, "7"), std::string("KSP2KEY 7 throttle_full 0"));
    Held h;
    h.pitch = -1;
    h.roll = .5f;
    h.throttle = -1;
    CHECK_EQ(redux_hold_request(h, "8"), std::string("KSP2HOLD 8 -1 0 0.5 0 0 0 -1"));
    CHECK_EQ(decode_redux_ack("KSP2KEY 7 queued", "KSP2KEY", "7"), std::string("queued"));
    CHECK_EQ(decode_redux_ack("KSP2HOLD 8 disabled", "KSP2HOLD", "8"), std::string("disabled"));
    CHECK(throws([] { decode_redux_ack("KSP2KEY 6 queued", "KSP2KEY", "7"); }));
    CHECK(throws([] { decode_redux_ack("KSP2KEY 7 ", "KSP2KEY", "7"); }));
    CHECK(throws([] { decode_redux_ack("KSP2KEY 7 Queued!", "KSP2KEY", "7"); }));
}

TEST(redux_late_replies_do_not_shift_nonces) {
    // Loopback only: a reply delayed past one poll must not shift every later reply by one nonce.
    net::Fd server(net::open_socket(SOCK_DGRAM)), client(net::open_socket(SOCK_DGRAM));
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(at);
    CHECK(server && client);
    CHECK(::bind(server.get(), reinterpret_cast<sockaddr*>(&at), sizeof(at)) == 0);
    CHECK(::getsockname(server.get(), reinterpret_cast<sockaddr*>(&at), &length) == 0);
    CHECK(::connect(client.get(), reinterpret_cast<sockaddr*>(&at), sizeof(at)) == 0);
    net::set_nonblocking(client.get(), true);
    sockaddr_in self{};
    socklen_t self_length = sizeof(self);
    CHECK(::getsockname(client.get(), reinterpret_cast<sockaddr*>(&self), &self_length) == 0);
    auto reply_for = [](const std::string& nonce) {
        std::string p = PACKET;
        return p.replace(9, 2, nonce);
    };
    const net::socket_t fd = server.get();
    auto serve = [fd](std::vector<std::string> replies) {
        return std::thread([fd, replies] {
            char buffer[64];
            sockaddr_in from{};
            socklen_t size = sizeof(from);
            if (::recvfrom(fd, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &size) <= 0) return;
            for (const auto& r : replies) ::sendto(fd, r.data(), int(r.size()), 0, reinterpret_cast<sockaddr*>(&from), size);
        });
    };
    auto late = serve({reply_for("1")});
    CHECK(redux_exchange(client.get(), "KSP2HMI1 1", "1", 0.5) != std::nullopt);
    late.join();
    // Queue two stale replies, then answer nonce 3 only after an unrelated late one.
    const std::string stale = reply_for("1");
    for (int i = 0; i < 2; ++i)
        CHECK(::sendto(fd, stale.data(), int(stale.size()), 0, reinterpret_cast<sockaddr*>(&self), self_length) >= 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto mixed = serve({reply_for("2"), reply_for("3")});
    auto got = redux_exchange(client.get(), "KSP2HMI1 3", "3", 0.5);
    mixed.join();
    CHECK(got && !got->version2 && got->snapshot.vessel == "Test");
    CHECK(!redux_exchange(client.get(), "KSP2HMI1 4", "4", 0.05));
    // A keyboard acknowledgement matches its own nonce the same way.
    auto ack = serve({"KSP2KEY 4 queued", "KSP2KEY 5 queued"});
    auto status = redux_ack_exchange(client.get(), "KSP2KEY 5 stage 0", "KSP2KEY", "5", 0.5);
    ack.join();
    CHECK(status && *status == "queued");
}

TEST(redux_unresolved_name_keeps_the_source_alive) {
    // A PC name that does not resolve yet (boot, DHCP) once ended the worker; it now waits and retries.
    KrpcSource::Options o;
    o.address = "no-such-host.invalid";
    o.discovery = false;
    ReduxSource src(o, false);
    src.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    auto snap = src.latest();
    src.stop();
    CHECK(snap && snap->state == State::Connecting);
    CHECK(snap && snap->detail.find("Redux") != std::string::npos);
}

// ------------------------------------------------------------------------------------------- KSP2ORB1 orbit report
namespace {

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

std::string golden() { return trimmed(read_file("redux_orb1_v1.txt")); }
std::string maneuver_only() { return trimmed(read_file("redux_orb1_maneuver_v1.txt")); }

std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> w;
    std::istringstream in(s);
    for (std::string t; in >> t;) w.push_back(t);
    return w;
}

std::string joined(const std::vector<std::string>& w) {
    std::string s;
    for (const auto& t : w) s += (s.empty() ? "" : " ") + t;
    return s;
}

/// The golden body with token `index` replaced.
std::string with_token(size_t index, const std::string& value) {
    auto w = words(golden());
    w.at(index) = value;
    return joined(w);
}

bool same(const Vec3& a, const Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

/// A loopback UDP socket pair: `server` bound to an ephemeral port, `client` connected to it.
struct Loopback {
    net::Fd server{net::open_socket(SOCK_DGRAM)}, client{net::open_socket(SOCK_DGRAM)};
    sockaddr_in at{};
    Loopback() {
        at.sin_family = AF_INET;
        at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t length = sizeof(at);
        CHECK(server && client);
        CHECK(::bind(server.get(), reinterpret_cast<sockaddr*>(&at), sizeof(at)) == 0);
        CHECK(::getsockname(server.get(), reinterpret_cast<sockaddr*>(&at), &length) == 0);
        CHECK(::connect(client.get(), reinterpret_cast<sockaddr*>(&at), sizeof(at)) == 0);
        net::set_nonblocking(client.get(), true);
    }
    int port() const { return ntohs(at.sin_port); }
};

}  // namespace

TEST(redux_orbit_golden_body_maps_to_the_snapshot_types) {
    auto o = parse_redux_orbit(golden());
    CHECK(o.has_value());
    if (!o) return;
    CHECK(o->ut == 1000.5 && o->body == "Kerbin" && o->mu == 3531600000000.0);
    CHECK(same(o->position, {1300000, 0, 0}) && same(o->velocity, {0, 1648.2, 0}));
    CHECK(!o->maneuver);
    CHECK(o->target && o->target->name == "Mun" && o->target->body && o->target->radius == 200000);
    CHECK(o->target && same(o->target->position, {-9000000, 7937253.9, 0}) && same(o->target->velocity, {-358.8, -406.8, 0}));
    CHECK(o->approach && o->approach->ut == 1520.4 && !o->approach->planned);
    CHECK(o->approach && same(o->approach->vessel, {-1288000, 0, 1300000}) && same(o->approach->target, {0, -8400000, 8000000}));
    CHECK_EQ(o->encounters.size(), size_t(1));
    if (o->encounters.size() == 1) {
        const Encounter& e = o->encounters[0];
        CHECK(e.body == "Mun" && e.radius == 200000 && e.mu == 65138000000.0 && e.soi == 2429559 && e.ut == 1500.25 && !e.planned);
        CHECK(same(e.center, {-8000000, 8000000, 0}) && same(e.position, {1500000, -1800000, 0}) && same(e.velocity, {-300, -100, 0}));
    }
    Snapshot s;
    const Axes axes = view_axes(o->position, o->velocity);
    apply_redux_orbit(s, *o, axes);
    CHECK(s.orbit_state && s.orbit_state->mu == o->mu && same(s.orbit_state->position, o->position));
    CHECK(s.orbit_state && same(s.orbit_state->axes.x, axes.x) && same(s.orbit_state->axes.y, axes.y));
    CHECK(!s.maneuver_orbit && s.target_orbit && s.approach && s.encounters.size() == 1);
    // A Zup frame with the vessel in the x/y plane: the view plane is that plane, x right and y up.
    CHECK_NEAR(axes.x.x, 1, 1e-12);
    CHECK_NEAR(axes.y.y, 1, 1e-12);
}

TEST(redux_orbit_maneuver_only_dash_and_exponents) {
    auto m = parse_redux_orbit(maneuver_only());
    CHECK(m && m->body == "Mun" && m->mu == 1 && m->ut == 5 && !m->target && !m->approach && m->encounters.empty());
    CHECK(m && m->maneuver && m->maneuver->ut == 9 && m->maneuver->soi == 0);  // 0 = unbounded
    CHECK(m && m->maneuver && same(m->maneuver->position, {1, 0, 0}) && same(m->maneuver->velocity, {0, 1, 0}));
    CHECK(!parse_redux_orbit("-"));
    // C# "R" switches to an exponent for very large or small values.
    auto w = words(golden());
    w[2] = "3.5316E+12", w[7] = "1.6482E+03", w[9 - 1] = "1E-05", w[19 + 0] = "1.5204e3";
    auto e = parse_redux_orbit(joined(w));
    CHECK(e && e->mu == 3.5316e12 && e->velocity.y == 1648.2 && e->velocity.z == 1e-05 && e->approach && e->approach->ut == 1520.4);
    // The whole reply: header, nonce and "-".
    auto r = decode_redux_orbit("KSP2ORB1 77 " + golden(), "77");
    CHECK(r && r->target && r->encounters.size() == 1);
    CHECK(!decode_redux_orbit("KSP2ORB1 77 -", "77"));
    CHECK(throws([] { decode_redux_orbit("KSP2ORB1 76 " + golden(), "77"); }));
    CHECK(throws([] { decode_redux_orbit("KSP2ORB1 77", "77"); }));
    CHECK(throws([] { decode_redux_orbit("KSP2HMI2 77 " + golden(), "77"); }));
}

TEST(redux_orbit_rejects_malformed_reports) {
    const std::string g = golden();
    auto w = words(g);
    auto without_last = w;
    without_last.pop_back();
    std::vector<std::string> bad = {
        "", " ", joined(without_last), g + " 0", g + " ", " " + g, g + "\n",
        with_token(1, "4B657262696E"),         // uppercase hex
        with_token(1, "4b6"),                  // odd hex
        with_token(1, "4b65zz"),               // not hex
        with_token(1, "-"),                    // no body name
        with_token(1, "0a"),                   // control character in a name
        with_token(2, "0"), with_token(2, "-1"),  // mu must be > 0
        with_token(3, "nan"), with_token(3, "inf"), with_token(3, "1e999"), with_token(3, "0x10"), with_token(3, "1.5.2"),
        with_token(3, "--1"), with_token(3, "1e"), with_token(3, "1,5"),
        with_token(9, "8"), with_token(9, "-1"), with_token(9, "06"), with_token(9, "x"),
        with_token(9, "7"), with_token(9, "2"), with_token(9, "4"),  // token counts that no longer match the mask
        with_token(11, "2"), with_token(11, "true"),                  // target isBody flag
        with_token(12, "-1"),                                         // target radius
        with_token(20, "1.0"),                                        // approach planned flag
        with_token(27, "5"), with_token(27, "2"), with_token(27, "0"),  // n out of range / not matching
        with_token(28, "-"),                                          // encounter without a body name
        with_token(30, "0"),                                          // encounter mu
        with_token(33, "2"),                                          // encounter planned flag
        g.substr(0, g.size() - 2),                                    // cut in the middle of the last group
    };
    // Approach without a target (mask 4 alone, with its 8 values).
    bad.push_back("1 4b657262696e 1 1 0 0 0 1 0 4 1 0 1 0 0 0 1 0 0");
    for (const std::string& b : bad) {
        bool rejected = throws([&] { parse_redux_orbit(b); });
        if (!rejected) std::fprintf(stderr, "accepted: %s\n", b.substr(0, 80).c_str());
        CHECK(rejected);
    }
    // Five encounters is one more than the protocol allows even with the right token count.
    std::string five = with_token(27, "5");
    std::string group = g.substr(g.find(" 4d756e 200000 65138000000"));
    for (int i = 0; i < 4; ++i) five += group;
    CHECK(throws([&] { parse_redux_orbit(five); }));
    std::string four = with_token(27, "4");
    for (int i = 0; i < 3; ++i) four += group;
    auto parsed = parse_redux_orbit(four);
    CHECK(parsed && parsed->encounters.size() == 4);
    // The same four-encounter report with 256-byte names and zero-padded numbers: valid up to 4096 bytes, rejected
    // beyond (the reply limit), although every token alone is still well formed.
    auto inflate = [&](size_t width) {
        auto t = words(four);
        std::string big;
        for (int i = 0; i < 256; ++i) big += "4d";
        t[1] = t[10] = big;
        auto pad = [width](const std::string& v) {
            const bool minus = v[0] == '-';
            std::string digits = minus ? v.substr(1) : v;
            if (digits.size() + minus < width) digits.insert(0, width - digits.size() - minus, '0');
            return (minus ? "-" : "") + digits;
        };
        for (int i = 0; i < 4; ++i) {
            const size_t at = 28 + 15 * size_t(i);
            t[at] = big;
            for (size_t k = 1; k < 15; ++k)
                if (k != 5) t[at + k] = pad(t[at + k]);
        }
        return joined(t);
    };
    const std::string fits = inflate(8), oversized = inflate(30);
    CHECK(fits.size() <= 4096 && oversized.size() > 4096);
    CHECK(parse_redux_orbit(fits).has_value());
    CHECK(throws([&] { parse_redux_orbit(oversized); }));
}

TEST(redux_orbit_tracker_cadence_staleness_and_vessel_changes) {
    ReduxOrbitTracker t;
    CHECK(t.due(0));
    t.sent(10);
    CHECK(!t.due(10.5) && t.due(11));
    Snapshot s;
    s.vessel = "Test", s.body = "Kerbin";
    t.apply(s, 10);
    CHECK(!s.orbit_state);  // nothing yet
    t.received(parse_redux_orbit(golden()), "Test", 10);
    Snapshot fresh = s;
    t.apply(fresh, 12.9);
    CHECK(fresh.orbit_state && fresh.target_orbit && fresh.approach && fresh.encounters.size() == 1 && t.holding());
    const Axes first = fresh.orbit_state->axes;
    Snapshot old = s;
    t.apply(old, 13.1);  // no fresh reply for over 3 s
    CHECK(!old.orbit_state && !old.target_orbit && old.encounters.empty() && !t.holding());
    Snapshot after = s;
    t.apply(after, 13.2);
    CHECK(!after.orbit_state);  // dropped stays dropped
    // The view axes stay fixed for the vessel and body while the orbit turns, as in the kRPC source.
    auto turned = parse_redux_orbit(golden());
    turned->position = {0, 1300000, 0}, turned->velocity = {0, 0, 1648.2};
    t.received(turned, "Test", 20);
    Snapshot kept = s;
    t.apply(kept, 20);
    CHECK(kept.orbit_state && same(kept.orbit_state->axes.x, first.x) && same(kept.orbit_state->axes.y, first.y));
    // Another vessel or another body drops the report at once.
    Snapshot other = s;
    other.vessel = "Other";
    t.apply(other, 20.1);
    CHECK(!other.orbit_state && !t.holding());
    t.received(turned, "Other", 21);
    Snapshot recomputed = other;
    t.apply(recomputed, 21);
    CHECK(recomputed.orbit_state && !same(recomputed.orbit_state->axes.y, first.y));  // a new vessel: new axes
    Snapshot moved = other;
    moved.body = "Mun";
    t.apply(moved, 21.1);
    CHECK(!moved.orbit_state && !t.holding());
    // "-" (no orbit data) drops it at once too.
    t.received(parse_redux_orbit(golden()), "Test", 30);
    t.received(std::nullopt, "Test", 30.5);
    Snapshot dash = s;
    t.apply(dash, 30.6);
    CHECK(!dash.orbit_state);
    t.reset();
    CHECK(t.due(0) && !t.holding());
}

TEST(redux_orbit_exchange_matches_its_nonce) {
    Loopback link;
    const net::socket_t fd = link.server.get();
    auto serve = [fd](std::vector<std::string> replies) {
        return std::thread([fd, replies] {
            char buffer[256];
            sockaddr_in from{};
            socklen_t size = sizeof(from);
            if (::recvfrom(fd, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &size) <= 0) return;
            for (const auto& r : replies) ::sendto(fd, r.data(), int(r.size()), 0, reinterpret_cast<sockaddr*>(&from), size);
        });
    };
    // A foreign nonce and a malformed reply are skipped; the matching one is taken.
    auto mixed = serve({"KSP2ORB1 4 -", "KSP2ORB1 5 garbage", "KSP2ORB1 5 " + golden()});
    auto got = redux_orbit_exchange(link.client.get(), "5", 0.5);
    mixed.join();
    CHECK(got && *got && (*got)->target && (*got)->target->name == "Mun");
    auto dash = serve({"KSP2ORB1 6 -"});
    auto none = redux_orbit_exchange(link.client.get(), "6", 0.5);
    dash.join();
    CHECK(none && !*none);  // answered: no orbit data
    auto silent = serve({});
    CHECK(!redux_orbit_exchange(link.client.get(), "7", 0.05));  // unsupported server: no reply at all
    silent.join();
}

TEST(redux_source_requests_the_orbit_report_once_a_second_and_drops_it_when_stale) {
    // A fake Redux plugin on a loopback port: KSP2HMI snapshots always, KSP2ORB1 while `orbit` is set.
    Loopback link;
    std::atomic<bool> running{true}, orbit{true};
    std::atomic<int> orbit_requests{0}, snapshots{0};
    const net::socket_t fd = link.server.get();
    std::thread game([&] {
        char buffer[256];
        while (running) {
            if (net::wait(fd, false, 20) <= 0) continue;
            sockaddr_in from{};
            socklen_t size = sizeof(from);
            const long count = ::recvfrom(fd, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &size);
            if (count <= 0) continue;
            const std::string request(buffer, size_t(count));
            const std::string nonce = request.substr(request.find(' ') + 1);
            std::string reply;
            if (request.rfind("KSP2HMI", 0) == 0) {
                reply = PACKET;
                reply.replace(9, 2, nonce);
                ++snapshots;
            } else if (request.rfind("KSP2ORB1 ", 0) == 0) {
                ++orbit_requests;
                if (!orbit) continue;
                reply = "KSP2ORB1 " + nonce + " " + golden();
            } else {
                continue;  // VESSEL view and the rest: an older plugin
            }
            ::sendto(fd, reply.data(), int(reply.size()), 0, reinterpret_cast<sockaddr*>(&from), size);
        }
    });
    check::set_env("KSPHMI_REDUX_PORT", std::to_string(link.port()));
    KrpcSource::Options o;
    o.address = "127.0.0.1";
    o.discovery = false;
    ReduxSource src(o, false);
    src.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(2300));
    auto with = src.latest();
    const int first = orbit_requests;
    orbit = false;  // the game stops answering KSP2ORB1 (stale report): the snapshot must keep flowing
    std::this_thread::sleep_for(std::chrono::milliseconds(3600));
    auto without = src.latest();
    const int second = orbit_requests - first;
    src.stop();
    running = false;
    game.join();
    check::unset_env("KSPHMI_REDUX_PORT");
    CHECK(first >= 2 && first <= 4);    // about 1 Hz, not with every 10 Hz snapshot
    CHECK(second >= 3 && second <= 5);  // still asked about once a second while unanswered
    CHECK(snapshots > 30);
    CHECK(with && with->state == State::Flight && with->orbit_state && with->target_orbit && with->approach);
    CHECK(with && with->encounters.size() == 1 && with->orbit_state->mu == 3531600000000.0);
    CHECK(without && without->state == State::Flight && without->vessel == "Test");
    CHECK(without && !without->orbit_state && !without->target_orbit && without->encounters.empty());
}

int main(int argc, char** argv) { return RUN_TESTS(argc, argv); }
