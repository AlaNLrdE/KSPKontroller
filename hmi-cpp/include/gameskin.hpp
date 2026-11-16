// Approved KSP1 Stock / KSP2 Redux instrument skins; both work with either telemetry backend.
#pragma once
#include <SDL.h>

#include <string>
#include <vector>

#include "canvas.hpp"
#include "model.hpp"
#include "netinfo.hpp"
#include "orbitart.hpp"
namespace ksphmi {
class Display;
std::string game_skin_font(bool redux);
class GameSkin {
   public:
    GameSkin(Display& display, bool redux);
    ~GameSkin();
    GameSkin(const GameSkin&) = delete;
    GameSkin& operator=(const GameSkin&) = delete;
    void draw(const Snapshot& s, double now);
    int builds = 0;

   private:
    void build(const Snapshot& s, const std::string& key);
    void paint(Canvas& c, bool art, const Snapshot& s, double now);
    void header(Canvas& c, bool art, const Snapshot& s, double now);
    void flight(Canvas& c, bool art, const Snapshot& s, double now);
    void waiting(Canvas& c, bool art, const Snapshot& s, double now);
    void facility(Canvas& c, bool art, const Snapshot& s);
    void editor(Canvas& c, bool art, const Snapshot& s);
    void construction(Canvas& c, bool art, const Snapshot& s);
    void orbit(Canvas& c, const Rect& r, const Snapshot& s);
    void history(Canvas& c, bool art, const Rect& r, const Snapshot& s);
    Display& d_;
    bool redux_;
    SDL_Texture* panel_ = nullptr;
    std::string key_;
    OrbitArt orbit_art_;
    NetInfo net_;
    struct Sample {
        double ut, altitude, speed;
    };
    std::vector<Sample> history_;
    std::string history_body_, history_vessel_;
};
}  // namespace ksphmi
