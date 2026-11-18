#include "endpoint.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>

#include "net.hpp"

#if !defined(_WIN32)
#include <arpa/inet.h>
#endif

namespace ksphmi {

namespace {

bool contains(const std::vector<std::string>& list, const std::string& value) {
    return !value.empty() && std::find(list.begin(), list.end(), value) != list.end();
}

bool parse(const std::string& ip, uint32_t& out) {
    in_addr a{};
    if (ip.empty() || inet_pton(AF_INET, ip.c_str(), &a) != 1) return false;
    out = ntohl(a.s_addr);
    return true;
}

bool same_24(const std::string& a, const std::string& b) {
    uint32_t x = 0, y = 0;
    return parse(a, x) && parse(b, y) && (x >> 8) == (y >> 8);
}

}  // namespace

std::string normalise_mac(const std::string& text) {
    std::string out;
    int groups = 0;
    size_t i = 0;
    while (i <= text.size()) {
        size_t start = i;
        while (i < text.size() && std::isxdigit(static_cast<unsigned char>(text[i]))) ++i;
        const size_t len = i - start;
        if (len == 0 || len > 2) return "";
        std::string part = text.substr(start, len);
        if (len == 1) part = "0" + part;
        for (char& c : part) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        out += (out.empty() ? "" : ":") + part;
        if (++groups == 6) return i == text.size() ? out : "";
        if (i >= text.size() || (text[i] != ':' && text[i] != '-')) return "";
        ++i;
    }
    return "";
}

std::vector<ArpEntry> parse_arp_table(const std::string& text) {
    // "IP address       HW type     Flags       HW address            Mask     Device"
    std::vector<ArpEntry> out;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream f(line);
        std::string ip, type, flags, mac, mask, device;
        if (!(f >> ip >> type >> flags >> mac >> mask >> device)) continue;
        uint32_t unused = 0;
        if (!parse(ip, unused)) continue;  // the header row
        if (std::strtoul(flags.c_str(), nullptr, 16) == 0) continue;  // incomplete / failed
        const std::string m = normalise_mac(mac);
        if (m.empty() || m == "00:00:00:00:00:00") continue;
        out.push_back({ip, m, device});
    }
    return out;
}

std::vector<ArpEntry> read_arp_table() {
    std::ifstream in("/proc/net/arp");
    if (!in) return {};
    std::stringstream buf;
    buf << in.rdbuf();
    return parse_arp_table(buf.str());
}

bool is_ipv4_literal(const std::string& host) {
    uint32_t unused = 0;
    return parse(host, unused);
}

EndpointChoice choose_endpoint(const std::vector<std::string>& resolved, const std::vector<ArpEntry>& arp,
                               const std::string& prefer_mac, bool current_ok, EndpointMemory& memory) {
    auto pick = [&](const std::string& address, const char* reason) {
        memory.current = address;
        return EndpointChoice{address, reason};
    };
    if (prefer_mac.empty()) return resolved.empty() ? EndpointChoice{} : pick(resolved.front(), "");
    // 2. The neighbour table shows the preferred MAC near the PC's answers (or anywhere when nothing is known yet).
    std::vector<std::string> nearby = resolved;
    if (!memory.current.empty()) nearby.push_back(memory.current);
    if (!memory.learned.empty()) nearby.push_back(memory.learned);
    const ArpEntry* best = nullptr;
    for (const auto& e : arp) {
        if (e.mac != prefer_mac) continue;
        bool close = nearby.empty();
        for (const auto& r : nearby) close = close || same_24(e.ip, r);
        if (!close) continue;
        if (!best || e.ip == memory.current || (e.ip == memory.learned && best->ip != memory.current)) best = &e;
    }
    if (best) {
        memory.learned = best->ip;
        return pick(best->ip, "preferred MAC");
    }
    // A complete neighbour entry for the current address (any MAC) also means it still answers: Linux marks an
    // address that stopped answering ARP as incomplete/failed (flags 0x0), which parse_arp_table drops.
    bool alive = current_ok;
    for (const auto& e : arp) alive = alive || (!memory.current.empty() && e.ip == memory.current);
    // 3. The MAC column can vanish (entry flushed): the learned Ethernet address stays while it answers.
    if (!memory.learned.empty() && memory.current == memory.learned && alive)
        return pick(memory.current, "remembered preferred MAC");
    // 4. Sticky: a working address, or one the name still answers with, is never swapped for an equivalent one.
    if (!memory.current.empty() && (alive || contains(resolved, memory.current))) return pick(memory.current, "kept");
    // 5. The current address stopped working and the name points elsewhere.
    if (!resolved.empty()) return pick(resolved.front(), "resolved");
    return {};  // nothing resolves and nothing known answers: the caller keeps what it has
}

void poke_neighbour(const std::string& ip) {
    auto addr = net::resolve_ipv4(ip, 9);
    if (!addr) return;
    net::Fd s(net::open_socket(SOCK_DGRAM));
    if (!s) return;
    sendto(s.get(), "", 0, 0, reinterpret_cast<const sockaddr*>(&*addr), sizeof *addr);
}

EndpointChoice EndpointResolver::resolve(bool current_ok) {
    const std::vector<std::string> resolved = net::resolve_all_ipv4(host_);
    if (!preferring()) return choose_endpoint(resolved, {}, "", current_ok, memory_);
    EndpointChoice choice = choose_endpoint(resolved, read_arp_table(), prefer_mac_, current_ok, memory_);
    for (const auto& ip : resolved)
        if (ip != choice.address) poke_neighbour(ip);
    return choice;
}

std::string endpoint_note(const EndpointChoice& choice, const std::string& previous) {
    std::string parts = choice.reason;
    if (!previous.empty()) parts += (parts.empty() ? "" : ", ") + std::string("was ") + previous;
    return parts.empty() ? "" : " (" + parts + ")";
}

}  // namespace ksphmi
