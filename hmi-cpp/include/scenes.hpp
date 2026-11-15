// Scene context supplied by KSPSerialIO on UDP 50002 (port of scenes.py and discovery.py); independent of the
// flight telemetry link. Protocol: request "KSPHMI/1", reply "KSPHMI/1 <scene>".
#pragma once

#include <array>
#include <string>
#include <vector>

namespace ksphmi {

struct Card {
    const char* label;
    const char* heading;
    const char* hint;
};

struct View {
    const char* key;
    const char* title;
    const char* subtitle;
    std::array<Card, 3> cards;
};

/// Each facility view has a distinct purpose and three task cards. No simulated mission data is shown live.
const std::vector<View>& views();
/// The view for a scene name, or nullptr.
const View* find_view(const std::string& scene);

/// A context reply: "KSPHMI/1 <scene>[ skin=<name>]". The simulator adds the optional skin field (the HMI look it
/// should use); KSPSerialIO sends none, so in the game the [display] skin setting decides.
struct Context {
    std::string scene;  ///< a known facility view, or "" (flight, main menu, unknown)
    std::string skin;   ///< "" when the reply has no skin field
};
/// Parse a reply: "" fields for anything but "KSPHMI/1 <scene>[ skin=<name>]" in ASCII (unknown scenes give "").
Context parse_context_reply(const std::string& reply);
/// Scene name from a reply, or "" for anything but "KSPHMI/1 <known scene>[ fields]" in ASCII.
std::string parse_context(const std::string& reply);

/// One bounded request (0.15 s). An older plugin or a blocked UDP port leaves the generic waiting view.
Context read_context_reply(const std::string& address, int port = 50002);
std::string read_context(const std::string& address, int port = 50002);

/// Only private, link-local or loopback IPv4 peers may redirect the HMI (Python ipaddress.is_private rules).
bool is_local(const std::string& address);

/// Broadcast the context request and return the first local server that answers, or "".
/// Set discovery = false in config.toml on an untrusted network: any LAN host answering could redirect it.
std::string discover(int port = 50002, double timeout = 0.5,
                     const std::vector<std::string>& targets = {"255.255.255.255"});

}  // namespace ksphmi
