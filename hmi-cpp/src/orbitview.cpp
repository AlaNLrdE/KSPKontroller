// Body-centred inertial orbit geometry (port of orbitview.py), independent of attitude and periapsis conventions.
#include <chrono>

#include "model.hpp"

namespace ksphmi {

double monotonic() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

Axes view_axes(const Vec3& position, const Vec3& velocity) {
    auto normal = unit(cross(position, velocity));
    if (!normal) return {{1, 0, 0}, {0, 0, 1}};
    for (const Vec3& reference : {Vec3{1, 0, 0}, Vec3{0, 0, 1}}) {
        auto right = unit(reference - *normal * dot(reference, *normal));
        if (right) return {*right, cross(*normal, *right)};
    }
    return {{1, 0, 0}, {0, 0, 1}};
}

std::optional<Conic> OrbitState::conic() const {
    double radius = std::sqrt(dot(position, position));
    if (radius <= 0 || mu <= 0 || !position.finite() || !velocity.finite() || !std::isfinite(mu)) return std::nullopt;
    Vec3 h = cross(position, velocity);
    auto normal = unit(h);
    if (!normal) return std::nullopt;
    double speed2 = dot(velocity, velocity), rv = dot(position, velocity);
    Vec3 ev = (position * (speed2 - mu / radius) - velocity * rv) * (1.0 / mu);
    double e = std::sqrt(dot(ev, ev));
    std::optional<Vec3> peri;
    if (e > 1e-8) {
        peri = unit(ev);
    } else {
        // A circle has no unique periapsis: choose a stable drawing basis.
        const Vec3& right = axes.x;
        peri = unit(right - *normal * dot(right, *normal));
        if (!peri) peri = unit(position);
    }
    if (!peri) return std::nullopt;
    Conic c;
    c.p = dot(h, h) / mu;
    c.eccentricity = e;
    c.peri_direction = *peri;
    c.transverse = cross(*normal, *peri);
    return c;
}

std::optional<Vec3> Conic::position(double anomaly) const {
    double denominator = 1 + eccentricity * std::cos(anomaly);
    if (denominator <= 1e-6) return std::nullopt;
    double radius = p / denominator;
    return (peri_direction * std::cos(anomaly) + transverse * std::sin(anomaly)) * radius;
}

double anomaly_of(const Conic& conic, const Vec3& position) {
    return std::atan2(dot(position, conic.transverse), dot(position, conic.peri_direction));
}

std::optional<ConicSpan> conic_span(const Conic& conic, double start, double limit) {
    const double e = conic.eccentricity, p = conic.p;
    if (!(p > 0) || !std::isfinite(e) || e < 0 || !std::isfinite(start)) return std::nullopt;
    const bool bounded = limit > 0 && std::isfinite(limit);
    if (e < 1 && (!bounded || p / (1 - e) <= limit)) return ConicSpan{-PI, PI, true};
    // r <= limit  <=>  cos(nu) >= (p / limit - 1) / e. Open conics also stop short of the asymptote.
    double reach = e >= 1 ? std::acos(-1 / e) * (1 - 1e-4) : PI;
    if (bounded) {
        if (e <= 0) return std::nullopt;  // a circle larger than the limit
        double c = (p / limit - 1) / e;
        if (c > 1) return std::nullopt;   // periapsis beyond the limit
        if (c > -1) reach = std::min(reach, std::acos(c));
    }
    double from = std::remainder(start, 2 * PI);
    if (std::fabs(from) > reach) return std::nullopt;  // the burn point itself is outside
    if (reach - from < 1e-9) return std::nullopt;
    return ConicSpan{from, reach, false};
}

bool span_contains(const ConicSpan& span, double anomaly) {
    if (span.closed) return true;
    if (!std::isfinite(anomaly)) return false;
    double ahead = std::fmod(anomaly - span.from, 2 * PI);
    if (ahead < 0) ahead += 2 * PI;
    return span.from + ahead <= span.to;
}

ConicSpan span_until(const Conic& conic, const ConicSpan& span, double start, const Vec3& end) {
    double stop = anomaly_of(conic, end);
    double from = span.closed ? std::remainder(start, 2 * PI) : span.from;
    double ahead = std::fmod(stop - from, 2 * PI);
    if (!(ahead >= 0)) ahead = std::isfinite(ahead) ? ahead + 2 * PI : 0;
    double to = from + ahead;
    if (!span.closed) to = std::min(to, span.to);
    return ConicSpan{from, to, false};
}

std::optional<Conic> Encounter::conic() const { return OrbitState{position, velocity, mu, {}}.conic(); }

std::optional<ConicSpan> Encounter::span() const {
    auto c = conic();
    if (!c) return std::nullopt;
    // The entry lies on the sphere itself, but kRPC's position at the SOI change can sit slightly outside it (Ike,
    // 2026-11-16: 18 m beyond a 1,049,599 m sphere, which discarded the whole encounter). An entry up to 0.1 %
    // outside widens the limit just to reach it, so the exit stays on the sphere.
    double limit = 0;
    if (soi > 0 && std::isfinite(soi)) {
        const double entry = std::sqrt(dot(position, position));
        limit = soi * (1 + 1e-6);
        if (entry > limit && entry <= soi * (1 + 1e-3)) limit = entry * (1 + 1e-9);
    }
    return conic_span(*c, anomaly_of(*c, position), limit);
}

std::optional<Vec3> Encounter::exit() const {
    auto c = conic();
    auto s = span();
    if (!c || !s || s->closed || !(soi > 0) || !std::isfinite(soi)) return std::nullopt;
    auto v = c->position(s->to);
    if (!v || std::sqrt(dot(*v, *v)) < soi * .999) return std::nullopt;  // stopped short of the sphere: asymptote
    return v;
}

std::optional<double> Encounter::periapsis_altitude() const {
    auto c = conic();
    if (!c) return std::nullopt;
    return c->periapsis() - radius;
}

std::vector<Point2> conic_track(const Conic& conic, const ConicSpan& span, const Axes& axes, int samples) {
    std::vector<Point2> points;
    samples = std::max(samples, 2);
    points.reserve(static_cast<size_t>(samples) + 1);
    for (int i = 0; i <= samples; ++i) {
        double anomaly = span.from + (span.to - span.from) * i / samples;
        if (span.closed && i == samples) anomaly = span.from;  // bit-identical closing point
        auto v = conic.position(anomaly);
        if (!v || !v->finite()) continue;
        points.push_back({dot(*v, axes.x), dot(*v, axes.y)});
    }
    return points;
}

std::vector<std::vector<Point2>> dash_polyline(const std::vector<Point2>& points, double dash, double gap,
                                               double phase) {
    std::vector<std::vector<Point2>> out;
    if (points.size() < 2 || !(dash > 0) || !(gap >= 0) || !std::isfinite(dash + gap)) return out;
    const double period = dash + gap;
    double t = std::fmod(phase, period);  // position inside the pattern: [0, dash) on, [dash, period) off
    if (!(t >= 0)) t = t < 0 ? t + period : 0;
    bool on = t < dash;
    std::vector<Point2> current;
    if (on) current.push_back(points.front());
    const size_t max_dashes = 100000;  // a degenerate dash length must not exhaust memory
    for (size_t i = 1; i < points.size() && out.size() < max_dashes; ++i) {
        const Point2 a = points[i - 1], b = points[i];
        double length = std::hypot(b.first - a.first, b.second - a.second);
        if (!(length > 0) || !std::isfinite(length)) continue;
        double done = 0;
        while (done < length && out.size() < max_dashes) {
            double to_switch = (on ? dash : period) - t;
            bool switches = to_switch <= length - done;
            double step = switches ? to_switch : length - done;
            done += step;
            double k = std::min(1.0, done / length);
            Point2 q{a.first + (b.first - a.first) * k, a.second + (b.second - a.second) * k};
            if (on) current.push_back(q);
            if (!switches) {
                t += step;
                break;
            }
            if (on) {
                if (current.size() > 1) out.push_back(std::move(current));
                current.clear();
                on = false;
                t = dash;
            } else {
                on = true;
                t = 0;
                current.push_back(q);
            }
        }
    }
    if (on && current.size() > 1) out.push_back(std::move(current));
    return out;
}

}  // namespace ksphmi
