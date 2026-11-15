#include "segments.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace ksphmi::seg {

namespace {
// Proportions of a digit, in digit heights.
constexpr float WIDTH = .54f, THICK = .13f, GAP = .024f, SKEW = .07f;
constexpr float CELL_GAP = .22f, COLON = .24f, BLANK = .62f, PAD_X = .34f, PAD_Y = .25f, BEZEL = .09f;

enum : uint8_t { A = 1, B = 2, C = 4, D = 8, E = 16, F = 32, G = 64 };

struct Digit {
    float x, y, h;  // top left of the digit's box (unskewed) and its height
    Pt at(float px, float py) const {
        // Slanted as an electroluminescent display: the top leans right, centred on the middle of the box.
        return {x + px + SKEW * (h / 2 - py), y + py};
    }
};

std::vector<Pt> horizontal(const Digit& d, float xa, float xb, float y, float t, float g) {
    float s = t / 2;
    return {d.at(xa + g, y), d.at(xa + g + s, y - s), d.at(xb - g - s, y - s),
            d.at(xb - g, y), d.at(xb - g - s, y + s), d.at(xa + g + s, y + s)};
}

std::vector<Pt> vertical(const Digit& d, float x, float ya, float yb, float t, float g) {
    float s = t / 2;
    return {d.at(x, ya + g), d.at(x + s, ya + g + s), d.at(x + s, yb - g - s),
            d.at(x, yb - g), d.at(x - s, yb - g - s), d.at(x - s, ya + g + s)};
}

// The seven segments of one digit; `grow` thickens them (glow) without moving their centre lines.
void segments(Canvas& c, const Digit& d, uint8_t on, Color color, float grow = 0) {
    const float h = d.h, w = WIDTH * h, t = THICK * h + grow, g = std::max(0.0f, GAP * h - grow / 2);
    const float half = THICK * h / 2, xl = half, xr = w - half, yt = half, ym = h / 2, yb = h - half;
    if (on & A) c.polygon(horizontal(d, xl, xr, yt, t, g), color, false);
    if (on & B) c.polygon(vertical(d, xr, yt, ym, t, g), color, false);
    if (on & C) c.polygon(vertical(d, xr, ym, yb, t, g), color, false);
    if (on & D) c.polygon(horizontal(d, xl, xr, yb, t, g), color, false);
    if (on & E) c.polygon(vertical(d, xl, ym, yb, t, g), color, false);
    if (on & F) c.polygon(vertical(d, xl, yt, ym, t, g), color, false);
    if (on & G) c.polygon(horizontal(d, xl, xr, ym, t, g), color, false);
}

void dot(Canvas& c, const Digit& d, float px, float py, float size, Color color) {
    float s = size / 2;
    c.polygon({d.at(px - s, py - s), d.at(px + s, py - s), d.at(px + s, py + s), d.at(px - s, py + s)}, color, false);
}

// Opaque blend over the dark glass: SDL's software renderer draws translucent triangles through a temporary
// surface (slow), so the glow is pre-mixed with the window colour instead of drawn with alpha.
Color over_glass(Color c, float t) {
    auto m = [&](uint8_t a, uint8_t b) { return uint8_t(std::lround(b + (a - b) * t)); };
    return Color{m(c.r, WINDOW.r), m(c.g, WINDOW.g), m(c.b, WINDOW.b), 255};
}
}  // namespace

uint8_t bits(char ch) {
    switch (std::toupper(static_cast<unsigned char>(ch))) {
        case '0': case 'O': return A | B | C | D | E | F;
        case '1': case 'I': return B | C;
        case '2': case 'Z': return A | B | D | E | G;
        case '3': return A | B | C | D | G;
        case '4': return B | C | F | G;
        case '5': case 'S': return A | C | D | F | G;
        case '6': return A | C | D | E | F | G;
        case '7': return A | B | C;
        case '8': return A | B | C | D | E | F | G;
        case '9': return A | B | C | D | F | G;
        case '-': return G;
        case '_': return D;
        case 'A': return A | B | C | E | F | G;
        case 'B': return C | D | E | F | G;
        case 'C': return A | D | E | F;
        case 'D': return B | C | D | E | G;
        case 'E': return A | D | E | F | G;
        case 'F': return A | E | F | G;
        case 'G': return A | C | D | E | F;
        case 'H': return B | C | E | F | G;
        case 'J': return B | C | D | E;
        case 'L': return D | E | F;
        case 'N': return C | E | G;
        case 'P': return A | B | E | F | G;
        case 'R': return E | G;
        case 'T': return D | E | F | G;
        case 'U': return B | C | D | E | F;
        case 'Y': return B | C | D | F | G;
        default: return 0;
    }
}

Layout layout(const std::string& pattern, float digit_h) {
    Layout l;
    l.pattern = pattern;
    l.digit_h = digit_h;
    float x = 0;
    for (size_t i = 0; i < pattern.size(); ++i) {
        char k = pattern[i];
        float w = k == '8' ? WIDTH * digit_h : k == ':' ? COLON * digit_h : BLANK * digit_h;
        if (i && k == '8' && pattern[i - 1] == '8') x += CELL_GAP * digit_h;  // room for the decimal point
        l.cells.push_back({k, x, w});
        x += w;
    }
    l.pad_x = PAD_X * digit_h, l.pad_y = PAD_Y * digit_h;
    l.w = std::round(x + 2 * l.pad_x + SKEW * digit_h), l.h = std::round(digit_h + 2 * l.pad_y);
    return l;
}

std::vector<Lit> assign(const std::string& pattern, const std::string& text) {
    std::vector<Lit> out(pattern.size());
    int ti = static_cast<int>(text.size()) - 1;
    bool dp = false;
    for (int pi = static_cast<int>(pattern.size()) - 1; pi >= 0; --pi) {
        while (ti >= 0 && text[ti] == '.') dp = true, --ti;
        if (ti < 0) break;
        const char k = pattern[pi], ch = text[ti];
        if (k == '8') {
            if (ch == ':') {  // no colon cell here: drop the colon, keep this digit cell for the next character
                --ti, ++pi;
                continue;
            }
            out[pi].segments = bits(ch), out[pi].dp = dp, dp = false;
            --ti;
        } else if (k == ':') {
            if (ch == ':') out[pi].colon = true, --ti;
        } else if (ch == ' ') {
            --ti;
        }
    }
    return out;
}

void window(Canvas& c, float x, float y, const Layout& l, Color ghost) {
    const float h = l.digit_h, b = std::max(2.0f, std::round(BEZEL * h));
    Rect outer = Rect::f(x, y, l.w, l.h);
    c.rounded_rect(outer.inflate(2, 2), std::max(2.0f, .16f * h), Color{196, 199, 200});  // light edge of the cut-out
    c.rounded_rect(outer, std::max(2.0f, .14f * h), Color{34, 36, 38});
    Rect glass = outer.inflate(-2 * b, -2 * b);
    c.rounded_rect(glass, std::max(1.0f, .08f * h), WINDOW);
    // glass: a faint reflection across the top
    c.fill_rect(float(glass.x + b), float(glass.y + 1), float(glass.w - 2 * b), std::max(1.0f, .03f * h), Color{30, 38, 36});
    for (size_t i = 0; i < l.cells.size(); ++i) {
        const Cell& cell = l.cells[i];
        Digit d{x + l.pad_x + cell.x, y + l.pad_y, h};
        if (cell.kind == '8') {
            segments(c, d, 0x7F, ghost);
            dot(c, d, WIDTH * h + CELL_GAP * h / 2, h - THICK * h / 2, THICK * h, ghost);
        } else if (cell.kind == ':') {
            dot(c, d, cell.w / 2, .32f * h, THICK * h, ghost);
            dot(c, d, cell.w / 2, .72f * h, THICK * h, ghost);
        }
    }
}

void draw(Canvas& c, float x, float y, const Layout& l, const std::string& text, Color on) {
    const float h = l.digit_h;
    const auto lit = assign(l.pattern, text);
    const Color glow = over_glass(on, .24f);
    for (int pass = 0; pass < 2; ++pass) {
        const Color color = pass ? on : glow;
        const float grow = pass ? 0 : std::max(2.0f, .07f * h);
        for (size_t i = 0; i < l.cells.size(); ++i) {
            const Cell& cell = l.cells[i];
            Digit d{x + l.pad_x + cell.x, y + l.pad_y, h};
            if (cell.kind == '8') {
                if (lit[i].segments) segments(c, d, lit[i].segments, color, grow);
                if (lit[i].dp) dot(c, d, WIDTH * h + CELL_GAP * h / 2, h - THICK * h / 2, THICK * h + grow, color);
            } else if (cell.kind == ':' && lit[i].colon) {
                dot(c, d, cell.w / 2, .32f * h, THICK * h + grow, color);
                dot(c, d, cell.w / 2, .72f * h, THICK * h + grow, color);
            }
        }
    }
}

}  // namespace ksphmi::seg
