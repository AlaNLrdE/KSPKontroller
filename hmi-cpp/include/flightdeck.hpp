// Apollo Flight Deck: painted aerospace panels (proposal A), Mission Control NavBall (proposal B).
#pragma once
#include <SDL.h>
#include <functional>
#include <string>
#include <vector>
#include "canvas.hpp"
#include "model.hpp"
#include "orbitart.hpp"
namespace ksphmi {
class Display;
std::string flight_deck_font();
class FlightDeck {
   public:
    explicit FlightDeck(Display& display) : d_(display) {}
    ~FlightDeck();
    FlightDeck(const FlightDeck&) = delete;
    FlightDeck& operator=(const FlightDeck&) = delete;
    void draw(Canvas& c, bool art, const Snapshot& s, double now);
    std::function<void(const std::string&, const Color*)> on_lamp;

   private:
    void orbit(Canvas& c, const Rect& rect, const Snapshot& s);
    void history(Canvas& c, bool art, const Rect& rect, const Snapshot& s);
    struct Sample {
        double ut, altitude, speed;
    };
    std::vector<Sample> history_;
    std::string vessel_;
    Display& d_;
    OrbitArt orbit_art_;
};
}  // namespace ksphmi
