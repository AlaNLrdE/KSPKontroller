#include "config.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "log.hpp"

namespace ksphmi {

namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

[[noreturn]] void fail(int line, const std::string& what) {
    throw std::runtime_error("line " + std::to_string(line) + ": " + what);
}

// Remove a trailing comment that is not inside a string.
std::string strip_comment(const std::string& s) {
    char quote = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (quote) {
            if (c == '\\' && quote == '"') ++i;
            else if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '#') {
            return s.substr(0, i);
        }
    }
    return s;
}

std::string unquote_key(const std::string& k, int line) {
    if (k.empty()) fail(line, "missing key");
    if (k.size() >= 2 && (k.front() == '"' || k.front() == '\'') && k.back() == k.front())
        return k.substr(1, k.size() - 2);
    for (char c : k)
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) fail(line, "bad key '" + k + "'");
    return k;
}

std::string parse_value(const std::string& v, int line) {
    if (v.empty()) fail(line, "missing value");
    if (v.front() == '\'') {
        if (v.size() < 2 || v.back() != '\'') fail(line, "unterminated string");
        return v.substr(1, v.size() - 2);
    }
    if (v.front() == '"') {
        std::string out;
        size_t i = 1;
        for (; i < v.size() && v[i] != '"'; ++i) {
            if (v[i] != '\\') {
                out += v[i];
                continue;
            }
            if (++i >= v.size()) fail(line, "bad escape");
            switch (v[i]) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                default: fail(line, "unsupported escape");
            }
        }
        if (i != v.size() - 1) fail(line, "unterminated string");
        return out;
    }
    if (v == "true" || v == "false") return v;
    std::string number;
    for (char c : v)
        if (c != '_') number += c;
    char* end = nullptr;
    std::strtod(number.c_str(), &end);
    if (number.empty() || *end) fail(line, "unsupported value '" + v + "'");
    return number;
}

bool to_bool(const TomlValues& m, const std::string& key, bool current) {
    auto it = m.find(key);
    if (it == m.end()) return current;
    if (it->second == "true") return true;
    if (it->second == "false") return false;
    throw std::runtime_error(key + " must be true or false");
}

double to_double(const TomlValues& m, const std::string& key, double current) {
    auto it = m.find(key);
    if (it == m.end()) return current;
    char* end = nullptr;
    double v = std::strtod(it->second.c_str(), &end);
    if (it->second.empty() || *end) throw std::runtime_error(key + " must be a number");
    return v;
}

int to_int(const TomlValues& m, const std::string& key, int current) {
    const double v = to_double(m, key, current);
    if (!std::isfinite(v) || std::trunc(v) != v ||
        v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max())
        throw std::runtime_error(key + " must be an integer within the int range");
    return static_cast<int>(v);
}

std::string to_string(const TomlValues& m, const std::string& key, const std::string& current) {
    auto it = m.find(key);
    return it == m.end() ? current : it->second;
}

bool is_file(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

std::string dirname(const std::string& p) {
    size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? "." : (slash == 0 ? "/" : p.substr(0, slash));
}

// [vab] target and [vab.destinations]: later files add destinations or change their delta-v.
void apply_vab(Config& c, const TomlValues& m) {
    static const std::string prefix = "vab.destinations.";
    for (auto it = m.lower_bound(prefix); it != m.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it) {
        const std::string key = it->first.substr(prefix.size());
        bool plain = !key.empty() && key.size() <= 32;
        for (char ch : key) plain = plain && (std::islower(static_cast<unsigned char>(ch)) || std::isdigit(static_cast<unsigned char>(ch)) || ch == '_');
        if (!plain) throw std::runtime_error(it->first + ": destination names use a-z, 0-9 and _ (at most 32)");
        const double dv = to_double(m, it->first, 0);
        if (!std::isfinite(dv) || dv <= 0 || dv > 100000)
            throw std::runtime_error(it->first + " must be a delta-v in m/s between 0 and 100000");
        auto found = std::find_if(c.vab.destinations.begin(), c.vab.destinations.end(),
                                  [&](const Destination& d) { return d.key == key; });
        if (found != c.vab.destinations.end()) found->delta_v = dv;
        else c.vab.destinations.push_back({key, dv});
    }
    if (static_cast<int>(c.vab.destinations.size()) > MAX_DESTINATIONS)
        throw std::runtime_error("vab.destinations: at most " + std::to_string(MAX_DESTINATIONS) + " destinations");
    std::sort(c.vab.destinations.begin(), c.vab.destinations.end(), [](const Destination& a, const Destination& b) {
        return a.delta_v != b.delta_v ? a.delta_v < b.delta_v : a.key < b.key;
    });
    // TWR colours of the stage table (and the apollo TWR lamps): finite, positive, red limit <= amber limit
    TwrLimits twr = c.vab.twr;
    for (auto [key, field] : {std::pair<const char*, double*>{"vab.launch_twr_min", &twr.launch_bad},
                              {"vab.launch_twr_warn", &twr.launch_warn}, {"vab.upper_twr_warn", &twr.upper_warn}}) {
        const double v = to_double(m, key, *field);
        if (!std::isfinite(v) || v <= 0 || v > 100) throw std::runtime_error(std::string(key) + " must be a TWR between 0 and 100");
        *field = v;
    }
    if (twr.launch_bad > twr.launch_warn)
        throw std::runtime_error("vab.launch_twr_min must not exceed vab.launch_twr_warn");
    c.vab.twr = twr;
    const double atmosphere = to_double(m, "vab.atmosphere_dv", c.vab.atmosphere_dv);
    if (!std::isfinite(atmosphere) || atmosphere < 0 || atmosphere > 100000)
        throw std::runtime_error("vab.atmosphere_dv must be a delta-v in m/s between 0 and 100000");
    c.vab.atmosphere_dv = atmosphere;
    c.vab.target = to_string(m, "vab.target", c.vab.target);
    if (!c.vab.target.empty() &&
        std::none_of(c.vab.destinations.begin(), c.vab.destinations.end(),
                     [&](const Destination& d) { return d.key == c.vab.target; }))
        throw std::runtime_error("vab.target '" + c.vab.target + "' is not one of [vab.destinations]");
}

}  // namespace

TomlValues parse_toml(const std::string& text) {
    TomlValues out;
    std::istringstream in(text);
    std::string raw, table;
    int line = 0;
    while (std::getline(in, raw)) {
        ++line;
        std::string s = trim(strip_comment(raw));
        if (s.empty()) continue;
        if (s.front() == '[') {
            if (s.back() != ']' || s.size() < 3 || s[1] == '[') fail(line, "unsupported table header");
            std::string name = trim(s.substr(1, s.size() - 2));
            table.clear();
            for (size_t at = 0;;) {  // dotted names: each part is a key of its own
                size_t dot = name.find('.', at);
                std::string part = trim(name.substr(at, dot == std::string::npos ? std::string::npos : dot - at));
                table += (table.empty() ? "" : ".") + unquote_key(part, line);
                if (dot == std::string::npos) break;
                at = dot + 1;
            }
            continue;
        }
        size_t eq = s.find('=');
        if (eq == std::string::npos) fail(line, "expected key = value");
        std::string key = unquote_key(trim(s.substr(0, eq)), line);
        out[table.empty() ? key : table + "." + key] = parse_value(trim(s.substr(eq + 1)), line);
    }
    return out;
}

void apply_toml(Config& c, const TomlValues& m) {
    c.source = to_string(m, "source", c.source);
    if (c.source != "krpc" && c.source != "demo" && c.source != "redux" && c.source != "auto")
        throw std::runtime_error("source must be \"krpc\", \"demo\", \"redux\" or \"auto\"");
    c.krpc.address = to_string(m, "krpc.address", c.krpc.address);
    c.krpc.rpc_port = to_int(m, "krpc.rpc_port", c.krpc.rpc_port);
    c.krpc.stream_port = to_int(m, "krpc.stream_port", c.krpc.stream_port);
    c.krpc.rate_hz = to_double(m, "krpc.rate_hz", c.krpc.rate_hz);
    c.krpc.retry_seconds = to_double(m, "krpc.retry_seconds", c.krpc.retry_seconds);
    c.krpc.discovery = to_bool(m, "krpc.discovery", c.krpc.discovery);
    c.demo.speed = to_double(m, "demo.speed", c.demo.speed);
    c.display.output = to_string(m, "display.output", c.display.output);
    c.display.fbdev = to_string(m, "display.fbdev", c.display.fbdev);
    c.display.skin = to_string(m, "display.skin", c.display.skin);
    c.display.fps = to_int(m, "display.fps", c.display.fps);
    c.display.layer_cache = to_string(m, "display.layer_cache", c.display.layer_cache);
    c.display.flight_scene = to_string(m, "display.flight_scene", c.display.flight_scene);
    c.display.fullscreen = to_bool(m, "display.fullscreen", c.display.fullscreen);
    c.display.width = to_int(m, "display.width", c.display.width);
    c.display.height = to_int(m, "display.height", c.display.height);
    c.display.monitor = to_string(m, "display.monitor", c.display.monitor);
    if (!valid_monitor(c.display.monitor))
        throw std::runtime_error("display.monitor must be \"auto\" or a display index such as \"1\"");
    const double day_hours = to_double(m, "format.day_hours", c.format.day_hours);
    if (!std::isfinite(day_hours) || day_hours <= 0 || std::floor(day_hours) != day_hours ||
        day_hours > std::numeric_limits<int>::max())
        throw std::runtime_error("format.day_hours must be a positive integer within the int range");
    c.format.day_hours = static_cast<int>(day_hours);
    apply_vab(c, m);
}

bool valid_monitor(const std::string& setting) {
    if (setting == "auto") return true;
    if (setting.empty() || setting.size() > 2) return false;
    for (char ch : setting)
        if (ch < '0' || ch > '9') return false;
    return true;
}

int monitor_index(const std::string& setting, int displays) {
    if (displays <= 1 || !valid_monitor(setting)) return 0;
    if (setting == "auto") return displays - 1;
    int index = std::stoi(setting);
    return index < displays ? index : 0;
}

std::string find_app_dir() {
    if (const char* env = std::getenv("KSPHMI_APP_DIR")) return env;
    std::vector<std::string> candidates;
    char exe[4096];
#if defined(_WIN32)
    // ksphmi.exe's path as UTF-8 with forward slashes (dirname() and the candidates below use '/').
    wchar_t wide[4096];
    DWORD w = GetModuleFileNameW(nullptr, wide, sizeof wide / sizeof wide[0]);
    long n = w > 0 && w < sizeof wide / sizeof wide[0]
                 ? WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(w), exe, sizeof exe - 1, nullptr, nullptr)
                 : -1;
    for (long i = 0; i < n; ++i)
        if (exe[i] == '\\') exe[i] = '/';
#elif defined(__APPLE__)
    uint32_t size = sizeof exe;
    ssize_t n = _NSGetExecutablePath(exe, &size) == 0 ? static_cast<ssize_t>(std::char_traits<char>::length(exe)) : -1;
#else
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
#endif
    if (n > 0) {
        exe[n] = 0;
        std::string dir = dirname(exe);
        // next to config.toml, one level below it (~/ksphmi/cpp/build on the Pi), or a build tree build/hmi-cpp*
        for (const char* rel : {"", "/..", "/../../hmi-cpp", "/../../../hmi-cpp"}) candidates.push_back(dir + rel);
    }
    candidates.push_back(".");
    candidates.push_back("hmi-cpp");
    for (const auto& d : candidates)
        if (is_file(d + "/config.toml")) return d;
    return ".";
}

Config load_config(const std::string& path) {
    Config cfg;
    std::vector<std::string> files;
    if (!path.empty()) {
        files.push_back(path);
    } else {
        std::string app = find_app_dir();
        files.push_back(app + "/config.toml");
        const char* local = std::getenv("KSPHMI_LOCAL_CONFIG");
        files.push_back(local ? local : app + "/config.local.toml");
    }
    for (const auto& f : files) {
        std::ifstream in(f);
        if (!in) continue;
        std::stringstream buf;
        buf << in.rdbuf();
        try {
            apply_toml(cfg, parse_toml(buf.str()));
        } catch (const std::exception& e) {
            throw std::runtime_error(f + ": " + e.what());
        }
        cfg.loaded.push_back(f);
    }
    if (const char* address = std::getenv("KSPHMI_KRPC_ADDRESS"))
        if (*address) cfg.krpc.address = address;
    return cfg;
}

}  // namespace ksphmi
