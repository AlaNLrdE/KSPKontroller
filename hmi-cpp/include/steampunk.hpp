// Shared artwork/typeface helpers: steampunk brass/iron and Apollo facility/editor mounting plates.
#pragma once
#include <string>

#include "canvas.hpp"

namespace ksphmi {
std::string steam_font(bool headings);
/// Changes only this Canvas's active face; cached glyph atlases survive switching skins.
class TypefaceScope {
   public:
    TypefaceScope(Canvas& c, const std::string& path) : c_(c), previous_(c.use_font(path)) {}
    TypefaceScope(const TypefaceScope&) = delete;
    TypefaceScope& operator=(const TypefaceScope&) = delete;
    ~TypefaceScope() { c_.use_font(previous_); }

   private:
    Canvas& c_;
    std::string previous_;
};

/// Texture lifetime is the static-art renderer's lifetime, never a telemetry frame.
class SteampunkArt {
   public:
    explicit SteampunkArt(Canvas& c, bool aerospace = false);
    ~SteampunkArt();
    SteampunkArt(const SteampunkArt&) = delete;
    SteampunkArt& operator=(const SteampunkArt&) = delete;
    void panel(const Rect& r, double corner);
    void dial(double cx, double cy, double radius);
    void sight_tube(const Rect& r);
    void bezel(double cx, double cy, double outer, double inner);

   private:
    SDL_Texture *panel_ = nullptr, *bezel_ = nullptr, *dial_ = nullptr, *tube_ = nullptr;
    bool aerospace_ = false;
    Canvas& c_;
};
}  // namespace ksphmi
