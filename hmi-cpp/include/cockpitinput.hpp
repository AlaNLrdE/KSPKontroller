// The cockpit panel scene's controls and what a click on each one does (no SDL and no drawing here, so the
// behaviour is unit tested without a display; cockpit.hpp lays the controls out and draws them).
//
// A click produces the same Command as the control's KSP key and goes through the same queue to the telemetry
// thread. The lamps never show a state the game has not reported: a pressed control shows "sent" until the
// snapshot confirms the change, and "rejected" for a moment when the game did not change within CONFIRM_TIMEOUT.
// STAGE needs ARM first (ARM_WINDOW); ABORT, CUT and QUICKLOAD open a guard on the first click and act on the
// second one within COVER_WINDOW. The keyboard keys (Space, Backspace, F9) are unchanged: they act at once.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "keyboard.hpp"
#include "model.hpp"

namespace ksphmi {

/// The two flight scenes, cycled with Ctrl+"+" / Ctrl+"-".
enum class FlightScene { Instruments, Cockpit };
constexpr int FLIGHT_SCENES = 2;
const char* flight_scene_name(FlightScene s);
/// "instruments" or "cockpit" ("panel" too); nothing for any other text.
std::optional<FlightScene> parse_flight_scene(const std::string& name);
/// `step` scenes forward (negative: back), cyclically.
FlightScene step_scene(FlightScene s, int step);

/// Every clickable control of the cockpit panel.
enum class Ctl {
    Sas, Rcs, Wheels,
    Mode0, Mode1, Mode2, Mode3, Mode4, Mode5, Mode6, Mode7, Mode8, Mode9,  // HOLD PRO RET N+ N- R+ R- TGT ANTI NODE
    RefOrbit, RefSurface, RefTarget,
    Arm, Stage, Abort,
    Deploy, Cut,
    Gear, Lights, Brakes, Solar, Antennas, Radiators, CargoBays, Intakes,
    Group1, Group2, Group3, Group4, Group5, Group6, Group7, Group8, Group9, Group10,
    ThrottleFull, ThrottleCut, WarpDown, WarpStop, WarpUp, Map, Quicksave, Quickload,
    TabInstruments, TabCockpit,
    Count
};
constexpr int CTL_COUNT = static_cast<int>(Ctl::Count);
inline Ctl ctl_at(Ctl first, int offset) { return static_cast<Ctl>(static_cast<int>(first) + offset); }
inline int ctl_offset(Ctl c, Ctl first) { return static_cast<int>(c) - static_cast<int>(first); }

/// SAS mode buttons in the HMI's SAS MODE order, with the kRPC SASMode value and snapshot name of each.
struct SasModeInfo {
    const char* legend;
    int krpc;
    const char* name;  ///< Navigation::sas_mode
};
extern const SasModeInfo SAS_MODES_HMI[10];
/// The switch a control flips, if it is one of the panel's switches.
std::optional<CockpitSwitch> switch_of(Ctl c);
/// "SAS", "GEAR", "1" ... as engraved on the panel (also the log name of a click).
const char* ctl_legend(Ctl c);
/// The KSP key printed beside the control ("T", "Alt+2", "SPACE"), "" when it has none.
const char* ctl_key(Ctl c);
/// The SAS mode unavailable now because its reference is missing (TGT / ANTI without a target, NODE without a node).
bool sas_mode_unavailable(int hmi_index, const Snapshot& s);

/// What a click asks for: game commands and/or a flight scene.
struct CockpitPress {
    std::vector<Command> commands;
    std::optional<FlightScene> scene;
};

class CockpitInput {
public:
    static constexpr double ARM_WINDOW = 5.0, COVER_WINDOW = 5.0;
    static constexpr double CONFIRM_TIMEOUT = 2.5, SENT_FLASH = 0.6, REJECT_FLASH = 1.2;

    enum class Feedback { None, Sent, Rejected };

    /// Mouse button down on `c`, with the snapshot shown at that moment.
    CockpitPress press(Ctl c, const Snapshot& s, double now);
    /// Mouse button up (anywhere): ends a held control (BRAKES).
    std::vector<Command> release(double now);
    /// Forget everything held, armed or open (focus lost, flight left, scene changed). Releases held brakes.
    std::vector<Command> reset();
    /// Compare pending clicks with the latest snapshot: confirmed ones end, late ones become "rejected".
    void update(const Snapshot& s, double now);

    bool armed(double now) const { return now < armed_until_; }
    /// Seconds left in the ARM window (0 when not armed).
    double armed_left(double now) const { return armed(now) ? armed_until_ - now : 0; }
    bool cover_open(Ctl c, double now) const;
    Feedback feedback(Ctl c, double now) const;
    std::optional<Ctl> held() const { return held_; }
    /// True while something changes with time alone (arm window, open guard, sent / rejected flash): the display
    /// must redraw every frame, not only on new telemetry.
    bool busy(double now) const;

private:
    struct Pending {
        double since = 0;
        int expect = 0;  ///< the value that confirms the click (see expectation())
        bool flash_only = false;  ///< no game state confirms it: only the short "sent" flash
    };
    void expect(Ctl c, const Snapshot& s, double now);
    double armed_until_ = -1;
    std::map<Ctl, double> covers_;     ///< open guard until
    std::map<Ctl, Pending> pending_;   ///< waiting for the game
    std::map<Ctl, double> rejected_;   ///< amber until
    std::optional<Ctl> held_;
};

}  // namespace ksphmi
