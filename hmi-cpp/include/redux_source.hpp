// KSP 2 Redux as a flight data source: the KSP2 Redux workbench's KSPKontroller plugin answers UDP requests on port
// 50003 (docs/hmi.rst "KSP 2 Redux source"). One request, one nonce-matched reply; no kRPC involved. With
// source = "auto" the kRPC source runs too and is published whenever Redux has not answered for a second, so one
// HMI serves KSP 1 and KSP 2.
//
// Requests and replies (ASCII; labels hex-encoded in snapshots):
//   KSP2HMI1|KSP2HMI2 <nonce>        -> scalar snapshot (v2 adds navigation and native resources)
//   KSP2SHIP <nonce>                 -> "\n" + VAB ship report v1, or "-"
//   KSP2VSHP|KSP2VSTA <nonce>        -> "\n" + VESSEL shape / state v1, or "-"
//   KSP2KEY <nonce> <name> <value>   -> "queued" | "disabled" | "invalid" | "busy"  (remote keyboard, one-shot)
//   KSP2HOLD <nonce> p y r x y z t   -> "queued" | "disabled" | "invalid"          (held keys, repeated per tick)
#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "net.hpp"
#include "shipreport.hpp"
#include "telemetry.hpp"
#include "vesselview.hpp"

namespace ksphmi {

/// Decode one nonce-matched snapshot reply. Unsupported optional panels stay empty. Throws on anything invalid.
Snapshot decode_redux(const std::string& packet, const std::string& nonce, double received);
Quat redux_rotation(Vec3 forward, Vec3 up);
/// Redux GameState name -> the HMI's facility view key; unknown states are returned unchanged.
std::string redux_scene(const std::string& state, bool flight);

/// "<prefix> <nonce>\n<text>": the text, or nullopt for "-" (the game answers without one). Throws on a foreign
/// nonce, another prefix or an oversized reply.
std::optional<std::string> decode_redux_text(const std::string& packet, const std::string& prefix, const std::string& nonce);
/// "KSP2SHIP <nonce>\n" + ship report v1 text, or "-" when the game has no editor report (null result).
std::shared_ptr<const ShipReport> decode_redux_ship(const std::string& packet, const std::string& nonce);
/// "<prefix> <nonce> <status>": the status word. Throws on a foreign nonce or malformed reply.
std::string decode_redux_ack(const std::string& packet, const std::string& prefix, const std::string& nonce);

/// The remote keyboard on the wire: a command's request ("KSP2KEY 7 group 10"), the held keys ("KSP2HOLD 8 -1 0 0 0
/// 0 0 1": pitch yaw roll x y z, then the throttle ramp direction -1/0/1).
std::string redux_key_request(const Command& c, const std::string& nonce);
std::string redux_hold_request(const Held& h, const std::string& nonce);

struct ReduxReply {
    Snapshot snapshot;
    bool version2 = false;
};
/// One request/reply on a connected UDP socket. Late replies to earlier nonces are discarded, before sending and
/// while waiting, so one delay cannot desynchronize later polls.
std::optional<ReduxReply> redux_exchange(net::socket_t fd, const std::string& request, const std::string& nonce, double timeout);
/// nullopt: no valid reply. A reply holding null: the game answered without an editor report.
std::optional<std::shared_ptr<const ShipReport>> redux_ship_exchange(net::socket_t fd, const std::string& nonce, double timeout);
/// A text reply (KSP2VSHP / KSP2VSTA): nullopt = no valid reply; an empty optional inside = "-".
std::optional<std::optional<std::string>> redux_text_exchange(net::socket_t fd, const std::string& prefix, const std::string& nonce,
                                                              double timeout);
/// A keyboard request's status word, nullopt without a valid reply.
std::optional<std::string> redux_ack_exchange(net::socket_t fd, const std::string& request, const std::string& prefix,
                                              const std::string& nonce, double timeout);

class ReduxSource final : public Source {
public:
    /// `fallback`: also run the kRPC source and publish it whenever Redux has not answered for a second.
    ReduxSource(KrpcSource::Options options, bool fallback);
    ~ReduxSource() override { stop(); }
    void command(const Command& c) override;
    void hold(const Held& h) override;

protected:
    void run() override;

private:
    void send_inputs(net::socket_t fd, uint64_t& sequence);
    KrpcSource::Options options_;
    bool fallback_;
    std::mutex input_mutex_;
    bool linked_ = false;                 ///< Redux answering (input_mutex_)
    KrpcSource* krpc_ = nullptr;          ///< the fallback while it runs (input_mutex_)
    std::vector<Command> commands_;       ///< keyboard commands for the Redux thread (input_mutex_)
    Held held_;                           ///< held keys as the display thread last saw them (input_mutex_)
    Held sent_;                           ///< held keys last sent (Redux thread)
    std::string held_problem_;            ///< last held-key problem logged (Redux thread)
};

}  // namespace ksphmi
