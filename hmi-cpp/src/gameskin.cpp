#include "gameskin.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "audio.hpp"
#include "cockpit.hpp"
#include "mechjebpage.hpp"
#include "config.hpp"
#include "display.hpp"
#include "fmt.hpp"
#include "gauges.hpp"
#include "navball.hpp"
#include "resource_ui.hpp"
#include "scenes.hpp"
#include "shipreport.hpp"
#include "steampunk.hpp"

namespace ksphmi {
std::string game_skin_font(bool redux) {
    return find_app_dir() + "/assets/fonts/" + (redux ? "ChakraPetch-SemiBold.ttf" : "RobotoCondensed-SemiBold.ttf");
}
namespace {
constexpr Color WHITE{231, 235, 239}, GREEN{143, 232, 29}, CYAN{54, 221, 237}, AMBER{248, 199, 47}, RED{248, 85, 67};
std::string upper(std::string s) {
    for (char& ch : s) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}
std::string number(double v, int dp = 0) { return std::isfinite(v) ? fmt::str("%.*f", dp, v) : "--"; }
Color blend(Color a, Color b, double t) {
    return {uint8_t(a.r * (1 - t) + b.r * t), uint8_t(a.g * (1 - t) + b.g * t), uint8_t(a.b * (1 - t) + b.b * t)};
}
struct Paint {
    Canvas& c;
    bool redux;
    double u;
    Color bg, well, rail, muted, green, accent;
    Paint(Canvas& c, bool r)
        : c(c),
          redux(r),
          u(c.u),
          bg(r ? Color{10, 14, 23} : Color{41, 44, 48}),
          well(r ? Color{14, 19, 29} : Color{26, 29, 30}),
          rail(r ? Color{117, 128, 157} : Color{116, 121, 127}),
          muted(r ? Color{162, 172, 199} : Color{182, 190, 193}),
          green(r ? Color{76, 248, 53} : GREEN),
          accent(r ? Color{66, 73, 155} : Color{47, 87, 100}) {}
    void text(const std::string& s, float size, Color ink, double x, double y, Anchor anchor, double room) {
        const int width = c.text_width(c.font(size), s);
        const float fitted = size * float(std::min(1.0, std::max(1.0, room) / std::max(1, width)));
        Font& font = c.font(fitted);
        std::string value = s;
        if (c.text_width(font, value) > room) {
            const std::string suffix = "...";
            if (c.text_width(font, suffix) > room) return;
            while (!value.empty() && c.text_width(font, value + suffix) > room) {
                size_t end = value.size() - 1;
                while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xc0) == 0x80) --end;
                value.resize(end);
            }
            value += suffix;
        }
        c.text(value, fitted, ink, x, y, anchor);
    }
    void gradient(const Rect& r, Color top, Color bottom) {
        for (int y = 0; y < r.h; ++y)
            c.fill_rect(float(r.x), float(r.y + y), float(r.w), 1,
                        blend(top, bottom, double(y) / std::max(1, r.h - 1)));
    }
    void box(const Rect& r, bool inset = false) {
        const float rad = float((redux ? .4 : .7) * u);
        if (!redux) {
            c.rounded_rect(r, rad, {9, 12, 16});
            c.rounded_rect(r.inflate(-.2 * u, -.2 * u), std::max(1.0f, rad - 1), {160, 166, 173});
            c.rounded_rect(r.inflate(-.45 * u, -.45 * u), std::max(1.0f, rad - 2), {72, 77, 85});
            c.rounded_rect(r.inflate(-.75 * u, -.75 * u), std::max(1.0f, rad - 3), well);
        } else {
            c.rounded_rect(r, rad, rail);
            c.rounded_rect(r.inflate(-.22 * u, -.22 * u), std::max(1.0f, rad - 1), well);
        }
        if (!redux) {
            const Color light = inset ? Color{16, 18, 20} : Color{162, 168, 174};
            c.line({float(r.x + .4 * u), float(r.y + .25 * u)}, {float(r.right() - .4 * u), float(r.y + .25 * u)}, 1,
                   light);
            c.line({float(r.x + .3 * u), float(r.bottom() - .25 * u)},
                   {float(r.right() - .3 * u), float(r.bottom() - .25 * u)}, 1, {10, 12, 15});
        }
    }
    void group(const Rect& r, const std::string& title) {
        box(r);
        const Rect bar = Rect::f(r.x + .7 * u, r.y + .7 * u, r.w - 1.4 * u, 3.4 * u);
        gradient(bar, redux ? Color{68, 76, 164} : Color{91, 96, 104}, redux ? Color{44, 49, 116} : Color{67, 72, 79});
        if (!redux) {
            const double w = std::min(r.w * .82, std::max(12 * u, double(c.text_width(c.font(2.25f), title)) + 3 * u));
            c.polygon({{float(bar.x), float(bar.y)},
                       {float(bar.x + w), float(bar.y)},
                       {float(bar.x + w - 1.5 * u), float(bar.bottom())},
                       {float(bar.x), float(bar.bottom())}},
                      accent);
        }
        text(title, 3.0f, WHITE, r.x + 1.15 * u, r.y + 2.0 * u, Anchor::MidLeft, r.w - 2.3 * u);
    }
    void button(const Rect& r, const std::string& label, bool active, Color on = GREEN, bool filled = false) {
        c.rounded_rect(r, float(.5 * u), active ? on : rail);
        const Rect inside = r.inflate(-.22 * u, -.22 * u);
        if (redux)
            c.rounded_rect(inside, float(.3 * u), active && filled ? on : well);
        else {
            c.rounded_rect(inside, float(.3 * u), active ? on : Color{59, 64, 70});
            c.line({float(inside.x + .3 * u), float(inside.y + .3 * u)},
                   {float(inside.right() - .3 * u), float(inside.y + .3 * u)}, 1,
                   active ? Color{201, 255, 119} : Color{137, 143, 152});
        }
        text(label, std::min(3.0f, float(r.h / u * .68)), active ? (redux && !filled ? on : Color{12, 25, 11}) : muted,
             r.centerx(), r.centery(), Anchor::Center, r.w - .7 * u);
    }
    void metric(const Rect& r, const std::string& label, const std::string& value, Color ink, bool art) {
        if (art) {
            box(r, true);
            Rect title = Rect::f(r.x + .3 * u, r.y + .3 * u, r.w - .6 * u, 2.7 * u);
            gradient(title, redux ? Color{68, 76, 164} : Color{84, 89, 96},
                     redux ? Color{45, 51, 119} : Color{60, 65, 70});
            text(label, 2.7f, WHITE, r.centerx(), r.y + 1.85 * u, Anchor::Center, r.w - 1.0 * u);
        } else {
            const auto split = value.find_last_of(' ');
            if (redux && r.h >= 9.5 * u && split != std::string::npos && value.size() - split <= 5) {
                text(value.substr(0, split), 5.4f, ink, r.centerx(), r.y + 5.9 * u, Anchor::Center, r.w - 1.2 * u);
                text(value.substr(split + 1), 2.3f, CYAN, r.centerx(), r.bottom() - 1.2 * u, Anchor::Center, r.w - u);
            } else
                text(value, 4.5f, ink, r.centerx(), r.y + (r.h + 3.1 * u) / 2, Anchor::Center, r.w - 1.2 * u);
        }
    }
    void bar(const Rect& r, double fraction, Color ink) {
        c.rounded_rect(r, float(.25 * u), rail);
        c.rounded_rect(r.inflate(-.2 * u, -.2 * u), float(.2 * u), {10, 14, 18});
        Rect fill = Rect::f(r.x + .2 * u, r.y + .2 * u, std::max(0.0, r.w - .4 * u) * std::clamp(fraction, 0.0, 1.0),
                            r.h - .4 * u);
        if (fill.w > 0) {
            if (redux)
                c.rect(fill, ink);
            else
                gradient(fill, blend(ink, WHITE, .35), blend(ink, {8, 15, 8}, .2));
        }
    }
};
void resource_icon(Canvas& c, const std::string& name, double x, double y, double u) {
    if (name == "ElectricCharge") {
        c.polygon({{float(x + .5 * u), float(y - 1.2 * u)},
                   {float(x - .8 * u), float(y + .2 * u)},
                   {float(x + .05 * u), float(y + .2 * u)}},
                  AMBER);
        c.polygon({{float(x - .5 * u), float(y + 1.2 * u)},
                   {float(x + .8 * u), float(y - .2 * u)},
                   {float(x - .05 * u), float(y - .2 * u)}},
                  AMBER);
    } else if (name == "Oxidizer") {
        c.circle(float(x), float(y + .4 * u), float(.65 * u), WHITE);
        c.polygon({{float(x), float(y - 1.2 * u)},
                   {float(x - .6 * u), float(y + .3 * u)},
                   {float(x + .6 * u), float(y + .3 * u)}},
                  WHITE);
    } else if (name == "MonoPropellant") {
        for (int i = 0; i < 3; ++i)
            c.circle(float(x + (i - 1) * .7 * u), float(y + (i % 2 ? -.4 : .4) * u), float(.42 * u), WHITE);
    } else {
        c.rounded_rect(Rect::f(x - .65 * u, y - 1.0 * u, 1.3 * u, 2 * u), float(.2 * u), WHITE);
        c.rect(Rect::f(x - .4 * u, y - .65 * u, .8 * u, .5 * u), {28, 33, 38});
        c.polyline({{float(x + .65 * u), float(y - .3 * u)},
                    {float(x + 1.1 * u), float(y)},
                    {float(x + 1.1 * u), float(y + .7 * u)}},
                   false, float(.2 * u), WHITE);
    }
}
const Resource* resource(const Snapshot& s, const char* name) {
    if (s.systems)
        for (const auto& r : s.systems->resources)
            if (r.name == name) return &r;
    return nullptr;
}
const std::array<const char*, 6> resource_labels = {"ELECTRIC CHARGE", "LIQUID FUEL", "OXIDIZER",
                                                    "MONOPROPELLANT",  "SOLID FUEL",  "XENON GAS"};
std::vector<std::pair<std::string, std::string>> visible_resources(const Snapshot& s) {
    const auto names = hmi_resource_names(s), labels = hmi_resource_labels(s, resource_labels.data());
    std::vector<std::pair<std::string, std::string>> list;
    for (size_t i = 0; i < names.size(); ++i) {
        auto r = resource(s, names[i].c_str());
        if ((!s.systems || !s.systems->native_resources) && i >= 4 && (!r || r->capacity.value_or(0) <= 0)) continue;
        list.emplace_back(names[i], labels[i]);
    }
    if (list.size() > 6) {
        const auto page = page_slice(list.size(), 6, s.received);
        return {list.begin() + page.begin, list.begin() + page.end};
    }
    return list;
}
void bezel(Canvas& c, bool redux, double cx, double cy, double outer, double inner) {
    const std::string path = find_app_dir() + "/assets/" + (redux ? "ksp2" : "ksp1") + "/navball-bezel.bmp";
    SDL_Surface* surface = SDL_LoadBMP(path.c_str());
    if (!surface) throw std::runtime_error("game instrument art " + path + ": " + SDL_GetError());
    SDL_Texture* t = SDL_CreateTextureFromSurface(c.renderer(), surface);
    SDL_FreeSurface(surface);
    if (!t) throw std::runtime_error(SDL_GetError());
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_NONE);
    constexpr int count = 160;
    std::vector<SDL_Vertex> vertices;
    std::vector<int> indices;
    for (int i = 0; i <= count; ++i) {
        const double a = i * 2 * PI / count, dx = std::cos(a), dy = std::sin(a);
        for (int k = 0; k < 2; ++k) {
            const double r = k ? inner : outer, uv = k ? .40 : .485;
            vertices.push_back({{float(cx + r * dx), float(cy + r * dy)},
                                {255, 255, 255, 255},
                                {float(.5 + uv * dx), float(.5 + uv * dy)}});
        }
        if (i < count) {
            const int j = i * 2;
            for (int index : {j, j + 2, j + 1, j + 1, j + 2, j + 3}) indices.push_back(index);
        }
    }
    c.triangles(t, vertices.data(), int(vertices.size()), indices.data(), int(indices.size()));
    c.flush();
    SDL_RenderFlush(c.renderer());
    SDL_DestroyTexture(t);
}
Rect ball_rect(const Canvas& c, const Rect& r) { return Rect(r.x, r.y, r.w, r.h - int(5 * c.u)); }
std::string context(const Snapshot& s) {
    if (s.state == State::Flight) return upper(s.body) + " / " + upper(fmt::situation(s.situation));
    if (s.state == State::NoVessel && s.ship) return "VAB / " + editor_mode_label(s.ship->editor_mode);
    if (s.state == State::NoVessel)
        if (const auto* v = find_view(s.scene)) return upper(v->title);
    return s.state == State::Connecting ? "LINK SEARCH" : "NOT IN FLIGHT";
}
void mode_strip(Paint& p, const Rect& r, const std::string& mode) {
    static const char* keys[] = {"construction", "actions", "crew", "cargo"};
    static const char* labels[] = {"BUILD", "ACTIONS", "CREW", "CARGO"};
    for (int i = 0; i < 4; ++i)
        p.button(Rect::f(r.x + i * r.w / 4.0, r.y, r.w / 4.0 - .6 * p.u, r.h), labels[i], mode == keys[i], p.green);
}
struct OrbitScope {
    Display& d;
    bool titles, crt;
    decltype(Display::orbit_label) label;
    decltype(Display::orbit_body) body;
    Display::OrbitStyle style;
    explicit OrbitScope(Display& d)
        : d(d), titles(d.panel_titles), crt(d.orbit_crt), label(d.orbit_label), body(d.orbit_body), style(d.orbit_style) {}
    ~OrbitScope() {
        d.orbit_style = style;
        d.panel_titles = titles;
        d.orbit_crt = crt;
        d.orbit_label = std::move(label);
        d.orbit_body = std::move(body);
    }
};

/// The cockpit panel with the game's own boxes, title bars, buttons and bars (KSP 1 stock grey, KSP 2 indigo).
class GameCockpitPainter : public CockpitPainter {
public:
    GameCockpitPainter(Canvas& c, bool redux) : p_(c, redux) {}
    Color ink() const override { return WHITE; }
    Color dim_ink() const override { return p_.muted; }
    Color well() const override { return p_.well; }
    Color key_fill() const override { return p_.redux ? Color{30, 36, 62} : Color{59, 64, 70}; }
    Color key_ink() const override { return Color{214, 220, 228}; }
    Color unlit_ink() const override { return p_.muted; }
    void readout(Canvas& c, const Rect& r, const std::string& pattern, const std::string& text, bool art) override {
        if (art) p_.box(r, true);
        else CockpitPainter::readout(c, r, pattern, text, art);
    }
    Color lamp_color(Lamp l) const override {
        switch (l) {
            case Lamp::Green: return p_.green;
            case Lamp::White: return p_.redux ? CYAN : WHITE;
            case Lamp::Amber: return AMBER;
            case Lamp::Red: return RED;
            default: return p_.rail;
        }
    }
    void frame(Canvas&, const Rect& r, const std::string& title) override { p_.group(r, title); }
    void text(Canvas&, const std::string& s, float units, Color color, double x, double y, Anchor a, double room) override {
        p_.text(s, units, color, x, y, a, room > 0 ? room : 1e9);
    }
    void lamp(Canvas&, const Rect& r, const std::string& legend, Lamp lit, bool art) override {
        if (art) p_.button(r, legend, false);
        else if (lit != Lamp::Off) p_.button(r, legend, true, lamp_color(lit), true);
    }
    Color lit_ink() const override { return Color{12, 25, 11}; }
    void bar(Canvas& c, const Rect& r, double fraction, bool art) override {
        if (art) p_.bar(r, 0, p_.green);
        else if (fraction > 0) p_.bar(r, fraction, p_.green);
        (void)c;
    }

private:
    mutable Paint p_;
};
}  // namespace
GameSkin::GameSkin(Display& d, bool redux) : d_(d), redux_(redux), net_(d.config().krpc.address) {}
GameSkin::~GameSkin() { SDL_DestroyTexture(panel_); }
void GameSkin::build(const Snapshot& s, const std::string& key) {
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, d_.w(), d_.h(), 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) throw std::runtime_error(SDL_GetError());
    SDL_Renderer* renderer = SDL_CreateSoftwareRenderer(surface);
    if (!renderer) {
        SDL_FreeSurface(surface);
        throw std::runtime_error(SDL_GetError());
    }
    try {
        {
            Canvas art(renderer, d_.w(), d_.h(), false, game_skin_font(redux_));
            paint(art, true, s, 0);
            art.flush();
        }
        SDL_RenderPresent(renderer);
        SDL_Texture* t = SDL_CreateTextureFromSurface(d_.canvas().renderer(), surface);
        if (!t) throw std::runtime_error(SDL_GetError());
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_NONE);
        SDL_DestroyTexture(panel_);
        panel_ = t;
        key_ = key;
        ++builds;
    } catch (...) {
        SDL_DestroyRenderer(renderer);
        SDL_FreeSurface(surface);
        throw;
    }
    SDL_DestroyRenderer(renderer);
    SDL_FreeSurface(surface);
}
void GameSkin::draw(const Snapshot& s, double now) {
    Canvas& c = d_.canvas();
    TypefaceScope face(c, game_skin_font(redux_));
    std::string key =
        std::to_string(c.w) + ":" + std::to_string(c.h) + ":" + std::to_string(int(s.state)) + ":" + s.scene;
    if (s.state == State::Flight)
        for (const auto& item : visible_resources(s)) key += ":" + item.first;
    if (s.ship) key += ":" + s.ship->editor_mode + ":" + std::to_string(d_.config().vab.destinations.size());
    if (d_.cockpit_scene(s)) key = std::to_string(c.w) + ":" + std::to_string(c.h) + ":cockpit";
    if (d_.mechjeb_scene(s)) key = std::to_string(c.w) + ":" + std::to_string(c.h) + ":mechjeb:" + d_.mechjeb_key(s);
    if (!panel_ || key != key_) build(s, key);
    c.image(panel_, 0, 0, float(c.w), float(c.h));
    paint(c, false, s, now);
    c.flush();
}
void GameSkin::paint(Canvas& c, bool art, const Snapshot& s, double now) {
    if (art) c.clear(Paint(c, redux_).bg);
    header(c, art, s, now);
    if (d_.cockpit_scene(s)) {
        GameCockpitPainter painter(c, redux_);
        paint_cockpit(c, art, s, CockpitView{d_.cockpit_input(), now}, painter);
    } else if (d_.mechjeb_scene(s)) {
        GameCockpitPainter painter(c, redux_);
        paint_mechjeb(c, art, s, MechJebView{d_.mechjeb_input(), now}, painter);
    } else if (s.state == State::Flight)
        flight(c, art, s, now);
    else if (s.state == State::NoVessel && s.scene == "vab" && s.ship) {
        if (s.ship->editor_mode == "construction")
            construction(c, art, s);
        else
            editor(c, art, s);
    } else if (s.state == State::NoVessel && find_view(s.scene))
        facility(c, art, s);
    else
        waiting(c, art, s, now);
}
void GameSkin::header(Canvas& c, bool art, const Snapshot& s, double now) {
    Paint p(c, redux_);
    const double u = c.u;
    const Rect r = Rect::f(.8 * u, .5 * u, c.w - 1.6 * u, 5.4 * u);
    if (art) {
        p.box(r);
        return;
    }
    p.text(redux_ ? "KSP KONTROLLER / REDUX" : "KSP KONTROLLER / STOCK", 2.8f, WHITE, r.x + u, r.centery(),
           Anchor::MidLeft, c.w * .29);
    const bool flight = s.state == State::Flight;
    const std::string title = flight ? s.vessel : s.ship ? s.ship->name : context(s);
    p.text(upper(title), 2.4f, redux_ ? WHITE : AMBER, c.w * .34, r.centery(), Anchor::MidLeft, c.w * .28);
    if (!redux_ && flight) {
        const Rect odo = Rect::f(c.w * .64, 1.1 * u, c.w * .20, 4.1 * u);
        p.box(odo, true);
        const std::string value =
            std::isfinite(s.altitude) ? fmt::str("%07.0f", std::clamp(s.altitude, 0.0, 9999999.0)) : "-------";
        const double cw = (odo.w - 3 * u) / 7;
        for (int i = 0; i < 7; ++i) {
            const Rect cell = Rect::f(odo.x + .5 * u + i * cw, odo.y + .4 * u, cw - .2 * u, odo.h - .8 * u);
            p.gradient(cell, {219, 224, 223}, {147, 157, 162});
            p.text(value.substr(i, 1), 3.8f, {12, 17, 20}, cell.centerx(), cell.centery(), Anchor::Center,
                   cell.w - .2 * u);
            c.line({float(cell.x), float(cell.centery())}, {float(cell.right()), float(cell.centery())}, 1,
                   {95, 107, 112});
        }
        p.text("m", 2.2f, AMBER, odo.right() - u, odo.centery(), Anchor::Center, 1.7 * u);
    } else
        p.text(flight               ? upper(s.body)
               : s.ship             ? "EDITOR"
               : find_view(s.scene) ? "FACILITY"
                                    : "NO TELEMETRY",
               1.75f, p.muted, c.w * .66, r.centery(), Anchor::MidLeft, c.w * .18);
    std::string status = s.state == State::Connecting ? "SEARCHING" : stale(s, now) ? "NO DATA" : "CONNECTED";
    Color ink = s.state == State::Connecting || stale(s, now) ? AMBER : p.green;
    if (flight && !stale(s, now)) {
        const auto* power = resource(s, "ElectricCharge");
        const auto* fuel = resource(s, "LiquidFuel");
        const auto pf = power ? power->fraction() : std::nullopt;
        auto ff = fuel ? fuel->fraction() : std::nullopt;
        if (s.systems && s.systems->native_resources)
            for (const auto& r : s.systems->resources) {
                if (r.name == "ElectricCharge") continue;
                const auto fraction = r.fraction();
                if (fraction && (!ff || *fraction < *ff)) ff = fraction;
            }
        if (s.periapsis < 0) {
            status = "PE SURFACE";
            ink = RED;
        } else if (s.periapsis < s.atmosphere_depth) {
            status = "PE ATMO";
            ink = AMBER;
        } else if (pf && *pf <= gauges::WARN_BELOW) {
            status = "LOW POWER";
            ink = AMBER;
        } else if (ff && *ff <= gauges::WARN_BELOW) {
            status = "LOW FUEL";
            ink = AMBER;
        } else if (s.eccentricity >= 1) {
            status = "ESCAPE";
            ink = AMBER;
        }
    }
    p.button(Rect::f(c.w * .86, 1.0 * u, c.w * .13 - 1.2 * u, 4.3 * u), status, true, ink, true);
}
void GameSkin::flight(Canvas& c, bool art, const Snapshot& s, double) {
    Paint p(c, redux_);
    const double u = c.u, left = c.w * .48;
    const Rect metrics = Rect::f(1.1 * u, 7 * u, left - 1.1 * u, 22 * u);
    if (art && !redux_) p.group(metrics, "FLIGHT TELEMETRY");
    const double start = redux_ ? 7 * u : 11.2 * u, step = redux_ ? 10.7 * u : 8.5 * u;
    const double width = (metrics.w - 1.6 * u - 4 * .7 * u) / 5;
    struct Metric {
        const char* label;
        std::string value;
        Color ink;
    };
    const Metric data[] = {{"ALTITUDE", fmt::distance(s.altitude), p.green},
                           {"ORBIT SPEED", fmt::speed(s.orbital_speed), p.green},
                           {"APOAPSIS", s.eccentricity >= 1 ? "ESCAPE" : fmt::distance(s.apoapsis), p.green},
                           {"PERIAPSIS", fmt::distance(s.periapsis), s.periapsis < 0 ? RED : p.green},
                           {"VERTICAL", fmt::speed(s.vertical_speed), p.green},
                           {"SURF SPEED", fmt::speed(s.surface_speed), p.green},
                           {"INCLINATION", number(s.inclination, 2) + " deg", p.green},
                           {"ECCENTRICITY", number(s.eccentricity, 3), p.green},
                           {"PERIOD", s.eccentricity >= 1 ? "--" : fmt::duration(s.period), p.green},
                           {"SEMI-MAJOR", fmt::distance(s.semi_major_axis), p.green}};
    for (int i = 0; i < 10; ++i) {
        const Rect r =
            Rect::f(metrics.x + .8 * u + (i % 5) * (width + .7 * u), start + (i / 5) * step, width, step - .7 * u);
        if (!art && d_.on_tile_begin) d_.on_tile_begin(r);
        p.metric(r, data[i].label, data[i].value, redux_ && !(data[i].ink == RED) ? WHITE : data[i].ink, art);
        if (!art && d_.on_tile_end) d_.on_tile_end(r);
    }
    const Rect res = Rect::f(1.1 * u, 30 * u, left - 1.1 * u, 20 * u);
    if (art) p.group(res, "VESSEL RESOURCES");
    const auto available = visible_resources(s);
    const int count = std::max(1, int(available.size()));
    const double rh = (res.h - 4.7 * u) / count;
    if (!art && available.empty())
        p.text("NO RESOURCE DATA", 2.0f, p.muted, res.centerx(), res.centery(), Anchor::Center, res.w - 3 * u);
    for (size_t i = 0; i < available.size(); ++i) {
        auto r = resource(s, available[i].first.c_str());
        const double y = res.y + 4.2 * u + i * rh;
        const Rect bar = Rect::f(res.x + res.w * .32, y + .4 * u, res.w * .39, rh * .48);
        if (art) {
            resource_icon(c, available[i].first, res.x + 2 * u, y + rh * .5, std::min(u, rh * .28));
            p.text(available[i].second, 2.7f, WHITE, res.x + 4 * u, y + rh * .5, Anchor::MidLeft, res.w * .32 - 5 * u);
            p.box(bar, true);
        } else {
            if (d_.on_gauge) d_.on_gauge(available[i].first, bar);
            const auto fraction = r ? r->fraction() : std::nullopt;
            std::string value = "UNKNOWN";
            Color ink = p.muted;
            if (fraction) {
                ink = *fraction <= gauges::BAD_BELOW ? RED : *fraction <= gauges::WARN_BELOW ? AMBER : p.green;
                p.bar(bar, *fraction, ink);
                value = number(*r->amount) + " / " + number(*r->capacity) + "  " + number(*fraction * 100) + "%";
            } else if (r && r->capacity && *r->capacity == 0)
                value = "NOT INSTALLED";
            p.text(value, 2.4f, ink, res.right() - u, y + rh * .5, Anchor::MidRight, res.w * .26);
        }
    }
    history(c, art, Rect::f(1.1 * u, 51.5 * u, left - 1.1 * u, 23.5 * u), s);
    const Rect st = Rect::f(1.1 * u, 76.5 * u, left - 1.1 * u, 22 * u);
    if (art) {
        p.group(st, "STAGING / READ ONLY");
        const char* labels[] = {"STAGE", "PARTS", "DELTA-V (m/s)", "STATUS"};
        for (int i = 0; i < 4; ++i)
            p.text(labels[i], 1.8f, p.muted, st.x + st.w * (.10 + i * .25), st.y + 5.7 * u, Anchor::Center, st.w * .22);
    } else if (s.systems && s.systems->native_resources && s.systems->stages.empty())
        p.text("STAGE DATA UNAVAILABLE", 2.5f, AMBER, st.centerx(), st.centery(), Anchor::Center, st.w - 3 * u);
    else
        for (int i = 0; i < 4; ++i) {
            std::string values[] = {"--", "--", "--", "--"};
            bool active = false;
            if (s.systems && i < int(s.systems->stages.size())) {
                const auto& t = s.systems->stages[i];
                active = s.systems->current_stage && *s.systems->current_stage == t.number;
                values[0] = std::to_string(t.number);
                values[1] = t.parts ? std::to_string(*t.parts) : "--";
                values[2] = t.delta_v ? number(*t.delta_v) : "--";
                values[3] = active                                                             ? "ACTIVE"
                            : s.systems->current_stage && t.number > *s.systems->current_stage ? "STAGED"
                                                                                               : "PENDING";
            }
            const double y = st.y + (8.5 + i * 3.3) * u;
            c.line({float(st.x + u), float(y + 1.5 * u)}, {float(st.right() - u), float(y + 1.5 * u)}, 1, p.rail);
            for (int j = 0; j < 4; ++j)
                p.text(values[j], 2.7f, active ? p.green : p.muted, st.x + st.w * (.10 + j * .25), y, Anchor::Center,
                       st.w * .22);
        }
    const auto layout = flight_layout(c, true);
    if (art) {
        p.group(layout.orbit, "ORBIT");
        p.group(layout.vessel, "VESSEL");
        p.group(layout.navball, "NAVBALL");
    } else {
        orbit(c, layout.orbit, s);
        const Rect inside =
            Rect::f(layout.vessel.x + u, layout.vessel.y + 8 * u, layout.vessel.w - 2 * u, layout.vessel.h - 9 * u);
        const VesselStyle style{p.well, WHITE, {38, 45, 52}, p.green, p.muted, WHITE};
        d_.vessel_contents(inside, s, style);
        for (int i = 0; i < 2; ++i)
            p.button(Rect::f(layout.vessel.x + (1 + i * 5.8) * u, layout.vessel.y + 4.5 * u, 5.1 * u, 2.7 * u),
                     i ? "HEAT" : "FUEL", d_.vessel_heat() == bool(i), p.green);
    }
    const Rect ball = ball_rect(c, layout.navball);
    const auto globe = Navball::globe_geometry(c, ball, false, redux_);
    if (art) bezel(c, redux_, globe.cx, globe.cy, globe.r + 2.3 * u, globe.r + .1 * u);
    const double side = std::max(7 * u, std::min(globe.cx - globe.r - 5.5 * u - layout.navball.x,
                                                 layout.navball.right() - globe.cx - globe.r - 5.5 * u));
    if (!redux_) {
        const double radius = globe.r + 3.7 * u;
        if (art) {
            c.arc(float(globe.cx), float(globe.cy), float(radius), radians(130), radians(230), float(.7 * u),
                  {94, 103, 111});
            c.arc(float(globe.cx), float(globe.cy), float(radius), radians(-50), radians(50), float(.7 * u),
                  {94, 103, 111});
            for (int i = 0; i <= 5; ++i)
                for (int side : {-1, 1}) {
                    const double a = radians(side < 0 ? 130 + i * 20 : -50 + i * 20);
                    c.line({float(globe.cx + std::cos(a) * radius), float(globe.cy + std::sin(a) * radius)},
                           {float(globe.cx + std::cos(a) * (radius + u * .6)),
                            float(globe.cy + std::sin(a) * (radius + u * .6))},
                           1, WHITE);
                }
        } else if (s.nav) {
            if (std::isfinite(s.nav->throttle))
                c.arc(float(globe.cx), float(globe.cy), float(radius), radians(130),
                      radians(130 + 100 * std::clamp(s.nav->throttle, 0.0, 1.0)), float(.7 * u), p.green);
            if (std::isfinite(s.nav->g_force))
                c.arc(float(globe.cx), float(globe.cy), float(radius), radians(-50),
                      radians(-50 + 100 * std::clamp(s.nav->g_force / 15.0, 0.0, 1.0)), float(.7 * u),
                      s.nav->g_force >= 10  ? RED
                      : s.nav->g_force >= 5 ? AMBER
                                            : p.green);
        }
    }
    const char* labels[] = {"SPEED", "PITCH", "THROTTLE", "HEADING", "ROLL", "G-FORCE"};
    for (int i = 0; i < 6; ++i) {
        const double x = i < 3 ? layout.navball.x + 1.4 * u : layout.navball.right() - side - 1.4 * u;
        Rect r = Rect::f(x, layout.navball.y + (8.4 + (i % 3) * 10.6) * u, side, 8.5 * u);
        std::string value = "--";
        if (s.nav) {
            const auto& n = *s.nav;
            if (i == 0) value = n.speed ? number(*n.speed, 1) + " m/s" : "--";
            if (i == 1) value = number(n.pitch, 1) + " deg";
            if (i == 2) value = number(n.throttle * 100) + "%";
            if (i == 3) value = number(n.heading, 1) + " deg";
            if (i == 4) value = number(n.roll, 1) + " deg";
            if (i == 5) value = number(n.g_force, 2) + " g";
        }
        p.metric(r, labels[i], value, p.green, art);
    }
    const Rect& nav = layout.navball;
    const std::optional<double> dv =
        s.nav ? (s.nav->node_remaining ? s.nav->node_remaining : s.nav->node_dv) : std::nullopt;
    const std::string node_values[] = {dv ? number(*dv) + " m/s" : "--",
                                       s.nav && s.nav->node_time ? fmt::duration(*s.nav->node_time) : "--",
                                       s.nav && s.nav->burn_seconds ? fmt::duration(*s.nav->burn_seconds) : "--"};
    const char* node_labels[] = {"NODE DELTA-V", "TIME TO NODE", "BURN TIME"};
    for (int i = 0; i < 3; ++i) {
        const Rect r =
            Rect::f(nav.x + u + i * (nav.w - 2 * u) / 3, nav.bottom() - 11 * u, (nav.w - 2 * u) / 3 - .6 * u, 3.8 * u);
        if (art) {
            p.box(r, true);
            p.text(node_labels[i], 1.25f, p.muted, r.x + .5 * u, r.y + .7 * u, Anchor::MidLeft, r.w - u);
        } else
            p.text(node_values[i], 2.0f, p.green, r.right() - .5 * u, r.y + 2.4 * u, Anchor::MidRight, r.w - u);
    }
    if (!art) {
        for (int i = 0; i < 5; ++i) {
            const char* modes[] = {"SAS", "RCS", "ORBIT", "SURFACE", "TARGET"};
            bool on = s.nav && (i == 0   ? s.nav->sas
                                : i == 1 ? s.nav->rcs
                                         : s.nav->speed_mode == (i == 2   ? "orbit"
                                                                 : i == 3 ? "surface"
                                                                          : "target"));
            p.button(Rect::f(nav.x + u + i * (nav.w - 2 * u) / 5, nav.bottom() - 6.5 * u, (nav.w - 2 * u) / 5 - .6 * u,
                             2.6 * u),
                     modes[i], on, i >= 2 && !redux_ ? CYAN : p.green, i < 2);
        }
        const auto& modes = Navball::sas_modes();
        for (int i = 0; i < int(modes.size()); ++i)
            p.button(Rect::f(nav.x + u + i * (nav.w - 2 * u) / 10, nav.bottom() - 3.2 * u,
                             (nav.w - 2 * u) / 10 - .5 * u, 2.6 * u),
                     modes[i].second, s.nav && s.nav->sas && s.nav->sas_mode == modes[i].first, p.green);
        auto& n = d_.navball();
        const bool chrome = n.chrome;
        n.chrome = false;
        try {
            n.draw(c, ball, s.nav);
        } catch (...) {
            n.chrome = chrome;
            throw;
        }
        n.chrome = chrome;
        if (!s.nav) p.text("ATTITUDE UNAVAILABLE", 2.7f, AMBER, globe.cx, globe.cy, Anchor::Center, globe.r * 1.7);
    }
}
void GameSkin::orbit(Canvas& c, const Rect& r, const Snapshot& s) {
    Paint p(c, redux_);
    OrbitScope scope(d_);
    d_.panel_titles = false;
    d_.orbit_crt = false;
    // The map view's colours: a violet target orbit and mark, encounter patches in warm yellow.
    d_.orbit_style = Display::OrbitStyle{Color{186, 104, 255}, Color{255, 200, 72}};
    d_.orbit_body = [&](double x, double y, double radius) { orbit_art_.body(c, s.body, x, y, radius, false); };
    d_.orbit_label = [&](const std::string& value, float size, Color ink, double x, double y, Anchor a) {
        if (value == s.body) return Rect();
        return c.text(value, std::min(2.1f, size), ink, x, y, a);
    };
    d_.orbit(Rect::f(r.x + .5 * c.u, r.y, r.w - c.u, r.h - 1.0 * c.u), s);
    p.text(s.body, 1.7f, redux_ ? CYAN : AMBER, r.right() - c.u, r.y + 5.3 * c.u, Anchor::MidRight, r.w * .7);
}
void GameSkin::history(Canvas& c, bool art, const Rect& r, const Snapshot& s) {
    Paint p(c, redux_);
    const double u = c.u;
    const Rect plot = Rect::f(r.x + 3 * u, r.y + 6 * u, r.w - 5 * u, r.h - 9 * u);
    if (art) {
        p.group(r, "ALTITUDE / SPEED HISTORY");
        p.text("LAST 180 s", 1.45f, p.muted, r.right() - u, r.y + 4.6 * u, Anchor::MidRight, r.w * .3);
        for (int i = 0; i <= 4; ++i) {
            const float y = float(plot.y + plot.h * i / 4);
            c.line({float(plot.x), y}, {float(plot.right()), y}, 1, p.rail);
        }
        p.text("-180 s", 1.35f, p.muted, plot.x, plot.bottom() + u, Anchor::TopLeft, plot.w * .3);
        p.text("NOW", 1.35f, p.muted, plot.right(), plot.bottom() + u, Anchor::TopRight, plot.w * .3);
        return;
    }
    if (!std::isfinite(s.ut) || !std::isfinite(s.altitude) || !std::isfinite(s.orbital_speed)) {
        p.text("HISTORY DATA UNAVAILABLE", 1.9f, p.muted, plot.centerx(), plot.centery(), Anchor::Center, plot.w);
        return;
    }
    if (history_body_ != s.body || history_vessel_ != s.vessel || (!history_.empty() && s.ut < history_.back().ut))
        history_.clear();
    history_body_ = s.body;
    history_vessel_ = s.vessel;
    if (std::isfinite(s.ut) && std::isfinite(s.altitude) && std::isfinite(s.orbital_speed) &&
        (history_.empty() || s.ut > history_.back().ut + .2))
        history_.push_back({s.ut, s.altitude, s.orbital_speed});
    while (history_.size() > 1 && (history_.front().ut < s.ut - 180 || history_.size() > 1800))
        history_.erase(history_.begin());
    if (history_.size() < 2) {
        p.text("COLLECTING FLIGHT DATA", 1.9f, p.muted, plot.centerx(), plot.centery(), Anchor::Center, plot.w);
        return;
    }
    const Rect keep = c.clip();
    c.set_clip(keep.clip(plot));
    for (int series = 0; series < 2; ++series) {
        double hi = 1;
        for (const auto& a : history_) hi = std::max(hi, series ? a.speed : a.altitude);
        std::vector<Pt> points;
        for (const auto& a : history_)
            points.push_back({float(plot.right() - (s.ut - a.ut) / 180 * plot.w),
                              float(plot.bottom() - std::max(0.0, series ? a.speed : a.altitude) / hi * plot.h * .95)});
        c.polyline(points, false, float(.18 * u), series ? (redux_ ? p.green : AMBER) : CYAN);
    }
    c.set_clip(keep);
}
void GameSkin::waiting(Canvas& c, bool art, const Snapshot& s, double now) {
    Paint p(c, redux_);
    const double u = c.u;
    const Rect outer = Rect::f(2 * u, 10 * u, c.w - 4 * u, 76 * u);
    const Rect left = Rect::f(3 * u, 17 * u, c.w * .21 - 3 * u, 60 * u);
    const Rect middle = Rect::f(c.w * .23, 17 * u, c.w * .42, 60 * u);
    const Rect right = Rect::f(c.w * .67, 17 * u, c.w * .30 - 2 * u, 60 * u);
    const double cx = middle.centerx(), cy = middle.y + middle.h * .51, radius = std::min(middle.w * .39, 20 * u);
    if (art) {
        p.group(outer, "TELEMETRY LINK");
        p.group(left, "LINK STATUS");
        p.group(middle, "LINK SEARCH");
        p.group(right, "CONNECTION");
        c.circle(float(cx), float(cy), float(radius), p.well);
        bezel(c, redux_, cx, cy, radius + 1.5 * u, radius);
        c.ring(float(cx), float(cy), float(12 * u), 1, p.rail);
        c.line({float(cx - radius + u), float(cy)}, {float(cx + radius - u), float(cy)}, 1, p.rail);
        c.line({float(cx), float(cy - radius + u)}, {float(cx), float(cy + radius - u)}, 1, p.rail);
        p.text("LINK SEARCH / ILLUSTRATION", 1.6f, p.muted, cx, cy + 15 * u, Anchor::Center, middle.w - 3 * u);
        // Neutral connection glyphs; status and endpoint remain live data.
        c.ring(float(left.centerx()), float(left.y + 16 * u), float(4 * u), float(.25 * u), WHITE);
        c.line({float(left.centerx() - 2 * u), float(left.y + 18 * u)},
               {float(left.centerx() + 2 * u), float(left.y + 14 * u)}, float(.6 * u), WHITE);
    } else {
        orbit_art_.body(c, "Kerbin", cx, cy, 8 * u, false);
        const double ang = waiting_sweep_angle(redux_ ? "ksp2" : "ksp1", now);  // the radar ping follows it
        c.circle(float(cx + 12 * u * std::cos(ang)), float(cy + 12 * u * std::sin(ang)), float(.5 * u),
                 s.state == State::Connecting ? AMBER : p.green);
        p.text(s.state == State::Connecting ? "WAITING FOR GAME" : "NOT IN FLIGHT", 3.8f,
               s.state == State::Connecting ? AMBER : p.green, left.centerx(), left.y + 29 * u, Anchor::Center,
               left.w - 3 * u);
        p.text("NO FLIGHT DATA", 2.5f, p.muted, left.centerx(), left.y + 42 * u, Anchor::Center, left.w - 3 * u);
        p.text(s.detail, 2.0f, p.muted, outer.centerx(), 81 * u, Anchor::Center, outer.w - 6 * u);
        p.text(net_.line(), 2.0f, p.muted, c.w / 2.0, 94 * u, Anchor::Center, c.w - 5 * u);
    }
    const std::string endpoint = s.server.empty() ? d_.config().krpc.address : s.server;
    const char* labels[] = {"STATUS", "GAME PC", "SOURCE"};
    const std::string values[] = {s.state == State::Connecting ? "SEARCHING" : "CONNECTED", endpoint,
                                  upper(d_.config().source)};
    for (int i = 0; i < 3; ++i)
        p.metric(Rect::f(right.x + u, right.y + (7 + i * 16) * u, right.w - 2 * u, 12 * u), labels[i], values[i],
                 i == 0 ? (s.state == State::Connecting ? AMBER : p.green) : WHITE, art);
}
void GameSkin::facility(Canvas& c, bool art, const Snapshot& s) {
    Paint p(c, redux_);
    const double u = c.u;
    const auto* view = find_view(s.scene);
    if (!view) return;
    if (art) {
        p.text(upper(view->title), 4.5f, WHITE, 3 * u, 12 * u, Anchor::TopLeft, c.w - 6 * u);
        for (int i = 0; i < 3; ++i) {
            const Rect r = Rect::f(3 * u, (24 + i * 21) * u, c.w - 6 * u, 18 * u);
            p.group(r, upper(view->cards[i].label));
            p.text(std::to_string(i + 1), 4.0f, p.green, r.x + 3 * u, r.y + 9 * u, Anchor::Center, 5 * u);
            p.text(view->cards[i].heading, 3.4f, WHITE, r.x + 8 * u, r.y + 6.5 * u, Anchor::TopLeft, r.w - 11 * u);
            p.text(view->cards[i].hint, 2.5f, p.muted, r.x + 8 * u, r.y + 12 * u, Anchor::TopLeft, r.w - 11 * u);
        }
        p.text("SCENE CONTEXT LIVE / PREPARATION CHECKLIST", 2.0f, p.muted, 3 * u, 92 * u, Anchor::TopLeft,
               c.w - 6 * u);
    }
}
void GameSkin::editor(Canvas& c, bool art, const Snapshot& s) {
    if (!s.ship) return;
    Paint p(c, redux_);
    const double u = c.u;
    const auto page = editor_page(*s.ship);
    const double gap = 1.2 * u, width = (c.w - 5 * u) / 4;
    for (int i = 0; i < 4 && i < int(page.metrics.size()); ++i)
        p.metric(Rect::f(2 * u + i * (width + gap * .333), 9 * u, width, 9 * u), page.metrics[i].first,
                 page.metrics[i].second, p.green, art);
    const double pw = (c.w - 5 * u) / 2;
    auto table = [&](const std::vector<EditorRow>& rows, const std::string& title, int col) {
        const Rect r = Rect::f(2 * u + col * (pw + u), 20 * u, pw, 69 * u);
        if (art) {
            p.group(r, title);
            return;
        }
        if (d_.on_vab_panel) d_.on_vab_panel(title, r);
        const auto slice = page_slice(rows.size(), 8, s.received);
        for (size_t i = slice.begin; i < slice.end; ++i) {
            const auto& row = rows[i];
            const double y = r.y + (6 + (i - slice.begin) * 7.1) * u;
            const bool selected =
                s.ship->editor_mode == "actions" && col == 0 && row.label == destination_label(s.ship->selected_group);
            if (selected)
                c.rounded_rect(Rect::f(r.x + .7 * u, y - .5 * u, r.w - 1.4 * u, 6.5 * u), float(.3 * u),
                               redux_ ? Color{25, 64, 37} : Color{35, 75, 96});
            c.line({float(r.x + u), float(y + 6.2 * u)}, {float(r.right() - u), float(y + 6.2 * u)}, 1, p.rail);
            const Color ink = row.warning ? RED : WHITE;
            p.text(row.label, 2.3f, ink, r.x + 1.3 * u, y, Anchor::TopLeft, r.w * .57);
            p.text(row.value, 2.2f,
                   row.warning            ? RED
                   : row.value == "EMPTY" ? p.muted
                                          : p.green,
                   r.right() - 1.3 * u, y, Anchor::TopRight, r.w * .38);
            p.text(row.detail, 1.7f, p.muted, r.x + 1.3 * u, y + 3.1 * u, Anchor::TopLeft, r.w - 2.6 * u);
        }
        p.text(fmt::str("PAGE %zu / %zu", slice.page + 1, slice.pages), 1.5f, p.muted, r.x + u, r.bottom() - 2.5 * u,
               Anchor::TopLeft, r.w - 2 * u);
    };
    table(page.left, page.left_title, 0);
    table(page.right, page.right_title, 1);
    if (!art) mode_strip(p, Rect::f(2 * u, 92 * u, c.w - 4 * u, 5 * u), s.ship->editor_mode);
}
void GameSkin::construction(Canvas& c, bool art, const Snapshot& s) {
    if (!s.ship) return;
    Paint p(c, redux_);
    const double u = c.u;
    const auto& report = *s.ship;
    const auto mission = mission_delta_v(report, d_.config().vab.atmosphere_dv);
    const auto target = d_.vab_target(&report);
    const bool pending = report.delta_v_pending;
    const auto* dest = target.destination;
    const double margin = dest ? mission.total - dest->delta_v : 0;
    const char* labels[] = {"TOTAL DELTA-V", "MISSION DELTA-V", "TARGET", "MARGIN"};
    const std::string values[] = {pending ? "PENDING" : number(report.total_delta_v()) + " m/s",
                                  pending ? "PENDING" : number(mission.total) + " m/s",
                                  dest ? number(dest->delta_v) + " m/s" : "--",
                                  pending || !dest ? "--" : number(margin) + " m/s"};
    for (int i = 0; i < 4; ++i) {
        const Rect r = Rect::f(2 * u + i * (c.w - 4 * u) / 4, 9 * u, (c.w - 4 * u) / 4 - u, 9 * u);
        p.metric(r, labels[i], values[i], pending ? AMBER : i == 3 && margin < 0 ? RED : p.green, art);
        if (!art && d_.on_vab_panel) d_.on_vab_panel(labels[i], r);
    }
    const double split = c.w * .53;
    const Rect stages = Rect::f(2 * u, 20 * u, split - 3 * u, 69 * u),
               destinations = Rect::f(split, 20 * u, c.w - split - 2 * u, 31 * u),
               vehicle = Rect::f(split, 53 * u, c.w - split - 2 * u, 36 * u);
    if (art) {
        p.group(stages, "STAGES / MISSION DELTA-V");
        p.group(destinations, "DESTINATIONS");
        p.group(vehicle, "VEHICLE");
    } else {
        if (d_.on_vab_panel) {
            d_.on_vab_panel("stages", stages);
            d_.on_vab_panel("destinations", destinations);
            d_.on_vab_panel("vehicle", vehicle);
        }
        if (pending)
            p.text(report.empty_craft() ? "EMPTY CRAFT" : "DELTA-V COMPUTING", 2.5f, AMBER, stages.centerx(),
                   stages.y + 10 * u, Anchor::Center, stages.w - 3 * u);
        else {
            // Rows share the panel's height: as many stages as fit at 5u or more each (a 13.5u row showed 4 of an
            // 11-stage ship per page with most of the panel empty; user, 2026-11-16). Pages only when even that
            // is too few. Text shrinks with the row so number and part lines never overlap.
            const double top = 8.5 * u, bottom = stages.h - 4 * u, room = bottom - top;
            const size_t fit = std::max<size_t>(1, static_cast<size_t>(room / (5.0 * u)));
            const size_t count = report.stages.size();
            const auto slice = page_slice(count, std::min(count ? count : 1, fit), s.received);
            const size_t shown = std::max<size_t>(1, std::min(count, fit));  // same row height on every page
            const double pitch = std::min(13.5 * u, room / static_cast<double>(shown));
            const float big = static_cast<float>(std::min(2.7, .48 * pitch / u));
            const float small = static_cast<float>(std::min(1.8, .32 * pitch / u));
            const char* cols[] = {"STAGE", "DELTA-V", "TWR", "BURN"};
            for (int i = 0; i < 4; ++i)
                p.text(cols[i], 1.6f, p.muted, stages.x + stages.w * (.11 + i * .25), stages.y + 5 * u, Anchor::Center,
                       stages.w * .22);
            for (size_t i = slice.begin; i < slice.end; ++i) {
                const auto& st = report.stages[i];
                const double y = stages.y + top + (i - slice.begin) * pitch + big * u * .5;
                const Color twr = twr_level(report, i, d_.config().vab.twr) == TwrLevel::Bad    ? RED
                                  : twr_level(report, i, d_.config().vab.twr) == TwrLevel::Warn ? AMBER
                                                                                                : p.green;
                const std::string cells[] = {std::to_string(st.stage), number(st.delta_v), number(st.twr, 2),
                                             number(st.burn) + " s"};
                for (int j = 0; j < 4; ++j)
                    p.text(cells[j], big, j == 2 ? twr : p.green, stages.x + stages.w * (.11 + j * .25), y,
                           Anchor::Center, stages.w * .22);
                const auto lines = stage_detail_lines(st);
                if (!lines.empty())
                    p.text(lines.front(), small, p.muted, stages.x + u, y + big * u * .62, Anchor::TopLeft, stages.w - 2 * u);
            }
            if (slice.pages > 1)
                p.text(fmt::str("STAGES PAGE %zu / %zu", slice.page + 1, slice.pages), 1.5f, p.muted, stages.x + u,
                       stages.bottom() - 2.5 * u, Anchor::TopLeft, stages.w - 2 * u);
        }
        const auto& ds = d_.config().vab.destinations;
        const double cellw = (destinations.w - 2 * u) / 3;
        for (size_t i = 0; i < ds.size(); ++i) {
            const auto& d = ds[i];
            const Rect r = Rect::f(destinations.x + u + (i % 3) * cellw, destinations.y + (5 + (i / 3) * 5.5) * u,
                                   cellw - .7 * u, 4.4 * u);
            p.button(r, destination_label(d.key) + " " + number(d.delta_v), !pending,
                     pending                      ? p.muted
                     : mission.total >= d.delta_v ? p.green
                                                  : RED);
            if (dest && dest->key == d.key) {
                const Rect outline = r.inflate(.35 * u, .35 * u);
                c.polyline({{float(outline.x), float(outline.y)},
                            {float(outline.right()), float(outline.y)},
                            {float(outline.right()), float(outline.bottom())},
                            {float(outline.x), float(outline.bottom())}},
                           true, 1, WHITE);
            }
        }
        auto v = [&](const std::optional<double>& x) { return x ? number(*x, 1) : "--"; };
        const std::pair<const char*, std::string> fields[] = {
            {"MASS", v(report.mass) + " t"},
            {"DRY MASS", v(report.dry) + " t"},
            {"PARTS", report.parts ? std::to_string(*report.parts) : "--"},
            {"CREW / SEATS", (report.crew ? std::to_string(*report.crew) : "--") + " / " +
                                 (report.seats ? std::to_string(*report.seats) : "--")},
            {"COST", v(report.cost)},
            {"FUNDS", v(report.funds)}};
        for (int i = 0; i < 6; ++i) {
            const double x = vehicle.x + u + (i % 2) * (vehicle.w - 2 * u) / 2, y = vehicle.y + (6 + (i / 2) * 9) * u;
            p.text(fields[i].first, 1.7f, p.muted, x, y, Anchor::TopLeft, vehicle.w * .45);
            p.text(fields[i].second, 3.1f, p.green, x, y + 2.8 * u, Anchor::TopLeft, vehicle.w * .45);
        }
        p.text(mission_basis(report, mission), 1.35f, p.muted, 2 * u, 90 * u, Anchor::TopLeft, c.w - 4 * u);
        mode_strip(p, Rect::f(2 * u, 93 * u, c.w - 4 * u, 4.5 * u), "construction");
    }
}
}  // namespace ksphmi
