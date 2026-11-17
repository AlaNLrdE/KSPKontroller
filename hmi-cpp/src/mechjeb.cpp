#include "mechjeb.hpp"

#include <cmath>
#include <cstdlib>
#include <sstream>

namespace ksphmi {

namespace {

int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void set_error(std::string* error, const std::string& why) {
    if (error) *error = why;
}

}  // namespace

std::string percent_decode(const std::string& s, bool* ok) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') {
            out += s[i];
            continue;
        }
        const int hi = i + 2 < s.size() ? hex(s[i + 1]) : -1, lo = i + 2 < s.size() ? hex(s[i + 2]) : -1;
        if (hi < 0 || lo < 0) {
            if (ok) *ok = false;
            return s;
        }
        out += char(hi * 16 + lo);
        i += 2;
    }
    if (ok) *ok = true;
    return out;
}

std::optional<double> MechJebReport::number(const std::string& key) const {
    auto it = values.find(key);
    if (it == values.end() || it->second.empty()) return std::nullopt;
    const char* begin = it->second.c_str();
    char* end = nullptr;
    const double v = std::strtod(begin, &end);
    if (end == begin || *end != '\0' || !std::isfinite(v)) return std::nullopt;
    return v;
}

std::optional<bool> MechJebReport::flag(const std::string& key) const {
    auto it = values.find(key);
    if (it == values.end()) return std::nullopt;
    if (it->second == "1") return true;
    if (it->second == "0") return false;
    return std::nullopt;
}

std::optional<std::string> MechJebReport::text(const std::string& key) const {
    auto it = values.find(key);
    if (it == values.end()) return std::nullopt;
    return it->second;
}

std::optional<MechJebReport> parse_mechjeb_report(const std::string& text, std::string* error) {
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line)) return set_error(error, "empty report"), std::nullopt;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    MechJebReport r;
    std::optional<int> version;
    std::optional<uint32_t> rev;
    std::optional<std::string> state;
    std::istringstream head(line);
    std::string word;
    while (head >> word) {
        const size_t eq = word.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = word.substr(0, eq), v = word.substr(eq + 1);
        char* end = nullptr;
        if (k == "v") {
            const long n = std::strtol(v.c_str(), &end, 10);
            if (end != v.c_str() && *end == '\0') version = int(n);
        } else if (k == "rev") {
            const unsigned long long n = std::strtoull(v.c_str(), &end, 10);
            if (end != v.c_str() && *end == '\0' && n <= 0xffffffffULL && v[0] != '-') rev = uint32_t(n);
        } else if (k == "mj") {
            state = v;
        } else if (k == "version") {
            r.version = percent_decode(v);
        }
    }
    if (!version) return set_error(error, "no version on the first line"), std::nullopt;
    if (*version != MECHJEB_REPORT_VERSION)
        return set_error(error, "unsupported report version " + std::to_string(*version)), std::nullopt;
    if (!rev) return set_error(error, "no revision on the first line"), std::nullopt;
    if (!state) return set_error(error, "no mj= state on the first line"), std::nullopt;
    r.rev = *rev;
    if (*state == "ok") r.status = MechJebReport::Status::Ok;
    else if (*state == "missing") r.status = MechJebReport::Status::Missing;
    else if (*state == "nocore") r.status = MechJebReport::Status::NoCore;
    else if (state->rfind("error", 0) == 0) {
        r.status = MechJebReport::Status::Error;
        r.error = state->size() > 6 ? percent_decode(state->substr(6)) : "unknown error";
    } else
        return set_error(error, "unknown mj state '" + *state + "'"), std::nullopt;
    int number = 1;
    while (std::getline(in, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0)
            return set_error(error, "line " + std::to_string(number) + " is not key=value"), std::nullopt;
        r.values[line.substr(0, eq)] = percent_decode(line.substr(eq + 1));
    }
    return r;
}

const char* demo_mechjeb_text() {
    return
        "v=1 rev=42 mj=ok version=2.15.3.0\n"
        "ascent.enabled=1\n"
        "ascent.status=Awaiting%20liftoff\n"
        "ascent.t_minus=0\n"
        "ascent.path=0\n"
        "ascent.altitude=80000\n"
        "ascent.inclination=0\n"
        "ascent.lan=0\n"
        "ascent.launch_phase_angle=0\n"
        "ascent.launch_lan_difference=0\n"
        "ascent.warp_count_down=11\n"
        "ascent.classic.auto=0\n"
        "ascent.classic.auto_turn_start_altitude=1450\n"
        "ascent.classic.turn_start_altitude=500\n"
        "ascent.classic.turn_start_velocity=50\n"
        "ascent.classic.turn_end_altitude=45000\n"
        "ascent.classic.turn_end_angle=0\n"
        "ascent.classic.turn_shape_exponent=0.4\n"
        "ascent.pvg.desired_apoapsis=0\n"
        "ascent.pvg.desired_attach_alt=0\n"
        "ascent.pvg.desired_fpa=0\n"
        "ascent.pvg.pitch_start_velocity=50\n"
        "ascent.pvg.pitch_rate=0.5\n"
        "ascent.force_roll=1\n"
        "ascent.vertical_roll=90\n"
        "ascent.turn_roll=90\n"
        "ascent.roll_altitude=50\n"
        "ascent.limit_aoa=1\n"
        "ascent.max_aoa=5\n"
        "ascent.limit_qa=0\n"
        "ascent.max_qa=2000\n"
        "ascent.corrective_steering=0\n"
        "ascent.corrective_steering_gain=0.6\n"
        "ascent.autostage=1\n"
        "ascent.last_stage=0\n"
        "ascent.autodeploy_solar_panels=1\n"
        "ascent.autodeploy_antennas=1\n"
        "ascent.skip_circularization=0\n"
        "thrust.limit_q=1\n"
        "thrust.max_q=20000\n"
        "thrust.limit_overheat=1\n"
        "thrust.limit_acceleration=0\n"
        "thrust.max_acceleration=40\n"
        "thrust.limit_throttle=0\n"
        "thrust.max_throttle=1\n"
        "thrust.limit_min_throttle=0\n"
        "thrust.min_throttle=0.05\n"
        "thrust.smooth_throttle=0\n"
        "thrust.prevent_flameout=0\n"
        "thrust.flameout_safety_pct=0.05\n"
        "thrust.prevent_unstable_ignition=0\n"
        "thrust.differential_throttle=0\n"
        "staging.autostage_limit=0\n"
        "staging.pre_delay=0.5\n"
        "staging.post_delay=1\n"
        "staging.clamp_thrust_pct=0.99\n"
        "staging.hot_staging=0\n"
        "staging.hot_staging_lead_time=1\n"
        "staging.drop_solids=0\n"
        "staging.drop_solids_lead_time=1\n"
        "staging.autostaging_once=0\n"
        "staging.fairing_max_q=50000\n"
        "staging.fairing_min_altitude=50000\n"
        "staging.fairing_max_flux=1135\n";
}

std::shared_ptr<const MechJebReport> demo_mechjeb_report() {
    static const std::shared_ptr<const MechJebReport> report = [] {
        auto parsed = parse_mechjeb_report(demo_mechjeb_text());
        return parsed ? std::make_shared<const MechJebReport>(std::move(*parsed)) : nullptr;
    }();
    return report;
}

}  // namespace ksphmi
