// Layout and clipping regressions ported from hmi-pi/tests, drawn off-screen with SDL's software renderer.
// Needs the TTF font (fonts-freefont-ttf, or KSPHMI_FONT).
#include <SDL.h>

#include <chrono>
#include <memory>

#include "check.hpp"
#include "config.hpp"
#include "display.hpp"
#include "log.hpp"
#include "scenes.hpp"
#include "telemetry.hpp"
#include "theme.hpp"

using namespace ksphmi;

namespace {

struct Screen {
    Screen(int w, int h) {
        surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
        renderer = SDL_CreateSoftwareRenderer(surface);
        canvas = std::make_unique<Canvas>(renderer, w, h, false);
        display = std::make_unique<Display>(Config{}, *canvas);
    }
    ~Screen() {
        display.reset();
        canvas.reset();
        SDL_DestroyRenderer(renderer);
        SDL_FreeSurface(surface);
    }
    uint32_t pixel(int x, int y) const {
        return static_cast<const uint32_t*>(surface->pixels)[y * (surface->pitch / 4) + x] & 0xFFFFFF;
    }
    /// Every pixel outside `allowed` still has the fill colour.
    bool untouched_outside(const Rect& allowed, uint32_t fill) const {
        for (int y = 0; y < surface->h; ++y)
            for (int x = 0; x < surface->w; ++x) {
                bool inside = x >= allowed.x && x < allowed.right() && y >= allowed.y && y < allowed.bottom();
                if (!inside && pixel(x, y) != fill) return false;
            }
        return true;
    }
    void fill(Color c) { canvas->clear(c); }

    SDL_Surface* surface;
    SDL_Renderer* renderer;
    std::unique_ptr<Canvas> canvas;
    std::unique_ptr<Display> display;
};

const std::pair<int, int> SIZES[] = {{1260, 840}, {1920, 1080}, {2520, 1680}};
constexpr Color FILL{1, 2, 3};
constexpr uint32_t FILL_PIXEL = 0x010203;

Rect orbit_panel(const Screen& s) {
    double u = s.canvas->u;
    return Rect::f(s.canvas->w * .44 + 1.6 * u, 13 * u, s.canvas->w * .56 - 4.1 * u, 22 * u);
}

Snapshot demo(double ut = 0) { return DemoSource().sample(ut); }

}  // namespace

TEST(compact_metrics_leave_space_for_resources_and_staging) {
    for (auto [w, h] : SIZES) {
        Screen s(w, h);
        Display& d = *s.display;
        std::vector<Rect> tiles, active;
        d.on_tile_begin = [&](const Rect& r) { tiles.push_back(r), active.push_back(r); };
        d.on_tile_end = [&](const Rect&) { active.pop_back(); };
        s.canvas->on_text = [&](const std::string& label, const Rect& r) {
            Rect bounds = active.empty() ? Rect(0, 0, w, h) : active.back();
            if (!bounds.contains(r)) check::fail(__FILE__, __LINE__, "text outside its tile: " + label);
        };
        Snapshot base = demo();
        Systems all;
        all.current_stage = 3;
        for (const char* name : RESOURCE_NAMES) all.resources.push_back({name, 7.5, 100.0});
        for (int n : {3, 2, 1, 0}) all.stages.push_back({n, 10, 1234.0});
        Systems empty;
        empty.current_stage = 0;
        for (const std::optional<Systems>& systems : {base.systems, std::optional<Systems>{}, std::optional<Systems>{empty},
                                                      std::optional<Systems>{all}}) {
            tiles.clear();
            Snapshot snap = base;
            snap.systems = systems;
            d.draw(snap, monotonic());
            CHECK_EQ(tiles.size(), size_t(10));
            int top = h, bottom = 0;
            for (const Rect& r : tiles) top = std::min(top, r.y), bottom = std::max(bottom, r.bottom());
            CHECK(bottom - top < h * .40);
            CHECK(bottom < h * .54);
        }
    }
}

TEST(resource_dials_and_their_text_stay_inside_the_panel) {
    for (auto [w, h] : SIZES) {
        Screen s(w, h);
        Display& d = *s.display;
        double u = s.canvas->u;
        Rect panel = Rect::f(2.5 * u, 54 * u, w * .44 - 2.5 * u, 25 * u);
        std::vector<std::pair<std::string, Rect>> texts;
        int dials = 0;
        d.on_gauge = [&](const std::string& label, const Rect& r) {
            ++dials;
            if (!panel.contains(r)) check::fail(__FILE__, __LINE__, "dial outside the panel: " + label);
        };
        s.canvas->on_text = [&](const std::string& t, const Rect& r) {
            if (panel.collides(r)) texts.emplace_back(t, r);
        };
        Snapshot snap = demo();
        Systems mixed;
        mixed.current_stage = 0;
        mixed.resources = {{"ElectricCharge", 4.0, 200.0}, {"LiquidFuel", 300.0, 1440.0}, {"Oxidizer", std::nullopt, std::nullopt},
                           {"MonoPropellant", 0.0, 0.0}, {"SolidFuel", 12345.6, 12345.6}, {"XenonGas", 0.0, 5700.0}};
        for (const Systems& systems : {*snap.systems, mixed}) {
            texts.clear();
            dials = 0;
            snap.systems = systems;
            d.draw(snap, monotonic());
            CHECK(dials >= 4);
            CHECK(texts.size() > 6);
            for (const auto& [t, r] : texts)
                if (!panel.contains(r)) check::fail(__FILE__, __LINE__, "text outside the resources panel: " + t);
            for (const auto& [ta, ra] : texts)
                for (const auto& [tb, rb] : texts)
                    if (&ta != &tb && (ta == "ELECTRICITY" || ta == "MONOPROPELLANT" || ta == "XENON GAS") &&
                        (tb == "ELECTRICITY" || tb == "MONOPROPELLANT" || tb == "XENON GAS") && ra.collides(rb))
                        check::fail(__FILE__, __LINE__, "dial labels overlap: " + ta + " / " + tb);
        }
    }
}

TEST(escape_orbit_and_far_vessel_never_draw_outside_panel) {
    for (auto [w, h] : SIZES) {
        Screen s(w, h);
        Rect panel = orbit_panel(s);
        Snapshot snap;
        for (double e : {1.001, 1.5, 8.0}) {
            snap = demo();
            snap.body = "Mun", snap.body_radius = 200000, snap.atmosphere_depth = 0;
            snap.eccentricity = e, snap.semi_major_axis = -1e6;
            snap.true_anomaly = std::acos(-1 / e) - .00001;  // near an asymptote, far beyond the view
            snap.orbit_state.reset();
            s.fill(FILL);
            Rect before = s.canvas->clip();
            s.display->orbit(panel, snap);
            s.canvas->flush();
            CHECK(s.canvas->clip() == before);
            CHECK(s.untouched_outside(panel, FILL_PIXEL));
        }
        Rect custom = panel.inflate(-4, -4);  // a caller's tighter clip survives too
        s.canvas->set_clip(custom);
        s.display->orbit(panel, snap);
        CHECK(s.canvas->clip() == custom);
        s.canvas->set_clip(Rect(0, 0, w, h));
    }
}

TEST(inertial_marker_stays_put_and_inertial_escape_stays_clipped) {
    double mu = 3.5316e12, radius = 700000, speed = std::sqrt(mu / radius);
    Axes axes = view_axes({radius, 0, 0}, {0, 0, speed});
    for (auto [w, h] : SIZES) {
        Screen s(w, h);
        Rect panel = orbit_panel(s);
        std::vector<Pt> positions;
        for (auto [delta, anomaly] : {std::pair<double, double>{.05, 0}, {-.05, PI}}) {
            Snapshot snap = demo();
            snap.orbit_state = OrbitState{{radius, 0, 0}, {0, 0, speed + delta}, mu, axes};
            snap.true_anomaly = anomaly;  // deliberately flipped: the marker must use the Cartesian position
            s.display->orbit(panel, snap);
            CHECK(s.display->last_vessel_marker.has_value());
            if (s.display->last_vessel_marker) positions.push_back(*s.display->last_vessel_marker);
        }
        if (positions.size() == 2) CHECK(std::hypot(positions[0].x - positions[1].x, positions[0].y - positions[1].y) < 1);

        // Far out on an actual inertial hyperbola: neither curve nor marker may leak.
        double peri = 280000, gm = 6.5e10;
        OrbitState state{{peri, 0, 0}, {0, 0, std::sqrt(gm * 2.5 / peri)}, gm, axes};
        Conic conic = *state.conic();
        double anomaly = std::acos(-1 / conic.eccentricity) - .00001;
        state.position = *conic.position(anomaly);
        double k = std::sqrt(gm / conic.p);
        state.velocity = (conic.peri_direction * -std::sin(anomaly) + conic.transverse * (conic.eccentricity + std::cos(anomaly))) * k;
        Snapshot snap = demo();
        snap.body = "Mun", snap.body_radius = 200000, snap.atmosphere_depth = 0, snap.orbit_state = state;
        s.fill(FILL);
        s.display->orbit(panel, snap);
        s.canvas->flush();
        CHECK(s.untouched_outside(panel, FILL_PIXEL));
    }
}

TEST(navball_labels_fit_and_globe_never_leaks_into_other_panels) {
    for (auto [w, h] : SIZES) {
        Screen s(w, h);
        double u = s.canvas->u;
        Rect panel = Rect::f(w * .44 + 1.6 * u, 36.6 * u, w * .56 - 4.1 * u, 60.9 * u);
        std::map<std::string, Rect> labels;
        s.canvas->on_text = [&](const std::string& label, const Rect& r) {
            if (!panel.contains(r)) check::fail(__FILE__, __LINE__, "navball text outside its panel: " + label);
            labels[label] = r;
        };
        Snapshot snap = demo();
        const double attitudes[][3] = {{0, 0, 0}, {90, 0, 90}, {-90, 270, -90}, {10, 125, 180}};
        for (const auto& a : attitudes) {
            s.fill(FILL);
            Navigation nav = *snap.nav;
            nav.rotation = attitude(a[0], a[1], a[2]);
            nav.pitch = a[0], nav.heading = a[1], nav.roll = a[2];
            labels.clear();
            s.display->navball().draw(*s.canvas, panel, nav);
            s.canvas->flush();
            char hdg[32];
            std::snprintf(hdg, sizeof hdg, "HDG %03d\xC2\xB0", static_cast<int>(a[1]));
            CHECK(labels.count(hdg));
            for (const char* label : {"HOLD", "PRO", "RET", "N+", "N-", "R+", "R-", "TGT", "ANTI", "NODE"}) {
                CHECK(labels.count(label));
                if (labels.count(hdg) && labels.count(label)) CHECK(!labels[hdg].collides(labels[label]));
            }
            CHECK_EQ(s.pixel(panel.x - 1, panel.centery()), FILL_PIXEL);
        }
        s.display->navball().draw(*s.canvas, panel, Navigation{});
        s.display->navball().draw(*s.canvas, panel, std::nullopt);
        Navigation broken;
        broken.rotation = {0, 0, 0, 0};  // a zero quaternion must not take the screen down
        s.display->navball().draw(*s.canvas, panel, broken);
    }
}

TEST(apsis_labels_stay_inside_orbit_panel_as_orbit_grows) {
    for (auto [w, h] : SIZES) {
        Screen s(w, h);
        double u = s.canvas->u;
        Rect panel = orbit_panel(s);
        std::vector<std::string> seen;
        s.canvas->on_text = [&](const std::string& label, const Rect& r) {
            if (label != "Ap" && label != "Pe") return;
            if (!panel.inflate(-2 * u, -2 * u).contains(r)) check::fail(__FILE__, __LINE__, "apsis label outside: " + label);
            seen.push_back(label);
        };
        for (double apoapsis : {90e3, 250e3, 2e6, 50e6, 1e12}) {
            Snapshot snap = DemoSource(20, 10, 80000, apoapsis).sample(0);
            s.display->orbit(panel, snap);
            snap.orbit_state.reset();  // the fallback view without the inertial state
            s.display->orbit(panel, snap);
        }
        CHECK_EQ(seen.size(), size_t(20));
        Snapshot hyperbolic = demo();
        hyperbolic.eccentricity = 1.5, hyperbolic.semi_major_axis = -1e6;
        hyperbolic.orbit_state.reset();
        s.display->orbit(panel, hyperbolic);
        CHECK(!seen.empty() && seen.back() == "Pe");
    }
}

TEST(all_views_render_within_screen) {
    for (auto [w, h] : {std::pair<int, int>{1260, 840}, {1920, 1080}}) {
        Screen s(w, h);
        s.canvas->on_text = [&](const std::string& label, const Rect& r) {
            if (!Rect(0, 0, w, h).contains(r)) check::fail(__FILE__, __LINE__, "text off screen: " + label);
        };
        for (const auto& v : views()) {
            Snapshot snap;
            snap.state = State::NoVessel;
            snap.scene = v.key;
            s.display->draw(snap, monotonic());
        }
        Snapshot waiting;
        waiting.detail = "connecting to kRPC at 192.168.1.2:50000";
        s.display->draw(waiting, monotonic());
        Snapshot no_vessel;
        no_vessel.state = State::NoVessel;
        s.display->draw(no_vessel, monotonic());
        Snapshot old = demo();
        old.received = monotonic() - 10;  // stale: NO DATA header
        s.display->draw(old, monotonic());
    }
}

TEST(full_frame_software_render_time) {
    Screen s(2520, 1680);
    DemoSource src;
    s.display->draw(src.sample(0), monotonic());  // warm-up: fonts, label atlas, lighting
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 1; i <= 5; ++i) s.display->draw(src.sample(i), monotonic());
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 5;
    std::printf("     software flight frame at 2520x1680: %.1f ms\n", ms);
    auto time = [&](const char* what, const std::function<void()>& f) {
        auto a = std::chrono::steady_clock::now();
        for (int i = 0; i < 5; ++i) f(), s.canvas->flush();
        std::printf("       %-8s %.1f ms\n", what,
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count() / 5);
    };
    Snapshot snap = src.sample(3);
    double u = s.canvas->u;
    time("clear", [&] { s.canvas->clear(theme::BG); });
    time("navball", [&] { s.display->navball().draw(*s.canvas, Rect::f(2520 * .44 + 1.6 * u, 36.6 * u, 2520 * .56 - 4.1 * u, 60.9 * u), snap.nav); });
    time("orbit", [&] { s.display->orbit(Rect::f(2520 * .44 + 1.6 * u, 13 * u, 2520 * .56 - 4.1 * u, 22 * u), snap); });
    time("header", [&] { s.display->header(snap, monotonic()); });
    time("tile", [&] { s.display->tile(Rect::f(40, 200, 500, 117), "ALTITUDE", "123.4 km", theme::VALUE); });
    time("panel", [&] { theme::panel(*s.canvas, Rect::f(40, 200, 500, 117)); });
    time("text", [&] { s.canvas->text("123.4 km", 4.0, theme::VALUE, 50, 220); });
    time("resource", [&] { s.display->resources(Rect::f(42, 54 * u, 1067, 25 * u), snap); });
    time("staging", [&] { s.display->staging(Rect::f(42, 80.6 * u, 1067, 15 * u), snap); });
    CHECK(ms < 1000);
}

int main(int argc, char** argv) {
    log::set_quiet(true);
    if (SDL_Init(0) != 0) return 2;
    int rc = RUN_TESTS(argc, argv);
    TTF_Quit();
    SDL_Quit();
    return rc;
}
