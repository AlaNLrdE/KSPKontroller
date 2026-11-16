#include "apollo.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>

#include "display.hpp"
#include "fmt.hpp"
#include "gauges.hpp"
#include "navball.hpp"
#include "scenes.hpp"
#include "segments.hpp"
#include "theme.hpp"

namespace ksphmi {

namespace {

// ------------------------------------------------------------------------------------------- palette
constexpr Color GREY_TOP{134, 137, 138}, GREY_BOTTOM{112, 115, 116};  // the panel, lit from above
constexpr Color INK{240, 240, 232}, SHADOW{54, 56, 57}, DIM_INK{196, 198, 192};  // painted labels
constexpr Color PLATE{20, 21, 22}, BEZEL{48, 50, 52}, BEZEL_HI{92, 95, 97}, WELL{6, 8, 9};
constexpr Color LAMP_OFF{60, 62, 61}, LAMP_LEGEND{122, 124, 118}, LAMP_TEXT{34, 26, 16};
constexpr Color L_WHITE{240, 236, 214}, L_GREEN{124, 232, 120}, L_AMBER{255, 178, 42}, L_RED{246, 66, 48};
constexpr Color POINTER{246, 244, 232};


float rnd(double v) { return static_cast<float>(std::lround(v)); }

Color mix(Color a, Color b, double t) {
    return Color{uint8_t(std::lround(a.r + (b.r - a.r) * t)), uint8_t(std::lround(a.g + (b.g - a.g) * t)),
                 uint8_t(std::lround(a.b + (b.b - a.b) * t)), 255};
}

std::string upper(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

// UTF-8 code points as separate strings (letter-spaced text is drawn one character at a time).
std::vector<std::string> chars(const std::string& s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        size_t n = 1;
        unsigned char b = static_cast<unsigned char>(s[i]);
        if (b >= 0xF0) n = 4;
        else if (b >= 0xE0) n = 3;
        else if (b >= 0xC0) n = 2;
        out.push_back(s.substr(i, n));
        i += n;
    }
    return out;
}

constexpr float TRACK = .12f;  // letter spacing of the painted labels, in font heights

int spaced_width(Canvas& c, const std::string& s, float units, float track = TRACK) {
    Font& f = c.font(units);
    auto cs = chars(s);
    int w = 0;
    for (const auto& ch : cs) w += c.text_width(f, ch);
    return w + static_cast<int>(std::lround(track * f.height() * .6)) * std::max(0, int(cs.size()) - 1);
}

// Painted (silk-screened) label: wide letter spacing and a one-pixel shadow cut into the panel.
Rect label(Canvas& c, const std::string& s, float units, double x, double y, Anchor a = Anchor::TopLeft,
           Color color = INK, bool shadow = true, float track = TRACK) {
    Font& f = c.font(units);
    const int tw = spaced_width(c, s, units, track), th = f.height();
    const int sp = static_cast<int>(std::lround(track * th * .6));
    int ix = static_cast<int>(x), iy = static_cast<int>(y);
    Rect r(ix, iy, tw, th);
    switch (a) {
        case Anchor::TopLeft: break;
        case Anchor::TopRight: r.x = ix - tw; break;
        case Anchor::Center: r.x = ix - tw / 2, r.y = iy - th / 2; break;
        case Anchor::MidTop: r.x = ix - tw / 2; break;
        case Anchor::MidLeft: r.y = iy - th / 2; break;
        case Anchor::MidRight: r.x = ix - tw, r.y = iy - th / 2; break;
    }
    const int off = std::max(1, static_cast<int>(.07 * c.u));
    for (int pass = shadow ? 0 : 1; pass < 2; ++pass) {
        int pen = r.x + (pass ? 0 : off);
        for (const auto& ch : chars(s)) {
            if (ch != " ") c.text(ch, units, pass ? color : SHADOW, pen, r.y + (pass ? 0 : off));
            pen += c.text_width(f, ch) + sp;
        }
    }
    return r;
}

// Largest size (in 0.05 unit steps, at most `units`) at which every text fits in `room` pixels.
float fit(Canvas& c, const std::vector<std::string>& texts, float units, double room) {
    int widest = 1;
    for (const auto& t : texts) widest = std::max(widest, spaced_width(c, t, units));
    return static_cast<float>(std::floor(units * std::min(1.0, room / widest) * 20) / 20);
}

// A slotted screw head.
void screw(Canvas& c, double x, double y, double r, double angle) {
    c.circle(float(x), float(y), float(r * 1.25), Color{86, 88, 89});
    c.circle(float(x), float(y), float(r), Color{60, 62, 63});
    c.circle(float(x - .1 * r), float(y - .1 * r), float(r * .82), Color{150, 153, 154});
    c.circle(float(x + .08 * r), float(y + .08 * r), float(r * .6), Color{128, 131, 132});
    double dx = std::cos(angle) * r * .8, dy = std::sin(angle) * r * .8;
    c.line({float(x - dx), float(y - dy)}, {float(x + dx), float(y + dy)}, std::max(1.5f, float(.22 * r)),
           Color{44, 46, 47});
}

// A groove across the panel: dark cut with a light lower lip.
void groove(Canvas& c, double x0, double x1, double y) {
    float t = std::max(1.0f, rnd(.12 * c.u));
    c.fill_rect(float(x0), float(y), float(x1 - x0), t, Color{66, 68, 69});
    c.fill_rect(float(x0), float(y + t), float(x1 - x0), t, Color{168, 171, 171});
}

// White group line around related instruments, broken at the top for its title (painted Apollo panel markings).
void frame(Canvas& c, const Rect& r, const std::string& title) {
    const double u = c.u;
    const float t = std::max(2.0f, rnd(.17 * u));
    const float units = 1.7f;
    int tw = title.empty() ? 0 : spaced_width(c, title, units);
    float gap_l = float(r.x + 1.4 * u), gap_r = title.empty() ? gap_l : float(gap_l + tw + 1.2 * u);
    auto line = [&](float x, float y, float w, float h) {
        c.fill_rect(x + 1, y + 1, w, h, SHADOW);
        c.fill_rect(x, y, w, h, INK);
    };
    line(float(r.x), float(r.y), gap_l - r.x, t);
    line(gap_r, float(r.y), float(r.right()) - gap_r, t);
    line(float(r.x), float(r.bottom() - t), float(r.w), t);
    line(float(r.x), float(r.y), t, float(r.h));
    line(float(r.right() - t), float(r.y), t, float(r.h));
    if (!title.empty()) label(c, title, units, gap_l + .6 * u, r.y + t / 2.0, Anchor::MidLeft);
}

// Raised dark bezel around a display (ORBIT, NavBall, scope): screws in the corners, title on the top band.
// Returns the well inside it.
Rect bezel(Canvas& c, const Rect& r, const std::string& title, bool art) {
    const double u = c.u;
    const double top = title.empty() ? 1.1 * u : 2.6 * u, side = 1.1 * u;
    Rect well = Rect::f(r.x + side, r.y + top, r.w - 2 * side, r.h - top - side);
    if (!art) return well;
    c.rounded_rect(r.inflate(.5 * u, .5 * u), rnd(1.3 * u), Color{82, 84, 85});  // shadow of the recess
    c.rounded_rect(r, rnd(1.1 * u), BEZEL);
    c.fill_rect(float(r.x + 1.1 * u), float(r.y + .15 * u), float(r.w - 2.2 * u), std::max(1.0f, rnd(.15 * u)), BEZEL_HI);
    c.rounded_rect(well.inflate(.5 * u, .5 * u), rnd(.7 * u), Color{24, 25, 26});
    c.rounded_rect(well, rnd(.5 * u), WELL);
    const double sr = .5 * u, in = .62 * u + sr * .3;
    screw(c, r.x + in, r.y + in, sr, .6);
    screw(c, r.right() - in, r.y + in, sr, 2.1);
    screw(c, r.x + in, r.bottom() - in, sr, 1.2);
    screw(c, r.right() - in, r.bottom() - in, sr, .2);
    if (!title.empty()) label(c, title, 1.7f, r.x + 2.6 * u, r.y + top / 2 - .05 * u, Anchor::MidLeft);
    return well;
}

// Square backlit annunciator. Art: housing and the unlit legend; live: the lit face over it.
void lamp(Canvas& c, const Rect& r, const std::string& legend, float units, bool art, const Color* lit) {
    const double u = c.u;
    const float rad = rnd(.35 * u);
    Rect face = r.inflate(-.5 * u, -.5 * u);
    if (art) {
        c.rounded_rect(r.inflate(.3 * u, .3 * u), rad + 2, Color{86, 88, 89});
        c.rounded_rect(r, rad, Color{30, 31, 32});
        c.rounded_rect(face, rad * .6f, LAMP_OFF);
        c.fill_rect(float(face.x + 2), float(face.y + 1), float(face.w - 4), std::max(1.0f, rnd(.12 * u)), Color{78, 80, 79});
        label(c, legend, units, face.centerx(), face.centery(), Anchor::Center, LAMP_LEGEND, false, .06f);
        return;
    }
    if (!lit) return;
    c.rounded_rect(face, rad * .6f, mix(*lit, Color{0, 0, 0}, .18));
    c.rounded_rect(face.inflate(-.35 * u, -.35 * u), rad * .5f, *lit);
    c.rounded_rect(face.inflate(-.35 * u, -1.1 * u), rad * .5f, mix(*lit, Color{255, 255, 255}, .25));
    label(c, legend, units, face.centerx(), face.centery(), Anchor::Center, LAMP_TEXT, false, .06f);
}

// -------------------------------------------------------------------------------------- readouts
struct Reading {
    std::string text, unit;
};

Reading split(const std::string& s) {
    size_t sp = s.rfind(' ');
    if (sp == std::string::npos) return {s, ""};
    return {s.substr(0, sp), s.substr(sp + 1)};
}

// A countdown or a period on a fixed HHH:MM:SS window; past 999 hours it switches to days (D:H:M).
Reading hms(double s, int day_hours) {
    if (!std::isfinite(s)) return {"--:--:--", "H:M:S"};
    const char* sign = s < 0 ? "-" : "";
    long long t = std::llround(std::min(std::fabs(s), 1e12));
    long long h = t / 3600, m = t / 60 % 60, sec = t % 60;
    if (h < 1000) return {fmt::str("%s%lld:%02lld:%02lld", sign, h, m, sec), "H:M:S"};
    long long day = static_cast<long long>(day_hours) * 3600, d = std::min(t / day, 999LL);
    return {fmt::str("%s%lld:%02lld:%02lld", sign, d, t % day / 3600, m), "D:H:M"};
}

Color seg_color(Color theme_color) {
    if (theme_color == theme::WARN) return seg::AMBER;
    if (theme_color == theme::BAD) return seg::RED;
    return seg::GREEN;
}

std::string amount_text(double v) { return fmt::str(std::fabs(v) >= 100 ? "%.0f" : "%.1f", v); }

constexpr const char* METER_LABELS[6] = {"ELEC", "LIQ FUEL", "OXIDIZER", "MONOPROP", "SOLID", "XENON"};
constexpr int R_LIVE = 0, R_UNKNOWN = 1, R_ABSENT = 2;

const Resource* find_resource(const Snapshot& s, const char* name) {
    if (!s.systems) return nullptr;
    const Resource* found = nullptr;
    for (const auto& r : s.systems->resources)
        if (r.name == name) found = &r;
    return found;
}

int resource_state(const Resource& r) {
    if (r.fraction()) return R_LIVE;
    return r.capacity && *r.capacity == 0 ? R_ABSENT : R_UNKNOWN;
}

// Base panel texture written straight into the surface: vertical light falloff, horizontal brushing, fine grain.
void brushed(SDL_Surface* s) {
    uint32_t seed = 0x2545F491u;
    auto next = [&] { seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5; return seed; };
    for (int y = 0; y < s->h; ++y) {
        double t = double(y) / std::max(1, s->h - 1);
        Color base = mix(GREY_TOP, GREY_BOTTOM, t);
        int row = int(next() % 5) - 2;  // brushing: each row a little lighter or darker
        auto* px = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(s->pixels) + y * s->pitch);
        for (int x = 0; x < s->w; ++x) {
            int n = row + int(next() % 5) - 2;
            auto ch = [&](int v) { return uint32_t(std::clamp(v + n, 0, 255)); };
            px[x] = 0xFF000000u | ch(base.r) << 16 | ch(base.g) << 8 | ch(base.b);
        }
    }
}

// --------------------------------------------------------------------------------------- geometry
struct TileGeo {
    seg::Layout main, time;
    float mx, my, tx, ty;
};

TileGeo tile_geo(Canvas& c, const Rect& r, const std::string& pattern, bool timed) {
    const double u = c.u;
    TileGeo g;
    g.main = seg::layout(pattern, rnd(3.2 * u));
    g.mx = float(r.x + .7 * u), g.my = float(r.bottom() - g.main.h - .3 * u);
    if (timed) {
        double room = r.right() - .7 * u - (g.mx + g.main.w + 4.3 * u);
        float dh = rnd(1.55 * u);
        g.time = seg::layout("888:88:88", dh);
        if (g.time.w > room) g.time = seg::layout("888:88:88", std::max(4.0f, std::floor(float(dh * room / g.time.w))));
        g.tx = float(r.right() - .7 * u - g.time.w), g.ty = float(r.bottom() - .3 * u - g.time.h);
    }
    return g;
}

void tile(Canvas& c, bool art, Display& d, const Rect& r, const char* name, const std::string& pattern,
          const Reading& value, Color color, const Reading* time = nullptr, const char* time_label = nullptr) {
    const double u = c.u;
    TileGeo g = tile_geo(c, r, pattern, time != nullptr);
    if (art) {
        label(c, name, 1.7f, r.x + .7 * u, r.y + .3 * u);
        seg::window(c, g.mx, g.my, g.main);
        if (time) {
            seg::window(c, g.tx, g.ty, g.time);
            // the unit (H:M:S or D:H:M) changes and is drawn live to the right of this caption
            double unit_w = spaced_width(c, "D:H:M", 1.3f, .08f) + .8 * u;
            label(c, time_label, 1.3f, g.tx + g.time.w - unit_w, r.y + .45 * u, Anchor::TopRight, DIM_INK, true, .08f);
        }
        return;
    }
    if (d.on_tile_begin) d.on_tile_begin(r);
    seg::draw(c, g.mx, g.my, g.main, value.text, seg_color(color));
    if (!value.unit.empty()) label(c, value.unit, 2.0f, g.mx + g.main.w + .8 * u, g.my + g.main.h / 2, Anchor::MidLeft);
    if (time) {
        seg::draw(c, g.tx, g.ty, g.time, time->text, seg::GREEN);
        label(c, time->unit, 1.3f, g.tx + g.time.w, r.y + .45 * u, Anchor::TopRight, INK, true, .08f);
    }
    if (d.on_tile_end) d.on_tile_end(r);
}

// Header: name plate (left), universal time (middle), caution and warning lamps (right).
constexpr int LAMP_COLS = 5;
// In the VAB only the link lamps and the stage TWR lamps mean something; the blank housings are left for the
// phase-2 checklist lamps (NO CHUTE, PARTS LIMIT, ...).
constexpr const char* VAB_LAMPS[2][LAMP_COLS] = {{"UPLINK", "NO DATA", "NO LINK", "", ""},
                                                 {"LAUNCH TWR", "UPPER TWR", "", "", ""}};
constexpr const char* LAMPS[2][LAMP_COLS] = {{"UPLINK", "NO DATA", "NO LINK", "SAS", "RCS"},
                                             {"PE ATMO", "PE SURF", "LOW PWR", "LOW FUEL", "ESCAPE"}};

struct HeaderGeo {
    Rect plate;
    float ut_x, ut_y;
    seg::Layout ut;
    Rect lamps[2][LAMP_COLS];
    float lamp_units;
};

HeaderGeo header_geo(Canvas& c, bool vab) {
    const double u = c.u, w = c.w;
    HeaderGeo g;
    g.plate = Rect::f(2.5 * u, 1.4 * u, w * .44 - 3.2 * u, 8.2 * u);
    g.ut = seg::layout("88 888 88:88:88", rnd(3.0 * u));
    g.ut_x = float(w * .44 + 1.6 * u), g.ut_y = float(3.4 * u);
    const double lw = 7.6 * u, lh = 3.6 * u, gap = .6 * u;
    const double x0 = w - 2.5 * u - (LAMP_COLS * lw + (LAMP_COLS - 1) * gap);
    for (int row = 0; row < 2; ++row)
        for (int col = 0; col < LAMP_COLS; ++col)
            g.lamps[row][col] = Rect::f(x0 + col * (lw + gap), 1.5 * u + row * (lh + gap), lw, lh);
    std::vector<std::string> legends;
    for (const auto& row : vab ? VAB_LAMPS : LAMPS)  // each table fits on its own: flight is unchanged
        for (const char* l : row) legends.push_back(l);
    g.lamp_units = fit(c, legends, 1.5f, lw - 1.6 * u);
    return g;
}

// The unlit cells of each group of the UT window, for the YR / DAY / HR / MIN / SEC captions.
std::vector<std::pair<float, float>> groups(const seg::Layout& l) {
    std::vector<std::pair<float, float>> out;
    float start = -1, end = 0;
    for (const auto& cell : l.cells) {
        if (cell.kind == '8') {
            if (start < 0) start = cell.x;
            end = cell.x + cell.w;
        } else if (start >= 0) {
            out.push_back({start, end}), start = -1;
        }
    }
    if (start >= 0) out.push_back({start, end});
    return out;
}

}  // namespace

// ============================================================================================ skin
ApolloSkin::ApolloSkin(Display& display, const Config& cfg)
    : d_(display),
      day_hours_(cfg.format.day_hours),
      server_text_(fmt::str("kRPC server: %s:%d", cfg.krpc.address.c_str(), cfg.krpc.rpc_port)),
      net_(cfg.krpc.address) {}

ApolloSkin::~ApolloSkin() {
    if (panel_) SDL_DestroyTexture(panel_);
}

ApolloSkin::Key ApolloSkin::key_for(const Snapshot& s) const {
    Key k;
    k.w = d_.w(), k.h = d_.h();
    if (s.state == State::Flight) {
        k.screen = FLIGHT;
        for (int i = 0; i < static_cast<int>(RESOURCE_NAMES.size()); ++i) {
            const Resource* r = find_resource(s, RESOURCE_NAMES[i]);
            if (r && (i < 4 || r->capacity.value_or(0) > 0)) k.resources.push_back(i * 4 + resource_state(*r));
        }
        if (!s.systems) k.resources.push_back(-1);
    } else if (s.state == State::NoVessel && s.scene == "vab" && s.ship) {
        // the art depends on the size, the [vab] config and the highlighted destination (engraved), never on the rest
        k.screen = VAB;
        const TargetChoice t = d_.vab_target(s.ship.get());
        if (t.destination) k.scene = t.destination->key + (t.from_app ? "@app" : "@config");
        k.scene += "@" + s.ship->editor_mode;
    } else if (s.state == State::NoVessel && find_view(s.scene)) {
        k.screen = FACILITY, k.scene = s.scene;
    } else {
        k.screen = s.state == State::NoVessel ? NO_VESSEL : WAITING;
    }
    return k;
}

void ApolloSkin::build(const Key& key, const Snapshot& s) {
    if (panel_) SDL_DestroyTexture(panel_), panel_ = nullptr;
    key_ = key;
    ++builds;
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, key.w, key.h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) return;
    brushed(surface);
    if (SDL_Renderer* renderer = SDL_CreateSoftwareRenderer(surface)) {
        {
            Canvas art(renderer, key.w, key.h, false);
            header(art, true, s, 0, key.screen);
            if (key.screen == FLIGHT) flight(art, true, s);
            else if (key.screen == FACILITY) facility(art, true, s);
            else if (key.screen == VAB) vab(art, true, s);
            else waiting(art, true, s, 0, key.screen == NO_VESSEL);
            art.flush();
        }
        SDL_RenderPresent(renderer);
        SDL_DestroyRenderer(renderer);
        panel_ = SDL_CreateTextureFromSurface(d_.canvas().renderer(), surface);
        if (panel_) SDL_SetTextureBlendMode(panel_, SDL_BLENDMODE_NONE);
    }
    SDL_FreeSurface(surface);
}

void ApolloSkin::draw(const Snapshot& s, double now) {
    Canvas& c = d_.canvas();
    Key key = key_for(s);
    if (!panel_ || !(key == key_)) build(key, s);
    if (panel_) c.image(panel_, 0, 0, float(c.w), float(c.h));
    else c.clear(GREY_BOTTOM);
    header(c, false, s, now, key.screen);
    if (key.screen == FLIGHT) flight(c, false, s);
    else if (key.screen == FACILITY) facility(c, false, s);
    else if (key.screen == VAB) vab(c, false, s);
    else waiting(c, false, s, now, key.screen == NO_VESSEL);
    c.flush();
}

void ApolloSkin::header(Canvas& c, bool art, const Snapshot& s, double now, int screen) {
    const double u = c.u;
    const HeaderGeo g = header_geo(c, screen == VAB);
    const Rect& p = g.plate;
    if (art) {
        c.rounded_rect(p.inflate(.4 * u, .4 * u), rnd(.7 * u), Color{84, 86, 87});
        c.rounded_rect(p, rnd(.5 * u), PLATE);
        c.fill_rect(float(p.x + .6 * u), float(p.y + .12 * u), float(p.w - 1.2 * u), std::max(1.0f, rnd(.1 * u)), Color{60, 62, 64});
        const double sr = .38 * u;
        screw(c, p.x + .9 * u, p.y + .9 * u, sr, .4), screw(c, p.right() - .9 * u, p.y + .9 * u, sr, 1.9);
        screw(c, p.x + .9 * u, p.bottom() - .9 * u, sr, 2.7), screw(c, p.right() - .9 * u, p.bottom() - .9 * u, sr, 1.1);
        if (screen == FLIGHT) {
            label(c, "UNIVERSAL TIME", 1.5f, g.ut_x, 1.1 * u);
            seg::window(c, g.ut_x, g.ut_y, g.ut);
            static const char* names[] = {"YR", "DAY", "HR", "MIN", "SEC"};
            auto gs = groups(g.ut);
            for (size_t i = 0; i < gs.size() && i < 5; ++i)
                label(c, names[i], 1.2f, g.ut_x + g.ut.pad_x + (gs[i].first + gs[i].second) / 2, g.ut_y + g.ut.h + .25 * u,
                      Anchor::MidTop, DIM_INK, true, .06f);
        }
        for (int row = 0; row < 2; ++row)
            for (int col = 0; col < LAMP_COLS; ++col)
                lamp(c, g.lamps[row][col], (screen == VAB ? VAB_LAMPS : LAMPS)[row][col], g.lamp_units, true, nullptr);
        groove(c, 0, c.w, 10.9 * u);
        return;
    }
    // name plate: engraved white on black, uppercase
    const double room = p.w - 3.6 * u;
    std::string title, line2;
    if (screen == FLIGHT) {
        title = upper(s.vessel.empty() ? "Unnamed vessel" : s.vessel);
        line2 = upper(s.body) + "   \xC2\xB7   " + upper(fmt::situation(s.situation));
    } else if (screen == VAB && s.ship) {
        title = upper(s.ship->name.empty() ? "Untitled ship" : s.ship->name);
        line2 = "VAB   \xC2\xB7   " + editor_mode_label(s.ship->editor_mode);
    } else if (screen == FACILITY) {
        title = "KSP KONTROLLER", line2 = upper(find_view(s.scene)->subtitle);
    } else {
        title = "KSP KONTROLLER", line2 = "FLIGHT DATA DISPLAY";
    }
    float tu = fit(c, {title}, 3.6f, room), lu = fit(c, {line2}, 2.1f, room);
    label(c, title, std::max(tu, 2.0f), p.x + 1.8 * u, p.y + 1.1 * u, Anchor::TopLeft, INK, false);
    label(c, line2, std::max(lu, 1.4f), p.x + 1.8 * u, p.y + 5.3 * u, Anchor::TopLeft, DIM_INK, false);
    if (screen == FLIGHT) {
        std::string clock = fmt::clock(s.ut, day_hours_);
        clock.erase(std::remove_if(clock.begin(), clock.end(), [](char ch) { return ch == 'Y' || ch == 'D'; }), clock.end());
        seg::draw(c, g.ut_x, g.ut_y, g.ut, clock, seg::GREEN);
    }
    // lamps
    const bool is_stale = stale(s, now), flight_now = s.state == State::Flight;
    const auto& legends = screen == VAB ? VAB_LAMPS : LAMPS;
    auto show = [&](int row, int col, bool on, Color color) {
        if (on_lamp) on_lamp(legends[row][col], on ? &color : nullptr);
        if (on) lamp(c, g.lamps[row][col], legends[row][col], g.lamp_units, false, &color);
    };
    show(0, 0, s.state != State::Connecting && !is_stale, L_GREEN);
    show(0, 1, is_stale, L_AMBER);
    show(0, 2, s.state == State::Connecting, L_RED);
    if (screen == VAB) {  // no flight annunciators in the editor
        const TwrLimits& limits = d_.config().vab.twr;
        auto twr_lamp = [&](int col, TwrLevel level) {
            show(1, col, level == TwrLevel::Warn || level == TwrLevel::Bad, level == TwrLevel::Bad ? L_RED : L_AMBER);
        };
        twr_lamp(0, s.ship && s.ship->editor_mode == "construction" ? launch_twr_level(*s.ship, limits) : TwrLevel::None);
        twr_lamp(1, s.ship && s.ship->editor_mode == "construction" ? upper_twr_level(*s.ship, limits) : TwrLevel::None);
        return;
    }
    show(0, 3, flight_now && s.nav && s.nav->sas, L_WHITE);
    show(0, 4, flight_now && s.nav && s.nav->rcs, L_WHITE);
    show(1, 0, flight_now && s.periapsis >= 0 && s.periapsis < s.atmosphere_depth, L_AMBER);
    show(1, 1, flight_now && s.periapsis < 0, L_RED);
    auto low = [&](int col, const char* name) {
        const Resource* r = flight_now ? find_resource(s, name) : nullptr;
        std::optional<double> f;
        if (r) f = r->fraction();
        if (f && *f <= gauges::WARN_BELOW) show(1, col, true, *f <= gauges::BAD_BELOW ? L_RED : L_AMBER);
    };
    low(2, "ElectricCharge");
    low(3, "LiquidFuel");
    show(1, 4, flight_now && s.eccentricity >= 1, L_WHITE);
}

void ApolloSkin::flight(Canvas& c, bool art, const Snapshot& s) {
    const double u = c.u;
    const double top = 13 * u, bottom = c.h - 2.5 * u;
    const double left_w = c.w * 0.44;
    const double x0 = 2.5 * u;
    const double metric_gap = .8 * u;
    const double col_w = (left_w - x0 - metric_gap) / 2;
    const double big_h = 7 * u, row_h = 7 * u;
    const std::string DIST = "8888888";
    if (art) frame(c, Rect::f(x0 - .8 * u, top - .9 * u, left_w - x0 + 1.3 * u, 5 * row_h + 4 * metric_gap + 1.6 * u),
                   "ORBITAL PARAMETERS");

    const bool hyperbolic = s.eccentricity >= 1;
    Color pe_color = s.periapsis < 0 ? theme::BAD : s.periapsis < s.atmosphere_depth ? theme::WARN : theme::ACCENT;
    Reading ap = hyperbolic ? Reading{"ESCAPE", ""} : split(fmt::distance(s.apoapsis));
    Reading t_ap = hyperbolic ? Reading{"", "H:M:S"} : hms(s.time_to_apoapsis, day_hours_);
    Reading t_pe = hms(s.time_to_periapsis, day_hours_);
    tile(c, art, d_, Rect::f(x0, top, col_w, big_h), "APOAPSIS", "888888", ap, theme::GOOD, &t_ap, "TIME TO AP");
    tile(c, art, d_, Rect::f(x0 + col_w + metric_gap, top, col_w, big_h), "PERIAPSIS", "888888",
         split(fmt::distance(s.periapsis)), pe_color, &t_pe, "TIME TO PE");

    std::string inc = fmt::degrees(s.inclination);
    if (inc.size() > 2 && inc.compare(inc.size() - 2, 2, "\xC2\xB0") == 0) inc.resize(inc.size() - 2);
    struct Row {
        const char* la;
        std::string pa;
        Reading va;
        Color ca;
        const char* lb;
        std::string pb;
        Reading vb;
        Color cb;
    };
    const Row rows[] = {
        {"ALTITUDE", DIST, split(fmt::distance(s.altitude)), theme::VALUE, "VERTICAL SPEED", DIST,
         split(fmt::speed(s.vertical_speed)), s.vertical_speed >= 0 ? theme::GOOD : theme::WARN},
        {"ORBITAL SPEED", DIST, split(fmt::speed(s.orbital_speed)), theme::VALUE, "SURFACE SPEED", DIST,
         split(fmt::speed(s.surface_speed)), theme::VALUE},
        {"INCLINATION", DIST, Reading{inc, "DEG"}, theme::VALUE, "ECCENTRICITY", DIST,
         Reading{fmt::str("%.4f", s.eccentricity), ""}, theme::VALUE},
        {"PERIOD", "888:88:88", hyperbolic ? Reading{"--:--:--", "H:M:S"} : hms(s.period, day_hours_), theme::VALUE,
         "SEMI-MAJOR AXIS", DIST, split(fmt::distance(s.semi_major_axis)), theme::VALUE},
    };
    double y = top + big_h + metric_gap;
    for (const Row& row : rows) {
        tile(c, art, d_, Rect::f(x0, y, col_w, row_h), row.la, row.pa, row.va, row.ca);
        tile(c, art, d_, Rect::f(x0 + col_w + metric_gap, y, col_w, row_h), row.lb, row.pb, row.vb, row.cb);
        y += row_h + metric_gap;
    }
    resources(c, art, Rect::f(x0, 54 * u, left_w - x0, 25 * u), s);
    staging(c, art, Rect::f(x0, 80.6 * u, left_w - x0, bottom - 80.6 * u), s);

    const FlightLayout layout = flight_layout(c);
    const Rect& orbit = layout.orbit;
    Rect orbit_well = bezel(c, orbit, "ORBIT", art);
    vessel(c, art, layout.vessel, s);
    navball(c, art, layout.navball, s);
    if (art) {
        label(c, "INERTIAL", 1.3f, orbit.right() - 2.6 * u, orbit.y + 1.3 * u, Anchor::MidRight, DIM_INK, true, .08f);
        return;
    }
    // ORBIT's drawing is the classic one; its body and apsis labels are painted in the panel lettering.
    d_.panel_titles = false;
    d_.orbit_label = [&c](const std::string& t, float units, Color color, double x, double y, Anchor a) {
        return label(c, t.size() > 2 ? upper(t) : t, units * .8f, x, y, a, color, false, .08f);
    };
    try {
        d_.orbit(orbit_well, s);
    } catch (...) {
        d_.panel_titles = true, d_.orbit_label = nullptr;
        throw;
    }
    d_.panel_titles = true, d_.orbit_label = nullptr;
}

void ApolloSkin::vessel(Canvas& c, bool art, const Rect& rect, const Snapshot& s) {
    const double u = c.u;
    const Rect well = bezel(c, rect, "VESSEL", art);
    const double bw = 5.6 * u, bh = 2.3 * u, gap = .5 * u;
    const double bx = well.x + 1.0 * u, by = well.y + 1.0 * u;
    const float units = fit(c, {"FUEL", "HEAT"}, 1.2f, bw - 1.2 * u);
    for (int i = 0; i < 2; ++i) {
        const Rect r = Rect::f(bx + i * (bw + gap), by, bw, bh);
        const bool on = (i == 1) == d_.vessel_heat();
        lamp(c, r, i ? "HEAT" : "FUEL", units, art, art || !on ? nullptr : &L_WHITE);
    }
    if (art) return;
    const double top = by + bh + .8 * u;
    d_.vessel_contents(Rect::f(well.x, top, well.w, well.bottom() - top), s,
                       VesselStyle{WELL, Color{120, 126, 122}, Color{34, 38, 36}, L_GREEN, Color{20, 20, 20}, DIM_INK});
}

namespace {
// Metal disc lit from above (the NavBall's round bezel), drawn row by row into the cached panel.
void metal_disc(Canvas& c, double cx, double cy, double radius) {
    int rr = static_cast<int>(radius);
    for (int dy = -rr; dy <= rr; ++dy) {
        double t = (dy + radius) / (2 * radius);
        double v = t < .45 ? 150 - 70 * t / .45 : 80 + 70 * (t - .45) / .55;  // light top, dark middle, lighter bottom
        double half = std::sqrt(std::max(radius * radius - double(dy) * dy, 0.0));
        uint8_t l = uint8_t(std::clamp(std::lround(v), 0L, 255L));
        c.fill_rect(float(cx - half), float(cy + dy), float(2 * half), 1.0f, Color{l, uint8_t(l + 2), uint8_t(l + 3)});
    }
}

// A captioned readout: painted caption, then the window below it. Returns the window's top left.
struct Readout {
    float x, y;
    seg::Layout l;
};
Readout readout(Canvas& c, bool art, double x, double y, double room, const char* caption, const std::string& pattern,
                double max_h, bool right = false) {
    const double u = c.u;
    seg::Layout probe = seg::layout(pattern, 100);
    float dh = std::max(4.0f, std::floor(float(std::min(max_h, 100 * room / probe.w))));
    Readout r{0, float(y + 2.0 * u), seg::layout(pattern, dh)};
    r.x = float(right ? x + room - r.l.w : x);
    if (art) {
        label(c, caption, 1.35f, right ? x + room : x, y, right ? Anchor::TopRight : Anchor::TopLeft, INK, true, .08f);
        seg::window(c, r.x, r.y, r.l);
    }
    return r;
}
}  // namespace

void ApolloSkin::navball(Canvas& c, bool art, const Rect& rect, const Snapshot& s) {
    const double u = c.u;
    const Navball::Globe g = Navball::globe_geometry(c, rect);
    const double ring = g.r + 4.0 * u;
    // readout columns either side of the bezel
    const double lx = rect.x + 1.6 * u, lw = g.cx - ring - 1.2 * u - lx;
    const double rx = g.cx + ring + 1.2 * u, rw = rect.right() - 1.6 * u - rx;
    // Five rows down to the BURN readout; they close up on a shorter panel so the last one (about 6.2u tall)
    // stays above the SAS MODE caption and lamps at the bottom.
    const double last = std::min(36.0 * u, rect.h - 5.9 * u - 1.8 * u - .6 * u - 6.2 * u);
    const double k = (last - 3.3 * u) / (32.7 * u);
    const double rows[5] = {rect.y + 3.3 * u, rect.y + (3.3 + 10.3 * k) * u, rect.y + (3.3 + 17.9 * k) * u,
                            rect.y + (3.3 + 25.5 * k) * u, rect.y + last};
    const Readout speed = readout(c, art, lx, rows[0] + 3.4 * u, lw, "SPEED  M/S", "8888888", 2.4 * u);
    const Readout pitch = readout(c, art, lx, rows[1], lw, "PITCH  DEG", "88888", 2.6 * u);
    const Readout thr = readout(c, art, lx, rows[2], lw, "THROTTLE  %", "888", 2.6 * u);
    const Readout node_dv = readout(c, art, lx, rows[3], lw, "NODE DV  M/S", "888888", 2.1 * u);
    const Readout burn = readout(c, art, lx, rows[4], lw, "BURN  H:M:S", "888:88:88", 2.1 * u);
    const Readout hdg = readout(c, art, rx, rows[0], rw, "HEADING  DEG", "888", 3.4 * u, true);
    const Readout roll = readout(c, art, rx, rows[1], rw, "ROLL  DEG", "88888", 2.6 * u, true);
    const Readout gee = readout(c, art, rx, rows[2], rw, "G-FORCE", "888", 2.6 * u, true);
    const Readout t_node = readout(c, art, rx, rows[3], rw, "TIME TO NODE  H:M:S", "888:88:88", 2.1 * u, true);
    const Readout start = readout(c, art, rx, rows[4], rw, "BURN START IN  H:M:S", "888:88:88", 2.1 * u, true);
    // speed reference lamps above the speed window
    static const char* const MODES[3][2] = {{"orbit", "ORBIT"}, {"surface", "SURF"}, {"target", "TGT"}};
    const double mw = (lw - 2 * .5 * u) / 3, mh = 2.3 * u;
    const float mode_units = fit(c, {"ORBIT", "SURF", "TGT"}, 1.2f, mw - 1.2 * u);
    auto mode_rect = [&](int i) { return Rect::f(lx + i * (mw + .5 * u), rows[0] + .3 * u, mw, mh); };
    // SAS mode lamps along the bottom
    const auto& sas = Navball::sas_modes();
    const double gap = .5 * u, sw = std::min(7.0 * u, (rect.w - 3.2 * u - 9 * gap) / 10), sh = 3.3 * u;
    const double sx = rect.centerx() - (10 * sw + 9 * gap) / 2, sy = rect.bottom() - 5.9 * u;
    std::vector<std::string> legends;
    for (const auto& m : sas) legends.push_back(m.second);
    const float sas_units = fit(c, legends, 1.35f, sw - 1.3 * u);
    if (art) {
        frame(c, rect, "NAVBALL  \xC2\xB7  FLIGHT DIRECTOR ATTITUDE INDICATOR");
        c.circle(float(g.cx), float(g.cy), float(ring + .5 * u), Color{84, 86, 87});
        metal_disc(c, g.cx, g.cy, ring);
        c.ring(float(g.cx), float(g.cy), float(ring), std::max(1.0f, rnd(.12 * u)), Color{52, 54, 55});
        c.circle(float(g.cx), float(g.cy), float(g.r + 3.0 * u), Color{30, 31, 32});
        c.circle(float(g.cx), float(g.cy), float(g.r + 2.7 * u), WELL);
        for (int k = 0; k < 4; ++k) {
            double a = PI / 4 + k * PI / 2;
            screw(c, g.cx + (ring - .9 * u) * std::cos(a), g.cy + (ring - .9 * u) * std::sin(a), .5 * u, a * 1.7);
        }
        label(c, "SPEED REF", 1.15f, lx, rows[0] - 1.6 * u, Anchor::TopLeft, DIM_INK, true, .08f);
        for (int i = 0; i < 3; ++i) lamp(c, mode_rect(i), MODES[i][1], mode_units, true, nullptr);
        label(c, "SAS MODE", 1.15f, sx, sy - 1.8 * u, Anchor::TopLeft, DIM_INK, true, .08f);
        for (int i = 0; i < 10; ++i) lamp(c, Rect::f(sx + i * (sw + gap), sy, sw, sh), sas[i].second, sas_units, true, nullptr);
        return;
    }
    Navball& ball = d_.navball();
    ball.chrome = false;
    ball.draw(c, rect, s.nav);
    ball.chrome = true;
    if (!s.nav) {
        label(c, "ATTITUDE UNAVAILABLE", 2.0f, g.cx, g.cy, Anchor::Center, INK, false);
        return;
    }
    const Navigation& n = *s.nav;
    for (int i = 0; i < 3; ++i)
        if (n.speed_mode == MODES[i][0]) lamp(c, mode_rect(i), MODES[i][1], mode_units, false, &L_WHITE);
    for (int i = 0; i < 10; ++i)
        if (n.sas && n.sas_mode == sas[i].first)
            lamp(c, Rect::f(sx + i * (sw + gap), sy, sw, sh), sas[i].second, sas_units, false, &L_GREEN);
    seg::draw(c, speed.x, speed.y, speed.l, n.speed ? fmt::str("%.1f", *n.speed) : "--", seg::GREEN);
    seg::draw(c, pitch.x, pitch.y, pitch.l, fmt::str("%.1f", n.pitch), seg::GREEN);
    seg::draw(c, roll.x, roll.y, roll.l, fmt::str("%.1f", n.roll), seg::GREEN);
    long long heading = ((std::llround(n.heading) % 360) + 360) % 360;
    seg::draw(c, hdg.x, hdg.y, hdg.l, fmt::str("%03lld", heading), seg::GREEN);
    seg::draw(c, thr.x, thr.y, thr.l, std::to_string(std::llround(std::clamp(n.throttle, 0.0, 1.0) * 100)), seg::GREEN);
    Color g_color = n.g_force < 5 ? seg::GREEN : n.g_force < 10 ? seg::AMBER : seg::RED;
    seg::draw(c, gee.x, gee.y, gee.l, fmt::str("%.1f", n.g_force), g_color);
    if (n.node_remaining) {
        seg::draw(c, node_dv.x, node_dv.y, node_dv.l, fmt::str("%.1f", std::max(0.0, *n.node_remaining)), seg::GREEN);
        if (n.node_time) seg::draw(c, t_node.x, t_node.y, t_node.l, hms(*n.node_time, day_hours_).text, seg::GREEN);
        if (n.burn_seconds) seg::draw(c, burn.x, burn.y, burn.l, hms(*n.burn_seconds, day_hours_).text, seg::GREEN);
        if (n.burn_seconds && n.node_time) {
            double in = *n.node_time - *n.burn_seconds / 2;
            seg::draw(c, start.x, start.y, start.l, hms(in, day_hours_).text, in < 0 ? seg::AMBER : seg::GREEN);
        }
    }
}

void ApolloSkin::resources(Canvas& c, bool art, const Rect& rect, const Snapshot& s) {
    const double u = c.u;
    if (art) frame(c, rect, "RESOURCES  \xC2\xB7  VESSEL TOTALS");
    if (!s.systems) {
        if (!art) label(c, "RESOURCE DATA UNAVAILABLE", 2.0f, rect.centerx(), rect.centery(), Anchor::Center, DIM_INK);
        return;
    }
    std::vector<std::pair<int, const Resource*>> visible;
    for (int i = 0; i < static_cast<int>(RESOURCE_NAMES.size()); ++i) {
        const Resource* r = find_resource(s, RESOURCE_NAMES[i]);
        if (r && (i < 4 || r->capacity.value_or(0) > 0)) visible.emplace_back(i, r);
    }
    if (visible.empty()) return;
    const int n = static_cast<int>(visible.size());
    const double cw = (rect.w - 2 * u) / n;
    std::vector<std::string> names;
    for (auto& v : visible) names.push_back(METER_LABELS[v.first]);
    const float lu = fit(c, names, 1.7f, cw - 1.2 * u);
    const seg::Layout pct = seg::layout("888", rnd(2.2 * u)), amount = seg::layout("888888", rnd(1.35 * u));
    for (int i = 0; i < n; ++i) {
        const Resource& r = *visible[i].second;
        const int state = resource_state(r);
        const double mx = rect.x + u + (i + .5) * cw;
        const double mw = std::min(3.4 * u, cw * .32), my0 = rect.y + 4.4 * u, my1 = rect.y + 16.6 * u;
        Rect meter = Rect::f(mx + .9 * u - mw / 2, my0, mw, my1 - my0);
        const double scale_h = meter.h - 1.0 * u, base = meter.bottom() - .5 * u;  // 0 % .. 100 % inside the window
        auto level_y = [&](double f) { return base - f * scale_h; };
        const float px = float(mx - pct.w / 2), py = float(rect.y + 17.3 * u);
        const float ax = float(mx - amount.w / 2), ay = float(py + pct.h + .35 * u);
        if (art) {
            label(c, METER_LABELS[visible[i].first], lu, mx, rect.y + 1.8 * u, Anchor::MidTop);
            c.rounded_rect(meter.inflate(.7 * u, .7 * u), rnd(.4 * u), Color{40, 42, 44});
            c.rounded_rect(meter.inflate(.3 * u, .3 * u), rnd(.3 * u), Color{22, 23, 24});
            c.rect(meter, WELL);
            // red and amber zones along the left edge of the window
            float zx = float(meter.x + 1), zw = float(std::max(2.0, .35 * u));
            c.fill_rect(zx, float(level_y(gauges::BAD_BELOW)), zw, float(gauges::BAD_BELOW * scale_h), Color{210, 40, 32});
            c.fill_rect(zx, float(level_y(gauges::WARN_BELOW)), zw, float((gauges::WARN_BELOW - gauges::BAD_BELOW) * scale_h),
                        Color{230, 150, 24});
            // scale on the panel, left of the window
            for (int k = 0; k <= 10; ++k) {
                double len = k % 5 == 0 ? .9 * u : .5 * u, yy = level_y(k / 10.0);
                float t = std::max(1.5f, rnd((k % 5 == 0 ? .18 : .11) * u));
                c.fill_rect(float(meter.x - .75 * u - len), float(yy - t / 2), float(len), t, INK);
            }
            for (int k : {0, 50, 100})
                label(c, std::to_string(k), 1.15f, meter.x - 2.0 * u, level_y(k / 100.0), Anchor::MidRight, INK, true, .04f);
            seg::window(c, px, py, pct);
            label(c, "%", 1.4f, px + pct.w + .4 * u, py + pct.h / 2, Anchor::MidLeft);
            seg::window(c, ax, ay, amount);
            if (state == R_ABSENT) {  // barber pole: not installed
                c.set_clip(meter);
                float stripe = float(.9 * u);
                for (double sy = meter.y - meter.w; sy < meter.bottom() + meter.w; sy += 2 * stripe)
                    c.polygon({{float(meter.x), float(sy)}, {float(meter.right()), float(sy - meter.w)},
                               {float(meter.right()), float(sy - meter.w + stripe)}, {float(meter.x), float(sy + stripe)}},
                              Color{226, 226, 218}, false);
                c.set_clip(Rect(0, 0, c.w, c.h));
            }
            continue;
        }
        if (d_.on_gauge) d_.on_gauge(METER_LABELS[visible[i].first], meter);
        auto f = r.fraction();
        if (f) {
            Color col = *f <= gauges::BAD_BELOW ? seg::RED : *f <= gauges::WARN_BELOW ? seg::AMBER : seg::GREEN;
            double ly = level_y(*f);
            c.fill_rect(float(meter.x + .5 * u), float(ly), float(meter.w - .5 * u), float(base - ly),
                        mix(col, WELL, .72));
            // pointer: a bar across the window and a wedge pointing at the scale
            float t = std::max(2.0f, rnd(.28 * u));
            c.fill_rect(float(meter.x + .5 * u), float(ly - t / 2), float(meter.w - .5 * u), t, POINTER);
            float wx = float(meter.right() + .25 * u), ww = float(1.1 * u);
            c.polygon({{wx, float(ly)}, {wx + ww, float(ly - .6 * u)}, {wx + ww, float(ly + .6 * u)}}, POINTER);
            seg::draw(c, px, py, pct, std::to_string(std::lround(100 * *f)), col);
            seg::draw(c, ax, ay, amount, amount_text(*r.amount), seg::GREEN);
        } else if (state == R_UNKNOWN) {
            seg::draw(c, px, py, pct, "---", seg::AMBER);
            seg::draw(c, ax, ay, amount, "------", seg::AMBER);
        }
    }
}

void ApolloSkin::staging(Canvas& c, bool art, const Rect& rect, const Snapshot& s) {
    const double u = c.u;
    const double row0 = rect.y + 3.9 * u, pitch = 3.15 * u;
    const float dh = rnd(1.9 * u);
    const seg::Layout stage = seg::layout("88", dh), parts = seg::layout("888", dh), dv = seg::layout("888888", dh);
    const double lamp_x = rect.x + 1.4 * u, lamp_w = 7.0 * u;
    const double sx = lamp_x + lamp_w + 2.0 * u, ppx = sx + stage.w + 4.0 * u, dx = ppx + parts.w + 4.0 * u;
    const seg::Layout current = seg::layout("88", rnd(4.6 * u));
    const float cx = float(rect.right() - 2.4 * u - current.w), cy = float(row0 + .4 * u);
    const auto& data = s.systems;
    if (art) {
        frame(c, rect, "STAGING");
        label(c, "STAGE", 1.3f, sx, rect.y + 1.6 * u, Anchor::TopLeft, DIM_INK, true, .08f);
        label(c, "PARTS", 1.3f, ppx, rect.y + 1.6 * u, Anchor::TopLeft, DIM_INK, true, .08f);
        label(c, "DELTA-V  M/S", 1.3f, dx, rect.y + 1.6 * u, Anchor::TopLeft, DIM_INK, true, .08f);
        label(c, "CURRENT", 1.5f, cx + current.w / 2, cy + current.h + .5 * u, Anchor::MidTop);
        seg::window(c, cx, cy, current);
        for (int i = 0; i < 4; ++i) {
            double y = row0 + i * pitch;
            lamp(c, Rect::f(lamp_x, y + (stage.h - 2.7 * u) / 2, lamp_w, 2.7 * u), "ACTIVE", 1.15f, true, nullptr);
            seg::window(c, float(sx), float(y), stage);
            seg::window(c, float(ppx), float(y), parts);
            seg::window(c, float(dx), float(y), dv);
        }
        return;
    }
    if (data && data->current_stage) seg::draw(c, cx, cy, current, std::to_string(*data->current_stage), seg::GREEN);
    else seg::draw(c, cx, cy, current, "--", seg::AMBER);
    if (!data || data->stages.empty()) {
        label(c, !data ? "STAGE DATA UNAVAILABLE" : "NO PENDING STAGES", 1.6f, (dx + dv.w + cx) / 2, row0 + 1.5 * pitch,
              Anchor::Center, DIM_INK);
        return;
    }
    for (size_t index = 0; index < data->stages.size() && index < 4; ++index) {
        const Stage& st = data->stages[index];
        double y = row0 + index * pitch;
        bool active = data->current_stage && st.number == *data->current_stage;
        Color lit = L_GREEN;
        if (active) lamp(c, Rect::f(lamp_x, y + (stage.h - 2.7 * u) / 2, lamp_w, 2.7 * u), "ACTIVE", 1.15f, false, &lit);
        seg::draw(c, float(sx), float(y), stage, fmt::str("%02d", st.number), seg::GREEN);
        seg::draw(c, float(ppx), float(y), parts, st.parts ? std::to_string(*st.parts) : "--", seg::GREEN);
        seg::draw(c, float(dx), float(y), dv, st.delta_v ? fmt::str("%.0f", *st.delta_v) : "--", seg::GREEN);
    }
}

void ApolloSkin::editor(Canvas& c, bool art, const Snapshot& s) {
    const ShipReport& report = *s.ship;
    const EditorPage page = editor_page(report);
    const double u = c.u, gap = 1.6 * u, x = 2.5 * u, width = c.w - 5 * u;
    const double metric_w = (width - 3 * gap) / 4;
    for (size_t i = 0; i < page.metrics.size(); ++i)
        tile(c, art, d_, Rect::f(x + i * (metric_w + gap), 13 * u, metric_w, 7 * u), page.metrics[i].first.c_str(),
             "888888", {page.metrics[i].second, ""}, theme::VALUE);
    const double panel_w = (width - gap) / 2;
    auto table = [&](double px, const std::string& title, const std::vector<EditorRow>& rows) {
        const Rect box = Rect::f(px, 23 * u, panel_w, 70 * u);
        if (art) { frame(c, box, title.c_str()); return; }
        if (d_.on_vab_panel) d_.on_vab_panel(title, box);
        const double left = box.x + 1.3 * u, right = box.right() - 1.3 * u, room = right - left;
        const PageSlice slice = page_slice(rows.size(), 10, s.received);
        for (size_t i = slice.begin; i < slice.end; ++i) {
            const auto& row = rows[i];
            const double y = box.y + 3.2 * u + (i - slice.begin) * 6 * u;
            const double label_room = row.value.empty() ? room : room * .52;
            label(c, row.label, std::min(2.0f, fit(c, {row.label}, 2.0f, label_room)), left, y, Anchor::TopLeft, row.warning ? L_AMBER : INK, false);
            if (!row.value.empty()) label(c, row.value, std::min(2.0f, fit(c, {row.value}, 2.0f, room * .45)), right, y,
                                         Anchor::TopRight, row.warning ? L_AMBER : INK, false);
            if (!row.detail.empty()) label(c, row.detail, std::min(1.65f, fit(c, {row.detail}, 1.65f, room)), left,
                                          y + 2.6 * u, Anchor::TopLeft, DIM_INK, false);
        }
        label(c, fmt::str("PAGE %zu / %zu%s", slice.page + 1, slice.pages, slice.pages > 1 ? "  /  AUTO 8s" : ""),
              1.5, left, box.bottom() - 3 * u, Anchor::TopLeft, DIM_INK, false);
    };
    table(x, page.left_title, page.left);
    table(x + panel_w + gap, page.right_title, page.right);
    if (!art) label(c, "EDITOR SYNC  /  SAS NEXT  /  RCS PREVIOUS", 1.7, x, 96 * u, Anchor::TopLeft, DIM_INK, false);
}

void ApolloSkin::vab(Canvas& c, bool art, const Snapshot& s) {
    if (s.ship && s.ship->editor_mode != "construction") { editor(c, art, s); return; }
    const double u = c.u;
    const ShipReport* r = s.ship.get();
    if (!art && !r) return;
    const double top = 13 * u, bottom = c.h - 2.5 * u;
    const double left_w = c.w * 0.44, x0 = 2.5 * u, metric_gap = .8 * u;
    const double col_w = (left_w - x0 - metric_gap) / 2, row_h = 7 * u;
    const double x1 = left_w + 2.4 * u, right_r = c.w - 2.5 * u, rw = right_r - x1;
    const TargetChoice choice = d_.vab_target(r);
    const Destination* target = choice.destination;
    const auto& dests = d_.config().vab.destinations;
    const bool pending = r && r->delta_v_pending;
    // TOTAL is KSP's own figure (the app's situation); the mission delta-v judges target, margin and destinations
    const double total = r ? r->total_delta_v() : 0;
    const MissionDeltaV mission = r ? mission_delta_v(*r, d_.config().vab.atmosphere_dv) : MissionDeltaV{};
    const double mtotal = mission.total;
    auto clamp_dv = [](double v) { return std::isfinite(v) ? fmt::str("%.0f", std::min(std::fabs(v), 99999.0)) : "-----"; };

    // ---- DELTA-V group: totals, target and margin
    const Rect dv_frame = Rect::f(x0 - .8 * u, top - .9 * u, left_w - x0 + 1.3 * u, bottom - top + .9 * u);
    if (art) frame(c, dv_frame, "DELTA-V");
    else if (d_.on_vab_panel) d_.on_vab_panel("delta-v", dv_frame);
    Color total_color = theme::VALUE;
    if (!pending && target) total_color = mtotal >= target->delta_v ? theme::GOOD : theme::BAD;
    const std::string target_name = target ? "TARGET " + destination_label(target->key) : "NO TARGET";
    tile(c, art, d_, Rect::f(x0, top, col_w, row_h), "TOTAL \xCE\x94V", "888888",
         {pending ? "-----" : clamp_dv(total), "M/S"}, pending ? theme::WARN : theme::VALUE);
    tile(c, art, d_, Rect::f(x0 + col_w + metric_gap, top, col_w, row_h), "MISSION \xCE\x94V", "888888",
         {pending ? "-----" : clamp_dv(mtotal), "M/S"}, pending ? theme::WARN : total_color);
    const double y2 = top + row_h + metric_gap;
    tile(c, art, d_, Rect::f(x0, y2, col_w, row_h), target_name.c_str(), "888888",
         {target ? clamp_dv(target->delta_v) : "-----", "M/S"}, target ? theme::VALUE : theme::WARN);
    std::string margin = "-----";
    if (!pending && target) margin = (mtotal < target->delta_v ? "-" : "") + clamp_dv(mtotal - target->delta_v);
    tile(c, art, d_, Rect::f(x0 + col_w + metric_gap, y2, col_w, row_h), "MARGIN", "888888", {margin, "M/S"},
         pending || !target ? theme::WARN : total_color);

    // ---- stage stack: one band per stage across the well, the target as a pointer; the calc lamp beside it
    const double sy = y2 + row_h + 2.6 * u;
    const Rect calc = Rect::f(left_w - 9.6 * u, sy - .2 * u, 9.6 * u, 3.6 * u);
    const Rect well = Rect::f(x0, sy, calc.x - 1.2 * u - x0, 3.2 * u);
    if (art) {
        label(c, "STAGE STACK", 1.3f, x0 + .3 * u, sy - 1.9 * u, Anchor::TopLeft, DIM_INK, true, .08f);
        c.rounded_rect(well.inflate(.35 * u, .35 * u), rnd(.4 * u), Color{30, 31, 32});
        c.rect(well, WELL);
        lamp(c, calc, "\xCE\x94V CALC", 1.3f, true, nullptr);
    } else {
        if (pending && !r->empty_craft()) {
            Color amber = L_AMBER;
            lamp(c, calc, "\xCE\x94V CALC", 1.3f, false, &amber);
        }
        // the bands are each stage's share of the mission delta-v, so the target pointer compares like with like
        const double scale = std::max({mtotal, target ? target->delta_v : 0.0, 1.0}) * 1.08;
        if (!pending && mtotal > 0) {
            static const Color bands[] = {seg::GREEN, seg::AMBER, Color{120, 200, 255}, L_WHITE, Color{230, 140, 230}};
            double x = well.x;
            int index = 0;
            for (double share : mission.per_stage) {
                if (share <= 0) continue;
                const double w = well.w * share / scale;
                c.fill_rect(float(x), float(well.y + .4 * u), float(std::max(1.0, w - .25 * u)), float(well.h - .8 * u),
                            mix(bands[index++ % 5], WELL, .25));
                x += w;
            }
        }
        if (target) {
            const float tx = float(well.x + well.w * target->delta_v / scale), t = std::max(2.0f, rnd(.25 * u));
            c.fill_rect(tx - t / 2, float(well.y - .5 * u), t, float(well.h + u), POINTER);
        }
    }

    // ---- stage table: fixed rows of segment windows (static art), lit per stage
    const double ty = sy + well.h + 2.0 * u;
    const float dh = rnd(2.0 * u);
    const seg::Layout stage = seg::layout("88", dh), dv = seg::layout("88888", dh), twr = seg::layout("8888", dh),
                      burn = seg::layout("8888", dh);
    const double gap = 1.3 * u;
    const double cx_stage = x0 + .6 * u, cx_dv = cx_stage + stage.w + gap, cx_vac = cx_dv + dv.w + gap,
                 cx_twr = cx_vac + dv.w + gap, cx_burn = cx_twr + twr.w + gap, cx_ev = cx_burn + burn.w + gap;
    constexpr int ROWS = 4;  // metric windows plus two full-width lines of named staging components
    const double ev_room = left_w - .6 * u - cx_ev, row0 = ty + 2.2 * u;
    const double pitch = std::max(dv.h + .8 * u, (bottom - 1.5 * u - row0) / (ROWS + 1));
    if (art) {
        const char* heads[] = {"STG", "\xCE\x94V M/S", "VAC M/S", "TWR", "BURN S", "FIRES"};
        const double xs[] = {cx_stage, cx_dv, cx_vac, cx_twr, cx_burn, cx_ev};
        for (int i = 0; i < 6; ++i) label(c, heads[i], 1.2f, xs[i], ty, Anchor::TopLeft, DIM_INK, true, .06f);
        for (int i = 0; i < ROWS; ++i) {
            const float y = float(row0 + i * pitch);
            seg::window(c, float(cx_stage), y, stage);
            seg::window(c, float(cx_dv), y, dv);
            seg::window(c, float(cx_vac), y, dv);
            seg::window(c, float(cx_twr), y, twr);
            seg::window(c, float(cx_burn), y, burn);
        }
    }
    if (!art) {
        if (pending) {  // a plate over the empty stage rows, so the state cannot be missed
            const bool empty = r->empty_craft();
            const std::string head = empty ? "EMPTY CRAFT" : "CALCULATING \xCE\x94V",
                              note = empty ? "ADD PARTS TO SEE THE DELTA-V REPORT" : "KSP IS COMPUTING THE STAGE DELTA-V";
            const Rect plate = Rect::f(x0, row0 + .5 * pitch, left_w - x0 - .6 * u, 3 * pitch);
            const float edge = std::max(2.0f, rnd(.2 * u));
            c.rounded_rect(plate, rnd(.6 * u), empty ? DIM_INK : L_AMBER);
            c.rounded_rect(plate.inflate(-edge, -edge), rnd(.5 * u), Color{24, 25, 26});
            label(c, head, std::min(3.2f, fit(c, {head}, 3.2f, plate.w - 2 * u)), plate.centerx(), plate.y + .9 * pitch,
                  Anchor::Center, empty ? INK : L_AMBER, false);
            label(c, note, std::min(1.7f, fit(c, {note}, 1.7f, plate.w - 2 * u)), plate.centerx(), plate.y + 2.1 * pitch,
                  Anchor::Center, empty ? DIM_INK : INK, false);
        } else if (r->stages.empty()) {
            label(c, "NO STAGES", 1.7f, (x0 + left_w) / 2, row0 + ROWS * pitch + .6 * u, Anchor::MidTop, DIM_INK, false);
        }
        const PageSlice stages = page_slice(r->stages.size(), ROWS, s.received);
        for (size_t i = stages.begin; !pending && i < stages.end; ++i) {
            const StageReport& st = r->stages[i];
            const float y = float(row0 + (i - stages.begin) * pitch);
            const TwrLevel level = twr_level(*r, static_cast<size_t>(i), d_.config().vab.twr);
            const Color twr_color = level == TwrLevel::Bad ? seg::RED : level == TwrLevel::Warn ? seg::AMBER : seg::GREEN;
            seg::draw(c, float(cx_stage), y, stage, fmt::str("%02d", std::min(st.stage, 99)), seg::GREEN);
            seg::draw(c, float(cx_dv), y, dv, clamp_dv(st.delta_v), seg::GREEN);
            seg::draw(c, float(cx_vac), y, dv, clamp_dv(st.delta_v_vac), seg::GREEN);
            seg::draw(c, float(cx_twr), y, twr, std::isfinite(st.twr) ? fmt::str("%.2f", std::min(st.twr, 99.99)) : "----",
                      twr_color);
            seg::draw(c, float(cx_burn), y, burn, std::isfinite(st.burn) ? fmt::str("%.0f", std::min(st.burn, 9999.0)) : "----",
                      seg::GREEN);
            const std::string ev = events_text(st);
            if (!ev.empty() && ev_room > u)
                label(c, ev, std::min(1.4f, fit(c, {ev}, 1.4f, ev_room)), cx_ev, y + dv.h / 2, Anchor::MidLeft, INK, true, .06f);
            const auto details = stage_detail_lines(st);
            const PageSlice parts = page_slice(details.size(), 2, s.received / stages.pages);
            const double room = left_w - x0 - .6 * u;
            for (size_t j = parts.begin; j < parts.end; ++j)
                label(c, details[j], std::min(1.35f, fit(c, {details[j]}, 1.35f, room)), x0,
                      y + (3.5 + 1.8 * (j - parts.begin)) * u, Anchor::TopLeft, INK, false);
        }
        if (!pending)
            label(c, fmt::str("STAGES PAGE %zu / %zu  /  AUTO 8s", stages.page + 1, stages.pages), 1.5f, cx_stage,
                  row0 + ROWS * pitch + .6 * u, Anchor::TopLeft, DIM_INK);
    }

    // ---- DESTINATIONS: lamps, green reachable, red not; the configured target outlined
    constexpr int COLS = 3;
    const int n = std::min(static_cast<int>(dests.size()), MAX_DESTINATIONS);
    const double lamp_gap = .9 * u, lamp_w = (rw - 2.4 * u - (COLS - 1) * lamp_gap) / COLS, lamp_h = 4.0 * u;
    const double dest_top = top + 1.4 * u, dest_rows = std::max(1, (MAX_DESTINATIONS + COLS - 1) / COLS);
    const double notes_y = dest_top + dest_rows * (lamp_h + lamp_gap) + .2 * u;  // two live lines under the lamps
    const Rect dest_frame = Rect::f(x1 - .8 * u, top - .9 * u, rw + 1.3 * u, 2.3 * u + dest_rows * (lamp_h + lamp_gap) + 5.0 * u);
    std::vector<std::string> legends;
    for (int i = 0; i < n; ++i) legends.push_back(destination_label(dests[i].key) + "  " + fmt::str("%.0f", dests[i].delta_v));
    const float legend_units = legends.empty() ? 1.4f : std::min(1.4f, fit(c, legends, 1.4f, lamp_w - 1.8 * u));
    auto lamp_rect = [&](int i) {
        return Rect::f(x1 + .4 * u + (i % COLS) * (lamp_w + lamp_gap), dest_top + (i / COLS) * (lamp_h + lamp_gap), lamp_w, lamp_h);
    };
    if (art) {
        frame(c, dest_frame, "DESTINATIONS");
        if (n == 0) label(c, "NO DESTINATIONS IN CONFIG.TOML", 1.5f, dest_frame.centerx(), dest_frame.centery(), Anchor::Center);
        for (int i = 0; i < n; ++i) {
            const Rect lr = lamp_rect(i);
            if (target && dests[i].key == target->key)  // engraved white ring around the selected destination
                c.rounded_rect(lr.inflate(.75 * u, .75 * u), rnd(.6 * u), INK);
            lamp(c, lr, legends[i], legend_units, true, nullptr);
        }
    } else {
        if (d_.on_vab_panel) d_.on_vab_panel("destinations", dest_frame);
        for (int i = 0; i < n && !pending; ++i) {
            Color lit = mtotal >= dests[i].delta_v ? L_GREEN : L_RED;
            lamp(c, lamp_rect(i), legends[i], legend_units, false, &lit);
        }
        const double room = rw - .8 * u;
        const std::string basis = "MISSION \xCE\x94V " + (pending ? std::string("-----") : clamp_dv(mtotal)) + " M/S  \xC2\xB7  " +
                                  mission_basis(*r, mission);
        label(c, basis, std::min(1.4f, fit(c, {basis}, 1.4f, room)), x1 + .4 * u, notes_y, Anchor::TopLeft,
              mission.kerbin_asl ? INK : L_AMBER, false);
        const std::string why = target ? "TARGET " + destination_label(target->key) +
                                             (choice.from_app ? "  \xC2\xB7  FROM KSP \xCE\x94V APP" : "  \xC2\xB7  FROM CONFIG")
                                       : std::string("NO TARGET  \xC2\xB7  SET [VAB] TARGET IN CONFIG.TOML");
        label(c, why, std::min(1.4f, fit(c, {why}, 1.4f, room)), x1 + .4 * u, notes_y + 2.2 * u, Anchor::TopLeft, DIM_INK, false);
    }

    // ---- VEHICLE: segment readouts
    const double vy = dest_frame.bottom() + 2.2 * u;
    const double vcol = (rw - metric_gap) / 2;
    const Rect veh_frame = Rect::f(x1 - .8 * u, vy - .9 * u, rw + 1.3 * u, 4 * row_h + 3 * metric_gap + 1.8 * u);
    if (art) frame(c, veh_frame, "VEHICLE");
    else if (d_.on_vab_panel) d_.on_vab_panel("vehicle", veh_frame);
    auto mass_text = [](const std::optional<double>& v) {
        if (!v) return std::string("-----");
        return fmt::str(*v >= 1000 ? "%.0f" : *v >= 100 ? "%.1f" : "%.2f", std::min(*v, 999999.0));
    };
    auto whole = [](const std::optional<double>& v) { return v ? fmt::str("%.0f", std::min(std::fabs(*v), 99999999.0)) : "-------"; };
    auto count = [](const std::optional<int>& v, int width) { return v ? fmt::str("%*d", width, *v) : std::string(width, '-'); };
    const Color funds_color = r && r->funds && r->cost ? (*r->funds >= *r->cost ? theme::GOOD : theme::BAD) : theme::VALUE;
    std::string size = "---- ----";
    if (r && r->height && r->width) size = fmt::str("%5.1f %5.1f", std::min(*r->height, 999.9), std::min(*r->width, 999.9));
    struct T {
        const char* name;
        const char* pattern;
        Reading value;
        Color color;
    };
    const T tiles[8] = {
        {"MASS", "888888", {r ? mass_text(r->mass) : "", "T"}, theme::VALUE},
        {"DRY MASS", "888888", {r ? mass_text(r->dry) : "", "T"}, theme::VALUE},
        {"COST", "88888888", {r ? whole(r->cost) : "", ""}, theme::VALUE},
        {"FUNDS", "88888888", {r ? whole(r->funds) : "", ""}, funds_color},
        {"PARTS", "8888", {r ? count(r->parts, 4) : "", ""}, theme::VALUE},
        {"CREW  SEATS", "88 88", {r ? count(r->crew, 2) + " " + count(r->seats, 2) : "", ""}, theme::VALUE},
        {"HEIGHT  WIDTH", "8888 8888", {size, "M"}, theme::VALUE},
        {"ENGINES", "888", {r ? count(r->engines, 3) : "", ""}, theme::VALUE},
    };
    for (int i = 0; i < 8; ++i)
        tile(c, art, d_, Rect::f(x1 + (i % 2) * (vcol + metric_gap), vy + (i / 2) * (row_h + metric_gap), vcol, row_h),
             tiles[i].name, tiles[i].pattern, tiles[i].value, tiles[i].color);
    if (!art)
        label(c, fmt::str("SAS NEXT / RCS PREVIOUS  \xC2\xB7  REVISION %u", r->revision), 1.5f, x1, veh_frame.bottom() + 1.4 * u,
              Anchor::TopLeft, DIM_INK);
}

void ApolloSkin::facility(Canvas& c, bool art, const Snapshot& s) {
    const double u = c.u;
    const View* view = find_view(s.scene);
    if (!view) return;
    if (!art) return;  // everything on this screen is static: it lives in the cached panel
    label(c, upper(view->title), 5.0f, 4 * u, 13.5 * u);
    frame(c, Rect::f(3 * u, 22 * u, c.w - 6 * u, 62 * u), "PREPARATION CHECKLIST");
    for (int index = 0; index < 3; ++index) {
        const Card& card = view->cards[index];
        Rect plate = Rect::f(5 * u, (25.5 + index * 19) * u, c.w - 10 * u, 16 * u);
        c.rounded_rect(plate.inflate(.4 * u, .4 * u), rnd(.7 * u), Color{84, 86, 87});
        c.rounded_rect(plate, rnd(.5 * u), PLATE);
        const double sr = .38 * u;
        screw(c, plate.x + .9 * u, plate.y + .9 * u, sr, .4), screw(c, plate.right() - .9 * u, plate.y + .9 * u, sr, 1.9);
        screw(c, plate.x + .9 * u, plate.bottom() - .9 * u, sr, 2.7);
        screw(c, plate.right() - .9 * u, plate.bottom() - .9 * u, sr, 1.1);
        Rect num = Rect::f(plate.x + 2.4 * u, plate.y + 3 * u, 8 * u, 10 * u);
        lamp(c, num, std::to_string(index + 1), 4.0f, true, nullptr);
        double tx = num.right() + 3 * u, room = plate.right() - 2.4 * u - tx;
        label(c, upper(card.label), 1.8f, tx, plate.y + 2.2 * u, Anchor::TopLeft, L_AMBER, false);
        label(c, card.heading, fit(c, {card.heading}, 3.6f, room), tx, plate.y + 5.3 * u, Anchor::TopLeft, INK, false, .04f);
        c.text(card.hint, std::min(2.6f, static_cast<float>(2.6 * std::min(1.0, room / std::max(1, c.text_width(c.font(2.6f), card.hint))))),
               DIM_INK, tx, plate.y + 10.6 * u);
    }
    label(c, "SCENE CONTEXT LIVE  \xC2\xB7  FLIGHT TELEMETRY RESUMES WHEN YOU LAUNCH", 1.7f, 4 * u, 87.5 * u);
}

void ApolloSkin::waiting(Canvas& c, bool art, const Snapshot& s, double now, bool no_vessel) {
    const double u = c.u;
    const double cx = c.w / 2.0, sy = 34 * u, sr = 18 * u;
    Rect annunciator = Rect::f(cx - 24 * u, 57 * u, 48 * u, 7.5 * u);
    const char* legend = no_vessel ? "NOT IN FLIGHT" : "WAITING FOR KSP";
    if (art) {
        frame(c, Rect::f(3 * u, 13 * u, c.w - 6 * u, 72 * u), "TELEMETRY LINK");
        c.circle(float(cx), float(sy), float(sr + 1.6 * u), Color{82, 84, 85});
        c.circle(float(cx), float(sy), float(sr + 1.2 * u), BEZEL);
        c.arc(float(cx), float(sy), float(sr + 1.1 * u), 40 * PI / 180, 140 * PI / 180, std::max(1.0f, rnd(.2 * u)), BEZEL_HI);
        c.circle(float(cx), float(sy), float(sr), WELL);
        for (int k = 0; k < 4; ++k) {
            double a = PI / 4 + k * PI / 2;
            screw(c, cx + (sr + 3.2 * u) * std::cos(a), sy + (sr + 3.2 * u) * std::sin(a), .55 * u, a);
        }
        // reticle
        c.ring(float(cx), float(sy), float(11 * u), std::max(1.0f, rnd(.2 * u)), Color{26, 40, 34});
        c.fill_rect(float(cx - sr + u), float(sy), float(2 * sr - 2 * u), 1, Color{26, 40, 34});
        c.fill_rect(float(cx), float(sy - sr + u), 1, float(2 * sr - 2 * u), Color{26, 40, 34});
        c.circle(float(cx), float(sy), float(6 * u), mix(theme::BODY, WELL, .2));
        lamp(c, annunciator, legend, 3.0f, true, nullptr);
        return;
    }
    double ang = now * 1.2;
    c.circle(float(cx + 11 * u * std::cos(ang)), float(sy + 11 * u * std::sin(ang)), float(1.2 * u), seg::GREEN);
    // the annunciator blinks at 1 Hz, as a master caution
    if (std::fmod(now, 1.0) < .6) {
        Color lit = no_vessel ? L_WHITE : L_AMBER;
        lamp(c, annunciator, legend, 3.0f, false, &lit);
    }
    std::string hint = no_vessel ? "Launch or switch to a vessel to see its orbit"
                                 : "Start KSP and the kRPC server (port 50000 on the PC's LAN address)";
    label(c, upper(hint), fit(c, {upper(hint)}, 2.2f, c.w - 10 * u), cx, 67 * u, Anchor::MidTop);
    if (!s.detail.empty()) {
        std::string detail = s.detail.substr(0, 110);
        c.text(detail, 2.4f, DIM_INK, cx, 72 * u, Anchor::MidTop);
    }
    c.text(net_.line(), 2.8f, INK, cx, c.h - 9.5 * u, Anchor::MidTop);
    c.text(s.server.empty() ? server_text_ : s.server, 2.4f, DIM_INK, cx, c.h - 5 * u, Anchor::MidTop);
}

}  // namespace ksphmi
