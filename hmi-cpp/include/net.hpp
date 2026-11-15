// Small POSIX socket helpers shared by the kRPC client, scene context, discovery and network info.
#pragma once

#include <netinet/in.h>
#include <unistd.h>

#include <optional>
#include <string>

namespace ksphmi::net {

/// Owns a file descriptor.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& o) noexcept : fd_(o.release()) {}
    Fd& operator=(Fd&& o) noexcept {
        if (this != &o) reset(o.release());
        return *this;
    }
    ~Fd() { reset(); }
    int get() const { return fd_; }
    explicit operator bool() const { return fd_ >= 0; }
    int release() {
        int f = fd_;
        fd_ = -1;
        return f;
    }
    void reset(int fd = -1) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

/// IPv4 address of a host name or dotted quad; nothing when it does not resolve.
std::optional<sockaddr_in> resolve_ipv4(const std::string& host, int port);

std::string to_string(const in_addr& a);

/// Receive timeout on a socket (seconds, > 0).
void set_timeout(int fd, double seconds);

}  // namespace ksphmi::net
