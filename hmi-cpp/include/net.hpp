// Small socket helpers shared by the kRPC client, scene context, discovery and network info.
//
// One abstraction over two socket APIs: POSIX sockets (the Pi, Linux, WSL, macOS) and Winsock 2 (the native
// Windows build with MSYS2 UCRT64). Code outside src/net.cpp uses these helpers instead of close(), poll(),
// errno or fcntl(), which Winsock spells differently; the POSIX branches keep the exact calls used before.
#pragma once

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace ksphmi::net {

#if defined(_WIN32)
using socket_t = SOCKET;
constexpr socket_t NO_SOCKET = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t NO_SOCKET = -1;
#endif

/// socket(AF_INET, type, 0); on Windows it starts Winsock (WSAStartup) once before the first socket.
socket_t open_socket(int type);
void close_socket(socket_t fd);

/// Owns a socket.
class Fd {
public:
    Fd() = default;
    explicit Fd(socket_t fd) : fd_(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& o) noexcept : fd_(o.release()) {}
    Fd& operator=(Fd&& o) noexcept {
        if (this != &o) reset(o.release());
        return *this;
    }
    ~Fd() { reset(); }
    socket_t get() const { return fd_; }
    explicit operator bool() const { return fd_ != NO_SOCKET; }
    socket_t release() {
        socket_t f = fd_;
        fd_ = NO_SOCKET;
        return f;
    }
    void reset(socket_t fd = NO_SOCKET) {
        if (fd_ != NO_SOCKET) close_socket(fd_);
        fd_ = fd;
    }

private:
    socket_t fd_ = NO_SOCKET;
};

/// The last socket error (errno, or WSAGetLastError() on Windows) and its text.
int last_error();
std::string error_text(int e);
/// The call was interrupted by a signal (EINTR); retry it.
bool interrupted(int e);
/// Nothing to read or no room to write yet (EAGAIN / EWOULDBLOCK).
bool would_block(int e);
/// A non-blocking connect() is under way (EINPROGRESS; WSAEWOULDBLOCK on Windows).
bool connect_pending(int e);

/// poll() on one socket for POLLIN or POLLOUT: > 0 ready (or failed: read the error with the next call),
/// 0 timed out, < 0 error (last_error()).
int wait(socket_t fd, bool for_write, int timeout_ms);
/// Switch O_NONBLOCK (FIONBIO on Windows).
void set_nonblocking(socket_t fd, bool on);
/// send() that never raises SIGPIPE; recv(). Byte counts, or < 0 on error (last_error()).
long send_some(socket_t fd, const char* data, size_t size);
long recv_some(socket_t fd, char* data, size_t size);
/// shutdown(SHUT_RDWR): wakes a thread blocked on the socket.
void shutdown_both(socket_t fd);

/// IPv4 address of a host name or dotted quad; nothing when it does not resolve.
std::optional<sockaddr_in> resolve_ipv4(const std::string& host, int port);

std::string to_string(const in_addr& a);
/// Every IPv4 answer for `host`, in resolver order, duplicates removed (empty when it does not resolve).
std::vector<std::string> resolve_all_ipv4(const std::string& host);

/// Receive timeout on a socket (seconds, > 0).
void set_timeout(socket_t fd, double seconds);

/// gethostname() ("" when unknown).
std::string host_name();

}  // namespace ksphmi::net
