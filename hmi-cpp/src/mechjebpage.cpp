#include "mechjebpage.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "fmt.hpp"

namespace ksphmi {

namespace {

constexpr double RH = 4.6, PITCH = 5.5, IN = 4.6, PAD = 1.2;  // row height, row pitch, below a title, inner pad

std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

/// Builds the rows of one group: label or flag lamp on the left, - [value] + and the unit on the right.
struct Rows {
    MechJebLayout& l;
    double u, x, w, y;

    void caption(const std::string& text, float units, double cx, double cy, Anchor a, double room, bool dim = false) {
        l.captions.push_back({text, units, cx, cy, a, room, dim});
    }
    /// The stepper of `key` at the right of the row; returns where the left part ends.
    double stepper(const std::string& key) {
        const MjField* f = mj_field(key);
        const double uw = 5.2 * u, bw = 3.8 * u, vw = 11 * u, g = .4 * u;
        const double ux = x + w - uw;
        const Rect plus = Rect::f(ux - g - bw, y, bw, RH * u);
        const Rect value = Rect::f(plus.x - g - vw, y, vw, RH * u);
        const Rect minus = Rect::f(value.x - g - bw, y, bw, RH * u);
        l.items.push_back({MjControl{MjControl::Minus, 0, key}, minus, "-"});
        l.items.push_back({MjControl{MjControl::Value, 0, key}, value, ""});
        l.items.push_back({MjControl{MjControl::Plus, 0, key}, plus, "+"});
        if (f && *f->unit) caption(f->unit, 1.4f, ux + .5 * u, y + RH * u / 2, Anchor::MidLeft, uw - .6 * u, true);
        return minus.x - .8 * u;
    }
    void number(const std::string& key) {
        const double left = stepper(key);
        const MjField* f = mj_field(key);
        caption(f ? f->label : key, 1.5f, x, y + RH * u / 2, Anchor::MidLeft, left - x);
        y += PITCH * u;
    }
    void flag_number(const std::string& flag, const std::string& key) {
        const double left = stepper(key);
        const MjField* f = mj_field(flag);
        l.items.push_back({MjControl{MjControl::Flag, 0, flag}, Rect::f(x, y, left - x, RH * u), f ? f->label : flag});
        y += PITCH * u;
    }
    void flags(const std::vector<std::string>& keys) {
        const double g = .8 * u, fw = (w - g * (keys.size() - 1)) / keys.size();
        for (size_t i = 0; i < keys.size(); ++i) {
            const MjField* f = mj_field(keys[i]);
            l.items.push_back({MjControl{MjControl::Flag, 0, keys[i]}, Rect::f(x + i * (fw + g), y, fw, RH * u),
                               f ? f->label : keys[i]});
        }
        y += PITCH * u;
    }
};

/// `s` cut with "..." until it fits `room` pixels at `units` (the painter may still shrink it a little).
std::string shorten(Canvas& c, std::string s, float units, double room) {
    if (c.text_width(c.font(units), s) <= room * 1.4) return s;  // the painter fits up to ~1.7x by shrinking; Apollo letter-spaces
    while (s.size() > 4 && c.text_width(c.font(units), s + "...") > room * 1.4) s.pop_back();
    return s + "...";
}

}  // namespace

// ============================================================================================ layout
MechJebLayout mechjeb_layout(const Canvas& c, MjPage page, int path) {
    const double u = c.u, W = c.w, m = 2.5 * u, g = 1.8 * u;
    MechJebLayout l;
    l.ribbon = ribbon_layout(c);
    for (int i = 0; i < FLIGHT_SCENES; ++i)
        l.items.push_back({MjControl{MjControl::Tab, i}, l.ribbon.tabs[size_t(i)], ctl_legend(ctl_at(Ctl::TabInstruments, i))});
    l.bar = Rect::f(m, 15.8 * u, W - 2 * m, 4.8 * u);
    const double top = 21.8 * u, bottom = 98.5 * u;

    if (page == MjPage::Menu) {
        l.captions.push_back({"MECHJEB 2  \xC2\xB7  MODULES", 2.2f, l.bar.x + 1.2 * u, double(l.bar.centery()),
                              Anchor::MidLeft, (W - 2 * m) * .5, false, true});
        const double tw = (W - 2 * m - 2 * g) / 3, th = (bottom - top - 2 * g) / 3;
        for (int i = 0; i < MJ_MODULE_COUNT; ++i) {
            const Rect tile = Rect::f(m + (i % 3) * (tw + g), top + (i / 3) * (th + g), tw, th);
            l.groups.push_back({tile, MJ_MODULES[i].title});
            l.items.push_back({MjControl{MjControl::Module, i}, tile, ""});
            const double ix = tile.x + PAD * u, iw = tile.w - 2 * PAD * u;
            l.captions.push_back({MJ_MODULES[i].summary, 1.6f, ix, tile.y + (IN + 1.6) * u, Anchor::MidLeft, iw, true});
            l.tile_state[size_t(i)] = Rect::f(ix, tile.bottom() - 6.4 * u, iw, 4.6 * u);
            if (!MJ_MODULES[i].live)
                l.captions.push_back({"PHASE 2  \xC2\xB7  COMING LATER", 1.6f, double(tile.centerx()),
                                      double(l.tile_state[size_t(i)].centery()), Anchor::Center, iw, true});
        }
        return l;
    }

    // Ascent Guidance: back button and title on the page bar, then three columns of groups.
    const Rect back = Rect::f(l.bar.x + .6 * u, l.bar.y + .5 * u, 14 * u, l.bar.h - 1.0 * u);
    l.items.push_back({MjControl{MjControl::Back}, back, "< MECHJEB"});
    l.captions.push_back({"ASCENT GUIDANCE", 2.2f, back.right() + 1.6 * u, double(l.bar.centery()), Anchor::MidLeft,
                          (W - 2 * m) * .4, false, true});
    const double colw = (W - 2 * m - 2 * g) / 3, iw = colw - 2 * PAD * u;
    const double x0 = m, x1 = m + colw + g, x2 = m + 2 * (colw + g);
    auto group = [&](double x, double y, double h, const char* title) {
        l.groups.push_back({Rect::f(x, y, colw, h), title});
        return Rows{l, u, x + PAD * u, iw, y + IN * u};
    };
    auto height = [&](int rows, double extra = 0) { return (IN + rows * PITCH + .4 + extra) * u; };

    // column 1: TARGET ORBIT, ASCENT PATH
    double y = top;
    {
        Rows r = group(x0, y, height(2), "TARGET ORBIT");
        r.number("ascent.altitude");
        r.number("ascent.inclination");
        y += height(2) + g;
    }
    {
        Rows r = group(x0, y, bottom - y, "ASCENT PATH");
        const double pw = (iw - 1.6 * u) / 3;
        for (int i = 0; i < 3; ++i)
            l.items.push_back({MjControl{MjControl::Path, i}, Rect::f(r.x + i * (pw + .8 * u), r.y, pw, RH * u), MJ_PATHS[i]});
        r.y += (PITCH + .6) * u;
        if (path == 0) {
            r.flags({"ascent.classic.auto"});
            for (const char* k : {"ascent.classic.turn_start_altitude", "ascent.classic.turn_start_velocity",
                                  "ascent.classic.turn_end_altitude", "ascent.classic.turn_end_angle",
                                  "ascent.classic.turn_shape_exponent"})
                r.number(k);
        } else if (path == 1) {
            r.number("ascent.classic.turn_start_altitude");
            r.number("ascent.classic.turn_start_velocity");
            r.caption("MORE GRAVITY TURN SETTINGS IN A LATER VERSION", 1.4f, r.x, r.y + RH * u / 2, Anchor::MidLeft, iw, true);
        } else if (path == 2) {
            for (const char* k : {"ascent.pvg.desired_apoapsis", "ascent.pvg.desired_attach_alt", "ascent.pvg.desired_fpa",
                                  "ascent.pvg.pitch_start_velocity", "ascent.pvg.pitch_rate"})
                r.number(k);
        } else {
            r.caption("PATH SETTINGS: NO DATA", 1.5f, r.x, r.y + RH * u / 2, Anchor::MidLeft, iw, true);
        }
    }
    // column 2: GUIDANCE, THRUST
    y = top;
    {
        Rows r = group(x1, y, height(5), "GUIDANCE");
        r.flag_number("ascent.force_roll", "ascent.vertical_roll");
        r.number("ascent.turn_roll");
        r.flag_number("ascent.limit_aoa", "ascent.max_aoa");
        r.flag_number("ascent.limit_qa", "ascent.max_qa");
        r.flag_number("ascent.corrective_steering", "ascent.corrective_steering_gain");
        y += height(5) + g;
    }
    {
        Rows r = group(x1, y, bottom - y, "THRUST");
        r.flag_number("thrust.limit_q", "thrust.max_q");
        r.flag_number("thrust.limit_acceleration", "thrust.max_acceleration");
        r.flag_number("thrust.limit_throttle", "thrust.max_throttle");
        r.flag_number("thrust.limit_min_throttle", "thrust.min_throttle");
        r.flags({"thrust.limit_overheat", "thrust.smooth_throttle"});
    }
    // column 3: STAGING & FINISH, LAUNCH
    y = top;
    {
        Rows r = group(x2, y, height(3), "STAGING & FINISH");
        r.flag_number("ascent.autostage", "ascent.last_stage");
        r.flags({"ascent.autodeploy_solar_panels", "ascent.autodeploy_antennas"});
        r.flags({"ascent.skip_circularization"});
        y += height(3) + g;
    }
    {
        Rows r = group(x2, y, bottom - y, "LAUNCH");
        const double ix = r.x, bh = 7 * u, aw = iw * .2, dw = iw * .3, ew = iw - aw - dw - 1.6 * u;
        const Rect arm = Rect::f(ix, r.y, aw, bh);
        l.items.push_back({MjControl{MjControl::Arm}, arm, "ARM"});
        l.items.push_back({MjControl{MjControl::Action, int(MjAction::Engage)}, Rect::f(arm.right() + .8 * u, r.y, ew, bh),
                           mj_action_legend(MjAction::Engage)});
        l.items.push_back({MjControl{MjControl::Action, int(MjAction::Disengage)},
                           Rect::f(ix + iw - dw, r.y, dw, bh), mj_action_legend(MjAction::Disengage)});
        l.arm_text = Rect::f(ix, r.y + bh + .3 * u, aw + 4 * u, 2.4 * u);
        r.y += bh + 3.2 * u;
        const double hw = (iw - .8 * u) / 2;
        l.items.push_back({MjControl{MjControl::Action, int(MjAction::LaunchRendezvous)}, Rect::f(ix, r.y, hw, RH * u),
                           mj_action_legend(MjAction::LaunchRendezvous)});
        l.items.push_back({MjControl{MjControl::Action, int(MjAction::LaunchTargetPlane)},
                           Rect::f(ix + hw + .8 * u, r.y, hw, RH * u), mj_action_legend(MjAction::LaunchTargetPlane)});
        r.y += PITCH * u;
        l.items.push_back({MjControl{MjControl::Action, int(MjAction::AbortTimedLaunch)}, Rect::f(ix, r.y, iw, RH * u),
                           mj_action_legend(MjAction::AbortTimedLaunch)});
        r.y += (PITCH + .6) * u;
        r.caption("STATUS", 1.4f, ix, r.y, Anchor::TopLeft, iw, true);
        l.status = Rect::f(ix, r.y + 2 * u, iw, 4.6 * u);
        l.notice = Rect::f(ix, l.status.bottom() + .2 * u, iw, 2.4 * u);
        r.y = l.notice.bottom() + .8 * u;
        r.caption("FLIGHT", 1.4f, ix, r.y, Anchor::TopLeft, iw, true);
        r.y += 2.2 * u;
        static const char* const names[4] = {"APOAPSIS  km", "PERIAPSIS  km", "ALTITUDE  km", "VERTICAL  m/s"};
        const double fw = (iw - 1.2 * u) / 2;
        for (int i = 0; i < 4; ++i) {
            const double fx = ix + (i % 2) * (fw + 1.2 * u), fy = r.y + (i / 2) * 6.6 * u;
            l.flight[size_t(i)] = Rect::f(fx, fy, fw, 4.2 * u);
            r.caption(names[i], 1.3f, fx, fy + 4.4 * u, Anchor::TopLeft, fw, true);
        }
    }
    return l;
}

KeypadLayout keypad_layout(const Canvas& c) {
    const double u = c.u, w = std::min(48 * u, c.w - 8 * u), h = 58 * u;
    KeypadLayout k;
    k.rect = Rect::f(c.w / 2.0 - w / 2, 24 * u, w, h);
    const double ix = k.rect.x + 2 * u, iw = w - 4 * u;
    k.title_y = k.rect.y + 2.6 * u;
    k.entry = Rect::f(ix, k.rect.y + 5 * u, iw, 7.4 * u);
    k.error_y = k.entry.bottom() + 1.7 * u;
    k.range_y = k.entry.bottom() + 4.2 * u;
    const double gap = .9 * u, kw = (iw - 3 * gap) / 4, kh = 8.2 * u, ky = k.rect.y + 19.4 * u;
    auto at = [&](int col, int row, double span = 1) {
        return Rect::f(ix + col * (kw + gap), ky + row * (kh + gap), kw * span + gap * (span - 1), kh);
    };
    const int grid[3][3] = {{7, 8, 9}, {4, 5, 6}, {1, 2, 3}};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) k.keys[size_t(grid[row][col])] = at(col, row);
    k.keys[PAD_DELETE] = at(3, 0);
    k.keys[PAD_MINUS] = at(3, 1);
    k.keys[PAD_POINT] = at(3, 2);
    k.keys[0] = at(0, 3);
    const double rest = (iw - kw - gap - gap) / 2;
    k.keys[PAD_CANCEL] = Rect::f(ix + kw + gap, ky + 3 * (kh + gap), rest, kh);
    k.keys[PAD_SET] = Rect::f(ix + kw + 2 * gap + rest, ky + 3 * (kh + gap), rest, kh);
    return k;
}

std::optional<MjControl> mechjeb_hit(const MechJebLayout& l, bool keypad_open, const KeypadLayout& k, int x, int y) {
    auto in = [&](const Rect& r) { return r.w > 0 && x >= r.x && x < r.right() && y >= r.y && y < r.bottom(); };
    if (keypad_open) {
        for (int i = 0; i < PAD_KEYS; ++i)
            if (in(k.keys[size_t(i)])) return MjControl{MjControl::Pad, i};
        return std::nullopt;  // modal: CANCEL or Escape closes it
    }
    for (const MjItem& i : l.items)
        if (in(i.face)) return i.ctl;
    return std::nullopt;
}

int mechjeb_path(const Snapshot& s) {
    if (!s.mechjeb || !s.mechjeb->ok()) return -1;
    const std::optional<double> p = s.mechjeb->number("ascent.path");
    return p && *p >= 0 && *p <= 2 && *p == std::round(*p) ? int(*p) : -1;
}

// ============================================================================================ composition
void paint_mechjeb(Canvas& c, bool art, const Snapshot& s, const MechJebView& v, CockpitPainter& p) {
    const MechJebInput* in = art ? nullptr : v.input;
    const MjPage page = v.input ? v.input->page() : MjPage::Menu;
    const MechJebLayout l = mechjeb_layout(c, page, mechjeb_path(s));
    const double u = c.u, now = v.now;
    const MechJebReport* mj = s.mechjeb && s.mechjeb->ok() ? s.mechjeb.get() : nullptr;

    paint_ribbon(c, art, FlightScene::MechJeb, p);
    if (art) {
        c.rounded_rect(l.bar, float(.5 * u), p.key_fill());
        for (const auto& g : l.groups) p.frame(c, g.rect, g.title);
        for (const auto& t : l.captions)
            p.text(c, t.text, t.units, t.bar ? p.key_ink() : t.dim ? p.dim_ink() : p.ink(), t.x, t.y, t.anchor, t.room);
    }

    // MechJeb's state on the page bar: never a value it did not report.
    std::string state;
    Lamp tone = Lamp::Off;
    if (!s.mechjeb) state = s.mechjeb_problem.empty() ? "MECHJEB: NO DATA" : "MECHJEB: NO DATA  (" + s.mechjeb_problem + ")";
    else if (s.mechjeb->status == MechJebReport::Status::Missing) state = "MECHJEB NOT INSTALLED", tone = Lamp::Amber;
    else if (s.mechjeb->status == MechJebReport::Status::NoCore) state = "NO MECHJEB CORE ON THIS VESSEL", tone = Lamp::Amber;
    else if (s.mechjeb->status == MechJebReport::Status::Error) state = "MECHJEB ERROR: " + upper(s.mechjeb->error), tone = Lamp::Red;
    else state = "MECHJEB " + s.mechjeb->version, tone = Lamp::Green;
    if (!art)
        p.text(c, state, 1.6f, tone == Lamp::Off ? p.key_ink() : p.lamp_color(tone), l.bar.right() - 1.2 * u,
               l.bar.centery(), Anchor::MidRight, l.bar.w * .45);

    const std::optional<bool> engaged = mj ? mj->flag("ascent.enabled") : std::nullopt;
    const bool armed = in && in->armed(now);
    for (const MjItem& item : l.items) {
        const MjControl& k = item.ctl;
        switch (k.kind) {
            case MjControl::Tab: break;  // paint_ribbon
            case MjControl::Module: {
                const Rect& st = l.tile_state[size_t(k.index)];
                if (MJ_MODULES[k.index].live) {
                    p.lamp(c, st, "AUTOPILOT ENGAGED", engaged.value_or(false) ? Lamp::Green : Lamp::Off, art);
                    if (!art && !mj) p.veil(c, st);
                } else if (!art) {
                    p.veil(c, item.face.inflate(-.8 * u, -.8 * u));
                }
                break;
            }
            case MjControl::Value: {
                const MjField* f = mj_field(k.key);
                const std::string pattern = f ? mj_pattern(*f) : "8888";
                std::string text;
                if (!art && mj && f)
                    if (const std::optional<double> x = mj->number(k.key)) {
                        text = mj_format(*f, *x);
                        if (text.size() > pattern.size() + 1) text = std::string(pattern.size(), 'E');  // off the scale
                    }
                p.readout(c, item.face, pattern, text, art);
                break;
            }
            case MjControl::Flag:
                p.lamp(c, item.face, item.legend, mj && mj->flag(k.key).value_or(false) ? Lamp::Green : Lamp::Off, art);
                break;
            case MjControl::Path:
                p.lamp(c, item.face, item.legend, mechjeb_path(s) == k.index ? Lamp::Green : Lamp::Off, art);
                break;
            case MjControl::Arm: p.lamp(c, item.face, item.legend, armed ? Lamp::Amber : Lamp::Off, art); break;
            case MjControl::Action: {
                Lamp lit = Lamp::Off;
                if (k.index == int(MjAction::Engage) && engaged.value_or(false)) lit = Lamp::Green;
                if (k.index == int(MjAction::Engage) && armed && !engaged.value_or(false)) lit = Lamp::White;
                if (k.index == int(MjAction::AbortTimedLaunch) && mj && mj->number("ascent.t_minus").value_or(0) > 0)
                    lit = Lamp::Amber;
                p.lamp(c, item.face, item.legend, lit, art);
                break;
            }
            default: p.lamp(c, item.face, item.legend, Lamp::Off, art); break;  // Back, Minus, Plus
        }
        // Without MechJeb data the page's controls cannot act: veiled, with no value behind them.
        const bool setting = k.kind != MjControl::Tab && k.kind != MjControl::Back && k.kind != MjControl::Module;
        if (!art && setting && !mj) p.veil(c, item.face);
        else if (!art && mj && (k.kind == MjControl::Value || k.kind == MjControl::Flag) && !mj->has(k.key))
            p.veil(c, item.face);  // this MechJeb has no such setting
    }

    if (page == MjPage::Ascent) {
        for (int i = 0; i < 4; ++i) {
            std::string text;
            if (!art && s.state == State::Flight) {
                const double values[4] = {s.apoapsis / 1000, s.periapsis / 1000, s.altitude / 1000, s.vertical_speed};
                if (std::isfinite(values[i]))
                    text = fmt::str(i == 3 ? "%.0f" : "%.1f", std::clamp(values[i], -99999.0, 999999.0));
            }
            p.readout(c, l.flight[size_t(i)], i == 3 ? "-88888" : "888888.8", text, art);
        }
        if (!art) {
            p.text(c, armed ? fmt::str("ARMED  %.0f S", std::ceil(in->armed_left(now))) : "SAFE", 1.5f,
                   armed ? p.lamp_color(Lamp::Amber) : p.dim_ink(), l.arm_text.x, l.arm_text.centery(), Anchor::MidLeft,
                   l.arm_text.w);
            std::string status;
            Color color = p.ink();
            if (!mj) status = state, color = tone == Lamp::Off ? p.dim_ink() : p.lamp_color(tone);
            else {
                status = upper(mj->text("ascent.status").value_or(""));
                if (status.empty()) status = "--";
                const double t = mj->number("ascent.t_minus").value_or(0);
                if (t > 0) status += fmt::str("  \xC2\xB7  T-%.0f S", std::ceil(t));
                if (engaged) status = std::string(*engaged ? "ENGAGED  \xC2\xB7  " : "OFF  \xC2\xB7  ") + status;
            }
            // One line when it fits, else two (split at the space nearest the middle), each shortened with "..."
            // if MechJeb's text is longer still: the status never leaves its group.
            const float units = 1.7f, small = 1.4f;
            if (c.text_width(c.font(units), status) <= l.status.w) {
                p.text(c, status, units, color, l.status.x, l.status.centery(), Anchor::MidLeft, l.status.w);
            } else {
                size_t cut = status.size() / 2;
                const size_t before = status.rfind(' ', cut), after = status.find(' ', cut);
                if (before != std::string::npos && (after == std::string::npos || cut - before <= after - cut)) cut = before;
                else if (after != std::string::npos) cut = after;
                const std::string lines[2] = {status.substr(0, cut), cut < status.size() ? status.substr(cut + 1) : ""};
                for (int i = 0; i < 2; ++i)
                    p.text(c, shorten(c, lines[i], small, l.status.w), small, color, l.status.x,
                           l.status.y + l.status.h * (i ? .75 : .25), Anchor::MidLeft, l.status.w);
            }
            if (in) {
                const std::string notice = in->notice(now);
                if (!notice.empty())
                    p.text(c, notice, 1.4f, p.lamp_color(Lamp::Amber), l.notice.x, l.notice.centery(), Anchor::MidLeft,
                           l.notice.w);
            }
        }
    }

    if (!in) return;
    for (const MjItem& item : l.items) p.feedback(c, item.face, in->feedback(item.ctl.id(), now), now);

    // The keypad overlay, on top of everything (live pass only).
    const Keypad& pad = in->keypad();
    if (!pad.is_open()) return;
    const KeypadLayout k = keypad_layout(c);
    const MjField* f = mj_field(pad.key());
    c.rounded_rect(Rect(0, l.ribbon.ribbon.bottom(), c.w, c.h - l.ribbon.ribbon.bottom()), 0, Color{0, 0, 0, 120});
    c.rounded_rect(k.rect.inflate(2 * u, 2 * u), float(.8 * u), p.well());  // one unit around the frame
    p.frame(c, k.rect, f ? std::string(f->label) + (*f->unit ? std::string("  (") + f->unit + ")" : "") : pad.key());
    p.readout(c, k.entry, std::string(Keypad::MAX_CHARS, '8'), "", true);
    p.readout(c, k.entry, std::string(Keypad::MAX_CHARS, '8'), pad.text(), false);
    if (!pad.error().empty())
        p.text(c, pad.error(), 1.5f, p.lamp_color(Lamp::Amber), k.rect.centerx(), k.error_y, Anchor::Center, k.rect.w - 4 * u);
    else if (f)
        p.text(c, fmt::str("RANGE %s TO %s", mj_format(*f, f->min / f->scale).c_str(), mj_format(*f, f->max / f->scale).c_str()),
               1.4f, p.dim_ink(), k.rect.centerx(), k.error_y, Anchor::Center, k.rect.w - 4 * u);
    p.text(c, "KEYBOARD: DIGITS  .  -  BACKSPACE  ENTER  ESC", 1.3f, p.dim_ink(), k.rect.centerx(), k.range_y,
           Anchor::Center, k.rect.w - 4 * u);
    for (int i = 0; i < PAD_KEYS; ++i) {
        p.lamp(c, k.keys[size_t(i)], mj_pad_legend(i), Lamp::Off, true);
        if (i == PAD_SET) p.lamp(c, k.keys[size_t(i)], mj_pad_legend(i), Lamp::Green, false);
    }
}

}  // namespace ksphmi
