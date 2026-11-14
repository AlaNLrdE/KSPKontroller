// The screens (port of display.py), laid out in units of the screen height so the same code fits the Pi monitor
// (2520x1680, 3:2), a 1920x1080 TV or a development window.
#pragma once

#include <functional>
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

    /// One frame (without presenting it).
    void draw(const Snapshot& snap, double now);

    void header(const Snapshot& s, double now);
    void waiting(const Snapshot& s, double now);
    void facility(const Snapshot& s);
    void flight(const Snapshot& s);
    void tile(const Rect& rect, const std::string& label, const std::string& value, Color color,
              const std::string& note = "");
    void resources(const Rect& rect, const Snapshot& s);
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
    std::optional<Pt> last_vessel_marker;

private:
    void orbit_contents(const Rect& rect, const Snapshot& s);
    void inertial_orbit(const Rect& rect, const Snapshot& s);
    int apsis_label_width();

    Canvas& c_;
    int day_hours_;
    std::string server_text_;
    NetInfo net_;
    Navball navball_;
};

}  // namespace ksphmi
