// This display's network identity for the waiting screen (port of netinfo.py).
#pragma once

#include <string>
#include <vector>

namespace ksphmi {

/// Local IPv4 address used to reach `target` (a UDP connect sends nothing). Empty when unknown.
std::string route_address(const std::string& target);

/// Every IPv4 address of this machine's interfaces, loopback excluded.
std::vector<std::string> all_addresses();

/// Cached, refreshed every `every` seconds (DHCP may change the address while the HMI runs).
class NetInfo {
public:
    explicit NetInfo(std::string server, double every = 15.0);
    void refresh(double now);
    /// "This display: 192.168.1.20 (hostname), also 10.0.0.3"
    std::string line();

    std::string server, hostname, primary;
    std::vector<std::string> others;

private:
    double every_, at_ = -1e9;
};

}  // namespace ksphmi
