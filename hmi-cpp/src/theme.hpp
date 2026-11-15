// Colours and the panel shape shared by the screens (display.py and navball.py constants).
#pragma once

#include "canvas.hpp"

namespace ksphmi::theme {

constexpr Color BG{10, 14, 20}, PANEL{19, 26, 36}, GRID{34, 45, 60}, MUTED{88, 104, 128}, LABEL{122, 140, 164},
    VALUE{232, 238, 245}, ACCENT{70, 200, 255}, GOOD{96, 222, 140}, WARN{255, 186, 72}, BAD{255, 92, 92},
    BODY{46, 96, 150}, ATMOSPHERE{24, 48, 78},
    MANEUVER{224, 160, 48};  // planned (post-burn) orbit, as KSP's orange dashed patch

// navball.py
constexpr Color SKY{37, 133, 173}, GROUND{157, 91, 35}, INK{215, 226, 221}, GOLD{255, 194, 39}, WHITE{235, 242, 245},
    GREEN{180, 245, 38}, MAGENTA{240, 83, 213}, CYAN{65, 225, 234};

inline void panel(Canvas& c, const Rect& r) { c.rounded_rect(r, static_cast<float>(static_cast<int>(1.2 * c.u)), PANEL); }

}  // namespace ksphmi::theme
