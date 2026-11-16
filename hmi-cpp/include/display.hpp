// The screens (port of display.py), laid out in units of the screen height so the same code fits the Pi monitor
// (2520x1680, 3:2), a 1920x1080 TV or a development window.
#pragma once

#include <array>
#include <functional>
#include <map>
#include <tuple>
#include <memory>
#include <string>
#include <optional>

#include "canvas.hpp"
#include "config.hpp"
#include "model.hpp"
#include "navball.hpp"
#include "netinfo.hpp"

namespace ksphmi {

class ApolloSkin;

/// The flight screen's right column, the same in both skins: ORBIT and VESSEL side by side on top, the NavBall
/// below. VESSEL is narrow and tall (rockets are), the NavBall keeps the remaining 49 % of the height.
struct FlightLayout {
    Rect orbit, vessel, navball;
};
FlightLayout flight_layout(const Canvas& c);

/// Colours of the VESSEL schematic and legend, per skin.
struct VesselStyle {
    Color background, outline, empty, fuel, heat_outline, text;
};

class Display {
public:
    Display(const Config& cfg, Canvas& canvas);
    ~Display();
    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;

    /// One frame (without presenting it). With the layer cache on, it is drawn into the cached layer (a render
    /// target) and copied to the screen, and the pixels under the NavBall globe are kept for animate().
    void draw(const Snapshot& snap, double now);
    /// A NavBall-only frame between samples: the cached layer, then the globe square redrawn at the current
    /// smoothed attitude. False (nothing drawn) when there is no layer for it: cache off, or the last draw had no
    /// globe (not in flight, no attitude). Any change of data, skin or screen goes through draw(), which rebuilds
    /// the layer, so the cache cannot go stale.
    bool animate();
    /// Turn the layer cache on or off; it stays off when the renderer has no render targets. Returns the state.
    bool set_layer_cache(bool on);
    bool layer_cache() const { return layer_cache_; }

    void header(const Snapshot& s, double now);
    void waiting(const Snapshot& s, double now);
    void facility(const Snapshot& s);
    /// VAB scene with an editor ship report: delta-v, stages, destinations ([vab] in config.toml) and vehicle.
    void editor_report(const Snapshot& s);
    void vab_report(const Snapshot& s);
    /// The highlighted destination: from KSP's delta-v app body (choose_target), else the configured [vab] target.
    TargetChoice vab_target(const ShipReport* report) const;
    void flight(const Snapshot& s);
    void tile(const Rect& rect, const std::string& label, const std::string& value, Color color,
              const std::string& note = "");
    void resources(const Rect& rect, const Snapshot& s);
    /// One resource dial centred on (cx, cy); `room` is the caption width available in its cell.
    void gauge(const Resource& resource, const std::string& label, double cx, double cy, double radius, double room,
               float label_units);
    void staging(const Rect& rect, const Snapshot& s);
    /// ORBIT panel; its contents are clipped to the plot area and the caller's clip is restored.
    void orbit(const Rect& rect, const Snapshot& s);
    /// VESSEL panel (classic skin): frame, title, FUEL / HEAT lamps and the schematic.
    void vessel(const Rect& rect, const Snapshot& s);
    /// The side view inside `area` (fuel levels or heat colours) and its legend, clipped to `area`; the caller's
    /// clip is restored. Parts are drawn farthest first.
    void vessel_contents(const Rect& area, const Snapshot& s, const VesselStyle& style);
    /// What the VESSEL panel shows; Ctrl+0 switches.
    bool vessel_heat() const { return vessel_heat_; }
    void set_vessel_heat(bool heat) { vessel_heat_ = heat; }

    Canvas& canvas() { return c_; }
    Navball& navball() { return navball_; }
    int w() const { return c_.w; }
    int h() const { return c_.h; }
    double u() const { return c_.u; }

    /// Layout hooks for tests: every metric tile, and where the live vessel ring was drawn.
    std::function<void(const Rect&)> on_tile_begin, on_tile_end;
    /// Called with the bounding square of every resource dial.
    std::function<void(const std::string&, const Rect&)> on_gauge;
    std::optional<Pt> last_vessel_marker;
    /// Called with every panel / tile of the VAB report (tests: text stays inside, panels do not overlap).
    std::function<void(const std::string&, const Rect&)> on_vab_panel;
    const Config& config() const { return cfg_; }
    /// Where the maneuver node diamond was drawn, and how often the dashed post-burn orbit was rebuilt.
    std::optional<Pt> last_node_marker;
    /// ORBIT draws its own title and INERTIAL caption (the apollo skin paints them on the bezel instead).
    bool panel_titles = true;
    /// The skin drawn now: "classic" or "apollo". The context reply's skin (simulator) wins over [display] skin.
    const std::string& skin() const { return skin_; }
    static bool known_skin(const std::string& name);
    /// Switch at once (logged); unknown names are ignored. The next draw re-applies the context or config choice.
    void set_skin(const std::string& name, const char* why = "request");
    /// Skin chosen on the keyboard (1 classic, 2 apollo): wins over the context reply and [display] skin.
    /// "" returns to those. Unknown names are ignored.
    void choose_skin(const std::string& name);
    /// ORBIT's body and apsis labels go through this when set (the apollo skin paints them in its lettering).
    std::function<Rect(const std::string&, float, Color, double, double, Anchor)> orbit_label;
    ApolloSkin* apollo() { return apollo_.get(); }
    int maneuver_builds = 0;

private:
    /// Dashed post-burn orbit in view-plane metres, rebuilt only when the node orbit or the quantised scale change.
    struct ManeuverCache {
        bool valid = false;
        Vec3 position, velocity, axis_x, axis_y;
        double mu = 0, soi = 0, scale = 0, dash = 0;
        int w = 0, h = 0;
        std::vector<std::vector<Point2>> dashes;
    };
    ManeuverCache maneuver_cache_;
    void maneuver_orbit(const Snapshot& s, const Conic& next, const ConicSpan& span, double scale, double reach,
                        double cx, double cy, const Rect& rect, bool markers);

    void orbit_contents(const Rect& rect, const Snapshot& s);
    void inertial_orbit(const Rect& rect, const Snapshot& s);
    int apsis_label_width();
    Rect orbit_text(const std::string& s, float units, Color color, double x, double y, Anchor a);
    /// Static dial face (chrome bezel, shaded face, scale, E/F, resource icon, odometer window), rendered once
    /// per size, state (live, unknown, absent) and resource at twice the resolution, smoothed down and kept as a
    /// texture.
    SDL_Texture* gauge_face(int radius, int state, const std::string& name);

    struct GaugeLayout {
        Rect rect;
        std::vector<int> names;
        double radius = 0;
        std::vector<std::array<double, 4>> cells;  // cx, cy, cell_x, cell_w
        float label_units = 0;
    };
    std::optional<GaugeLayout> gauge_layout_;
    std::map<std::tuple<int, int, std::string>, SDL_Texture*> gauge_faces_;

    void draw_screen(const Snapshot& snap, double now);
    void capture_under_globe(const Rect& square);
    void free_layer();
    bool layer_cache_ = false, layer_valid_ = false;
    SDL_Texture* layer_ = nullptr;  ///< full screen, everything of the last draw()
    SDL_Texture* under_ = nullptr;  ///< what was under the globe square before the globe was drawn
    Rect under_rect_;
    int under_w_ = 0, under_h_ = 0;

    Canvas& c_;
    int day_hours_;
    std::string server_text_;
    NetInfo net_;
    Navball navball_;
    Config cfg_;
    std::string config_skin_, skin_, chosen_skin_;
    bool vessel_heat_ = false;
    std::unique_ptr<ApolloSkin> apollo_;
};

}  // namespace ksphmi
