#include "flightdeck.hpp"
#include "apollo.hpp"
#include "config.hpp"
#include "display.hpp"
#include "fmt.hpp"
#include "gauges.hpp"
#include "navball.hpp"
#include "segments.hpp"
#include "steampunk.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace ksphmi {
std::string flight_deck_font() {
    static const std::string path = find_app_dir() + "/assets/fonts/BarlowCondensed-SemiBold.ttf";
    return path;
}
namespace {
constexpr Color BLACK{13, 16, 16}, WHITE{235, 236, 224}, INK{22, 24, 23}, GREEN{155, 242, 139}, DIM{119, 143, 117};
constexpr Color AMBER{246, 190, 62}, RED{246, 78, 58}, GRID{40, 51, 43};
SDL_Texture* load(Canvas& c, const char* name) {
    const std::string path = find_app_dir() + "/assets/apollo/" + name;
    SDL_Surface* s = SDL_LoadBMP(path.c_str());
    if (!s) throw std::runtime_error("Apollo asset " + path + ": " + SDL_GetError());
    SDL_Texture* t = SDL_CreateTextureFromSurface(c.renderer(), s);
    SDL_FreeSurface(s);
    if (!t) throw std::runtime_error(SDL_GetError());
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_NONE);
    return t;
}
void sliced(Canvas& c, SDL_Texture* t, const Rect& r, double corner) {
    int w, h;
    SDL_QueryTexture(t, nullptr, nullptr, &w, &h);
    const int a = int(w * .125), b = int(h * .125);
    const int d = std::max(1, int(std::min({corner, r.w / 2.0, r.h / 2.0})));
    const int sx[] = {0, a, w - a, w}, sy[] = {0, b, h - b, h};
    const int dx[] = {r.x, r.x + d, r.right() - d, r.right()}, dy[] = {r.y, r.y + d, r.bottom() - d, r.bottom()};
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x)
            c.blit(t, Rect(sx[x], sy[y], sx[x + 1] - sx[x], sy[y + 1] - sy[y]),
                   Rect(dx[x], dy[y], dx[x + 1] - dx[x], dy[y + 1] - dy[y]));
}
struct Art {
    Canvas& c;
    SDL_Texture *panel = nullptr, *module = nullptr, *ring = nullptr;
    explicit Art(Canvas& c) : c(c) {
        try {
            panel = load(c, "panel.bmp");
            module = load(c, "module.bmp");
            ring = load(c, "navball-bezel.bmp");
        } catch (...) {
            SDL_DestroyTexture(panel);
            SDL_DestroyTexture(module);
            SDL_DestroyTexture(ring);
            throw;
        }
    }
    Art(const Art&) = delete;
    Art& operator=(const Art&) = delete;
    ~Art() {
        c.flush();
        SDL_DestroyTexture(panel);
        SDL_DestroyTexture(module);
        SDL_DestroyTexture(ring);
    }
};
void text(Canvas& c, const std::string& s, float units, Color ink, double x, double y, Anchor anchor, double room) {
    const int width = c.text_width(c.font(units), s);
    units *= float(std::min(1.0, room / std::max(1, width)));
    c.text(s, units, ink, x, y, anchor);
}
void group(Canvas& c, Art& a, const Rect& r, const char* title, bool dark = false) {
    sliced(c, dark ? a.module : a.panel, r, 2.4 * c.u);
    text(c, title, 3.2f, dark ? WHITE : INK, r.x + 2 * c.u, r.y + 1.8 * c.u, Anchor::MidLeft, r.w - 4 * c.u);
}
void glass(Canvas& c, const Rect& r) {
    c.rounded_rect(r.inflate(.25 * c.u, .25 * c.u), float(.5 * c.u), Color{75, 79, 73});
    c.rounded_rect(r, float(.3 * c.u), BLACK);
    c.fill_rect(float(r.x + .4 * c.u), float(r.y + .18 * c.u), float(r.w - .8 * c.u), float(.12 * c.u),
                Color{34, 47, 37});
}
std::string capitals(std::string s) {
    for (char& ch : s) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}
std::string number(double x, int decimals = 0) { return std::isfinite(x) ? fmt::str("%.*f", decimals, x) : "--"; }
void value(Canvas& c, const Rect& r, const std::string& s, const std::string& unit = "", Color color = GREEN) {
    const double u = c.u, unit_room = unit.empty() ? 0 : std::min(3.2 * u, r.w * .25);
    const double room = r.w - .8 * u - unit_room;
    std::string pattern;
    bool numeric = true;
    for (char ch : s) {
        if (ch == '.') continue;
        if (ch == ':')
            pattern += ':';
        else if (ch == ' ')
            pattern += ' ';
        else {
            pattern += '8';
            if (!(ch >= '0' && ch <= '9') && ch != '-' && ch != '+') numeric = false;
        }
    }
    if (pattern.empty()) pattern = "88";
    seg::Layout probe = seg::layout(pattern, 100);
    const float height = float(std::max(3.0, std::min((r.h - .7 * u) / 1.5, room * 100 / probe.w)));
    if (numeric) {
        seg::Layout l = seg::layout(pattern, height);
        seg::draw(c, float(r.x + .3 * u + (room - l.w) / 2), float(r.centery() - l.h / 2), l, s, color);
    } else
        text(c, s, 3, color, r.x + room / 2, r.centery(), Anchor::Center, room);
    if (!unit.empty()) text(c, unit, 1.9f, color, r.right() - .4 * u, r.centery(), Anchor::MidRight, unit_room);
}
void annulus(Canvas& c, SDL_Texture* t, double cx, double cy, double outer, double inner) {
    std::vector<SDL_Vertex> vertices;
    std::vector<int> indices;
    constexpr int n = 160;
    for (int i = 0; i <= n; ++i) {
        const double a = 2 * PI * i / n, x = std::cos(a), y = std::sin(a);
        for (int k = 0; k < 2; ++k) {
            const double r = k ? inner : outer, uv = k ? .347 : .486;
            vertices.push_back({{float(cx + r * x), float(cy + r * y)},
                                {255, 255, 255, 255},
                                {float(.5 + uv * x), float(.5 + uv * y)}});
        }
        if (i < n) {
            const int j = 2 * i;
            for (int v : {j, j + 2, j + 1, j + 1, j + 2, j + 3}) indices.push_back(v);
        }
    }
    c.triangles(t, vertices.data(), int(vertices.size()), indices.data(), int(indices.size()));
}
const Resource* resource(const Snapshot& s, const char* name) {
    if (s.systems)
        for (const auto& r : s.systems->resources)
            if (r.name == name) return &r;
    return nullptr;
}
void annunciator(Canvas& c, const Rect& r, const char* label, bool art, bool on, Color lit = GREEN) {
    if (art) glass(c, r);
    if (art || on) {
        const Rect face = r.inflate(-.5 * c.u, -.5 * c.u);
        if (on) {
            c.rounded_rect(face, float(.25 * c.u), lit);
            c.fill_rect(float(face.x + .2 * c.u), float(face.y + .2 * c.u), float(face.w - .4 * c.u), float(.25 * c.u),
                        WHITE);
        }
        text(c, label, 1.8f, on ? INK : DIM, r.centerx(), r.centery(), Anchor::Center, r.w - 1.2 * c.u);
    }
}
}  // namespace
FlightDeck::~FlightDeck() = default;

void FlightDeck::draw(Canvas& c, bool art, const Snapshot& s, double now) {
    TypefaceScope font(c, flight_deck_font());
    const double u = c.u, w = c.w, left = w * .5 - 1.5 * u;
    std::unique_ptr<Art> assets;
    if (art) {
        assets = std::make_unique<Art>(c);
        sliced(c, assets->panel, Rect(0, 0, c.w, c.h), 2 * u);
    }
    // Slim continuous flight-deck header.
    const Rect header = Rect::f(.5 * u, .5 * u, w - u, 5.4 * u);
    if (art) {
        sliced(c, assets->panel, header, 1.5 * u);
        text(c, "APOLLO FLIGHT DECK", 3.6f, INK, 2 * u, 3 * u, Anchor::MidLeft, w * .19 - 3 * u);
        glass(c, Rect::f(w * .20, 1.3 * u, w * .23, 3.5 * u));
        glass(c, Rect::f(w * .44, 1.3 * u, w * .17, 3.5 * u));
        glass(c, Rect::f(w * .62, 1.3 * u, w * .16, 3.5 * u));
    } else {
        text(c, capitals(s.vessel), 2.6f, GREEN, w * .315, 3 * u, Anchor::Center, w * .23 - 2 * u);
        text(c, capitals(s.body + " / " + fmt::situation(s.situation)), 2.4f, GREEN, w * .525, 3 * u, Anchor::Center,
             w * .17 - 2 * u);
        text(c, fmt::clock(s.ut, d_.config().format.day_hours), 2.4f, GREEN, w * .70, 3 * u, Anchor::Center,
             w * .16 - 2 * u);
    }
    const char* lamps[] = {"UPLINK", "SAS", "RCS", "LOW FUEL"};
    auto fuel = resource(s, "LiquidFuel");
    const auto f = fuel ? fuel->fraction() : std::nullopt;
    const bool lit[] = {!stale(s, now), s.nav && s.nav->sas, s.nav && s.nav->rcs, f && *f <= gauges::WARN_BELOW};
    const bool low_power = resource(s, "ElectricCharge") && resource(s, "ElectricCharge")->fraction() &&
                           *resource(s, "ElectricCharge")->fraction() <= gauges::WARN_BELOW;
    const bool pe_atmo = s.periapsis >= 0 && s.periapsis < s.atmosphere_depth;
    const bool pe_surf = s.periapsis < 0, escape = s.eccentricity >= 1;
    const char* caution = stale(s, now) ? "NO DATA"
                          : pe_surf     ? "PE SURF"
                          : pe_atmo     ? "PE ATMO"
                          : low_power   ? "LOW PWR"
                          : escape      ? "ESCAPE"
                                        : "LOW FUEL";
    for (int i = 0; i < 4; ++i) {
        const Rect r = Rect::f(w * .79 + i * (w * .205 / 4), 1.2 * u, w * .205 / 4 - .55 * u, 3.7 * u);
        annunciator(
            c, r, i == 3 && !art ? caution : lamps[i], art,
            art ? false : (i == 3 ? (lit[i] || pe_atmo || pe_surf || low_power || escape || stale(s, now)) : lit[i]),
            i == 3 ? AMBER : GREEN);
        if (!art && on_lamp) {
            const Color color = i == 3 ? AMBER : GREEN;
            on_lamp(lamps[i], lit[i] ? &color : nullptr);
        }
    }
    if (!art && on_lamp) {
        for (const auto& entry : {std::pair<const char*, bool>{"PE ATMO", pe_atmo},
                                  {"PE SURF", pe_surf},
                                  {"ESCAPE", escape},
                                  {"LOW PWR", low_power},
                                  {"NO DATA", stale(s, now)},
                                  {"NO LINK", false}})
            on_lamp(entry.first, entry.second ? &AMBER : nullptr);
    }
    const double gap = .8 * u, tw = (left - 1.4 * u - 4 * gap) / 5, th = 12.4 * u;
    struct Metric {
        const char* label;
        std::string digits, unit;
    };
    const Metric metrics[] = {
        {"ALTITUDE", number(s.altitude), "m"},
        {"ORBIT SPEED", number(s.orbital_speed, 1), "m/s"},
        {"APOAPSIS", s.eccentricity >= 1 ? "ESCAPE" : number(s.apoapsis), "m"},
        {"PERIAPSIS", number(s.periapsis), "m"},
        {"VERTICAL", number(s.vertical_speed, 1), "m/s"},
        {"SURF SPEED", number(s.surface_speed, 1), "m/s"},
        {"INCLINATION", number(s.inclination, 2), "deg"},
        {"ECCENTRICITY", number(s.eccentricity, 4), ""},
        {"PERIOD", s.eccentricity >= 1 ? "--" : fmt::duration(s.period, d_.config().format.day_hours), ""},
        {"SEMI-MAJOR", number(s.semi_major_axis), "m"}};
    for (int i = 0; i < 10; ++i) {
        const Rect r = Rect::f(1.4 * u + (i % 5) * (tw + gap), 7 * u + (i / 5) * (th + gap), tw, th);
        const Rect read = Rect::f(r.x + .8 * u, r.y + 6 * u, r.w - 1.6 * u, 5 * u);
        if (art) {
            sliced(c, assets->module, r, 1.7 * u);
            text(c, metrics[i].label, 3.0f, WHITE, r.centerx(), r.y + 2.6 * u, Anchor::Center, r.w - 2 * u);
            glass(c, read);
        } else {
            if (d_.on_tile_begin) d_.on_tile_begin(r);
            value(c, read, metrics[i].digits, metrics[i].unit, i == 3 && s.periapsis < 0 ? RED : GREEN);
            if (d_.on_tile_end) d_.on_tile_end(r);
        }
    }
    const Rect res = Rect::f(1.4 * u, 34 * u, left - 1.4 * u, 22 * u);
    if (art) group(c, *assets, res, "RESOURCES");
    const char* names[] = {"LiquidFuel", "Oxidizer", "ElectricCharge", "MonoPropellant", "SolidFuel", "XenonGas"};
    const char* labels[] = {"LIQUID FUEL", "OXIDIZER", "ELECTRIC CHARGE", "MONOPROPELLANT", "SOLID FUEL", "XENON GAS"};
    int count = 4;
    for (int i = 4; i < 6; ++i) {
        auto r = resource(s, names[i]);
        if (r && r->capacity.value_or(0) > 0) ++count;
    }
    const double row = (res.h - 5 * u) / count;
    for (int i = 0, index = 0; i < 6; ++i) {
        auto r = resource(s, names[i]);
        if (i >= 4 && (!r || r->capacity.value_or(0) <= 0)) continue;
        const double y = res.y + 4.6 * u + index++ * row;
        const Rect bar = Rect::f(res.x + res.w * .26, y, res.w * .43, 2.3 * u);
        if (art) {
            text(c, labels[i], 2.4f, INK, res.x + 1.8 * u, bar.centery(), Anchor::MidLeft, res.w * .23);
            glass(c, bar);
            glass(c, Rect::f(res.x + res.w * .72, y, res.w * .26, 2.3 * u));
            for (int j = 0; j <= 4; ++j)
                c.line({float(bar.x + j * bar.w / 4.0), float(bar.y - .5 * u)},
                       {float(bar.x + j * bar.w / 4.0), float(bar.y - .15 * u)}, 1, INK);
        } else {
            if (d_.on_gauge) d_.on_gauge(labels[i], bar);
            const auto fraction = r ? r->fraction() : std::nullopt;
            if (fraction) {
                const Color color = *fraction < gauges::BAD_BELOW    ? RED
                                    : *fraction < gauges::WARN_BELOW ? AMBER
                                                                     : GREEN;
                c.fill_rect(float(bar.x + .4 * u), float(bar.y + .4 * u), float((bar.w - .8 * u) * *fraction),
                            float(bar.h - .8 * u), color);
                c.fill_rect(float(bar.x + .4 * u), float(bar.y + .4 * u), float((bar.w - .8 * u) * *fraction),
                            float(.3 * u), WHITE);
            }
            const std::string amount = fraction
                                           ? fmt::str("%.0f / %.0f (%.0f%%)", *r->amount, *r->capacity, 100 * *fraction)
                                       : r && r->capacity && *r->capacity == 0 ? "ABSENT"
                                                                               : "UNKNOWN";
            text(c, amount, 2.35f, fraction ? GREEN : AMBER, res.right() - 1.5 * u, bar.centery(), Anchor::MidRight,
                 res.w * .27);
        }
    }
    const Rect hist = Rect::f(1.4 * u, 57.3 * u, left - 1.4 * u, 24 * u);
    if (art) {
        group(c, *assets, hist, "FLIGHT HISTORY");
        glass(c, Rect::f(hist.x + 1.5 * u, hist.y + 4 * u, hist.w - 3 * u, hist.h - 5.2 * u));
    }
    history(c, art, hist, s);
    const Rect stages = Rect::f(1.4 * u, 82.6 * u, left - 1.4 * u, c.h - 84 * u);
    if (art) {
        group(c, *assets, stages, "STAGING");
        glass(c, Rect::f(stages.x + 1.5 * u, stages.y + 3.5 * u, stages.w - 3 * u, stages.h - 4.5 * u));
        const char* headers[] = {"STAGE", "PARTS", "DELTA-V (m/s)", "STATUS"};
        for (int i = 0; i < 4; ++i)
            text(c, headers[i], 2.3f, WHITE, stages.x + (i + .5) * stages.w / 4, stages.y + 4.7 * u, Anchor::Center,
                 stages.w / 4 - 2 * u);
    } else if (s.systems) {
        const auto& data = *s.systems;
        for (size_t i = 0; i < data.stages.size() && i < 4; ++i) {
            const auto& st = data.stages[i];
            const bool active = data.current_stage && *data.current_stage == st.number;
            const double y = stages.y + 7 * u + i * 2.2 * u;
            const std::string cells[] = {std::to_string(st.number), st.parts ? std::to_string(*st.parts) : "--",
                                         st.delta_v ? number(*st.delta_v) : "--", active ? "ACTIVE" : "PENDING"};
            for (int j = 0; j < 4; ++j)
                text(c, cells[j], 2.7f, active ? GREEN : WHITE, stages.x + (j + .5) * stages.w / 4, y, Anchor::Center,
                     stages.w / 4 - 2 * u);
        }
    }
    const auto layout = flight_layout(c, true);
    if (art) {
        group(c, *assets, layout.orbit, "ORBIT");
        sliced(c, assets->module,
               Rect::f(layout.orbit.x + 1.1 * u, layout.orbit.y + 4 * u, layout.orbit.w - 2.2 * u,
                       layout.orbit.h - 5.1 * u),
               1.5 * u);
        group(c, *assets, layout.vessel, "VESSEL");
        sliced(c, assets->module,
               Rect::f(layout.vessel.x + 1.1 * u, layout.vessel.y + 4 * u, layout.vessel.w - 2.2 * u,
                       layout.vessel.h - 5.1 * u),
               1.5 * u);
        group(c, *assets, layout.navball, "");
    } else
        orbit(c, layout.orbit, s);
    // Live vessel silhouette and tank indicators; geometry comes from KSP, never from the texture asset.
    const Rect v = layout.vessel;
    const Rect shape = Rect::f(v.x + 2 * u, v.y + 5 * u, v.w * .48 - 2 * u, v.h - 7 * u);
    if (!art) {
        d_.vessel_contents(shape, s, VesselStyle{BLACK, WHITE, Color{35, 46, 37}, GREEN, BLACK, WHITE});
        for (int i = 0; i < 4; ++i) {
            auto r = resource(s, names[i]);
            const auto f = r ? r->fraction() : std::nullopt;
            const double y = v.y + (7 + i * 5.5) * u;
            text(c, labels[i], 1.45f, WHITE, v.x + v.w * .52, y, Anchor::MidLeft, v.w * .43);
            const Rect bar = Rect::f(v.x + v.w * .52, y + 1.1 * u, v.w * .41, 1.5 * u);
            c.rounded_rect(bar, 2, Color{42, 48, 43});
            if (f) c.fill_rect(float(bar.x), float(bar.y), float(bar.w * *f), float(bar.h), GREEN);
            text(c, f ? fmt::str("%.0f%%", 100 * *f) : "--", 1.6f, GREEN, v.right() - 2 * u, y, Anchor::MidRight,
                 v.w * .17);
        }
    }
    // NavBall B: steel/black annulus, six recessed side instruments, mode lamps below.
    const Rect nav = layout.navball;
    const auto g = Navball::globe_geometry(c, nav, false, true);
    const double outer = g.r + 3.1 * u, side = std::max(5 * u, (nav.w - 2 * outer - 5 * u) / 2), lx = nav.x + 1.6 * u,
                 rx = nav.right() - side - 1.6 * u;
    const char* read_labels[] = {"SPEED", "HEADING", "PITCH", "ROLL", "THROTTLE", "G-FORCE"};
    const double read_h = 9 * u;
    for (int i = 0; i < 6; ++i) {
        const int j = i % 3;
        const double x = i < 3 ? lx : rx;
        const Rect r = Rect::f(x, nav.y + (7 + j * 13) * u, side, read_h);
        const Rect window = Rect::f(r.x + .8 * u, r.y + 3.6 * u, r.w - 1.6 * u, r.h - 4.3 * u);
        if (art) {
            sliced(c, assets->module, r, 1.3 * u);
            text(c, read_labels[i], 2.8f, WHITE, r.centerx(), r.y + 1.7 * u, Anchor::Center, r.w - 2 * u);
            glass(c, window);
        } else {
            std::string n = "--", unit;
            if (s.nav) {
                const auto& t = *s.nav;
                if (i == 0) {
                    n = t.speed ? number(*t.speed, 1) : "--";
                    unit = "m/s";
                }
                if (i == 1) {
                    n = number(t.heading, 1);
                    unit = "deg";
                }
                if (i == 2) {
                    n = number(t.pitch, 1);
                    unit = "deg";
                }
                if (i == 3) {
                    n = number(t.roll, 1);
                    unit = "deg";
                }
                if (i == 4) {
                    n = number(t.throttle * 100);
                    unit = "%";
                }
                if (i == 5) {
                    n = number(t.g_force, 2);
                    unit = "g";
                }
            }
            value(c, window, n, unit);
        }
    }
    if (art) {
        annulus(c, assets->ring, g.cx, g.cy, outer, g.r + .2 * u);
        const double r = outer + .25 * u;
        for (int i = 0; i < 4; ++i) {
            const double a = i * PI / 2, dx = std::cos(a), dy = std::sin(a), tx = -dy, ty = dx;
            c.polygon({{float(g.cx + dx * (r - 1.5 * u)), float(g.cy + dy * (r - 1.5 * u))},
                       {float(g.cx + dx * r + tx * .6 * u), float(g.cy + dy * r + ty * .6 * u)},
                       {float(g.cx + dx * r - tx * .6 * u), float(g.cy + dy * r - ty * .6 * u)}},
                      WHITE);
        }
    } else {
        auto& ball = d_.navball();
        ball.chrome = false;
        ball.draw(c, nav, s.nav);
        ball.chrome = true;
    }
    const auto& modes = Navball::sas_modes();
    const double mw = (nav.w - 4 * u) / 10, y = nav.bottom() - 5 * u;
    if (art) text(c, "SAS MODE", 1.7f, INK, nav.x + 2 * u, y - 1.5 * u, Anchor::MidLeft, nav.w * .2);
    for (int i = 0; i < 10; ++i) {
        const Rect r = Rect::f(nav.x + 2 * u + i * mw, y, mw - .4 * u, 3.3 * u);
        annunciator(c, r, modes[i].second, art, !art && s.nav && s.nav->sas && s.nav->sas_mode == modes[i].first);
    }
}

void FlightDeck::orbit(Canvas& c, const Rect& rect, const Snapshot& s) {
    const double u = c.u;
    // Restore hooks even if invalid telemetry or an SDL allocation throws. Shared orbital physics is unchanged.
    struct Restore {
        Display& d;
        bool titles, crt;
        std::function<Rect(const std::string&, float, Color, double, double, Anchor)> label;
        std::function<void(double, double, double)> body;
        ~Restore() {
            d.panel_titles = titles;
            d.orbit_crt = crt;
            d.orbit_label = std::move(label);
            d.orbit_body = std::move(body);
        }
    } restore{d_, d_.panel_titles, d_.orbit_crt, d_.orbit_label, d_.orbit_body};
    const Rect screen = Rect::f(rect.x + 1.5 * u, rect.y + 4.5 * u, rect.w - 3 * u, rect.h - 6 * u);
    c.rounded_rect(screen, float(.5 * u), Color{0, 8, 3});
    d_.panel_titles = false;
    d_.orbit_crt = true;
    d_.orbit_body = [&](double x, double y, double r) { orbit_art_.body(c, s.body, x, y, r, true); };
    d_.orbit_label = [&](const std::string& label, float, Color color, double x, double y, Anchor anchor) {
        if (label == s.body) return Rect();
        c.text(label, 1.8f, Color{9, 63, 25}, x + 1, y + 1, anchor);
        return c.text(label, 1.8f, color, x, y, anchor);
    };
    d_.orbit(Rect::f(rect.x + 1.2 * u, rect.y, rect.w - 2.4 * u, rect.h - 2 * u), s);
    text(c, s.body, 2.2f, Color{105, 241, 142}, rect.right() - 2.5 * u, rect.y + 6.5 * u, Anchor::MidRight,
         rect.w * .5);
    const std::string pe = "Pe " + fmt::distance(s.periapsis);
    const std::string ap = s.eccentricity >= 1 ? "ESCAPE" : "Ap " + fmt::distance(s.apoapsis);
    text(c, ap, 1.65f, Color{105, 241, 142}, rect.x + 2.8 * u, rect.y + 6.5 * u, Anchor::MidLeft, rect.w * .48);
    text(c, pe, 1.65f, Color{105, 241, 142}, rect.right() - 2.5 * u, rect.bottom() - 2.8 * u, Anchor::MidRight,
         rect.w * .48);
    text(c,
         "a " + fmt::distance(s.semi_major_axis) + "  e " + number(s.eccentricity, 4) + "  i " +
             number(s.inclination, 2) + " deg",
         1.6f, Color{105, 241, 142}, rect.x + 2.8 * u, rect.bottom() - 2.8 * u, Anchor::MidLeft, rect.w * .62);
    orbit_art_.glass(c, screen);
}
void FlightDeck::history(Canvas& c, bool art, const Rect& rect, const Snapshot& s) {
    const double u = c.u;
    const Rect plot = Rect::f(rect.x + 4.5 * u, rect.y + 6 * u, rect.w - 10 * u, rect.h - 10 * u);
    if (art) {
        for (int i = 0; i <= 4; ++i) {
            const float y = float(plot.y + i * plot.h / 4.0);
            c.line({float(plot.x), y}, {float(plot.right()), y}, 1, GRID);
        }
        for (int i = 0; i <= 6; ++i) {
            const float x = float(plot.x + i * plot.w / 6.0);
            c.line({x, float(plot.y)}, {x, float(plot.bottom())}, 1, GRID);
        }
        text(c, "ALTITUDE (km)", 1.6f, GREEN, plot.x, rect.y + 4.8 * u, Anchor::MidLeft, plot.w * .4);
        text(c, "SPEED (m/s)", 1.6f, AMBER, plot.right(), rect.y + 4.8 * u, Anchor::MidRight, plot.w * .4);
        text(c, "LAST 180 SECONDS", 1.6f, WHITE, plot.centerx(), rect.bottom() - 1.9 * u, Anchor::Center, plot.w);
        return;
    }
    if (!std::isfinite(s.ut) || !std::isfinite(s.altitude) || !std::isfinite(s.orbital_speed)) return;
    const std::string key = s.vessel + "@" + s.body;
    if (vessel_ != key || (!history_.empty() && s.ut < history_.back().ut)) history_.clear();
    vessel_ = key;
    if (history_.empty() || s.ut > history_.back().ut + .2) history_.push_back({s.ut, s.altitude, s.orbital_speed});
    while (history_.size() > 1 && (history_.size() > 900 || history_.front().ut < s.ut - 180))
        history_.erase(history_.begin());
    if (history_.size() < 2) {
        text(c, "COLLECTING FLIGHT DATA", 2, WHITE, plot.centerx(), plot.centery(), Anchor::Center, plot.w);
        return;
    }
    double altitude = 1, speed = 1;
    for (const auto& sample : history_)
        altitude = std::max(altitude, sample.altitude), speed = std::max(speed, sample.speed);
    for (int i = 0; i <= 4; ++i) {
        const double y = plot.bottom() - i * plot.h / 4.0;
        text(c, number(altitude / 1000 * i / 4, 0), 1.5f, GREEN, plot.x - .7 * u, y, Anchor::MidRight, 3 * u);
        text(c, number(speed * i / 4, 0), 1.5f, AMBER, plot.right() + .7 * u, y, Anchor::MidLeft, 4 * u);
    }
    const Rect previous = c.clip();
    c.set_clip(previous.clip(plot));
    for (int series = 0; series < 2; ++series) {
        std::vector<Pt> points;
        for (const auto& sample : history_)
            points.push_back(
                {float(plot.right() - (s.ut - sample.ut) / 180 * plot.w),
                 float(plot.bottom() - (series ? sample.speed / speed : sample.altitude / altitude) * plot.h)});
        c.polyline(points, false, float(.18 * u), series ? AMBER : GREEN);
    }
    c.set_clip(previous);
}
}  // namespace ksphmi
