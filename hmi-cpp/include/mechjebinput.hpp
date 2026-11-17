// The MechJeb scene's controls and what a click on each one does (no SDL and no drawing here, so the behaviour is
// unit tested without a display; mechjebpage.hpp lays the pages out and draws them).
//
// Settings go to the game as KSPHMIFlight.MechJebSet(key, value) and actions as MechJebAction(action), through
// the same command queue and thread as the keys. Like the cockpit panel, nothing shows a value the game has not
// reported: a changed setting shows "sent" until the next report carries it, and "rejected" when the plugin
// refuses it (unknown key, read only, invalid value) or no report confirms it within CONFIRM_TIMEOUT. ENGAGE (and
// the two timed launches, which engage the autopilot too) needs ARM first, in the cockpit panel's 5 s window.
// Values are typed on an on-screen keypad (Keypad) or stepped with - / + (right click: ten steps).
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "cockpitinput.hpp"
#include "keyboard.hpp"
#include "model.hpp"

namespace ksphmi {

/// The page the MechJeb scene shows; kept while the HMI runs.
enum class MjPage { Menu, Ascent };

/// One report key as the HMI shows and edits it. Numbers are shown in display units: display = SI * scale.
struct MjField {
    const char* key;
    const char* label;  ///< engraved caption (also the keypad title)
    bool flag;          ///< a 0/1 lamp toggle; the rest is unused
    const char* unit = "";
    double scale = 1;   ///< display value per SI unit (km: 0.001, %: 100)
    double step = 1;    ///< display units per - / + click (right click: x10)
    int decimals = 0;
    double min = 0, max = 0;  ///< display units, inclusive
    bool integer = false;
};
/// The field of a report key, or null.
const MjField* mj_field(const std::string& key);
/// Every field the Ascent Guidance page shows (tests).
const std::vector<MjField>& mj_fields();
/// The SI value shown in display units with the field's decimals ("80.0").
std::string mj_format(const MjField& f, double si);
/// The segment pattern wide enough for every value in range ("8888.8", "-888.8").
std::string mj_pattern(const MjField& f);
/// A display value, checked against the field's range and integer rule, in SI units; nothing when invalid.
std::optional<double> mj_si(const MjField& f, double display);
/// The SI text sent with MechJebSet ("80000", "0.4").
std::string mj_wire(double si);
/// One - / + click from the SI value `si`: `direction` -1 or +1 steps (x10 with `coarse`), clamped to the range and
/// rounded to the field's decimals.
double mj_step(const MjField& f, double si, int direction, bool coarse);

/// The module tiles of the menu, in the approved order: Ascent Guidance (live) first, Smart A.S.S. last.
struct MjModule {
    const char* title;
    const char* summary;
    bool live;  ///< phase 1: only Ascent Guidance opens a page
};
extern const MjModule MJ_MODULES[9];
constexpr int MJ_MODULE_COUNT = 9;

/// The ascent paths of ascent.path, in MechJeb's AscentType order.
extern const char* const MJ_PATHS[3];

/// The LAUNCH group's actions (KSPHMIFlight.MechJebAction names).
enum class MjAction { Engage, Disengage, LaunchRendezvous, LaunchTargetPlane, AbortTimedLaunch };
constexpr int MJ_ACTION_COUNT = 5;
const char* mj_action_name(MjAction a);    ///< "ascent.engage" ...
const char* mj_action_legend(MjAction a);  ///< "ENGAGE AUTOPILOT" ...
/// Engage and the timed launches start the autopilot: they need ARM first.
bool mj_action_needs_arm(MjAction a);

/// The on-screen keypad's keys: digits 0-9, then these.
enum MjPadKey { PAD_POINT = 10, PAD_MINUS, PAD_DELETE, PAD_CANCEL, PAD_SET, PAD_KEYS };
const char* mj_pad_legend(int key);

/// Something clickable in the MechJeb scene.
struct MjControl {
    enum Kind { Tab, Module, Back, Minus, Plus, Value, Flag, Path, Arm, Action, Pad };
    Kind kind = Back;
    int index = 0;    ///< Tab: FlightScene; Module: MJ_MODULES index; Path: 0-2; Action: MjAction; Pad: key
    std::string key{};  ///< Minus / Plus / Value / Flag: the report key
    /// What its feedback is kept under: the key for Minus / Plus / Value / Flag (they change the same setting),
    /// "path:N", "action:N", "arm", "module:N".
    std::string id() const;
    bool operator==(const MjControl& o) const { return kind == o.kind && index == o.index && key == o.key; }
};

/// The numeric keypad that edits one field. The text starts as the shown value; the first digit replaces it.
class Keypad {
public:
    void open(const MjField& f, const std::optional<double>& si);
    void close() { open_ = false, error_.clear(); }
    bool is_open() const { return open_; }
    const std::string& key() const { return key_; }
    const std::string& text() const { return text_; }
    /// Why SET was refused ("" when it was not).
    const std::string& error() const { return error_; }
    /// True until the first key edits the starting value.
    bool fresh() const { return fresh_; }
    /// A digit, '.' or '-' (the sign toggles).
    void type(char c);
    void backspace();
    /// SET: the SI value to send, or nothing (error() says why) when the text is not valid for the field.
    std::optional<double> accept();

    static constexpr size_t MAX_CHARS = 10;

private:
    std::string key_, text_, error_;
    bool open_ = false, fresh_ = false;
};

/// What a click asks for: commands and/or a flight scene.
struct MechJebPress {
    std::vector<Command> commands;
    std::optional<FlightScene> scene;
};

class MechJebInput {
public:
    static constexpr double ARM_WINDOW = 5.0, CONFIRM_TIMEOUT = 3.0, SENT_FLASH = .6, REJECT_FLASH = 1.2;
    static constexpr double NOTICE_TIME = 5.0;
    using Feedback = CockpitInput::Feedback;

    MjPage page() const { return page_; }
    void set_page(MjPage p) { page_ = p; }

    /// Mouse button down on `c` with the snapshot shown at that moment; `coarse` (a right click): ten steps per
    /// - / +. Not Shift: the left Shift key ramps the throttle in flight.
    MechJebPress press(const MjControl& c, const Snapshot& s, double now, bool coarse = false);
    /// A key while the keypad is open: digits, '.', '-', Backspace, Enter (SET) and Escape (CANCEL) type into it.
    /// False (not consumed) for any other key or when the keypad is closed.
    bool key(const KeyEvent& e, const Snapshot& s, double now, MechJebPress& out);
    /// Compare pending changes with the newest snapshot (report and the plugin's last reply).
    void update(const Snapshot& s, double now);
    /// Scene or flight left: disarm, close the keypad, forget pending changes. The page is kept.
    void reset();

    bool armed(double now) const { return now < armed_until_; }
    double armed_left(double now) const { return armed(now) ? armed_until_ - now : 0; }
    Feedback feedback(const std::string& id, double now) const;
    const Keypad& keypad() const { return keypad_; }
    /// The last refusal ("ALTITUDE: INVALID VALUE") while it is recent, else "".
    std::string notice(double now) const { return now < notice_until_ ? notice_ : ""; }
    /// Something changes with time alone: the display must redraw every frame.
    bool busy(double now) const;

private:
    struct Pending {
        double since = 0;
        std::string target;  ///< the key or action the plugin's reply names
        std::string key;     ///< the report key that confirms it ("" = flash only)
        double expect = 0;
    };
    void send_set(const std::string& id, const MjField& f, double si, double now, MechJebPress& out);
    void send_action(MjAction a, double now, MechJebPress& out);
    void reject(const std::string& id, const std::string& why, double now);

    MjPage page_ = MjPage::Menu;
    Keypad keypad_;
    double armed_until_ = -1;
    std::map<std::string, Pending> pending_;
    std::map<std::string, double> rejected_;
    uint32_t reply_seen_ = 0;
    std::string notice_;
    double notice_until_ = -1;
};

}  // namespace ksphmi
