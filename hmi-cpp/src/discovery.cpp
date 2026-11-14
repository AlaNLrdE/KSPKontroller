// Find KSPSerialIO on the local IPv4 LAN with its existing scene responder (port of discovery.py).
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>

#include <cctype>
#include <cstdint>
#include <cstring>

#include "model.hpp"
#include "net.hpp"
#include "scenes.hpp"

namespace ksphmi {

namespace {

// Strict dotted quad as Python's ipaddress.IPv4Address: four decimal octets, no leading zeros.
bool parse_ipv4(const std::string& s, uint32_t& out) {
    uint32_t value = 0;
    int parts = 0;
    size_t i = 0;
    while (parts < 4) {
        size_t start = i;
        unsigned octet = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])) && i - start < 3)
            octet = octet * 10 + static_cast<unsigned>(s[i++] - '0');
        size_t len = i - start;
        if (len == 0 || octet > 255 || (len > 1 && s[start] == '0')) return false;
        value = (value << 8) | octet;
        ++parts;
        if (parts < 4) {
            if (i >= s.size() || s[i] != '.') return false;
            ++i;
        }
    }
    if (i != s.size()) return false;
    out = value;
    return true;
}

bool in(uint32_t ip, uint32_t net, int bits) {
    uint32_t mask = bits == 0 ? 0 : 0xFFFFFFFFu << (32 - bits);
    return (ip & mask) == (net & mask);
}

constexpr uint32_t ip(unsigned a, unsigned b, unsigned c, unsigned d) { return (a << 24) | (b << 16) | (c << 8) | d; }

}  // namespace

bool is_local(const std::string& address) {
    uint32_t a;
    if (!parse_ipv4(address, a)) return false;
    // Python 3.11 IPv4Address.is_private, plus is_link_local and is_loopback (both inside that list).
    static const struct {
        uint32_t net;
        int bits;
    } private_nets[] = {
        {ip(0, 0, 0, 0), 8},       {ip(10, 0, 0, 0), 8},      {ip(127, 0, 0, 0), 8},     {ip(169, 254, 0, 0), 16},
        {ip(172, 16, 0, 0), 12},   {ip(192, 0, 0, 0), 29},    {ip(192, 0, 0, 170), 31},  {ip(192, 0, 2, 0), 24},
        {ip(192, 168, 0, 0), 16},  {ip(198, 18, 0, 0), 15},   {ip(198, 51, 100, 0), 24}, {ip(203, 0, 113, 0), 24},
        {ip(240, 0, 0, 0), 4},     {ip(255, 255, 255, 255), 32},
    };
    for (const auto& n : private_nets)
        if (in(a, n.net, n.bits)) return true;
    return false;
}

std::string discover(int port, double timeout, const std::vector<std::string>& targets) {
    net::Fd sock(socket(AF_INET, SOCK_DGRAM, 0));
    if (!sock) return "";
    int yes = 1;
    setsockopt(sock.get(), SOL_SOCKET, SO_BROADCAST, &yes, sizeof yes);
    static const char request[] = "KSPHMI/1";
    for (const auto& target : targets) {
        auto addr = net::resolve_ipv4(target, port);
        if (!addr || sendto(sock.get(), request, sizeof request - 1, 0, reinterpret_cast<const sockaddr*>(&*addr),
                            sizeof *addr) < 0)
            return "";
    }
    double deadline = monotonic() + timeout;
    for (;;) {
        double left = deadline - monotonic();
        if (left <= 0) return "";
        pollfd p{sock.get(), POLLIN, 0};
        if (poll(&p, 1, std::max(1, static_cast<int>(left * 1000))) <= 0) continue;
        char buf[256];
        sockaddr_in peer{};
        socklen_t len = sizeof peer;
        ssize_t n = recvfrom(sock.get(), buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&peer), &len);
        if (n < 0) return "";
        std::string data(buf, static_cast<size_t>(n));
        std::string host = net::to_string(peer.sin_addr);
        bool named = false;  // something after "KSPHMI/1 " besides white space
        for (size_t i = 9; i < data.size(); ++i)
            if (!std::isspace(static_cast<unsigned char>(data[i]))) named = true;
        if (ntohs(peer.sin_port) == port && is_local(host) && data.compare(0, 9, "KSPHMI/1 ") == 0 && named)
            return host;
    }
}

}  // namespace ksphmi
