#include "keyboard.hpp"

namespace ksphmi {

namespace {

// kRPC SpaceCenter.SASMode values in the order of the HMI's SAS MODE buttons (Alt+1 ... Alt+0):
// HOLD, PRO, RET, N+, N-, R+, R-, TGT, ANTI, NODE.
constexpr int SAS_MODE_BY_DIGIT[10] = {/*0 NODE*/ 1, /*1 HOLD*/ 0, /*2 PRO*/ 2, /*3 RET*/ 3, /*4 N+*/ 4,
                                       /*5 N-*/ 5,   /*6 R+*/ 6,   /*7 R-*/ 7,  /*8 TGT*/ 8, /*9 ANTI*/ 9};

const char* sas_mode_name(int mode) {
    static const char* const names[] = {"stability assist", "maneuver", "prograde", "retrograde", "normal",
                                        "anti-normal",      "radial",   "anti-radial", "target", "anti-target"};
    return mode >= 0 && mode < 10 ? names[mode] : "?";
}

}  // namespace

std::string command_name(const Command& c) {
    switch (c.kind) {
        case Command::ToggleSas: return "SAS toggle";
        case Command::ToggleRcs: return "RCS toggle";
        case Command::Stage: return "stage";
        case Command::ToggleGear: return "gear toggle";
        case Command::ToggleLights: return "lights toggle";
        case Command::SetBrakes: return c.value ? "brakes on" : "brakes off";
        case Command::Abort: return "abort";
        case Command::ActionGroup: return "action group " + std::to_string(c.value ? c.value : 10);
        case Command::SasMode: return std::string("SAS mode ") + sas_mode_name(c.value);
        case Command::ThrottleFull: return "throttle full";
        case Command::ThrottleCut: return "throttle cut";
        case Command::WarpUp: return "time warp up";
        case Command::WarpDown: return "time warp down";
        case Command::WarpStop: return "time warp stop";
        case Command::Quicksave: return "quicksave";
        case Command::Quickload: return "quickload";
    }
    return "?";
}

KeyResult Keyboard::down(const KeyEvent& e, double now) {
    KeyResult r;
    if (e.key == Key::Unknown) return r;
    if (e.key != Key::Digit && !down_.insert(e.key).second) return r;  // already held: auto-repeat
    switch (e.key) {
        case Key::Escape:
        case Key::Quit: r.quit = true; break;
        case Key::Digit:
            if (e.digit < 0 || e.digit > 9) break;
            if (e.ctrl) {  // Ctrl+1 / Ctrl+2: skins; this Ctrl press must not ramp the throttle
                ctrl_used_ = true;
                if (e.digit == 1) r.skin = "classic";
                else if (e.digit == 2) r.skin = "apollo";
            } else if (e.alt) {
                r.commands.push_back({Command::SasMode, SAS_MODE_BY_DIGIT[e.digit]});
            } else {
                r.commands.push_back({Command::ActionGroup, e.digit});  // kRPC: 0 = group 10, as KSP's key 0
            }
            break;
        case Key::T: r.commands.push_back({Command::ToggleSas}); break;
        case Key::F: r.commands.push_back({Command::ToggleSas}); break;  // SAS hold: inverted while held
        case Key::R: r.commands.push_back({Command::ToggleRcs}); break;
        case Key::Z: r.commands.push_back({Command::ThrottleFull}); break;
        case Key::X: r.commands.push_back({Command::ThrottleCut}); break;
        case Key::G: r.commands.push_back({Command::ToggleGear}); break;
        case Key::U: r.commands.push_back({Command::ToggleLights}); break;
        case Key::B: r.commands.push_back({Command::SetBrakes, 1}); break;
        case Key::Space: r.commands.push_back({Command::Stage}); break;
        case Key::Backspace: r.commands.push_back({Command::Abort}); break;
        case Key::Comma: r.commands.push_back({Command::WarpDown}); break;
        case Key::Period: r.commands.push_back({Command::WarpUp}); break;
        case Key::Slash: r.commands.push_back({Command::WarpStop}); break;
        case Key::F5: r.commands.push_back({Command::Quicksave}); break;
        case Key::F9: f9_since_ = now, f9_fired_ = false; break;
        case Key::LeftCtrl: ctrl_since_ = now, ctrl_used_ = false; break;
        default: break;  // axes and Shift: read through held()
    }
    return r;
}

KeyResult Keyboard::up(const KeyEvent& e, double) {
    KeyResult r;
    if (e.key == Key::Digit || !down_.erase(e.key)) return r;
    if (e.key == Key::F) r.commands.push_back({Command::ToggleSas});  // SAS hold released: back as it was
    if (e.key == Key::B) r.commands.push_back({Command::SetBrakes, 0});
    if (e.key == Key::F9) f9_since_ = -1;
    if (e.key == Key::LeftCtrl) ctrl_since_ = -1, ctrl_used_ = false;
    return r;
}

KeyResult Keyboard::tick(double now) {
    KeyResult r;
    if (f9_since_ >= 0 && !f9_fired_ && now - f9_since_ >= QUICKLOAD_HOLD) {
        f9_fired_ = true;
        r.commands.push_back({Command::Quickload});
    }
    return r;
}

Held Keyboard::held(double now) const {
    auto on = [&](Key k) { return down_.count(k) != 0; };
    auto axis = [&](Key minus, Key plus) { return float((on(plus) ? 1 : 0) - (on(minus) ? 1 : 0)); };
    Held h;
    h.pitch = axis(Key::W, Key::S);
    h.yaw = axis(Key::A, Key::D);
    h.roll = axis(Key::Q, Key::E);
    h.x = axis(Key::L, Key::J);
    h.y = axis(Key::I, Key::K);
    h.z = axis(Key::H, Key::N);
    const bool ctrl_ramp = on(Key::LeftCtrl) && !ctrl_used_ && ctrl_since_ >= 0 && now - ctrl_since_ >= CTRL_RAMP_DELAY;
    h.throttle = (on(Key::LeftShift) ? 1 : 0) - (ctrl_ramp ? 1 : 0);
    return h;
}

KeyResult Keyboard::release_all() {
    KeyResult r;
    if (down_.count(Key::F)) r.commands.push_back({Command::ToggleSas});
    if (down_.count(Key::B)) r.commands.push_back({Command::SetBrakes, 0});
    down_.clear();
    ctrl_since_ = f9_since_ = -1;
    ctrl_used_ = f9_fired_ = false;
    return r;
}

}  // namespace ksphmi
