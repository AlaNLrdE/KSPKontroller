// The HMI keyboard as a remote KSP keyboard: the game's default flight keys, translated into commands the
// telemetry thread sends over kRPC (one-shot presses) and a held-key state (attitude/translation axes, throttle
// ramp). No SDL here: main.cpp maps SDL key codes to Key, so the mapping is unit tested without a display.
#pragma once

#include <set>
#include <string>
#include <vector>

namespace ksphmi {

/// The keys the HMI understands. Letters, digits and symbols follow KSP's default flight bindings.
enum class Key {
    Unknown,
    T, R, F, Z, X, G, U, B,                 // SAS, RCS, SAS hold, throttle full / cut, gear, lights, brakes (held)
    M,                                      // map view toggle
    Space, Backspace,                       // stage, abort
    Comma, Period, Slash,                   // time warp down / up / stop
    F5, F9,                                 // quicksave, quickload (held 1 s)
    W, S, A, D, Q, E,                       // pitch down / up, yaw left / right, roll left / right
    H, N, I, K, J, L,                       // translate forward / back, down / up, left / right
    LeftShift, LeftCtrl,                    // throttle up / down (held)
    Digit,                                  // 0-9: action groups; Ctrl+1-5 skins, Ctrl+0 VESSEL view; Alt+digit SAS modes
    Plus, Minus,                            // Ctrl+"+" / Ctrl+"-": next / previous flight scene
    Escape, Quit,                           // Esc and q close the HMI
};

/// One key press or release, with the modifiers held at that moment.
struct KeyEvent {
    Key key = Key::Unknown;
    int digit = -1;     ///< 0-9 for Key::Digit
    bool ctrl = false;  ///< either Ctrl (Ctrl+1 through Ctrl+5 choose the skin)
    bool alt = false;   ///< either Alt / Option (Alt+digit chooses the SAS mode)
};

/// A one-shot game action, sent by the telemetry thread in flight (only ToggleSas / ToggleRcs in the VAB).
struct Command {
    enum Kind {
        ToggleSas, ToggleRcs, Stage, ToggleGear, ToggleLights, SetBrakes, Abort, ActionGroup, SasMode,
        ThrottleFull, ThrottleCut, WarpUp, WarpDown, WarpStop, Quicksave, Quickload, ToggleMap,
        ToggleControl, SpeedMode, DeployChutes, CutChutes,  // cockpit panel controls (cockpitinput.hpp)
    };
    Kind kind;
    /// SetBrakes: 1 on / 0 off; ActionGroup: kRPC group 0-9 (0 = group 10); SasMode: kRPC SASMode; ToggleControl:
    /// an index of CONTROL_PROPERTIES; SpeedMode: kRPC SpeedMode (0 orbit, 1 surface, 2 target)
    int value = 0;
    bool operator==(const Command& o) const { return kind == o.kind && value == o.value; }
};

/// "SAS on/off toggle", "stage", ... for the log.
std::string command_name(const Command& c);

/// The kRPC Control properties a ToggleControl command flips, by Command::value.
inline constexpr const char* CONTROL_PROPERTIES[] = {"ReactionWheels", "SolarPanels", "Antennas",
                                                     "Radiators",      "CargoBays",   "Intakes"};
constexpr int CONTROL_PROPERTY_COUNT = 6;

/// Keys held right now, as KSP's FlightInputHandler would apply them: FlightCtrlState signs (W pitch -1, S +1,
/// A yaw -1, D +1, Q roll -1, E +1; L translate X -1, J +1; I translate Y -1, K +1; H translate Z -1, N +1) and a
/// throttle ramp direction (+1 Shift, -1 Ctrl, 0 none).
struct Held {
    float pitch = 0, yaw = 0, roll = 0, x = 0, y = 0, z = 0;
    int throttle = 0;
    bool axes() const { return pitch != 0 || yaw != 0 || roll != 0 || x != 0 || y != 0 || z != 0; }
    bool operator==(const Held& o) const {
        return pitch == o.pitch && yaw == o.yaw && roll == o.roll && x == o.x && y == o.y && z == o.z &&
               throttle == o.throttle;
    }
    bool operator!=(const Held& o) const { return !(*this == o); }
};

/// What a key event asks for: game commands, a skin ("classic" / "apollo"), the VESSEL view switch (Ctrl+0),
/// a flight scene step (Ctrl+"+" = +1, Ctrl+"-" = -1) and/or quitting.
struct KeyResult {
    std::vector<Command> commands;
    std::string skin;
    bool vessel_mode = false;
    int flight_scene = 0;
    bool quit = false;
};

class Keyboard {
public:
    /// Ctrl starts the throttle-down ramp only after this long, so Ctrl+1 through Ctrl+5 never move the throttle.
    static constexpr double CTRL_RAMP_DELAY = 0.25;
    /// F9 must be held this long before the quickload is sent (KSP also asks for a held F9).
    static constexpr double QUICKLOAD_HOLD = 1.0;

    KeyResult down(const KeyEvent& e, double now);
    KeyResult up(const KeyEvent& e, double now);
    /// Time-driven actions (the held F9 quickload); call once per frame.
    KeyResult tick(double now);
    /// The held-key state at `now`.
    Held held(double now) const;
    /// Forget every held key (the window lost focus: no key-up events will follow). Brakes held on B are released.
    KeyResult release_all();

private:
    std::set<Key> down_;
    double ctrl_since_ = -1;
    bool ctrl_used_ = false;  ///< a Ctrl+digit or Ctrl+"+"/"-" chord: this Ctrl press never ramps the throttle
    double f9_since_ = -1;
    bool f9_fired_ = false;
};

}  // namespace ksphmi
