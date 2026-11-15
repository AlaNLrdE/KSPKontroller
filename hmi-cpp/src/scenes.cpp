#include "scenes.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>

#include "net.hpp"

namespace ksphmi {

const std::vector<View>& views() {
    static const std::vector<View> v = {
        {"space_center", "Space Center", "MISSION OPERATIONS",
         {{{"BUILD", "VAB / Spaceplane Hangar", "Design the next mission vehicle"},
           {"PLAN", "Mission Control", "Review contracts and their requirements"},
           {"PREPARE", "Astronaut Complex", "Recruit and assign your crew"}}}},
        {"vab", "Vehicle Assembly Building", "ROCKET PREPARATION",
         {{{"01", "Payload & crew", "Check capacity, crew and mission equipment"},
           {"02", "Propulsion & staging", "Review delta-v, thrust and decouplers"},
           {"03", "Launch readiness", "Check parachutes, power and communications"}}}},
        {"sph", "Spaceplane Hangar", "AIRCRAFT PREPARATION",
         {{{"01", "Balance", "Compare centre of mass and centre of lift"},
           {"02", "Flight controls", "Check control surfaces and action groups"},
           {"03", "Runway readiness", "Review landing gear, brakes and fuel"}}}},
        {"mission_control", "Mission Control", "CONTRACT PLANNING",
         {{{"OBJECTIVE", "Mission requirements", "Review every parameter before accepting"},
           {"SCHEDULE", "Deadline & destination", "Allow time for transfer and completion"},
           {"BUDGET", "Rewards & penalties", "Compare advance, payout and launch cost"}}}},
        {"astronaut_complex", "Astronaut Complex", "CREW OPERATIONS",
         {{{"PILOT", "Flight control", "Review pilot availability and experience"},
           {"ENGINEER", "Vehicle support", "Plan repairs and surface operations"},
           {"SCIENTIST", "Science operations", "Choose crew for experiments and laboratories"}}}},
        {"research", "Research & Development", "TECHNOLOGY PLANNING",
         {{{"SCIENCE", "Research budget", "Review available science before unlocking nodes"},
           {"TECHNOLOGY", "Mission capabilities", "Prioritize parts needed for the next mission"},
           {"EXPERIMENTS", "Science archives", "Review results and unexplored destinations"}}}},
        {"administration", "Administration", "PROGRAM STRATEGY",
         {{{"STRATEGY", "Active policies", "Review commitments and resource conversion"},
           {"RESOURCES", "Program priorities", "Balance funds, science and reputation"},
           {"PLANNING", "Long-term goals", "Choose policies that support upcoming missions"}}}},
        {"tracking_station", "Tracking Station", "FLEET OPERATIONS",
         {{{"TRACK", "Active missions", "Select a vessel and inspect its trajectory"},
           {"PLAN", "Transfers & encounters", "Review destinations and sphere changes"},
           {"CONTROL", "Resume a mission", "Switch to a vessel for live flight telemetry"}}}},
    };
    return v;
}

const View* find_view(const std::string& scene) {
    for (const auto& v : views())
        if (scene == v.key) return &v;
    return nullptr;
}

std::string parse_context(const std::string& reply) {
    for (unsigned char c : reply)
        if (c >= 0x80) return "";  // Python decode('ascii') fails
    size_t space = reply.find(' ');
    if (space == std::string::npos) return "";
    std::string prefix = reply.substr(0, space), scene = reply.substr(space + 1);
    return prefix == "KSPHMI/1" && find_view(scene) ? scene : "";
}

std::string read_context(const std::string& address, int port) {
    auto addr = net::resolve_ipv4(address, port);
    if (!addr) return "";
    net::Fd sock(socket(AF_INET, SOCK_DGRAM, 0));
    if (!sock) return "";
    net::set_timeout(sock.get(), 0.15);
    if (connect(sock.get(), reinterpret_cast<const sockaddr*>(&*addr), sizeof *addr) != 0) return "";
    static const char request[] = "KSPHMI/1";
    if (send(sock.get(), request, sizeof request - 1, 0) < 0) return "";
    char buf[256];
    ssize_t n = recv(sock.get(), buf, sizeof buf, 0);
    if (n <= 0) return "";
    return parse_context(std::string(buf, static_cast<size_t>(n)));
}

}  // namespace ksphmi
