#include "net.hpp"

#if defined(_WIN32)
#include <climits>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/time.h>

#include <cerrno>
#endif

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace ksphmi::net {

#if defined(_WIN32)

namespace {
// Winsock needs WSAStartup before socket(), getaddrinfo() or gethostname(); never cleaned up (process lifetime).
void start() {
    static const bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)started;
}
}  // namespace

socket_t open_socket(int type) {
    start();
    socket_t fd = socket(AF_INET, type, 0);
    if (fd != NO_SOCKET && type == SOCK_DGRAM) {
        // An ICMP port unreachable for an earlier datagram makes Windows fail the next recvfrom() with
        // WSAECONNRESET; POSIX ignores it on unconnected sockets. Discovery broadcasts to several targets.
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
        BOOL report = FALSE;
        DWORD bytes = 0;
        WSAIoctl(fd, SIO_UDP_CONNRESET, &report, sizeof report, nullptr, 0, &bytes, nullptr, nullptr);
    }
    return fd;
}

void close_socket(socket_t fd) { closesocket(fd); }

int last_error() { return WSAGetLastError(); }

std::string error_text(int e) {
    char buf[256] = {0};
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                             static_cast<DWORD>(e), 0, buf, sizeof buf, nullptr);
    std::string text(buf, n);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '.'))
        text.pop_back();
    return text.empty() ? "socket error " + std::to_string(e) : text;
}

bool interrupted(int e) { return e == WSAEINTR; }
bool would_block(int e) { return e == WSAEWOULDBLOCK; }
bool connect_pending(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }

int wait(socket_t fd, bool for_write, int timeout_ms) {
    // select() rather than WSAPoll(): older WSAPoll builds never report a refused non-blocking connect.
    // A failed connect shows in the exception set; the caller then reads SO_ERROR.
    fd_set ready, failed;
    FD_ZERO(&ready);
    FD_ZERO(&failed);
    FD_SET(fd, &ready);
    FD_SET(fd, &failed);
    timeval tv{};
    if (timeout_ms > 0) tv.tv_sec = timeout_ms / 1000, tv.tv_usec = (timeout_ms % 1000) * 1000;
    return select(0, for_write ? nullptr : &ready, for_write ? &ready : nullptr, &failed, timeout_ms < 0 ? nullptr : &tv);
}

void set_nonblocking(socket_t fd, bool on) {
    u_long mode = on ? 1 : 0;
    ioctlsocket(fd, FIONBIO, &mode);
}

long send_some(socket_t fd, const char* data, size_t size) {
    return send(fd, data, static_cast<int>(std::min<size_t>(size, INT_MAX)), 0);
}

long recv_some(socket_t fd, char* data, size_t size) {
    return recv(fd, data, static_cast<int>(std::min<size_t>(size, INT_MAX)), 0);
}

void shutdown_both(socket_t fd) { shutdown(fd, SD_BOTH); }

void set_timeout(socket_t fd, double seconds) {
    // Winsock takes the receive / send timeouts as a DWORD of milliseconds.
    DWORD ms = static_cast<DWORD>(std::max(1.0, seconds * 1000));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
}

#else

namespace {
void start() {}
}  // namespace

socket_t open_socket(int type) { return socket(AF_INET, type, 0); }

void close_socket(socket_t fd) { ::close(fd); }

int last_error() { return errno; }

std::string error_text(int e) { return std::strerror(e); }

bool interrupted(int e) { return e == EINTR; }
bool would_block(int e) { return e == EAGAIN || e == EWOULDBLOCK; }
bool connect_pending(int e) { return e == EINPROGRESS; }

int wait(socket_t fd, bool for_write, int timeout_ms) {
    pollfd p{fd, static_cast<short>(for_write ? POLLOUT : POLLIN), 0};
    return poll(&p, 1, timeout_ms);
}

void set_nonblocking(socket_t fd, bool on) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}

long send_some(socket_t fd, const char* data, size_t size) { return send(fd, data, size, MSG_NOSIGNAL); }

long recv_some(socket_t fd, char* data, size_t size) { return recv(fd, data, size, 0); }

void shutdown_both(socket_t fd) { shutdown(fd, SHUT_RDWR); }

void set_timeout(socket_t fd, double seconds) {
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(seconds);
    tv.tv_usec = static_cast<suseconds_t>((seconds - static_cast<double>(tv.tv_sec)) * 1e6);
    if (tv.tv_sec == 0 && tv.tv_usec == 0) tv.tv_usec = 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

#endif

std::optional<sockaddr_in> resolve_ipv4(const std::string& host, int port) {
    start();
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

std::string host_name() {
    start();
    char name[256] = {0};
    if (gethostname(name, sizeof name - 1) != 0) return "";
    return name;
}

}  // namespace ksphmi::net
