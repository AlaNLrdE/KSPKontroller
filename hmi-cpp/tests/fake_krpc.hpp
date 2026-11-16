// An in-process kRPC server for client tests: real handshake, Request/Response and StreamUpdate messages.
// Procedures are answered by a handler; tools/kspsim covers the full SpaceCenter behaviour end to end.
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net.hpp"
#include "pb.hpp"

struct FakeResult {
    std::string value;
    bool null = false;
    std::string error;  ///< non-empty: answer with an Error of this description
};

class FakeServer {
public:
    using Handler = std::function<FakeResult(const std::string& procedure, const std::vector<std::string>& args)>;

    explicit FakeServer(Handler handler) : handler_(std::move(handler)) {
        rpc_listener_ = listen_any(rpc_port);
        stream_listener_ = listen_any(stream_port);
        threads_.emplace_back([this] { serve_rpc(); });
        threads_.emplace_back([this] { serve_stream(); });
    }

    ~FakeServer() {
        stop_ = true;
        for (Socket fd : {rpc_listener_, stream_listener_, rpc_client_.load(), stream_client_.load()})
            if (fd != ksphmi::net::NO_SOCKET) ksphmi::net::shutdown_both(fd), ksphmi::net::close_socket(fd);
        for (auto& t : threads_) t.join();
    }

    /// Disconnect only the stream transport; keep RPC available for liveness probes.
    void disconnect_stream() {
        Socket fd = stream_client_.load();
        if (fd != ksphmi::net::NO_SOCKET) ksphmi::net::shutdown_both(fd);
    }

    int rpc_port = 0, stream_port = 0;
    std::atomic<int> calls{0};
    /// AddStream answers the call's error when it fails at once (real kRPC); off keeps older tests' streams.
    std::atomic<bool> fail_add_stream{false};
    std::atomic<bool> pause_streams{false};  ///< keep both sockets open while withholding stream updates

private:
    using Socket = ksphmi::net::socket_t;

    static Socket listen_any(int& port) {
        Socket fd = ksphmi::net::open_socket(SOCK_STREAM);
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof yes);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
        listen(fd, 4);
        socklen_t len = sizeof a;
        getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        return fd;
    }

    static bool recv_exact(Socket fd, std::string& out, size_t n) {
        out.resize(n);
        size_t got = 0;
        while (got < n) {
            long k = ksphmi::net::recv_some(fd, &out[got], n - got);
            if (k <= 0) return false;
            got += static_cast<size_t>(k);
        }
        return true;
    }

    static bool recv_message(Socket fd, std::string& out) {
        std::string prefix, byte;
        do {
            if (!recv_exact(fd, byte, 1)) return false;
            prefix += byte;
        } while (static_cast<uint8_t>(byte[0]) & 0x80);
        size_t used;
        return recv_exact(fd, out, ksphmi::pb::read_varint(prefix, used));
    }

    static void send_message(Socket fd, const std::string& m) {
        std::string data = ksphmi::pb::varint(m.size()) + m;
        ksphmi::net::send_some(fd, data.data(), data.size());
    }

    static std::string ok_response(const std::string& identifier) {
        ksphmi::pb::Writer w;
        if (!identifier.empty()) w.field_bytes(3, identifier);
        w.field_varint(4, 1);
        return w.out;
    }

    struct Parsed {
        std::string procedure;
        std::vector<std::string> args;
    };

    static Parsed parse_call(std::string_view call) {
        Parsed p;
        ksphmi::pb::Reader r(call);
        while (r.next()) {
            if (r.field() == 2) p.procedure = std::string(r.bytes_value());
            if (r.field() == 3) {
                ksphmi::pb::Reader a(r.bytes_value());
                uint64_t position = 0;
                std::string value;
                while (a.next()) {
                    if (a.field() == 1) position = a.varint_value();
                    if (a.field() == 2) value = std::string(a.bytes_value());
                }
                if (p.args.size() <= position) p.args.resize(position + 1);
                p.args[position] = value;
            }
        }
        return p;
    }

    std::string result(const Parsed& p) {
        FakeResult res;
        if (p.procedure == "AddStream") {
            Parsed inner = parse_call(p.args.at(0));
            res = handler_(inner.procedure, inner.args);  // like kRPC: a call that fails cannot become a stream
            if (!res.error.empty() && fail_add_stream) return encode(res);
            res = FakeResult{};
            std::lock_guard<std::mutex> lock(mutex_);
            uint64_t id = next_++;
            streams_[id] = inner;
            ksphmi::pb::Writer s;
            s.field_varint(1, id);
            res.value = s.out;
        } else if (p.procedure == "SetStreamRate" || p.procedure == "RemoveStream") {
            res.null = true;
        } else {
            res = handler_(p.procedure, p.args);
        }
        return encode(res);
    }

    static std::string encode(const FakeResult& res) {
        ksphmi::pb::Writer w;
        if (!res.error.empty()) {
            ksphmi::pb::Writer e;
            e.field_bytes(1, "SpaceCenter");
            e.field_bytes(3, res.error);
            w.field_bytes(1, e.out);
        } else if (res.null) {
            w.field_varint(3, 1);
        } else {
            w.field_bytes(2, res.value);
        }
        return w.out;
    }

    void serve_rpc() {
        Socket c = accept(rpc_listener_, nullptr, nullptr);
        if (c == ksphmi::net::NO_SOCKET) return;
        rpc_client_ = c;
        std::string m;
        if (!recv_message(c, m)) return;
        send_message(c, ok_response("fake-client-id"));
        while (!stop_ && recv_message(c, m)) {
            ksphmi::pb::Writer response;
            ksphmi::pb::Reader r(m);
            while (r.next())
                if (r.field() == 1) response.field_bytes(2, result(parse_call(r.bytes_value())));
            ++calls;
            send_message(c, response.out);
        }
    }

    void serve_stream() {
        Socket c = accept(stream_listener_, nullptr, nullptr);
        if (c == ksphmi::net::NO_SOCKET) return;
        stream_client_ = c;
        std::string m;
        if (!recv_message(c, m)) return;
        send_message(c, ok_response(""));
        while (!stop_) {
            if (pause_streams) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            ksphmi::pb::Writer update;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const auto& [id, call] : streams_) {
                    ksphmi::pb::Writer one;
                    one.field_varint(1, id);
                    one.field_bytes(2, result_unlocked(call));
                    update.field_bytes(1, one.out);
                }
            }
            if (!update.out.empty()) send_message(c, update.out);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    std::string result_unlocked(const Parsed& p) {  // stream calls never add streams
        FakeResult res = handler_(p.procedure, p.args);
        ksphmi::pb::Writer w;
        if (!res.error.empty()) {
            ksphmi::pb::Writer e;
            e.field_bytes(3, res.error);
            w.field_bytes(1, e.out);
        } else if (res.null) {
            w.field_varint(3, 1);
        } else {
            w.field_bytes(2, res.value);
        }
        return w.out;
    }

    Handler handler_;
    Socket rpc_listener_ = ksphmi::net::NO_SOCKET, stream_listener_ = ksphmi::net::NO_SOCKET;
    std::atomic<Socket> rpc_client_{ksphmi::net::NO_SOCKET}, stream_client_{ksphmi::net::NO_SOCKET};
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    std::map<uint64_t, Parsed> streams_;
    uint64_t next_ = 1;
    std::vector<std::thread> threads_;
};
