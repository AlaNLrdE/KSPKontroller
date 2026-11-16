// Layout and clipping regressions, drawn off-screen with SDL's software renderer.
// Needs the TTF font (fonts-freefont-ttf, or KSPHMI_FONT).
#include <SDL.h>

#include <chrono>
#include <cmath>
#include <iterator>
#include <map>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>

#include "apollo.hpp"
#include "gameskin.hpp"
#include "bootstate.hpp"
#include "check.hpp"
#include "config.hpp"
#include "display.hpp"
#include "log.hpp"
#include "png.hpp"
#include "orbitart.hpp"
#include "navball.hpp"
#include "scenes.hpp"
#include "segments.hpp"
#include "steamdials.hpp"
#include "shipreport.hpp"
#include "splash.hpp"
#include "telemetry.hpp"
#include "theme.hpp"

using namespace ksphmi;

namespace {

Config repo_config() {
    std::ifstream in(std::string(KSPHMI_SOURCE_DIR) + "/config.toml");
    std::stringstream buf;
    buf << in.rdbuf();
    Config cfg;
    apply_toml(cfg, parse_toml(buf.str()));
    return cfg;
}

struct Screen {
    Screen(int w, int h, const std::string& skin = "classic", Config cfg = Config()) {
        surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
        renderer = SDL_CreateSoftwareRenderer(surface);
        canvas = std::make_unique<Canvas>(renderer, w, h, false);
        cfg.display.skin = skin;
        display = std::make_unique<Display>(cfg, *canvas);
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

Rect orbit_panel(const Screen& s) { return flight_layout(*s.canvas).orbit; }

Snapshot demo(double ut = 0) { return DemoSource().sample(ut); }

/// The live game of 2026-11-16: a 79 km orbit of Duna, Ike targeted, a 284 m/s node 1819 s ahead raising Ap to
/// 2.6 Mm and entering Ike's sphere of influence. `encounter` false: the same plan just misses Ike (closest
/// approach markers instead of the encounter).
Snapshot duna_ike(bool encounter) {
    const double MU = 3.0136321e11, R = 320000, MI = 1.8568369e10, RI = 130000, SI = 1049598.5, RO = 3.2e6;
    Snapshot s = demo(1200);
    s.body = "Duna", s.body_radius = R, s.atmosphere_depth = 50000, s.apoapsis = s.periapsis = 79000;
    s.encounters.clear();
    const double r = R + 79000, v = std::sqrt(MU / r);
    auto around = [](double angle, double radius) { return Vec3{std::cos(angle), 0, std::sin(angle)} * radius; };
    auto ahead = [](double angle, double speed) { return Vec3{-std::sin(angle), 0, std::cos(angle)} * speed; };
    const Axes axes{{1, 0, 0}, {0, 0, 1}};
    s.orbit_state = OrbitState{around(0, r), ahead(0, v), MU, axes};
    // The post-burn ellipse from the node: Pe 79,354 m, Ap 2,599,287 m.
    const double node = 1819 * std::sqrt(MU / (r * r * r)), rp = R + 79354, ra = R + 2599287;
    const double vp = std::sqrt(MU * 2 * ra / (rp * (rp + ra)));
    s.maneuver_orbit = ManeuverOrbit{around(node, rp), ahead(node, vp), s.ut + 1819, 47921949};
    // Ike where the planned path meets its sphere (true anomaly 2.66 on the ellipse), and where it is now.
    const double e = (ra - rp) / (ra + rp), p = rp * (1 + e), nu = 2.6625, h = std::sqrt(MU * p);
    const double re = p / (1 + e * std::cos(nu));
    Vec3 entry = around(node + nu, re);
    Vec3 velocity = (around(node + nu, 1) * (MU / h * e * std::sin(nu))) + ahead(node + nu, MU / h * (1 + e * std::cos(nu)));
    const double psi = node + nu - std::acos((RO * RO + re * re - SI * SI) / (2 * RO * re));
    const double vi = std::sqrt(MU / RO), n = std::sqrt(MU / (RO * RO * RO)), te = 6580.8;
    TargetOrbit ike;
    ike.name = "Ike", ike.body = true, ike.radius = RI;
    ike.position = around(psi - n * te, RO), ike.velocity = ahead(psi - n * te, vi);
    s.target_orbit = ike;
    if (encounter) {
        Encounter x;
        x.body = "Ike", x.radius = RI, x.mu = MI, x.soi = SI, x.ut = s.ut + te, x.planned = true;
        x.center = around(psi, RO), x.position = entry - x.center, x.velocity = velocity - ahead(psi, vi);
        s.encounters.push_back(x);
        s.approach.reset();
    } else {  // the plan 0.4 rad earlier: Ike passes 1.6 Mm ahead of the apoapsis
        Vec3 ap = around(node + PI, ra);
        s.approach = ClosestApproach{s.ut + 1819 + 2 * 3600, true, ap, around(node + PI + .45, RO)};
    }
    return s;
}

}  // namespace

TEST(out_of_range_clock_telemetry_renders_in_both_skins) {
    for (const char* skin : {"classic", "apollo", "steampunk", "ksp1", "ksp2"}) {
        Screen s(1260, 840, skin);
        Snapshot snap = demo();
        snap.ut = 1e100;
        s.display->draw(snap, monotonic());
        s.canvas->flush();
    }
}

TEST(compact_metrics_leave_space_for_resources_and_staging) {
    for (const auto& size : SIZES) {
        const int w = size.first, h = size.second;
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
    for (const auto& size : SIZES) {
        const int w = size.first, h = size.second;
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

TEST(flight_layout_panels_tile_the_right_column) {
    for (bool deck : {false, true})
    for (const auto& size : SIZES) {
        Screen s(size.first, size.second);
        const FlightLayout l = flight_layout(*s.canvas, deck);
        CHECK(!l.orbit.collides(l.vessel) && !l.orbit.collides(l.navball) && !l.vessel.collides(l.navball));
        CHECK_EQ(l.orbit.y, l.vessel.y);
        CHECK(l.vessel.w < l.orbit.w);
        if (!deck) CHECK(l.vessel.h > l.vessel.w);  // classic profile keeps a narrow, tall rocket panel
        CHECK(l.navball.right() <= s.canvas->w && l.navball.bottom() <= s.canvas->h);
        CHECK(l.navball.h > l.orbit.h);
    }
}

TEST(vessel_panel_draws_both_views_inside_its_panel) {
    for (const auto& size : SIZES) {
        Screen s(size.first, size.second);
        const Rect panel = flight_layout(*s.canvas).vessel;
        Snapshot snap = demo();
        for (bool heat : {false, true}) {
            s.display->set_vessel_heat(heat);
            for (int variant = 0; variant < 3; ++variant) {
                if (variant == 1) snap.vessel_view.reset();  // no KSPHMIFlight: "NO VESSEL DATA"
                if (variant == 2) {                          // one huge part far from the origin still fits
                    VesselView v;
                    v.parts.push_back(VesselPart{{{1e5, 1e5}, {1e5 + 300, 1e5}, {1e5 + 150, 1e5 + 900}}, 0, 50, 99});
                    snap.vessel_view = std::make_shared<const VesselView>(v);
                }
                s.fill(FILL);
                const Rect before = s.canvas->clip();
                s.display->vessel(panel, snap);
                s.canvas->flush();
                CHECK(s.canvas->clip() == before);
                CHECK(s.untouched_outside(panel, FILL_PIXEL));
            }
            snap = demo();
        }
    }
}

TEST(escape_orbit_and_far_vessel_never_draw_outside_panel) {
    for (const auto& size : SIZES) {
        const int w = size.first, h = size.second;
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
    for (const auto& size : SIZES) {
        const int w = size.first, h = size.second;
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

TEST(nearly_parabolic_apoapsis_is_omitted_when_its_position_is_unavailable) {
    const double mu = KERBIN_MU, radius = KERBIN_RADIUS + 100000, e = .9999995;
    const Vec3 position{radius, 0, 0}, velocity{0, 0, std::sqrt(mu * (1 + e) / radius)};
    OrbitState state{position, velocity, mu, view_axes(position, velocity)};
    auto conic = state.conic();
    CHECK(conic.has_value());
    CHECK(conic->apoapsis().has_value());
    CHECK(!conic->position(PI));
    for (const char* skin : {"classic", "apollo", "steampunk"}) {
        Screen screen(1260, 840, skin);
        Rect panel = orbit_panel(screen);
        for (bool maneuver : {false, true}) {
            Snapshot snap = demo();
            snap.orbit_state = maneuver ? demo().orbit_state : std::optional<OrbitState>{state};
            if (maneuver) snap.maneuver_orbit = ManeuverOrbit{position, velocity, 0};
            else snap.maneuver_orbit.reset();
            int ap_labels = 0;
            screen.canvas->on_text = [&](const std::string& label, const Rect&) {
                if (label == "Ap") ++ap_labels;
            };
            screen.fill(FILL);
            screen.display->orbit(panel, snap);
            screen.canvas->flush();
            CHECK_EQ(ap_labels, maneuver ? 1 : 0);  // only the ordinary vessel orbit has a drawable apoapsis
            CHECK(screen.display->last_vessel_marker.has_value());
            CHECK(screen.untouched_outside(panel, FILL_PIXEL));
        }
    }
}

TEST(navball_labels_fit_and_globe_never_leaks_into_other_panels) {
    for (const auto& size : SIZES) {
        const int w = size.first, h = size.second;
        Screen s(w, h);
        double u = s.canvas->u;
        Rect panel = flight_layout(*s.canvas).navball;
        (void)u;
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
                if (labels.count(hdg) && labels.count(label)) {
                    CHECK(!labels[hdg].collides(labels[label]));
                    CHECK(labels[label].y - labels[hdg].bottom() >= 1.2 * u);  // a visible gap, not touching
                }
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

TEST(target_approach_and_encounter_draw_inside_the_orbit_panel) {
    for (const char* skin : {"classic", "apollo", "steampunk", "ksp1", "ksp2"})
        for (const auto& size : SIZES) {
            Screen s(size.first, size.second, skin);
            for (bool encounter : {false, true}) {
                Snapshot snap = duna_ike(encounter);
                s.display->draw(snap, monotonic());
                s.canvas->flush();
                CHECK(s.display->last_target_marker.has_value());
                CHECK(s.display->last_encounter_marker.has_value() == encounter);
                CHECK(s.display->last_approach_marker.has_value() == !encounter);  // KSP shows one or the other
                Rect panel = orbit_panel(s);
                s.fill(FILL);
                s.display->orbit(panel, snap);
                s.canvas->flush();
                CHECK(s.untouched_outside(panel, FILL_PIXEL));
                for (const auto& marker : {s.display->last_target_marker, s.display->last_encounter_marker})
                    if (marker) CHECK(panel.contains(Rect::f(marker->x, marker->y, 1, 1)));
            }
            // A target vessel around another body never reaches the snapshot (the source checks the parent), and
            // broken values draw nothing rather than garbage.
            Snapshot broken = duna_ike(true);
            broken.target_orbit->position = {NAN, 0, 0};
            broken.encounters.front().mu = 0;
            broken.approach = ClosestApproach{0, false, {NAN, 0, 0}, {0, 0, 0}};
            s.display->draw(broken, monotonic());
            s.canvas->flush();
            CHECK(!s.display->last_target_marker && !s.display->last_encounter_marker && !s.display->last_approach_marker);
        }
}

TEST(orbit_scale_holds_while_optional_data_flickers) {
    Screen s(1920, 1080, "classic");
    Rect panel = orbit_panel(s);
    Snapshot full = duna_ike(true), bare = duna_ike(true);
    bare.maneuver_orbit.reset(), bare.target_orbit.reset(), bare.encounters.clear(), bare.approach.reset();
    auto scale_at = [&](Snapshot snap, double t) {
        snap.received = t;
        s.display->orbit(panel, snap);
        return s.display->last_orbit_scale;
    };
    const double wide = scale_at(full, 100);
    double low = wide, high = wide;
    for (int i = 1; i <= 40; ++i) {  // 4 s of a node / target that comes and goes at every sample
        double k = scale_at(i % 2 ? bare : full, 100 + .1 * i);
        low = std::min(low, k), high = std::max(high, k);
    }
    CHECK(high / low < 1.03);
    double held = scale_at(bare, 104.5);
    CHECK(held / wide < 1.03);  // the bare orbit alone, but only 0.5 s after the plan vanished
    double later = scale_at(bare, 107);
    CHECK(later > 2 * wide);  // gone for over 2 s: the view closes in on the current orbit
    // Content that appears grows the view at once, so the target never draws outside it.
    double grown = scale_at(full, 107.1);
    CHECK_NEAR(grown, wide, wide * .03);
    CHECK(s.display->last_target_marker.has_value());
}

TEST(apsis_labels_stay_inside_orbit_panel_as_orbit_grows) {
    for (const auto& size : SIZES) {
        const int w = size.first, h = size.second;
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
            snap.maneuver_orbit.reset();  // the current orbit's labels (the dashed orbit has its own test)
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

namespace {
int maneuver_pixels(const Screen& s, const Rect& r) {
    int n = 0;
    for (int y = r.y; y < r.bottom(); ++y)
        for (int x = r.x; x < r.right(); ++x)
            if (s.pixel(x, y) == (uint32_t(theme::MANEUVER.r) << 16 | theme::MANEUVER.g << 8 | theme::MANEUVER.b)) ++n;
    return n;
}
}  // namespace

TEST(maneuver_orbit_is_dashed_cached_and_clipped) {
    for (const auto& size : SIZES) {
        const int w = size.first, h = size.second;
        Screen s(w, h);
        double u = s.canvas->u;
        Rect panel = orbit_panel(s);
        std::vector<Rect> labels;
        s.canvas->on_text = [&](const std::string& label, const Rect& r) {
            if (label == "Ap" || label == "Pe") labels.push_back(r);
        };
        Snapshot snap = demo();
        CHECK(snap.maneuver_orbit.has_value());
        s.fill(FILL);
        s.display->orbit(panel, snap);
        s.canvas->flush();
        CHECK(s.display->last_node_marker.has_value());
        CHECK_EQ(s.display->maneuver_builds, 1);
        CHECK(maneuver_pixels(s, panel) > 50);
        CHECK(s.untouched_outside(panel, FILL_PIXEL));
        CHECK_EQ(labels.size(), size_t(4));  // current Ap/Pe and the planned orbit's own Ap/Pe
        for (const Rect& r : labels) CHECK(panel.inflate(-2 * u, -2 * u).contains(r));
        if (s.display->last_node_marker) {  // the diamond sits on the burn point
            Pt node = *s.display->last_node_marker;
            CHECK(s.pixel(int(node.x + .7 * u), int(node.y)) != FILL_PIXEL);
        }
        Snapshot later = snap;
        later.ut += 1, later.altitude += 1;  // a new sample, same node: no rebuild
        s.display->orbit(panel, later);
        s.display->orbit(panel, snap);
        CHECK_EQ(s.display->maneuver_builds, 1);
        snap.maneuver_orbit->velocity = snap.maneuver_orbit->velocity * 1.01;  // node edited: rebuilt once
        s.display->orbit(panel, snap);
        s.display->orbit(panel, snap);
        CHECK_EQ(s.display->maneuver_builds, 2);

        // Escape burns and a post-burn ellipse beyond the sphere of influence: drawn up to it, never outside.
        for (double prograde : {1200.0, 3000.0, 30000.0}) {
            Snapshot escape = demo();
            escape.maneuver_orbit = demo_maneuver(escape.semi_major_axis, escape.eccentricity, KERBIN_MU, .4, prograde);
            s.fill(FILL);
            s.display->orbit(panel, escape);
            s.canvas->flush();
            CHECK(s.untouched_outside(panel, FILL_PIXEL));
            CHECK(maneuver_pixels(s, panel) > 20);
            escape.maneuver_orbit->soi = 1.2e6;  // a small sphere of influence cuts the patch
            s.display->orbit(panel, escape);
        }
        Snapshot broken = demo();
        broken.maneuver_orbit->velocity = {NAN, 0, 0};
        s.display->orbit(panel, broken);
        CHECK(!s.display->last_node_marker);
        broken.maneuver_orbit.reset();
        s.display->orbit(panel, broken);
        CHECK(!s.display->last_node_marker);
        s.canvas->on_text = nullptr;
    }
}

TEST(orbit_fills_the_panel_and_navball_stays_dominant) {
    for (const auto& size : SIZES) {
        const int w = size.first, h = size.second;
        Screen s(w, h);
        double u = s.canvas->u;
        Rect panel = orbit_panel(s);
        s.fill(FILL);
        s.display->orbit(panel, demo());
        s.canvas->flush();
        int top = h, bottom = -1;
        for (int y = panel.y; y < panel.bottom(); ++y)
            for (int x = panel.x; x < panel.right(); ++x) {
                uint32_t p = s.pixel(x, y);
                bool curve = p == (uint32_t(theme::ACCENT.r) << 16 | theme::ACCENT.g << 8 | theme::ACCENT.b) ||
                             p == (uint32_t(theme::MANEUVER.r) << 16 | theme::MANEUVER.g << 8 | theme::MANEUVER.b);
                if (curve) top = std::min(top, y), bottom = std::max(bottom, y);
            }
        // The conics span most of the drawable height (panel minus title and label margins).
        CHECK(bottom - top >= .85 * (panel.h - 9.3 * u));
        // The NavBall gave height to the ORBIT / VESSEL row (user, 2026-11-16); its globe, in both skins, still
        // spans at least 80 % of that row's height.
        const Rect nav = flight_layout(*s.canvas).navball;
        for (bool classic : {false, true})
            CHECK(2 * Navball::globe_geometry(*s.canvas, nav, classic).r >= .8 * panel.h);
    }
}

TEST(all_views_render_within_screen) {
    for (const auto& size : {std::pair<int, int>{1260, 840}, {1920, 1080}}) {
        const int w = size.first, h = size.second;
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
    time("orbit", [&] { s.display->orbit(Rect::f(2520 * .44 + 1.6 * u, 13 * u, 2520 * .56 - 4.1 * u, 27 * u), snap); });
    time("header", [&] { s.display->header(snap, monotonic()); });
    time("tile", [&] { s.display->tile(Rect::f(40, 200, 500, 117), "ALTITUDE", "123.4 km", theme::VALUE); });
    time("panel", [&] { theme::panel(*s.canvas, Rect::f(40, 200, 500, 117)); });
    time("text", [&] { s.canvas->text("123.4 km", 4.0, theme::VALUE, 50, 220); });
    time("resource", [&] { s.display->resources(Rect::f(42, 54 * u, 1067, 25 * u), snap); });
    time("staging", [&] { s.display->staging(Rect::f(42, 80.6 * u, 1067, 15 * u), snap); });
    CHECK(ms < 1000);
}

// ------------------------------------------------------------------------------------- apollo skin

TEST(segment_text_aligns_right_with_decimal_points_and_separators) {
    auto lit = seg::assign("8888888", "12.40");
    CHECK_EQ(int(lit[3].segments), int(seg::bits('1')));
    CHECK(lit[4].dp && !lit[5].dp);
    CHECK_EQ(int(lit[6].segments), int(seg::bits('0')));
    CHECK_EQ(int(lit[0].segments), 0);
    auto ut = seg::assign("88 888 88:88:88", "1 001 05:23:11");
    CHECK_EQ(int(ut[1].segments), int(seg::bits('1')));
    CHECK_EQ(int(ut[3].segments), int(seg::bits('0')));
    CHECK(ut[9].colon && ut[12].colon);
    auto over = seg::assign("888", "12345");  // too long: the left characters are dropped
    CHECK_EQ(int(over[0].segments), int(seg::bits('3')));
    auto l = seg::layout("888:88:88", 40);
    CHECK(l.w > 0 && l.h > 40);
}

TEST(instrument_skins_keep_the_layout_zones_and_text_inside_their_tiles) {
    for (const char* skin : {"apollo", "steampunk"})
    for (const auto& size : SIZES) {
        const int w = size.first, h = size.second;
        Screen s(w, h, skin);
        Display& d = *s.display;
        CHECK_EQ(d.skin(), std::string(skin));
        std::vector<Rect> tiles, active;
        d.on_tile_begin = [&](const Rect& r) { tiles.push_back(r), active.push_back(r); };
        d.on_tile_end = [&](const Rect&) { active.pop_back(); };
        double u = s.canvas->u;
        Rect res = std::string(skin) == "steampunk" ? Rect::f(2.5 * u, 46 * u, w * .44 - 2.5 * u, 18 * u) :
                   Rect::f(1.4 * u, 34 * u, w * .5 - 2.9 * u, 22 * u);
        int meters = 0;
        d.on_gauge = [&](const std::string& label, const Rect& r) {
            ++meters;
            if (!res.contains(r)) check::fail(__FILE__, __LINE__, "meter outside the resources panel: " + label);
        };
        s.canvas->on_text = [&](const std::string& t, const Rect& r) {
            Rect bounds = active.empty() ? Rect(0, 0, w, h) : active.back();
            if (!bounds.contains(r)) check::fail(__FILE__, __LINE__, "text outside its tile: " + t);
        };
        Snapshot base = demo();
        Systems all;
        all.current_stage = 3;
        for (const char* name : RESOURCE_NAMES) all.resources.push_back({name, 7.5, 100.0});
        for (int n : {3, 2, 1, 0}) all.stages.push_back({n, 10, 1234.0});
        Systems mixed;
        mixed.current_stage = 0;
        mixed.resources = {{"ElectricCharge", 4.0, 200.0}, {"LiquidFuel", 300.0, 1440.0}, {"Oxidizer", std::nullopt, std::nullopt},
                           {"MonoPropellant", 0.0, 0.0}, {"SolidFuel", 12345.6, 12345.6}, {"XenonGas", 0.0, 5700.0}};
        for (const std::optional<Systems>& systems : {base.systems, std::optional<Systems>{}, std::optional<Systems>{all},
                                                      std::optional<Systems>{mixed}}) {
            tiles.clear();
            meters = 0;
            Snapshot snap = base;
            snap.systems = systems;
            d.draw(snap, monotonic());
            CHECK_EQ(tiles.size(), size_t(10));
            int top = h, bottom = 0;
            for (const Rect& r : tiles) top = std::min(top, r.y), bottom = std::max(bottom, r.bottom());
            CHECK(bottom - top < h * .40);
            if (systems) CHECK(meters >= 4);
        }
        // escape trajectory and a huge periapsis countdown still fit their windows' tiles
        Snapshot escape = base;
        escape.eccentricity = 1.4, escape.periapsis = -2e6, escape.time_to_periapsis = 9e7;
        d.draw(escape, monotonic());
    }
}

TEST(apollo_panel_art_is_built_once_per_screen_kind) {
    Screen s(1260, 840, "apollo");
    DemoSource src;
    for (int i = 0; i < 4; ++i) s.display->draw(src.sample(i), monotonic());
    CHECK_EQ(s.display->apollo()->builds, 1);
    Snapshot waiting;
    s.display->draw(waiting, monotonic());
    s.display->draw(waiting, monotonic());
    CHECK_EQ(s.display->apollo()->builds, 2);
    // the classic skin is untouched by the option
    Screen classic(1260, 840);
    CHECK(classic.display->apollo() == nullptr);
}

TEST(skin_switches_at_runtime_from_the_context_and_keeps_its_cache) {
    Screen s(1260, 840);  // config: classic
    Snapshot snap = demo();
    s.display->draw(snap, monotonic());
    CHECK_EQ(s.display->skin(), std::string("classic"));
    CHECK(s.display->apollo() == nullptr);  // built lazily, only when first shown
    snap.skin = "apollo";  // the simulator asks for it on the context reply
    s.display->draw(snap, monotonic());
    CHECK_EQ(s.display->skin(), std::string("apollo"));
    CHECK_EQ(s.display->apollo()->builds, 1);
    snap.skin = "classic";
    s.display->draw(snap, monotonic());
    CHECK_EQ(s.display->skin(), std::string("classic"));
    snap.skin = "apollo";
    s.display->draw(snap, monotonic());
    CHECK_EQ(s.display->apollo()->builds, 1);  // its panel art survived the switch: no rebuild, no new texture
    snap.skin = "chrome";  // unknown: back to the [display] setting
    s.display->draw(snap, monotonic());
    CHECK_EQ(s.display->skin(), std::string("classic"));
    snap.skin = "";
    Screen a(1260, 840, "apollo");
    a.display->draw(snap, monotonic());
    CHECK_EQ(a.display->skin(), std::string("apollo"));
}

TEST(instrument_views_render_within_screen) {
    for (const char* skin : {"apollo", "steampunk"})
    for (const auto& size : {std::pair<int, int>{1260, 840}, {1920, 1080}, {2520, 1680}}) {
        const int w = size.first, h = size.second;
        Screen s(w, h, skin);
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
        old.vessel = "A very long vessel name that never fits on the name plate at all";
        old.received = monotonic() - 10;  // stale: NO DATA lamp
        s.display->draw(old, monotonic());
    }
}

TEST(apollo_full_frame_software_render_time) {
    Screen s(2520, 1680, "apollo");
    DemoSource src;
    auto t0 = std::chrono::steady_clock::now();
    s.display->draw(src.sample(0), monotonic());  // builds the panel art
    double build = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    t0 = std::chrono::steady_clock::now();
    for (int i = 1; i <= 5; ++i) s.display->draw(src.sample(i), monotonic());
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 5;
    std::printf("     apollo flight frame at 2520x1680: %.1f ms (panel art built once in %.0f ms)\n", ms, build);
    CHECK(ms < 1000);
}

// [display] skin = "auto" (the default): the KSP1 Stock look for KSP 1 and before any game, KSP2 Redux for Redux;
// the keyboard and the simulator's context reply still win (user, 2026-11-16).
TEST(auto_skin_follows_the_game) {
    Screen s(1280, 800, "auto");
    CHECK_EQ(s.display->skin(), std::string("ksp1"));
    Snapshot waiting;
    s.display->draw(waiting, 100.2);
    CHECK_EQ(s.display->skin(), std::string("ksp1"));
    Snapshot redux = demo(0);
    redux.game = "ksp2";
    s.display->draw(redux, monotonic());
    CHECK_EQ(s.display->skin(), std::string("ksp2"));
    Snapshot ksp1 = demo(0);
    ksp1.game = "ksp1";
    s.display->draw(ksp1, monotonic());
    CHECK_EQ(s.display->skin(), std::string("ksp1"));
    ksp1.skin = "steampunk";  // the simulator's context reply
    s.display->draw(ksp1, monotonic());
    CHECK_EQ(s.display->skin(), std::string("steampunk"));
    s.display->choose_skin("classic");  // Ctrl+1
    s.display->draw(redux, monotonic());
    CHECK_EQ(s.display->skin(), std::string("classic"));
    CHECK_EQ(Config().display.skin, std::string("auto"));
}

// KSPHMI_SHOTS=DIR saves the apollo screens that --demo --screenshot cannot reach (facility, no vessel, stale).
TEST(instrument_review_screenshots_on_request) {
    const char* dir = std::getenv("KSPHMI_SHOTS");
    if (!dir) return;
    for (const char* skin : {"classic", "apollo", "steampunk", "ksp1", "ksp2"}) {
    Screen s(2520, 1680, skin);
    const std::string prefix = std::string(dir) + "/" + skin;
    Snapshot connecting;
    connecting.detail = "Waiting for the telemetry service";
    s.display->draw(connecting, 100.2);
    CHECK(save_png(s.surface, prefix + "_waiting.png"));
    for (const auto& view : views()) {
        Snapshot facility;
        facility.state = State::NoVessel;
        facility.scene = view.key;
        s.display->draw(facility, 100.2);
        CHECK(save_png(s.surface, prefix + "_" + view.key + ".png"));
    }
    Snapshot no_vessel;
    no_vessel.state = State::NoVessel;
    s.display->draw(no_vessel, 100.2);
    CHECK(save_png(s.surface, prefix + "_no_vessel.png"));
    Snapshot warn = demo(1200);
    warn.periapsis = 40000, warn.received = monotonic() - 10;
    for (auto& r : warn.systems->resources)
        if (r.name == "ElectricCharge") r.amount = *r.capacity * .06;
        else if (r.name == "LiquidFuel") r.amount = *r.capacity * .18;
        else if (r.name == "Oxidizer") r.amount.reset();
    s.display->draw(warn, monotonic());
    CHECK(save_png(s.surface, prefix + "_cautions.png"));
    // On the launch pad, nose straight up (user photos 2026-11-16): the NavBall shows its zenith pole.
    Snapshot pad = demo(0);
    pad.nav->pitch = 90, pad.nav->heading = 270, pad.nav->roll = -90, pad.nav->rotation = attitude(90, 270, -90);
    s.display->navball().hold = pad.nav->rotation;
    s.display->draw(pad, monotonic());
    CHECK(save_png(s.surface, prefix + "_zenith.png"));
    s.display->navball().hold.reset();
    // A game-like maneuver node: kilometres per second, hours away, a long burn.
    Snapshot node = demo(1200);
    node.nav->node_dv = 1834.6, node.nav->node_remaining = 1834.6, node.nav->node_time = 3 * 3600 + 25 * 60 + 7;
    node.nav->burn_seconds = 312;
    s.display->draw(node, monotonic());
    CHECK(save_png(s.surface, prefix + "_node.png"));
    // Around the Sun (user, 2026-11-16): a 17 Gm orbit drew Kerbol as a 2-3 px dot; the disc keeps a visible size.
    Snapshot sun = demo(1200);
    if (sun.orbit_state) {
        OrbitState o = *sun.orbit_state;
        const double k = 1.686e10 / std::sqrt(dot(o.position, o.position)), mu = 1.1723328e18;
        const double v = std::sqrt(mu / o.mu / k);
        o.position = o.position * k, o.velocity = o.velocity * v, o.mu = mu;
        sun.orbit_state = o;
    }
    sun.body = "Sun", sun.body_radius = 261600000, sun.atmosphere_depth = 0, sun.maneuver_orbit.reset();
    s.display->draw(sun, monotonic());
    CHECK(save_png(s.surface, prefix + "_sun.png"));
    // Duna with Ike targeted (live game 2026-11-16): the closest approach markers, then the planned encounter.
    s.display->draw(duna_ike(false), monotonic());
    CHECK(save_png(s.surface, prefix + "_target.png"));
    s.display->draw(duna_ike(true), monotonic());
    CHECK(save_png(s.surface, prefix + "_encounter.png"));
    }
}

// ------------------------------------------------------------------------------------------- VAB ship report
namespace {
std::shared_ptr<const ShipReport> sample_ship(const std::string& variant = "") {
    // kerbalx / duna: the live stock Kerbal X with KSP's app on Kerbin sea level / Duna vacuum
    const std::string file = variant == "kerbalx"  ? "ship_report_kerbal_x.txt"
                             : variant == "duna"   ? "ship_report_kerbal_x_duna.txt"
                                                   : "ship_report_v1.txt";
    std::ifstream in(std::string(KSPHMI_SOURCE_DIR) + "/tests/data/" + file);
    std::stringstream buf;
    buf << in.rdbuf();
    auto r = parse_ship_report(buf.str());
    if (!r) return nullptr;
    if (variant == "pending") r->delta_v_pending = true, r->stages.clear();
    if (variant == "empty") r->delta_v_pending = true, r->stages.clear(), r->parts = 0, r->crew = 0, r->seats = 0;
    if (variant == "many") {  // more stages than rows, a long name, a big ship: everything must still fit
        r->name = "An extremely long ship name that is never going to fit on any header at all, really";
        for (int i = 3; i < 14; ++i) r->stages.push_back({i, 12345, 23456, 12.34, 9999, {{"engines", 12}, {"decouplers", 4}, {"chutes", 8}}, std::numeric_limits<double>::quiet_NaN(), {}});
        r->mass = 12345.67, r->cost = 98765432, r->funds = 1234;
    }
    if (variant == "heavy") r->stages.front().twr = .74;  // cannot leave the pad
    return std::make_shared<const ShipReport>(std::move(*r));
}

Snapshot vab(std::shared_ptr<const ShipReport> ship) {
    Snapshot s;
    s.state = State::NoVessel;
    s.scene = "vab";
    s.ship = std::move(ship);
    return s;
}
}  // namespace

TEST(vab_report_renders_inside_its_panels_in_both_skins) {
    for (const char* skin : {"classic", "apollo", "steampunk"})
        for (const auto& size : SIZES) {
            const int w = size.first, h = size.second;
            Screen s(w, h, skin, repo_config());
            std::vector<std::pair<std::string, Rect>> panels;
            s.display->on_vab_panel = [&](const std::string& name, const Rect& r) { panels.emplace_back(name, r); };
            std::vector<std::pair<std::string, Rect>> texts;
            s.canvas->on_text = [&](const std::string& label, const Rect& r) { texts.emplace_back(label, r); };
            for (const char* variant : {"", "pending", "many", "heavy", "kerbalx", "duna", "empty"}) {
                panels.clear(), texts.clear();
                s.display->draw(vab(sample_ship(variant)), monotonic());
                CHECK(!panels.empty());
                for (const auto& [label, r] : texts)
                    if (!Rect(0, 0, w, h).contains(r))
                        check::fail(__FILE__, __LINE__, std::string(skin) + " " + variant + ": text off screen: " + label);
                for (size_t i = 0; i < panels.size(); ++i) {
                    if (!Rect(0, 0, w, h).contains(panels[i].second))
                        check::fail(__FILE__, __LINE__, std::string(skin) + ": panel off screen: " + panels[i].first);
                    for (size_t j = i + 1; j < panels.size(); ++j)
                        if (panels[i].second.collides(panels[j].second))
                            check::fail(__FILE__, __LINE__, std::string(skin) + ": panels overlap: " + panels[i].first +
                                                                " / " + panels[j].first);
                }
                // text that starts in a panel stays in it (the table's events column must shrink to fit)
                for (const auto& [label, r] : texts)
                    for (const auto& [name, p] : panels)
                        if (p.contains(Rect(r.x, r.y, 1, 1)) && !p.inflate(1, 1).contains(r))
                            check::fail(__FILE__, __LINE__, std::string(skin) + " " + variant + " " + std::to_string(w) +
                                                                ": text '" + label + "' leaves " + name);
            }
        }
}

TEST(vab_without_a_report_keeps_the_static_cards) {
    for (const char* skin : {"classic", "apollo", "steampunk"}) {
        Screen s(1260, 840, skin, repo_config());
        std::vector<std::string> texts;
        s.canvas->on_text = [&](const std::string& label, const Rect&) { texts.push_back(label); };
        int panels = 0;
        s.display->on_vab_panel = [&](const std::string&, const Rect&) { ++panels; };
        s.display->draw(vab(nullptr), monotonic());
        CHECK_EQ(panels, 0);
        bool card = false;
        for (const auto& t : texts) card = card || t.find("Propulsion") != std::string::npos || t == "P";
        CHECK(card || std::string(skin) != "classic");  // apollo paints its cards into the cached art (no on_text)
        // a report for another facility does not take over its screen
        Snapshot sph = vab(sample_ship());
        sph.scene = "sph";
        s.display->draw(sph, monotonic());
        CHECK_EQ(panels, 0);
    }
}

TEST(apollo_vab_header_shows_only_link_and_twr_lamps) {
    Screen s(1920, 1080, "apollo", repo_config());
    s.display->draw(vab(sample_ship()), monotonic());  // creates the skin
    std::map<std::string, const Color*> seen;
    std::map<std::string, Color> colors;
    s.display->apollo()->on_lamp = [&](const std::string& legend, const Color* lit) {
        seen[legend] = lit;
        if (lit) colors[legend] = *lit;
    };
    s.display->draw(vab(sample_ship("heavy")), monotonic());
    std::set<std::string> legends;
    for (const auto& kv : seen) legends.insert(kv.first);
    CHECK(legends == (std::set<std::string>{"UPLINK", "NO DATA", "NO LINK", "LAUNCH TWR", "UPPER TWR"}));
    CHECK(seen["UPLINK"] != nullptr);
    CHECK(seen["LAUNCH TWR"] != nullptr && colors["LAUNCH TWR"] == Color({246, 66, 48}));  // 0.74 < 1.0: red
    CHECK(seen["UPPER TWR"] != nullptr && colors["UPPER TWR"] == Color({255, 178, 42}));   // 0.62 < 0.8: amber
    seen.clear();
    s.display->draw(vab(sample_ship("pending")), monotonic());
    CHECK(seen.count("LAUNCH TWR") && seen["LAUNCH TWR"] == nullptr && seen["UPPER TWR"] == nullptr);
    // flight keeps its annunciators
    seen.clear();
    s.display->draw(demo(), monotonic());
    CHECK(seen.count("SAS") && seen.count("PE ATMO") && seen.count("ESCAPE") && !seen.count("LAUNCH TWR"));
}

TEST(vab_report_software_render_time) {
    for (const char* skin : {"classic", "apollo", "steampunk"}) {
        Screen s(2520, 1680, skin, repo_config());
        Snapshot snap = vab(sample_ship());
        s.display->draw(snap, monotonic());  // apollo: builds the panel art
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 5; ++i) s.display->draw(snap, monotonic());
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 5;
        std::printf("     %s VAB report frame at 2520x1680: %.1f ms\n", skin, ms);
        CHECK(ms < 1000);
    }
}

// KSPHMI_SHOTS=DIR saves the VAB report screens (sample data, not the game) in both skins.
TEST(vab_review_screenshots_on_request) {
    const char* dir = std::getenv("KSPHMI_SHOTS");
    if (!dir) return;
    for (const char* skin : {"classic", "apollo", "steampunk", "ksp1", "ksp2"}) {
        Screen s(2520, 1680, skin, repo_config());
        for (const char* variant : {"", "pending", "heavy", "kerbalx", "duna", "empty"}) {
            s.display->draw(vab(sample_ship(variant)), monotonic());
            std::string name = std::string(dir) + "/" + skin + "_vab" + (*variant ? std::string("_") + variant : "") + ".png";
            CHECK(save_png(s.surface, name));
        }
    }
}

TEST(editor_modes_keep_real_skin_geometry_and_cache_stable) {
    const char* shots = std::getenv("KSPHMI_SHOTS");
    for (const char* skin : {"classic", "apollo", "steampunk", "ksp1", "ksp2"})
        for (const auto& size : SIZES) {
            Screen screen(size.first, size.second, skin, repo_config());
            std::ifstream file(std::string(KSPHMI_SOURCE_DIR) + "/tests/data/editor_report_v1.txt");
            std::stringstream text;
            text << file.rdbuf();
            auto parsed = parse_ship_report(text.str());
            CHECK(parsed.has_value());
            std::vector<std::pair<std::string, Rect>> labels;
            screen.canvas->on_text = [&](const std::string& s, const Rect& box) { labels.emplace_back(s, box); };
            for (const char* mode : {"construction", "actions", "crew", "cargo"}) {
                auto data = *parsed;
                data.editor_mode = mode;
                Snapshot snap = vab(std::make_shared<const ShipReport>(data));
                snap.received = 0;
                labels.clear();
                screen.display->draw(snap, 0);
                for (const auto& entry : labels)
                    if (!Rect(0, 0, size.first, size.second).contains(entry.second))
                        check::fail(__FILE__, __LINE__, std::string(skin) + " " + mode + ": off-screen " + entry.first);
                if (shots && size.first == 2520) CHECK(save_png(screen.surface, std::string(shots) + "/" + skin + "_editor_" + mode + ".png"));
                // Live group labels and manifests must not invalidate Apollo's static art.
                data.selected_group = "custom2";
                data.manifest[0].name = "A different kerbal";
                snap.ship = std::make_shared<const ShipReport>(data);
                auto apollo = screen.display->apollo();
                int before = apollo ? apollo->builds : 0;
                screen.display->draw(snap, 0);
                CHECK(!apollo || apollo->builds == before);
            }
        }
}

namespace {
const char* const SPLASH_STAGES[] = {"system", "network", "offline", "hmi", "ready", "failed"};
SplashAnim splash_anim(const BootView& v, double pulse = 1.0) { return {v.progress, v.done ? 0.9 : 0.0, pulse}; }
}  // namespace

TEST(boot_splash_fills_the_screen_and_fits_at_every_size) {
    for (const auto& size : SIZES)
        for (const char* stage : SPLASH_STAGES) {
            const int w = size.first, h = size.second;
            Screen s(w, h);
            s.fill(FILL);
            std::vector<std::pair<std::string, Rect>> texts;
            s.canvas->on_text = [&](const std::string& t, const Rect& r) { texts.emplace_back(t, r); };
            const BootView view = boot_view(boot_demo_inputs(stage, w, h));
            draw_splash(*s.canvas, view, splash_anim(view));
            const uint32_t bg = (theme::BG.r << 16) | (theme::BG.g << 8) | theme::BG.b;
            // Every corner is the HMI background: no console black or stale pixels survive the splash.
            CHECK_EQ(s.pixel(0, 0), bg);
            CHECK_EQ(s.pixel(w - 1, h - 1), bg);
            std::map<std::string, int> seen;
            for (auto& [t, r] : texts) ++seen[t];
            for (const char* t : {"KSPKontroller", "SINCE POWER-ON", "BOOT SEQUENCE", "KERNEL", "DISPLAY", "SYSTEM",
                                  "NETWORK", "HMI", "Arduino panel works without this display"})
                CHECK(seen.count(t));
            CHECK(seen.count(view.clock));
            CHECK(seen.count(view.message));
            CHECK(seen.count(view.footer));
            for (const auto& row : view.rows) CHECK(seen.count(row.detail));
            CHECK(seen.count(view.failed ? std::string("HMI FAILED")
                                         : std::to_string(static_cast<int>(std::lround(view.progress * 100))) + "%"));
            // Everything on the screen, nothing on top of anything else.
            Rect screen(0, 0, w, h);
            for (size_t i = 0; i < texts.size(); ++i) {
                if (!screen.contains(texts[i].second)) std::fprintf(stderr, "outside: %s\n", texts[i].first.c_str());
                CHECK(screen.contains(texts[i].second));
                for (size_t j = i + 1; j < texts.size(); ++j) {
                    bool hit = texts[i].second.collides(texts[j].second);
                    if (hit)
                        std::fprintf(stderr, "%dx%d %s: '%s' overlaps '%s'\n", w, h, stage, texts[i].first.c_str(),
                                     texts[j].first.c_str());
                    CHECK(!hit);
                }
            }
        }
}

TEST(boot_splash_partial_frames_match_a_full_redraw) {
    // SplashPainter restores only the animated regions from its static copy: the result must be the same picture.
    for (const auto& size : {std::pair<int, int>{1260, 840}, {1920, 1080}, {2520, 1680}}) {
        const int w = size.first, h = size.second;
        SDL_Surface* target = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
        {
            SplashPainter painter(target);
            const char* order[] = {"system", "failed", "network", "ready", "offline", "hmi"};
            for (size_t i = 0; i + 1 < std::size(order); ++i) {
                const BootView a = boot_view(boot_demo_inputs(order[i], w, h));
                const BootView b = boot_view(boot_demo_inputs(order[i + 1], w, h));
                painter.draw(a, splash_anim(a, 0.2));
                painter.draw(b, splash_anim(b, 0.7));
                Screen ref(w, h);
                draw_splash(*ref.canvas, b, splash_anim(b, 0.7));
                int differ = 0;
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                        differ += (static_cast<const uint32_t*>(target->pixels)[y * (target->pitch / 4) + x] &
                                   0xFFFFFF) != ref.pixel(x, y);
                if (differ) std::fprintf(stderr, "%dx%d %s -> %s: %d pixels differ\n", w, h, order[i], order[i + 1],
                                         differ);
                CHECK_EQ(differ, 0);
            }
        }
        SDL_FreeSurface(target);
    }
}

TEST(boot_splash_screenshots) {
    const char* dir = std::getenv("KSPHMI_SHOTS");
    if (!dir) return;
    for (const char* stage : SPLASH_STAGES) {
        Screen s(2520, 1680);
        const BootView view = boot_view(boot_demo_inputs(stage, 2520, 1680));
        draw_splash(*s.canvas, view, splash_anim(view));
        CHECK(save_png(s.surface, std::string(dir) + "/splash_" + stage + ".png"));
    }
}

// ------------------------------------------------------------------------------------- layer cache
namespace {
std::vector<uint32_t> pixels(const Screen& s) {
    std::vector<uint32_t> out;
    for (int y = 0; y < s.surface->h; ++y)
        for (int x = 0; x < s.surface->w; ++x) out.push_back(s.pixel(x, y));
    return out;
}
int differing(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    int n = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) n += a[i] != b[i];
    return n + static_cast<int>(a.size() > b.size() ? a.size() - b.size() : b.size() - a.size());
}
}  // namespace

TEST(waiting_and_no_vessel_use_the_same_planet_material_as_their_flight_theme) {
    for (const char* skin : {"apollo", "steampunk"}) {
        Screen screen(1260, 840, skin);
        for (const auto state : {State::Connecting, State::NoVessel}) {
            Snapshot snap; snap.state = state;
            screen.display->draw(snap, 100.2);
            const double u = screen.canvas->u, cx = screen.surface->w / 2.0, cy = 34*u;
            std::set<uint32_t> colors;
            int blue = 0, nongreen = 0;
            for (int y=int(cy-3*u); y<int(cy+3*u); ++y)
                for (int x=int(cx-3*u); x<int(cx+3*u); ++x) {
                    const auto pixel = screen.pixel(x,y);
                    const int r=(pixel>>16)&255, g=(pixel>>8)&255, b=pixel&255;
                    colors.insert(pixel);
                    blue += b > r+20 && g > r+10;
                    nongreen += std::max({r,g,b}) >= 32 && (g < r*1.5 || g < b*1.3);
                }
            CHECK(colors.size() > 100); // textured globe rather than the legacy flat blue disc
            if (std::string(skin) == "apollo") CHECK_EQ(nongreen, 0);
            else CHECK(blue > 100);
            CHECK(!screen.display->orbit_crt && !screen.display->orbit_body);
        }
    }
}

TEST(steampunk_alert_legends_replace_caution_and_facility_cards_use_dark_materials) {
    Screen screen(1260, 840, "steampunk");
    const double u = screen.canvas->u;
    std::string legend;
    screen.canvas->on_text = [&](const std::string& text, const Rect& box) {
        if (box.x >= screen.surface->w*.9 && box.bottom() < 7*u) legend += text;
    };
    Snapshot connecting;
    screen.display->draw(connecting, 100.2);
    CHECK_EQ(legend, "NOLINK"); // CAUTION must not remain beneath the changing warning
    for (const auto& view : views()) {
        Snapshot facility; facility.state=State::NoVessel; facility.scene=view.key; facility.received=100.2;
        legend.clear();
        screen.display->draw(facility, 100.2);
        CHECK_EQ(legend, "CAUTION");
        const auto panel=screen.pixel(int(25*u),int(39.5*u));
        CHECK(((panel>>16)&255)+((panel>>8)&255)+(panel&255) < 100);
    }
}

TEST(steampunk_destination_jewels_preserve_reachability_colours) {
    Screen screen(1260, 840, "steampunk", repo_config());
    auto snap=vab(sample_ship("heavy"));
    screen.display->draw(snap, 100.2);
    const double u=screen.canvas->u;
    int green=0, red=0;
    for (int y=int(13*u); y<int(42*u); ++y)
        for (int x=int(screen.surface->w*.52); x<screen.surface->w; ++x) {
            const auto pixel=screen.pixel(x,y);
            const int r=(pixel>>16)&255,g=(pixel>>8)&255,b=pixel&255;
            green += g>r*1.5 && g>b*1.5 && g>80;
            red += r>g*1.5 && r>b*1.5 && r>80;
        }
    CHECK(green>20); CHECK(red>20);
}

TEST(game_themes_cache_art_and_keep_native_resource_names_and_absence_semantics) {
    for(const char* skin:{"ksp1","ksp2"}) {
        Screen screen(1260,840,skin);
        auto snap=demo(600);snap.received=0;
        std::vector<std::string> text,gauges;
        screen.canvas->on_text=[&](const std::string& value,const Rect&){text.push_back(value);};
        screen.display->on_gauge=[&](const std::string& name,const Rect&){gauges.push_back(name);};
        screen.display->draw(snap,0);
        auto* renderer=screen.display->game_skin();CHECK(renderer);const int before=renderer->builds;
        snap.altitude+=10;snap.vessel="A different name";screen.display->draw(snap,0);
        CHECK_EQ(renderer->builds,before);
        snap.systems->native_resources=true;
        snap.systems->resources={{"Methalox",10.0,20.0},{"Hydrogen",0.0,100.0},{"ElectricCharge",{},100.0}};
        text.clear();gauges.clear();screen.display->draw(snap,0);
        CHECK_EQ(renderer->builds,before+1);
        CHECK(std::find(gauges.begin(),gauges.end(),"Methalox")!=gauges.end());
        CHECK(std::find(gauges.begin(),gauges.end(),"Hydrogen")!=gauges.end());
        CHECK(std::find(text.begin(),text.end(),"UNKNOWN")!=text.end());
        CHECK(std::find(gauges.begin(),gauges.end(),"LiquidFuel")==gauges.end());
        snap.systems->stages.clear();text.clear();screen.display->draw(snap,0);
        CHECK(std::find(text.begin(),text.end(),"STAGE DATA UNAVAILABLE")!=text.end());
        CHECK(std::find(text.begin(),text.end(),"LOW FUEL")!=text.end());
        snap.ut=std::numeric_limits<double>::quiet_NaN();text.clear();screen.display->draw(snap,0);
        CHECK(std::find(text.begin(),text.end(),"HISTORY DATA UNAVAILABLE")!=text.end());
        text.clear();snap.state=State::Connecting;screen.display->draw(snap,0);
        CHECK(std::find(text.begin(),text.end(),"WAITING FOR GAME")!=text.end());
        CHECK(std::find(text.begin(),text.end(),"SAS")==text.end());
        CHECK(std::find(text.begin(),text.end(),"THROTTLE")==text.end());
        CHECK(!screen.display->orbit_crt&&!screen.display->orbit_body&&screen.display->panel_titles);
    }
}

TEST(game_themes_restore_typefaces_and_keep_every_scene_inside_the_screen) {
    for(const char* skin:{"ksp1","ksp2"})for(const auto& size:SIZES) {
        Screen screen(size.first,size.second,skin,repo_config());
        const auto original=screen.canvas->font_path();
        screen.canvas->on_text=[&](const std::string& label,const Rect& r){
            if(!Rect(0,0,size.first,size.second).contains(r))check::fail(__FILE__,__LINE__,std::string(skin)+" off-screen "+label);
        };
        auto snap=demo(600);screen.display->draw(snap,0);
        CHECK_EQ(screen.canvas->font_path(),original);
        snap.vessel=std::string(180,'W')+" "+"\xc3\xa1";screen.display->draw(snap,0);
        Snapshot waiting;screen.display->draw(waiting,0);
        waiting.state=State::NoVessel;screen.display->draw(waiting,0);
        for(const auto& scene:views()){waiting.scene=scene.key;screen.display->draw(waiting,0);}
        snap.vessel="Demo Orbiter";
        screen.display->choose_skin("classic");screen.display->draw(snap,0);
        CHECK_EQ(screen.canvas->font_path(),original);
    }
}

TEST(steampunk_dials_calibrate_signed_values_and_never_invent_missing_measurements) {
    SteamDial d{50,0,100,1,"u"};
    CHECK_EQ(*steam_dial_fraction(d),.5);
    d.minimum=-100; d.value=-50;
    CHECK_EQ(*steam_dial_fraction(d),.25);
    d.value=1e300; CHECK_EQ(*steam_dial_fraction(d),1.0); CHECK_EQ(d.value,1e300);
    d.value=-1e300; CHECK_EQ(*steam_dial_fraction(d),0.0);
    d.value=std::numeric_limits<double>::quiet_NaN(); CHECK(!steam_dial_fraction(d));
    d.value=std::numeric_limits<double>::infinity(); CHECK(!steam_dial_fraction(d));
    d.value=0;d.minimum=d.maximum;CHECK(!steam_dial_fraction(d));
    auto snap=demo();snap.eccentricity=1.4;
    const auto escape=steam_dials(snap);
    CHECK(!steam_dial_fraction(escape[2]));  // no fictional apoapsis on an escape trajectory
    CHECK(!steam_dial_fraction(escape[8]));  // no fictional orbital period either
    for(const auto& size:SIZES) {
        Screen screen(size.first,size.second,"steampunk");
        const double u=screen.canvas->u,width=(size.first*.44-2.5*u-4*.8*u)/5;
        const Rect tile=Rect::f(2.5*u,8*u,width,17.5*u);
        const auto geometry=steam_dial_geometry(*screen.canvas,tile);
        CHECK(tile.contains(Rect::f(geometry.cx-geometry.r,geometry.cy-geometry.r,2*geometry.r,2*geometry.r)));
        CHECK(tile.contains(geometry.reading));
    }
}

TEST(steampunk_needles_move_with_telemetry_inside_their_calibrated_faces) {
    Screen screen(1920,1080,"steampunk");
    auto snap=demo();snap.altitude=0;
    screen.display->draw(snap,0);
    const auto baseline=pixels(screen);
    snap.altitude=500000;
    screen.display->draw(snap,0);
    const double u=screen.canvas->u,width=(1920*.44-2.5*u-4*.8*u)/5;
    const auto g=steam_dial_geometry(*screen.canvas,Rect::f(2.5*u,8*u,width,17.5*u));
    int changed=0;
    for(int y=int(g.cy-g.r*.7);y<int(g.cy+g.r*.7);++y)
        for(int x=int(g.cx-g.r*.7);x<int(g.cx+g.r*.7);++x)
            changed+=baseline[y*1920+x]!=screen.pixel(x,y);
    CHECK(changed>30);  // the face/scale are artwork, but the needle must actually move
}

TEST(steampunk_typefaces_do_not_leak_into_other_skins) {
    for (const char* original : {"classic", "apollo"}) {
        Screen s(1260, 840, original);
        Snapshot snap = demo(600);
        s.display->navball().hold = attitude(0, 0, 0);
        s.display->draw(snap, 0);
        const auto expected = pixels(s);
        snap.skin = "steampunk";
        s.display->draw(snap, 0);
        CHECK(differing(pixels(s), expected) > 1000);
        snap.skin = original;
        s.display->draw(snap, 0);
        CHECK_EQ(differing(pixels(s), expected), 0);
    }
}

TEST(instrument_history_waits_for_samples_and_resets_for_a_new_vessel_or_time_rewind) {
    for (const char* skin : {"steampunk", "apollo"}) {
    Screen s(1260, 840, skin);
    Snapshot snap = demo(600);
    bool collecting = false;
    s.canvas->on_text = [&](const std::string& text, const Rect&) {
        if (text == "COLLECTING FLIGHT DATA") collecting = true;
    };
    auto draw = [&] { collecting = false; s.display->draw(snap, 0); return collecting; };
    CHECK(draw());
    CHECK(draw());  // repainting the same UT must not invent a second sample
    snap.ut += 1;
    CHECK(!draw());
    snap.ut -= 5;
    CHECK(draw());
    snap.ut += 1;
    CHECK(!draw());
    snap.vessel = "Another vessel";
    CHECK(draw());
    }
}

TEST(body_material_catalog_is_distinct_cached_by_body_and_monochrome_only_for_crt) {
    Screen screen(512,512);
    OrbitArt art;
    CHECK_EQ(OrbitArt::stock_bodies().size(),17u);
    CHECK_EQ(OrbitArt::material_key("Kerbol"),"sun");
    CHECK_EQ(OrbitArt::material_key("The Mun"),"mun");
    CHECK_EQ(OrbitArt::material_key("Custom World"),"custom_world");
    CHECK(OrbitArt::material_key("../../kerbin").empty());
    auto draw=[&](const std::string& body,bool crt) {
        screen.canvas->clear({0,0,0});
        art.body(*screen.canvas,body,256,256,210,crt);
        screen.canvas->flush(); SDL_RenderFlush(screen.renderer);
        return pixels(screen);
    };
    for(bool crt:{false,true}) {
        std::set<uint64_t> identities;
        for(const auto& name:OrbitArt::stock_bodies()) {
            const auto image=draw(name,crt);
            uint64_t fingerprint=1469598103934665603ull;
            std::set<uint32_t> detail;
            for(int y=120;y<390;y+=3)for(int x=120;x<390;x+=3) {
                const auto pixel=image[y*512+x];
                fingerprint=(fingerprint^pixel)*1099511628211ull;
                detail.insert(pixel);
                if(crt) {
                    const int r=(pixel>>16)&255,g=(pixel>>8)&255,b=pixel&255;
                    CHECK(g>=r);CHECK(g>=b);
                }
            }
            CHECK(detail.size()>50); CHECK(identities.insert(fingerprint).second);
        }
        const auto first=draw("Kerbin",crt);
        CHECK(first!=draw("Duna",crt));
        CHECK(first==draw("Kerbin",crt)); // return after an SOI change cannot retain the previous texture
        CHECK(draw("Sun",crt)==draw("Kerbol",crt));
        CHECK(draw("Unknown Mod World",crt)!=first);
    }
}

TEST(all_themes_change_orbit_material_when_telemetry_changes_body) {
    const char* shots=std::getenv("KSPHMI_BODY_SHOTS");
    for(const char* skin:{"classic","apollo","steampunk"}) {
        Screen screen(1920,1080,skin);
        auto snap=demo(600); snap.received=0;
        screen.display->navball().hold=attitude(0,0,0);
        const auto layout=flight_layout(*screen.canvas,(std::string(skin)=="apollo"||std::string(skin)=="ksp1"||std::string(skin)=="ksp2"));
        auto draw=[&](const char* body) {
            snap.body=body;
            // Keep orbital geometry fixed to isolate the material, but do not invent a lunar atmosphere.
            snap.atmosphere_depth=std::string(body)=="Mun" ? 0 : std::string(body)=="Duna" ? 50000 :
                                  std::string(body)=="Jool" ? 200000 : 70000;
            screen.display->draw(snap,0);
            screen.canvas->flush(); SDL_RenderFlush(screen.renderer);
            std::vector<uint32_t> region;
            for(int y=layout.orbit.y;y<layout.orbit.bottom();++y)
                for(int x=layout.orbit.x;x<layout.orbit.right();++x)region.push_back(screen.pixel(x,y));
            if(shots) CHECK(save_png(screen.surface,std::string(shots)+"/"+skin+"_"+body+".png"));
            return region;
        };
        const auto kerbin=draw("Kerbin");
        CHECK(kerbin!=draw("Duna")); CHECK(kerbin!=draw("Jool")); CHECK(kerbin!=draw("Mun"));
        CHECK(kerbin==draw("Kerbin"));
    }
}

TEST(steampunk_orbit_uses_the_detailed_color_planet_and_restores_shared_render_hooks) {
    Screen s(1920, 1080, "steampunk");
    Snapshot snap = demo(600);
    snap.atmosphere_depth = 0;
    const Rect plot = flight_layout(*s.canvas).orbit;
    auto sea_colors = [&] {
        std::set<uint32_t> colors;
        for (int y=plot.y;y<plot.bottom();++y) for(int x=plot.x;x<plot.right();++x) {
            const uint32_t pixel=s.pixel(x,y);
            const int r=(pixel>>16)&255, g=(pixel>>8)&255, b=pixel&255;
            if(b>r+20 && g>r+10) colors.insert(pixel);
        }
        return colors.size();
    };
    s.display->draw(snap, 0);
    const size_t detailed = sea_colors();
    CHECK(detailed > 100);  // terrain/cloud/ocean pixels, not a flat coloured circle
    CHECK(!s.display->orbit_body && !s.display->orbit_label && s.display->panel_titles);
    snap.body="Mun";
    s.display->draw(snap, 0);
    CHECK(sea_colors() * 5 < detailed);  // marker/font antialiasing may remain, but the ocean detail must disappear
    CHECK(!s.display->orbit_crt);  // a previous Apollo frame cannot turn the steampunk view monochrome
}

TEST(apollo_orbit_is_entirely_green_crt_including_maneuvers_and_live_markers) {
    for (const auto& size : {std::pair<int,int>{1260,840}, {1920,1080}}) {
        Screen s(size.first,size.second,"apollo");
        Snapshot snap=demo(600);
        const double u=s.canvas->u;
        const Rect orbit=flight_layout(*s.canvas,true).orbit;
        const Rect screen=Rect::f(orbit.x+2.5*u,orbit.y+5.5*u,orbit.w-5*u,orbit.h-8*u);
        for (const char* body : {"Kerbin","Mun"}) {
            snap.body=body;
            s.display->draw(snap,0);
            std::set<uint32_t> phosphor;
            int colored=0;
            for(int y=screen.y;y<screen.bottom();++y) for(int x=screen.x;x<screen.right();++x) {
                const uint32_t pixel=s.pixel(x,y);
                const int r=(pixel>>16)&255,g=(pixel>>8)&255,b=pixel&255;
                if(std::max({r,g,b})<32)continue;
                if(g<r*1.5 || g<b*1.3)++colored;
                else phosphor.insert(pixel);
            }
            CHECK_EQ(colored,0);
            CHECK(phosphor.size()>20);
            CHECK(!s.display->orbit_crt && !s.display->orbit_body && !s.display->orbit_label);
        }
        CHECK_EQ(s.display->apollo()->builds,1);
    }
}

TEST(navball_only_frames_over_the_layer_cache_match_a_full_redraw) {
    const Quat before = attitude(5, 30, 0), after = attitude(25, 75, 20);
    for (const char* skin : {"classic", "apollo", "steampunk", "ksp1", "ksp2"})
        for (const auto& size : {std::pair<int, int>{1260, 840}, {2520, 1680}}) {
            const int w = size.first, h = size.second;
            Snapshot snap = demo(600);
            CHECK(snap.nav.has_value());
            // Reference: the plain full redraw at the new attitude, without the cache.
            Screen ref(w, h, skin);
            CHECK(!ref.display->animate());  // cache off: the caller does a full draw
            ref.display->navball().hold = after;
            ref.display->draw(snap, monotonic());
            const auto expected = pixels(ref);
            // A full frame through the cache is the same picture.
            Screen cached(w, h, skin);
            CHECK(cached.display->set_layer_cache(true));
            cached.display->navball().hold = after;
            cached.display->draw(snap, monotonic());
            CHECK_EQ(differing(pixels(cached), expected), 0);
            // Composed: the layer drawn at the old attitude, then only the NavBall at the new one.
            Screen composed(w, h, skin);
            CHECK(composed.display->set_layer_cache(true));
            composed.display->navball().hold = before;
            composed.display->draw(snap, monotonic());
            const auto old_frame = pixels(composed);
            CHECK(differing(old_frame, expected) > 1000);  // the attitude change is visible
            composed.display->navball().hold = after;
            CHECK(composed.display->animate());
            CHECK_EQ(differing(pixels(composed), expected), 0);
        }
}

TEST(layer_cache_follows_skin_switches_and_screens) {
    const Quat after = attitude(-15, 200, -30);
    Screen s(1260, 840, "classic");
    CHECK(s.display->set_layer_cache(true));
    Snapshot snap = demo(900);
    s.display->navball().hold = attitude(0, 0, 0);
    s.display->draw(snap, monotonic());
    snap.skin = "apollo";  // switch while flying: the next full draw rebuilds the layer in the new skin
    s.display->draw(snap, monotonic());
    s.display->navball().hold = after;
    CHECK(s.display->animate());
    const auto composed = pixels(s);
    Screen ref(1260, 840, "apollo");
    ref.display->navball().hold = after;
    ref.display->draw(snap, monotonic());
    CHECK_EQ(differing(composed, pixels(ref)), 0);
    // No globe in the last draw (not in flight, or no attitude): no NavBall-only frames.
    Snapshot waiting = snap;
    waiting.state = State::Connecting;
    s.display->draw(waiting, monotonic());
    CHECK(!s.display->animate());
    Snapshot blind = snap;
    blind.nav.reset();
    s.display->draw(blind, monotonic());
    CHECK(!s.display->animate());
    s.display->draw(snap, monotonic());
    CHECK(s.display->animate());
    CHECK(!s.display->set_layer_cache(false));
    CHECK(!s.display->animate());
}

TEST(navball_only_frame_software_time) {
    Screen s(2520, 1680);
    CHECK(s.display->set_layer_cache(true));
    Snapshot snap = demo(300);
    s.display->navball().hold = attitude(0, 0, 0);
    s.display->draw(snap, monotonic());  // warm-up
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 1; i <= 5; ++i) s.display->navball().hold = attitude(0, i * 3.0, 0), s.display->draw(snap, monotonic());
    auto t1 = std::chrono::steady_clock::now();
    for (int i = 1; i <= 5; ++i) {
        s.display->navball().hold = attitude(0, i * 3.0, 0);
        CHECK(s.display->animate());
    }
    auto t2 = std::chrono::steady_clock::now();
    double full = std::chrono::duration<double, std::milli>(t1 - t0).count() / 5;
    double anim = std::chrono::duration<double, std::milli>(t2 - t1).count() / 5;
    std::printf("     software 2520x1680 with the layer cache: full %.1f ms, navball-only %.1f ms\n", full, anim);
    CHECK(anim < full);
}

TEST(navball_attitude_smoothing) {
    Quat a = attitude(0, 0, 0), b = attitude(0, 90, 0);
    // Smoothing moves part of the way each frame, converges, and snaps for large jumps.
    Quat s = a;
    double prev = angle_between(s, attitude(0, 20, 0));
    for (int i = 0; i < 30; ++i) {
        s = Navball::smooth(s, attitude(0, 20, 0), 1.0 / 30);
        double d = angle_between(s, attitude(0, 20, 0));
        CHECK(d < prev + 1e-9);
        prev = d;
    }
    CHECK(prev < 0.1);
    CHECK_NEAR(angle_between(Navball::smooth(a, b, 1.0 / 30), b), 0, 1e-6);  // 90 > SNAP_DEG
    CHECK_NEAR(angle_between(Navball::smooth(a, attitude(0, 20, 0), 0), attitude(0, 20, 0)), 0, 1e-6);
}

int main(int argc, char** argv) {
    log::set_quiet(true);
    if (SDL_Init(0) != 0) return 2;
    int rc = RUN_TESTS(argc, argv);
    TTF_Quit();
    SDL_Quit();
    return rc;
}
