// The screens (port of display.py), laid out in units of the screen height so the same code fits the Pi monitor
// (2520x1680, 3:2), a 1920x1080 TV or a development window.
#pragma once

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>

#include "canvas.hpp"
#include "config.hpp"
#include "model.hpp"
#include "navball.hpp"
#include "netinfo.hpp"

namespace ksphmi {

class Display {
public:
    Display(const Config& cfg, Canvas& canvas);
    ~Display();
    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;

    /// One frame (without presenting it).
    void draw(const Snapshot& snap, double now);

    void header(const Snapshot& s, double now);
    void waiting(const Snapshot& s, double now);
    void facility(const Snapshot& s);
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

private:
    void orbit_contents(const Rect& rect, const Snapshot& s);
    void inertial_orbit(const Rect& rect, const Snapshot& s);
    int apsis_label_width();
    /// Static dial face (bezel, track, warning zones, ticks), rendered once per size at twice the resolution,
    /// smoothed down and kept as a texture.
    SDL_Texture* gauge_face(int radius, bool live);

    struct GaugeLayout {
        Rect rect;
        std::vector<int> names;
        double radius = 0;
        std::vector<std::array<double, 4>> cells;  // cx, cy, cell_x, cell_w
        float label_units = 0;
    };
    std::optional<GaugeLayout> gauge_layout_;
    std::map<std::pair<int, bool>, SDL_Texture*> gauge_faces_;

    Canvas& c_;
    int day_hours_;
    std::string server_text_;
    NetInfo net_;
    Navball navball_;
};

}  // namespace ksphmi
