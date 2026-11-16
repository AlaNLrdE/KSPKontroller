// Calibrated mechanical flight instruments: static scales, live needles and exact numeric readouts.
#pragma once
#include <array>
#include <optional>

#include "canvas.hpp"
#include "model.hpp"
namespace ksphmi {
struct SteamDial {
    double value, minimum, maximum, divisor;
    const char* unit;
};
std::array<SteamDial, 10> steam_dials(const Snapshot& s);
/// No value means unavailable. Out-of-range values peg the needle; the exact readout is never clamped.
std::optional<double> steam_dial_fraction(const SteamDial& dial);
struct SteamDialGeometry {
    double cx, cy, r;
    Rect reading;
};
SteamDialGeometry steam_dial_geometry(const Canvas& c, const Rect& tile);
void steam_dial_scale(Canvas& c, const SteamDialGeometry& g, const SteamDial& dial);
void steam_dial_needle(Canvas& c, const SteamDialGeometry& g, const SteamDial& dial);
}  // namespace ksphmi
