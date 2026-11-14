#include "display.hpp"

#include <cmath>

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
constexpr Color FACE{11, 16, 23};

// Ring between two radii and two fractions of the dial, as convex quads (Canvas::polygon wants convex shapes).
void dial_sector(Canvas& c, double cx, double cy, double r_out, double r_in, double f0, double f1, Color color,
                 double step_degrees = 4.0) {
    auto fs = gauges::steps(f0, f1, step_degrees);
    for (size_t i = 0; i + 1 < fs.size(); ++i) {
        auto a = gauges::point(cx, cy, r_out, fs[i]), b = gauges::point(cx, cy, r_out, fs[i + 1]);
        auto d = gauges::point(cx, cy, r_in, fs[i + 1]), e = gauges::point(cx, cy, r_in, fs[i]);
        c.polygon({{float(a.first), float(a.second)}, {float(b.first), float(b.second)},
                   {float(d.first), float(d.second)}, {float(e.first), float(e.second)}}, color, false);
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

SDL_Texture* Display::gauge_face(int radius, bool live) {
    auto key = std::make_pair(radius, live);
    auto found = gauge_faces_.find(key);
    if (found != gauge_faces_.end()) return found->second;
    if (gauge_faces_.size() > 16) {
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
            double c = side, r = 2.0 * radius;
            f.circle(c, c, r, live ? Color{30, 40, 54} : Color{25, 33, 45});
            f.circle(c, c, std::round(.93 * r), FACE);
            dial_sector(f, c, c, .86 * r, .70 * r, 0, 1, Color{27, 36, 50}, 2);
            if (live) {
                auto dim = [](Color k) {
                    return Color{uint8_t(std::lround(.55 * k.r + .45 * FACE.r)), uint8_t(std::lround(.55 * k.g + .45 * FACE.g)),
                                 uint8_t(std::lround(.55 * k.b + .45 * FACE.b))};
                };
                dial_sector(f, c, c, .91 * r, .87 * r, 0, gauges::BAD_BELOW, dim(BAD), 2);
                dial_sector(f, c, c, .91 * r, .87 * r, gauges::BAD_BELOW, gauges::WARN_BELOW, dim(WARN), 2);
            }
            for (int step = 0; step <= 20; ++step) {
                double fr = step / 20.0;
                bool major = step % 10 == 0, ten = step % 2 == 0;
                double inner = major ? .55 : ten ? .60 : .64;
                Color color = live ? (major ? VALUE : ten ? LABEL : MUTED) : GRID;
                auto p0 = gauges::point(c, c, inner * r, fr), p1 = gauges::point(c, c, .68 * r, fr);
                f.line({float(p0.first), float(p0.second)}, {float(p1.first), float(p1.second)},
                       std::max(2.0f, rnd((major ? .045 : .03) * r)), color);
            }
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
    if (on_gauge) on_gauge(label, Rect::f(cx - radius, cy - radius, 2 * radius, 2 * radius));
    if (SDL_Texture* face = gauge_face(r, fraction.has_value())) {
        float side = static_cast<float>(2 * r + 4);
        // pygame centres the face with integer rects: get_rect(center=(int(cx), int(cy)))
        int left = static_cast<int>(cx) - (2 * r + 4) / 2, top = static_cast<int>(cy) - (2 * r + 4) / 2;
        c_.image(face, static_cast<float>(left), static_cast<float>(top), side, side);
    }
    Color color = MUTED;
    std::string centre, amount, reference;
    if (fraction) {
        color = *fraction <= gauges::BAD_BELOW ? BAD : *fraction <= gauges::WARN_BELOW ? WARN : GOOD;
        if (*fraction > 0) dial_sector(c_, cx, cy, .86 * r, .70 * r, 0, *fraction, color);
        // pointer across the track at the current level
        double a = gauges::angle(*fraction), dx = std::cos(a), dy = -std::sin(a);
        double nx = -dy * .035 * r, ny = dx * .035 * r;
        double x0 = cx + dx * .62 * r, y0 = cy + dy * .62 * r, x1 = cx + dx * .93 * r, y1 = cy + dy * .93 * r;
        c_.polygon({{float(x0 + nx), float(y0 + ny)}, {float(x1 + nx), float(y1 + ny)},
                    {float(x1 - nx), float(y1 - ny)}, {float(x0 - nx), float(y0 - ny)}}, VALUE);
        centre = fmt::str("%lld%%", py_round(100 * *fraction));
        amount = amount_text(*resource.amount) + " / " + amount_text(*resource.capacity);
        reference = amount_text(*resource.capacity) + " / " + amount_text(*resource.capacity);
    } else {
        bool absent = resource.capacity && *resource.capacity == 0;
        centre = absent ? "N/A" : "--";
        amount = reference = absent ? "NOT INSTALLED" : "--";
    }
    c_.text(centre, static_cast<float>(.44 * r / u), color, cx, cy, Anchor::Center);
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
