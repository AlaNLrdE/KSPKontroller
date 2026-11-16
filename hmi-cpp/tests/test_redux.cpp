// KSP 2 Redux source: wire decoding, facility scenes, ship/VESSEL text replies, remote keyboard requests and the
// nonce matching of the UDP exchange (loopback only, no game).
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

int main(int argc, char** argv) { return RUN_TESTS(argc, argv); }
