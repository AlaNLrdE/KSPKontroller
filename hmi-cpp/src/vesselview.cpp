#include "vesselview.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>

namespace ksphmi {

namespace {

std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find(sep, start);
        if (end == std::string_view::npos) end = s.size();
        out.push_back(s.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

std::vector<std::string_view> lines(std::string_view text) {
    std::vector<std::string_view> out;
    for (std::string_view l : split(text, '\n')) {
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.remove_suffix(1);
        if (!l.empty()) out.push_back(l);
    }
    return out;
}

bool number(std::string_view s, double& out) {
    if (s.empty() || s.size() > 32) return false;
    const std::string copy(s);
    char* end = nullptr;
    out = std::strtod(copy.c_str(), &end);
    return end == copy.c_str() + copy.size() && std::isfinite(out);
}

bool integer(std::string_view s, long long& out) {
    double d;
    if (!number(s, d) || std::floor(d) != d || std::fabs(d) > 4294967295.0) return false;
    out = static_cast<long long>(d);
    return true;
}

/// "v=1 rev=N[ parts=K]": the revision, and the part count when asked for.
bool header(std::string_view line, uint32_t& revision, long long* parts) {
    auto fields = split(line, ' ');
    if (fields.size() != (parts ? 3u : 2u) || fields[0] != "v=1") return false;
    long long rev;
    if (fields[1].substr(0, 4) != "rev=" || !integer(fields[1].substr(4), rev) || rev < 0) return false;
    revision = static_cast<uint32_t>(rev);
    if (parts && (fields[2].substr(0, 6) != "parts=" || !integer(fields[2].substr(6), *parts) || *parts < 0))
        return false;
    return true;
}

}  // namespace

std::optional<VesselView> parse_vessel_shape(std::string_view text) {
    auto ls = lines(text);
    VesselView view;
    long long count = 0;
    if (ls.empty() || !header(ls[0], view.revision, &count)) return std::nullopt;
    if (static_cast<long long>(ls.size()) - 1 != count) return std::nullopt;
    for (size_t i = 1; i < ls.size(); ++i) {
        if (ls[i].substr(0, 2) != "p=") return std::nullopt;
        auto fields = split(ls[i].substr(2), ',');
        if (fields.size() < 4) return std::nullopt;  // depth and at least three points
        VesselPart part;
        if (!number(fields[0], part.depth)) return std::nullopt;
        for (size_t f = 1; f < fields.size(); ++f) {
            auto xy = split(fields[f], ':');
            Point2 p;
            if (xy.size() != 2 || !number(xy[0], p.first) || !number(xy[1], p.second)) return std::nullopt;
            part.hull.push_back(p);
        }
        view.parts.push_back(std::move(part));
    }
    return view;
}

bool apply_vessel_state(VesselView& view, std::string_view text) {
    auto ls = lines(text);
    uint32_t revision;
    if (ls.size() != 2 || !header(ls[0], revision, nullptr) || revision != view.revision) return false;
    if (ls[1].substr(0, 2) != "s=") return false;
    auto entries = ls[1].size() == 2 ? std::vector<std::string_view>{} : split(ls[1].substr(2), ',');
    if (entries.size() != view.parts.size()) return false;
    std::vector<std::pair<int, int>> values;
    for (std::string_view e : entries) {
        auto fh = split(e, ':');
        long long fuel, heat;
        if (fh.size() != 2 || !integer(fh[0], fuel) || !integer(fh[1], heat)) return false;
        values.emplace_back(int(std::clamp(fuel, -1LL, 100LL)), int(std::clamp(heat, 0LL, 100LL)));
    }
    for (size_t i = 0; i < values.size(); ++i) view.parts[i].fuel = values[i].first, view.parts[i].heat = values[i].second;
    return true;
}

std::string format_vessel_shape(const VesselView& view) {
    std::string out = "v=1 rev=" + std::to_string(view.revision) + " parts=" + std::to_string(view.parts.size()) + "\n";
    char buf[64];
    for (const VesselPart& p : view.parts) {
        std::snprintf(buf, sizeof buf, "p=%.2f", p.depth);
        out += buf;
        for (const Point2& q : p.hull) {
            std::snprintf(buf, sizeof buf, ",%.2f:%.2f", q.first, q.second);
            out += buf;
        }
        out += "\n";
    }
    return out;
}

std::string format_vessel_state(const VesselView& view) {
    std::string out = "v=1 rev=" + std::to_string(view.revision) + "\ns=";
    for (size_t i = 0; i < view.parts.size(); ++i)
        out += (i ? "," : "") + std::to_string(view.parts[i].fuel) + ":" + std::to_string(view.parts[i].heat);
    return out + "\n";
}

VesselView demo_vessel_view(double t) {
    // shape, centre x, top (from the nose, 0..1), width, height in units of the 20 m stack; fuel %, heat %
    struct Spec {
        char shape;
        double cx, top, w, h;
        int fuel, heat;
    };
    static const Spec specs[] = {
        {'c', 0, 0, .13, .07, -1, 30},       {'r', 0, .07, .13, .015, -1, 45},  {'r', 0, .085, .13, .012, -1, 20},
        {'r', 0, .10, .13, .12, 62, 15},     {'b', 0, .22, .09, .05, -1, 55},   {'r', 0, .27, .18, .012, -1, 25},
        {'r', 0, .285, .18, .19, 48, 20},    {'r', 0, .475, .18, .19, 48, 22},  {'b', 0, .665, .12, .06, -1, 70},
        {'r', 0, .725, .24, .012, -1, 30},   {'r', 0, .74, .24, .17, 85, 35},   {'b', 0, .91, .16, .08, -1, 93},
        {'c', -.19, .55, .09, .06, -1, 40},  {'r', -.19, .61, .09, .31, 30, 50}, {'b', -.19, .92, .07, .05, -1, 88},
        {'c', .19, .55, .09, .06, -1, 40},   {'r', .19, .61, .09, .31, 30, 50},  {'b', .19, .92, .07, .05, -1, 88},
    };
    const double H = 20;
    VesselView view;
    view.revision = 1;
    const double wobble = std::sin(t / 7.0);
    for (const Spec& s : specs) {
        VesselPart p;
        const double l = (s.cx - s.w / 2) * H, r = (s.cx + s.w / 2) * H;
        const double top = (1 - s.top) * H, bottom = (1 - s.top - s.h) * H;
        if (s.shape == 'c') p.hull = {{l, bottom}, {r, bottom}, {(l + r) / 2, top}};
        else if (s.shape == 'b') p.hull = {{l, bottom}, {r, bottom}, {l + .7 * (r - l), top}, {l + .3 * (r - l), top}};
        else p.hull = {{l, bottom}, {r, bottom}, {r, top}, {l, top}};
        p.fuel = s.fuel;
        p.heat = std::clamp(int(s.heat + (s.heat > 80 ? 5 * wobble : 0)), 0, 100);
        view.parts.push_back(std::move(p));
    }
    return view;
}

std::vector<size_t> vessel_draw_order(const VesselView& view) {
    std::vector<size_t> order(view.parts.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return view.parts[a].depth > view.parts[b].depth; });
    return order;
}

std::vector<Point2> clip_below(const std::vector<Point2>& polygon, double level) {
    // Sutherland-Hodgman against the half-plane y >= level (screen y grows downwards)
    std::vector<Point2> out;
    const size_t n = polygon.size();
    for (size_t i = 0; i < n; ++i) {
        const Point2& a = polygon[i];
        const Point2& b = polygon[(i + 1) % n];
        const bool ain = a.second >= level, bin = b.second >= level;
        if (ain) out.push_back(a);
        if (ain != bin) {
            const double f = (level - a.second) / (b.second - a.second);
            out.push_back({a.first + f * (b.first - a.first), level});
        }
    }
    return out.size() >= 3 ? out : std::vector<Point2>{};
}

}  // namespace ksphmi
