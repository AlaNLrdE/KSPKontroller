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

from . import fmt
from .netinfo import NetInfo
from .scenes import VIEWS
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
        self.text(self.server_text, 3.0, LABEL, (cx, self.h - 5 * u), "midtop")

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
        left_w = self.w * 0.52
        x0 = 2.5 * u
        col_w = (left_w - x0 - gap) / 2

        # Ap / Pe: the two numbers that matter most, large
        big_h = 22 * u
        pe_color = BAD if s.periapsis < 0 else WARN if s.periapsis < s.atmosphere_depth else ACCENT
        hyperbolic = s.eccentricity >= 1
        self.tile(pygame.Rect(x0, top, col_w, big_h), "APOAPSIS",
                  "Escape" if hyperbolic else fmt.distance(s.apoapsis), GOOD,
                  "" if hyperbolic else "in " + fmt.duration(s.time_to_apoapsis, self.day_hours), big=True)
        pe_note = "below the surface" if s.periapsis < 0 else ("inside the atmosphere" if s.periapsis < s.atmosphere_depth else "")
        self.tile(pygame.Rect(x0 + col_w + gap, top, col_w, big_h), "PERIAPSIS", fmt.distance(s.periapsis), pe_color,
                  "in " + fmt.duration(s.time_to_periapsis, self.day_hours) + ("  ·  " + pe_note if pe_note else ""), big=True)

        rows = [
            ("ALTITUDE", fmt.distance(s.altitude), VALUE, "VERTICAL SPEED", fmt.speed(s.vertical_speed),
             GOOD if s.vertical_speed >= 0 else WARN),
            ("ORBITAL SPEED", fmt.speed(s.orbital_speed), VALUE, "SURFACE SPEED", fmt.speed(s.surface_speed), VALUE),
            ("INCLINATION", fmt.degrees(s.inclination), VALUE, "ECCENTRICITY", "%.4f" % s.eccentricity, VALUE),
            ("PERIOD", "--" if hyperbolic else fmt.duration(s.period, self.day_hours), VALUE,
             "SEMI-MAJOR AXIS", fmt.distance(s.semi_major_axis), VALUE),
        ]
        y = top + big_h + gap
        row_h = (bottom - y - gap * (len(rows) - 1)) / len(rows)
        for la, va, ca, lb, vb, cb in rows:
            self.tile(pygame.Rect(x0, y, col_w, row_h), la, va, ca)
            self.tile(pygame.Rect(x0 + col_w + gap, y, col_w, row_h), lb, vb, cb)
            y += row_h + gap

        self.orbit(pygame.Rect(left_w + gap, top, self.w - left_w - gap - 2.5 * u, bottom - top), s)

    def tile(self, rect: pygame.Rect, label: str, value: str, color, note: str = "", big: bool = False) -> None:
        u = self.u
        self.panel(rect)
        pad = 1.8 * u
        self.text(label, 2.6, LABEL, (rect.x + pad, rect.y + pad))
        vsize = 9.5 if big else min(6.5, rect.h / u * 0.48)
        self.text(value, vsize, color, (rect.x + pad, rect.y + pad + 3.2 * u))
        if note:
            self.text(note, 3.0, LABEL, (rect.x + pad, rect.bottom - pad), "bottomleft")

    def orbit(self, rect: pygame.Rect, s: Snapshot) -> None:
        """Side view of the orbit in its own plane, periapsis to the right, body centre at the focus."""
        u = self.u
        self.panel(rect)
        self.text("ORBIT", 2.6, LABEL, (rect.x + 1.8 * u, rect.y + 1.8 * u))
        area = rect.inflate(-6 * u, -10 * u).move(0, 2 * u)

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
