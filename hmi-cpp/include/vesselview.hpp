// The VESSEL panel's data: a side view of the active vessel (one convex outline per part) and each part's fuel
// and heat, from KSPSerialIO.Hmi.dll's KSPHMIFlight service (VesselShape / VesselState, text format v1, see
// docs/hmi.rst "VESSEL panel"). No SDL here: parsing and the demo vessel are unit tested in ksphmi_core.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "vec.hpp"

namespace ksphmi {

/// One part: its convex side-view outline in metres in the root part's frame (first = right, second = up along
/// the stack), its depth (mean z, larger = farther from the viewer) and its live state.
struct VesselPart {
    std::vector<Point2> hull;
    double depth = 0;
    int fuel = -1;  ///< % of the part's propellant capacity (0-100), -1 = no propellant tanks
    int heat = 0;   ///< % of the part's maximum temperature, the hotter of internal and skin (0-100)
};

struct VesselView {
    uint32_t revision = 0;          ///< shape revision; a state is only applied to the shape it was built for
    std::vector<VesselPart> parts;  ///< in shape order (the state lists parts in this order)
};

/// "v=1 rev=N parts=K" then one "p=depth,x:y,x:y,..." line per part (at least 3 points). nullopt when the text is
/// not format v1, a number does not parse, a hull has fewer than 3 points or the part count disagrees.
std::optional<VesselView> parse_vessel_shape(std::string_view text);

/// "v=1 rev=N" then "s=fuel:heat,fuel:heat,..." in shape order. Applies it to `view` and returns true only when
/// the revision matches and there is one entry per part; values are clamped (fuel -1..100, heat 0..100).
bool apply_vessel_state(VesselView& view, std::string_view text);

/// Shape and state text of `view` (tests and the simulator fixture use the same format the plugin writes).
std::string format_vessel_shape(const VesselView& view);
std::string format_vessel_state(const VesselView& view);

/// The demo's three-stage rocket with two boosters; fuel drains and the engines warm up with `t` (seconds).
VesselView demo_vessel_view(double t);

/// Part indices in draw order: farthest first (largest depth), ties in shape order.
std::vector<size_t> vessel_draw_order(const VesselView& view);

/// The part of a convex polygon at or below the horizontal line y = level (screen coordinates, y down): still
/// convex, empty when the polygon is entirely above it.
std::vector<Point2> clip_below(const std::vector<Point2>& polygon, double level);

}  // namespace ksphmi
