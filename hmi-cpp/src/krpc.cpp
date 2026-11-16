#include "krpc.hpp"

#if !defined(_WIN32)
#include <netinet/tcp.h>
#endif

#include <chrono>
#include <cmath>
#include <cstring>

#include "fmt.hpp"
#include "model.hpp"
#include "pb.hpp"

namespace ksphmi::krpc {

namespace {

constexpr double NO_DEADLINE = INFINITY;
constexpr double STREAM_IDLE_LIMIT = 3.0;

int wait_ms(double deadline, double slice) {
    double left = deadline - monotonic();
    if (left <= 0) return 0;
    return static_cast<int>(std::ceil(std::min(left, slice) * 1000));
}

net::Fd connect_tcp(const std::string& address, int port, double timeout, double deadline) {
    auto addr = net::resolve_ipv4(address, port);
    if (!addr) throw ConnectionError("cannot resolve '" + address + "'");
    net::Fd fd(net::open_socket(SOCK_STREAM));
    if (!fd) throw ConnectionError(net::error_text(net::last_error()));
    net::set_nonblocking(fd.get(), true);
    if (connect(fd.get(), reinterpret_cast<const sockaddr*>(&*addr), sizeof *addr) != 0) {
        const int e = net::last_error();
        if (!net::connect_pending(e)) throw ConnectionError(net::error_text(e));
        double until = std::min(deadline, monotonic() + timeout);
        int r;
        do r = net::wait(fd.get(), true, wait_ms(until, 3600));
        while (r < 0 && net::interrupted(net::last_error()));
        if (r == 0) throw TimeoutError("timed out");
        int err = 0;
        socklen_t len = sizeof err;
        getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
        if (err) throw ConnectionError(net::error_text(err));
    }
    net::set_nonblocking(fd.get(), false);
    int yes = 1;
    setsockopt(fd.get(), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof yes);
    return fd;
}

void send_all(net::socket_t fd, const std::string& data, double deadline) {
    size_t sent = 0;
    while (sent < data.size()) {
        int r = net::wait(fd, true, wait_ms(deadline, 3600));
        if (r == 0) throw TimeoutError("timed out sending to kRPC");
        if (r < 0) {
            const int e = net::last_error();
            if (net::interrupted(e)) continue;
            throw ConnectionError(net::error_text(e));
        }
        long n = net::send_some(fd, data.data() + sent, data.size() - sent);
        if (n < 0) {
            const int e = net::last_error();
            if (net::interrupted(e) || net::would_block(e)) continue;
            throw ConnectionError(net::error_text(e));
        }
        sent += static_cast<size_t>(n);
    }
}

void send_message(net::socket_t fd, const std::string& message, double deadline) {
    send_all(fd, pb::varint(message.size()) + message, deadline);
}

// Reads exactly n bytes. Wakes up regularly to honour `closing`.
std::string recv_exact(net::socket_t fd, size_t n, double deadline, const std::atomic<bool>& closing, const char* timeout_text) {
    std::string out;
    out.resize(n);
    size_t got = 0;
    while (got < n) {
        if (closing) throw ConnectionError("connection closed");
        if (monotonic() >= deadline) throw TimeoutError(timeout_text);
        int r = net::wait(fd, false, wait_ms(deadline, 0.25));
        if (r < 0) {
            const int e = net::last_error();
            if (!net::interrupted(e)) throw ConnectionError(net::error_text(e));
        }
        if (r <= 0) continue;
        long k = net::recv_some(fd, &out[got], n - got);
        if (k == 0) throw ConnectionError("connection closed by the server");
        if (k < 0) {
            const int e = net::last_error();
            if (net::interrupted(e) || net::would_block(e)) continue;
            throw ConnectionError(net::error_text(e));
        }
        got += static_cast<size_t>(k);
    }
    return out;
}

std::string recv_message(net::socket_t fd, double deadline, const std::atomic<bool>& closing,
                         const char* timeout_text = "timed out waiting for kRPC") {
    std::string prefix;
    for (;;) {
        prefix += recv_exact(fd, 1, deadline, closing, timeout_text);
        if (!(static_cast<uint8_t>(prefix.back()) & 0x80)) break;
        if (prefix.size() > 10) throw ConnectionError("malformed message size");
    }
    size_t used;
    uint64_t size = pb::read_varint(prefix, used);
    if (size > (64u << 20)) throw ConnectionError("message too large");
    return recv_exact(fd, static_cast<size_t>(size), deadline, closing, timeout_text);
}

RpcError decode_error(std::string_view message) {
    std::string service, name, description;
    pb::Reader r(message);
    while (r.next()) {
        if (r.wire() != pb::Bytes) continue;
        if (r.field() == 1) service = std::string(r.bytes_value());
        else if (r.field() == 2) name = std::string(r.bytes_value());
        else if (r.field() == 3) description = std::string(r.bytes_value());
    }
    return RpcError(description.empty() ? name : description, service, name);
}

std::string connection_request(int type, const std::string& name, const std::string& identifier) {
    pb::Writer w;
    if (type) w.field_varint(1, static_cast<uint64_t>(type));
    if (!name.empty()) w.field_bytes(2, name);
    if (!identifier.empty()) w.field_bytes(3, identifier);
    w.field_varint(4, PROTOCOL_VERSION);
    return w.out;
}

// Returns the client identifier; throws on a refused handshake.
std::string check_response(const std::string& message, bool check_version) {
    uint64_t status = 0, version = 0;
    std::string text, identifier;
    pb::Reader r(message);
    while (r.next()) {
        if (r.field() == 1 && r.wire() == pb::Varint) status = r.varint_value();
        else if (r.field() == 2 && r.wire() == pb::Bytes) text = std::string(r.bytes_value());
        else if (r.field() == 3 && r.wire() == pb::Bytes) identifier = std::string(r.bytes_value());
        else if (r.field() == 4 && r.wire() == pb::Varint) version = r.varint_value();
    }
    if (status != 0) throw ConnectionError(text.empty() ? "connection refused by kRPC" : text);
    if (check_version && version != PROTOCOL_VERSION)
        throw ConnectionError("The server does not support protocol version 1. Use a client from the same kRPC "
                              "release as the server.");
    return identifier;
}

}  // namespace

std::string encode_call(const Call& call) {
    pb::Writer w;
    w.field_bytes(1, call.service);
    w.field_bytes(2, call.procedure);
    for (size_t i = 0; i < call.args.size(); ++i) {
        pb::Writer a;
        if (i) a.field_varint(1, i);
        a.field_bytes(2, call.args[i]);
        w.field_bytes(3, a.out);
    }
    return w.out;
}

Value decode_result(std::string_view result) {
    Value v;
    pb::Reader r(result);
    while (r.next()) {
        if (r.field() == 1 && r.wire() == pb::Bytes) throw decode_error(r.bytes_value());
        if (r.field() == 2 && r.wire() == pb::Bytes) v.data = std::string(r.bytes_value());
        if (r.field() == 3 && r.wire() == pb::Varint) v.null = r.varint_value() != 0;
    }
    return v;
}

Client::Client(const Options& options) : options_(options) {
    double start = monotonic();
    double deadline = start + options.handshake_timeout;
    std::string timeout_text = fmt::str("connected but no answer to the handshake within %.0f s (kRPC busy, paused or "
                                        "misconfigured)",
                                        options.handshake_timeout);
    rpc_ = connect_tcp(options.address, options.rpc_port, options.tcp_timeout, deadline);
    send_message(rpc_.get(), connection_request(0, options.name, ""), deadline);
    std::string identifier =
        check_response(recv_message(rpc_.get(), deadline, closing_, timeout_text.c_str()), true);
    stream_ = connect_tcp(options.address, options.stream_port, options.tcp_timeout, deadline);
    send_message(stream_.get(), connection_request(1, "", identifier), deadline);
    check_response(recv_message(stream_.get(), deadline, closing_, timeout_text.c_str()), false);
    reader_ = std::thread([this] { stream_loop(); });
}

Client::~Client() {
    close();
    if (reader_.joinable()) reader_.join();
}

void Client::close() noexcept {
    closing_ = true;
    if (rpc_) net::shutdown_both(rpc_.get());
    if (stream_) net::shutdown_both(stream_.get());
    updated_.notify_all();
}

Value Client::invoke(const Call& call) {
    try {
        return invoke_once(call);
    } catch (const RpcError&) {
        throw;
    } catch (...) {
        broken_ = true;
        throw;
    }
}

Value Client::invoke_once(const Call& call) {
    pb::Writer request;
    request.field_bytes(1, encode_call(call));
    double deadline = monotonic() + options_.tcp_timeout;
    send_message(rpc_.get(), request.out, deadline);
    std::string response = recv_message(rpc_.get(), deadline, closing_);
    std::string_view first;
    bool have = false;
    pb::Reader r(response);
    while (r.next()) {
        if (r.field() == 1 && r.wire() == pb::Bytes) throw decode_error(r.bytes_value());
        if (r.field() == 2 && r.wire() == pb::Bytes && !have) first = r.bytes_value(), have = true;
    }
    if (!have) throw ConnectionError("kRPC response without a result");
    return decode_result(first);
}

uint64_t Client::add_stream(const Call& call, double rate) {
    Value v = invoke({"KRPC", "AddStream", {encode_call(call)}});
    uint64_t id = 0;
    pb::Reader r(v.data);
    while (r.next())
        if (r.field() == 1 && r.wire() == pb::Varint) id = r.varint_value();
    if (rate > 0) {
        try {
            invoke({"KRPC", "SetStreamRate", {pb::enc_object(id), pb::enc_float(static_cast<float>(rate))}});
        } catch (const RpcError&) {  // very old servers have no per-stream rate
        }
    }
    return id;
}

void Client::remove_stream(uint64_t id) noexcept {
    if (broken_ || closing_) return;  // a dead link would make every removal wait for its timeout
    try {
        invoke({"KRPC", "RemoveStream", {pb::enc_object(id)}});
    } catch (...) {
    }
    std::lock_guard<std::mutex> lock(mutex_);
    latest_.erase(id);
}

Value Client::stream(uint64_t id, double wait) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto ready = [&] { return latest_.count(id) || !stream_error_.empty() || closing_; };
    if (!updated_.wait_for(lock, std::chrono::duration<double>(wait), ready))
        throw TimeoutError("no value from kRPC stream within " + fmt::str("%.0f s", wait));
    if (stream_error_.empty() && !latest_.empty() && monotonic() - stream_received_ > STREAM_IDLE_LIMIT) {
        stream_error_ = "stream channel inactive for 3 s";
        updated_.notify_all();
    }
    if (!stream_error_.empty()) throw ConnectionError(stream_error_);
    auto it = latest_.find(id);
    if (it == latest_.end()) throw ConnectionError(stream_error_.empty() ? "connection closed" : stream_error_);
    std::string result = it->second;
    lock.unlock();
    return decode_result(result);
}

void Client::stream_loop() {
    try {
        while (!closing_) {
            std::string update = recv_message(stream_.get(), NO_DEADLINE, closing_);
            std::vector<std::pair<uint64_t, std::string>> results;
            pb::Reader r(update);
            while (r.next()) {
                if (r.field() != 1 || r.wire() != pb::Bytes) continue;
                uint64_t id = 0;
                std::string result;
                pb::Reader s(r.bytes_value());
                while (s.next()) {
                    if (s.field() == 1 && s.wire() == pb::Varint) id = s.varint_value();
                    else if (s.field() == 2 && s.wire() == pb::Bytes) result = std::string(s.bytes_value());
                }
                results.emplace_back(id, std::move(result));
            }
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [id, result] : results) latest_[id] = std::move(result);
            if (!results.empty()) stream_received_ = monotonic();
            updated_.notify_all();
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(mutex_);
        stream_error_ = std::string("stream connection lost: ") + e.what();
        updated_.notify_all();
    }
}

}  // namespace ksphmi::krpc
