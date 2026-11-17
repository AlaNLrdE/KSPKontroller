// The MechJeb scene's pages: one shared layout for every skin (mechjeb_layout, keypad_layout), the click hit test
// and the composition (paint_mechjeb) that draws them with a skin's CockpitPainter, the cockpit panel's materials:
// group frames, lamps (flags, path, buttons), segment windows (values) and veils. Static art (frames, captions,
// housings, unlit legends, windows) is drawn in the `art` pass, which skins cache per screen size, page and
// ascent path; the live pass draws lit lamps, values, status, click feedback and the keypad overlay.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "canvas.hpp"
#include "cockpit.hpp"
#include "mechjebinput.hpp"
#include "model.hpp"

namespace ksphmi {

struct MjItem {
    MjControl ctl;
    Rect face;           ///< drawn control and click area
    std::string legend;  ///< lamp legend ("" for value windows)
};

struct MjText {
    std::string text;
    float units;
    double x, y;
    Anchor anchor;
    double room;  ///< the text is fitted into this many pixels
    bool dim = false;
    bool bar = false;  ///< on the page bar (key cap colours)
};

/// Where everything of a MechJeb page goes, in pixels, from the screen size, the page and the ascent path alone.
struct MechJebLayout {
    RibbonLayout ribbon;
    Rect bar;  ///< page bar: back button, page title, MechJeb state
    struct Group {
        Rect rect;
        std::string title;
    };
    std::vector<Group> groups;
    std::vector<MjItem> items;
    std::vector<MjText> captions;  ///< static painted captions (labels, units, module summaries)
    // Ascent Guidance page, live areas
    Rect arm_text, status, notice;
    std::array<Rect, 4> flight{};  ///< APOAPSIS, PERIAPSIS, ALTITUDE (km), VERTICAL SPEED (m/s)
    // Menu page: the state line inside each module tile
    std::array<Rect, MJ_MODULE_COUNT> tile_state{};
};
/// `path`: ascent.path of the report shown (0-2), -1 when unknown (no per-path settings).
MechJebLayout mechjeb_layout(const Canvas& c, MjPage page, int path);

struct KeypadLayout {
    Rect rect, entry;
    double title_y = 0, error_y = 0, range_y = 0;
    std::array<Rect, PAD_KEYS> keys{};
};
KeypadLayout keypad_layout(const Canvas& c);

/// The control under (x, y) in canvas pixels: only the keypad's keys while it is open.
std::optional<MjControl> mechjeb_hit(const MechJebLayout& l, bool keypad_open, const KeypadLayout& k, int x, int y);

/// ascent.path of the snapshot's report, -1 when unknown.
int mechjeb_path(const Snapshot& s);

struct MechJebView {
    const MechJebInput* input = nullptr;  ///< page, arming, feedback and keypad; null = the menu
    double now = 0;
};

/// Draw the scene below the skin's own header: the art pass (`art` true) or the live pass.
void paint_mechjeb(Canvas& c, bool art, const Snapshot& s, const MechJebView& view, CockpitPainter& p);

}  // namespace ksphmi
