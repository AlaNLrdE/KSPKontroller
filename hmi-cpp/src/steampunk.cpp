#include "steampunk.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "config.hpp"

namespace ksphmi {
std::string steam_font(bool headings) {
    static const std::string root = find_app_dir() + "/assets/fonts/";
    static const std::string title = root + "Cinzel.ttf", figures = root + "CrimsonPro.ttf";
    return headings ? title : figures;
}
namespace {
SDL_Texture* load(Canvas& c, const char* file, bool aerospace = false) {
    const std::string path = find_app_dir() + (aerospace ? "/assets/apollo/" : "/assets/steampunk/") + file;
    SDL_Surface* s = SDL_LoadBMP(path.c_str());
    if (!s) throw std::runtime_error("steampunk asset " + path + ": " + SDL_GetError());
    SDL_Texture* t = SDL_CreateTextureFromSurface(c.renderer(), s);
    SDL_FreeSurface(s);
    if (!t) throw std::runtime_error(SDL_GetError());
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_NONE);
    return t;
}
}  // namespace
SteampunkArt::SteampunkArt(Canvas& c, bool aerospace) : aerospace_(aerospace), c_(c) {
    panel_ = load(c, aerospace ? "panel.bmp" : "panel-v2.bmp", aerospace);
    try {
        bezel_ = load(c, aerospace ? "navball-bezel.bmp" : "navball-bezel-v2.bmp", aerospace);
        if (!aerospace) {
            dial_ = load(c, "dial-face.bmp");
            tube_ = load(c, "sight-tube.bmp");
        }
    } catch (...) {
        SDL_DestroyTexture(panel_);
        SDL_DestroyTexture(bezel_);
        SDL_DestroyTexture(dial_);
        SDL_DestroyTexture(tube_);
        throw;
    }
}
SteampunkArt::~SteampunkArt() {
    c_.flush();
    SDL_DestroyTexture(panel_);
    SDL_DestroyTexture(bezel_);
    SDL_DestroyTexture(dial_);
    SDL_DestroyTexture(tube_);
}
void SteampunkArt::panel(const Rect& r, double corner) {
    // Nine-slice the rails; keep screw heads circular and preserve their physical size at every aspect ratio.
    int w, h;
    SDL_QueryTexture(panel_, nullptr, nullptr, &w, &h);
    int sx = int(w * (aerospace_ ? .125 : .16)), sy = int(h * (aerospace_ ? .125 : .16));
    const int d = std::max(1, int(std::min({corner, r.w / 2.0, r.h / 2.0})));
    const int xs[] = {0, sx, w - sx, w}, ys[] = {0, sy, h - sy, h};
    const int dx[] = {r.x, r.x + d, r.right() - d, r.right()};
    const int dy[] = {r.y, r.y + d, r.bottom() - d, r.bottom()};
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x)
            c_.blit(panel_, Rect(xs[x], ys[y], xs[x + 1] - xs[x], ys[y + 1] - ys[y]),
                    Rect(dx[x], dy[y], dx[x + 1] - dx[x], dy[y + 1] - dy[y]));
}
void SteampunkArt::dial(double cx, double cy, double radius) {
    if (!dial_) return;
    constexpr int n = 96;
    std::vector<SDL_Vertex> vertices{{{float(cx), float(cy)}, {255, 255, 255, 255}, {.5f, .5f}}};
    std::vector<int> indices;
    for (int i = 0; i <= n; ++i) {
        const double angle = i * 2 * 3.14159265358979323846 / n, x = std::cos(angle), y = std::sin(angle);
        vertices.push_back({{float(cx + radius * x), float(cy + radius * y)},
                            {255, 255, 255, 255},
                            {float(.5 + .485 * x), float(.5 + .485 * y)}});
        if (i < n)
            for (int k : {0, i + 1, i + 2}) indices.push_back(k);
    }
    c_.triangles(dial_, vertices.data(), int(vertices.size()), indices.data(), int(indices.size()));
}
void SteampunkArt::sight_tube(const Rect& r) {
    if (!tube_) return;
    int w, h;
    SDL_QueryTexture(tube_, nullptr, nullptr, &w, &h);
    // Ignore only the black margins above/below the photographed tube; keep its complete fittings.
    const int sy = int(h * .19), sh = int(h * .57), edge = int(w * .20);
    const int cap = std::min(r.w / 3, int(r.h * 1.0));
    c_.blit(tube_, Rect(0, sy, edge, sh), Rect(r.x, r.y, cap, r.h));
    c_.blit(tube_, Rect(edge, sy, w - 2 * edge, sh), Rect(r.x + cap, r.y, r.w - 2 * cap, r.h));
    c_.blit(tube_, Rect(w - edge, sy, edge, sh), Rect(r.right() - cap, r.y, cap, r.h));
}
void SteampunkArt::bezel(double cx, double cy, double outer, double inner) {
    // Map the complete photographed annulus onto the reserved bezel zone. No black square around the dial.
    constexpr int n = 160;
    constexpr double pi = 3.14159265358979323846;
    std::vector<SDL_Vertex> vertices;
    std::vector<int> indices;
    for (int i = 0; i <= n; ++i) {
        const double a = 2 * pi * i / n, x = std::cos(a), y = std::sin(a);
        for (int k = 0; k < 2; ++k) {
            const double r = k ? inner : outer, uv = k ? (aerospace_ ? .347 : .337) : .488;
            vertices.push_back({{float(cx + r * x), float(cy + r * y)},
                                {255, 255, 255, 255},
                                {float(.5 + uv * x), float(.5 + uv * y)}});
        }
        if (i < n) {
            int j = i * 2;
            for (int index : {j, j + 2, j + 1, j + 1, j + 2, j + 3}) indices.push_back(index);
        }
    }
    c_.triangles(bezel_, vertices.data(), int(vertices.size()), indices.data(), int(indices.size()));
}
}  // namespace ksphmi
