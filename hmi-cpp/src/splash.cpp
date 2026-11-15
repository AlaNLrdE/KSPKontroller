#include "splash.hpp"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>

#include "fbdev.hpp"
#include "log.hpp"
#include "theme.hpp"

namespace ksphmi {

namespace {
const char* LOG = "ksphmi.splash";
}

void draw_splash(Canvas& c) {
    using namespace theme;
    const double u = c.u;
    const float cx = static_cast<float>(c.w / 2.0), cy = static_cast<float>(c.h * 0.5);
    c.clear(BG);
    // The waiting screen's planet and orbit (display.cpp), so the hand-over to the HMI looks like one screen.
    const float py = static_cast<float>(cy - 14 * u);
    c.circle(cx, py, static_cast<float>(6 * u), BODY);
    c.ring(cx, py, static_cast<float>(11 * u), std::max(1.0f, static_cast<float>(static_cast<int>(0.25 * u))), GRID);
    c.circle(cx + static_cast<float>(11 * u * std::cos(-0.6)), py + static_cast<float>(11 * u * std::sin(-0.6)),
             static_cast<float>(1.2 * u), ACCENT);
    c.text("KSP HMI", 9, VALUE, cx, cy + 2 * u, Anchor::MidTop);
    c.text("Starting...", 4, LABEL, cx, cy + 13 * u, Anchor::MidTop);
    // A static loading bar: the splash exits at once, the HMI's own waiting screen animates.
    const float bw = static_cast<float>(36 * u), bh = static_cast<float>(1.2 * u);
    const float bx = cx - bw / 2, by = static_cast<float>(cy + 20 * u);
    c.rounded_rect(Rect::f(bx, by, bw, bh), bh / 2, PANEL);
    c.rounded_rect(Rect::f(bx, by, bw * 0.3, bh), bh / 2, ACCENT);
    c.flush();
}

int run_splash(const std::string& device) {
    std::unique_ptr<Framebuffer> fb;
    try {
        fb = std::make_unique<Framebuffer>(device);
    } catch (const std::exception& e) {
        log::error(LOG, e.what());
        return 1;
    }
    fb->activate();  // nothing may have set the display mode yet when the console stays silent
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, fb->width(), fb->height(), 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) {
        log::error(LOG, std::string("SDL surface: ") + SDL_GetError());
        return 1;
    }
    int code = 0;
    SDL_Renderer* renderer = SDL_CreateSoftwareRenderer(surface);
    try {
        if (!renderer) throw std::runtime_error(std::string("software renderer: ") + SDL_GetError());
        Canvas canvas(renderer, fb->width(), fb->height(), false);
        draw_splash(canvas);
    } catch (const std::exception& e) {
        // No font (or renderer): the plain background still hides whatever the console showed.
        log::warning(LOG, std::string("splash without text: ") + e.what());
        SDL_FillRect(surface, nullptr, SDL_MapRGB(surface->format, theme::BG.r, theme::BG.g, theme::BG.b));
    }
    try {
        fb->write(surface);
        log::info(LOG, "splash on " + device);
    } catch (const std::exception& e) {
        log::error(LOG, e.what());
        code = 1;
    }
    if (renderer) SDL_DestroyRenderer(renderer);
    SDL_FreeSurface(surface);
    return code;
}

}  // namespace ksphmi
