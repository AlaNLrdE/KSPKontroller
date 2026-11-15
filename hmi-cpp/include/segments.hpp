// Seven-segment readouts drawn as polygons (the "apollo" skin): electroluminescent digits on a dark window.
//
// A readout has a fixed pattern of cells: '8' a digit cell (seven segments and a decimal point), ':' a colon,
// ' ' a narrow blank gap. The window, its glass bezel and every unlit ("ghost") segment are static and belong in
// cached panel art (`window`); per frame only the lit segments are drawn (`draw`).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "canvas.hpp"

namespace ksphmi::seg {

/// Segment bits: a (top) b (top right) c (bottom right) d (bottom) e (bottom left) f (top left) g (middle).
uint8_t bits(char c);

struct Cell {
    char kind;  ///< '8', ':' or ' '
    float x, w; ///< from the left of the first cell, in pixels
};

/// Cells of `pattern` for digits `digit_h` pixels tall, and the outer size of the window around them.
struct Layout {
    std::string pattern;
    std::vector<Cell> cells;
    float digit_h = 0, pad_x = 0, pad_y = 0, w = 0, h = 0;
};
Layout layout(const std::string& pattern, float digit_h);

/// Which cell shows what: `text` aligned to the right of the pattern. A '.' lights the decimal point of the digit
/// before it, ':' and ' ' in the text match the separator cells, other characters fill digit cells (unknown ones
/// stay dark). Characters that do not fit are dropped from the left.
struct Lit {
    uint8_t segments = 0;
    bool dp = false, colon = false;
};
std::vector<Lit> assign(const std::string& pattern, const std::string& text);

struct Style {
    Color on, ghost;
};
constexpr Color WINDOW{7, 10, 9}, GREEN{132, 255, 150}, AMBER{255, 184, 48}, RED{255, 74, 52};
constexpr Color GHOST{22, 34, 26};

/// Static art: bezel, dark glass and ghost segments, with the window's top left corner at (x, y).
void window(Canvas& c, float x, float y, const Layout& l, Color ghost = GHOST);
/// Lit segments (with a faint glow) of `text`.
void draw(Canvas& c, float x, float y, const Layout& l, const std::string& text, Color on);

}  // namespace ksphmi::seg
