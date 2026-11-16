// Shared instrument renderer: "steampunk" uses a scoped brass palette and engraved figures.
// The "apollo" skin delegates flight to FlightDeck (proposal A materials / proposal B NavBall), and shares
// the facility/editor composition below with steampunk.
//
// Grey panel with white painted labels and group lines, recessed wells, screws, seven-segment readouts
// (segments.hpp) and caution/warning lamp tiles. All static art (panel texture, frames, labels, lamp housings,
// segment windows with their unlit segments, meter scales, instrument bezels) is drawn once per screen size and
// screen kind into one texture; per frame only the lit segments, lit lamps, pointers, a few changing labels, the
// ORBIT plot and the NavBall are drawn. Materials never contain fixed telemetry values.
#pragma once

#include <SDL.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "canvas.hpp"
#include "config.hpp"
#include "model.hpp"
#include "netinfo.hpp"

namespace ksphmi {

class Display;
class FlightDeck;
class OrbitArt;

class ApolloSkin {
public:
    ApolloSkin(Display& display, const Config& cfg, bool steampunk = false);
    ~ApolloSkin();
    ApolloSkin(const ApolloSkin&) = delete;
    ApolloSkin& operator=(const ApolloSkin&) = delete;

    void draw(const Snapshot& s, double now);
    /// How often the static panel art was rebuilt (tests: once per screen kind, not per frame).
    int builds = 0;
    /// Called for every header lamp evaluated live: its legend, and the lit colour or nullptr when unlit
    /// (the flight LOW PWR / LOW FUEL lamps are only reported when their resource is low).
    std::function<void(const std::string&, const Color*)> on_lamp;

private:
    enum Screen { FLIGHT, FACILITY, WAITING, NO_VESSEL, VAB };
    struct Key {
        int w = 0, h = 0, screen = -1;
        std::string scene;
        std::vector<std::string> resource_names;
        std::vector<int> resources;  // index in the resource names * 4 + state
        bool operator==(const Key& o) const {
            return w == o.w && h == o.h && screen == o.screen && scene == o.scene && resources == o.resources &&
                   resource_names == o.resource_names;
        }
    };
    Key key_for(const Snapshot& s) const;
    void build(const Key& key, const Snapshot& s);

    // Each section draws its static art (`art`, into the cached panel) or its live parts, from the same geometry.
    void header(Canvas& c, bool art, const Snapshot& s, double now, int screen);
    void flight(Canvas& c, bool art, const Snapshot& s);
    /// VESSEL bezel with FUEL / HEAT lamps (art pass) and the schematic (live pass, Display::vessel_contents).
    void vessel(Canvas& c, bool art, const Rect& rect, const Snapshot& s);
    void history(Canvas& c, bool art, const Rect& rect, const Snapshot& s);
    void resources(Canvas& c, bool art, const Rect& rect, const Snapshot& s);
    void staging(Canvas& c, bool art, const Rect& rect, const Snapshot& s);
    /// NavBall zone: white group frame, the globe in a round metal bezel, segment readouts and SAS mode lamps.
    void navball(Canvas& c, bool art, const Rect& rect, const Snapshot& s);
    void facility(Canvas& c, bool art, const Snapshot& s);
    /// VAB editor ship report (Snapshot::ship): DELTA-V group, stage table, destination lamps, VEHICLE readouts.
    void editor(Canvas& c, bool art, const Snapshot& s);
    void vab(Canvas& c, bool art, const Snapshot& s);
    void waiting(Canvas& c, bool art, const Snapshot& s, double now, bool no_vessel);

    Display& d_;
    bool steampunk_;
    double render_now_ = 0;
    std::unique_ptr<FlightDeck> flight_deck_;
    std::unique_ptr<OrbitArt> orbit_art_;
    struct HistorySample { double ut, altitude, speed; };
    std::vector<HistorySample> history_;
    std::string history_vessel_;
    int day_hours_;
    std::string server_text_;
    NetInfo net_;
    SDL_Texture* panel_ = nullptr;
    Key key_;
};

}  // namespace ksphmi
