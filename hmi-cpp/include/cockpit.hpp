// The cockpit panel scene: one shared layout for every skin (cockpit_layout), the click hit test, and the
// composition (paint_cockpit) that asks a skin's CockpitPainter for each material: group frames, lamps, toggle
// levers, readouts, key caps and guards. Every skin keeps its own lamps, lettering and panel art; the composition
// only decides what goes where and which lamp is lit. Static art (frames, housings, unlit legends, captions) is
// drawn in the `art` pass, which skins cache per screen size; the live pass draws lit lamps, lever positions,
// values and click feedback.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "canvas.hpp"
#include "cockpitinput.hpp"
#include "model.hpp"

namespace ksphmi {

/// The scene ribbon below the header (cockpit panel and MechJeb scenes): a tab per FlightScene.
struct RibbonLayout {
    Rect ribbon;
    std::array<Rect, FLIGHT_SCENES> tabs{};
};
RibbonLayout ribbon_layout(const Canvas& c);

/// Where everything of the panel goes, in pixels, from the screen size alone (same for every skin).
struct CockpitLayout {
    Rect ribbon, attitude, staging, recovery, systems, groups, strip;
    std::array<Rect, CTL_COUNT> face{};   ///< the button, lamp or switch cell: also the click area
    std::array<Rect, CTL_COUNT> key{};    ///< the KSP key cap under the control (empty: none)
    std::array<Rect, CTL_COUNT> lever{};  ///< switches: the lever inside the cell (lamp = face top)
    std::array<Rect, CTL_COUNT> lamp{};   ///< switches: the name lamp at the top of the cell
    Rect stage_readout, next_list, speed_readout, throttle_bar, throttle_readout, warp_readout, arm_lamp;
    std::array<Rect, 4> chute_lamps{};    ///< STOWED, ARMED, OPEN, CUT
    std::array<Rect, 3> safety_lamps{};   ///< SAFE, RISKY, UNSAFE
    struct Caption {
        std::string text;
        float units;
        double x, y;
        Anchor anchor;
        bool ribbon = false;  ///< on the dark scene ribbon (key cap colours)
    };
    std::vector<Caption> captions;  ///< static painted captions (SAS MODE, NEXT STAGE ...)
};
CockpitLayout cockpit_layout(const Canvas& c);

/// The control under (x, y) in canvas pixels, if any.
std::optional<Ctl> cockpit_hit(const CockpitLayout& l, int x, int y);

enum class Lamp { Off, Green, White, Amber, Red };

/// A skin's materials for the panel. The defaults are plain shapes in the painter's colours; skins override what
/// they have (Apollo lamps and segment windows, KSP buttons ...).
class CockpitPainter {
public:
    virtual ~CockpitPainter() = default;
    // Colours used by the defaults and by the composition's own text and icons.
    virtual Color ink() const = 0;       ///< painted captions and legends
    virtual Color dim_ink() const = 0;   ///< secondary captions, unlit symbols
    virtual Color lit_ink() const { return Color{14, 20, 14}; }  ///< text and symbols on a lit face
    virtual Color unlit_ink() const { return dim_ink(); }        ///< symbols on an unlit lamp face
    /// Symbols drawn by the composition on a lamp face (SAS mode icons): lit or unlit.
    virtual Color face_ink(Lamp lit) const { return lit != Lamp::Off ? lit_ink() : unlit_ink(); }
    virtual Color veil_ink() const { return Color{226, 228, 220}; }  ///< text over a grey veil
    virtual Color lamp_color(Lamp l) const;
    virtual Color well() const { return Color{8, 10, 12}; }
    virtual Color key_fill() const { return Color{26, 30, 34}; }
    virtual Color key_ink() const { return dim_ink(); }

    /// A group frame with its title (art).
    virtual void frame(Canvas& c, const Rect& r, const std::string& title) = 0;
    /// Text in the skin's lettering, fitted into `room` pixels when room > 0.
    virtual void text(Canvas& c, const std::string& s, float units, Color color, double x, double y, Anchor a,
                      double room = 0);
    /// A backlit lamp or push button: art draws the housing and unlit legend, live the lit face (when `lit` is
    /// not Off) with its legend. The legend is fitted inside the face.
    virtual void lamp(Canvas& c, const Rect& r, const std::string& legend, Lamp lit, bool art) = 0;
    /// A toggle lever: art draws its base, live the lever up (`on`) or down; `fitted` false greys it.
    virtual void lever(Canvas& c, const Rect& r, std::optional<bool> on, bool fitted, bool art);
    /// A numeric window: art its glass (and unlit segments for `pattern`), live `text` aligned right.
    virtual void readout(Canvas& c, const Rect& r, const std::string& pattern, const std::string& text, bool art);
    /// The key cap under a control (art).
    virtual void keycap(Canvas& c, const Rect& r, const std::string& key);
    /// A horizontal bar (throttle): art the track, live the fill.
    virtual void bar(Canvas& c, const Rect& r, double fraction, bool art);
    /// The guard over ABORT / CUT / QUICK LOAD: striped when closed, a thin raised frame when open (live).
    virtual void guard(Canvas& c, const Rect& r, bool open);
    /// Grey veil over a control that cannot act now (not fitted, no target, stage lock) (live).
    virtual void veil(Canvas& c, const Rect& r);
    /// Click feedback around a control (live): "sent" outline, "rejected" amber outline.
    virtual void feedback(Canvas& c, const Rect& r, CockpitInput::Feedback f, double now);
};

/// The input state the live pass shows (armed, open guards, feedback); null in the art pass or without clicks.
struct CockpitView {
    const CockpitInput* input = nullptr;
    double now = 0;
};

/// The ribbon with its captions and tabs, `shown` lit (art: ribbon, captions and unlit tabs; live: the lit tab).
void paint_ribbon(Canvas& c, bool art, FlightScene shown, CockpitPainter& p);

/// Draw the panel below the skin's own header: the art pass (`art` true) or the live pass.
void paint_cockpit(Canvas& c, bool art, const Snapshot& s, const CockpitView& view, CockpitPainter& p);

/// The Classic skin's materials (theme.hpp colours, rounded cards).
class ClassicCockpitPainter : public CockpitPainter {
public:
    Color ink() const override;
    Color dim_ink() const override;
    Color unlit_ink() const override { return Color{196, 208, 222}; }
    Color key_fill() const override { return Color{30, 40, 54}; }
    void frame(Canvas& c, const Rect& r, const std::string& title) override;
    void lamp(Canvas& c, const Rect& r, const std::string& legend, Lamp lit, bool art) override;
};

}  // namespace ksphmi
