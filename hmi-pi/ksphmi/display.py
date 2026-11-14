"""Drawing. Everything is laid out in units of the screen height, so the same code fits the Pi monitor
(2520x1680, 3:2), a 1920x1080 TV or a development window on the PC.

Two outputs: ``sdl`` (a window, or SDL full screen) and ``fbdev``, used on the Pi: frames are drawn in memory and
copied to the Linux framebuffer (``/dev/fb0``). fbdev needs no EGL, no DRM master and no desktop, and when the
HMI stops the text console shows again. (SDL's kmsdrm driver set the mode but left the Pi 4 screen black.)"""
from __future__ import annotations

import logging
import math
import os
import time

import pygame
import pygame.gfxdraw

from . import fmt, gauges
from .netinfo import NetInfo
from .scenes import VIEWS
from .navball import Navball
from .systems import RESOURCE_NAMES, RESOURCE_LABELS
from .telemetry import CONNECTING, FLIGHT, NO_VESSEL, Snapshot, stale

BG = (10, 14, 20)
PANEL = (19, 26, 36)
GRID = (34, 45, 60)
MUTED = (88, 104, 128)
LABEL = (122, 140, 164)
VALUE = (232, 238, 245)
ACCENT = (70, 200, 255)
GOOD = (96, 222, 140)
WARN = (255, 186, 72)
BAD = (255, 92, 92)
BODY = (46, 96, 150)
ATMOSPHERE = (24, 48, 78)


FBIOBLANK = 0x4611        # linux/fb.h
FB_BLANK_UNBLANK = 0


class Framebuffer:
    """A Linux fbdev device: geometry from sysfs, 16 bpp (RGB565) or 32 bpp (XRGB8888).

    The device can be left *blanked* (powered down, ``/sys/class/graphics/fb0/blank`` = 4: it happened after
    an SDL kmsdrm run), and then every frame written is invisible. So it is unblanked at start and every
    ``UNBLANK_EVERY`` seconds; the FBIOBLANK ioctl needs only write access to the device, not root."""

    UNBLANK_EVERY = 30.0

    def __init__(self, device: str) -> None:
        sysfs = "/sys/class/graphics/" + os.path.basename(device)

        def attr(name: str) -> str:
            with open(os.path.join(sysfs, name)) as f:
                return f.read().strip()

        self.size = tuple(int(v) for v in attr("virtual_size").split(","))
        bpp, self.stride = int(attr("bits_per_pixel")), int(attr("stride"))
        masks = {16: (0xF800, 0x07E0, 0x001F, 0), 32: (0xFF0000, 0x00FF00, 0x0000FF, 0)}.get(bpp)
        if masks is None:
            raise RuntimeError("%s: unsupported %d bits per pixel" % (device, bpp))
        self.converted = pygame.Surface(self.size, 0, bpp, masks)
        self.fb = open(device, "r+b", buffering=0)
        self._unblanked_at = -1e9
        self.unblank()

    def unblank(self) -> None:
        import fcntl
        try:
            fcntl.ioctl(self.fb.fileno(), FBIOBLANK, FB_BLANK_UNBLANK)
        except OSError as e:  # keep drawing; the log says why the screen may stay dark
            logging.getLogger(__name__).warning("cannot unblank the framebuffer: %s", e)
        self._unblanked_at = time.monotonic()

    def write(self, frame: pygame.Surface) -> None:
        if time.monotonic() - self._unblanked_at > self.UNBLANK_EVERY:
            self.unblank()
        self.converted.blit(frame, (0, 0))
        data = self.converted.get_buffer().raw
        pitch = self.converted.get_pitch()
        self.fb.seek(0)
        if pitch == self.stride:
            self.fb.write(data)
        else:  # padded lines in the device
            row = self.size[0] * self.converted.get_bytesize()
            for y in range(self.size[1]):
                self.fb.seek(y * self.stride)
                self.fb.write(data[y * pitch:y * pitch + row])


class Display:
    def __init__(self, cfg: dict, windowed: bool = False, size: tuple[int, int] | None = None) -> None:
        dcfg = cfg["display"]
        output = os.environ.get("KSPHMI_OUTPUT") or dcfg.get("output", "sdl")
        self.fb = None
        if output == "fbdev" and not windowed:
            os.environ["SDL_VIDEODRIVER"] = "dummy"  # pygame events and fonts still need an initialised display
        pygame.display.init()
        pygame.font.init()
        self.day_hours = int(cfg["format"]["day_hours"])
        k = cfg["krpc"]
        self.server_text = "kRPC server: %s:%s" % (k["address"], k["rpc_port"])
        self.net = NetInfo(str(k["address"]))
        if output == "fbdev" and not windowed:
            self.fb = Framebuffer(dcfg.get("fbdev", "/dev/fb0"))
            self.screen = pygame.Surface(self.fb.size)
        else:
            fullscreen = dcfg["fullscreen"] and not windowed
            if size is None:
                size = (int(dcfg["width"]), int(dcfg["height"]))
                if not fullscreen and size == (0, 0):
                    size = (1280, 800)
            self.screen = pygame.display.set_mode(size, pygame.FULLSCREEN if fullscreen else 0)
            pygame.display.set_caption("KSP HMI")
            if fullscreen:
                pygame.mouse.set_visible(False)
        self.w, self.h = self.screen.get_size()
        self.u = self.h / 100.0  # layout unit: 1 % of the screen height
        self._fonts: dict[int, pygame.font.Font] = {}
        self._text_cache: dict[tuple, pygame.Surface] = {}
        self._gauge_faces: dict[tuple, pygame.Surface] = {}
        self._gauge_layout: tuple | None = None
        self.navball = Navball()

    # ----------------------------------------------------------------------------------------- helpers
    def font(self, size_units: float) -> pygame.font.Font:
        px = max(8, int(size_units * self.u))
        f = self._fonts.get(px)
        if f is None:
            f = self._fonts[px] = pygame.font.Font(None, px)
        return f

    def text(self, s: str, size_units: float, color, pos, anchor: str = "topleft") -> pygame.Rect:
        key = (s, size_units, color)
        surf = self._text_cache.get(key)
        if surf is None:
            if len(self._text_cache) > 600:  # values change every frame: keep the cache bounded
                self._text_cache.clear()
            surf = self._text_cache[key] = self.font(size_units).render(s, True, color)
        rect = surf.get_rect(**{anchor: (int(pos[0]), int(pos[1]))})
        self.screen.blit(surf, rect)
        return rect

    def panel(self, rect: pygame.Rect) -> None:
        pygame.draw.rect(self.screen, PANEL, rect, border_radius=int(1.2 * self.u))

    # ------------------------------------------------------------------------------------------- frame
    def draw(self, snap: Snapshot) -> None:
        self.screen.fill(BG)
        now = time.monotonic()
        self.header(snap, now)
        if snap.state == FLIGHT:
            self.flight(snap)
        elif snap.state == NO_VESSEL and snap.scene in VIEWS:
            self.facility(snap)
        else:
            self.waiting(snap, now)
        if self.fb:
            self.fb.write(self.screen)
        else:
            pygame.display.flip()

    def header(self, snap: Snapshot, now: float) -> None:
        u = self.u
        top = 1.5 * u
        if snap.state == FLIGHT:
            self.text(snap.vessel or "Unnamed vessel", 5.2, VALUE, (2.5 * u, top))
            self.text("%s  ·  %s" % (snap.body, fmt.situation(snap.situation)), 3.0, LABEL, (2.5 * u, top + 5.4 * u))
            self.text(fmt.clock(snap.ut, self.day_hours), 3.4, LABEL, (self.w - 2.5 * u, top + 5.0 * u), "topright")
        elif snap.state == NO_VESSEL and snap.scene in VIEWS:
            self.text("KSP KONTROLLER", 5.2, VALUE, (2.5 * u, top))
            self.text(VIEWS[snap.scene][1], 3.0, LABEL, (2.5 * u, top + 5.4 * u))
        if stale(snap, now):
            status, color = "NO DATA %ds" % (now - snap.received), WARN
        elif snap.state == FLIGHT:
            status, color = "LIVE", GOOD
        elif snap.state == NO_VESSEL:
            status, color = "CONNECTED", GOOD  # the link is up; the centre of the screen says not in flight
        else:
            status, color = "OFFLINE", BAD
        r = self.text(status, 3.4, color, (self.w - 2.5 * u, top + 0.6 * u), "topright")
        pygame.draw.circle(self.screen, color, (r.left - 1.6 * u, r.centery), 0.9 * u)
        pygame.draw.line(self.screen, GRID, (2.5 * u, 11 * u), (self.w - 2.5 * u, 11 * u), max(1, int(0.2 * u)))

    def waiting(self, snap: Snapshot, now: float) -> None:
        u = self.u
        cx, cy = self.w / 2, self.h * 0.5
        # a small moon circling a planet, so a frozen screen is easy to tell from a waiting one
        pygame.draw.circle(self.screen, BODY, (cx, cy - 12 * u), 6 * u)
        ang = now * 1.2
        pygame.draw.circle(self.screen, GRID, (cx, cy - 12 * u), 11 * u, max(1, int(0.25 * u)))
        pygame.draw.circle(self.screen, ACCENT, (cx + 11 * u * math.cos(ang), cy - 12 * u + 11 * u * math.sin(ang)), 1.2 * u)
        title = "Not in flight" if snap.state == NO_VESSEL else "Waiting for KSP"
        hint = ("Launch or switch to a vessel to see its orbit" if snap.state == NO_VESSEL
                else "Start KSP and the kRPC server (port 50000 on the PC's LAN address)")
        self.text(title, 8, VALUE, (cx, cy + 4 * u), "midtop")
        self.text(hint, 3.4, LABEL, (cx, cy + 13 * u), "midtop")
        if snap.detail:
            self.text(snap.detail[:110], 2.8, MUTED, (cx, cy + 19 * u), "midtop")
        # where to find this display (SSH, deploys) and where it looks for the game
        self.text(self.net.line(), 3.4, VALUE, (cx, self.h - 9.5 * u), "midtop")
        self.text(snap.server or self.server_text, 3.0, LABEL, (cx, self.h - 5 * u), "midtop")

    def facility(self, snap: Snapshot) -> None:
        u = self.u
        title, subtitle, cards = VIEWS[snap.scene]
        self.text(title, 6.5, VALUE, (4 * u, 15 * u))
        self.text("PREPARATION CHECKLIST", 2.8, ACCENT, (4 * u, 24 * u))
        for index, (label, heading, hint) in enumerate(cards):
            y = (31 + index * 17) * u
            self.panel(pygame.Rect(4 * u, y, self.w - 8 * u, 14 * u))
            self.text(label, 2.6, ACCENT, (6 * u, y + 1.5 * u))
            self.text(heading, 4.2, VALUE, (6 * u, y + 4.7 * u))
            self.text(hint, 2.8, LABEL, (6 * u, y + 10 * u))
        self.text("Scene context live  |  Flight telemetry resumes when you launch", 2.6,
                  MUTED, (4 * u, 87 * u))

    # ------------------------------------------------------------------------------------------ flight
    def flight(self, s: Snapshot) -> None:
        u = self.u
        top, bottom, gap = 13 * u, self.h - 2.5 * u, 1.6 * u
        left_w = self.w * 0.44
        x0 = 2.5 * u
        metric_gap = .8 * u
        col_w = (left_w - x0 - metric_gap) / 2

        # Compact Ap / Pe row keeps countdowns in the label line.
        big_h = 7 * u
        pe_color = BAD if s.periapsis < 0 else WARN if s.periapsis < s.atmosphere_depth else ACCENT
        hyperbolic = s.eccentricity >= 1
        self.tile(pygame.Rect(x0, top, col_w, big_h), "APOAPSIS",
                  "Escape" if hyperbolic else fmt.distance(s.apoapsis), GOOD,
                  "" if hyperbolic else "T " + fmt.duration(s.time_to_apoapsis, self.day_hours), big=True)
        pe_note = "below the surface" if s.periapsis < 0 else ("inside the atmosphere" if s.periapsis < s.atmosphere_depth else "")
        self.tile(pygame.Rect(x0 + col_w + metric_gap, top, col_w, big_h), "PERIAPSIS", fmt.distance(s.periapsis), pe_color,
                  "T " + fmt.duration(s.time_to_periapsis, self.day_hours), big=True)

        rows = [
            ("ALTITUDE", fmt.distance(s.altitude), VALUE, "VERTICAL SPEED", fmt.speed(s.vertical_speed),
             GOOD if s.vertical_speed >= 0 else WARN),
            ("ORBITAL SPEED", fmt.speed(s.orbital_speed), VALUE, "SURFACE SPEED", fmt.speed(s.surface_speed), VALUE),
            ("INCLINATION", fmt.degrees(s.inclination), VALUE, "ECCENTRICITY", "%.4f" % s.eccentricity, VALUE),
            ("PERIOD", "--" if hyperbolic else fmt.duration(s.period, self.day_hours), VALUE,
             "SEMI-MAJOR AXIS", fmt.distance(s.semi_major_axis), VALUE),
        ]
        y = top + big_h + metric_gap
        row_h = 7 * u
        for la, va, ca, lb, vb, cb in rows:
            self.tile(pygame.Rect(x0, y, col_w, row_h), la, va, ca)
            self.tile(pygame.Rect(x0 + col_w + metric_gap, y, col_w, row_h), lb, vb, cb)
            y += row_h + metric_gap

        if pe_note:
            self.text("PERIAPSIS: " + pe_note.upper(), 1.8, pe_color, (x0 + u, y - .3 * u))
        self.resources(pygame.Rect(x0, 54 * u, left_w - x0, 25 * u), s)
        self.staging(pygame.Rect(x0, 80.6 * u, left_w - x0, bottom - 80.6 * u), s)

        right_x, right_w = left_w + gap, self.w - left_w - gap - 2.5 * u
        orbit_h = 22 * u
        self.orbit(pygame.Rect(right_x, top, right_w, orbit_h), s)
        self.navball.draw(self, pygame.Rect(right_x, top + orbit_h + gap, right_w,
                                           bottom - top - orbit_h - gap), s)

    def tile(self, rect: pygame.Rect, label: str, value: str, color, note: str = "", big: bool = False) -> None:
        u = self.u
        self.panel(rect)
        pad = 1 * u
        self.text(label, 1.9, LABEL, (rect.x + pad, rect.y + .7 * u))
        self.text(value, 4.0, color, (rect.x + pad, rect.y + 2.9 * u))
        if note:
            self.text(note, 1.6, LABEL, (rect.right - pad, rect.y + .7 * u), "topright")

    # Text sizes (units) below each dial: label, amount and the gap between them.
    GAUGE_LABEL, GAUGE_AMOUNT, GAUGE_GAP = 1.9, 1.8, .3

    def resources(self, rect: pygame.Rect, snap: Snapshot) -> None:
        u = self.u
        self.panel(rect)
        self.text("RESOURCES", 2.4, LABEL, (rect.x + 1.3 * u, rect.y + u))
        self.text("VESSEL TOTALS · KSP UNITS", 1.7, MUTED, (rect.right - 1.3 * u, rect.y + u), 'topright')
        if snap.systems is None:
            self.text("Resource data unavailable", 2.4, MUTED, rect.center, 'center')
            return
        by_name = {r.name: r for r in snap.systems.resources}
        visible = [name for name in RESOURCE_NAMES if name in by_name and
                   (name in RESOURCE_NAMES[:4] or (by_name[name].capacity or 0) > 0)]
        if not visible:
            return
        radius, cells, label_units = self.gauge_layout(rect, tuple(visible))
        for name, (cx, cy, cell_x, cell_w) in zip(visible, cells):
            self.gauge(by_name[name], RESOURCE_LABELS[RESOURCE_NAMES.index(name)], cx, cy, radius,
                       cell_w - 2 * u, label_units)

    def gauge_layout(self, rect: pygame.Rect, names: tuple) -> tuple:
        """Dial radius, centres and a common label size, recomputed only when the panel or the set changes."""
        key = (tuple(rect), names)
        if self._gauge_layout is None or self._gauge_layout[0] != key:
            u = self.u
            text_h = (self.GAUGE_LABEL + self.GAUGE_AMOUNT + self.GAUGE_GAP) * u
            radius, cells = gauges.gauge_layout(rect.x + .6 * u, rect.y + 4.2 * u, rect.w - 1.2 * u,
                                                rect.h - 4.6 * u, len(names), text_h, .4 * u)
            room = min(cell_w for _, _, _, cell_w in cells) - 2 * u
            widest = max(self.font(self.GAUGE_LABEL).size(RESOURCE_LABELS[RESOURCE_NAMES.index(n)])[0]
                         for n in names)
            label_units = self.GAUGE_LABEL * min(1.0, room / max(widest, 1))
            self._gauge_layout = (key, (radius, cells, math.floor(label_units * 20) / 20))
        return self._gauge_layout[1]

    def gauge_face(self, radius: int, live: bool) -> pygame.Surface:
        """The static part of a dial (bezel, track, warning zones, ticks), drawn once per size at twice the
        resolution and smoothed down, so the edges are antialiased without per-frame cost."""
        key = (radius, live)
        face = self._gauge_faces.get(key)
        if face is not None:
            return face
        if len(self._gauge_faces) > 16:
            self._gauge_faces.clear()
        side = 2 * radius + 4
        big = pygame.Surface((2 * side, 2 * side))
        big.fill(PANEL)
        c, r = side, 2 * radius
        pygame.draw.circle(big, (30, 40, 54) if live else (25, 33, 45), (c, c), r)
        pygame.draw.circle(big, (11, 16, 23), (c, c), round(.93 * r))
        pygame.draw.polygon(big, (27, 36, 50), gauges.sector(c, c, .86 * r, .70 * r, 0, 1, 2))
        if live:
            for f0, f1, color in ((0, gauges.BAD_BELOW, BAD), (gauges.BAD_BELOW, gauges.WARN_BELOW, WARN)):
                dim = tuple(round(.55 * a + .45 * b) for a, b in zip(color, (11, 16, 23)))
                pygame.draw.polygon(big, dim, gauges.sector(c, c, .91 * r, .87 * r, f0, f1, 2))
        for step in range(21):
            f = step / 20
            major, ten = step % 10 == 0, step % 2 == 0
            inner = .55 if major else .60 if ten else .64
            color = (VALUE if major else LABEL if ten else MUTED) if live else GRID
            pygame.draw.line(big, color, gauges.point(c, c, inner * r, f), gauges.point(c, c, .68 * r, f),
                             max(2, round((.045 if major else .03) * r)))
        face = self._gauge_faces[key] = pygame.transform.smoothscale(big, (side, side))
        return face

    def gauge(self, resource, label: str, cx: float, cy: float, radius: float, room: float,
              label_units: float) -> None:
        u = self.u
        fraction = resource.fraction
        r = max(4, int(radius))
        face = self.gauge_face(r, fraction is not None)
        self.screen.blit(face, face.get_rect(center=(int(cx), int(cy))))
        if fraction is not None:
            color = BAD if fraction <= gauges.BAD_BELOW else WARN if fraction <= gauges.WARN_BELOW else GOOD
            if fraction > 0:
                pts = gauges.sector(cx, cy, .86 * r, .70 * r, 0, fraction)
                pygame.draw.polygon(self.screen, color, pts)
                pygame.gfxdraw.aapolygon(self.screen, [(round(x), round(y)) for x, y in pts], color)
            # pointer across the track at the current level
            a = gauges.gauge_angle(fraction)
            dx, dy = math.cos(a), -math.sin(a)
            nx, ny = -dy * .035 * r, dx * .035 * r
            p0, p1 = (cx + dx * .62 * r, cy + dy * .62 * r), (cx + dx * .93 * r, cy + dy * .93 * r)
            needle = [(p0[0] + nx, p0[1] + ny), (p1[0] + nx, p1[1] + ny), (p1[0] - nx, p1[1] - ny),
                      (p0[0] - nx, p0[1] - ny)]
            pygame.draw.polygon(self.screen, VALUE, needle)
            pygame.gfxdraw.aapolygon(self.screen, [(round(x), round(y)) for x, y in needle], VALUE)
            centre = "%d%%" % round(100 * fraction)
            amount = "%s / %s" % (_amount(resource.amount), _amount(resource.capacity))
            reference = "%s / %s" % (_amount(resource.capacity), _amount(resource.capacity))
        else:
            color = MUTED
            centre = 'N/A' if resource.capacity == 0 else '--'
            amount = reference = 'NOT INSTALLED' if resource.capacity == 0 else '--'
        self.text(centre, .44 * r / u, color if fraction is not None else MUTED, (cx, cy), 'center')
        top = cy + gauges.BELOW_CENTRE * r
        self.text(label, label_units, LABEL if fraction is not None else MUTED, (cx, top), 'midtop')
        width = self.font(self.GAUGE_AMOUNT).size(reference)[0]
        amount_units = math.floor(self.GAUGE_AMOUNT * min(1.0, room / max(width, 1)) * 20) / 20
        self.text(amount, amount_units, color, (cx, top + (label_units + self.GAUGE_GAP) * u), 'midtop')

    def staging(self, rect: pygame.Rect, snap: Snapshot) -> None:
        u = self.u
        self.panel(rect)
        self.text("STAGING", 2.4, LABEL, (rect.x + 1.3 * u, rect.y + u))
        data = snap.systems
        current = str(data.current_stage) if data is not None and data.current_stage is not None else '--'
        self.text("CURRENT " + current, 2.4, ACCENT, (rect.right - 1.3 * u, rect.y + u), 'topright')
        if data is None or not data.stages:
            self.text("Stage data unavailable" if data is None else "No pending activation stages", 2.2, MUTED,
                      (rect.centerx, rect.y + 8 * u), 'center')
            return
        for index, stage in enumerate(data.stages[:4]):
            y = rect.y + (5 + index * 2.7) * u
            active = stage.number == data.current_stage
            color = ACCENT if active else VALUE
            self.text("%02d  %s" % (stage.number, 'CURRENT' if active else 'PENDING'), 2.2,
                      color, (rect.x + 1.3 * u, y))
            self.text('%s parts' % (stage.parts if stage.parts is not None else '--'), 2.0, LABEL,
                      (rect.centerx, y))
            self.text(('DV ' + fmt.speed(stage.delta_v)) if stage.delta_v is not None else 'DV --', 2.2, color,
                      (rect.right - 1.3 * u, y), 'topright')

    def orbit(self, rect: pygame.Rect, s: Snapshot) -> None:
        """Side view of the orbit in its own plane, periapsis to the right, body centre at the focus."""
        u = self.u
        self.panel(rect)
        self.text("ORBIT", 2.6, LABEL, (rect.x + 1.8 * u, rect.y + 1.8 * u))
        if s.orbit_state is not None:
            self.text("INERTIAL", 1.8, MUTED, (rect.right - 1.8 * u, rect.y + 1.8 * u), 'topright')
        # Escape trajectories are unbounded. Contain every drawing primitive,
        # including offscreen vessel markers, without covering the panel title.
        previous_clip = self.screen.get_clip()
        plot_bounds = pygame.Rect(rect.x + 1.2 * u, rect.y + 5 * u,
                                  rect.w - 2.4 * u, rect.h - 6.2 * u)
        self.screen.set_clip(previous_clip.clip(plot_bounds))
        try:
            self._orbit_contents(rect, s)
        finally:
            self.screen.set_clip(previous_clip)

    def _orbit_contents(self, rect: pygame.Rect, s: Snapshot) -> None:
        if s.orbit_state is not None:
            self._inertial_orbit(rect, s)
            return
        u = self.u
        # Reserve room for the labels outside both apsis markers. When width limits
        # the orbit scale, a fixed margin can leave Ap or Pe outside the panel.
        label_width = max(self.font(2.8).size(label)[0] for label in ("Ap", "Pe"))
        side_margin = label_width + 2.6 * u  # marker-to-label gap + panel padding
        area = rect.inflate(-2 * side_margin, -10 * u).move(0, 2 * u)

        radius = max(s.body_radius, 1.0)
        atmo = radius + s.atmosphere_depth
        e, a = s.eccentricity, s.semi_major_axis
        rp = a * (1 - e)
        elliptic = e < 1 and a > 0
        if elliptic:
            ra = a * (1 + e)
            b = a * math.sqrt(1 - e * e)
            xmin, xmax, ymax = min(-ra, -atmo), max(rp, atmo), max(b, atmo)
        else:  # hyperbolic: show the neighbourhood of the body only
            view = max(4 * atmo, 2.5 * abs(rp))
            xmin, xmax, ymax = -view, view, view
        scale = min(area.w / (xmax - xmin), area.h / (2 * ymax))
        fx = area.centerx - (xmin + xmax) / 2 * scale  # focus (body centre) on screen
        fy = area.centery

        def to_screen(r: float, nu: float) -> tuple[float, float]:
            return fx + r * math.cos(nu) * scale, fy - r * math.sin(nu) * scale

        if s.atmosphere_depth > 0:
            pygame.draw.circle(self.screen, ATMOSPHERE, (fx, fy), max(3, atmo * scale))
        p = a * (1 - e * e)
        points = []
        limit = 3 * max(xmax - xmin, 2 * ymax)
        for i in range(241):
            nu = -math.pi + 2 * math.pi * i / 240
            denom = 1 + e * math.cos(nu)
            if denom <= 1e-3:
                continue
            r = p / denom
            if 0 < r < limit:
                points.append(to_screen(r, nu))
        if len(points) > 1:
            pygame.draw.aalines(self.screen, ACCENT, elliptic, points)
        pygame.draw.circle(self.screen, BODY, (fx, fy), max(2, radius * scale))
        self.text(s.body, 2.8, VALUE, (fx, fy), "center")

        marker = 0.8 * u
        if elliptic:
            apx, apy = to_screen(a * (1 + e), math.pi)
            pygame.draw.circle(self.screen, GOOD, (apx, apy), marker)
            self.text("Ap", 2.8, GOOD, (apx - 1.4 * u, apy), "midright")
        pex, pey = to_screen(rp, 0.0)
        pygame.draw.circle(self.screen, WARN if s.periapsis < s.atmosphere_depth else ACCENT, (pex, pey), marker)
        self.text("Pe", 2.8, VALUE, (pex + 1.4 * u, pey), "midleft")

        r_now = p / (1 + e * math.cos(s.true_anomaly)) if 1 + e * math.cos(s.true_anomaly) > 1e-3 else 0
        if r_now > 0:
            vx, vy = to_screen(r_now, s.true_anomaly)
            pygame.draw.circle(self.screen, VALUE, (vx, vy), 1.3 * u)
            pygame.draw.circle(self.screen, BG, (vx, vy), 0.6 * u)

    def _inertial_orbit(self, rect: pygame.Rect, s: Snapshot) -> None:
        """Keep the body and inertial view axes fixed; plot the measured position directly."""
        u, state = self.u, s.orbit_state
        if not all(math.isfinite(v) for v in (*state.position, *state.velocity)):
            return
        conic = state.conic()
        radius = max(s.body_radius, 1)
        atmo = radius + s.atmosphere_depth
        if conic is None:
            reach = max(4 * atmo, math.sqrt(sum(v*v for v in state.position)))
        elif conic.apoapsis is not None:
            reach = max(atmo, conic.apoapsis)
        else:
            reach = max(4 * atmo, 2.5 * conic.periapsis)
        margin = max(self.font(2.8).size(label)[0] for label in ('Ap', 'Pe')) + 2.6 * u
        area = rect.inflate(-2 * margin, -10 * u).move(0, 2 * u)
        scale = min(area.w, area.h) / (2 * reach)
        cx, cy = area.center

        def point(vector):
            x, y = state.project(vector)
            return cx + x * scale, cy - y * scale

        if s.atmosphere_depth > 0:
            pygame.draw.circle(self.screen, ATMOSPHERE, (cx, cy), max(3, atmo * scale))
        if conic is not None:
            segment = []
            for index in range(241):
                vector = conic.position(-math.pi + 2 * math.pi * index / 240)
                if vector is not None and sum(v*v for v in vector) <= (12 * reach)**2:
                    segment.append(point(vector))
                else:
                    if len(segment) > 1:
                        pygame.draw.aalines(self.screen, ACCENT, False, segment)
                    segment = []
            if len(segment) > 1:
                pygame.draw.aalines(self.screen, ACCENT, False, segment)
        pygame.draw.circle(self.screen, BODY, (cx, cy), max(2, radius * scale))
        self.text(s.body, 2.8, VALUE, (cx, cy), 'center')
        # At zero eccentricity the apsides have no unique direction; do not invent one.
        if conic is not None and conic.eccentricity > 1e-5:
            if conic.apoapsis is not None:
                ap = point(conic.position(math.pi))
                pygame.draw.circle(self.screen, GOOD, ap, .8 * u)
                self.text('Ap', 2.8, GOOD, (ap[0] - 1.4 * u, ap[1]), 'midright')
            pe = point(conic.position(0))
            pygame.draw.circle(self.screen, WARN if s.periapsis < s.atmosphere_depth else ACCENT, pe, .8 * u)
            self.text('Pe', 2.8, VALUE, (pe[0] + 1.4 * u, pe[1]), 'midleft')
        # This is the white ring the user follows during docking: it does not use
        # true anomaly, attitude, target velocity, or the changing periapsis direction.
        position = point(state.position)
        pygame.draw.circle(self.screen, VALUE, position, 1.3 * u)
        pygame.draw.circle(self.screen, BG, position, .6 * u)


def _amount(value: float) -> str:
    """Compact KSP units for the dial captions: whole units from 100 up, one decimal below."""
    return "%.0f" % value if abs(value) >= 100 else "%.1f" % value
