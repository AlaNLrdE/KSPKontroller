// Settings: hmi-cpp/config.toml (the defaults), then config.local.toml (KSPHMI_LOCAL_CONFIG),
// then the KSPHMI_KRPC_ADDRESS environment variable written by ./ci deploy_hmi.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace ksphmi {

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
        int fps = 10;
        bool fullscreen = true;
        int width = 0, height = 0;
    } display;
    struct {
        int day_hours = 6;
    } format;
    std::vector<std::string> loaded;  ///< files read, in order
};

/// Flat view of a TOML document: "table.key" -> raw scalar (strings unquoted, numbers and booleans as text).
using TomlValues = std::map<std::string, std::string>;

/// Parse the TOML subset the settings use: [tables], key = "string" | 'literal' | number | true/false, comments.
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
