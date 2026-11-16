#include "netinfo.hpp"

#include "net.hpp"

#if defined(_WIN32)
#include <iphlpapi.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#endif

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace ksphmi {

std::string route_address(const std::string& target) {
    auto addr = net::resolve_ipv4(target, 9);
    if (!addr) return "";
    net::Fd s(net::open_socket(SOCK_DGRAM));
    if (!s || connect(s.get(), reinterpret_cast<const sockaddr*>(&*addr), sizeof *addr) != 0) return "";
    sockaddr_in local{};
    socklen_t len = sizeof local;
    if (getsockname(s.get(), reinterpret_cast<sockaddr*>(&local), &len) != 0) return "";
    return net::to_string(local.sin_addr);
}

#if defined(_WIN32)

namespace {
// One network adapter as the waiting and boot screens need it (Windows: GetAdaptersAddresses).
struct Adapter {
    std::string name;
    bool up = false;
    std::vector<std::string> addresses;  // IPv4, loopback excluded
};

std::string utf8(const wchar_t* w) {
    if (!w || !*w) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "";
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], n, nullptr, nullptr);
    out.resize(static_cast<size_t>(n - 1));
    return out;
}

std::vector<Adapter> adapters() {
    std::vector<Adapter> found;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf;
    ULONG r = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && r == ERROR_BUFFER_OVERFLOW; ++tries) {
        buf.resize(size);
        r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                 nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (r != NO_ERROR) return found;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        Adapter one;
        one.name = utf8(a->FriendlyName);
        one.up = a->OperStatus == IfOperStatusUp;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            std::string s = net::to_string(reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr);
            if (!s.empty() && s.rfind("127.", 0) != 0) one.addresses.push_back(s);
        }
        found.push_back(std::move(one));
    }
    return found;
}
}  // namespace

std::vector<std::string> all_addresses() {
    std::vector<std::string> found;
    for (const auto& a : adapters())
        if (a.up) found.insert(found.end(), a.addresses.begin(), a.addresses.end());
    return found;
}

NetInterface primary_interface() {
    NetInterface up;
    for (const auto& a : adapters()) {
        if (!a.up) continue;
        if (!a.addresses.empty()) return {a.name, a.addresses.front()};
        if (up.name.empty()) up.name = a.name;
    }
    return up;
}

#else

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

#endif

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
    const std::string hostname = net::host_name();
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
