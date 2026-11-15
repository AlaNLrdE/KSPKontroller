#include "display.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>

#include "fmt.hpp"
#include "gauges.hpp"
#include "scenes.hpp"
#include "theme.hpp"

namespace ksphmi {

using namespace theme;

namespace {
long long py_round(double v) { return fmt::round_half_even(v); }
float rnd(double v) { return static_cast<float>(py_round(v)); }

// At most n bytes, never cutting a UTF-8 sequence in half (Python slices characters; close enough on screen).
// Compact KSP units for the dial captions: whole units from 100 up, one decimal below.
std::string amount_text(double v) { return fmt::str(std::fabs(v) >= 100 ? "%.0f" : "%.1f", v); }

// Text sizes (units) below each dial: label, amount and the gap between them.
constexpr float GAUGE_LABEL = 1.9f, GAUGE_AMOUNT = 1.8f, GAUGE_GAP = .3f;

// Ring between two radii and two fractions of the dial, as convex quads (Canvas::polygon wants convex shapes).
void dial_sector(Canvas& c, double cx, double cy, double r_out, double r_in, double f0, double f1, Color color,
                 double step_degrees = 4.0) {
    auto pts = gauges::sector(cx, cy, r_out, r_in, f0, f1, step_degrees);
    const size_t n = pts.size();
    for (size_t i = 0; i + 1 < n / 2; ++i) {
        auto a = pts[i], b = pts[i + 1], d = pts[n - 2 - i], e = pts[n - 1 - i];
        c.polygon({{float(a.first), float(a.second)}, {float(b.first), float(b.second)},
                   {float(d.first), float(d.second)}, {float(e.first), float(e.second)}}, color, false);
    }
}

constexpr int GAUGE_LIVE = 0, GAUGE_UNKNOWN = 1, GAUGE_ABSENT = 2;  // dial face states (index of the face colours)
constexpr Color NEEDLE_RED{222, 36, 28}, DIGITS{236, 236, 226};    // dial needle, light digits of the windows

Color mix(Color a, Color b, double t) {
    return Color{uint8_t(std::lround(a.r + (b.r - a.r) * t)), uint8_t(std::lround(a.g + (b.g - a.g) * t)),
                 uint8_t(std::lround(a.b + (b.b - a.b) * t))};
}

// A disc shaded top to bottom through grey stops {position 0..1, level 0..255}, slightly cool so it reads as
// polished steel. Drawn once into cached faces.
void gradient_disc(Canvas& c, double cx, double cy, double radius, std::initializer_list<std::pair<double, double>> stops) {
    const std::vector<std::pair<double, double>> s(stops);
    int rr = static_cast<int>(radius);
    for (int dy = -rr; dy <= rr; ++dy) {
        double t = (dy + radius) / (2 * radius), v = s.back().second;
        for (size_t i = 0; i + 1 < s.size(); ++i)
            if (t <= s[i + 1].first) {
                v = s[i].second + (s[i + 1].second - s[i].second) * (t - s[i].first) / std::max(s[i + 1].first - s[i].first, 1e-9);
                break;
            }
        double half = std::sqrt(std::max(radius * radius - double(dy) * dy, 0.0));
        auto level = [&](double k) { return uint8_t(std::clamp(std::lround(v - k), 0L, 255L)); };
        c.fill_rect(float(cx - half), float(cy + dy), float(2 * half), 1.0f, Color{level(4), level(6), level(10)});
    }
}

// Centred text of about `px` pixels on a face canvas (whose unit is 1 % of its own height).
void face_text(Canvas& f, const std::string& s, double px, Color color, double x, double y) {
    f.text(s, static_cast<float>((std::round(px) + .5) / f.u), color, x, y, Anchor::Center);
}

// A small symbol per resource, drawn with primitives into a cached face; `s` is its height, `face` the colour
// behind it (for cut-outs).
void gauge_icon(Canvas& f, const std::string& name, double x, double y, double s, Color color, Color face) {
    auto P = [&](double px, double py) { return Pt{float(x + px * s), float(y + py * s)}; };
    auto tri = [&](Pt a, Pt b, Pt c) { f.polygon({a, b, c}, color, false); };
    float line = std::max(2.0f, rnd(.09 * s));
    if (name == "ElectricCharge") {  // lightning bolt, split into convex pieces
        Pt a = P(.10, -.52), b = P(-.30, .06), c = P(-.02, .06), d = P(-.14, .52), e = P(.30, -.10), g = P(.02, -.10),
           h = P(.16, -.52);
        tri(a, b, c), tri(a, c, g), tri(a, g, h), tri(c, d, e), tri(c, e, g);
    } else if (name == "LiquidFuel") {  // fuel pump with its hose and nozzle
        f.rounded_rect(Rect::f(x - .36 * s, y - .46 * s, .46 * s, .92 * s), rnd(.06 * s), color);
        f.fill_rect(float(x - .27 * s), float(y - .36 * s), float(.28 * s), float(.24 * s), face);
        f.polyline({P(.08, -.30), P(.24, -.18), P(.24, .26), P(.34, .26), P(.34, -.28)}, false, line, color);
        tri(P(.28, -.32), P(.42, -.40), P(.42, -.24));
    } else if (name == "Oxidizer") {  // droplet marked O2
        f.circle(float(x), float(y + .14 * s), float(.34 * s), color);
        tri(P(0, -.52), P(-.30, -.02), P(.30, -.02));
        face_text(f, "O", .52 * s, face, x - .06 * s, y + .14 * s);
        face_text(f, "2", .30 * s, face, x + .15 * s, y + .26 * s);
    } else if (name == "MonoPropellant") {  // RCS tank marked MP
        f.rounded_rect(Rect::f(x - .50 * s, y - .38 * s, s, .76 * s), rnd(.36 * s), color);
        face_text(f, "MP", .56 * s, face, x, y + .02 * s);
    } else if (name == "SolidFuel") {  // solid booster: nose cone, casing, fins and nozzle
        tri(P(0, -.56), P(-.15, -.30), P(.15, -.30));
        f.fill_rect(float(x - .15 * s), float(y - .30 * s), float(.30 * s), float(.66 * s), color);
        tri(P(-.15, .10), P(-.30, .38), P(-.15, .34));
        tri(P(.15, .10), P(.30, .38), P(.15, .34));
        f.polygon({P(-.08, .36), P(.08, .36), P(.13, .52), P(-.13, .52)}, color, false);
        f.line(P(-.15, -.08), P(.15, -.08), line, face);
    } else if (name == "XenonGas") {  // element symbol in a ring
        f.ring(float(x), float(y), float(.44 * s), line, color);
        face_text(f, "Xe", .56 * s, color, x, y + .02 * s);
    }
}

std::string clip_utf8(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    size_t end = n;
    while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
    return s.substr(0, end);
}
}  // namespace

Display::Display(const Config& cfg, Canvas& canvas)
    : c_(canvas),
      day_hours_(cfg.format.day_hours),
      server_text_(fmt::str("kRPC server: %s:%d", cfg.krpc.address.c_str(), cfg.krpc.rpc_port)),
      net_(cfg.krpc.address) {}

Display::~Display() {
    for (auto& [key, texture] : gauge_faces_) SDL_DestroyTexture(texture);
}

void Display::draw(const Snapshot& snap, double now) {
    c_.clear(BG);
    header(snap, now);
    if (snap.state == State::Flight) flight(snap);
    else if (snap.state == State::NoVessel && find_view(snap.scene)) facility(snap);
    else waiting(snap, now);
    c_.flush();
}

void Display::header(const Snapshot& snap, double now) {
    const double u = c_.u;
    const double top = 1.5 * u;
    const int w = c_.w;
    if (snap.state == State::Flight) {
        c_.text(snap.vessel.empty() ? "Unnamed vessel" : snap.vessel, 5.2, VALUE, 2.5 * u, top);
        c_.text(snap.body + "  \xC2\xB7  " + fmt::situation(snap.situation), 3.0, LABEL, 2.5 * u, top + 5.4 * u);
        c_.text(fmt::clock(snap.ut, day_hours_), 3.4, LABEL, w - 2.5 * u, top + 5.0 * u, Anchor::TopRight);
    } else if (snap.state == State::NoVessel && find_view(snap.scene)) {
        c_.text("KSP KONTROLLER", 5.2, VALUE, 2.5 * u, top);
        c_.text(find_view(snap.scene)->subtitle, 3.0, LABEL, 2.5 * u, top + 5.4 * u);
    }
    std::string status;
    Color color;
    if (stale(snap, now)) {
        status = fmt::str("NO DATA %ds", static_cast<int>(now - snap.received)), color = WARN;
    } else if (snap.state == State::Flight) {
        status = "LIVE", color = GOOD;
    } else if (snap.state == State::NoVessel) {
        status = "CONNECTED", color = GOOD;  // the link is up; the centre of the screen says not in flight
    } else {
        status = "OFFLINE", color = BAD;
    }
    Rect r = c_.text(status, 3.4, color, w - 2.5 * u, top + 0.6 * u, Anchor::TopRight);
    c_.circle(static_cast<float>(r.x - 1.6 * u), static_cast<float>(r.centery()), static_cast<float>(0.9 * u), color);
    c_.line({static_cast<float>(2.5 * u), static_cast<float>(11 * u)}, {static_cast<float>(w - 2.5 * u), static_cast<float>(11 * u)},
            std::max(1.0f, static_cast<float>(static_cast<int>(0.2 * u))), GRID);
}

void Display::waiting(const Snapshot& snap, double now) {
    const double u = c_.u;
    const double cx = c_.w / 2.0, cy = c_.h * 0.5;
    // A small moon circling a planet, so a frozen screen is easy to tell from a waiting one.
    c_.circle(static_cast<float>(cx), static_cast<float>(cy - 12 * u), static_cast<float>(6 * u), BODY);
    double ang = now * 1.2;
    c_.ring(static_cast<float>(cx), static_cast<float>(cy - 12 * u), static_cast<float>(11 * u),
            std::max(1.0f, static_cast<float>(static_cast<int>(0.25 * u))), GRID);
    c_.circle(static_cast<float>(cx + 11 * u * std::cos(ang)), static_cast<float>(cy - 12 * u + 11 * u * std::sin(ang)),
              static_cast<float>(1.2 * u), ACCENT);
    bool no_vessel = snap.state == State::NoVessel;
    c_.text(no_vessel ? "Not in flight" : "Waiting for KSP", 8, VALUE, cx, cy + 4 * u, Anchor::MidTop);
    c_.text(no_vessel ? "Launch or switch to a vessel to see its orbit"
                      : "Start KSP and the kRPC server (port 50000 on the PC's LAN address)",
            3.4, LABEL, cx, cy + 13 * u, Anchor::MidTop);
    if (!snap.detail.empty()) c_.text(clip_utf8(snap.detail, 110), 2.8, MUTED, cx, cy + 19 * u, Anchor::MidTop);
    // Where to find this display (SSH, deploys) and where it looks for the game.
    c_.text(net_.line(), 3.4, VALUE, cx, c_.h - 9.5 * u, Anchor::MidTop);
    c_.text(snap.server.empty() ? server_text_ : snap.server, 3.0, LABEL, cx, c_.h - 5 * u, Anchor::MidTop);
}

void Display::facility(const Snapshot& snap) {
    const double u = c_.u;
    const View* view = find_view(snap.scene);
    c_.text(view->title, 6.5, VALUE, 4 * u, 15 * u);
    c_.text("PREPARATION CHECKLIST", 2.8, ACCENT, 4 * u, 24 * u);
    for (int index = 0; index < 3; ++index) {
        const Card& card = view->cards[index];
        double y = (31 + index * 17) * u;
        panel(c_, Rect::f(4 * u, y, c_.w - 8 * u, 14 * u));
        c_.text(card.label, 2.6, ACCENT, 6 * u, y + 1.5 * u);
        c_.text(card.heading, 4.2, VALUE, 6 * u, y + 4.7 * u);
        c_.text(card.hint, 2.8, LABEL, 6 * u, y + 10 * u);
    }
    c_.text("Scene context live  |  Flight telemetry resumes when you launch", 2.6, MUTED, 4 * u, 87 * u);
}

void Display::flight(const Snapshot& s) {
    const double u = c_.u;
    const double top = 13 * u, bottom = c_.h - 2.5 * u, gap = 1.6 * u;
    const double left_w = c_.w * 0.44;
    const double x0 = 2.5 * u;
    const double metric_gap = .8 * u;
    const double col_w = (left_w - x0 - metric_gap) / 2;

    // Compact Ap / Pe row keeps the countdowns in the label line.
    const double big_h = 7 * u;
    Color pe_color = s.periapsis < 0 ? BAD : s.periapsis < s.atmosphere_depth ? WARN : ACCENT;
    bool hyperbolic = s.eccentricity >= 1;
    tile(Rect::f(x0, top, col_w, big_h), "APOAPSIS", hyperbolic ? "Escape" : fmt::distance(s.apoapsis), GOOD,
         hyperbolic ? "" : "T " + fmt::duration(s.time_to_apoapsis, day_hours_));
    std::string pe_note = s.periapsis < 0 ? "below the surface"
                          : s.periapsis < s.atmosphere_depth ? "inside the atmosphere" : "";
    tile(Rect::f(x0 + col_w + metric_gap, top, col_w, big_h), "PERIAPSIS", fmt::distance(s.periapsis), pe_color,
         "T " + fmt::duration(s.time_to_periapsis, day_hours_));

    struct Row {
        const char* la;
        std::string va;
        Color ca;
        const char* lb;
        std::string vb;
        Color cb;
    };
    const Row rows[] = {
        {"ALTITUDE", fmt::distance(s.altitude), VALUE, "VERTICAL SPEED", fmt::speed(s.vertical_speed),
         s.vertical_speed >= 0 ? GOOD : WARN},
        {"ORBITAL SPEED", fmt::speed(s.orbital_speed), VALUE, "SURFACE SPEED", fmt::speed(s.surface_speed), VALUE},
        {"INCLINATION", fmt::degrees(s.inclination), VALUE, "ECCENTRICITY", fmt::str("%.4f", s.eccentricity), VALUE},
        {"PERIOD", hyperbolic ? "--" : fmt::duration(s.period, day_hours_), VALUE, "SEMI-MAJOR AXIS",
         fmt::distance(s.semi_major_axis), VALUE},
    };
    double y = top + big_h + metric_gap;
    const double row_h = 7 * u;
    for (const Row& row : rows) {
        tile(Rect::f(x0, y, col_w, row_h), row.la, row.va, row.ca);
        tile(Rect::f(x0 + col_w + metric_gap, y, col_w, row_h), row.lb, row.vb, row.cb);
        y += row_h + metric_gap;
    }
    if (!pe_note.empty()) {
        std::string upper = pe_note;
        for (char& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        c_.text("PERIAPSIS: " + upper, 1.8, pe_color, x0 + u, y - .3 * u);
    }
    resources(Rect::f(x0, 54 * u, left_w - x0, 25 * u), s);
    staging(Rect::f(x0, 80.6 * u, left_w - x0, bottom - 80.6 * u), s);

    const double right_x = left_w + gap, right_w = c_.w - left_w - gap - 2.5 * u;
    const double orbit_h = 22 * u;
    orbit(Rect::f(right_x, top, right_w, orbit_h), s);
    navball_.draw(c_, Rect::f(right_x, top + orbit_h + gap, right_w, bottom - top - orbit_h - gap), s.nav);
}

void Display::tile(const Rect& rect, const std::string& label, const std::string& value, Color color,
                   const std::string& note) {
    if (on_tile_begin) on_tile_begin(rect);
    const double u = c_.u, pad = 1 * u;
    panel(c_, rect);
    c_.text(label, 1.9, LABEL, rect.x + pad, rect.y + .7 * u);
    c_.text(value, 4.0, color, rect.x + pad, rect.y + 2.9 * u);
    if (!note.empty()) c_.text(note, 1.6, LABEL, rect.right() - pad, rect.y + .7 * u, Anchor::TopRight);
    if (on_tile_end) on_tile_end(rect);
}

void Display::resources(const Rect& rect, const Snapshot& snap) {
    const double u = c_.u;
    panel(c_, rect);
    c_.text("RESOURCES", 2.4, LABEL, rect.x + 1.3 * u, rect.y + u);
    c_.text("VESSEL TOTALS \xC2\xB7 KSP UNITS", 1.7, MUTED, rect.right() - 1.3 * u, rect.y + u, Anchor::TopRight);
    if (!snap.systems) {
        c_.text("Resource data unavailable", 2.4, MUTED, rect.centerx(), rect.centery(), Anchor::Center);
        return;
    }
    std::vector<std::pair<int, const Resource*>> visible;  // index in RESOURCE_NAMES, resource
    for (int i = 0; i < static_cast<int>(RESOURCE_NAMES.size()); ++i) {
        const Resource* found = nullptr;
        for (const auto& r : snap.systems->resources)
            if (r.name == RESOURCE_NAMES[i]) found = &r;  // last one wins, as a dict built from the tuple
        if (found && (i < 4 || found->capacity.value_or(0) > 0)) visible.emplace_back(i, found);
    }
    if (visible.empty()) return;
    std::vector<int> names;
    for (const auto& v : visible) names.push_back(v.first);
    if (!gauge_layout_ || !(gauge_layout_->rect == rect) || gauge_layout_->names != names) {
        GaugeLayout g;
        g.rect = rect;
        g.names = names;
        double text_h = (GAUGE_LABEL + GAUGE_AMOUNT + GAUGE_GAP) * u;
        auto l = gauges::layout(rect.x + .6 * u, rect.y + 4.2 * u, rect.w - 1.2 * u, rect.h - 4.6 * u,
                                static_cast<int>(names.size()), text_h, .4 * u);
        g.radius = l.radius;
        double room = 1e300;
        for (const auto& cell : l.cells) {
            g.cells.push_back({cell.cx, cell.cy, cell.cell_x, cell.cell_w});
            room = std::min(room, cell.cell_w - 2 * u);
        }
        int widest = 1;
        for (int n : names) widest = std::max(widest, c_.text_width(c_.font(GAUGE_LABEL), RESOURCE_LABELS[n]));
        g.label_units = static_cast<float>(std::floor(GAUGE_LABEL * std::min(1.0, room / widest) * 20) / 20);
        gauge_layout_ = std::move(g);
    }
    const GaugeLayout& g = *gauge_layout_;
    for (size_t i = 0; i < visible.size(); ++i) {
        const auto& cell = g.cells[i];
        gauge(*visible[i].second, RESOURCE_LABELS[visible[i].first], cell[0], cell[1], g.radius, cell[3] - 2 * u,
              g.label_units);
    }
}

SDL_Texture* Display::gauge_face(int radius, int state, const std::string& name) {
    auto key = std::make_tuple(radius, state, name);
    auto found = gauge_faces_.find(key);
    if (found != gauge_faces_.end()) return found->second;
    if (gauge_faces_.size() > 40) {
        for (auto& [k, texture] : gauge_faces_) SDL_DestroyTexture(texture);
        gauge_faces_.clear();
    }
    int side = 2 * radius + 4, big = 2 * side;
    SDL_Surface* hi = SDL_CreateRGBSurfaceWithFormat(0, big, big, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_Surface* lo = SDL_CreateRGBSurfaceWithFormat(0, side, side, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_Renderer* renderer = hi ? SDL_CreateSoftwareRenderer(hi) : nullptr;
    SDL_Texture* texture = nullptr;
    if (hi && lo && renderer) {
        {
            Canvas f(renderer, big, big, false);
            f.clear(PANEL);
            const double c = side, r = 2.0 * radius;
            const bool live = state == GAUGE_LIVE;
            // chrome: an outer ring lit from above, a reversed bevel inside it, a dark seat, then the face
            gradient_disc(f, c, c, r, {{0, 250}, {.38, 196}, {.52, 92}, {.70, 128}, {1, 205}});
            f.ring(float(c), float(c), float(r), std::max(1.0f, rnd(.012 * r)), Color{40, 44, 50});
            gradient_disc(f, c, c, std::round(.88 * r), {{0, 70}, {.5, 150}, {1, 240}});
            f.circle(float(c), float(c), float(std::round(.83 * r)), Color{26, 28, 32});
            f.arc(float(c), float(c), float(.95 * r), 100 * PI / 180, 160 * PI / 180, std::max(2.0f, rnd(.03 * r)),
                  Color{255, 255, 255});
            static const Color RIM[3] = {{176, 178, 182}, {116, 120, 126}, {58, 62, 68}};
            static const Color MID[3] = {{250, 250, 247}, {164, 168, 174}, {88, 92, 98}};
            const Color rim = RIM[state], mid = MID[state];
            int fr = static_cast<int>(std::lround(gauges::FACE * r));
            for (int i = fr; i > 0; i -= std::max(1, fr / 48)) {
                double t = std::pow(std::clamp((fr - i) / (.45 * fr), 0.0, 1.0), .7);  // darker towards the rim
                f.circle(float(c), float(c), float(i), mix(rim, mid, t));
            }
            const Color ink = live ? Color{18, 18, 20} : state == GAUGE_UNKNOWN ? Color{70, 74, 80} : Color{48, 52, 58};
            auto [px, py] = gauges::pivot(c, c, r);
            if (live) {
                dial_sector(f, px, py, gauges::ZONE_OUT * r, gauges::ZONE_IN * r, 0, gauges::BAD_BELOW,
                            Color{214, 30, 30}, 1);
                dial_sector(f, px, py, gauges::ZONE_OUT * r, gauges::ZONE_IN * r, gauges::BAD_BELOW,
                            gauges::WARN_BELOW, Color{236, 150, 20}, 1);
            }
            for (int step = 0; step <= 8; ++step) {
                double length = step == 4 ? .22 : step % 2 == 0 ? .16 : .09;
                double half = (step % 2 == 0 ? .050 : .026) * r / 2;
                double a = gauges::angle(step / 8.0), dx = std::cos(a), dy = -std::sin(a), nx = std::sin(a),
                       ny = std::cos(a);
                double outer = gauges::SCALE * r, inner = (gauges::SCALE - length) * r;
                f.polygon({{float(px + dx * outer + nx * half), float(py + dy * outer + ny * half)},
                           {float(px + dx * outer - nx * half), float(py + dy * outer - ny * half)},
                           {float(px + dx * inner - nx * half), float(py + dy * inner - ny * half)},
                           {float(px + dx * inner + nx * half), float(py + dy * inner + ny * half)}},
                          ink, false);
            }
            for (auto [s, fraction] : {std::pair{"E", 0.0}, std::pair{"F", 1.0}}) {
                auto p = gauges::point(px, py, gauges::LETTERS * r, fraction);
                face_text(f, s, .40 * r, ink, p.first, p.second);
            }
            gauge_icon(f, name, c, c - .13 * r, .34 * r, live ? ink : rim, mid);
            int ww = static_cast<int>(std::lround(gauges::WINDOW_W * r));
            int wh = static_cast<int>(std::lround((gauges::WINDOW_BOTTOM - gauges::WINDOW_TOP) * r));
            Rect window(static_cast<int>(c) - ww / 2, static_cast<int>(std::lround(c + gauges::WINDOW_TOP * r)), ww, wh);
            f.rounded_rect(window.inflate(std::round(.05 * r), std::round(.05 * r)), rnd(.06 * r), Color{120, 124, 130});
            f.rounded_rect(window, rnd(.04 * r), Color{8, 8, 10});
            f.flush();
        }
        SDL_RenderPresent(renderer);
        if (SDL_SoftStretchLinear(hi, nullptr, lo, nullptr) == 0) {
            texture = SDL_CreateTextureFromSurface(c_.renderer(), lo);
            if (texture) SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);
        }
    }
    if (renderer) SDL_DestroyRenderer(renderer);
    if (lo) SDL_FreeSurface(lo);
    if (hi) SDL_FreeSurface(hi);
    gauge_faces_[key] = texture;
    return texture;
}

void Display::gauge(const Resource& resource, const std::string& label, double cx, double cy, double radius,
                    double room, float label_units) {
    const double u = c_.u;
    auto fraction = resource.fraction();
    int r = std::max(4, static_cast<int>(radius));
    bool absent = !fraction && resource.capacity && *resource.capacity == 0;
    int state = fraction ? GAUGE_LIVE : absent ? GAUGE_ABSENT : GAUGE_UNKNOWN;
    if (on_gauge) on_gauge(label, Rect::f(cx - radius, cy - radius, 2 * radius, 2 * radius));
    if (SDL_Texture* face = gauge_face(r, state, resource.name)) {
        float side = static_cast<float>(2 * r + 4);
        // the face is centred with integer rects on (int(cx), int(cy))
        int left = static_cast<int>(cx) - (2 * r + 4) / 2, top = static_cast<int>(cy) - (2 * r + 4) / 2;
        c_.image(face, static_cast<float>(left), static_cast<float>(top), side, side);
    }
    auto [px, py] = gauges::pivot(static_cast<int>(cx), static_cast<int>(cy), r);
    Color color = MUTED, digits = MUTED;
    std::string window, amount, reference;
    if (fraction) {
        color = *fraction <= gauges::BAD_BELOW ? BAD : *fraction <= gauges::WARN_BELOW ? WARN : GOOD;
        digits = color == GOOD ? DIGITS : color;
        std::vector<Pt> needle;
        for (auto [x, y] : gauges::needle(px, py, r, *fraction)) needle.push_back({float(x), float(y)});
        c_.polygon(needle, NEEDLE_RED);
        window = fmt::str("%lld%%", py_round(100 * *fraction));
        amount = amount_text(*resource.amount) + " / " + amount_text(*resource.capacity);
        reference = amount_text(*resource.capacity) + " / " + amount_text(*resource.capacity);
    } else {
        window = absent ? "N/A" : "--";
        amount = reference = absent ? "NOT INSTALLED" : "--";
    }
    // charcoal hub with a small highlight, over the needle
    float hub = static_cast<float>(.13 * r), hx = static_cast<float>(std::round(px)), hy = static_cast<float>(std::round(py));
    c_.circle(hx, hy, hub, Color{20, 21, 24});
    c_.circle(hx, hy, .78f * hub, Color{58, 60, 66});
    c_.circle(hx - .22f * hub, hy - .22f * hub, .22f * hub, Color{92, 95, 102});
    c_.text(window, static_cast<float>(.34 * r / u), digits, cx,
            cy + (gauges::WINDOW_TOP + gauges::WINDOW_BOTTOM) / 2 * r, Anchor::Center);
    double top = cy + gauges::BELOW_CENTRE * r;
    c_.text(label, label_units, fraction ? LABEL : MUTED, cx, top, Anchor::MidTop);
    int width = c_.text_width(c_.font(GAUGE_AMOUNT), reference);
    float amount_units = static_cast<float>(std::floor(GAUGE_AMOUNT * std::min(1.0, room / std::max(width, 1)) * 20) / 20);
    c_.text(amount, amount_units, color, cx, top + (label_units + GAUGE_GAP) * u, Anchor::MidTop);
}

void Display::staging(const Rect& rect, const Snapshot& snap) {
    const double u = c_.u;
    panel(c_, rect);
    c_.text("STAGING", 2.4, LABEL, rect.x + 1.3 * u, rect.y + u);
    const auto& data = snap.systems;
    std::string current = data && data->current_stage ? std::to_string(*data->current_stage) : "--";
    c_.text("CURRENT " + current, 2.4, ACCENT, rect.right() - 1.3 * u, rect.y + u, Anchor::TopRight);
    if (!data || data->stages.empty()) {
        c_.text(!data ? "Stage data unavailable" : "No pending activation stages", 2.2, MUTED, rect.centerx(),
                rect.y + 8 * u, Anchor::Center);
        return;
    }
    for (size_t index = 0; index < data->stages.size() && index < 4; ++index) {
        const Stage& stage = data->stages[index];
        double y = rect.y + (5 + index * 2.7) * u;
        bool active = data->current_stage && stage.number == *data->current_stage;
        Color color = active ? ACCENT : VALUE;
        c_.text(fmt::str("%02d  %s", stage.number, active ? "CURRENT" : "PENDING"), 2.2, color, rect.x + 1.3 * u, y);
        c_.text((stage.parts ? std::to_string(*stage.parts) : std::string("--")) + " parts", 2.0, LABEL, rect.centerx(), y);
        c_.text(stage.delta_v ? "DV " + fmt::speed(*stage.delta_v) : "DV --", 2.2, color, rect.right() - 1.3 * u, y,
                Anchor::TopRight);
    }
}

int Display::apsis_label_width() {
    Font& f = c_.font(2.8);
    return std::max(c_.text_width(f, "Ap"), c_.text_width(f, "Pe"));
}

void Display::orbit(const Rect& rect, const Snapshot& s) {
    const double u = c_.u;
    panel(c_, rect);
    c_.text("ORBIT", 2.6, LABEL, rect.x + 1.8 * u, rect.y + 1.8 * u);
    if (s.orbit_state) c_.text("INERTIAL", 1.8, MUTED, rect.right() - 1.8 * u, rect.y + 1.8 * u, Anchor::TopRight);
    // Escape trajectories are unbounded: contain every primitive, including off-screen vessel markers, without
    // covering the panel title, and always give the caller its clip back.
    const Rect previous_clip = c_.clip();
    Rect plot = Rect::f(rect.x + 1.2 * u, rect.y + 5 * u, rect.w - 2.4 * u, rect.h - 6.2 * u);
    c_.set_clip(previous_clip.clip(plot));
    try {
        orbit_contents(rect, s);
    } catch (...) {
        c_.set_clip(previous_clip);
        throw;
    }
    c_.set_clip(previous_clip);
}

void Display::orbit_contents(const Rect& rect, const Snapshot& s) {
    last_vessel_marker.reset();
    if (s.orbit_state) {
        inertial_orbit(rect, s);
        return;
    }
    const double u = c_.u;
    // Reserve room for the labels outside both apsis markers.
    double side_margin = apsis_label_width() + 2.6 * u;
    Rect area = rect.inflate(-2 * side_margin, -10 * u).move(0, 2 * u);

    double radius = std::max(s.body_radius, 1.0);
    double atmo = radius + s.atmosphere_depth;
    double e = s.eccentricity, a = s.semi_major_axis;
    double rp = a * (1 - e);
    bool elliptic = e < 1 && a > 0;
    double xmin, xmax, ymax;
    if (elliptic) {
        double ra = a * (1 + e), b = a * std::sqrt(1 - e * e);
        xmin = std::min(-ra, -atmo), xmax = std::max(rp, atmo), ymax = std::max(b, atmo);
    } else {  // hyperbolic: show the neighbourhood of the body only
        double view = std::max(4 * atmo, 2.5 * std::fabs(rp));
        xmin = -view, xmax = view, ymax = view;
    }
    double scale = std::min(area.w / (xmax - xmin), area.h / (2 * ymax));
    double fx = area.centerx() - (xmin + xmax) / 2 * scale;  // focus (body centre) on screen
    double fy = area.centery();
    auto to_screen = [&](double r, double nu) {
        return Pt{static_cast<float>(fx + r * std::cos(nu) * scale), static_cast<float>(fy - r * std::sin(nu) * scale)};
    };
    if (s.atmosphere_depth > 0)
        c_.circle(static_cast<float>(fx), static_cast<float>(fy), static_cast<float>(std::max(3.0, atmo * scale)), ATMOSPHERE);
    double p = a * (1 - e * e);
    std::vector<Pt> points;
    double limit = 3 * std::max(xmax - xmin, 2 * ymax);
    for (int i = 0; i < 241; ++i) {
        double nu = -PI + 2 * PI * i / 240;
        double denom = 1 + e * std::cos(nu);
        if (denom <= 1e-3) continue;
        double r = p / denom;
        if (0 < r && r < limit) points.push_back(to_screen(r, nu));
    }
    if (points.size() > 1) c_.polyline(points, elliptic, 1, ACCENT);
    c_.circle(static_cast<float>(fx), static_cast<float>(fy), static_cast<float>(std::max(2.0, radius * scale)), BODY);
    c_.text(s.body, 2.8, VALUE, fx, fy, Anchor::Center);

    float marker = static_cast<float>(0.8 * u);
    if (elliptic) {
        Pt ap = to_screen(a * (1 + e), PI);
        c_.circle(ap.x, ap.y, marker, GOOD);
        c_.text("Ap", 2.8, GOOD, ap.x - 1.4 * u, ap.y, Anchor::MidRight);
    }
    Pt pe = to_screen(rp, 0.0);
    c_.circle(pe.x, pe.y, marker, s.periapsis < s.atmosphere_depth ? WARN : ACCENT);
    c_.text("Pe", 2.8, VALUE, pe.x + 1.4 * u, pe.y, Anchor::MidLeft);

    double denom = 1 + e * std::cos(s.true_anomaly);
    double r_now = denom > 1e-3 ? p / denom : 0;
    if (r_now > 0) {
        Pt v = to_screen(r_now, s.true_anomaly);
        c_.circle(v.x, v.y, static_cast<float>(1.3 * u), VALUE);
        c_.circle(v.x, v.y, static_cast<float>(0.6 * u), BG);
        last_vessel_marker = v;
    }
}

void Display::inertial_orbit(const Rect& rect, const Snapshot& s) {
    // Keep the body and the inertial view axes fixed; plot the measured position directly.
    const double u = c_.u;
    const OrbitState& state = *s.orbit_state;
    if (!state.position.finite() || !state.velocity.finite()) return;
    auto conic = state.conic();
    double radius = std::max(s.body_radius, 1.0);
    double atmo = radius + s.atmosphere_depth;
    double reach;
    if (!conic) reach = std::max(4 * atmo, std::sqrt(dot(state.position, state.position)));
    else if (conic->apoapsis()) reach = std::max(atmo, *conic->apoapsis());
    else reach = std::max(4 * atmo, 2.5 * conic->periapsis());
    double margin = apsis_label_width() + 2.6 * u;
    Rect area = rect.inflate(-2 * margin, -10 * u).move(0, 2 * u);
    double scale = std::min(area.w, area.h) / (2 * reach);
    double cx = area.centerx(), cy = area.centery();
    auto point = [&](const Vec3& v) {
        auto [x, y] = state.project(v);
        return Pt{static_cast<float>(cx + x * scale), static_cast<float>(cy - y * scale)};
    };
    if (s.atmosphere_depth > 0)
        c_.circle(static_cast<float>(cx), static_cast<float>(cy), static_cast<float>(std::max(3.0, atmo * scale)), ATMOSPHERE);
    if (conic) {
        std::vector<Pt> segment;
        for (int index = 0; index < 241; ++index) {
            auto vector = conic->position(-PI + 2 * PI * index / 240);
            if (vector && dot(*vector, *vector) <= (12 * reach) * (12 * reach)) {
                segment.push_back(point(*vector));
            } else {
                if (segment.size() > 1) c_.polyline(segment, false, 1, ACCENT);
                segment.clear();
            }
        }
        if (segment.size() > 1) c_.polyline(segment, false, 1, ACCENT);
    }
    c_.circle(static_cast<float>(cx), static_cast<float>(cy), static_cast<float>(std::max(2.0, radius * scale)), BODY);
    c_.text(s.body, 2.8, VALUE, cx, cy, Anchor::Center);
    // At zero eccentricity the apsides have no unique direction: do not invent one.
    if (conic && conic->eccentricity > 1e-5) {
        if (conic->apoapsis()) {
            Pt ap = point(*conic->position(PI));
            c_.circle(ap.x, ap.y, static_cast<float>(.8 * u), GOOD);
            c_.text("Ap", 2.8, GOOD, ap.x - 1.4 * u, ap.y, Anchor::MidRight);
        }
        Pt pe = point(*conic->position(0));
        c_.circle(pe.x, pe.y, static_cast<float>(.8 * u), s.periapsis < s.atmosphere_depth ? WARN : ACCENT);
        c_.text("Pe", 2.8, VALUE, pe.x + 1.4 * u, pe.y, Anchor::MidLeft);
    }
    // The white ring followed during docking: never from true anomaly, attitude, target velocity, or the changing
    // periapsis direction.
    Pt position = point(state.position);
    c_.circle(position.x, position.y, static_cast<float>(1.3 * u), VALUE);
    c_.circle(position.x, position.y, static_cast<float>(.6 * u), BG);
    last_vessel_marker = position;
}

}  // namespace ksphmi
