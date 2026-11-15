// Boot splash: `ksphmi --splash` animates the loading screen on the framebuffer until the HMI is ready.
//
// It runs from the system unit ksphmi-splash.service as soon as the DRM framebuffer /dev/fb0 exists, long before
// the user service (ksphmi.service) starts: no settings file, no kRPC. About 8 times a second it redraws the clock
// since power-on, the ascent trajectory (progress) and the BOOT SEQUENCE card from probes taken at 2 Hz on a
// worker (bootstate.hpp). It exits as soon as the HMI writes its ready file (first frame presented), on SIGTERM,
// or after `max_seconds`; the framebuffer keeps the last picture.
//
// Cost: the static parts (background, planet, card, labels, footer) are drawn once per size into a copy; each
// frame restores only the animated regions from it and redraws them, and Framebuffer::write skips unchanged rows.
#pragma once

#include <SDL.h>

#include <memory>
#include <string>

#include "bootstate.hpp"
#include "canvas.hpp"

namespace ksphmi {

/// What moves on the screen, eased by the caller from frame to frame.
struct SplashAnim {
    double shown = 0;  ///< progress drawn, 0..1 (eases toward BootView::progress)
    double orbit = 0;  ///< radians the ship has travelled along the target orbit after reaching it
    double pulse = 1;  ///< 0..1, brightness of the active row's "···" pill
};

/// The complete loading screen on `c`, flushed.
void draw_splash(Canvas& c, const BootView& view, const SplashAnim& anim);

/// Draws frames into an ARGB8888 surface, keeping the static layer between frames.
class SplashPainter {
public:
    explicit SplashPainter(SDL_Surface* target);
    ~SplashPainter();
    SplashPainter(const SplashPainter&) = delete;
    SplashPainter& operator=(const SplashPainter&) = delete;

    /// First frame (or new host name / display): everything; later frames: only the animated regions.
    void draw(const BootView& view, const SplashAnim& anim);
    Canvas& canvas() { return *canvas_; }

private:
    SDL_Surface* target_;
    SDL_Surface* static_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    std::unique_ptr<Canvas> canvas_;
    std::string static_key_;
};

/// Animate the loading screen on `device` until the HMI is ready (0), or log why it could not start (1).
/// Without the TTF font it still fills the screen with the HMI background (never the console's black and text).
int run_splash(const std::string& device, double max_seconds = 600);

}  // namespace ksphmi
