#include "cockpit.hpp"

#include <algorithm>
#include <cmath>

#include "fmt.hpp"
#include "theme.hpp"

namespace ksphmi {

namespace {

constexpr Ctl SWITCH_ROW_SYSTEMS[8] = {Ctl::Gear,     Ctl::Lights,    Ctl::Brakes,    Ctl::Solar,
                                       Ctl::Antennas, Ctl::Radiators, Ctl::CargoBays, Ctl::Intakes};

size_t at(Ctl c) { return static_cast<size_t>(c); }

Color mix(Color a, Color b, double t) {
    return Color{uint8_t(std::lround(a.r + (b.r - a.r) * t)), uint8_t(std::lround(a.g + (b.g - a.g) * t)),
                 uint8_t(std::lround(a.b + (b.b - a.b) * t)), a.a};
}

void outline(Canvas& c, const Rect& r, float t, Color color) {
    c.fill_rect(float(r.x), float(r.y), float(r.w), t, color);
    c.fill_rect(float(r.x), float(r.bottom() - t), float(r.w), t, color);
    c.fill_rect(float(r.x), float(r.y), t, float(r.h), color);
    c.fill_rect(float(r.right() - t), float(r.y), t, float(r.h), color);
}

/// The navball marker of each SAS mode button (HMI order), in the skin's ink.
void sas_icon(Canvas& c, int mode, double cx, double cy, double r, Color ink) {
    const float w = std::max(1.5f, float(r * .2));
    auto ring = [&](double radius) { c.ring(float(cx), float(cy), float(radius), w, ink); };
    auto line = [&](double x0, double y0, double x1, double y1) {
        c.line({float(cx + x0 * r), float(cy + y0 * r)}, {float(cx + x1 * r), float(cy + y1 * r)}, w, ink);
    };
    auto dot = [&](double radius) { c.circle(float(cx), float(cy), float(radius * r), ink); };
    switch (mode) {
        case 0: ring(r * .75), dot(.18); break;  // HOLD
        case 1:                                    // PRO: circle, centre dot, three spikes
            ring(r * .62), dot(.16);
            line(-.62, 0, -1.05, 0), line(.62, 0, 1.05, 0), line(0, -.62, 0, -1.05);
            break;
        case 2:  // RET: circle with a cross and a tail
            ring(r * .62);
            line(-.4, -.4, .4, .4), line(.4, -.4, -.4, .4), line(0, .62, 0, 1.05);
            break;
        case 3:
        case 4: {  // N+ / N-: triangle (N- with a dot instead of the apex spike)
            const std::vector<Pt> tri = {{float(cx), float(cy - .8 * r)},
                                         {float(cx + .78 * r), float(cy + .55 * r)},
                                         {float(cx - .78 * r), float(cy + .55 * r)}};
            c.polyline(tri, true, w, ink);
            if (mode == 4) dot(.16);
            else line(0, -.8, 0, -1.1);
            break;
        }
        case 5:  // R+: circle, dot and four spikes out
            ring(r * .6), dot(.16);
            line(0, -.6, 0, -1.05), line(0, .6, 0, 1.05), line(-.6, 0, -1.05, 0), line(.6, 0, 1.05, 0);
            break;
        case 6:  // R-: circle with four spikes in
            ring(r * .75);
            line(0, -.75, 0, -.3), line(0, .75, 0, .3), line(-.75, 0, -.3, 0), line(.75, 0, .3, 0);
            break;
        case 7:  // TGT: circle, dot, four short ticks
            ring(r * .7), dot(.16);
            line(0, -.7, 0, -1.0), line(0, .7, 0, 1.0), line(-.7, 0, -1.0, 0), line(.7, 0, 1.0, 0);
            break;
        case 8:  // ANTI: circle with a cross
            ring(r * .75);
            line(-.45, -.45, .45, .45), line(.45, -.45, -.45, .45);
            break;
        default:  // NODE: open bowl with two horns and a dot
            c.arc(float(cx), float(cy - .1 * r), float(.85 * r), PI, 2 * PI, w, ink);
            line(-.85, -.1, -1.05, -.65), line(.85, -.1, 1.05, -.65);
            dot(.17);
            break;
    }
}

}  // namespace

// ============================================================================================ layout
RibbonLayout ribbon_layout(const Canvas& c) {
    const double u = c.u, W = c.w, m = 2.5 * u;
    RibbonLayout r;
    r.ribbon = Rect::f(m, 12 * u, W - 2 * m, 2.8 * u);
    const double tw = 16 * u, th = 2.2 * u, ty = r.ribbon.y + .3 * u, gap = u;
    const double x0 = W / 2 - (FLIGHT_SCENES * tw + (FLIGHT_SCENES - 1) * gap) / 2;
    for (int i = 0; i < FLIGHT_SCENES; ++i) r.tabs[size_t(i)] = Rect::f(x0 + i * (tw + gap), ty, tw, th);
    return r;
}

void paint_ribbon(Canvas& c, bool art, FlightScene shown, CockpitPainter& p) {
    const RibbonLayout r = ribbon_layout(c);
    const double u = c.u;
    if (art) {
        c.rounded_rect(r.ribbon, float(.5 * u), p.key_fill());
        p.text(c, "< CTRL -", 1.5f, p.key_ink(), r.ribbon.x + 1.2 * u, r.ribbon.centery(), Anchor::MidLeft);
        p.text(c, "CTRL + >", 1.5f, p.key_ink(), r.ribbon.right() - 1.2 * u, r.ribbon.centery(), Anchor::MidRight);
    }
    for (int i = 0; i < FLIGHT_SCENES; ++i) {
        const Ctl tab = ctl_at(Ctl::TabInstruments, i);
        p.lamp(c, r.tabs[size_t(i)], ctl_legend(tab), static_cast<FlightScene>(i) == shown ? Lamp::White : Lamp::Off, art);
    }
}

CockpitLayout cockpit_layout(const Canvas& c) {
    const double u = c.u, W = c.w, m = 2.5 * u, g = 1.8 * u;
    CockpitLayout l;
    auto R = [](double x, double y, double w, double h) { return Rect::f(x, y, w, h); };
    auto key_under = [&](Ctl id, const Rect& face, double gap = .5) {
        const std::string k = ctl_key(id);
        if (k.empty()) return;
        const double w = std::max(4.2 * u, k.size() * 1.05 * u + 1.6 * u);
        l.key[at(id)] = R(face.centerx() - w / 2, face.bottom() + gap * u, w, 2.2 * u);
    };
    auto caption = [&](const std::string& text, float units, double x, double y, Anchor a) {
        l.captions.push_back({text, units, x, y, a});
    };
    // Ribbon: which flight scene is shown, and a tab per scene (also clickable; paint_ribbon draws it).
    const RibbonLayout ribbon = ribbon_layout(c);
    l.ribbon = ribbon.ribbon;
    for (int i = 0; i < FLIGHT_SCENES; ++i) l.face[at(ctl_at(Ctl::TabInstruments, i))] = ribbon.tabs[size_t(i)];

    const double IN = 4.6 * u;  // below a group frame's title (the KSP skins' title bars are the tallest)
    const double T = 16.5 * u, colw = (W - 2 * m - 2 * g) / 3;
    const double x0 = m, x1 = m + colw + g, x2 = m + 2 * (colw + g), pad = 1.2 * u, iw = colw - 2 * pad;
    l.attitude = R(x0, T, colw, 66 * u);
    l.staging = R(x1, T, colw, 37 * u);
    l.recovery = R(x1, T + 39 * u, colw, 27 * u);
    l.systems = R(x2, T, colw, 37 * u);
    l.groups = R(x2, T + 39 * u, colw, 27 * u);
    l.strip = R(m, 84.5 * u, W - 2 * m, 14 * u);

    // A switch cell: name lamp on top, lever below, key cap at the bottom (15.4u tall).
    auto toggle = [&](Ctl id, double x, double y, double w) {
        l.face[at(id)] = R(x, y, w, 15.4 * u);
        l.lamp[at(id)] = R(x + .5 * u, y, w - u, 3.6 * u);
        const double s = std::min(8.0 * u, w - 2 * u);
        l.lever[at(id)] = R(x + w / 2 - s / 2, y + 4.4 * u, s, 8 * u);
        const std::string k = ctl_key(id);
        if (!k.empty()) l.key[at(id)] = R(x + w / 2 - 2.6 * u, y + 12.9 * u, 5.2 * u, 2.2 * u);
    };

    // ATTITUDE CONTROL
    {
        const double ix = x0 + pad, y = T + IN, cw = iw / 3;
        toggle(Ctl::Sas, ix, y, cw), toggle(Ctl::Rcs, ix + cw, y, cw), toggle(Ctl::Wheels, ix + 2 * cw, y, cw);
        caption("SAS MODE", 1.6f, ix, T + 21.4 * u, Anchor::TopLeft);
        const double bw = (iw - 4 * .8 * u) / 5, bh = 7.2 * u;
        for (int i = 0; i < 10; ++i) {
            const Ctl id = ctl_at(Ctl::Mode0, i);
            l.face[at(id)] = R(ix + (i % 5) * (bw + .8 * u), T + 23.8 * u + (i / 5) * (bh + 3.4 * u), bw, bh);
            key_under(id, l.face[at(id)]);
        }
        caption("SPEED REFERENCE", 1.6f, ix, T + 45.4 * u, Anchor::TopLeft);
        const double rw = (iw - 1.6 * u) / 3;
        for (int i = 0; i < 3; ++i) l.face[at(ctl_at(Ctl::RefOrbit, i))] = R(ix + i * (rw + .8 * u), T + 47.8 * u, rw, 4.2 * u);
        l.speed_readout = R(ix, T + 53.8 * u, iw, 6.2 * u);
        caption("SPEED  M/S", 1.3f, ix, T + 60.7 * u, Anchor::TopLeft);
    }
    // STAGING & ABORT
    {
        const double ix = x1 + pad, y = T + IN;
        l.stage_readout = R(ix, y, 9 * u, 8 * u);
        caption("CURRENT", 1.3f, ix + 4.5 * u, y + 8.5 * u, Anchor::MidTop);
        caption("NEXT STAGE", 1.4f, ix + 11 * u, y, Anchor::TopLeft);
        l.next_list = R(ix + 11 * u, y + 2.4 * u, iw - 11 * u, 8.4 * u);
        const double y2 = y + 13.6 * u, aw = iw * .27, sw = iw * .36;
        l.face[at(Ctl::Arm)] = R(ix, y2, aw, 7.6 * u);
        l.arm_lamp = R(ix, y2 + 8.4 * u, aw, 3.6 * u);
        l.face[at(Ctl::Stage)] = R(ix + iw / 2 - sw / 2, y2, sw, 13 * u);
        key_under(Ctl::Stage, l.face[at(Ctl::Stage)]);
        l.face[at(Ctl::Abort)] = R(ix + iw - aw, y2, aw, 12 * u);
        key_under(Ctl::Abort, l.face[at(Ctl::Abort)], 1.5);
    }
    // RECOVERY
    {
        const double ix = x1 + pad, y = l.recovery.y + IN, area = iw * .63, lx = ix + iw * .37;
        l.face[at(Ctl::Deploy)] = R(ix, y, iw * .34, 10.6 * u);
        const double lw = (area * .64 - .8 * u) / 2;
        for (int i = 0; i < 4; ++i) l.chute_lamps[size_t(i)] = R(lx + (i % 2) * (lw + .8 * u), y + (i / 2) * 4.4 * u, lw, 3.6 * u);
        l.face[at(Ctl::Cut)] = R(ix + iw - area * .33, y, area * .33, 8 * u);
        caption("CHUTE SAFETY", 1.4f, ix, y + 12 * u, Anchor::TopLeft);
        const double sw = (iw - 1.6 * u) / 3;
        for (int i = 0; i < 3; ++i) l.safety_lamps[size_t(i)] = R(ix + i * (sw + .8 * u), y + 14.2 * u, sw, 3.6 * u);
    }
    // SYSTEMS
    {
        const double ix = x2 + pad, y = T + IN, cw = iw / 4;
        for (int i = 0; i < 8; ++i) toggle(SWITCH_ROW_SYSTEMS[i], ix + (i % 4) * cw, y + (i / 4) * 16.0 * u, cw);
    }
    // ACTION GROUPS
    {
        const double ix = x2 + pad, y = l.groups.y + IN, bw = (iw - 4 * .8 * u) / 5, bh = 7 * u;
        for (int i = 0; i < 10; ++i) {
            const Ctl id = ctl_at(Ctl::Group1, i);
            l.face[at(id)] = R(ix + (i % 5) * (bw + .8 * u), y + (i / 5) * 10.2 * u, bw, bh);
            key_under(id, l.face[at(id)]);
        }
    }
    // THROTTLE · TIME WARP · VIEW
    {
        const double ix = l.strip.x + pad, right = l.strip.right() - pad, y = l.strip.y + IN;
        l.throttle_bar = R(ix, y + .6 * u, std::min(W * .24, 40 * u), 3.2 * u);
        caption("THROTTLE", 1.3f, ix, l.throttle_bar.bottom() + 1.1 * u, Anchor::TopLeft);
        l.throttle_readout = R(l.throttle_bar.right() + 1.2 * u, y - .2 * u, 8.5 * u, 5 * u);
        double x = l.throttle_readout.right() + 1.2 * u;
        for (Ctl id : {Ctl::ThrottleFull, Ctl::ThrottleCut}) {
            l.face[at(id)] = R(x, y, 6.5 * u, 4.2 * u);
            key_under(id, l.face[at(id)]);
            x += 7.3 * u;
        }
        const double wx = std::max(x + 2 * u, ix + (right - ix) * .46);
        l.warp_readout = R(wx, y - .2 * u, 10 * u, 5 * u);
        caption("TIME WARP", 1.3f, wx, l.warp_readout.bottom() + .5 * u, Anchor::TopLeft);
        x = l.warp_readout.right() + 1.2 * u;
        for (Ctl id : {Ctl::WarpDown, Ctl::WarpStop, Ctl::WarpUp}) {
            l.face[at(id)] = R(x, y, 5.4 * u, 4.2 * u);
            key_under(id, l.face[at(id)]);
            x += 6.2 * u;
        }
        x = right - 9 * u;
        for (Ctl id : {Ctl::Quickload, Ctl::Quicksave, Ctl::Map}) {
            l.face[at(id)] = R(x, y, 9 * u, 4.2 * u);
            key_under(id, l.face[at(id)]);
            x -= 9.8 * u;
        }
    }
    return l;
}

std::optional<Ctl> cockpit_hit(const CockpitLayout& l, int x, int y) {
    for (int i = 0; i < CTL_COUNT; ++i) {
        const Rect& r = l.face[size_t(i)];
        if (r.w > 0 && x >= r.x && x < r.right() && y >= r.y && y < r.bottom()) return static_cast<Ctl>(i);
    }
    return std::nullopt;
}

// ============================================================================================ default materials
Color CockpitPainter::lamp_color(Lamp l) const {
    switch (l) {
        case Lamp::Green: return theme::GOOD;
        case Lamp::White: return Color{236, 236, 222};
        case Lamp::Amber: return theme::WARN;
        case Lamp::Red: return theme::BAD;
        default: return Color{40, 48, 60};
    }
}

std::string shorten_to(CockpitPainter& p, Canvas& c, std::string s, float units, double room) {
    if (room <= 0 || p.measure(c, s, units) <= room) return s;
    while (!s.empty() && p.measure(c, s + "...", units) > room) {
        size_t end = s.size() - 1;
        while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xc0) == 0x80) --end;  // whole UTF-8 characters
        s.resize(end);
    }
    return s.empty() ? s : s + "...";
}

void CockpitPainter::text(Canvas& c, const std::string& s, float units, Color color, double x, double y, Anchor a,
                          double room) {
    if (s.empty()) return;
    float size = units;
    std::string shown = s;
    if (room > 0) {
        const int w = measure(c, s, units);
        if (w > room) size = std::max(.8f, float(units * room / std::max(1, w)));
        shown = shorten_to(*this, c, s, size, room);
    }
    c.text(shown, size, color, x, y, a);
}

void CockpitPainter::lever(Canvas& c, const Rect& r, std::optional<bool> on, bool fitted, bool art) {
    const double u = c.u;
    const Rect track = Rect::f(r.centerx() - 1.6 * u, r.y + .4 * u, 3.2 * u, r.h - .8 * u);
    if (art) {
        c.rounded_rect(track.inflate(.4 * u, .4 * u), float(1.9 * u), mix(well(), dim_ink(), .25));
        c.rounded_rect(track, float(1.6 * u), well());
        return;
    }
    if (!on) return;  // unknown: no knob, never a guessed position
    const double ky = *on ? track.y + 1.6 * u : track.bottom() - 1.6 * u;
    const Color knob = !fitted ? mix(well(), dim_ink(), .4) : *on ? lamp_color(Lamp::Green) : mix(well(), ink(), .55);
    c.circle(float(track.centerx()), float(ky), float(1.35 * u), knob);
}

void CockpitPainter::readout(Canvas& c, const Rect& r, const std::string&, const std::string& text, bool art) {
    if (art) {
        c.rounded_rect(r, float(.5 * c.u), well());
        return;
    }
    this->text(c, text, float(r.h / c.u * .62), ink(), r.right() - .8 * c.u, r.centery(), Anchor::MidRight, r.w - 1.6 * c.u);
}

void CockpitPainter::keycap(Canvas& c, const Rect& r, const std::string& key) {
    c.rounded_rect(r, float(.35 * c.u), key_fill());
    text(c, key, float(r.h / c.u * .58), key_ink(), r.centerx(), r.centery(), Anchor::Center, r.w - .6 * c.u);
}

void CockpitPainter::bar(Canvas& c, const Rect& r, double fraction, bool art) {
    if (art) {
        c.rounded_rect(r, float(.4 * c.u), well());
        for (int i = 0; i <= 10; ++i) {
            const float x = float(r.x + r.w * i / 10.0);
            c.fill_rect(x, float(r.bottom() + .2 * c.u), std::max(1.0f, float(.12 * c.u)), float(.6 * c.u), dim_ink());
        }
        return;
    }
    const double f = std::clamp(fraction, 0.0, 1.0);
    if (f > 0)
        c.rounded_rect(Rect::f(r.x + 2, r.y + 2, (r.w - 4) * f, r.h - 4), float(.3 * c.u), lamp_color(Lamp::Green));
}

void CockpitPainter::guard(Canvas& c, const Rect& r, bool open) {
    const double u = c.u;
    if (open) {  // raised: only its hinge and frame stay visible above the button
        outline(c, r.inflate(.6 * u, .6 * u), std::max(2.0f, float(.25 * u)), Color{242, 194, 48});
        c.fill_rect(float(r.x - .3 * u), float(r.y - .9 * u), float(r.w + .6 * u), float(.6 * u), Color{242, 194, 48});
        return;
    }
    // Closed: a striped collar around a clear cover, so the button's legend stays readable through it.
    const Rect previous = c.clip();
    const double band = std::min(1.3 * u, std::min(r.w, r.h) * .22);
    const Rect bands[4] = {Rect::f(r.x, r.y, r.w, band), Rect::f(r.x, r.bottom() - band, r.w, band),
                           Rect::f(r.x, r.y, band, r.h), Rect::f(r.right() - band, r.y, band, r.h)};
    const double step = 2.2 * u;
    for (const Rect& b : bands) {
        c.set_clip(previous.w > 0 ? b.clip(previous) : b);
        c.rect(b, Color{24, 24, 24});
        for (double x = r.x - r.h; x < r.right(); x += step)
            c.polygon({{float(x), float(r.bottom())},
                       {float(x + step / 2), float(r.bottom())},
                       {float(x + step / 2 + r.h), float(r.y)},
                       {float(x + r.h), float(r.y)}},
                      Color{242, 194, 48}, false);
    }
    c.set_clip(previous);
    const Rect glass = r.inflate(-2 * band, -2 * band);
    c.fill_rect(float(glass.x), float(glass.y), float(glass.w), float(glass.h), Color{255, 255, 255, 34});
    c.line({float(glass.x + glass.w * .15), float(glass.bottom() - glass.h * .1)},
           {float(glass.x + glass.w * .45), float(glass.y + glass.h * .1)}, std::max(1.5f, float(.25 * u)),
           Color{255, 255, 255, 70});
    outline(c, r, std::max(2.0f, float(.2 * u)), Color{30, 30, 30});
}

void CockpitPainter::veil(Canvas& c, const Rect& r) { c.rounded_rect(r, float(.4 * c.u), Color{20, 22, 24, 150}); }

void CockpitPainter::feedback(Canvas& c, const Rect& r, CockpitInput::Feedback f, double now) {
    if (f == CockpitInput::Feedback::None) return;
    const double u = c.u;
    if (f == CockpitInput::Feedback::Rejected) {
        const bool on = std::fmod(now, .4) < .25;  // blinking amber: the game did not change
        if (on) outline(c, r.inflate(.7 * u, .7 * u), std::max(2.0f, float(.35 * u)), lamp_color(Lamp::Amber));
        return;
    }
    outline(c, r.inflate(.5 * u, .5 * u), std::max(2.0f, float(.22 * u)), mix(ink(), lamp_color(Lamp::White), .5));
}

// ============================================================================================ composition
void paint_cockpit(Canvas& c, bool art, const Snapshot& s, const CockpitView& v, CockpitPainter& p) {
    const CockpitLayout l = cockpit_layout(c);
    const double u = c.u;
    const Cockpit* k = s.cockpit ? &*s.cockpit : nullptr;
    const Navigation* nav = s.nav ? &*s.nav : nullptr;
    const CockpitInput* in = art ? nullptr : v.input;
    const double now = v.now;
    const bool armed = in && in->armed(now);
    auto face = [&](Ctl id) -> const Rect& { return l.face[at(id)]; };

    paint_ribbon(c, art, FlightScene::Cockpit, p);
    if (art) {
        p.frame(c, l.attitude, "ATTITUDE CONTROL");
        p.frame(c, l.staging, "STAGING & ABORT");
        p.frame(c, l.recovery, "RECOVERY");
        p.frame(c, l.systems, "SYSTEMS");
        p.frame(c, l.groups, "ACTION GROUPS");
        p.frame(c, l.strip, "THROTTLE  \xC2\xB7  TIME WARP  \xC2\xB7  VIEW");
        for (const auto& cap : l.captions)
            p.text(c, cap.text, cap.units, cap.ribbon ? p.key_ink() : p.ink(), cap.x, cap.y, cap.anchor);
        for (int i = 0; i < CTL_COUNT; ++i)
            if (l.key[size_t(i)].w > 0) p.keycap(c, l.key[size_t(i)], ctl_key(static_cast<Ctl>(i)));
    }

    // Switches: name lamp lit while on, lever up/down, grey when the vessel has no such part.
    auto switch_cell = [&](Ctl id, std::optional<bool> on, std::optional<bool> fitted) {
        const bool is_fitted = fitted.value_or(true);
        p.lamp(c, l.lamp[at(id)], ctl_legend(id), on.value_or(false) && is_fitted ? Lamp::Green : Lamp::Off, art);
        p.lever(c, l.lever[at(id)], on, is_fitted, art);
        if (!art && !is_fitted) {
            p.veil(c, face(id));
            p.text(c, "NOT FITTED", 1.3f, p.veil_ink(), face(id).centerx(), l.lever[at(id)].centery(), Anchor::Center,
                   face(id).w - .6 * u);
        }
    };
    switch_cell(Ctl::Sas, nav ? std::optional<bool>(nav->sas) : std::nullopt, true);
    switch_cell(Ctl::Rcs, nav ? std::optional<bool>(nav->rcs) : std::nullopt, true);
    for (Ctl id : {Ctl::Wheels, Ctl::Gear, Ctl::Lights, Ctl::Brakes, Ctl::Solar, Ctl::Antennas, Ctl::Radiators,
                   Ctl::CargoBays, Ctl::Intakes}) {
        const SwitchState st = k ? k->at(*switch_of(id)) : SwitchState{};
        switch_cell(id, st.on, st.fitted);
    }

    // SAS modes: icon and legend; lit green on the active mode (white when SAS is off but that mode is set).
    for (int i = 0; i < 10; ++i) {
        const Ctl id = ctl_at(Ctl::Mode0, i);
        const Rect& r = face(id);
        const bool selected = nav && nav->sas_mode == SAS_MODES_HMI[i].name;
        const Lamp lit = selected ? (nav->sas ? Lamp::Green : Lamp::White) : Lamp::Off;
        p.lamp(c, r, "", lit, art);
        if (art || lit != Lamp::Off) {
            const Color ink = p.face_ink(lit);
            sas_icon(c, i, r.centerx(), r.y + r.h * .38, std::min(r.w, r.h) * .22, ink);
            p.text(c, SAS_MODES_HMI[i].legend, 1.4f, ink, r.centerx(), r.bottom() - r.h * .2, Anchor::Center, r.w - .6 * u);
        }
        if (!art && sas_mode_unavailable(i, s)) p.veil(c, r);
    }
    // Speed reference
    static const char* const speed_modes[3] = {"orbit", "surface", "target"};
    for (int i = 0; i < 3; ++i) {
        const Ctl id = ctl_at(Ctl::RefOrbit, i);
        p.lamp(c, face(id), ctl_legend(id), nav && nav->speed_mode == speed_modes[i] ? Lamp::Green : Lamp::Off, art);
        if (!art && i == 2 && nav && !nav->target) p.veil(c, face(id));
    }
    p.readout(c, l.speed_readout, "88888.8",
              nav && nav->speed && std::isfinite(*nav->speed) ? fmt::str("%.1f", std::min(99999.9, std::fabs(*nav->speed))) : "",
              art);

    // Staging: current stage, what the next one fires, ARM -> STAGE, guarded ABORT.
    p.readout(c, l.stage_readout, "88",
              s.systems && s.systems->current_stage ? std::to_string(std::clamp(*s.systems->current_stage, 0, 99)) : "", art);
    if (!art) {
        const Rect& n = l.next_list;
        const double row = n.h / 3;
        std::vector<std::string> lines;
        if (!k || !k->next_known) lines.push_back("--");
        else if (k->next_stage.empty()) lines.push_back("NOTHING TO ACTIVATE");
        else
            for (size_t i = 0; i < k->next_stage.size(); ++i) {
                if (i == 2 && k->next_stage.size() > 3) {
                    lines.push_back(fmt::str("+ %d MORE", int(k->next_stage.size() - 2)));
                    break;
                }
                const auto& [title, count] = k->next_stage[i];
                lines.push_back(count > 1 ? fmt::str("%dx ", count) + title : title);
            }
        for (size_t i = 0; i < lines.size() && i < 3; ++i)
            p.text(c, lines[i], 1.5f, i == 0 ? p.ink() : p.dim_ink(), n.x, n.y + row * (i + .5), Anchor::MidLeft, n.w);
    }
    {
        const Rect& a = face(Ctl::Arm);
        const Rect plate = Rect::f(a.x + a.w * .18, a.y + a.h * .26, a.w * .64, a.h * .48);
        if (art) p.guard(c, a, false);
        p.lamp(c, plate, "ARM", armed ? Lamp::Amber : Lamp::Off, art);
    }
    if (!art) {
        p.text(c, armed ? fmt::str("ARMED  %.0f S", std::ceil(in->armed_left(now))) : "SAFE", 1.5f,
               armed ? p.ink() : p.dim_ink(), l.arm_lamp.centerx(), l.arm_lamp.centery(),
               Anchor::Center, l.arm_lamp.w);
    }
    const bool locked = k && k->stage_lock.value_or(false);
    p.lamp(c, face(Ctl::Stage), "STAGE", armed && !locked ? Lamp::White : Lamp::Off, art);
    if (!art && locked) {
        p.veil(c, face(Ctl::Stage));
        p.text(c, "STAGE LOCK", 1.5f, p.lamp_color(Lamp::Amber), face(Ctl::Stage).centerx(),
               face(Ctl::Stage).bottom() - 1.6 * u, Anchor::Center, face(Ctl::Stage).w);
    }
    p.lamp(c, face(Ctl::Abort), "ABORT", k && k->abort.value_or(false) ? Lamp::Red : Lamp::Off, art);
    if (!art) p.guard(c, face(Ctl::Abort).inflate(-.5 * u, -.5 * u), in && in->cover_open(Ctl::Abort, now));

    // Recovery: deploy, the parachutes' states and KSP's own deployment safety.
    const bool chutes_known = k && k->chutes.has_value();
    const bool no_chutes = chutes_known && *k->chutes == 0;
    const unsigned states = k ? k->chute_states : 0;
    const bool open = states & ((1u << CHUTE_SEMI_DEPLOYED) | (1u << CHUTE_DEPLOYED));
    p.lamp(c, face(Ctl::Deploy), "DEPLOY", open ? Lamp::Green : Lamp::Off, art);
    if (!art && no_chutes) p.veil(c, face(Ctl::Deploy));
    static const char* const chute_legends[4] = {"STOWED", "ARMED", "OPEN", "CUT"};
    const bool chute_on[4] = {bool(states & (1u << CHUTE_STOWED)), bool(states & (1u << CHUTE_ARMED)), open,
                              bool(states & (1u << CHUTE_CUT))};
    const Lamp chute_lit[4] = {Lamp::White, Lamp::Amber, Lamp::Green, Lamp::Red};
    for (int i = 0; i < 4; ++i)
        p.lamp(c, l.chute_lamps[size_t(i)], chute_legends[i], chute_on[i] ? chute_lit[i] : Lamp::Off, art);
    p.lamp(c, face(Ctl::Cut), "CUT", Lamp::Off, art);
    if (!art) p.guard(c, face(Ctl::Cut).inflate(-.5 * u, -.5 * u), in && in->cover_open(Ctl::Cut, now));
    static const char* const safety[3] = {"SAFE", "RISKY", "UNSAFE"};
    const Lamp safety_lit[3] = {Lamp::Green, Lamp::Amber, Lamp::Red};
    for (int i = 0; i < 3; ++i)
        p.lamp(c, l.safety_lamps[size_t(i)], safety[i],
               k && k->chute_safety == i ? safety_lit[i] : Lamp::Off, art);
    if (!art) {
        const std::string count = !chutes_known ? "PARACHUTES: NO DATA"
                                  : no_chutes   ? "NO PARACHUTES FITTED"
                                                : fmt::str("%d PARACHUTE%s", *k->chutes, *k->chutes == 1 ? "" : "S");
        const Rect& last = l.safety_lamps[0];
        p.text(c, count, 1.4f, p.dim_ink(), last.x, last.bottom() + 1.4 * u, Anchor::MidLeft, l.recovery.w - 2.4 * u);
    }

    // Action groups
    for (int i = 0; i < 10; ++i) {
        const Ctl id = ctl_at(Ctl::Group1, i);
        p.lamp(c, face(id), ctl_legend(id), k && k->groups[size_t(i)].value_or(false) ? Lamp::Green : Lamp::Off, art);
    }

    // Throttle, time warp, view
    const double throttle = nav ? nav->throttle : 0;
    p.bar(c, l.throttle_bar, throttle, art);
    p.readout(c, l.throttle_readout, "888", nav ? std::to_string(int(std::lround(throttle * 100))) : "", art);
    const std::optional<double> warp = k ? k->warp_rate : std::nullopt;
    p.readout(c, l.warp_readout, "888888", warp ? fmt::str("%.0f", std::min(999999.0, *warp)) : "", art);
    for (Ctl id : {Ctl::ThrottleFull, Ctl::ThrottleCut, Ctl::WarpDown, Ctl::WarpUp, Ctl::Map, Ctl::Quicksave})
        p.lamp(c, face(id), ctl_legend(id), Lamp::Off, art);
    p.lamp(c, face(Ctl::WarpStop), ctl_legend(Ctl::WarpStop), warp && *warp <= 1.0001 ? Lamp::Green : Lamp::Off, art);
    p.lamp(c, face(Ctl::Quickload), ctl_legend(Ctl::Quickload), Lamp::Off, art);
    if (!art) p.guard(c, face(Ctl::Quickload).inflate(-.4 * u, -.4 * u), in && in->cover_open(Ctl::Quickload, now));

    // Click feedback on top of everything
    if (in)
        for (int i = 0; i < CTL_COUNT; ++i) {
            const Ctl id = static_cast<Ctl>(i);
            const Rect& r = id == Ctl::Arm ? face(Ctl::Arm) : l.face[size_t(i)];
            p.feedback(c, r, in->feedback(id, now), now);
        }
}

// ============================================================================================ classic
Color ClassicCockpitPainter::ink() const { return theme::VALUE; }
Color ClassicCockpitPainter::dim_ink() const { return theme::LABEL; }

void ClassicCockpitPainter::frame(Canvas& c, const Rect& r, const std::string& title) {
    theme::panel(c, r);
    text(c, title, 2.0f, theme::LABEL, r.x + 1.6 * c.u, r.y + 1.0 * c.u, Anchor::TopLeft, r.w - 3.2 * c.u);
}

void ClassicCockpitPainter::lamp(Canvas& c, const Rect& r, const std::string& legend, Lamp lit, bool art) {
    const double u = c.u;
    const float rad = float(std::min(.9 * u, r.h * .3));
    if (art) {
        c.rounded_rect(r, rad, Color{38, 50, 68});
        text(c, legend, std::min(1.7f, float(r.h / u * .5)), Color{196, 208, 222}, r.centerx(), r.centery(),
             Anchor::Center, r.w - .8 * u);
        return;
    }
    if (lit == Lamp::Off) return;
    c.rounded_rect(r, rad, lamp_color(lit));
    text(c, legend, std::min(1.7f, float(r.h / u * .5)), lit_ink(), r.centerx(), r.centery(), Anchor::Center,
         r.w - .8 * u);
}

}  // namespace ksphmi
