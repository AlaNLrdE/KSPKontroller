#include "navball.hpp"

#include <chrono>
#include <cmath>
#include <stdexcept>

#include "fmt.hpp"
#include "navmath.hpp"
#include "theme.hpp"
#include "steampunk.hpp"

namespace ksphmi {

using namespace theme;

namespace {

long long py_round(double v) { return fmt::round_half_even(v); }
float rnd(double v) { return static_cast<float>(py_round(v)); }
long long pymod(long long a, long long b) { return ((a % b) + b) % b; }

std::string title_case(const std::string& s) {  // Python str.title() for one word
    std::string out = s;
    bool start = true;
    for (char& ch : out) {
        if (std::isalpha(static_cast<unsigned char>(ch))) {
            ch = static_cast<char>(start ? std::toupper(static_cast<unsigned char>(ch))
                                         : std::tolower(static_cast<unsigned char>(ch)));
            start = false;
        } else {
            start = true;
        }
    }
    return out;
}

std::string sprite_key(const std::string& text, bool heading, bool sky) {
    return text + (heading ? "|b" : "|n") + (sky ? "|s" : "|g");
}

}  // namespace

Navball::Navball() {
    for (int pitch = -90; pitch < 90; pitch += 10)
        for (int heading = 0; heading < 360; heading += 10) {
            Cell cell;
            cell.v[0] = globe(pitch, heading);
            cell.v[1] = globe(pitch + 10, heading);
            cell.v[2] = globe(pitch + 10, heading + 10);
            cell.v[3] = globe(pitch, heading + 10);
            cell.color = pitch >= 0 ? SKY : GROUND;
            cells_.push_back(cell);
        }
    Line equator{{}, 2};
    for (int h = 0; h <= 360; h += 3) equator.points.push_back(globe(0, h));
    lines_.push_back(equator);
    // Stock-style broken parallels distinguish the +/-45 degree bands from the local five-degree ladder ticks.
    // Gaps fall on meridians, leaving space for the bold compass bearings printed on these bands.
    for (int pitch : {-45, 45})
        for (int heading = 0; heading < 360; heading += 45) {
            Line l{{}, 1};
            for (int offset = 5; offset < 41; offset += 2) l.points.push_back(globe(pitch, heading + offset));
            lines_.push_back(l);
        }
    // Local pitch ladders: long numbered ten-degree ticks and short five-degree ticks.
    for (int pitch = -85; pitch < 86; pitch += 5) {
        if (pitch == 0) continue;
        double half_width = pitch % 10 == 0 ? 4.5 : 2.5;
        for (int heading = 0; heading < 360; heading += 45) {
            Line l{{}, pitch % 10 == 0 ? 2 : 1};
            for (int i = -4; i < 5; ++i) l.points.push_back(globe(pitch, heading + half_width * i / 4));
            lines_.push_back(l);
        }
    }
    for (int heading = 0; heading < 360; heading += 45) {
        Line l{{}, 1};
        for (int p = -90; p < 91; p += 2) l.points.push_back(globe(p, heading));
        lines_.push_back(l);
    }
    // Separate heading and pitch labels: a meridian's number is a compass bearing, while the flanking numbers
    // measure degrees above/below the horizon. Horizon bearings are lifted 5 degrees into the blue hemisphere so
    // their backgrounds never straddle the brown ground, including when inverted.
    for (int p : {5, 45, -45})
        for (int h = 0; h < 360; h += 45) labels_.push_back({std::to_string(h), double(p), double(h), true});
    for (int p = -80; p < 81; p += 10) {
        if (p == 0) continue;
        for (int h = 0; h < 360; h += 45)
            for (int offset : {-7, 7})
                labels_.push_back({std::to_string(p), double(p), double(pymod(h + offset, 360)), false});
    }
}

Navball::~Navball() {
    if (label_atlas_) SDL_DestroyTexture(label_atlas_);
    if (lighting_) SDL_DestroyTexture(lighting_);
}

void Navball::build_labels(Canvas& c) {
    // Knock out the grid under the numerals, like the stock texture: each label is white text on its
    // hemisphere's colour, composed once into one atlas.
    int px = std::max(8, static_cast<int>((flight_deck ? 3.0 : 2.3) * c.u));
    if (label_atlas_ && label_px_ == px && renderer_ == c.renderer() && label_font_ == c.font_path()) return;
    if (label_atlas_) SDL_DestroyTexture(label_atlas_);
    label_atlas_ = nullptr;
    sprites_.clear();
    renderer_ = c.renderer();
    label_px_ = px;
    label_font_ = c.font_path();
    Font& normal = c.font_px(px, false);
    Font& bold = c.font_px(px, true);
    struct Item {
        std::string key;
        SDL_Surface* surface;
    };
    std::vector<Item> items;
    for (const Label& l : labels_) {
        bool sky = l.pitch >= 0;
        std::string key = sprite_key(l.text, l.is_heading, sky);
        bool seen = false;
        for (const auto& it : items) seen = seen || it.key == key;
        if (seen) continue;
        TTF_Font* f = (l.is_heading ? bold : normal).ttf();
        SDL_Surface* glyph = TTF_RenderUTF8_Blended(f, l.text.c_str(), SDL_Color{WHITE.r, WHITE.g, WHITE.b, 255});
        if (!glyph) continue;
        SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, glyph->w, TTF_FontHeight(f), 32, SDL_PIXELFORMAT_ARGB8888);
        Color bg = sky ? SKY : GROUND;
        SDL_FillRect(s, nullptr, SDL_MapRGBA(s->format, bg.r, bg.g, bg.b, 255));
        SDL_SetSurfaceBlendMode(glyph, SDL_BLENDMODE_BLEND);
        SDL_BlitSurface(glyph, nullptr, s, nullptr);
        SDL_FreeSurface(glyph);
        items.push_back({key, s});
    }
    int width = 512, x = 1, y = 1, shelf = 0;
    std::vector<SDL_Rect> places;
    for (const auto& it : items) {
        if (x + it.surface->w + 1 > width) x = 1, y += shelf + 1, shelf = 0;
        places.push_back({x, y, it.surface->w, it.surface->h});
        x += it.surface->w + 1;
        shelf = std::max(shelf, it.surface->h);
    }
    atlas_w_ = width;
    atlas_h_ = y + shelf + 1;
    SDL_Surface* atlas = SDL_CreateRGBSurfaceWithFormat(0, atlas_w_, atlas_h_, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_FillRect(atlas, nullptr, 0);
    for (size_t i = 0; i < items.size(); ++i) {
        SDL_SetSurfaceBlendMode(items[i].surface, SDL_BLENDMODE_NONE);
        SDL_Rect dst = places[i];
        SDL_BlitSurface(items[i].surface, nullptr, atlas, &dst);
        sprites_[items[i].key] = {places[i]};
        SDL_FreeSurface(items[i].surface);
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    label_atlas_ = SDL_CreateTextureFromSurface(c.renderer(), atlas);
    SDL_FreeSurface(atlas);
    if (label_atlas_) {
        SDL_SetTextureBlendMode(label_atlas_, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(label_atlas_, SDL_ScaleModeLinear);
    }
}

void Navball::build_lighting(Canvas& c, int size, double radius) {
    // Fixed screen-space light: sphere normals give volume while the attitude rotates underneath. Computed once
    // per size (shade and highlight merged into one straight-alpha layer), never in the frame loop.
    if (lighting_ && lighting_size_ == size && lighting_radius_ == radius && renderer_ == c.renderer()) return;
    if (lighting_) SDL_DestroyTexture(lighting_);
    lighting_size_ = size;
    lighting_radius_ = radius;
    std::vector<uint32_t> pixels(static_cast<size_t>(size) * size, 0);
    for (int iy = 0; iy < size; ++iy) {
        double y = (iy + .5 - size / 2.0) / radius;
        for (int ix = 0; ix < size; ++ix) {
            double x = (ix + .5 - size / 2.0) / radius;
            double squared = x * x + y * y;
            if (squared >= 1) continue;
            double z = std::sqrt(1 - squared);
            double diffuse = std::max(0.0, -.35 * x - .42 * y + .837 * z);
            double brightness = std::max(.26, .40 + .60 * diffuse - .13 * (1 - z) * (1 - z));
            double shade = py_round(255 * (1 - brightness)) / 255.0;
            double reflection = std::pow(std::max(0.0, -.28 * x - .37 * y + .885 * z), 28);
            double shine = py_round(38 * reflection) / 255.0;
            double a = 1 - (1 - shade) * (1 - shine);
            if (a <= 0) continue;
            auto ch = [&](double v) { return static_cast<uint32_t>(std::lround(std::min(255.0, v * shine / a))); };
            uint32_t alpha = static_cast<uint32_t>(std::lround(a * 255));
            pixels[static_cast<size_t>(iy) * size + ix] = alpha << 24 | ch(230) << 16 | ch(242) << 8 | ch(255);
        }
    }
    lighting_ = SDL_CreateTexture(c.renderer(), SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, size, size);
    if (!lighting_) return;
    SDL_UpdateTexture(lighting_, nullptr, pixels.data(), size * 4);
    SDL_SetTextureBlendMode(lighting_, SDL_BLENDMODE_BLEND);
}

void Navball::symbol(Canvas& c, const std::string& kind, Color color, float x, float y, float s) {
    float w = std::max(1.0f, rnd(s * 0.17));
    auto seg = [&](double a, double r0, double r1) {
        c.line({x + static_cast<float>(r0 * std::cos(a)), y + static_cast<float>(r0 * std::sin(a))},
               {x + static_cast<float>(r1 * std::cos(a)), y + static_cast<float>(r1 * std::sin(a))}, w, color);
    };
    if (kind == "prograde" || kind == "retrograde") {
        c.ring(x, y, s * .58f, w, color);
        if (kind == "retrograde") {
            for (int dx : {-1, 1})
                c.line({x - dx * s * .42f, y - s * .42f}, {x + dx * s * .42f, y + s * .42f}, w, color);
        } else {
            c.circle(x, y, std::max(1.0f, s * .13f), color);
        }
        for (int angle : {-90, 30, 150}) seg(radians(angle), s * .6, s);
    } else if (kind == "normal" || kind == "anti_normal") {
        std::vector<Pt> p = {{x, y - s}, {x + s * .87f, y + s * .5f}, {x - s * .87f, y + s * .5f}};
        if (kind == "anti_normal")
            for (Pt& q : p) q.y = 2 * y - q.y;
        c.polyline(p, true, w, color);
        c.circle(x, y, std::max(1.0f, s * .15f), color);
    } else if (kind == "radial" || kind == "anti_radial") {
        c.ring(x, y, s * .65f, w, color);
        for (int angle = 0; angle < 360; angle += 90) seg(radians(angle), s * .7, s * 1.1);
        if (kind == "anti_radial") {
            c.line({x - s * .4f, y - s * .4f}, {x + s * .4f, y + s * .4f}, w, color);
            c.line({x - s * .4f, y + s * .4f}, {x + s * .4f, y - s * .4f}, w, color);
        }
    } else if (kind == "target" || kind == "anti_target") {
        c.ring(x, y, s * .55f, w, color);
        c.line({x - s, y}, {x + s, y}, w, color);
        c.line({x, y - s}, {x, y + s}, w, color);
        if (kind == "anti_target") c.line({x - s * .7f, y - s * .7f}, {x + s * .7f, y + s * .7f}, w, color);
    } else if (kind == "maneuver") {
        c.ring(x, y, s * .6f, w, color);
        for (int a = 0; a < 360; a += 120) seg(radians(a), s * .6, s * 1.15);
    }
}

Quat Navball::smooth(const Quat& shown, const Quat& target, double dt) {
    if (!(dt > 0) || angle_between(shown, target) > SNAP_DEG) return target;
    return slerp(shown, target, 1 - std::exp(-dt / SMOOTHING_S));
}

Navball::Globe Navball::globe_geometry(const Canvas& c, const Rect& rect, bool classic, bool flight_deck) {
    Globe g{rect.centerx() - 1.8 * c.u, rect.y + rect.h * .46, std::min(rect.h * .31, rect.w * .31)};
    if (flight_deck) return Globe{double(rect.centerx()), rect.y + rect.h * .46, std::min(rect.h * .335, rect.w * .28)};
    if (!classic) return g;
    // Above the globe: the speed box (5.4u) and .3u; below it: the HDG caption (.9u + about 3.4u), a 1.4u gap and
    // the SAS MODE row 8.5u above the bottom. A short panel shrinks the globe until both fit.
    const double above = 5.7 * c.u, below = (.9 + 3.4 + 1.4 + 8.5) * c.u;
    g.r = std::max(std::min(g.r, (rect.h - above - below) / 2), 4 * c.u);
    g.cy = std::clamp(g.cy, rect.y + g.r + above, rect.bottom() - g.r - below);
    return g;
}

const std::vector<std::pair<const char*, const char*>>& Navball::sas_modes() {
    static const std::vector<std::pair<const char*, const char*>> modes = {
        {"stability_assist", "HOLD"}, {"prograde", "PRO"}, {"retrograde", "RET"}, {"normal", "N+"},
        {"anti_normal", "N-"},        {"radial", "R+"},    {"anti_radial", "R-"}, {"target", "TGT"},
        {"anti_target", "ANTI"},      {"maneuver", "NODE"}};
    return modes;
}

void Navball::draw(Canvas& c, const Rect& rect, const std::optional<Navigation>& nav) {
    const double u = c.u;
    if (chrome) {
        panel(c, rect);
        c.text("NAVBALL", 2.6, {122, 140, 164}, rect.x + 1.8 * u, rect.y + 1.8 * u);
    }
    if (!nav) {
        animating_ = false;
        last_nav_.reset();
        if (chrome) c.text("Attitude unavailable", 3.4, WHITE, rect.centerx(), rect.centery(), Anchor::Center);
        return;
    }
    const Navigation& n = *nav;
    advance(n.rotation);
    if (chrome)
        c.text(fmt::str("P %+.1f\xC2\xB0   R %+.1f\xC2\xB0", n.pitch, n.roll), 2, INK, rect.right() - 1.8 * u,
               rect.y + 1.8 * u, Anchor::TopRight);
    last_nav_ = n, last_rect_ = rect, last_chrome_ = chrome;
    last_font_ = c.font_path();
    if (before_globe) before_globe(globe_square(c, rect, chrome, flight_deck));
    attitude_layer(c, rect, n);
}

void Navball::advance(const Quat& target) {
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (hold) {
        shown_ = *hold, shown_at_ = now, animating_ = true;
        return;
    }
    shown_ = shown_at_ < 0 ? target : smooth(shown_, target, now - shown_at_);
    shown_at_ = now;
    animating_ = angle_between(shown_, target) > 0.05;
}

Rect Navball::globe_square(const Canvas& c, const Rect& rect, bool classic, bool flight_deck) {
    const Globe g = globe_geometry(c, rect, classic, flight_deck);
    const int size = static_cast<int>(std::ceil(2 * g.r + 4));
    const double origin = size / 2.0;
    return Rect(static_cast<int>(g.cx - origin), static_cast<int>(g.cy - origin), size, size);
}

bool Navball::redraw(Canvas& c) {
    if (!last_nav_) return false;
    TypefaceScope face(c, last_font_);
    advance(last_nav_->rotation);
    const bool keep = chrome;
    chrome = last_chrome_;
    attitude_layer(c, last_rect_, *last_nav_);
    chrome = keep;
    return true;
}

void Navball::attitude_layer(Canvas& c, const Rect& rect, const Navigation& n) {
    const double u = c.u;
    const Globe g = globe_geometry(c, rect, chrome, flight_deck);
    const double radius = g.r;
    const double cx = g.cx, cy = g.cy;
    Basis axes;
    try {
        axes = basis(shown_);
    } catch (const std::invalid_argument&) {  // a zero quaternion: show the globe level rather than nothing
        axes = basis(Quat{0, 0, 0, 1});
    }
    const double r = radius;
    // The ball is composed at an integer offset (whole-pixel blit of its surface).
    const Rect square = globe_square(c, rect, chrome, flight_deck);
    const int size = square.w, bx = square.x, by = square.y;
    const double origin = size / 2.0;
    auto point = [&](const Vec3& v) { return Pt{static_cast<float>(bx + origin + v.x * r), static_cast<float>(by + origin + v.y * r)}; };

    const Rect previous_clip = c.clip();
    c.set_clip(previous_clip.clip(Rect(bx, by, size, size)));
    std::vector<Vec3> projected_cell(4);
    std::vector<Pt> pts;
    for (const Cell& cell : cells_) {
        for (int i = 0; i < 4; ++i) projected_cell[i] = projected(cell.v[i], axes);
        auto vertices = front_polygon(projected_cell);
        if (vertices.size() < 3) continue;
        pts.clear();
        for (const Vec3& v : vertices) pts.push_back(point(v));
        c.polygon(pts, cell.color, false);
    }
    auto stroke_line = [&](const std::vector<Pt>& segment, int width) {
        if (segment.size() < 2) return;
        float stroke = std::max(1.0f, rnd(width * .13 * u));
        c.polyline(segment, false, stroke, INK);
    };
    std::vector<Pt> segment;
    const double heading = degrees_of(std::atan2(axes.forward.z, axes.forward.y));
    if (flight_deck) {
        // Proposal B uses one continuous local pitch ladder, rather than repeated compass-number columns.
        auto curve = [&](const std::vector<Vec3>& vectors, int width, Color color) {
            std::vector<Pt> pts;
            for (const auto& vector : vectors) {
                Vec3 p = projected(vector, axes);
                if (p.z > .02) pts.push_back(point(p));
                else { if (pts.size() > 1) c.polyline(pts, false, std::max(1.0f, float(width*.13*u)), color); pts.clear(); }
            }
            if (pts.size() > 1) c.polyline(pts, false, std::max(1.0f, float(width*.13*u)), color);
        };
        for (int offset : {-60,-30,0,30,60}) {
            std::vector<Vec3> meridian;
            for (int pitch=-90; pitch<=90; pitch+=3) meridian.push_back(globe(pitch,heading+offset));
            curve(meridian,1,Color{115,164,174});
        }
        for (int pitch=-80; pitch<=80; pitch+=5) {
            const int span=pitch==0?90:pitch%10==0?17:10;
            std::vector<Vec3> rung;
            for (int offset=-span;offset<=span;offset+=1) rung.push_back(globe(pitch,heading+offset));
            curve(rung,pitch%10==0?2:1,INK);
        }
    } else {
    for (const Line& line : lines_) {
        segment.clear();
        for (const Vec3& vector : line.points) {
            Vec3 v = projected(vector, axes);
            if (v.z >= .005) {
                segment.push_back(point(v));
            } else {
                stroke_line(segment, line.width);
                segment.clear();
            }
        }
        stroke_line(segment, line.width);
    }
    }
    build_labels(c);
    if (label_atlas_) {
        std::vector<SDL_Vertex> verts;
        std::vector<int> idx;
        std::vector<Label> local_labels;
        if (flight_deck) {
            for (int pitch=-80;pitch<=80;pitch+=10) if (pitch)
                for (int side : {-1,1}) local_labels.push_back({std::to_string(pitch),double(pitch),heading+side*22,false});
        }
        for (const Label& l : flight_deck ? local_labels : labels_) {
            auto projection = label_projection(l.pitch, l.heading, axes);
            if (!projection) continue;
            auto it = sprites_.find(sprite_key(l.text, l.is_heading, l.pitch >= 0));
            if (it == sprites_.end()) continue;
            const SDL_Rect& src = it->second.src;
            // Foreshorten the lettering with the globe, rather than hide colliding rectangles as the ship rolls.
            double iw = std::max(1.0, static_cast<double>(py_round(src.w * projection->width)));
            double ih = std::max(1.0, static_cast<double>(py_round(src.h * projection->height)));
            Pt centre = point(projection->v);
            double a = radians(projection->angle), ca = std::cos(a), sa = std::sin(a);
            const double corners[4][2] = {{-iw / 2, -ih / 2}, {iw / 2, -ih / 2}, {iw / 2, ih / 2}, {-iw / 2, ih / 2}};
            const float uv[4][2] = {{float(src.x), float(src.y)},
                                    {float(src.x + src.w), float(src.y)},
                                    {float(src.x + src.w), float(src.y + src.h)},
                                    {float(src.x), float(src.y + src.h)}};
            int base = static_cast<int>(verts.size());
            auto alpha = static_cast<uint8_t>(projection->alpha);
            for (int k = 0; k < 4; ++k) {
                SDL_Vertex v;
                v.position = {static_cast<float>(centre.x + corners[k][0] * ca - corners[k][1] * sa),
                              static_cast<float>(centre.y + corners[k][0] * sa + corners[k][1] * ca)};
                v.color = {255, 255, 255, alpha};
                v.tex_coord = {uv[k][0] / atlas_w_, uv[k][1] / atlas_h_};
                verts.push_back(v);
            }
            for (int k : {0, 1, 2, 0, 2, 3}) idx.push_back(base + k);
        }
        if (!idx.empty())
            c.triangles(label_atlas_, verts.data(), static_cast<int>(verts.size()), idx.data(), static_cast<int>(idx.size()));
    }
    build_lighting(c, size, r);
    if (lighting_) c.image(lighting_, static_cast<float>(bx), static_cast<float>(by), size, size);
    c.set_clip(previous_clip);

    const float fx = static_cast<float>(cx), fy = static_cast<float>(cy), fr = static_cast<float>(r);
    const float fu = static_cast<float>(u);
    if (chrome) {
        c.ring(fx, fy, fr + 1.7f * fu, std::max(2.0f, rnd(2.6 * u)), {91, 104, 114});
        // The arc rect is 2 * (r + 1.7u) wide, truncated to whole pixels.
        float bezel = static_cast<int>(2 * (r + 1.7 * u)) / 2.0f;
        c.arc(fx, fy, bezel, PI / 4, 5 * PI / 4, std::max(1.0f, rnd(.7 * u)), {151, 164, 171});
        c.arc(fx, fy, bezel, 5 * PI / 4, 9 * PI / 4, std::max(1.0f, rnd(.7 * u)), {48, 57, 64});
    }
    c.ring(fx, fy, fr + .25f * fu, std::max(1.0f, rnd(.25 * u)), {181, 190, 194});
    // Roll scale and fixed top index: stationary while the globe turns.
    if (!flight_deck) for (int deg = -60; deg <= 60; deg += 10) {
        double a = radians(deg - 90);
        double outer = r + 2.6 * u, inner = outer - (deg % 30 == 0 ? .9 : .5) * u;
        c.line({static_cast<float>(cx + inner * std::cos(a)), static_cast<float>(cy + inner * std::sin(a))},
               {static_cast<float>(cx + outer * std::cos(a)), static_cast<float>(cy + outer * std::sin(a))},
               std::max(1.0f, rnd(.15 * u)), INK);
    }
    c.polygon({{fx, fy - fr - 1.5f * fu}, {fx - .8f * fu, fy - fr - 3 * fu}, {fx + .8f * fu, fy - fr - 3 * fu}}, flight_deck ? WHITE : GOLD);

    const struct {
        const char* kind;
        const char* opposite;
        const std::optional<Vec3>* vector;
        Color color;
    } markers[] = {{"prograde", "retrograde", &n.prograde, GOLD},
                   {"normal", "anti_normal", &n.normal, MAGENTA},
                   {"radial", "anti_radial", &n.radial, CYAN},
                   {"target", "anti_target", &n.target, MAGENTA},
                   {"maneuver", nullptr, &n.maneuver, {65, 140, 255}}};
    for (const auto& m : markers) {
        if (!*m.vector) continue;
        auto vector = unit(**m.vector);
        if (!vector) continue;
        std::vector<std::pair<const char*, Vec3>> versions = {{m.kind, *vector}};
        if (m.opposite) versions.push_back({m.opposite, -*vector});
        for (const auto& [kind, direction] : versions) {
            Vec3 p = projected(direction, axes);
            if (p.z < .03) continue;
            // Keep the entire glyph inside the rim, even near the limb.
            double marker_size = MARKER_SIZE * u;
            double limit = std::min(.91, 1 - 1.2 * marker_size / r);
            double length = std::hypot(p.x, p.y);
            double x = p.x, y = p.y;
            if (length > limit) x = x * limit / length, y = y * limit / length;
            symbol(c, kind, m.color, static_cast<float>(cx + x * r), static_cast<float>(cy + y * r),
                   static_cast<float>(marker_size));
        }
    }
    // Fixed stock-style gold aircraft / boresight symbol with a dark outline and a centre dot.
    std::vector<Pt> wings = {{fx - 5 * fu, fy},         {fx - 2.3f * fu, fy}, {fx - .8f * fu, fy + 1.2f * fu},
                             {fx + .8f * fu, fy + 1.2f * fu}, {fx + 2.3f * fu, fy}, {fx + 5 * fu, fy}};
    c.polyline(wings, false, std::max(3.0f, rnd(.75 * u)), {44, 30, 9});
    c.polyline(wings, false, std::max(2.0f, rnd(.35 * u)), GOLD);
    c.circle(fx, fy, std::max(2.0f, .55f * fu), {44, 30, 9});
    c.circle(fx, fy, std::max(1.0f, .3f * fu), GOLD);
    if (!chrome) return;

    // Speed mode and heading displays, plus the stock RCS/SAS status lamps.
    Rect box = Rect::f(cx - 9 * u, cy - r - 5.4 * u, 18 * u, 5.7 * u);
    c.rounded_rect(box, rnd(.5 * u), {50, 59, 64});
    c.text(title_case(n.speed_mode), 1.8, GREEN, cx, box.y + .4 * u, Anchor::MidTop);
    c.text(n.speed ? fmt::speed(*n.speed) : "-- m/s", 2.8, GREEN, cx, box.y + 2.4 * u, Anchor::MidTop);
    const Color off{108, 118, 121};
    c.text("RCS", 2.4, n.rcs ? GREEN : off, cx - 14 * u, box.centery(), Anchor::Center);
    c.text("SAS", 2.4, n.sas ? GREEN : off, cx + 14 * u, box.centery(), Anchor::Center);
    // Just under the rim, so it keeps clear of the SAS mode row below at every screen size.
    c.text(fmt::str("HDG %03lld\xC2\xB0", pymod(py_round(n.heading), 360)), 2.8, GREEN, cx, cy + r + .9 * u,
           Anchor::MidTop);

    // Throttle and G-force arcs, placed like the stock navball bezel.
    Color g_color = n.g_force < 5 ? GREEN : n.g_force < 10 ? Color{255, 186, 72} : Color{255, 92, 92};
    for (int side : {-1, 1}) {
        double value = side == -1 ? n.throttle : n.g_force, maximum = side == -1 ? 1 : 15;
        const char* label = side == -1 ? "THR" : "G";
        Color color = side == -1 ? WHITE : g_color;
        std::vector<Pt> arc;
        for (int i = 0; i < 81; ++i) {
            double angle = radians(side == -1 ? 140 + i : -40 + i);
            arc.push_back({static_cast<float>(cx + (r + 3.7 * u) * std::cos(angle)),
                           static_cast<float>(cy + (r + 3.7 * u) * std::sin(angle))});
        }
        c.polyline(arc, false, std::max(2.0f, rnd(.7 * u)), {66, 78, 85});
        if (side == 1)
            c.polyline(std::vector<Pt>(arc.begin(), arc.begin() + 20), false, std::max(2.0f, rnd(.6 * u)), {144, 64, 57});
        std::vector<std::pair<int, double>> ticks;
        if (side == -1) ticks = {{0, 140}, {100, 220}};
        else ticks = {{0, 40}, {5, 13.333}, {10, -13.333}, {15, -40}};
        for (const auto& [number, deg] : ticks) {
            double angle = radians(deg), inner = r + 3.2 * u, outer = r + 4.2 * u;
            c.line({static_cast<float>(cx + inner * std::cos(angle)), static_cast<float>(cy + inner * std::sin(angle))},
                   {static_cast<float>(cx + outer * std::cos(angle)), static_cast<float>(cy + outer * std::sin(angle))},
                   std::max(1.0f, rnd(.15 * u)), INK);
            c.text(std::to_string(number), 1.5, INK, cx + (r + 5.2 * u) * std::cos(angle),
                   cy + (r + 5.2 * u) * std::sin(angle), Anchor::Center);
        }
        long long count = std::max(1LL, std::min(80LL, py_round(value / maximum * 80)));
        std::vector<Pt> filled = side == -1 ? std::vector<Pt>(arc.begin(), arc.begin() + count + 1)
                                            : std::vector<Pt>(arc.end() - count - 1, arc.end());
        c.polyline(filled, false, std::max(2.0f, rnd(.45 * u)), color);
        double x = cx + side * (r + 5.8 * u);
        c.text(label, 2, INK, x, cy - 2 * u, Anchor::Center);
        c.text(side == -1 ? fmt::str("%lld%%", py_round(n.throttle * 100)) : fmt::str("%.1f", n.g_force), 2.1, color, x,
               cy + 1 * u, Anchor::Center);
    }
    if (n.node_remaining) {
        std::vector<Pt> arc;
        for (int i = 0; i < 101; ++i)
            arc.push_back({static_cast<float>(cx + (r + 8 * u) * std::cos(radians(-70 + i))),
                           static_cast<float>(cy + (r + 8 * u) * std::sin(radians(-70 + i)))});
        c.polyline(arc, false, std::max(2.0f, rnd(.6 * u)), {57, 71, 53});
        double remaining = std::max(0.0, *n.node_remaining);
        double dv = n.node_dv && *n.node_dv != 0 ? *n.node_dv : remaining;
        double fraction = std::min(1.0, remaining / std::max(dv, 1.0));
        long long count = std::max(1LL, py_round(100 * fraction));
        c.polyline(std::vector<Pt>(arc.end() - count - 1, arc.end()), false, std::max(2.0f, rnd(.4 * u)), GREEN);
        c.text(fmt::speed(remaining), 2, GREEN, cx + r + 7 * u, cy - r - 3 * u, Anchor::Center);
    }
    // SAS mode gadgets: read-only indicators following the game's selected mode.
    const auto& modes = sas_modes();
    double y = rect.bottom() - 8.5 * u, gap = .4 * u;
    double width = std::min(6.1 * u, (rect.w - 4 * u - 9 * gap) / 10);
    double start = rect.centerx() - (10 * width + 9 * gap) / 2;
    for (int i = 0; i < 10; ++i) {
        Rect button = Rect::f(start + i * (width + gap), y, width, 2.7 * u);
        bool active = n.sas && n.sas_mode == modes[i].first;
        c.rounded_rect(button, std::max(1.0f, rnd(.3 * u)), active ? Color{62, 91, 63} : Color{37, 46, 54});
        c.text(modes[i].second, 1.6, active ? GREEN : INK, button.centerx(), button.centery(), Anchor::Center);
    }
    if (n.node_remaining) {
        double remaining = std::max(*n.node_remaining, 0.0);
        double total = std::max(n.node_dv && *n.node_dv != 0 ? *n.node_dv : remaining, 1.0);
        Rect bar = Rect::f(rect.x + 2 * u, rect.bottom() - 5.2 * u, rect.w - 4 * u, .6 * u);
        c.rect(bar, {53, 65, 62});
        c.rect(Rect(bar.x, bar.y, static_cast<int>(bar.w * std::min(1.0, remaining / total)), bar.h), GREEN);
        std::string burn = n.burn_seconds ? fmt::duration(*n.burn_seconds) : "--";
        std::string countdown = n.node_time ? fmt::duration(*n.node_time) : "--";
        c.text("NODE " + fmt::speed(remaining) + "   T " + countdown + "   EST BURN " + burn, 2.1, GREEN,
               rect.centerx(), rect.bottom() - 3.8 * u, Anchor::MidTop);
        if (n.burn_seconds && n.node_time)
            c.text("START IN " + fmt::duration(*n.node_time - *n.burn_seconds / 2), 1.8, INK, rect.centerx(),
                   rect.bottom() - 1.6 * u, Anchor::MidTop);
    } else if (n.node_dv) {  // known node, no burn progress estimate
        c.text("NODE " + fmt::speed(*n.node_dv) + "   T " + (n.node_time ? fmt::duration(*n.node_time) : "--"), 2.1,
               GREEN, rect.centerx(), rect.bottom() - 3.8 * u, Anchor::MidTop);
    } else {
        c.text("NO MANEUVER NODE", 2.1, {108, 126, 141}, rect.centerx(), rect.bottom() - 3.8 * u, Anchor::MidTop);
    }
}

}  // namespace ksphmi
