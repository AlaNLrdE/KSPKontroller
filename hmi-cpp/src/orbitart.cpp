#include "orbitart.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <vector>

#include "config.hpp"
#include "vec.hpp"

namespace ksphmi {
OrbitArt::~OrbitArt() {
    SDL_DestroyTexture(color_);
    SDL_DestroyTexture(phosphor_);
    SDL_DestroyTexture(glass_);
}
const std::vector<std::string>& OrbitArt::stock_bodies() {
    static const std::vector<std::string> names = {"Sun",    "Moho", "Eve", "Gilly", "Kerbin", "Mun",
                                                   "Minmus", "Duna", "Ike", "Dres",  "Jool",   "Laythe",
                                                   "Vall",   "Tylo", "Bop", "Pol",   "Eeloo"};
    return names;
}
std::string OrbitArt::material_key(const std::string& name) {
    if (name.empty() || name.size() > 80) return "";
    std::string key;
    for (unsigned char ch : name) {
        if (ch > 127 || !(std::isalnum(ch) || ch == ' ' || ch == '_' || ch == '-')) return "";
        key += ch == ' ' ? '_' : char(std::tolower(ch));
    }
    if (key == "kerbol") key = "sun";
    if (key == "the_mun") key = "mun";
    return key;
}
Color OrbitArt::atmosphere_color(const std::string& name) {
    const auto key = material_key(name);
    if (key == "duna") return {76, 42, 28};
    if (key == "eve") return {48, 29, 67};
    if (key == "jool") return {31, 62, 24};
    return {24, 48, 78};
}
SDL_Texture* OrbitArt::planet(Canvas& c, const std::string& name, bool crt) {
    const std::string key = material_key(name);
    if (key.empty()) return nullptr;
    SDL_Texture*& cached = crt ? phosphor_ : color_;
    std::string& cached_name = crt ? phosphor_name_ : color_name_;
    if (cached_name == key) return cached;
    const std::string path = find_app_dir() + "/assets/bodies/" + key + ".bmp";
    SDL_Surface* source = SDL_LoadBMP(path.c_str());
    if (!source) {
        for (const auto& stock : stock_bodies())
            if (material_key(stock) == key) throw std::runtime_error("planet material " + path + ": " + SDL_GetError());
        // Custom systems can provide their own safely named asset; never substitute Kerbin for an unknown body.
        SDL_DestroyTexture(cached);
        cached = nullptr;
        cached_name = key;
        return nullptr;
    }
    SDL_Surface* surface = SDL_ConvertSurfaceFormat(source, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(source);
    if (!surface) throw std::runtime_error(SDL_GetError());
    if (crt) {
        for (int y = 0; y < surface->h; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(surface->pixels) + y * surface->pitch);
            for (int x = 0; x < surface->w; ++x) {
                const uint32_t pixel = row[x];
                const int r = (pixel >> 16) & 255, g = (pixel >> 8) & 255, b = pixel & 255;
                const double luma = (.2126 * r + .7152 * g + .0722 * b) / 255;
                const double light = std::pow(luma, .72);
                const auto red = uint32_t(7 + 43 * light), green = uint32_t(19 + 218 * light),
                           blue = uint32_t(8 + 68 * light);
                row[x] = 0xff000000u | red << 16 | green << 8 | blue;
            }
        }
    }
    SDL_Texture* replacement = SDL_CreateTextureFromSurface(c.renderer(), surface);
    SDL_FreeSurface(surface);
    if (!replacement) throw std::runtime_error(SDL_GetError());
    SDL_DestroyTexture(cached);
    cached = replacement;
    cached_name = key;
    SDL_SetTextureBlendMode(cached, SDL_BLENDMODE_NONE);
    return cached;
}
void OrbitArt::body(Canvas& c, const std::string& name, double x, double y, double radius, bool crt) {
    if (!std::isfinite(x + y + radius) || radius <= 0) return;
    if (SDL_Texture* texture = planet(c, name, crt)) {
        constexpr int n = 96;
        std::vector<SDL_Vertex> vertices{{{float(x), float(y)}, {255, 255, 255, 255}, {.5f, .5f}}};
        std::vector<int> indices;
        for (int i = 0; i <= n; ++i) {
            const double a = i * 2 * PI / n, dx = std::cos(a), dy = std::sin(a);
            vertices.push_back({{float(x + radius * dx), float(y + radius * dy)},
                                {255, 255, 255, 255},
                                {float(.5 + .466 * dx), float(.5 + .466 * dy)}});
            if (i < n)
                for (int j : {0, i + 1, i + 2}) indices.push_back(j);
        }
        c.triangles(texture, vertices.data(), int(vertices.size()), indices.data(), int(indices.size()));
    } else {
        // A modded/unknown body without supplied artwork gets a neutral shaded sphere, never Kerbin geography.
        Color base = crt                                 ? Color{47, 193, 76}
                     : name == "Duna"                    ? Color{171, 81, 48}
                     : name == "Eve"                     ? Color{118, 84, 156}
                     : name == "Jool"                    ? Color{105, 157, 72}
                     : name == "Laythe"                  ? Color{53, 112, 157}
                     : name == "Sun" || name == "Kerbol" ? Color{246, 191, 69}
                                                         : Color{148, 146, 135};
        c.circle(float(x), float(y), float(radius), base);
        for (int i = 0; i < 28; ++i) {
            const double r = radius * (1.0 - i / 32.0), t = .32 + .65 * i / 28;
            c.circle(float(x - radius * .08 * i / 28), float(y - radius * .08 * i / 28), float(r),
                     Color{uint8_t(base.r * t), uint8_t(base.g * t), uint8_t(base.b * t)});
        }
    }
    if (crt) {
        c.ring(float(x), float(y), float(radius), std::max(1.0f, float(.12 * c.u)), Color{72, 225, 109});
        // Fine latitude marks evoke a green tracking monitor without changing the sphere's projected size.
        for (int pitch : {-60, -30, 0, 30, 60}) {
            const double yy = std::sin(radians(pitch)) * radius,
                         half = std::sqrt(std::max(0.0, radius * radius - yy * yy));
            c.line({float(x - half), float(y + yy)}, {float(x + half), float(y + yy)}, 1, Color{16, 72, 29});
        }
    }
}
void OrbitArt::glass(Canvas& c, const Rect& viewport) {
    if (viewport.w <= 0 || viewport.h <= 0) return;
    if (!glass_ || glass_w_ != viewport.w || glass_h_ != viewport.h) {
        SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, viewport.w, viewport.h, 32, SDL_PIXELFORMAT_ARGB8888);
        if (!surface) throw std::runtime_error(SDL_GetError());
        for (int y = 0; y < surface->h; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(surface->pixels) + y * surface->pitch);
            for (int x = 0; x < surface->w; ++x) {
                const double nx = (2.0 * x - surface->w) / surface->w, ny = (2.0 * y - surface->h) / surface->h;
                const double edge = std::pow(std::min(1.0, (nx * nx + ny * ny) / 1.8), 2);
                const int alpha = std::min(110, int(9 + 58 * edge) + (y % 3 == 0 ? 44 : 0));
                row[x] = uint32_t(alpha) << 24 | 0x00000301u;
            }
        }
        SDL_Texture* replacement = SDL_CreateTextureFromSurface(c.renderer(), surface);
        SDL_FreeSurface(surface);
        if (!replacement) throw std::runtime_error(SDL_GetError());
        SDL_SetTextureBlendMode(replacement, SDL_BLENDMODE_BLEND);
        SDL_DestroyTexture(glass_);
        glass_ = replacement;
        glass_w_ = viewport.w;
        glass_h_ = viewport.h;
    }
    c.image(glass_, float(viewport.x), float(viewport.y), float(viewport.w), float(viewport.h));
}
}  // namespace ksphmi
