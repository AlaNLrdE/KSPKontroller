#include "cockpitinput.hpp"

#include <algorithm>

namespace ksphmi {

const SasModeInfo SAS_MODES_HMI[10] = {
    {"HOLD", 0, "stability_assist"}, {"PRO", 2, "prograde"},    {"RET", 3, "retrograde"},
    {"N+", 4, "normal"},             {"N-", 5, "anti_normal"},  {"R+", 6, "radial"},
    {"R-", 7, "anti_radial"},        {"TGT", 8, "target"},      {"ANTI", 9, "anti_target"},
    {"NODE", 1, "maneuver"},
};

namespace {

const char* const SPEED_MODES_HMI[3] = {"orbit", "surface", "target"};

bool between(Ctl c, Ctl first, Ctl last) {
    return static_cast<int>(c) >= static_cast<int>(first) && static_cast<int>(c) <= static_cast<int>(last);
}

/// The game value a control shows: 0/1 for switches and groups, the button index for SAS mode and speed
/// reference, the stage number, the parachute state bits. Nothing when the snapshot does not say.
std::optional<int> observe(Ctl c, const Snapshot& s) {
    const Cockpit* k = s.cockpit ? &*s.cockpit : nullptr;
    auto flag = [](const std::optional<bool>& b) { return b ? std::optional<int>(*b ? 1 : 0) : std::nullopt; };
    if (c == Ctl::Sas) return s.nav ? std::optional<int>(s.nav->sas) : std::nullopt;
    if (c == Ctl::Rcs) return s.nav ? std::optional<int>(s.nav->rcs) : std::nullopt;
    if (auto sw = switch_of(c)) return k ? flag(k->at(*sw).on) : std::nullopt;
    if (between(c, Ctl::Group1, Ctl::Group10)) return k ? flag(k->groups[ctl_offset(c, Ctl::Group1)]) : std::nullopt;
    if (between(c, Ctl::Mode0, Ctl::Mode9)) {
        if (!s.nav) return std::nullopt;
        for (int i = 0; i < 10; ++i)
            if (s.nav->sas_mode == SAS_MODES_HMI[i].name) return i;
        return -1;
    }
    if (between(c, Ctl::RefOrbit, Ctl::RefTarget)) {
        if (!s.nav) return std::nullopt;
        for (int i = 0; i < 3; ++i)
            if (s.nav->speed_mode == SPEED_MODES_HMI[i]) return i;
        return -1;
    }
    if (c == Ctl::Stage) return s.systems ? s.systems->current_stage : std::nullopt;
    if (c == Ctl::Abort) return k ? flag(k->abort) : std::nullopt;
    if (c == Ctl::Deploy || c == Ctl::Cut)
        return k && k->chutes ? std::optional<int>(static_cast<int>(k->chute_states)) : std::nullopt;
    return std::nullopt;
}

}  // namespace

const char* flight_scene_name(FlightScene s) { return s == FlightScene::Cockpit ? "cockpit" : "instruments"; }

std::optional<FlightScene> parse_flight_scene(const std::string& name) {
    if (name == "instruments") return FlightScene::Instruments;
    if (name == "cockpit" || name == "panel") return FlightScene::Cockpit;
    return std::nullopt;
}

FlightScene step_scene(FlightScene s, int step) {
    const int n = ((static_cast<int>(s) + step) % FLIGHT_SCENES + FLIGHT_SCENES) % FLIGHT_SCENES;
    return static_cast<FlightScene>(n);
}

std::optional<CockpitSwitch> switch_of(Ctl c) {
    switch (c) {
        case Ctl::Wheels: return CockpitSwitch::ReactionWheels;
        case Ctl::Gear: return CockpitSwitch::Gear;
        case Ctl::Lights: return CockpitSwitch::Lights;
        case Ctl::Brakes: return CockpitSwitch::Brakes;
        case Ctl::Solar: return CockpitSwitch::SolarPanels;
        case Ctl::Antennas: return CockpitSwitch::Antennas;
        case Ctl::Radiators: return CockpitSwitch::Radiators;
        case Ctl::CargoBays: return CockpitSwitch::CargoBays;
        case Ctl::Intakes: return CockpitSwitch::Intakes;
        default: return std::nullopt;
    }
}

const char* ctl_legend(Ctl c) {
    static const char* const groups[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10"};
    if (between(c, Ctl::Mode0, Ctl::Mode9)) return SAS_MODES_HMI[ctl_offset(c, Ctl::Mode0)].legend;
    if (between(c, Ctl::Group1, Ctl::Group10)) return groups[ctl_offset(c, Ctl::Group1)];
    switch (c) {
        case Ctl::Sas: return "SAS";
        case Ctl::Rcs: return "RCS";
        case Ctl::Wheels: return "REACTION WHEELS";
        case Ctl::RefOrbit: return "ORBIT";
        case Ctl::RefSurface: return "SURFACE";
        case Ctl::RefTarget: return "TARGET";
        case Ctl::Arm: return "ARM";
        case Ctl::Stage: return "STAGE";
        case Ctl::Abort: return "ABORT";
        case Ctl::Deploy: return "DEPLOY CHUTES";
        case Ctl::Cut: return "CUT CHUTES";
        case Ctl::Gear: return "GEAR";
        case Ctl::Lights: return "LIGHTS";
        case Ctl::Brakes: return "BRAKES";
        case Ctl::Solar: return "SOLAR";
        case Ctl::Antennas: return "ANTENNA";
        case Ctl::Radiators: return "RADIATOR";
        case Ctl::CargoBays: return "CARGO BAY";
        case Ctl::Intakes: return "INTAKES";
        case Ctl::ThrottleFull: return "FULL";
        case Ctl::ThrottleCut: return "CUT";
        case Ctl::WarpDown: return "<<";
        case Ctl::WarpStop: return "1x";
        case Ctl::WarpUp: return ">>";
        case Ctl::Map: return "MAP";
        case Ctl::Quicksave: return "QUICK SAVE";
        case Ctl::Quickload: return "QUICK LOAD";
        case Ctl::TabInstruments: return "INSTRUMENTS";
        case Ctl::TabCockpit: return "COCKPIT PANEL";
        default: return "?";
    }
}

const char* ctl_key(Ctl c) {
    static const char* const modes[] = {"Alt+1", "Alt+2", "Alt+3", "Alt+4", "Alt+5",
                                        "Alt+6", "Alt+7", "Alt+8", "Alt+9", "Alt+0"};
    static const char* const groups[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
    if (between(c, Ctl::Mode0, Ctl::Mode9)) return modes[ctl_offset(c, Ctl::Mode0)];
    if (between(c, Ctl::Group1, Ctl::Group10)) return groups[ctl_offset(c, Ctl::Group1)];
    switch (c) {
        case Ctl::Sas: return "T";
        case Ctl::Rcs: return "R";
        case Ctl::Stage: return "SPACE";
        case Ctl::Abort: return "BKSP";
        case Ctl::Gear: return "G";
        case Ctl::Lights: return "U";
        case Ctl::Brakes: return "B";
        case Ctl::ThrottleFull: return "Z";
        case Ctl::ThrottleCut: return "X";
        case Ctl::WarpDown: return ",";
        case Ctl::WarpStop: return "/";
        case Ctl::WarpUp: return ".";
        case Ctl::Map: return "M";
        case Ctl::Quicksave: return "F5";
        case Ctl::Quickload: return "F9";
        case Ctl::TabInstruments: return "Ctrl -";
        case Ctl::TabCockpit: return "Ctrl +";
        default: return "";
    }
}

bool sas_mode_unavailable(int hmi_index, const Snapshot& s) {
    if (!s.nav) return false;
    const std::string& name = SAS_MODES_HMI[hmi_index].name;
    if (name == "target" || name == "anti_target") return !s.nav->target;
    if (name == "maneuver") return !s.nav->maneuver;
    return false;
}

void CockpitInput::expect(Ctl c, const Snapshot& s, double now) {
    Pending p;
    p.since = now;
    const std::optional<int> before = observe(c, s);
    if (between(c, Ctl::Mode0, Ctl::Mode9)) p.expect = ctl_offset(c, Ctl::Mode0);
    else if (between(c, Ctl::RefOrbit, Ctl::RefTarget)) p.expect = ctl_offset(c, Ctl::RefOrbit);
    else if (c == Ctl::Abort) p.expect = 1;
    else if (c == Ctl::Stage || c == Ctl::Deploy || c == Ctl::Cut) p.expect = before ? -1000 - *before : 0;  // changed
    else if (before) p.expect = *before ? 0 : 1;  // a switch or group: the other position
    if (!before) p.flash_only = true;  // the game does not report it: only show that it was sent
    pending_[c] = p;
    rejected_.erase(c);
}

CockpitPress CockpitInput::press(Ctl c, const Snapshot& s, double now) {
    CockpitPress r;
    auto send = [&](Command cmd, bool confirm = true) {
        r.commands.push_back(cmd);
        if (confirm) expect(c, s, now);
        else pending_[c] = Pending{now, 0, true};
    };
    if (c == Ctl::TabInstruments) return r.scene = FlightScene::Instruments, r;
    if (c == Ctl::TabCockpit) return r.scene = FlightScene::Cockpit, r;
    if (between(c, Ctl::Mode0, Ctl::Mode9)) {
        send({Command::SasMode, SAS_MODES_HMI[ctl_offset(c, Ctl::Mode0)].krpc});
        return r;
    }
    if (between(c, Ctl::RefOrbit, Ctl::RefTarget)) {
        send({Command::SpeedMode, ctl_offset(c, Ctl::RefOrbit)});
        return r;
    }
    if (between(c, Ctl::Group1, Ctl::Group10)) {
        send({Command::ActionGroup, (ctl_offset(c, Ctl::Group1) + 1) % 10});  // kRPC: 0 = group 10
        return r;
    }
    auto guarded = [&](Command cmd) {  // first click opens the guard, the second one within the window acts
        if (cover_open(c, now)) {
            covers_.erase(c);
            send(cmd, c != Ctl::Quickload);
        } else {
            covers_[c] = now + COVER_WINDOW;
        }
    };
    switch (c) {
        case Ctl::Sas: send({Command::ToggleSas}); break;
        case Ctl::Rcs: send({Command::ToggleRcs}); break;
        case Ctl::Wheels: send({Command::ToggleControl, 0}); break;
        case Ctl::Solar: send({Command::ToggleControl, 1}); break;
        case Ctl::Antennas: send({Command::ToggleControl, 2}); break;
        case Ctl::Radiators: send({Command::ToggleControl, 3}); break;
        case Ctl::CargoBays: send({Command::ToggleControl, 4}); break;
        case Ctl::Intakes: send({Command::ToggleControl, 5}); break;
        case Ctl::Gear: send({Command::ToggleGear}); break;
        case Ctl::Lights: send({Command::ToggleLights}); break;
        case Ctl::Brakes:  // held, as the B key: on while the button is down
            r.commands.push_back({Command::SetBrakes, 1});
            held_ = c;
            break;
        case Ctl::Arm:
            armed_until_ = armed(now) ? -1 : now + ARM_WINDOW;
            rejected_.erase(Ctl::Arm);
            break;
        case Ctl::Stage:
            if (armed(now)) {
                armed_until_ = -1;
                send({Command::Stage});
            } else {
                rejected_[Ctl::Arm] = now + REJECT_FLASH;  // the ARM lamp flashes: arm first
            }
            break;
        case Ctl::Abort: guarded({Command::Abort}); break;
        case Ctl::Cut: guarded({Command::CutChutes}); break;
        case Ctl::Quickload: guarded({Command::Quickload}); break;
        case Ctl::Deploy: send({Command::DeployChutes}); break;
        case Ctl::ThrottleFull: send({Command::ThrottleFull}, false); break;
        case Ctl::ThrottleCut: send({Command::ThrottleCut}, false); break;
        case Ctl::WarpDown: send({Command::WarpDown}, false); break;
        case Ctl::WarpStop: send({Command::WarpStop}, false); break;
        case Ctl::WarpUp: send({Command::WarpUp}, false); break;
        case Ctl::Map: send({Command::ToggleMap}, false); break;
        case Ctl::Quicksave: send({Command::Quicksave}, false); break;
        default: break;
    }
    return r;
}

std::vector<Command> CockpitInput::release(double) {
    std::vector<Command> out;
    if (held_ == Ctl::Brakes) out.push_back({Command::SetBrakes, 0});
    held_.reset();
    return out;
}

std::vector<Command> CockpitInput::reset() {
    std::vector<Command> out = release(0);
    armed_until_ = -1;
    covers_.clear();
    pending_.clear();
    rejected_.clear();
    return out;
}

void CockpitInput::update(const Snapshot& s, double now) {
    for (auto it = pending_.begin(); it != pending_.end();) {
        const Ctl c = it->first;
        const Pending& p = it->second;
        bool done = false;
        if (p.flash_only) {
            done = now - p.since >= SENT_FLASH;
        } else {
            const std::optional<int> v = observe(c, s);
            if (v) done = p.expect <= -1000 ? *v != -1000 - p.expect : *v == p.expect;
            if (!done && now - p.since > CONFIRM_TIMEOUT) {
                rejected_[c] = now + REJECT_FLASH;
                done = true;
            }
        }
        it = done ? pending_.erase(it) : std::next(it);
    }
    for (auto it = rejected_.begin(); it != rejected_.end();) it = now >= it->second ? rejected_.erase(it) : std::next(it);
    for (auto it = covers_.begin(); it != covers_.end();) it = now >= it->second ? covers_.erase(it) : std::next(it);
    if (armed_until_ >= 0 && now >= armed_until_) armed_until_ = -1;
}

bool CockpitInput::cover_open(Ctl c, double now) const {
    auto it = covers_.find(c);
    return it != covers_.end() && now < it->second;
}

CockpitInput::Feedback CockpitInput::feedback(Ctl c, double now) const {
    auto r = rejected_.find(c);
    if (r != rejected_.end() && now < r->second) return Feedback::Rejected;
    auto p = pending_.find(c);
    if (p != pending_.end() && (!p->second.flash_only || now - p->second.since < SENT_FLASH)) return Feedback::Sent;
    return Feedback::None;
}

bool CockpitInput::busy(double now) const {
    return armed(now) || !covers_.empty() || !pending_.empty() || !rejected_.empty() || held_.has_value();
}

}  // namespace ksphmi
