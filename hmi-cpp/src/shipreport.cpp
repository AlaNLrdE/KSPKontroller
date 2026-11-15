#include "shipreport.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace ksphmi {

namespace {

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t at = 0;
    while (true) {
        size_t next = s.find(sep, at);
        out.push_back(s.substr(at, next == std::string::npos ? std::string::npos : next - at));
        if (next == std::string::npos) return out;
        at = next + 1;
    }
}

// A finite or "nan" number (strtod also accepts inf and hex: refuse those, the plugin never sends them).
bool number(const std::string& s, double& out) {
    if (s == "nan") {
        out = std::numeric_limits<double>::quiet_NaN();
        return true;
    }
    if (s.empty() || s.size() > 32) return false;
    for (char c : s)
        if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '.' || c == '+' || c == 'e' || c == 'E'))
            return false;
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return *end == 0 && std::isfinite(out);
}

bool integer(const std::string& s, long long lo, long long hi, long long& out) {
    if (s.empty() || s.size() > 19) return false;
    size_t i = s[0] == '-' ? 1 : 0;
    if (i == s.size()) return false;
    for (; i < s.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    out = std::strtoll(s.c_str(), nullptr, 10);
    return out >= lo && out <= hi;
}

bool count(const std::string& s, std::optional<int>& out) {
    long long v;
    if (!integer(s, 0, 1000000, v)) return false;
    out = static_cast<int>(v);
    return true;
}

bool optional_number(const std::string& s, std::optional<double>& out) {
    double v;
    if (!number(s, v)) return false;
    if (std::isnan(v)) out.reset();
    else out = v;
    return true;
}

struct Fail {
    std::string what;
};

double finite_sum(const std::vector<StageReport>& stages, double StageReport::*field) {
    double total = 0;
    for (const auto& s : stages)
        if (std::isfinite(s.*field)) total += s.*field;
    return total;
}

}  // namespace

double ShipReport::total_delta_v() const { return finite_sum(stages, &StageReport::delta_v); }
double ShipReport::total_delta_v_vac() const { return finite_sum(stages, &StageReport::delta_v_vac); }

std::string ShipReport::situation_label() const {
    static const std::pair<const char*, const char*> suffixes[] = {
        {"_asl", "SEA LEVEL"}, {"_alt", "ALTITUDE"}, {"_vac", "VACUUM"}};
    for (const auto& [suffix, label] : suffixes) {
        const size_t n = std::char_traits<char>::length(suffix);
        if (situation.size() > n && situation.compare(situation.size() - n, n, suffix) == 0)
            return destination_label(situation.substr(0, situation.size() - n)) + " \xC2\xB7 " + label;
    }
    return destination_label(situation);
}

std::optional<std::string> percent_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') {
            out += s[i];
            continue;
        }
        if (i + 2 >= s.size()) return std::nullopt;  // needs two hex digits after '%'
        if (!std::isxdigit(static_cast<unsigned char>(s[i + 1])) || !std::isxdigit(static_cast<unsigned char>(s[i + 2])))
            return std::nullopt;
        out += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
        i += 2;
    }
    return out;
}

std::string destination_label(const std::string& key) {
    std::string out = key;
    for (char& c : out) c = c == '_' ? ' ' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

int launch_stage_index(const ShipReport& report) {
    if (report.delta_v_pending) return -1;
    for (size_t i = 0; i < report.stages.size(); ++i)
        if (std::isfinite(report.stages[i].delta_v) && report.stages[i].delta_v > 0) return static_cast<int>(i);
    return -1;
}

TwrLevel twr_level(const ShipReport& report, size_t index, const TwrLimits& limits) {
    if (report.delta_v_pending || index >= report.stages.size()) return TwrLevel::None;
    const StageReport& st = report.stages[index];
    if (!std::isfinite(st.twr) || !std::isfinite(st.delta_v) || st.delta_v <= 0) return TwrLevel::None;
    const int launch = launch_stage_index(report);
    if (static_cast<int>(index) == launch) {
        if (st.twr < limits.launch_bad) return TwrLevel::Bad;
        return st.twr < limits.launch_warn ? TwrLevel::Warn : TwrLevel::Ok;
    }
    // stages fired before the launch stage cannot have delta-v (it is the first with some): only later ones here
    return st.twr < limits.upper_warn ? TwrLevel::Warn : TwrLevel::Ok;
}

TwrLevel launch_twr_level(const ShipReport& report, const TwrLimits& limits) {
    const int launch = launch_stage_index(report);
    return launch < 0 ? TwrLevel::None : twr_level(report, static_cast<size_t>(launch), limits);
}

TwrLevel upper_twr_level(const ShipReport& report, const TwrLimits& limits) {
    TwrLevel worst = TwrLevel::None;
    const int launch = launch_stage_index(report);
    for (size_t i = static_cast<size_t>(launch + 1); launch >= 0 && i < report.stages.size(); ++i)
        worst = std::max(worst, twr_level(report, i, limits));
    return worst;
}

std::string events_text(const StageReport& stage) {
    static const std::pair<const char*, const char*> names[] = {
        {"engines", "ENG"}, {"decouplers", "DECOUPLE"}, {"chutes", "CHUTE"}, {"clamps", "CLAMP"}, {"fairings", "FAIRING"}};
    std::string out;
    for (const auto& e : stage.events) {
        if (e.count <= 0) continue;
        std::string name = destination_label(e.kind);
        for (const auto& n : names)
            if (e.kind == n.first) name = n.second;
        if (!out.empty()) out += "  ";
        out += (e.count > 1 ? std::to_string(e.count) + " " : std::string()) + name;
    }
    return out;
}

std::optional<ShipReport> parse_ship_report(const std::string& text, std::string* error) {
    ShipReport r;
    try {
        if (text.empty()) throw Fail{"empty report"};
        std::vector<std::string> lines = split(text, '\n');
        if (!lines.empty() && lines.back().empty()) lines.pop_back();
        bool header = false;
        for (size_t n = 0; n < lines.size(); ++n) {
            std::string line = lines[n];
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            for (const std::string& group : split(line, ' ')) {
                const size_t eq = group.find('=');
                if (eq == std::string::npos || eq == 0) throw Fail{"line " + std::to_string(n + 1) + ": bad group '" + group + "'"};
                const std::string key = group.substr(0, eq), value = group.substr(eq + 1);
                auto bad = [&] { return Fail{"line " + std::to_string(n + 1) + ": bad " + key + " '" + value + "'"}; };
                if (n == 0 && !header) {
                    if (key != "v") throw Fail{"no version field"};
                    long long v;
                    if (!integer(value, 0, 1000000, v)) throw bad();
                    if (v != SHIP_REPORT_VERSION) throw Fail{"unsupported version " + value};
                    header = true;
                    continue;
                }
                if (!header) throw Fail{"no version field"};
                if (key == "rev") {
                    long long v;
                    if (!integer(value, 0, 4294967295LL, v)) throw bad();
                    r.revision = static_cast<uint32_t>(v);
                } else if (key == "facility" || key == "name" || key == "sit") {
                    auto decoded = percent_decode(value);
                    if (!decoded) throw bad();
                    (key == "facility" ? r.facility : key == "name" ? r.name : r.situation) = *decoded;
                } else if (key == "mass") {
                    if (!optional_number(value, r.mass)) throw bad();
                } else if (key == "dry") {
                    if (!optional_number(value, r.dry)) throw bad();
                } else if (key == "cost") {
                    if (!optional_number(value, r.cost)) throw bad();
                } else if (key == "funds") {
                    if (!optional_number(value, r.funds)) throw bad();
                } else if (key == "parts") {
                    if (!count(value, r.parts)) throw bad();
                } else if (key == "crew") {
                    if (!count(value, r.crew)) throw bad();
                } else if (key == "seats") {
                    if (!count(value, r.seats)) throw bad();
                } else if (key == "engines") {
                    if (!count(value, r.engines)) throw bad();
                } else if (key == "size") {
                    auto hw = split(value, ',');
                    if (hw.size() != 2 || !optional_number(hw[0], r.height) || !optional_number(hw[1], r.width)) throw bad();
                } else if (key == "dv") {
                    if (value != "pending") throw bad();
                    r.delta_v_pending = true;
                } else if (key == "st") {
                    auto f = split(value, ',');
                    StageReport st;
                    long long stage;
                    if (f.size() < 5 || !integer(f[0], 0, 1000, stage) || !number(f[1], st.delta_v) ||
                        !number(f[2], st.delta_v_vac) || !number(f[3], st.twr) || !number(f[4], st.burn))
                        throw bad();
                    st.stage = static_cast<int>(stage);
                    for (size_t i = 5; i < f.size(); ++i) {
                        const size_t colon = f[i].find(':');
                        long long k;
                        if (colon == std::string::npos || colon == 0 || !integer(f[i].substr(colon + 1), 0, 100000, k))
                            throw bad();
                        auto kind = percent_decode(f[i].substr(0, colon));
                        if (!kind) throw bad();
                        st.events.push_back({*kind, static_cast<int>(k)});
                    }
                    r.stages.push_back(std::move(st));
                }
                // anything else: a field or line kind of a later phase (chk=, lim=), ignored
            }
        }
        if (!header) throw Fail{"no version field"};
        if (r.delta_v_pending && !r.stages.empty()) throw Fail{"dv=pending together with st= lines"};
    } catch (const Fail& f) {
        if (error) *error = f.what;
        return std::nullopt;
    }
    if (error) error->clear();
    return r;
}

}  // namespace ksphmi
