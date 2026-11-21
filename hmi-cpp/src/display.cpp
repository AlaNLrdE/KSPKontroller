#include "display.hpp"

#include "audio.hpp"
#include "cockpit.hpp"
#include "mechjebpage.hpp"
#include "gameskin.hpp"
#include "orbitart.hpp"
#include "resource_ui.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>

#include "apollo.hpp"
#include "fmt.hpp"
#include "gauges.hpp"
#include "log.hpp"
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
      net_(cfg.krpc.address),
      cfg_(cfg),
      config_skin_(known_skin(cfg.display.skin) || cfg.display.skin == "auto" ? cfg.display.skin : "auto"),
      skin_(config_skin_ == "auto" ? auto_skin("") : config_skin_) {
    orbit_art_ = std::make_unique<OrbitArt>();
    target_art_ = std::make_unique<OrbitArt>();
    if (!known_skin(cfg.display.skin) && cfg.display.skin != "auto")
        log::warning("ksphmi.display", "unknown skin '" + cfg.display.skin + "', using auto");
    log::info("ksphmi.display", "skin: " + skin_ + (config_skin_ == "auto" ? " (auto until a game answers)" : " (config)"));
}

bool Display::known_skin(const std::string& name) { return name == "classic" || name == "apollo" || name == "steampunk" || name == "ksp1" || name == "ksp2"; }

void Display::set_skin(const std::string& name, const char* why) {
    if (!known_skin(name) || name == skin_) return;
    log::info("ksphmi.display", "skin: " + name + " (" + why + ", was " + skin_ + ")");
    skin_ = name;
}

/// ORBIT's central body on screen: true scale, but never a dot. Around the Sun an orbit of tens of Gm made Kerbol
/// 2-3 px (user, 2026-11-16), so the disc gets at least 2.4u, capped at 45 % of the periapsis on screen so it never
/// covers the orbit it is the focus of.
double shown_body_radius(double true_px, double periapsis_px, double u) {
    double floor = 2.4 * u;
    if (std::isfinite(periapsis_px) && periapsis_px > 0) floor = std::min(floor, .45 * periapsis_px);
    return std::max({2.0, true_px, floor});
}

FlightLayout flight_layout(const Canvas& c, bool flight_deck) {
    if (flight_deck) {
        const double u = c.u, gap = .9 * u, x = c.w * .5, width = c.w - x - 1.4 * u;
        const double vessel = width * .43;
        return FlightLayout{Rect::f(x, 7*u, width-vessel-gap, 33*u),
                            Rect::f(x+width-vessel, 7*u, vessel, 33*u),
                            Rect::f(x, 41*u, width, c.h-42.4*u)};
    }
    const double u = c.u;
    const double top = 13 * u, bottom = c.h - 2.5 * u, gap = 1.6 * u, left_w = c.w * 0.44;
    const double right_x = left_w + gap, right_w = c.w - left_w - gap - 2.5 * u;
    // 34u keeps all five NavBall readout rows above the SAS MODE lamps; VESSEL 30u wide suits a rocket's side view
    const double row_h = 34 * u, vessel_w = 30 * u;
    FlightLayout l;
    l.orbit = Rect::f(right_x, top, right_w - vessel_w - gap, row_h);
    l.vessel = Rect::f(right_x + right_w - vessel_w, top, vessel_w, row_h);
    l.navball = Rect::f(right_x, top + row_h + gap, right_w, bottom - top - row_h - gap);
    return l;
}

namespace {

/// Blue (cold) to green, yellow, orange and red at the part's maximum temperature.
Color heat_color(int percent) {
    static const std::pair<double, Color> stops[] = {
        {0, {40, 90, 220}}, {.4, {60, 200, 120}}, {.65, {240, 220, 60}}, {.85, {250, 140, 40}}, {1, {235, 40, 40}}};
    const double t = std::clamp(percent, 0, 100) / 100.0;
    for (size_t i = 1; i < std::size(stops); ++i) {
        if (t <= stops[i].first) {
            const double f = (t - stops[i - 1].first) / (stops[i].first - stops[i - 1].first);
            const Color a = stops[i - 1].second, b = stops[i].second;
            auto mix = [f](uint8_t x, uint8_t y) { return uint8_t(std::lround(x + (y - x) * f)); };
            return Color{mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b)};
        }
    }
    return stops[std::size(stops) - 1].second;
}

}  // namespace

void Display::vessel(const Rect& rect, const Snapshot& s) {
    const double u = c_.u;
    panel(c_, rect);
    c_.text("VESSEL", 2.6, LABEL, rect.x + 1.8 * u, rect.y + 1.8 * u);
    const double bw = 5.6 * u, bh = 2.3 * u, gap = .5 * u;
    const double bx = rect.right() - 1.8 * u - 2 * bw - gap, by = rect.y + 1.7 * u;
    for (int i = 0; i < 2; ++i) {
        const bool on = (i == 1) == vessel_heat_;
        const Rect r = Rect::f(bx + i * (bw + gap), by, bw, bh);
        c_.rounded_rect(r, float(.3 * u), on ? Color{34, 60, 44} : Color{36, 44, 58});
        c_.text(i ? "HEAT" : "FUEL", 1.2, on ? GOOD : LABEL, r.centerx(), r.centery(), Anchor::Center);
    }
    vessel_contents(Rect::f(rect.x + 1.2 * u, rect.y + 5 * u, rect.w - 2.4 * u, rect.h - 6.2 * u), s,
                    VesselStyle{PANEL, MUTED, GRID, GOOD, BG, LABEL});
}

void Display::vessel_contents(const Rect& area, const Snapshot& s, const VesselStyle& style) {
    const double u = c_.u;
    const Rect previous_clip = c_.clip();
    c_.set_clip(previous_clip.clip(area));
    const double legend_h = 3.4 * u;
    const Rect plot = Rect::f(area.x + .6 * u, area.y + .6 * u, area.w - 1.2 * u, area.h - legend_h - 1.2 * u);
    const VesselView* view = s.vessel_view.get();
    if (!view || view->parts.empty()) {
        c_.text("NO VESSEL DATA", 1.5, style.text, plot.centerx(), plot.centery(), Anchor::Center);
    } else {
        double x0 = 1e18, x1 = -1e18, y0 = 1e18, y1 = -1e18;
        for (const VesselPart& p : view->parts)
            for (const Point2& q : p.hull)
                x0 = std::min(x0, q.first), x1 = std::max(x1, q.first), y0 = std::min(y0, q.second), y1 = std::max(y1, q.second);
        const double scale = std::min(plot.w / std::max(x1 - x0, .01), plot.h / std::max(y1 - y0, .01));
        const double cx = plot.x + plot.w / 2.0, cy = plot.y + plot.h / 2.0, mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
        const float lw = std::max(1.0f, float(.15 * u));
        for (size_t index : vessel_draw_order(*view)) {
            const VesselPart& part = view->parts[index];
            std::vector<Point2> screen;
            std::vector<Pt> pts;
            for (const Point2& q : part.hull) {
                screen.push_back({cx + (q.first - mx) * scale, cy - (q.second - my) * scale});
                pts.push_back(Pt{float(screen.back().first), float(screen.back().second)});
            }
            if (vessel_heat_) {
                c_.polygon(pts, heat_color(part.heat));
                c_.polyline(pts, true, lw, style.heat_outline);
                continue;
            }
            c_.polygon(pts, part.fuel >= 0 ? style.empty : style.background);
            if (part.fuel > 0) {
                double top = 1e18, bottom = -1e18;
                for (const Point2& q : screen) top = std::min(top, q.second), bottom = std::max(bottom, q.second);
                std::vector<Pt> fill;
                for (const Point2& q : clip_below(screen, bottom - (bottom - top) * part.fuel / 100.0))
                    fill.push_back(Pt{float(q.first), float(q.second)});
                if (fill.size() >= 3) c_.polygon(fill, style.fuel);
            }
            c_.polyline(pts, true, lw, style.outline);
        }
    }
    // legend
    const double lx = area.x + 1.5 * u, lr = area.right() - 1.5 * u, ly = area.bottom() - legend_h;
    const double bar_h = 1.0 * u;
    if (vessel_heat_) {
        const int steps = 40;
        const double step = (lr - lx) / steps;
        for (int i = 0; i < steps; ++i)
            c_.fill_rect(float(lx + i * step), float(ly), float(step + 1), float(bar_h), heat_color(i * 100 / (steps - 1)));
        c_.text("0", 1.1, style.text, lx, ly + bar_h + .3 * u);
        c_.text("% OF MAX TEMP", 1.1, style.text, (lx + lr) / 2, ly + bar_h + .3 * u, Anchor::MidTop);
        c_.text("100", 1.1, style.text, lr, ly + bar_h + .3 * u, Anchor::TopRight);
    } else {
        const double mid = (lx + lr) / 2;
        c_.fill_rect(float(lx), float(ly), float(mid - lx - u), float(bar_h), style.fuel);
        c_.fill_rect(float(mid + u), float(ly), float(lr - mid - u), float(bar_h), style.empty);
        c_.text("FUEL", 1.1, style.text, (lx + mid - u) / 2, ly + bar_h + .3 * u, Anchor::MidTop);
        c_.text("EMPTY", 1.1, style.text, (mid + u + lr) / 2, ly + bar_h + .3 * u, Anchor::MidTop);
    }
    c_.set_clip(previous_clip);
}

void Display::choose_skin(const std::string& name) {
    if (name.empty() || known_skin(name)) chosen_skin_ = name;
}

Rect Display::orbit_text(const std::string& s, float units, Color color, double x, double y, Anchor a) {
    color = orbit_ink(color);
    if (orbit_label) return orbit_label(s, units, color, x, y, a);
    return c_.text(s, units, color, x, y, a);
}

Display::~Display() {
    for (auto& [key, texture] : gauge_faces_) SDL_DestroyTexture(texture);
    free_layer();
}

void Display::free_layer() {
    if (layer_) SDL_DestroyTexture(layer_), layer_ = nullptr;
    if (under_) SDL_DestroyTexture(under_), under_ = nullptr;
    under_w_ = under_h_ = 0;
    layer_valid_ = false;
}

bool Display::set_layer_cache(bool on) {
    layer_cache_ = on && SDL_RenderTargetSupported(c_.renderer());
    if (!layer_cache_) free_layer();
    return layer_cache_;
}

void Display::draw(const Snapshot& snap, double now) {
    layer_valid_ = false;
    if (layer_cache_ && !layer_) {
        layer_ = SDL_CreateTexture(c_.renderer(), SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, c_.w, c_.h);
        if (layer_) {
            SDL_SetTextureBlendMode(layer_, SDL_BLENDMODE_NONE);
        } else {
            log::warning("ksphmi.display", std::string("layer cache off: ") + SDL_GetError());
            layer_cache_ = false;
        }
    }
    if (!layer_cache_) return draw_screen(snap, now);
    // Everything goes into the layer; the NavBall reports its globe square just before drawing the globe, so
    // the pixels under it (panel, apollo bezel well) can be put back before each NavBall-only frame.
    c_.set_target(layer_);
    navball_.before_globe = [this](const Rect& square) { capture_under_globe(square); };
    draw_screen(snap, now);
    navball_.before_globe = nullptr;
    c_.set_target(nullptr);
    c_.blit(layer_, Rect(0, 0, c_.w, c_.h), Rect(0, 0, c_.w, c_.h));
    c_.flush();
}

void Display::capture_under_globe(const Rect& square) {
    const Rect s = Rect(0, 0, c_.w, c_.h).clip(square);
    if (s.w <= 0 || s.h <= 0) return;
    if (!under_ || under_w_ != s.w || under_h_ != s.h) {
        if (under_) SDL_DestroyTexture(under_);
        under_ = SDL_CreateTexture(c_.renderer(), SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, s.w, s.h);
        under_w_ = under_ ? s.w : 0, under_h_ = under_ ? s.h : 0;
        if (!under_) return;
        SDL_SetTextureBlendMode(under_, SDL_BLENDMODE_NONE);
    }
    c_.set_target(under_);
    c_.blit(layer_, s, Rect(0, 0, s.w, s.h));
    c_.set_target(layer_);
    under_rect_ = s;
    layer_valid_ = true;
}

bool Display::animate() {
    if (!layer_cache_ || !layer_valid_) return false;
    c_.blit(layer_, Rect(0, 0, c_.w, c_.h), Rect(0, 0, c_.w, c_.h));
    c_.blit(under_, Rect(0, 0, under_rect_.w, under_rect_.h), under_rect_);
    const Rect keep = c_.clip();
    c_.set_clip(under_rect_);
    navball_.redraw(c_);
    c_.set_clip(keep);
    c_.flush();
    return true;
}

void Display::draw_screen(const Snapshot& snap, double now) {
    // A keyboard choice wins; then the simulator's context reply; otherwise the [display] setting, where "auto"
    // follows the game that answers (ksp1 for KSP 1 over kRPC, ksp2 for KSP2 Redux; user 2026-11-16). Each skin keeps
    // its own caches (built lazily, released with the Display), so switching back and forth costs nothing after
    // the first.
    if (!chosen_skin_.empty()) set_skin(chosen_skin_, "keyboard");
    else if (known_skin(snap.skin)) set_skin(snap.skin, "context");
    else if (config_skin_ == "auto") set_skin(auto_skin(snap.game), snap.game.empty() ? "auto" : "auto, game");
    else set_skin(config_skin_, "config");
    cockpit_shown_ = cockpit_scene(snap);
    mechjeb_shown_ = mechjeb_scene(snap);
    mechjeb_path_ = mechjeb_path(snap);
    navball_.flight_deck = skin_ == "apollo" || skin_ == "ksp2";
    if (skin_ == "ksp1" || skin_ == "ksp2") {
        auto& skin=skin_=="ksp2"?ksp2_:ksp1_;
        if(!skin)skin=std::make_unique<GameSkin>(*this,skin_=="ksp2");
        skin->draw(snap,now);return;
    }
    if (skin_ == "steampunk") {
        if (!steampunk_) steampunk_ = std::make_unique<ApolloSkin>(*this, cfg_, true);
        steampunk_->draw(snap, now);
        return;
    }
    if (skin_ == "apollo") {
        if (!apollo_) apollo_ = std::make_unique<ApolloSkin>(*this, cfg_);
        apollo_->draw(snap, now);
        return;
    }
    c_.clear(BG);
    header(snap, now);
    if (cockpit_shown_) cockpit(snap, now);
    else if (mechjeb_shown_) mechjeb(snap, now);
    else if (snap.state == State::Flight) flight(snap);
    else if (snap.state == State::NoVessel && find_view(snap.scene)) facility(snap);
    else waiting(snap, now);
    c_.flush();
}

std::optional<Ctl> Display::cockpit_hit(int x, int y) const {
    if (!cockpit_shown_) return std::nullopt;
    return ksphmi::cockpit_hit(cockpit_layout(c_), x, y);
}

void Display::cockpit(const Snapshot& s, double now) {
    ClassicCockpitPainter painter;
    const CockpitView view{cockpit_input_, now};
    paint_cockpit(c_, true, s, view, painter);
    paint_cockpit(c_, false, s, view, painter);
}

std::optional<MjControl> Display::mechjeb_hit(int x, int y) const {
    if (!mechjeb_shown_) return std::nullopt;
    const MjPage page = mechjeb_input_ ? mechjeb_input_->page() : MjPage::Menu;
    const bool pad = mechjeb_input_ && mechjeb_input_->keypad().is_open();
    return ksphmi::mechjeb_hit(mechjeb_layout(c_, page, mechjeb_path_), pad, keypad_layout(c_), x, y);
}

void Display::mechjeb(const Snapshot& s, double now) {
    ClassicCockpitPainter painter;
    const MechJebView view{mechjeb_input_, now};
    paint_mechjeb(c_, true, s, view, painter);
    paint_mechjeb(c_, false, s, view, painter);
}

std::string Display::mechjeb_key(const Snapshot& s) const {
    const MjPage page = mechjeb_input_ ? mechjeb_input_->page() : MjPage::Menu;
    return std::string(page == MjPage::Ascent ? "ascent" : "menu") + ":" + std::to_string(mechjeb_path(s));
}

void Display::header(const Snapshot& snap, double now) {
    const double u = c_.u;
    const double top = 1.5 * u;
    const int w = c_.w;
    if (snap.state == State::Flight) {
        c_.text(snap.vessel.empty() ? "Unnamed vessel" : snap.vessel, 5.2, VALUE, 2.5 * u, top);
        c_.text(snap.body + "  \xC2\xB7  " + fmt::situation(snap.situation), 3.0, LABEL, 2.5 * u, top + 5.4 * u);
        c_.text(fmt::clock(snap.ut, day_hours_), 3.4, LABEL, w - 2.5 * u, top + 5.0 * u, Anchor::TopRight);
    } else if (snap.state == State::NoVessel && snap.scene == "vab" && snap.ship) {
        const std::string name = snap.ship->name.empty() ? "Untitled ship" : snap.ship->name;
        c_.text(clip_utf8(name, 60), 5.2, VALUE, 2.5 * u, top);
        c_.text("VEHICLE ASSEMBLY  \xC2\xB7  " + editor_mode_label(snap.ship->editor_mode), 3.0, LABEL, 2.5 * u, top + 5.4 * u);
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
    orbit_art_->body(c_, "Kerbin", cx, cy-12*u, 6*u, false);
    double ang = waiting_sweep_angle("classic", now);  // the radar ping follows it
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

// ------------------------------------------------------------------------------------------- VAB ship report
namespace {
// Largest size (0.05 unit steps, at most `units`, at least `least`) at which `s` fits in `room` pixels.
float fit_units(Canvas& c, const std::string& s, float units, double room, float least = 1.2f) {
    float size = units;
    while (size > least && c.text_width(c.font(size), s) > room) size = std::max(least, size - .05f);
    return size;
}

std::string dv_text(double v) { return std::isfinite(v) ? fmt::str("%.0f m/s", v) : "--"; }

// Stage colours of the stacked bar, cycled from the launch stage up.
constexpr Color STAGE_COLORS[] = {{70, 200, 255}, {96, 222, 140}, {255, 186, 72}, {200, 140, 255}, {255, 120, 160}};

void outline(Canvas& c, const Rect& r, float t, Color color) {
    c.fill_rect(float(r.x), float(r.y), float(r.w), t, color);
    c.fill_rect(float(r.x), float(r.bottom() - t), float(r.w), t, color);
    c.fill_rect(float(r.x), float(r.y), t, float(r.h), color);
    c.fill_rect(float(r.right() - t), float(r.y), t, float(r.h), color);
}
}  // namespace

TargetChoice Display::vab_target(const ShipReport* report) const {
    return choose_target(report, cfg_.vab.destinations, cfg_.vab.target);
}

void Display::editor_report(const Snapshot& snap) {
    const EditorPage page = editor_page(*snap.ship);
    const double u = c_.u, gap = 1.6 * u, x = 2.5 * u, width = c_.w - 5 * u;
    const double metric_w = (width - 3 * gap) / 4;
    for (size_t i = 0; i < page.metrics.size(); ++i)
        tile(Rect::f(x + i * (metric_w + gap), 13 * u, metric_w, 7 * u),
             page.metrics[i].first, page.metrics[i].second, VALUE);
    const double panel_w = (width - gap) / 2;
    auto table = [&](double px, const std::string& title, const std::vector<EditorRow>& rows) {
        const Rect box = Rect::f(px, 22 * u, panel_w, 72 * u);
        panel(c_, box);
        if (on_vab_panel) on_vab_panel(title, box);
        const double left = box.x + 1.3 * u, right = box.right() - 1.3 * u, room = right - left;
        c_.text(title, fit_units(c_, title, 2.3f, room), LABEL, left, box.y + u);
        const PageSlice slice = page_slice(rows.size(), 10, snap.received);
        for (size_t i = slice.begin; i < slice.end; ++i) {
            const auto& row = rows[i];
            const double y = box.y + 5.3 * u + (i - slice.begin) * 6 * u;
            const double label_room = row.value.empty() ? room : room * .52;
            c_.text(row.label, fit_units(c_, row.label, 2.1f, label_room), row.warning ? WARN : VALUE, left, y);
            if (!row.value.empty())
                c_.text(row.value, fit_units(c_, row.value, 2.1f, room * .45), row.warning ? WARN : ACCENT, right, y, Anchor::TopRight);
            if (!row.detail.empty()) c_.text(row.detail, fit_units(c_, row.detail, 1.8f, room), LABEL, left, y + 2.6 * u);
            c_.line({float(left), float(y + 5.1 * u)}, {float(right), float(y + 5.1 * u)}, 1, GRID);
        }
        c_.text(fmt::str("PAGE %zu / %zu%s", slice.page + 1, slice.pages, slice.pages > 1 ? "  /  AUTO 8s" : ""),
                1.8, MUTED, left, box.bottom() - 3.2 * u);
    };
    table(x, page.left_title, page.left);
    table(x + panel_w + gap, page.right_title, page.right);
    c_.text("EDITOR SYNC  /  SAS NEXT  /  RCS PREVIOUS", 1.9, MUTED, x, 96 * u);
}

void Display::vab_report(const Snapshot& snap) {
    if (snap.ship->editor_mode != "construction") { editor_report(snap); return; }
    const ShipReport& r = *snap.ship;
    const double u = c_.u;
    const double top = 13 * u, bottom = c_.h - 2.5 * u, x0 = 2.5 * u, gap = 1.6 * u;
    const double left_r = c_.w * 0.44, x1 = left_r + gap, right_r = c_.w - 2.5 * u;
    const float line = std::max(1.0f, static_cast<float>(static_cast<int>(0.2 * u)));
    const bool pending = r.delta_v_pending;
    // KSP's own totals (the app's situation) for comparison with the game; the mission delta-v judges destinations
    const double total = r.total_delta_v(), vac = r.total_delta_v_vac();
    const MissionDeltaV mission = mission_delta_v(r, cfg_.vab.atmosphere_dv);
    const double mtotal = mission.total;
    const TargetChoice choice = vab_target(&r);
    const Destination* target = choice.destination;

    // ---- left: delta-v
    const Rect dvp = Rect::f(x0, top, left_r - x0, bottom - top);
    if (on_vab_panel) on_vab_panel("delta-v", dvp);
    panel(c_, dvp);
    const double px = dvp.x + 1.3 * u, pr = dvp.right() - 1.3 * u, pw = pr - px;
    c_.text("\xCE\x94v TOTAL", 2.4, LABEL, px, dvp.y + u);
    const std::string sit = r.situation_label();
    c_.text(sit, fit_units(c_, sit, 1.9f, pw * .55), MUTED, pr, dvp.y + 1.2 * u, Anchor::TopRight);
    Color total_color = VALUE;  // the mission delta-v against the target
    if (!pending && target) total_color = mtotal >= target->delta_v ? GOOD : BAD;
    c_.text(pending ? "--" : dv_text(total), 8, pending ? WARN : VALUE, px, dvp.y + 4.2 * u);
    c_.text(pending ? "vacuum --" : "vacuum " + dv_text(vac), 2.4, LABEL, px, dvp.y + 14 * u);
    const std::string mission_text = pending ? "MISSION \xCE\x94v --" : "MISSION \xCE\x94v " + dv_text(mtotal);
    c_.text(mission_text, fit_units(c_, mission_text, 2.4f, pw * .5), total_color, pr, dvp.y + 14 * u, Anchor::TopRight);
    if (target) {
        const std::string want = "TARGET  " + destination_label(target->key) + "  " + dv_text(target->delta_v) +
                                 (choice.from_app ? "  \xC2\xB7 from KSP \xCE\x94v app" : "  \xC2\xB7 from config");
        c_.text(want, fit_units(c_, want, 2.1f, pw * .62), LABEL, px, dvp.y + 18.2 * u);
        if (!pending) {
            const double margin = mtotal - target->delta_v;
            const std::string m = margin >= 0 ? fmt::str("+%.0f m/s spare", margin) : fmt::str("%.0f m/s short", margin);
            c_.text(m, fit_units(c_, m, 2.6f, pw * .36), margin >= 0 ? GOOD : BAD, pr, dvp.y + 17.9 * u, Anchor::TopRight);
        }
    } else {
        c_.text("No target: set [vab] target in config.toml", 2.0, MUTED, px, dvp.y + 18.2 * u);
    }
    // mission delta-v against the target: a bar with the target as a tick
    const Rect bar = Rect::f(px, dvp.y + 22.4 * u, pw, 2.0 * u);
    c_.rect(bar, GRID);
    const double scale = std::max({mtotal, target ? target->delta_v : 0.0, 1.0}) * 1.08;
    if (!pending && mtotal > 0)
        c_.fill_rect(float(bar.x), float(bar.y), float(bar.w * std::min(1.0, mtotal / scale)), float(bar.h), total_color);
    if (target) {
        const float tx = float(bar.x + bar.w * target->delta_v / scale);
        c_.fill_rect(tx - line, float(bar.y - .6 * u), 2 * line, float(bar.h + 1.2 * u), VALUE);
    }

    // stacked stage bar, launch stage on the left
    c_.text("STAGE \xCE\x94v", 1.9, LABEL, px, dvp.y + 26.6 * u);
    const Rect stack = Rect::f(px, dvp.y + 29.4 * u, pw, 3.4 * u);
    c_.rect(stack, GRID);
    if (!pending && total > 0) {
        double x = stack.x;
        int index = 0;
        for (const auto& st : r.stages) {
            if (!std::isfinite(st.delta_v) || st.delta_v <= 0) continue;
            const double w = stack.w * st.delta_v / total;
            const Color col = STAGE_COLORS[index++ % (sizeof STAGE_COLORS / sizeof STAGE_COLORS[0])];
            c_.fill_rect(float(x), float(stack.y), float(std::max(1.0, w - line)), float(stack.h), col);
            const std::string n = std::to_string(st.stage);
            if (c_.text_width(c_.font(1.9f), n) + .8 * u < w)
                c_.text(n, 1.9, BG, x + w / 2, stack.centery(), Anchor::Center);
            x += w;
        }
    }

    // stage table
    const double ty = dvp.y + 35.6 * u;
    const double c_stage = px, c_dv = px + pw * .30, c_vac = px + pw * .46, c_twr = px + pw * .58,
                 c_burn = px + pw * .71, c_events = px + pw * .75;
    c_.text("STAGE", 1.8, MUTED, c_stage, ty);
    c_.text("\xCE\x94v", 1.8, MUTED, c_dv, ty, Anchor::TopRight);
    c_.text("VAC", 1.8, MUTED, c_vac, ty, Anchor::TopRight);
    c_.text("TWR", 1.8, MUTED, c_twr, ty, Anchor::TopRight);
    c_.text("BURN", 1.8, MUTED, c_burn, ty, Anchor::TopRight);
    c_.text("FIRES", 1.8, MUTED, c_events, ty);
    c_.line({float(px), float(ty + 3 * u)}, {float(pr), float(ty + 3 * u)}, line, GRID);
    const double row0 = ty + 4 * u, pitch = 9.5 * u;
    const int rows = std::max(1, static_cast<int>((dvp.bottom() - 1.5 * u - row0) / pitch));
    if (pending && r.empty_craft()) {  // KSP never computes delta-v without parts: not "calculating"
        c_.text("EMPTY CRAFT", 3.4, VALUE, dvp.centerx(), row0 + 3 * u, Anchor::MidTop);
        c_.text("Add parts to see the delta-v report", 2.2, MUTED, dvp.centerx(), row0 + 9 * u, Anchor::MidTop);
    } else if (pending) {
        const Rect plate = Rect::f(px, row0 + 1.5 * u, pw, 12 * u);
        outline(c_, plate, line * 2, WARN);
        c_.text("Calculating \xCE\x94v\xE2\x80\xA6", 3.4, WARN, dvp.centerx(), row0 + 3 * u, Anchor::MidTop);
        c_.text("KSP is computing the stage delta-v of this ship", 2.2, MUTED, dvp.centerx(), row0 + 9 * u, Anchor::MidTop);
    } else if (r.stages.empty()) {
        c_.text("No stages", 3.0, MUTED, dvp.centerx(), row0 + 3 * u, Anchor::MidTop);
    }
    const PageSlice stages = page_slice(r.stages.size(), rows, snap.received);
    for (size_t i = stages.begin; !pending && i < stages.end; ++i) {
        const StageReport& st = r.stages[i];
        double y = row0 + (i - stages.begin) * pitch;
        // [vab] TWR thresholds: launch stage red / amber, upper stages amber (stages without delta-v not judged)
        const TwrLevel level = twr_level(r, static_cast<size_t>(i), cfg_.vab.twr);
        const Color twr_color = level == TwrLevel::Bad ? BAD : level == TwrLevel::Warn ? WARN : VALUE;
        c_.text(std::to_string(st.stage), 3.0, ACCENT, c_stage, y);
        c_.text(std::isfinite(st.delta_v) ? fmt::str("%.0f", st.delta_v) : "--", 3.0, VALUE, c_dv, y, Anchor::TopRight);
        c_.text(std::isfinite(st.delta_v_vac) ? fmt::str("%.0f", st.delta_v_vac) : "--", 2.4, LABEL, c_vac, y + .5 * u,
                Anchor::TopRight);
        c_.text(std::isfinite(st.twr) ? fmt::str("%.2f", st.twr) : "--", 3.0, twr_color, c_twr, y, Anchor::TopRight);
        c_.text(std::isfinite(st.burn) ? fmt::duration(st.burn, day_hours_) : "--", 2.4, LABEL, c_burn, y + .5 * u,
                Anchor::TopRight);
        const std::string ev = events_text(st);
        if (!ev.empty()) c_.text(ev, fit_units(c_, ev, 2.0f, pr - c_events), LABEL, c_events, y + .7 * u);
        const auto details = stage_detail_lines(st);
        const PageSlice parts = page_slice(details.size(), 2, snap.received / stages.pages);
        for (size_t j = parts.begin; j < parts.end; ++j)
            c_.text(details[j], fit_units(c_, details[j], 1.65f, pw), LABEL, px, y + (3.4 + 2.0 * (j - parts.begin)) * u);
    }

    // ---- right: destinations
    const int n = static_cast<int>(cfg_.vab.destinations.size());
    const int cols = 2, drows = std::max(1, (std::min(n, MAX_DESTINATIONS) + cols - 1) / cols);
    const double dpitch = 4.4 * u, dest_h = 7.4 * u + 6 * dpitch;
    const Rect dp = Rect::f(x1, top, right_r - x1, dest_h);
    if (on_vab_panel) on_vab_panel("destinations", dp);
    panel(c_, dp);
    const double dx = dp.x + 1.3 * u, dw = dp.w - 2.6 * u, cell_w = (dw - u) / cols;
    c_.text("DESTINATIONS", 2.4, LABEL, dx, dp.y + u);
    c_.text("\xCE\x94v FROM THE PAD", 1.7, MUTED, dp.right() - 1.3 * u, dp.y + 1.3 * u, Anchor::TopRight);
    // what the lamps are judged with: the mission delta-v and its basis
    const std::string basis = (pending ? std::string("MISSION \xCE\x94v --") : "MISSION \xCE\x94v " + dv_text(mtotal)) +
                              "  \xC2\xB7  " + mission_basis(r, mission);
    c_.text(basis, fit_units(c_, basis, 1.8f, dw), mission.kerbin_asl ? LABEL : WARN, dx, dp.y + 4.0 * u);
    if (n == 0) c_.text("No destinations in config.toml [vab.destinations]", 2.0, MUTED, dp.centerx(), dp.centery(), Anchor::Center);
    for (int i = 0; i < n && i < MAX_DESTINATIONS; ++i) {
        const Destination& d = cfg_.vab.destinations[i];
        const int col = i / drows, row = i % drows;  // down the first column, then the second
        const Rect cell = Rect::f(dx + col * (cell_w + u), dp.y + 7.2 * u + row * dpitch, cell_w, dpitch - .6 * u);
        const bool reach = !pending && mtotal >= d.delta_v;
        if (target && d.key == target->key) outline(c_, cell.inflate(.3 * u, .2 * u), line, ACCENT);
        c_.circle(float(cell.x + 1.4 * u), float(cell.centery()), float(.8 * u), pending ? MUTED : reach ? GOOD : BAD);
        const std::string value = fmt::str("%.0f", d.delta_v);
        const double vw = c_.text_width(c_.font(1.9f), value);
        c_.text(value, 1.9, LABEL, cell.right() - .6 * u, cell.centery(), Anchor::MidRight);
        const std::string name = destination_label(d.key);
        c_.text(name, fit_units(c_, name, 2.1f, cell.w - 3.2 * u - vw - 1.2 * u), pending || reach ? VALUE : LABEL,
                cell.x + 2.8 * u, cell.centery(), Anchor::MidLeft);
    }

    // ---- right: vehicle summary
    const double vy = dp.bottom() + gap;
    c_.text("VEHICLE", 2.4, LABEL, x1 + .3 * u, vy);
    const double tw = (right_r - x1 - .8 * u) / 2, th = 7 * u, tg = .8 * u, t0 = vy + 3.4 * u;
    auto opt = [](const std::optional<double>& v, const char* f) { return v ? fmt::str(f, *v) : std::string("--"); };
    auto num = [](const std::optional<int>& v) { return v ? std::to_string(*v) : std::string("--"); };
    const std::string size = r.height && r.width ? fmt::str("%.1f \xC3\x97 %.1f m", *r.height, *r.width) : "--";
    const struct {
        const char* label;
        std::string value;
    } tiles[8] = {
        {"MASS", opt(r.mass, "%.2f t")},          {"DRY MASS", opt(r.dry, "%.2f t")},
        {"COST", opt(r.cost, "%.0f")},            {"FUNDS", r.funds ? fmt::str("%.0f", *r.funds) : "--"},
        {"PARTS", num(r.parts)},                  {"CREW / SEATS", num(r.crew) + " / " + num(r.seats)},
        {"HEIGHT \xC3\x97 WIDTH", size},          {"ENGINES", num(r.engines)},
    };
    for (int i = 0; i < 8; ++i) {
        const Rect t = Rect::f(x1 + (i % 2) * (tw + tg), t0 + (i / 2) * (th + tg), tw, th);
        if (on_vab_panel) on_vab_panel(tiles[i].label, t);
        Color color = VALUE;
        if (i == 3 && r.funds && r.cost) color = *r.funds >= *r.cost ? GOOD : BAD;  // can the program pay for it
        tile(t, tiles[i].label, tiles[i].value, color);
    }
    c_.text(fmt::str("SAS NEXT / RCS PREV | STAGES %zu/%zu", stages.page + 1, stages.pages), 2.2, MUTED, x1,
            bottom - 2.6 * u);
}

void Display::facility(const Snapshot& snap) {
    if (snap.scene == "vab" && snap.ship) {  // the live editor report replaces the VAB cards
        vab_report(snap);
        return;
    }
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
    const double top = 13 * u, bottom = c_.h - 2.5 * u;
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

    const FlightLayout layout = flight_layout(c_);
    orbit(layout.orbit, s);
    vessel(layout.vessel, s);
    navball_.draw(c_, layout.navball, s.nav);
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
    const auto resource_names = hmi_resource_names(snap);
    const auto resource_labels = hmi_resource_labels(snap, RESOURCE_LABELS.data());
    const bool native = snap.systems && snap.systems->native_resources;
    c_.text(native ? "VESSEL TOTALS \xC2\xB7 NATIVE UNITS" : "VESSEL TOTALS \xC2\xB7 KSP UNITS", 1.7, MUTED, rect.right() - 1.3 * u, rect.y + u, Anchor::TopRight);
    if (!snap.systems) {
        c_.text("Resource data unavailable", 2.4, MUTED, rect.centerx(), rect.centery(), Anchor::Center);
        return;
    }
    std::vector<std::pair<int, const Resource*>> visible;  // index in RESOURCE_NAMES, resource
    for (int i = 0; i < static_cast<int>(resource_names.size()); ++i) {
        const Resource* found = nullptr;
        for (const auto& r : snap.systems->resources)
            if (r.name == resource_names[i]) found = &r;  // last one wins, as a dict built from the tuple
        if (found && (i < 4 || found->capacity.value_or(0) > 0)) visible.emplace_back(i, found);
    }
    if (visible.empty()) return;
    std::vector<int> names;
    for (const auto& v : visible) names.push_back(v.first);
    if (!gauge_layout_ || !(gauge_layout_->rect == rect) || gauge_layout_->names != names ||
        gauge_layout_->labels != resource_labels) {
        GaugeLayout g;
        g.rect = rect;
        g.names = names;
        g.labels = resource_labels;
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
        for (int n : names) widest = std::max(widest, c_.text_width(c_.font(GAUGE_LABEL), resource_labels[n]));
        g.label_units = static_cast<float>(std::floor(GAUGE_LABEL * std::min(1.0, room / widest) * 20) / 20);
        gauge_layout_ = std::move(g);
    }
    const GaugeLayout& g = *gauge_layout_;
    for (size_t i = 0; i < visible.size(); ++i) {
        const auto& cell = g.cells[i];
        gauge(*visible[i].second, resource_labels[visible[i].first], cell[0], cell[1], g.radius, cell[3] - 2 * u,
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
        c_.text((!data || data->native_resources) ? "Stage data unavailable" : "No pending activation stages", 2.2, MUTED, rect.centerx(),
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

Color Display::orbit_ink(Color original) const {
    if (!orbit_crt) return original;
    if (original == BG) return Color{0,8,3};
    if (original == ATMOSPHERE) return Color{3,32,12};
    if (original == MANEUVER || original == orbit_style.target || original == orbit_style.encounter || original == MUTED)
        return Color{48,178,84};
    if (original == VALUE || original == WARN || original == BAD) return Color{156,255,181};
    return Color{83,232,120};
}

void Display::orbit_line(const std::vector<Pt>& points, bool closed, float width, Color color) {
    if (orbit_crt) {
        c_.polyline(points,closed,width+float(.45*c_.u),Color{3,35,13});
        c_.polyline(points,closed,width+float(.17*c_.u),Color{12,78,31});
    }
    c_.polyline(points,closed,width,orbit_ink(color));
}

void Display::orbit(const Rect& rect, const Snapshot& s) {
    const double u = c_.u;
    if (panel_titles) panel(c_, rect);
    if (panel_titles) c_.text("ORBIT", 2.6, LABEL, rect.x + 1.8 * u, rect.y + 1.8 * u);
    if (s.orbit_state && panel_titles) c_.text("INERTIAL", 1.8, MUTED, rect.right() - 1.8 * u, rect.y + 1.8 * u, Anchor::TopRight);
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
        c_.circle(static_cast<float>(fx), static_cast<float>(fy), static_cast<float>(std::max(3.0, atmo * scale)), (orbit_crt ? orbit_ink(ATMOSPHERE) : OrbitArt::atmosphere_color(s.body)));
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
    if (points.size() > 1) orbit_line(points, elliptic, 1, orbit_body ? Color{155,242,139} : ACCENT);
    const double shown = shown_body_radius(radius * scale, std::fabs(rp) * scale, u);
    if (orbit_body) orbit_body(fx, fy, shown);
    else orbit_art_->body(c_, s.body, fx, fy, shown, orbit_crt);
    if(orbit_body) orbit_text(s.body, 2.8, VALUE, fx, fy, Anchor::Center);
    else orbit_text(s.body, 1.8, VALUE, rect.right()-2*u, rect.y+6*u, Anchor::MidRight);

    float marker = static_cast<float>(0.8 * u);
    if (elliptic) {
        Pt ap = to_screen(a * (1 + e), PI);
        c_.circle(ap.x, ap.y, marker, orbit_ink(GOOD));
        orbit_text("Ap", 2.8, GOOD, ap.x - 1.4 * u, ap.y, Anchor::MidRight);
    }
    Pt pe = to_screen(rp, 0.0);
    c_.circle(pe.x, pe.y, marker, orbit_ink(s.periapsis < s.atmosphere_depth ? WARN : ACCENT));
    orbit_text("Pe", 2.8, VALUE, pe.x + 1.4 * u, pe.y, Anchor::MidLeft);

    double denom = 1 + e * std::cos(s.true_anomaly);
    double r_now = denom > 1e-3 ? p / denom : 0;
    if (r_now > 0) {
        Pt v = to_screen(r_now, s.true_anomaly);
        c_.circle(v.x, v.y, static_cast<float>(1.3 * u), orbit_ink(VALUE));
        c_.circle(v.x, v.y, static_cast<float>(0.6 * u), orbit_ink(BG));
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
    const double base = reach;  // the current orbit alone
    const double cap = 20 * base;  // optional content (planned orbit, target, encounters) widens the view at most 20 times
    // The post-burn orbit shares the frame and the view axes; widen the view to show it.
    std::optional<Conic> next;
    std::optional<ConicSpan> next_span;
    if (s.maneuver_orbit && s.maneuver_orbit->position.finite() && s.maneuver_orbit->velocity.finite()) {
        next = s.maneuver_orbit->conic(state);
        if (next) next_span = conic_span(*next, anomaly_of(*next, s.maneuver_orbit->position), s.maneuver_orbit->soi);
    }
    // A moon's sphere of influence ahead ends the patch there, as KSP's map view does; the patch inside it follows.
    const Encounter *current_encounter = nullptr, *planned_encounter = nullptr;
    for (const Encounter& e : s.encounters)
        if (e.center.finite() && e.position.finite() && e.velocity.finite() && e.conic() && e.span())
            (e.planned ? planned_encounter : current_encounter) = &e;
    if (!(next && next_span)) planned_encounter = nullptr;
    if (planned_encounter)
        next_span = span_until(*next, *next_span, anomaly_of(*next, s.maneuver_orbit->position), planned_encounter->entry());
    std::optional<ConicSpan> current_span;  // nothing: the whole conic
    if (current_encounter && conic)
        current_span = span_until(*conic, ConicSpan{-PI, PI, true}, anomaly_of(*conic, state.position), current_encounter->entry());
    else current_encounter = nullptr;
    std::optional<Conic> target;
    if (s.target_orbit && s.target_orbit->position.finite() && s.target_orbit->velocity.finite())
        target = s.target_orbit->conic(state);
    if (next && next_span) {
        double node = std::sqrt(dot(s.maneuver_orbit->position, s.maneuver_orbit->position));
        double far = next_span->closed && next->apoapsis() ? *next->apoapsis()
                                                            : std::max(2.5 * next->periapsis(), 3 * node);
        if (s.maneuver_orbit->soi > 0 && std::isfinite(s.maneuver_orbit->soi)) far = std::min(far, s.maneuver_orbit->soi);
        if (std::isfinite(far)) reach = std::max(reach, std::min(far, cap));
    }
    // Fit what is drawn (body and atmosphere, vessel, current conic, planned orbit, target, encounters) to the
    // drawable area, using its full width and height. The view axes stay fixed: only the scale and the offset follow.
    double margin = apsis_label_width() + 2.6 * u;  // Ap/Pe labels sit beside their markers
    Rect area = Rect::f(rect.x + margin, rect.y + 6.5 * u, rect.w - 2 * margin, rect.h - 9.3 * u);
    double x0 = -atmo, x1 = atmo, y0 = -atmo, y1 = atmo;
    auto include = [&](double x, double y, double rx, double ry) {
        if (!std::isfinite(x + y + rx + ry)) return;
        x0 = std::min(x0, x - rx), x1 = std::max(x1, x + rx), y0 = std::min(y0, y - ry), y1 = std::max(y1, y + ry);
    };
    auto include_point = [&](const Vec3& v, double r) {  // optional content only within the cap
        auto [x, y] = state.project(v);
        if (std::hypot(x, y) <= cap) include(x, y, r, r);
    };
    // Projected ellipse: centre - a e P + a cos E P + b sin E T, so each axis spans +-hypot(a P_i, b T_i).
    auto ellipse = [&](const Conic& c, double limit) {
        double e = c.eccentricity, a = c.p / (1 - e * e), b = a * std::sqrt(1 - e * e);
        if (!(a * (1 + e) <= limit)) return false;
        auto [px, py] = state.project(c.peri_direction);
        auto [tx, ty] = state.project(c.transverse);
        include(-a * e * px, -a * e * py, std::hypot(a * px, b * tx), std::hypot(a * py, b * ty));
        return true;
    };
    if (!(conic && conic->apoapsis() && ellipse(*conic, 1e300))) include(0, 0, base, base);
    if (next && next_span) {
        double limit = std::min(reach, cap);
        if (!(next_span->closed && ellipse(*next, limit))) include(0, 0, limit, limit);
    }
    if (target && !(target->apoapsis() && ellipse(*target, cap))) include_point(s.target_orbit->position, 0);
    for (const Encounter* e : {current_encounter, planned_encounter})
        if (e) include_point(e->center, std::min(e->soi, cap));
    if (s.approach) include_point(s.approach->vessel, 0), include_point(s.approach->target, 0);
    auto [vx, vy] = state.vessel_point();
    include(vx, vy, 0, 0);
    // Optional data that appears and disappears (a node, a target, an encounter) must not make the scale jump:
    // grow at once, shrink only after the content has filled less than 60 % of the view for 2 s.
    {
        ViewFit& f = view_fit_;
        const bool same = f.valid && f.body == s.body && f.mu == state.mu && f.axis_x == state.axes.x &&
                          f.axis_y == state.axes.y && f.w == area.w && f.h == area.h;
        const double t = s.received;
        auto fit_scale = [&](double a0, double a1, double b0, double b1) {
            return std::min(area.w / (a1 - a0), area.h / (b1 - b0));
        };
        const bool inside = same && x0 >= f.x0 && x1 <= f.x1 && y0 >= f.y0 && y1 <= f.y1;
        if (!same) {
            f = ViewFit{true, s.body, state.mu, state.axes.x, state.axes.y, area.w, area.h, x0, x1, y0, y1, t, false};
        } else if (!inside) {  // content left the view: grow at once to hold both
            f.x0 = std::min(f.x0, x0), f.x1 = std::max(f.x1, x1), f.y0 = std::min(f.y0, y0), f.y1 = std::max(f.y1, y1);
            f.shrinking = false;
        } else if (fit_scale(f.x0, f.x1, f.y0, f.y1) < .6 * fit_scale(x0, x1, y0, y1)) {
            if (!f.shrinking) f.shrinking = true, f.since = t;
            else if (!(t - f.since < 2.0)) f.x0 = x0, f.x1 = x1, f.y0 = y0, f.y1 = y1, f.shrinking = false;
        } else {
            f.shrinking = false;
        }
        x0 = f.x0, x1 = f.x1, y0 = f.y0, y1 = f.y1;
    }
    reach = std::max({std::fabs(x0), std::fabs(x1), std::fabs(y0), std::fabs(y1)});
    double scale = std::min(area.w / (x1 - x0), area.h / (y1 - y0));
    // Quantise the scale (0.5 % steps) so the cached dashes survive small changes of reach.
    if (scale > 0 && std::isfinite(scale)) scale = std::exp(std::round(std::log(scale) * 200) / 200);
    last_orbit_scale = scale;
    double cx = area.x + area.w / 2.0 - (x0 + x1) / 2 * scale, cy = area.y + area.h / 2.0 + (y0 + y1) / 2 * scale;
    auto point = [&](const Vec3& v) {
        auto [x, y] = state.project(v);
        return Pt{static_cast<float>(cx + x * scale), static_cast<float>(cy - y * scale)};
    };
    if (s.atmosphere_depth > 0)
        c_.circle(static_cast<float>(cx), static_cast<float>(cy), static_cast<float>(std::max(3.0, atmo * scale)), (orbit_crt ? orbit_ink(ATMOSPHERE) : OrbitArt::atmosphere_color(s.body)));
    const float orbit_width = static_cast<float>(std::max(1.5, .14 * u));  // as the dashed orbit
    // Like KSP's map view, an orbit line is brightest and widest just behind its body or vessel (the path it has
    // just travelled) and fades and thins towards where it is going (user, 2026-11-16). `here` is the object's true
    // anomaly; without one the line is drawn uniform. Drawn in 24 steps of opaque colour (no alpha blending).
    auto solid = [&](const Conic& c, double from, double to, Color color, float width, const Vec3& origin,
                     std::optional<double> here) {
        auto shade = [&](double anomaly) {
            if (!here) return 1.0;
            double behind = std::fmod(*here - anomaly, 2 * PI);
            if (behind < 0) behind += 2 * PI;
            return 1.0 - behind / (2 * PI);  // 1 just behind the object, 0 just ahead of it
        };
        std::vector<Pt> segment;
        int step = -1;
        auto flush = [&] {
            if (segment.size() > 1) {
                const double k = step < 0 ? 1.0 : (step + .5) / 24;
                orbit_line(segment, false, static_cast<float>(width * (.55 + .65 * k)), mix(BG, color, .3 + .7 * k));
            }
        };
        for (int index = 0; index <= 240; ++index) {
            const double anomaly = from + (to - from) * index / 240;
            auto vector = c.position(anomaly);
            if (vector && dot(*vector + origin, *vector + origin) <= (12 * reach) * (12 * reach)) {
                const int now = here ? std::min(23, static_cast<int>(shade(anomaly) * 24)) : -1;
                Pt p = point(*vector + origin);
                if (now != step && !segment.empty()) {
                    flush();
                    Pt last = segment.back();
                    segment.clear();
                    segment.push_back(last);  // keep the line continuous across shade steps
                }
                step = now;
                segment.push_back(p);
            } else {
                flush();
                segment.clear();
            }
        }
        flush();
    };
    if (target) {
        std::optional<double> at;
        if (s.target_orbit) at = anomaly_of(*target, s.target_orbit->position);
        solid(*target, -PI, PI, orbit_style.target, orbit_width * .85f, Vec3{}, at);
    }
    if (conic) {
        double from = current_span ? current_span->from : -PI, to = current_span ? current_span->to : PI;
        solid(*conic, from, to, orbit_body ? Color{155, 242, 139} : ACCENT, orbit_width, Vec3{},
              anomaly_of(*conic, state.position));
    }
    last_node_marker.reset();
    if (next && next_span) maneuver_orbit(s, *next, *next_span, scale, reach, cx, cy, rect, false);
    for (const Encounter* e : {current_encounter, planned_encounter})
        if (e) encounter_patch(*e, point, scale, reach, rect, false);
    const double periapsis = conic ? conic->periapsis() : std::sqrt(dot(state.position, state.position));
    const double shown = shown_body_radius(radius * scale, periapsis * scale, u);
    if (orbit_body) orbit_body(cx, cy, shown);
    else orbit_art_->body(c_, s.body, cx, cy, shown, orbit_crt);
    if(orbit_body) orbit_text(s.body, 2.8, VALUE, cx, cy, Anchor::Center);
    else orbit_text(s.body, 1.8, VALUE, rect.right()-2*u, rect.y+6*u, Anchor::MidRight);
    // At zero eccentricity the apsides have no unique direction: do not invent one.
    if (conic && conic->eccentricity > 1e-5) {
        if (auto position = conic->position(PI); position && (!current_span || span_contains(*current_span, PI))) {
            Pt ap = point(*position);
            c_.circle(ap.x, ap.y, static_cast<float>(.8 * u), orbit_ink(GOOD));
            orbit_text("Ap", 2.8, GOOD, ap.x - 1.4 * u, ap.y, Anchor::MidRight);
        }
        if (!current_span || span_contains(*current_span, 0)) {
            Pt pe = point(*conic->position(0));
            c_.circle(pe.x, pe.y, static_cast<float>(.8 * u), orbit_ink(s.periapsis < s.atmosphere_depth ? WARN : ACCENT));
            orbit_text("Pe", 2.8, VALUE, pe.x + 1.4 * u, pe.y, Anchor::MidLeft);
        }
    }
    if (next && next_span) maneuver_orbit(s, *next, *next_span, scale, reach, cx, cy, rect, true);
    last_encounter_marker.reset();
    for (const Encounter* e : {current_encounter, planned_encounter})
        if (e) encounter_patch(*e, point, scale, reach, rect, true);
    target_marks(s, point, scale, cx);
    // The white ring followed during docking: never from true anomaly, attitude, target velocity, or the changing
    // periapsis direction.
    Pt position = point(state.position);
    c_.circle(position.x, position.y, static_cast<float>(1.3 * u), orbit_ink(VALUE));
    c_.circle(position.x, position.y, static_cast<float>(.6 * u), orbit_ink(BG));
    last_vessel_marker = position;
}

void Display::encounter_patch(const Encounter& e, const std::function<Pt(const Vec3&)>& point, double scale,
                              double reach, const Rect& rect, bool markers) {
    // KSP's patched conics: the trajectory inside the moon's sphere of influence, drawn around the moon at the
    // entry time, with the SOI entry / exit icons and the encounter periapsis.
    const double u = c_.u;
    const Color ink = orbit_style.encounter;
    auto conic = e.conic();
    auto span = e.span();
    if (!conic || !span) return;
    if (!markers) {
        std::vector<Point2> track;
        const double limit = 12 * reach;
        for (int i = 0; i <= 160; ++i) {
            double anomaly = span->from + (span->to - span->from) * i / 160;
            auto v = conic->position(anomaly);
            if (!v) continue;
            Vec3 w = e.center + *v;
            if (dot(w, w) > limit * limit) continue;
            Pt p = point(w);
            track.push_back({p.x, p.y});
        }
        const float width = static_cast<float>(std::max(1.5, .14 * u));
        if (e.planned) {  // planned patches are dashed, as the post-burn orbit
            const double dash = std::max(3.0, .012 * rect.h);
            for (const auto& d : dash_polyline(track, dash, dash)) {
                std::vector<Pt> line;
                for (const Point2& p : d) line.push_back({static_cast<float>(p.first), static_cast<float>(p.second)});
                orbit_line(line, false, width, ink);
            }
        } else if (track.size() > 1) {
            std::vector<Pt> line;
            for (const Point2& p : track) line.push_back({static_cast<float>(p.first), static_cast<float>(p.second)});
            orbit_line(line, false, width, ink);
        }
        // The moon where the vessel meets it: an outline, so the live target disc stays the solid one.
        Pt c = point(e.center);
        const float r = static_cast<float>(std::max(e.radius * scale, .7 * u));
        c_.ring(c.x, c.y, r, static_cast<float>(std::max(1.0, .16 * u)), orbit_ink(ink));
        return;
    }
    Pt c = point(e.center);
    // A small ring with a notch pointing into (entry) or out of (exit) the sphere of influence.
    auto soi_icon = [&](const Vec3& where, bool entering) {
        Pt p = point(where);
        const float r = static_cast<float>(.95 * u);
        c_.circle(p.x, p.y, r, orbit_ink(BG));
        c_.ring(p.x, p.y, r, static_cast<float>(.24 * u), orbit_ink(ink));
        double dx = c.x - p.x, dy = c.y - p.y, length = std::hypot(dx, dy);
        if (!(length > 1e-3)) dx = 0, dy = 1, length = 1;
        dx /= length, dy /= length;
        if (!entering) dx = -dx, dy = -dy;
        const double tip = .55 * u, back = .35 * u, half = .38 * u;
        c_.polygon({{static_cast<float>(p.x + dx * tip), static_cast<float>(p.y + dy * tip)},
                    {static_cast<float>(p.x - dx * back - dy * half), static_cast<float>(p.y - dy * back + dx * half)},
                    {static_cast<float>(p.x - dx * back + dy * half), static_cast<float>(p.y - dy * back - dx * half)}},
                   orbit_ink(ink));
        return p;
    };
    last_encounter_marker = soi_icon(e.entry(), true);
    if (auto out = e.exit()) soi_icon(e.center + *out, false);
    if (span_contains(*span, 0)) {
        auto altitude = e.periapsis_altitude();
        Pt pe = point(e.center + *conic->position(0));
        const Color color = altitude && *altitude < 0 ? WARN : ink;
        c_.circle(pe.x, pe.y, static_cast<float>(.6 * u), orbit_ink(color));
        std::string label = "Pe " + (altitude ? fmt::distance(*altitude) : std::string("--"));
        if (pe.x >= c.x) orbit_text(label, 2.2, color, pe.x + 1.0 * u, pe.y, Anchor::MidLeft);
        else orbit_text(label, 2.2, color, pe.x - 1.0 * u, pe.y, Anchor::MidRight);
    }
}

void Display::target_marks(const Snapshot& s, const std::function<Pt(const Vec3&)>& point, double scale, double cx) {
    const double u = c_.u;
    last_target_marker.reset();
    last_approach_marker.reset();
    const Color ink = orbit_style.target;
    // KSP shows the encounter instead of the closest approach once the path enters the target's sphere.
    bool encounter = false;
    for (const Encounter& e : s.encounters)
        if (s.target_orbit && e.body == s.target_orbit->name) encounter = true;
    if (s.approach && !encounter && s.approach->vessel.finite() && s.approach->target.finite()) {
        const ClosestApproach& a = *s.approach;
        Pt v = point(a.vessel), t = point(a.target);
        // The two chevrons point at each other's orbit: the vessel's from above, the target's from below.
        auto chevron = [&](Pt p, bool down, Color color) {
            const float h = static_cast<float>(1.5 * u), w = static_cast<float>(.85 * u), gap = static_cast<float>(.25 * u);
            const float k = down ? -1.f : 1.f;
            c_.polygon({{p.x, p.y + k * gap}, {p.x - w, p.y + k * (gap + h)}, {p.x + w, p.y + k * (gap + h)}},
                       orbit_ink(BG));
            const float in = static_cast<float>(.25 * u);
            c_.polygon({{p.x, p.y + k * (gap + in)}, {p.x - w + in, p.y + k * (gap + h - in * .5f)},
                        {p.x + w - in, p.y + k * (gap + h - in * .5f)}},
                       orbit_ink(color));
        };
        if (std::hypot(v.x - t.x, v.y - t.y) > 1.5 * u) {
            std::vector<Point2> link{{v.x, v.y}, {t.x, t.y}};
            for (const auto& d : dash_polyline(link, .5 * u, .5 * u))
                orbit_line({{static_cast<float>(d.front().first), static_cast<float>(d.front().second)},
                            {static_cast<float>(d.back().first), static_cast<float>(d.back().second)}},
                           false, static_cast<float>(std::max(1.0, .1 * u)), MUTED);
        }
        chevron(v, true, a.planned ? MANEUVER : ACCENT);
        chevron(t, false, ink);
        const std::string label = fmt::distance(a.distance());
        if (v.x >= cx) orbit_text(label, 2.2, VALUE, v.x + 1.3 * u, v.y - 1.2 * u, Anchor::MidLeft);
        else orbit_text(label, 2.2, VALUE, v.x - 1.3 * u, v.y - 1.2 * u, Anchor::MidRight);
        last_approach_marker = v;
    }
    if (!s.target_orbit || !s.target_orbit->position.finite()) return;
    const TargetOrbit& target = *s.target_orbit;
    Pt p = point(target.position);
    double ring = 1.1 * u;
    if (target.body) {
        const double r = std::max(target.radius * scale, 1.0 * u);
        if (orbit_crt) c_.circle(p.x, p.y, static_cast<float>(r), orbit_ink(BODY));
        else target_art_->body(c_, target.name, p.x, p.y, r, false);
        ring = r + .55 * u;
    } else {
        c_.circle(p.x, p.y, static_cast<float>(.28 * u), orbit_ink(ink));
    }
    // The NavBall's target mark: a ring with four ticks.
    const float width = static_cast<float>(std::max(1.5, .2 * u));
    c_.ring(p.x, p.y, static_cast<float>(ring), width, orbit_ink(ink));
    for (int i = 0; i < 4; ++i) {
        const double a = i * PI / 2, dx = std::cos(a), dy = std::sin(a);
        c_.line({static_cast<float>(p.x + dx * (ring - .1 * u)), static_cast<float>(p.y + dy * (ring - .1 * u))},
                {static_cast<float>(p.x + dx * (ring + .6 * u)), static_cast<float>(p.y + dy * (ring + .6 * u))}, width,
                orbit_ink(ink));
    }
    if (p.x >= cx) orbit_text(target.name, 2.0, ink, p.x + ring + 1.0 * u, p.y + ring * .6, Anchor::MidLeft);
    else orbit_text(target.name, 2.0, ink, p.x - ring - 1.0 * u, p.y + ring * .6, Anchor::MidRight);
    last_target_marker = p;
}

void Display::maneuver_orbit(const Snapshot& s, const Conic& next, const ConicSpan& span, double scale, double reach,
                             double cx, double cy, const Rect& rect, bool markers) {
    // KSP's map view: the planned orbit is an orange dashed line with its own Ap/Pe, the current one stays solid.
    const double u = c_.u;
    const ManeuverOrbit& m = *s.maneuver_orbit;
    const OrbitState& state = *s.orbit_state;
    auto screen = [&](const Point2& p) {
        return Pt{static_cast<float>(cx + p.first * scale), static_cast<float>(cy - p.second * scale)};
    };
    auto point = [&](const Vec3& v) { return screen(state.project(v)); };
    if (!markers) {
        // Constant on-screen pattern: dash and gap in pixels, converted to metres at the (quantised) scale.
        const double dash_px = std::max(3.0, .012 * rect.h);
        ManeuverCache& k = maneuver_cache_;
        if (!k.valid || k.position != m.position || k.velocity != m.velocity || k.mu != state.mu ||
            k.axis_x != state.axes.x || k.axis_y != state.axes.y || k.soi != m.soi || k.scale != scale ||
            k.dash != dash_px || k.w != rect.w || k.h != rect.h || k.from != span.from || k.to != span.to ||
            k.closed != span.closed) {
            k = ManeuverCache{true, m.position, m.velocity, state.axes.x, state.axes.y, state.mu, m.soi, scale,
                              dash_px, rect.w, rect.h, {}, span.from, span.to, span.closed};
            ++maneuver_builds;
            const double limit = 12 * reach;  // farther points are far outside the panel: split there
            std::vector<Point2> piece;
            auto flush = [&] {
                for (auto& d : dash_polyline(piece, dash_px / scale, dash_px / scale)) k.dashes.push_back(std::move(d));
                piece.clear();
            };
            for (const Point2& p : conic_track(next, span, state.axes, 480)) {
                if (std::hypot(p.first, p.second) <= limit) piece.push_back(p);
                else flush();
            }
            flush();
        }
        const float width = static_cast<float>(std::max(1.5, .14 * u));
        std::vector<Pt> line;
        for (const auto& d : k.dashes) {
            line.clear();
            for (const Point2& p : d) line.push_back(screen(p));
            orbit_line(line, false, width, MANEUVER);
        }
        return;
    }
    // Own Ap/Pe and the node. Labels sit beside the marker, away from the body (the side margins are reserved
    // for apsis labels), so they never cover the body or the vessel.
    const float marker = static_cast<float>(.6 * u);
    auto apsis = [&](const char* label, const Vec3& where) {
        Pt p = point(where);
        c_.circle(p.x, p.y, marker, orbit_ink(MANEUVER));
        if (p.x >= cx) orbit_text(label, 2.4, MANEUVER, p.x + 1.1 * u, p.y, Anchor::MidLeft);
        else orbit_text(label, 2.4, MANEUVER, p.x - 1.1 * u, p.y, Anchor::MidRight);
    };
    if (next.eccentricity > 1e-5) {
        if (span_contains(span, PI)) {
            if (auto position = next.position(PI)) apsis("Ap", *position);
        }
        if (span_contains(span, 0)) apsis("Pe", *next.position(0));
    }
    Pt node = point(m.position);
    const float d = static_cast<float>(1.0 * u), inner = static_cast<float>(.45 * u);
    c_.polygon({{node.x, node.y - d}, {node.x + d, node.y}, {node.x, node.y + d}, {node.x - d, node.y}}, orbit_ink(MANEUVER));
    c_.polygon({{node.x, node.y - inner}, {node.x + inner, node.y}, {node.x, node.y + inner}, {node.x - inner, node.y}}, orbit_ink(BG));
    last_node_marker = node;
}

}  // namespace ksphmi
