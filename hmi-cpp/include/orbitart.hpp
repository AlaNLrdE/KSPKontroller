// Cached planet materials and green phosphor glass, shared by the Steampunk and Apollo ORBIT panels.
#pragma once
#include <SDL.h>

#include <string>
#include <vector>

#include "canvas.hpp"
namespace ksphmi {
class OrbitArt {
   public:
    ~OrbitArt();
    OrbitArt() = default;
    OrbitArt(const OrbitArt&) = delete;
    OrbitArt& operator=(const OrbitArt&) = delete;
    static const std::vector<std::string>& stock_bodies();
    static std::string material_key(const std::string& name);
    static Color atmosphere_color(const std::string& name);
    void body(Canvas& c, const std::string& name, double x, double y, double radius, bool crt);
    /// The scanline/vignette mask is baked once per viewport size, never per telemetry sample.
    void glass(Canvas& c, const Rect& viewport);

   private:
    SDL_Texture* planet(Canvas& c, const std::string& name, bool crt);
    std::string color_name_, phosphor_name_;
    SDL_Texture *color_ = nullptr, *phosphor_ = nullptr, *glass_ = nullptr;
    int glass_w_ = 0, glass_h_ = 0;
};
}  // namespace ksphmi
