#include "netinfo.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <cstring>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

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

NetInterface primary_interface() {
    NetInterface found, up;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return found;
    for (ifaddrs* i = list; i; i = i->ifa_next) {
        if (!i->ifa_name || (i->ifa_flags & IFF_LOOPBACK)) continue;
        if (up.name.empty() && (i->ifa_flags & IFF_UP)) up.name = i->ifa_name;
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        std::string a = net::to_string(reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr);
        if (!a.empty() && a.rfind("127.", 0) != 0) {
            found = {i->ifa_name, a};
            break;
        }
    }
    freeifaddrs(list);
    return found.name.empty() ? up : found;
}

namespace {
std::string network_line(const std::string& server, const std::string& hostname) {
    std::string primary = route_address(server);
    auto everything = all_addresses();
    if (primary.empty() || primary.rfind("127.", 0) == 0)
        primary = everything.empty() ? "" : everything.front();
    std::string text = "This display: " + (primary.empty() ? std::string("no network address") : primary) +
                       " (" + hostname + ")";
    bool first = true;
    for (const auto& address : everything) {
        if (address == primary) continue;
        text += (first ? ", also " : ", ") + address;
        first = false;
    }
    return text;
}
}  // namespace

struct NetInfo::State {
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    std::string text;
};

NetInfo::NetInfo(std::string server, double every, Query query) : state_(std::make_shared<State>()) {
    char name[256] = {0};
    gethostname(name, sizeof name - 1);
    const std::string hostname = name;
    state_->text = "This display: looking up network address (" + hostname + ")";
    if (!query) query = [hostname](const std::string& target) { return network_line(target, hostname); };
    // A system DNS call cannot be cancelled portably. The worker owns only shared state, so destruction
    // signals it without waiting for DNS or leaving references to the display behind.
    auto state = state_;
    std::thread([state, server = std::move(server), every, query = std::move(query)] {
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->stopping) return;
            }
            std::string text;
            try {
                text = query(server);  // never hold the cache lock during DNS or interface enumeration
            } catch (const std::exception&) {
                // Keep the last known identity and retry after the normal refresh interval.
            }
            std::unique_lock<std::mutex> lock(state->mutex);
            if (state->stopping) return;
            if (!text.empty()) state->text = std::move(text);
            if (state->wake.wait_for(lock, std::chrono::duration<double>(every), [&] { return state->stopping; }))
                return;
        }
    }).detach();
}

NetInfo::~NetInfo() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->stopping = true;
    state_->wake.notify_all();
}

std::string NetInfo::line() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->text;
}

}  // namespace ksphmi
