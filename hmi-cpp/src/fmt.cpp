#include "fmt.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <limits>

namespace ksphmi::fmt {

std::string str(const char* format, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, format);
    int n = std::vsnprintf(buf, sizeof buf, format, ap);
    va_end(ap);
    if (n < 0) return "";
    return std::string(buf, std::min<size_t>(static_cast<size_t>(n), sizeof buf - 1));
}

long long round_half_even(double v) { return static_cast<long long>(std::nearbyint(v)); }

namespace {
// "-0 m" / "-0.0 km" (a tiny negative value rounded away) reads as a sign flicker: drop the sign.
std::string no_negative_zero(const std::string& text) {
    if (!text.empty() && text[0] == '-' && text.find_first_of("123456789") == std::string::npos)
        return text.substr(1);
    return text;
}
}  // namespace

std::string distance(double m) {
    if (!std::isfinite(m)) return "--";
    double a = std::fabs(m);
    std::string text;
    if (a < 10000) text = str("%.0f m", m);
    else if (a < 10000000) text = str("%.1f km", m / 1e3);
    else if (a < 10000000000.0) text = str("%.2f Mm", m / 1e6);
    else text = str("%.2f Gm", m / 1e9);
    return no_negative_zero(text);
}

std::string speed(double ms) {
    if (!std::isfinite(ms)) return "--";
    if (std::fabs(ms) < 10000) return no_negative_zero(str("%.1f m/s", ms));
    return str("%.2f km/s", ms / 1e3);
}

std::string duration(double s, int day_hours) {
    if (!std::isfinite(s)) return "--";
    const char* sign = s < 0 ? "-" : "";
    long long t = round_half_even(std::min(std::fabs(s), 1e15));  // no overflow on absurd values
    long long day = static_cast<long long>(day_hours) * 3600;
    long long d = t / day;
    t %= day;
    long long h = t / 3600;
    t %= 3600;
    long long m = t / 60, sec = t % 60;
    if (d) return str("%s%lldd %lldh", sign, d, h);
    if (h) return str("%s%lldh %02lldm", sign, h, m);
    if (m) return str("%s%lldm %02llds", sign, m, sec);
    return str("%s%llds", sign, sec);
}

std::string clock(double ut, int day_hours, int year_days) {
    // Use the exclusive power-of-two bound: double(LLONG_MAX) rounds up to 2^63.
    const double limit = std::ldexp(1.0, std::numeric_limits<long long>::digits);
    if (!std::isfinite(ut) || ut < 0 || ut >= limit) return "--";
    long long t = static_cast<long long>(ut);
    long long day = static_cast<long long>(day_hours) * 3600;
    long long y = t / (day * year_days);
    t %= day * year_days;
    long long d = t / day;
    t %= day;
    long long h = t / 3600;
    t %= 3600;
    return str("Y%lld D%03lld %02lld:%02lld:%02lld", y + 1, d + 1, h, t / 60, t % 60);
}

std::string degrees(double v) { return std::isfinite(v) ? str("%.2f\xC2\xB0", v) : "--"; }

std::string situation(const std::string& name) {
    if (name == "pre_launch") return "Pre-launch";
    if (name == "sub_orbital") return "Sub-orbital";
    if (name == "splashed") return "Splashed down";
    // Python str.capitalize(): first character upper, the rest lower.
    std::string out;
    for (char c : name) out += c == '_' ? ' ' : c;
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<char>(i == 0 ? std::toupper(static_cast<unsigned char>(out[i]))
                                          : std::tolower(static_cast<unsigned char>(out[i])));
    return out;
}

}  // namespace ksphmi::fmt
