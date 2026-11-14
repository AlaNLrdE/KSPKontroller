// Drawing on an SDL_Renderer with pygame's semantics (rects, circles, rings, arcs, thick lines, anchored text).
//
// Everything is batched into SDL_RenderGeometry calls: consecutive shapes share one draw call, text comes from a
// per-size glyph atlas (no texture upload per changed string) and the batch is only flushed when the texture or
// the clip changes. With a GPU renderer edges get a one-pixel alpha feather (cheap anti-aliasing); the software
// renderer (fbdev, off-screen frames) skips it to save CPU.
#pragma once

#include <SDL.h>
#include <SDL_ttf.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ksphmi {

struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;
    bool operator==(const Color& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
};

/// Integer rectangle with pygame.Rect semantics (truncating constructor, C integer halves).
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    Rect() = default;
    Rect(int x_, int y_, int w_, int h_) : x(x_), y(y_), w(w_), h(h_) {}
    static Rect f(double x, double y, double w, double h) {
        return {static_cast<int>(x), static_cast<int>(y), static_cast<int>(w), static_cast<int>(h)};
    }
    int right() const { return x + w; }
    int bottom() const { return y + h; }
    int centerx() const { return x + w / 2; }
    int centery() const { return y + h / 2; }
    Rect inflate(double dx, double dy) const {
        int ix = static_cast<int>(dx), iy = static_cast<int>(dy);
        return {x - ix / 2, y - iy / 2, w + ix, h + iy};
    }
    Rect move(double dx, double dy) const { return {x + static_cast<int>(dx), y + static_cast<int>(dy), w, h}; }
    bool contains(const Rect& o) const {
        return x <= o.x && y <= o.y && right() >= o.right() && bottom() >= o.bottom() && right() > o.x &&
               bottom() > o.y;
    }
    bool collides(const Rect& o) const {
        return x < o.right() && o.x < right() && y < o.bottom() && o.y < bottom() && w > 0 && h > 0 && o.w > 0 &&
               o.h > 0;
    }
    Rect clip(const Rect& o) const {
        int l = std::max(x, o.x), t = std::max(y, o.y);
        int r = std::min(right(), o.right()), b = std::min(bottom(), o.bottom());
        if (r <= l || b <= t) return {l, t, 0, 0};
        return {l, t, r - l, b - t};
    }
    bool operator==(const Rect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};

struct Pt {
    float x = 0, y = 0;
};

enum class Anchor { TopLeft, TopRight, Center, MidTop, MidLeft, MidRight };

/// One font size of FreeSansBold (pygame's default font) with a glyph atlas texture.
class Font {
public:
    Font(SDL_Renderer* renderer, const std::string& path, int px, bool bold);
    ~Font();
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;

    struct Glyph {
        SDL_Rect src{0, 0, 0, 0};  ///< in the atlas; w = 0 for an empty glyph (space)
        int advance = 0, xoff = 0;
    };
    const Glyph& glyph(uint32_t codepoint);
    int height() const { return height_; }
    int kerning(uint32_t previous, uint32_t current) const;
    SDL_Texture* texture() const { return atlas_; }
    TTF_Font* ttf() const { return font_; }

private:
    SDL_Renderer* renderer_;
    TTF_Font* font_ = nullptr;
    SDL_Texture* atlas_ = nullptr;
    int atlas_w_, atlas_h_, pen_x_ = 1, pen_y_ = 1, shelf_h_ = 0, height_ = 0;
    std::map<uint32_t, Glyph> glyphs_;
};

/// Decode UTF-8 into code points (invalid bytes become U+FFFD).
std::vector<uint32_t> utf8_decode(const std::string& s);

/// Path of the TTF font: $KSPHMI_FONT, else FreeSansBold (fonts-freefont-ttf), else DejaVu Sans Bold.
std::string find_font();

class Canvas {
public:
    /// `antialias`: one-pixel feathered edges (worth it on a GPU renderer only).
    Canvas(SDL_Renderer* renderer, int width, int height, bool antialias, std::string font_path = find_font());
    ~Canvas();

    SDL_Renderer* renderer() const { return renderer_; }
    int w, h;
    double u;  ///< layout unit: 1 % of the screen height

    void clear(Color c);
    /// Submit the pending batch (call before present / read-back).
    void flush();

    Rect clip() const { return clip_; }
    void set_clip(const Rect& r);

    // Shapes. Coordinates in pixels (floats), colours with alpha.
    void rect(const Rect& r, Color c);
    void fill_rect(float x, float y, float w, float h, Color c);
    void rounded_rect(const Rect& r, float radius, Color c);
    void circle(float cx, float cy, float radius, Color c);
    /// pygame.draw.circle with a width: a ring whose outer edge is `radius`.
    void ring(float cx, float cy, float radius, float width, Color c);
    void line(Pt a, Pt b, float width, Color c);
    void polyline(const std::vector<Pt>& points, bool closed, float width, Color c);
    /// Filled convex polygon; `feather` adds the anti-aliased edge (off for tiling cells).
    void polygon(const std::vector<Pt>& points, Color c, bool feather = true);
    /// pygame.draw.arc on the square around (cx, cy): thickness inwards, angles counter-clockwise in radians.
    void arc(float cx, float cy, float radius, double start, double stop, float width, Color c);
    /// Raw triangles (positions in pixels, texture coordinates normalised).
    void triangles(SDL_Texture* texture, const SDL_Vertex* vertices, int count_vertices, const int* indices,
                   int count_indices);
    void image(SDL_Texture* texture, float x, float y, float w, float h, Color mod = {255, 255, 255, 255});

    // Text, sized in layout units as display.py: px = max(8, int(size_units * u)).
    Font& font(float size_units, bool bold = false);
    Font& font_px(int px, bool bold = false);
    int text_width(Font& f, const std::string& s);
    Rect text(const std::string& s, float size_units, Color c, double x, double y, Anchor a = Anchor::TopLeft);
    /// Called with every text rectangle (layout tests).
    std::function<void(const std::string&, const Rect&)> on_text;

private:
    void begin(SDL_Texture* texture, int vertices, int indices);
    int vertex(float x, float y, Color c, float u = 0, float v = 0);
    void tri(int a, int b, int c);
    void feather_edge(Pt a, Pt b, Pt outward, Color c);
    bool outside(float x0, float y0, float x1, float y1) const;
    void copy(SDL_Texture* texture, const SDL_Rect& src, const SDL_FRect& dst, Color mod);

    SDL_Renderer* renderer_;
    bool aa_;
    std::string font_path_;
    Rect clip_;
    SDL_Texture* batch_texture_ = nullptr;
    bool batch_opaque_ = true;
    std::vector<SDL_Vertex> vertices_;
    std::vector<int> indices_;
    std::map<std::pair<int, bool>, std::unique_ptr<Font>> fonts_;
};

}  // namespace ksphmi
