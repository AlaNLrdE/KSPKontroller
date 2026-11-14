#include "netinfo.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <cstring>

#include "model.hpp"
#include "net.hpp"

namespace ksphmi {

namespace net {

std::optional<sockaddr_in> resolve_ipv4(const std::string& host, int port) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
    if (host.empty() || getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return std::nullopt;
    sockaddr_in out{};
    std::memcpy(&out, res->ai_addr, sizeof out);
    freeaddrinfo(res);
    out.sin_port = htons(static_cast<uint16_t>(port));
    return out;
}

std::string to_string(const in_addr& a) {
    char buf[INET_ADDRSTRLEN];
    return inet_ntop(AF_INET, &a, buf, sizeof buf) ? buf : "";
}

void set_timeout(int fd, double seconds) {
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(seconds);
    tv.tv_usec = static_cast<suseconds_t>((seconds - static_cast<double>(tv.tv_sec)) * 1e6);
    if (tv.tv_sec == 0 && tv.tv_usec == 0) tv.tv_usec = 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

}  // namespace net

std::string route_address(const std::string& target) {
    auto addr = net::resolve_ipv4(target, 9);
    if (!addr) return "";
    net::Fd s(socket(AF_INET, SOCK_DGRAM, 0));
    if (!s || connect(s.get(), reinterpret_cast<const sockaddr*>(&*addr), sizeof *addr) != 0) return "";
    sockaddr_in local{};
    socklen_t len = sizeof local;
    if (getsockname(s.get(), reinterpret_cast<sockaddr*>(&local), &len) != 0) return "";
    return net::to_string(local.sin_addr);
}

std::vector<std::string> all_addresses() {
    std::vector<std::string> found;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return found;
    for (ifaddrs* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        std::string a = net::to_string(reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr);
        if (!a.empty() && a.rfind("127.", 0) != 0) found.push_back(a);
    }
    freeifaddrs(list);
    return found;
}

NetInfo::NetInfo(std::string server_, double every) : server(std::move(server_)), every_(every) {
    char name[256] = {0};
    gethostname(name, sizeof name - 1);
    hostname = name;
}

void NetInfo::refresh(double now) {
    if (now - at_ < every_) return;
    at_ = now;
    std::string p = route_address(server);
    auto everything = all_addresses();
    if (p.empty() || p.rfind("127.", 0) == 0) p = everything.empty() ? "" : everything.front();  // server = localhost
    primary = p;
    others.clear();
    for (const auto& a : everything)
        if (a != primary) others.push_back(a);
}

std::string NetInfo::line() {
    refresh(monotonic());
    std::string text = "This display: " + (primary.empty() ? std::string("no network address") : primary) + " (" +
                       hostname + ")";
    if (!others.empty()) {
        text += ", also ";
        for (size_t i = 0; i < others.size(); ++i) text += (i ? ", " : "") + others[i];
    }
    return text;
}

}  // namespace ksphmi
