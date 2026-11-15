// Settings: hmi-cpp/config.toml (the defaults), then config.local.toml (KSPHMI_LOCAL_CONFIG),
// then the KSPHMI_KRPC_ADDRESS environment variable written by ./ci deploy_hmi.
#pragma once

#include <map>
#include "shipreport.hpp"
#include <string>
#include <vector>

namespace ksphmi {

/// A mission the VAB report compares the ship's delta-v with: config key ("mun_landing") and m/s from the pad.
struct Destination {
    std::string key;
    double delta_v = 0;
};

/// Most destinations the VAB screens have room for.
constexpr int MAX_DESTINATIONS = 12;

struct Config {
    std::string source = "krpc";  // krpc | demo
    struct {
        std::string address = "localhost";
        int rpc_port = 50000, stream_port = 50001;
        double rate_hz = 10, retry_seconds = 3;
        bool discovery = true;
    } krpc;
    struct {
        double speed = 20;
    } demo;
    struct {
        std::string output = "sdl", fbdev = "/dev/fb0";
        std::string skin = "apollo";   ///< apollo (default) | classic
        int fps = 10;
        /// auto | on | off: compose NavBall-only animation frames over a cached render target (auto = GPU only)
        std::string layer_cache = "off";
        bool fullscreen = true;
        int width = 0, height = 0;
    } display;
    struct {
        int day_hours = 6;
    } format;
    /// [vab]: the highlighted destination and [vab.destinations] (key = m/s), sorted by delta-v then key.
    struct {
        std::string target;
        std::vector<Destination> destinations;
        TwrLimits twr;  ///< launch_twr_min, launch_twr_warn, upper_twr_warn
    } vab;
    std::vector<std::string> loaded;  ///< files read, in order
};

/// Flat view of a TOML document: "table.key" -> raw scalar (strings unquoted, numbers and booleans as text).
using TomlValues = std::map<std::string, std::string>;

/// Parse the TOML subset the settings use: [tables] (dotted names too, [vab.destinations]), key = "string" |
/// 'literal' | number | true/false, comments.
/// Throws std::runtime_error with the line number on anything else.
TomlValues parse_toml(const std::string& text);

/// Apply parsed values over cfg; unknown keys are ignored, malformed values throw std::runtime_error.
void apply_toml(Config& cfg, const TomlValues& values);

/// config.toml next to the application (see find_app_dir), then the local file, then the environment.
/// With `path` (--config), only that file (plus the environment).
Config load_config(const std::string& path = "");

/// Directory holding config.toml: $KSPHMI_APP_DIR, else next to / above the executable, else the source tree.
std::string find_app_dir();

}  // namespace ksphmi
