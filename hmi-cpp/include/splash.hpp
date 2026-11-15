// Boot splash: `ksphmi --splash` draws one loading screen straight to the framebuffer and exits.
//
// It runs from the system unit ksphmi-splash.service as soon as /dev/fb0 exists, long before the user service
// (ksphmi.service) starts: no settings file, no network, no kRPC. The framebuffer keeps the picture until the HMI
// writes its first frame over it, so the screen shows the splash for the rest of the boot.
#pragma once

#include <string>

#include "canvas.hpp"

namespace ksphmi {

/// Draw the loading screen on `c` (title, "Starting..." and a planet with an orbiting moon), flushed.
void draw_splash(Canvas& c);

/// Draw the loading screen once on `device` and return 0, or log why it could not and return 1.
/// Without the TTF font it still fills the screen with the HMI background (never the console's black and text).
int run_splash(const std::string& device);

}  // namespace ksphmi
