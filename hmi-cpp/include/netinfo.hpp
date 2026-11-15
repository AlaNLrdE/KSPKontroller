// This display's network identity for the waiting screen (port of netinfo.py).
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ksphmi {

/// Local IPv4 address used to reach `target` (a UDP connect sends nothing). Empty when unknown.
std::string route_address(const std::string& target);

/// Every IPv4 address of this machine's interfaces, loopback excluded.
std::vector<std::string> all_addresses();

/// Network identity refreshed on a worker; drawing only reads the cached line.
class NetInfo {
public:
    /// Optional lookup callback runs on the worker and may outlive this instance.
    using Query = std::function<std::string(const std::string&)>;
    explicit NetInfo(std::string server, double every = 15.0, Query query = {});
    ~NetInfo();
    NetInfo(const NetInfo&) = delete;
    NetInfo& operator=(const NetInfo&) = delete;
    /// "This display: 192.168.1.20 (hostname), also 10.0.0.3"; provisional while the first query runs.
    std::string line() const;

private:
    struct State;
    std::shared_ptr<State> state_;
};

}  // namespace ksphmi
