#include "config.hpp"

#include <unistd.h>

#include <cstdlib>
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
    return static_cast<int>(to_double(m, key, current));
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
            table = unquote_key(trim(s.substr(1, s.size() - 2)), line);
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
    c.krpc.address = to_string(m, "krpc.address", c.krpc.address);
    c.krpc.rpc_port = to_int(m, "krpc.rpc_port", c.krpc.rpc_port);
    c.krpc.stream_port = to_int(m, "krpc.stream_port", c.krpc.stream_port);
    c.krpc.rate_hz = to_double(m, "krpc.rate_hz", c.krpc.rate_hz);
    c.krpc.retry_seconds = to_double(m, "krpc.retry_seconds", c.krpc.retry_seconds);
    c.krpc.discovery = to_bool(m, "krpc.discovery", c.krpc.discovery);
    c.demo.speed = to_double(m, "demo.speed", c.demo.speed);
    c.display.output = to_string(m, "display.output", c.display.output);
    c.display.fbdev = to_string(m, "display.fbdev", c.display.fbdev);
    c.display.fps = to_int(m, "display.fps", c.display.fps);
    c.display.fullscreen = to_bool(m, "display.fullscreen", c.display.fullscreen);
    c.display.width = to_int(m, "display.width", c.display.width);
    c.display.height = to_int(m, "display.height", c.display.height);
    c.format.day_hours = to_int(m, "format.day_hours", c.format.day_hours);
}

std::string find_app_dir() {
    if (const char* env = std::getenv("KSPHMI_APP_DIR")) return env;
    std::vector<std::string> candidates;
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
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
