#include "mechjebinput.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "fmt.hpp"

namespace ksphmi {

namespace {

constexpr const char* DEG = "\xC2\xB0";

MjField number(const char* key, const char* label, const char* unit, double scale, double step, int decimals,
               double min, double max, bool integer = false) {
    return MjField{key, label, false, unit, scale, step, decimals, min, max, integer};
}
MjField flag(const char* key, const char* label) { return MjField{key, label, true}; }

std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

const std::vector<MjField>& mj_fields() {
    static const std::vector<MjField> fields = {
        // TARGET ORBIT
        number("ascent.altitude", "ALTITUDE", "km", .001, 1, 1, 0, 99999),
        number("ascent.inclination", "INCLINATION", DEG, 1, 1, 1, -180, 180),
        number("ascent.path", "ASCENT PATH", "", 1, 1, 0, 0, 2, true),
        // ASCENT PATH: Classic (GravityTurn shares the turn start altitude and speed)
        flag("ascent.classic.auto", "AUTO PATH"),
        number("ascent.classic.turn_start_altitude", "TURN START ALT", "km", .001, .5, 2, 0, 9999),
        number("ascent.classic.turn_start_velocity", "TURN START SPEED", "m/s", 1, 5, 0, 0, 9999),
        number("ascent.classic.turn_end_altitude", "TURN END ALT", "km", .001, 1, 1, 0, 9999),
        number("ascent.classic.turn_end_angle", "FINAL PATH ANGLE", DEG, 1, 1, 1, 0, 90),
        number("ascent.classic.turn_shape_exponent", "TURN SHAPE", "%", 100, 5, 0, 0, 100),
        // ASCENT PATH: PVG
        number("ascent.pvg.desired_apoapsis", "TARGET APOAPSIS", "km", .001, 1, 1, 0, 99999),
        number("ascent.pvg.desired_attach_alt", "ATTACH ALTITUDE", "km", .001, 1, 1, 0, 99999),
        number("ascent.pvg.desired_fpa", "FLIGHT PATH ANGLE", DEG, 1, .1, 1, -90, 90),
        number("ascent.pvg.pitch_start_velocity", "PITCH START SPEED", "m/s", 1, 5, 0, 0, 9999),
        number("ascent.pvg.pitch_rate", "PITCH RATE", "\xC2\xB0/s", 1, .05, 2, 0, 9.99),
        // GUIDANCE
        flag("ascent.force_roll", "FORCE ROLL"),
        number("ascent.vertical_roll", "CLIMB ROLL", DEG, 1, 5, 0, -360, 360),
        number("ascent.turn_roll", "TURN ROLL", DEG, 1, 5, 0, -360, 360),
        flag("ascent.limit_aoa", "LIMIT AoA"),
        number("ascent.max_aoa", "MAX AoA", DEG, 1, 1, 1, 0, 90),
        flag("ascent.limit_qa", "LIMIT Q\xC2\xB7" "AoA"),
        number("ascent.max_qa", "MAX Q\xC2\xB7" "AoA", "Pa\xC2\xB7\xC2\xB0", 1, 100, 0, 0, 999999),
        flag("ascent.corrective_steering", "CORRECTIVE"),
        number("ascent.corrective_steering_gain", "STEERING GAIN", "", 1, .05, 2, 0, 9.99),
        // THRUST
        flag("thrust.limit_q", "LIMIT Q"),
        number("thrust.max_q", "MAX Q", "Pa", 1, 1000, 0, 0, 9999999),
        flag("thrust.limit_overheat", "PREVENT OVERHEATS"),
        flag("thrust.limit_acceleration", "LIMIT ACCEL"),
        number("thrust.max_acceleration", "MAX ACCELERATION", "m/s\xC2\xB2", 1, 1, 1, 0, 9999),
        flag("thrust.limit_throttle", "LIMIT THROTTLE"),
        number("thrust.max_throttle", "MAX THROTTLE", "%", 100, 5, 0, 0, 100),
        flag("thrust.limit_min_throttle", "MIN THROTTLE"),
        number("thrust.min_throttle", "MIN THROTTLE", "%", 100, 5, 0, 0, 100),
        flag("thrust.smooth_throttle", "SMOOTH THROTTLE"),
        // STAGING & FINISH
        flag("ascent.autostage", "AUTOSTAGE"),
        number("ascent.last_stage", "LAST STAGE", "", 1, 1, 0, 0, 99, true),
        flag("ascent.autodeploy_solar_panels", "DEPLOY SOLAR"),
        flag("ascent.autodeploy_antennas", "DEPLOY ANTENNAS"),
        flag("ascent.skip_circularization", "SKIP CIRCULARIZATION"),
    };
    return fields;
}

const MjField* mj_field(const std::string& key) {
    for (const MjField& f : mj_fields())
        if (key == f.key) return &f;
    return nullptr;
}

std::string mj_format(const MjField& f, double si) {
    std::string s = fmt::str("%.*f", f.decimals, si * f.scale);
    if (s.find_first_not_of("-0.") == std::string::npos && s[0] == '-') s.erase(0, 1);  // never "-0.0"
    return s;
}

std::string mj_pattern(const MjField& f) {
    const double top = std::max(std::fabs(f.min), std::fabs(f.max));
    int digits = 1;
    for (double v = 10; v <= top + 1e-9; v *= 10) ++digits;
    std::string p = (f.min < 0 ? "8" : "") + std::string(size_t(digits), '8');
    if (f.decimals > 0) p += "." + std::string(size_t(f.decimals), '8');
    return p;
}

std::optional<double> mj_si(const MjField& f, double display) {
    if (f.flag || !std::isfinite(display)) return std::nullopt;
    if (f.integer && display != std::round(display)) return std::nullopt;
    if (display < f.min - 1e-9 || display > f.max + 1e-9) return std::nullopt;
    return display / f.scale;
}

std::string mj_wire(double si) {
    std::string s = fmt::str("%.10g", si);
    return s == "-0" ? "0" : s;
}

double mj_step(const MjField& f, double si, int direction, bool shift) {
    const double grid = std::pow(10.0, f.decimals);
    double d = si * f.scale + direction * f.step * (shift ? 10 : 1);
    d = std::clamp(d, f.min, f.max);
    d = std::round(d * grid) / grid;
    return d / f.scale;
}

const MjModule MJ_MODULES[MJ_MODULE_COUNT] = {
    {"ASCENT GUIDANCE", "LAUNCH INTO A TARGET ORBIT", true},
    {"MANEUVER PLANNER", "PLAN MANEUVER NODES", false},
    {"NODE EXECUTOR", "EXECUTE THE NEXT NODE", false},
    {"LANDING GUIDANCE", "LAND ON A TARGET", false},
    {"RENDEZVOUS", "MEET A TARGET VESSEL", false},
    {"DOCKING", "DOCKING AUTOPILOT", false},
    {"TRANSLATRON", "HOLD A VERTICAL SPEED", false},
    {"SPACEPLANE", "AIRCRAFT AUTOPILOT", false},
    {"SMART A.S.S.", "ATTITUDE HOLD", false},
};

const char* const MJ_PATHS[3] = {"CLASSIC", "GRAVITY TURN", "PVG"};

const char* mj_action_name(MjAction a) {
    switch (a) {
        case MjAction::Engage: return "ascent.engage";
        case MjAction::Disengage: return "ascent.disengage";
        case MjAction::LaunchRendezvous: return "ascent.launch_rendezvous";
        case MjAction::LaunchTargetPlane: return "ascent.launch_target_plane";
        case MjAction::AbortTimedLaunch: return "ascent.abort_timed_launch";
    }
    return "?";
}

const char* mj_action_legend(MjAction a) {
    switch (a) {
        case MjAction::Engage: return "ENGAGE AUTOPILOT";
        case MjAction::Disengage: return "DISENGAGE";
        case MjAction::LaunchRendezvous: return "LAUNCH TO RENDEZVOUS";
        case MjAction::LaunchTargetPlane: return "LAUNCH TO TARGET PLANE";
        case MjAction::AbortTimedLaunch: return "ABORT TIMED LAUNCH";
    }
    return "?";
}

bool mj_action_needs_arm(MjAction a) {
    return a == MjAction::Engage || a == MjAction::LaunchRendezvous || a == MjAction::LaunchTargetPlane;
}

const char* mj_pad_legend(int key) {
    static const char* const digits[10] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
    if (key >= 0 && key < 10) return digits[key];
    switch (key) {
        case PAD_POINT: return ".";
        case PAD_MINUS: return "-";
        case PAD_DELETE: return "DEL";
        case PAD_CANCEL: return "CANCEL";
        case PAD_SET: return "SET";
        default: return "?";
    }
}

std::string MjControl::id() const {
    switch (kind) {
        case Minus:
        case Plus:
        case Value:
        case Flag: return key;
        case Path: return "path:" + std::to_string(index);
        case Action: return "action:" + std::to_string(index);
        case Arm: return "arm";
        case Module: return "module:" + std::to_string(index);
        case Tab: return "tab:" + std::to_string(index);
        case Back: return "back";
        case Pad: return "pad:" + std::to_string(index);
    }
    return "?";
}

// ============================================================================================ keypad
void Keypad::open(const MjField& f, const std::optional<double>& si) {
    key_ = f.key;
    text_ = si ? mj_format(f, *si) : "";
    error_.clear();
    open_ = true;
    fresh_ = true;
}

void Keypad::type(char c) {
    if (!open_) return;
    error_.clear();
    if (c == '-') {  // the sign toggles, whatever was typed so far
        if (!text_.empty() && text_[0] == '-') text_.erase(0, 1);
        else if (text_.size() < MAX_CHARS) text_.insert(0, 1, '-');
        fresh_ = false;
        return;
    }
    if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '.')) return;
    if (fresh_) {  // the first digit replaces the shown value
        text_.clear();
        fresh_ = false;
    }
    if (c == '.' && text_.find('.') != std::string::npos) return;
    if (text_.size() >= MAX_CHARS) return;
    if (c == '.' && (text_.empty() || text_ == "-")) text_ += '0';
    text_ += c;
}

void Keypad::backspace() {
    if (!open_) return;
    error_.clear();
    if (fresh_) text_.clear();
    else if (!text_.empty()) text_.pop_back();
    fresh_ = false;
}

std::optional<double> Keypad::accept() {
    const MjField* f = mj_field(key_);
    if (!open_ || !f) return std::nullopt;
    if (text_.empty() || text_ == "-") return error_ = "TYPE A VALUE", std::nullopt;
    char* end = nullptr;
    const double v = std::strtod(text_.c_str(), &end);
    if (end == text_.c_str() || *end != '\0' || !std::isfinite(v)) return error_ = "NOT A NUMBER", std::nullopt;
    if (f->integer && v != std::round(v)) return error_ = "WHOLE NUMBERS ONLY", std::nullopt;
    const std::optional<double> si = mj_si(*f, v);
    if (!si) {
        const MjField range = *f;
        return error_ = fmt::str("RANGE %s TO %s", mj_format(range, range.min / range.scale).c_str(),
                                 mj_format(range, range.max / range.scale).c_str()),
               std::nullopt;
    }
    return si;
}

// ============================================================================================ input
void MechJebInput::send_set(const std::string& id, const MjField& f, double si, double now, MechJebPress& out) {
    Command cmd{Command::MechJebSet};
    cmd.key = f.key;
    cmd.text = mj_wire(si);
    out.commands.push_back(cmd);
    pending_[id] = Pending{now, f.key, f.key, si};
    rejected_.erase(id);
}

void MechJebInput::send_action(MjAction a, double now, MechJebPress& out) {
    Command cmd{Command::MechJebAction};
    cmd.key = mj_action_name(a);
    out.commands.push_back(cmd);
    const std::string id = MjControl{MjControl::Action, int(a)}.id();
    Pending p{now, cmd.key, "", 0};
    if (a == MjAction::AbortTimedLaunch) p.key.clear();  // nothing in the report says it happened
    else p.key = "ascent.enabled", p.expect = a == MjAction::Disengage ? 0 : 1;
    pending_[id] = p;
    rejected_.erase(id);
}

void MechJebInput::reject(const std::string& id, const std::string& why, double now) {
    rejected_[id] = now + REJECT_FLASH;
    if (!why.empty()) notice_ = why, notice_until_ = now + NOTICE_TIME;
}

MechJebPress MechJebInput::press(const MjControl& c, const Snapshot& s, double now, bool shift) {
    MechJebPress r;
    const MechJebReport* mj = s.mechjeb && s.mechjeb->ok() ? s.mechjeb.get() : nullptr;
    switch (c.kind) {
        case MjControl::Tab: r.scene = static_cast<FlightScene>(c.index); return r;
        case MjControl::Back:
            page_ = MjPage::Menu;
            keypad_.close();
            armed_until_ = -1;
            return r;
        case MjControl::Module:
            if (c.index >= 0 && c.index < MJ_MODULE_COUNT && MJ_MODULES[c.index].live) page_ = MjPage::Ascent;
            else reject(c.id(), "", now);  // phase 2: coming later
            return r;
        case MjControl::Pad: {
            if (!keypad_.is_open()) return r;
            if (c.index >= 0 && c.index < 10) keypad_.type(char('0' + c.index));
            else if (c.index == PAD_POINT) keypad_.type('.');
            else if (c.index == PAD_MINUS) keypad_.type('-');
            else if (c.index == PAD_DELETE) keypad_.backspace();
            else if (c.index == PAD_CANCEL) keypad_.close();
            else if (c.index == PAD_SET) {
                const MjField* f = mj_field(keypad_.key());
                if (const std::optional<double> si = keypad_.accept(); si && f) {
                    keypad_.close();
                    send_set(f->key, *f, *si, now, r);
                }
            }
            return r;
        }
        default: break;
    }
    if (keypad_.is_open() || page_ != MjPage::Ascent) return r;  // the keypad is modal
    if (!mj) return r;  // no MechJeb data: the controls are veiled and do nothing
    const MjField* f = mj_field(c.key);
    switch (c.kind) {
        case MjControl::Minus:
        case MjControl::Plus: {
            const std::optional<double> v = mj->number(c.key);
            if (!f || f->flag || !v) return reject(c.id(), "", now), r;  // unknown: nothing to step from
            send_set(c.id(), *f, mj_step(*f, *v, c.kind == MjControl::Plus ? 1 : -1, shift), now, r);
            break;
        }
        case MjControl::Value:
            if (!f || f->flag || !mj->has(c.key)) return reject(c.id(), "", now), r;
            keypad_.open(*f, mj->number(c.key));
            break;
        case MjControl::Flag: {
            const std::optional<bool> on = mj->flag(c.key);
            if (!f || !f->flag || !on) return reject(c.id(), "", now), r;
            send_set(c.id(), *f, *on ? 0 : 1, now, r);
            break;
        }
        case MjControl::Path:
            if (c.index < 0 || c.index > 2 || !mj->has("ascent.path")) return reject(c.id(), "", now), r;
            send_set(c.id(), *mj_field("ascent.path"), c.index, now, r);
            break;
        case MjControl::Arm:
            armed_until_ = armed(now) ? -1 : now + ARM_WINDOW;
            rejected_.erase("arm");
            break;
        case MjControl::Action: {
            if (c.index < 0 || c.index >= MJ_ACTION_COUNT) return r;
            const MjAction a = static_cast<MjAction>(c.index);
            if (mj_action_needs_arm(a)) {
                if (!armed(now)) return rejected_["arm"] = now + REJECT_FLASH, r;  // the ARM lamp flashes: arm first
                armed_until_ = -1;  // one engage per arming
            }
            send_action(a, now, r);
            break;
        }
        default: break;
    }
    return r;
}

bool MechJebInput::key(const KeyEvent& e, const Snapshot& s, double now, MechJebPress& out) {
    if (!keypad_.is_open() || e.ctrl || e.alt) return false;
    switch (e.key) {
        case Key::Digit:
            if (e.digit < 0 || e.digit > 9) return false;
            out = press(MjControl{MjControl::Pad, e.digit}, s, now);
            return true;
        case Key::Period: out = press(MjControl{MjControl::Pad, PAD_POINT}, s, now); return true;
        case Key::Minus: out = press(MjControl{MjControl::Pad, PAD_MINUS}, s, now); return true;
        case Key::Backspace: out = press(MjControl{MjControl::Pad, PAD_DELETE}, s, now); return true;
        case Key::Enter: out = press(MjControl{MjControl::Pad, PAD_SET}, s, now); return true;
        case Key::Escape: out = press(MjControl{MjControl::Pad, PAD_CANCEL}, s, now); return true;
        default: return false;
    }
}

void MechJebInput::update(const Snapshot& s, double now) {
    // The plugin's reply to the last MechJebSet / MechJebAction: a refusal ends that change at once.
    if (s.mechjeb_reply && s.mechjeb_reply->seq != reply_seen_) {
        reply_seen_ = s.mechjeb_reply->seq;
        const MechJebReply& reply = *s.mechjeb_reply;
        if (reply.reply != "ok") {
            for (auto it = pending_.begin(); it != pending_.end();) {
                if (it->second.target == reply.target) {
                    rejected_[it->first] = now + REJECT_FLASH;
                    it = pending_.erase(it);
                } else {
                    ++it;
                }
            }
            std::string name = reply.target;
            if (const MjField* f = mj_field(reply.target)) name = f->label;
            for (int i = 0; i < MJ_ACTION_COUNT; ++i)
                if (reply.target == mj_action_name(MjAction(i))) name = mj_action_legend(MjAction(i));
            notice_ = name + ": " + upper(reply.reply);
            notice_until_ = now + NOTICE_TIME;
        }
    }
    const MechJebReport* mj = s.mechjeb && s.mechjeb->ok() ? s.mechjeb.get() : nullptr;
    for (auto it = pending_.begin(); it != pending_.end();) {
        const Pending& p = it->second;
        bool done = false;
        if (p.key.empty()) {
            done = now - p.since >= SENT_FLASH;
        } else {
            if (mj)
                if (const std::optional<double> v = mj->number(p.key))
                    done = std::fabs(*v - p.expect) <= std::max(1e-9, std::fabs(p.expect) * 1e-6);
            if (!done && now - p.since > CONFIRM_TIMEOUT) {
                rejected_[it->first] = now + REJECT_FLASH;
                done = true;
            }
        }
        it = done ? pending_.erase(it) : std::next(it);
    }
    for (auto it = rejected_.begin(); it != rejected_.end();) it = now >= it->second ? rejected_.erase(it) : std::next(it);
    if (armed_until_ >= 0 && now >= armed_until_) armed_until_ = -1;
}

void MechJebInput::reset() {
    keypad_.close();
    armed_until_ = -1;
    pending_.clear();
    rejected_.clear();
    notice_.clear();
    notice_until_ = -1;
}

MechJebInput::Feedback MechJebInput::feedback(const std::string& id, double now) const {
    auto r = rejected_.find(id);
    if (r != rejected_.end() && now < r->second) return Feedback::Rejected;
    auto p = pending_.find(id);
    if (p != pending_.end() && (!p->second.key.empty() || now - p->second.since < SENT_FLASH)) return Feedback::Sent;
    return Feedback::None;
}

bool MechJebInput::busy(double now) const {
    return armed(now) || !pending_.empty() || !rejected_.empty() || now < notice_until_;
}

}  // namespace ksphmi
