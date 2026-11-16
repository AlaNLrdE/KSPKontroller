// A small kRPC client (protocol version 1, as the krpc 0.7 Python client and the kRPC 0.7 mod): one RPC
// connection for calls and one stream connection whose updates a background thread keeps in a table.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "net.hpp"

namespace ksphmi::krpc {

/// The server answered with an error (no such vessel, wrong scene, no target...): the link itself is fine.
struct RpcError : std::runtime_error {
    RpcError(const std::string& description, std::string service_ = "", std::string name_ = "")
        : std::runtime_error(description), service(std::move(service_)), name(std::move(name_)) {}
    std::string service, name;
};

/// The link failed: refused, closed, malformed reply, or timed out.
struct ConnectionError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct TimeoutError : ConnectionError {
    using ConnectionError::ConnectionError;
};

/// One remote procedure call with its already encoded arguments (pb::enc_*), in position order.
struct Call {
    std::string service, procedure;
    std::vector<std::string> args;
};

/// SpaceCenter procedure with its encoded arguments, in position order.
inline Call sc(std::string procedure, std::vector<std::string> args = {}) {
    return {"SpaceCenter", std::move(procedure), std::move(args)};
}

struct Value {
    bool null = false;
    std::string data;  ///< encoded value (pb::dec_*)
};

constexpr uint32_t PROTOCOL_VERSION = 1;

/// ProcedureCall message bytes (also the argument of KRPC.AddStream).
std::string encode_call(const Call& call);
/// A ProcedureResult message: the value, or throws RpcError.
Value decode_result(std::string_view result);

class Client {
public:
    struct Options {
        std::string address = "localhost";
        int rpc_port = 50000, stream_port = 50001;
        std::string name = "KSP HMI";
        double tcp_timeout = 5;         ///< reaching each port, and every RPC reply afterwards
        double handshake_timeout = 20;  ///< the whole connect, so a server that accepts but never answers is caught
    };

    /// Connects both ports. Throws ConnectionError / TimeoutError.
    explicit Client(const Options& options);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    Value invoke(const Call& call);
    /// KRPC.AddStream then, when rate > 0, KRPC.SetStreamRate. Throws RpcError when the call fails now.
    uint64_t add_stream(const Call& call, double rate);
    /// Never throws: removing a stream of a dead connection is a no-op.
    void remove_stream(uint64_t id) noexcept;
    /// Latest value of a stream, waiting up to `wait` seconds for the first one.
    /// Throws RpcError for a server error, ConnectionError if the stream link died or sent no results for 3 s.
    Value stream(uint64_t id, double wait = 5.0);

    /// Wake up and fail every blocked call (used on shutdown).
    void close() noexcept;

private:
    void stream_loop();
    Value invoke_once(const Call& call);

    Options options_;
    net::Fd rpc_, stream_;
    std::thread reader_;
    std::atomic<bool> closing_{false};
    std::atomic<bool> broken_{false};  // an RPC failed at the link level
    std::mutex rpc_mutex_;  // one request/reply at a time on the RPC socket: the telemetry and command threads share it
    std::mutex mutex_;
    std::condition_variable updated_;
    std::map<uint64_t, std::string> latest_;  // stream id -> ProcedureResult bytes
    double stream_received_ = 0;  // monotonic arrival time of the channel's latest results, guarded by mutex_
    std::string stream_error_;
};

}  // namespace ksphmi::krpc
