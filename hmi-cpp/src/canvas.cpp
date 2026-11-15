#include "canvas.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

#include "log.hpp"

namespace ksphmi {

namespace {

constexpr float TAU = 6.28318530717958647692f;

int segments_for(float radius, double sweep = TAU) {
    int n = static_cast<int>(std::ceil(std::sqrt(std::max(radius, 1.0f)) * 4.0 * sweep / TAU)) + 6;
    return std::max(6, std::min(n, 180));
}

Color transparent(Color c) { return {c.r, c.g, c.b, 0}; }

bool file_exists(const std::string& p) {
    std::ifstream f(p);
    return f.good();
}

}  // namespace

// =============================================================================================== UTF-8
std::vector<uint32_t> utf8_decode(const std::string& s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        auto b = static_cast<unsigned char>(s[i]);
        int extra = b < 0x80 ? 0 : (b >> 5) == 6 ? 1 : (b >> 4) == 14 ? 2 : (b >> 3) == 30 ? 3 : -1;
        if (extra < 0 || i + static_cast<size_t>(extra) >= s.size() + (extra == 0)) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        uint32_t cp = extra == 0 ? b : b & (0x3F >> extra);
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            auto c = static_cast<unsigned char>(s[i + k]);
            if ((c & 0xC0) != 0x80) ok = false;
            cp = (cp << 6) | (c & 0x3F);
        }
        out.push_back(ok ? cp : 0xFFFD);
        i += ok ? static_cast<size_t>(extra) + 1 : 1;
    }
    return out;
}

std::string find_font() {
    if (const char* env = std::getenv("KSPHMI_FONT"))
        if (*env) return env;
    for (const char* p : {"/usr/share/fonts/truetype/freefont/FreeSansBold.ttf",
                          "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"})
        if (file_exists(p)) return p;
    return "/usr/share/fonts/truetype/freefont/FreeSansBold.ttf";
}

// =============================================================================================== Font
Font::Font(SDL_Renderer* renderer, const std::string& path, int px, bool bold) : renderer_(renderer) {
    // Font sizes are given as line heights: the point size is 0.6875 of them (the layout was tuned that way).
    font_ = TTF_OpenFont(path.c_str(), std::max(1, static_cast<int>(px * 0.6875)));
    if (!font_) throw std::runtime_error("cannot open font " + path + ": " + TTF_GetError());
    if (bold) TTF_SetFontStyle(font_, TTF_STYLE_BOLD);
    TTF_SetFontKerning(font_, 1);
    height_ = TTF_FontHeight(font_);
    // Room for ~160 glyphs; non-power-of-two textures are fine on the Pi's V3D and in software.
    atlas_w_ = atlas_h_ = std::min(4096, static_cast<int>(std::ceil(10.6 * height_)) + 4);
    atlas_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, atlas_w_, atlas_h_);
    if (!atlas_) throw std::runtime_error(std::string("cannot create the glyph atlas: ") + SDL_GetError());
    SDL_SetTextureBlendMode(atlas_, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(atlas_, SDL_ScaleModeNearest);
    std::vector<uint32_t> zero(static_cast<size_t>(atlas_w_) * 64, 0);
    for (int y = 0; y < atlas_h_; y += 64) {
        SDL_Rect r{0, y, atlas_w_, std::min(64, atlas_h_ - y)};
        SDL_UpdateTexture(atlas_, &r, zero.data(), atlas_w_ * 4);
    }
}

Font::~Font() {
    if (atlas_) SDL_DestroyTexture(atlas_);
    if (font_) TTF_CloseFont(font_);
}

int Font::kerning(uint32_t previous, uint32_t current) const {
    return TTF_GetFontKerningSizeGlyphs32(font_, previous, current);
}

const Font::Glyph& Font::glyph(uint32_t cp) {
    auto it = glyphs_.find(cp);
    if (it != glyphs_.end()) return it->second;
    if (cp != '?' && !TTF_GlyphIsProvided32(font_, cp)) return glyphs_[cp] = glyph('?');
    Glyph g;
    int minx = 0, maxx = 0, miny = 0, maxy = 0, advance = 0;
    TTF_GlyphMetrics32(font_, cp, &minx, &maxx, &miny, &maxy, &advance);
    g.advance = advance;
    g.xoff = std::min(0, minx);  // a one-glyph rendering starts at the leftmost ink when it overhangs the pen
    SDL_Surface* s = (cp == ' ') ? nullptr : TTF_RenderGlyph32_Blended(font_, cp, SDL_Color{255, 255, 255, 255});
    if (s) {
        SDL_Surface* argb = s->format->format == SDL_PIXELFORMAT_ARGB8888 ? s
                                                                          : SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ARGB8888, 0);
        if (pen_x_ + argb->w + 1 > atlas_w_) {
            pen_x_ = 1;
            pen_y_ += shelf_h_ + 1;
            shelf_h_ = 0;
        }
        if (argb->w + 2 <= atlas_w_ && pen_y_ + argb->h + 1 <= atlas_h_) {
            g.src = {pen_x_, pen_y_, argb->w, argb->h};
            SDL_UpdateTexture(atlas_, &g.src, argb->pixels, argb->pitch);
            pen_x_ += argb->w + 1;
            shelf_h_ = std::max(shelf_h_, argb->h);
        } else if (cp != '?') {  // atlas full: show a placeholder rather than nothing
            if (argb != s) SDL_FreeSurface(argb);
            SDL_FreeSurface(s);
            return glyphs_[cp] = glyph('?');
        }
        if (argb != s) SDL_FreeSurface(argb);
        SDL_FreeSurface(s);
    }
    return glyphs_[cp] = g;
}

// =============================================================================================== Canvas
Canvas::Canvas(SDL_Renderer* renderer, int width, int height, bool antialias, std::string font_path)
    : w(width), h(height), u(height / 100.0), renderer_(renderer), aa_(antialias), font_path_(std::move(font_path)),
      clip_(0, 0, width, height) {
    if (!TTF_WasInit() && TTF_Init() != 0) throw std::runtime_error(std::string("TTF_Init: ") + TTF_GetError());
    if (!file_exists(font_path_)) throw std::runtime_error("font not found: " + font_path_ + " (apt install fonts-freefont-ttf, or set KSPHMI_FONT)");
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_RenderSetClipRect(renderer_, nullptr);
    vertices_.reserve(1 << 16);
    indices_.reserve(1 << 17);
}

Canvas::~Canvas() { fonts_.clear(); }

void Canvas::flush() {
    if (!indices_.empty()) {
        // SDL's software rasteriser blends each triangle through a temporary surface: opaque untextured
        // batches (everything drawn without anti-aliasing) are written directly instead.
        bool direct = !aa_ && !batch_texture_ && batch_opaque_;
        if (direct) SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
        SDL_RenderGeometry(renderer_, batch_texture_, vertices_.data(), static_cast<int>(vertices_.size()),
                           indices_.data(), static_cast<int>(indices_.size()));
        if (direct) SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    }
    vertices_.clear();
    indices_.clear();
    batch_opaque_ = true;
}

void Canvas::begin(SDL_Texture* texture, int vertices, int indices) {
    if (texture != batch_texture_ || vertices_.size() + static_cast<size_t>(vertices) > 60000) {
        flush();
        batch_texture_ = texture;
    }
    (void)indices;
}

int Canvas::vertex(float x, float y, Color c, float tu, float tv) {
    SDL_Vertex v;
    v.position = {x, y};
    v.color = {c.r, c.g, c.b, c.a};
    v.tex_coord = {tu, tv};
    batch_opaque_ = batch_opaque_ && c.a == 255;
    vertices_.push_back(v);
    return static_cast<int>(vertices_.size()) - 1;
}

void Canvas::tri(int a, int b, int c) {
    indices_.push_back(a);
    indices_.push_back(b);
    indices_.push_back(c);
}

void Canvas::clear(Color c) {
    flush();
    SDL_RenderSetClipRect(renderer_, nullptr);
    clip_ = {0, 0, w, h};
    SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, 255);
    SDL_RenderClear(renderer_);
}

void Canvas::set_clip(const Rect& r) {
    if (r == clip_) return;
    flush();
    clip_ = r;
    if (r == Rect(0, 0, w, h)) {
        SDL_RenderSetClipRect(renderer_, nullptr);
    } else {
        SDL_Rect s{r.x, r.y, std::max(0, r.w), std::max(0, r.h)};
        if (s.w == 0 || s.h == 0) s = {r.x, r.y, 0, 0};
        SDL_RenderSetClipRect(renderer_, &s);
    }
}

void Canvas::set_target(SDL_Texture* texture) {
    flush();
    SDL_SetRenderTarget(renderer_, texture);
    Rect keep = clip_;
    clip_ = Rect(-1, -1, -1, -1);  // force set_clip to apply it to this target
    set_clip(keep);
}

void Canvas::blit(SDL_Texture* texture, const Rect& src, const Rect& dst) {
    flush();
    SDL_RenderSetClipRect(renderer_, nullptr);
    SDL_SetTextureColorMod(texture, 255, 255, 255);
    SDL_SetTextureAlphaMod(texture, 255);
    SDL_Rect s{src.x, src.y, src.w, src.h}, d{dst.x, dst.y, dst.w, dst.h};
    SDL_RenderCopy(renderer_, texture, &s, &d);
    Rect keep = clip_;
    clip_ = Rect(-1, -1, -1, -1);
    set_clip(keep);
}

void Canvas::fill_rect(float x, float y, float rw, float rh, Color c) {
    if (rw <= 0 || rh <= 0) return;
    if (!aa_) {  // software renderer: SDL_FillRect / blended fill, far faster than rasterising two triangles
        flush();
        SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a);
        if (c.a == 255) SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
        SDL_FRect r{x, y, rw, rh};
        SDL_RenderFillRectF(renderer_, &r);
        if (c.a == 255) SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        return;
    }
    begin(nullptr, 4, 6);
    int a = vertex(x, y, c), b = vertex(x + rw, y, c), d = vertex(x + rw, y + rh, c), e = vertex(x, y + rh, c);
    tri(a, b, d);
    tri(a, d, e);
}

void Canvas::rect(const Rect& r, Color c) { fill_rect(r.x, r.y, r.w, r.h, c); }

void Canvas::feather_edge(Pt a, Pt b, Pt outward, Color c) {
    Color t = transparent(c);
    int i0 = vertex(a.x, a.y, c), i1 = vertex(b.x, b.y, c);
    int o0 = vertex(a.x + outward.x, a.y + outward.y, t), o1 = vertex(b.x + outward.x, b.y + outward.y, t);
    tri(i0, i1, o1);
    tri(i0, o1, o0);
}

void Canvas::polygon(const std::vector<Pt>& p, Color c, bool feather) {
    if (p.size() < 3) return;
    float x0 = p[0].x, y0 = p[0].y, x1 = x0, y1 = y0;
    for (const Pt& q : p) x0 = std::min(x0, q.x), y0 = std::min(y0, q.y), x1 = std::max(x1, q.x), y1 = std::max(y1, q.y);
    if (outside(x0, y0, x1, y1)) return;
    int n = static_cast<int>(p.size());
    begin(nullptr, n * 3, n * 9);
    int first = static_cast<int>(vertices_.size());
    for (const Pt& q : p) vertex(q.x, q.y, c);
    for (int i = 1; i + 1 < n; ++i) tri(first, first + i, first + i + 1);
    if (!(feather && aa_)) return;
    float cx = 0, cy = 0;
    for (const Pt& q : p) cx += q.x, cy += q.y;
    cx /= n, cy /= n;
    auto out = [&](const Pt& q) {
        float dx = q.x - cx, dy = q.y - cy, l = std::sqrt(dx * dx + dy * dy);
        return l > 1e-3f ? Pt{dx / l, dy / l} : Pt{0, 0};
    };
    Color t = transparent(c);
    int ring = static_cast<int>(vertices_.size());
    for (const Pt& q : p) {
        Pt o = out(q);
        vertex(q.x + o.x, q.y + o.y, t);
    }
    for (int i = 0; i < n; ++i) {
        int j = (i + 1) % n;
        tri(first + i, first + j, ring + j);
        tri(first + i, ring + j, ring + i);
    }
}

void Canvas::rounded_rect(const Rect& r, float radius, Color c) {
    float rr = std::floor(std::min({radius, r.w / 2.0f, r.h / 2.0f}));
    if (rr < 1 || c.a < 255) {  // translucent: one polygon, so no overlap is blended twice
        if (rr < 1) {
            rect(r, c);
            return;
        }
    } else {
        fill_rect(r.x + rr, r.y, r.w - 2 * rr, r.h, c);
        fill_rect(r.x, r.y + rr, rr, r.h - 2 * rr, c);
        fill_rect(r.right() - rr, r.y + rr, rr, r.h - 2 * rr, c);
        int k = std::max(3, std::min(16, static_cast<int>(rr / 2)));
        const float corners[4][3] = {{r.right() - rr, r.y + rr, -TAU / 4},
                                     {r.right() - rr, r.bottom() - rr, 0},
                                     {r.x + rr, r.bottom() - rr, TAU / 4},
                                     {r.x + rr, r.y + rr, TAU / 2}};
        for (const auto& corner : corners) {
            std::vector<Pt> p = {{corner[0], corner[1]}};
            for (int i = 0; i <= k; ++i) {
                float a = corner[2] + TAU / 4 * i / k;
                p.push_back({corner[0] + rr * std::cos(a), corner[1] + rr * std::sin(a)});
            }
            polygon(p, c, false);
            if (aa_) {  // feather only the rounded edge
                Color t = transparent(c);
                begin(nullptr, 2 * (k + 1), 6 * k);
                int first = static_cast<int>(vertices_.size());
                for (int i = 0; i <= k; ++i) {
                    float a = corner[2] + TAU / 4 * i / k;
                    vertex(corner[0] + rr * std::cos(a), corner[1] + rr * std::sin(a), c);
                    vertex(corner[0] + (rr + 0.8f) * std::cos(a), corner[1] + (rr + 0.8f) * std::sin(a), t);
                }
                for (int i = 0; i < k; ++i) {
                    int q = first + 2 * i;
                    tri(q, q + 2, q + 3);
                    tri(q, q + 3, q + 1);
                }
            }
        }
        return;
    }
    int k = std::max(3, std::min(16, static_cast<int>(rr / 2)));
    std::vector<Pt> p;
    p.reserve(4 * (k + 1));
    const float corners[4][3] = {{r.right() - rr, r.y + rr, -TAU / 4},
                                 {r.right() - rr, r.bottom() - rr, 0},
                                 {r.x + rr, r.bottom() - rr, TAU / 4},
                                 {r.x + rr, r.y + rr, TAU / 2}};
    for (const auto& corner : corners)
        for (int i = 0; i <= k; ++i) {
            float a = corner[2] + TAU / 4 * i / k;
            p.push_back({corner[0] + rr * std::cos(a), corner[1] + rr * std::sin(a)});
        }
    polygon(p, c);
}

bool Canvas::outside(float x0, float y0, float x1, float y1) const {
    // Shapes far outside the clip (an escape trajectory's vessel marker) are skipped: huge coordinates would
    // overflow the software rasteriser's fixed point.
    return x1 < clip_.x - 2 || y1 < clip_.y - 2 || x0 > clip_.right() + 2 || y0 > clip_.bottom() + 2;
}

void Canvas::circle(float cx, float cy, float radius, Color c) {
    if (radius <= 0 || outside(cx - radius, cy - radius, cx + radius, cy + radius)) return;
    int n = segments_for(radius);
    float core = aa_ ? std::max(0.0f, radius - 0.5f) : radius;
    begin(nullptr, 2 * n + 1, 9 * n);
    int center = vertex(cx, cy, c);
    int first = static_cast<int>(vertices_.size());
    for (int i = 0; i < n; ++i) {
        float a = TAU * i / n;
        vertex(cx + core * std::cos(a), cy + core * std::sin(a), c);
    }
    for (int i = 0; i < n; ++i) tri(center, first + i, first + (i + 1) % n);
    if (!aa_) return;
    int outer = static_cast<int>(vertices_.size());
    Color t = transparent(c);
    for (int i = 0; i < n; ++i) {
        float a = TAU * i / n;
        vertex(cx + (radius + 0.5f) * std::cos(a), cy + (radius + 0.5f) * std::sin(a), t);
    }
    for (int i = 0; i < n; ++i) {
        int j = (i + 1) % n;
        tri(first + i, first + j, outer + j);
        tri(first + i, outer + j, outer + i);
    }
}

void Canvas::arc(float cx, float cy, float radius, double start, double stop, float width, Color c) {
    if (radius <= 0 || width <= 0 || outside(cx - radius, cy - radius, cx + radius, cy + radius)) return;
    while (stop < start) stop += TAU;
    double sweep = stop - start;
    int n = segments_for(radius, sweep);
    float inner = std::max(0.0f, radius - width);
    // Pixel rings: [inner, radius], plus a feather outside each edge on a GPU.
    float radii[4] = {inner - 0.8f, inner, radius, radius + 0.8f};
    int rings = aa_ ? 4 : 2;
    const float* rs = aa_ ? radii : radii + 1;
    begin(nullptr, (n + 1) * rings, 6 * n * (rings - 1));
    int first = static_cast<int>(vertices_.size());
    for (int i = 0; i <= n; ++i) {
        double a = start + sweep * i / n;
        float ca = static_cast<float>(std::cos(a)), sa = static_cast<float>(std::sin(a));
        for (int k = 0; k < rings; ++k) {
            bool edge = aa_ && (k == 0 || k == 3);
            float rk = std::max(0.0f, rs[k]);
            vertex(cx + rk * ca, cy - rk * sa, edge ? transparent(c) : c);  // y up (mathematical angles)
        }
    }
    for (int i = 0; i < n; ++i)
        for (int k = 0; k + 1 < rings; ++k) {
            int a = first + i * rings + k, b = a + rings;
            tri(a, b, b + 1);
            tri(a, b + 1, a + 1);
        }
}

void Canvas::ring(float cx, float cy, float radius, float width, Color c) {
    if (width <= 0 || width >= radius) {
        circle(cx, cy, radius, c);
        return;
    }
    arc(cx, cy, radius, 0, TAU, width, c);
}

void Canvas::line(Pt a, Pt b, float width, Color c) {
    float dx = b.x - a.x, dy = b.y - a.y, l = std::sqrt(dx * dx + dy * dy);
    float pad = width + 2;
    if (l < 1e-4f || outside(std::min(a.x, b.x) - pad, std::min(a.y, b.y) - pad, std::max(a.x, b.x) + pad,
                             std::max(a.y, b.y) + pad))
        return;
    float nx = -dy / l, ny = dx / l;
    float half = aa_ ? std::max(width / 2 - 0.25f, 0.35f) : std::max(width / 2, 0.5f);
    begin(nullptr, 8, 18);
    int i0 = vertex(a.x + nx * half, a.y + ny * half, c), i1 = vertex(b.x + nx * half, b.y + ny * half, c);
    int i2 = vertex(b.x - nx * half, b.y - ny * half, c), i3 = vertex(a.x - nx * half, a.y - ny * half, c);
    tri(i0, i1, i2);
    tri(i0, i2, i3);
    if (!aa_) return;
    Color t = transparent(c);
    float f = half + 0.9f;
    int o0 = vertex(a.x + nx * f, a.y + ny * f, t), o1 = vertex(b.x + nx * f, b.y + ny * f, t);
    int o2 = vertex(b.x - nx * f, b.y - ny * f, t), o3 = vertex(a.x - nx * f, a.y - ny * f, t);
    tri(i0, o0, o1);
    tri(i0, o1, i1);
    tri(i3, i2, o2);
    tri(i3, o2, o3);
}

void Canvas::polyline(const std::vector<Pt>& p, bool closed, float width, Color c) {
    for (size_t i = 1; i < p.size(); ++i) line(p[i - 1], p[i], width, c);
    if (closed && p.size() > 2) line(p.back(), p.front(), width, c);
}

void Canvas::triangles(SDL_Texture* texture, const SDL_Vertex* v, int nv, const int* idx, int ni) {
    begin(texture, nv, ni);
    int base = static_cast<int>(vertices_.size());
    vertices_.insert(vertices_.end(), v, v + nv);
    for (int i = 0; i < ni; ++i) indices_.push_back(base + idx[i]);
}

void Canvas::copy(SDL_Texture* texture, const SDL_Rect& src, const SDL_FRect& dst, Color mod) {
    flush();
    SDL_SetTextureColorMod(texture, mod.r, mod.g, mod.b);
    SDL_SetTextureAlphaMod(texture, mod.a);
    SDL_RenderCopyF(renderer_, texture, &src, &dst);
}

void Canvas::image(SDL_Texture* texture, float x, float y, float iw, float ih, Color mod) {
    if (!aa_) {
        int tw = 0, th = 0;
        SDL_QueryTexture(texture, nullptr, nullptr, &tw, &th);
        copy(texture, SDL_Rect{0, 0, tw, th}, SDL_FRect{x, y, iw, ih}, mod);
        return;
    }
    begin(texture, 4, 6);
    int a = vertex(x, y, mod, 0, 0), b = vertex(x + iw, y, mod, 1, 0);
    int d = vertex(x + iw, y + ih, mod, 1, 1), e = vertex(x, y + ih, mod, 0, 1);
    tri(a, b, d);
    tri(a, d, e);
}

Font& Canvas::font_px(int px, bool bold) {
    auto key = std::make_pair(px, bold);
    auto it = fonts_.find(key);
    if (it == fonts_.end()) it = fonts_.emplace(key, std::make_unique<Font>(renderer_, font_path_, px, bold)).first;
    return *it->second;
}

Font& Canvas::font(float size_units, bool bold) { return font_px(std::max(8, static_cast<int>(size_units * u)), bold); }

int Canvas::text_width(Font& f, const std::string& s) {
    int width = 0;
    uint32_t previous = 0;
    for (uint32_t cp : utf8_decode(s)) {
        if (previous) width += f.kerning(previous, cp);
        width += f.glyph(cp).advance;
        previous = cp;
    }
    return width;
}

Rect Canvas::text(const std::string& s, float size_units, Color c, double x, double y, Anchor anchor) {
    Font& f = font(size_units);
    int tw = text_width(f, s), th = f.height();
    int ix = static_cast<int>(x), iy = static_cast<int>(y);
    Rect r(ix, iy, tw, th);
    switch (anchor) {
        case Anchor::TopLeft: break;
        case Anchor::TopRight: r.x = ix - tw; break;
        case Anchor::Center: r.x = ix - tw / 2, r.y = iy - th / 2; break;
        case Anchor::MidTop: r.x = ix - tw / 2; break;
        case Anchor::MidLeft: r.y = iy - th / 2; break;
        case Anchor::MidRight: r.x = ix - tw, r.y = iy - th / 2; break;
    }
    SDL_Texture* atlas = f.texture();
    float aw = 0, ah = 0;
    {
        int qw = 0, qh = 0;
        SDL_QueryTexture(atlas, nullptr, nullptr, &qw, &qh);
        aw = static_cast<float>(qw), ah = static_cast<float>(qh);
    }
    int pen = r.x;
    uint32_t previous = 0;
    for (uint32_t cp : utf8_decode(s)) {
        if (previous) pen += f.kerning(previous, cp);
        const Font::Glyph& g = f.glyph(cp);
        if (g.src.w > 0 && !aa_) {
            copy(atlas, g.src, SDL_FRect{static_cast<float>(pen + g.xoff), static_cast<float>(r.y),
                                         static_cast<float>(g.src.w), static_cast<float>(g.src.h)}, c);
        } else if (g.src.w > 0) {
            begin(atlas, 4, 6);
            float x0 = static_cast<float>(pen + g.xoff), y0 = static_cast<float>(r.y);
            float x1 = x0 + g.src.w, y1 = y0 + g.src.h;
            float u0 = g.src.x / aw, v0 = g.src.y / ah, u1 = (g.src.x + g.src.w) / aw, v1 = (g.src.y + g.src.h) / ah;
            int a = vertex(x0, y0, c, u0, v0), b = vertex(x1, y0, c, u1, v0);
            int d = vertex(x1, y1, c, u1, v1), e = vertex(x0, y1, c, u0, v1);
            tri(a, b, d);
            tri(a, d, e);
        }
        pen += g.advance;
        previous = cp;
    }
    if (on_text) on_text(s, r);
    return r;
}

}  // namespace ksphmi
