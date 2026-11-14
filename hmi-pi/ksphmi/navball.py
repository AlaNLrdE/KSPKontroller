"""Stock-inspired navball, drawn with pygame primitives and a quaternion-rotated sphere."""
import math
import pygame
from .navigation import basis, unit
from . import fmt

SKY, GROUND = (37, 133, 173), (157, 91, 35)
INK, GOLD, WHITE = (215, 226, 221), (255, 194, 39), (235, 242, 245)
GREEN, MAGENTA, CYAN = (180, 245, 38), (240, 83, 213), (65, 225, 234)


def globe(pitch, heading):
    p, h = math.radians(pitch), math.radians(heading)
    return math.sin(p), math.cos(p) * math.cos(h), math.cos(p) * math.sin(h)


def projected(vector, axes):
    # This runs thousands of times per frame on the Pi: avoid generator/zip allocations.
    x, y, z = vector
    right, forward, up = axes
    return (x * right[0] + y * right[1] + z * right[2],
            -(x * up[0] + y * up[1] + z * up[2]),
            x * forward[0] + y * forward[1] + z * forward[2])


def label_projection(pitch, heading, axes):
    """Continuous label projection: no collision-dependent visibility decisions."""
    v = projected(globe(pitch, heading), axes)
    if v[2] <= 0:
        return None
    north = projected(globe(pitch + .5, heading), axes)
    east = projected(globe(pitch, heading + .5), axes)
    step = math.radians(.5)
    width = max(.08, math.hypot(east[0] - v[0], east[1] - v[1]) / step)
    height = max(.08, math.hypot(north[0] - v[0], north[1] - v[1]) / step)
    angle = math.degrees(math.atan2(north[0] - v[0], -(north[1] - v[1])))
    edge = min(1, v[2] / .22)
    alpha = round(255 * edge * edge * (3 - 2 * edge))
    return v, width, height, angle, alpha


def front_polygon(points):
    """Clip a globe cell against the visible hemisphere before projecting it."""
    out = []
    previous = points[-1]
    for current in points:
        if (current[2] >= 0) != (previous[2] >= 0):
            t = previous[2] / (previous[2] - current[2])
            out.append(tuple(a + t * (b - a) for a, b in zip(previous, current)))
        if current[2] >= 0:
            out.append(current)
        previous = current
    return out


class Navball:
    MARKER_SIZE = 2.2  # screen-height units; twice the original marker radius

    def __init__(self):
        self.bold_fonts = {}
        self.label_images = {}
        self.lighting = None
        self.lighting_size = None
        self.cells = []
        for pitch in range(-90, 90, 10):
            for heading in range(0, 360, 10):
                vertices = [globe(p, h) for p, h in ((pitch, heading), (pitch + 10, heading),
                                                   (pitch + 10, heading + 10), (pitch, heading + 10))]
                self.cells.append((vertices, SKY if pitch >= 0 else GROUND))
        self.lines = []
        self.lines.append(([globe(0, h) for h in range(0, 361, 3)], 2))
        # Stock-style broken parallels distinguish the +/-45 degree bands
        # from the local five-degree ladder ticks. Gaps fall on meridians,
        # leaving space for the bold compass bearings printed on these bands.
        for pitch in (-45, 45):
            for heading in range(0, 360, 45):
                self.lines.append(([globe(pitch, heading + offset) for offset in range(5, 41, 2)], 1))
        # Local pitch ladders: long numbered ten-degree ticks and short five-degree ticks.
        for pitch in range(-85, 86, 5):
            if pitch == 0:
                continue
            half_width = 4.5 if pitch % 10 == 0 else 2.5
            for heading in range(0, 360, 45):
                self.lines.append(([globe(pitch, heading + half_width * i / 4) for i in range(-4, 5)],
                                   2 if pitch % 10 == 0 else 1))
        for heading in range(0, 360, 45):
            self.lines.append(([globe(p, heading) for p in range(-90, 91, 2)], 1))
        # Separate heading and pitch labels: a meridian's number is a compass
        # bearing, while the flanking numbers measure degrees above/below horizon.
        # Lift the horizon bearings into the blue hemisphere so their blue
        # backgrounds never straddle the brown ground, including when inverted.
        self.labels = [(str(h), p, h, True) for p in (5, 45, -45)
                       for h in range(0, 360, 45)]
        self.labels += [(str(p), p, (h + offset) % 360, False)
                        for p in range(-80, 81, 10) if p != 0
                        for h in range(0, 360, 45) for offset in (-7, 7)]

    @staticmethod
    def symbol(surface, kind, color, pos, size, back=False):
        x, y = pos
        w = max(1, round(size * 0.17))
        s = size
        if kind in ('prograde', 'retrograde'):
            pygame.draw.circle(surface, color, (x, y), s * .58, w)
            if kind == 'retrograde':
                for dx in (-1, 1):
                    pygame.draw.line(surface, color, (x - dx * s * .42, y - s * .42),
                                     (x + dx * s * .42, y + s * .42), w)
            else:
                pygame.draw.circle(surface, color, (x, y), max(1, s * .13))
            for angle in (-90, 30, 150):
                a = math.radians(angle)
                pygame.draw.line(surface, color, (x + s * .6 * math.cos(a), y + s * .6 * math.sin(a)),
                                 (x + s * math.cos(a), y + s * math.sin(a)), w)
        elif kind in ('normal', 'anti_normal'):
            points = [(x, y - s), (x + s * .87, y + s * .5), (x - s * .87, y + s * .5)]
            if kind == 'anti_normal':
                points = [(a, 2 * y - b) for a, b in points]
            pygame.draw.polygon(surface, color, points, w)
            pygame.draw.circle(surface, color, (x, y), max(1, s * .15))
        elif kind in ('radial', 'anti_radial'):
            pygame.draw.circle(surface, color, (x, y), s * .65, w)
            for angle in range(0, 360, 90):
                a = math.radians(angle)
                pygame.draw.line(surface, color, (x + s * .7 * math.cos(a), y + s * .7 * math.sin(a)),
                                 (x + s * 1.1 * math.cos(a), y + s * 1.1 * math.sin(a)), w)
            if kind == 'anti_radial':
                pygame.draw.line(surface, color, (x - s * .4, y - s * .4), (x + s * .4, y + s * .4), w)
                pygame.draw.line(surface, color, (x - s * .4, y + s * .4), (x + s * .4, y - s * .4), w)
        elif kind in ('target', 'anti_target'):
            pygame.draw.circle(surface, color, (x, y), s * .55, w)
            pygame.draw.line(surface, color, (x - s, y), (x + s, y), w)
            pygame.draw.line(surface, color, (x, y - s), (x, y + s), w)
            if kind == 'anti_target':
                pygame.draw.line(surface, color, (x - s * .7, y - s * .7), (x + s * .7, y + s * .7), w)
        elif kind == 'maneuver':
            pygame.draw.circle(surface, color, (x, y), s * .6, w)
            for a in range(0, 360, 120):
                a = math.radians(a)
                pygame.draw.line(surface, color, (x + s * .6 * math.cos(a), y + s * .6 * math.sin(a)),
                                 (x + s * 1.15 * math.cos(a), y + s * 1.15 * math.sin(a)), w)

    def light_maps(self, size, radius):
        """Cache a fixed screen-space light: sphere normals provide volume while attitude rotates underneath.

        Build at a small resolution and scale once; no numpy, shaders or per-pixel frame loop on the Pi.
        """
        key = (size, radius)
        if self.lighting_size != key:
            resolution = 192
            shade = pygame.Surface((resolution, resolution), pygame.SRCALPHA)
            shine = pygame.Surface((resolution, resolution), pygame.SRCALPHA)
            for iy in range(resolution):
                y = ((iy + .5) * size / resolution - size / 2) / radius
                for ix in range(resolution):
                    x = ((ix + .5) * size / resolution - size / 2) / radius
                    squared = x*x + y*y
                    if squared >= 1:
                        continue
                    z = math.sqrt(1 - squared)
                    diffuse = max(0, -.35*x - .42*y + .837*z)
                    brightness = max(.26, .40 + .60*diffuse - .13*(1 - z)**2)
                    shade.set_at((ix, iy), (0, 0, 0, round(255 * (1 - brightness))))
                    reflection = max(0, -.28*x - .37*y + .885*z)**28
                    shine.set_at((ix, iy), (230, 242, 255, round(38 * reflection)))
            self.lighting = (pygame.transform.smoothscale(shade, (size, size)),
                             pygame.transform.smoothscale(shine, (size, size)))
            self.lighting_size = key
        return self.lighting

    def draw(self, display, rect, snap):
        d, u = display, display.u
        d.panel(rect)
        d.text('NAVBALL', 2.6, (122, 140, 164), (rect.x + 1.8 * u, rect.y + 1.8 * u))
        n = snap.nav
        if n is None:
            d.text('Attitude unavailable', 3.4, WHITE, rect.center, 'center')
            return
        radius = min(rect.h * .31, rect.w * .31)
        cx, cy = rect.centerx - 1.8 * u, rect.y + rect.h * .46
        axes = basis(n.rotation)
        d.text('P %+.1f°   R %+.1f°' % (n.pitch, n.roll), 2, INK,
               (rect.right - 1.8 * u, rect.y + 1.8 * u), 'topright')
        r = radius
        size = math.ceil(2 * r + 4)
        ball = pygame.Surface((size, size), pygame.SRCALPHA)
        origin = size / 2

        def point(v):
            return origin + v[0] * r, origin + v[1] * r

        def stroke_line(segment, width):
            if len(segment) < 2:
                return
            stroke = max(1, round(width * .13 * u))
            if stroke == 1:
                pygame.draw.aalines(ball, INK, False, segment)
            else:
                pygame.draw.lines(ball, INK, False, segment, stroke)

        for cell, color in self.cells:
            vertices = front_polygon([projected(v, axes) for v in cell])
            if len(vertices) >= 3:
                pygame.draw.polygon(ball, color, [point(v) for v in vertices])
        for line, width in self.lines:
            segment = []
            for vector in line:
                v = projected(vector, axes)
                if v[2] >= .005:
                    segment.append(point(v))
                else:
                    stroke_line(segment, width)
                    segment = []
            stroke_line(segment, width)
        font = d.font(2.3)
        font_size = font.get_height()
        if font_size not in self.bold_fonts:
            heading_font = pygame.font.Font(None, max(8, int(2.3 * u)))
            heading_font.set_bold(True)
            self.bold_fonts[font_size] = heading_font
        heading_font = self.bold_fonts[font_size]
        for text, p, h, is_heading in self.labels:
            projection = label_projection(p, h, axes)
            if projection is not None:
                v, width_scale, height_scale, angle, alpha = projection
                # Knock out the grid under the numerals, like the stock texture,
                # rather than drawing a thin white line through every digit.
                label_font = heading_font if is_heading else font
                glyph_key = (font_size, text, is_heading, p >= 0)
                if glyph_key not in self.label_images:
                    glyph = label_font.render(text, True, WHITE, SKY if p >= 0 else GROUND)
                    # Font rendering with a background can return an indexed 8-bit
                    # surface. Use explicit RGBA without requiring an SDL video mode.
                    rgba = pygame.Surface(glyph.get_size(), pygame.SRCALPHA, 32)
                    rgba.blit(glyph, (0, 0))
                    self.label_images[glyph_key] = rgba
                image = self.label_images[glyph_key]
                # Foreshorten the lettering with the globe, rather than hide
                # colliding screen rectangles as the ship rolls.
                image = pygame.transform.smoothscale(image, (max(1, round(image.get_width() * width_scale)),
                                                             max(1, round(image.get_height() * height_scale))))
                image = pygame.transform.rotate(image, -angle)
                image.set_alpha(alpha)
                ball.blit(image, image.get_rect(center=point(v)))
        shade, shine = self.light_maps(size, r)
        ball.blit(shade, (0, 0))
        ball.blit(shine, (0, 0))
        d.screen.blit(ball, (cx - origin, cy - origin))
        pygame.draw.circle(d.screen, (91, 104, 114), (cx, cy), r + 1.7 * u, max(2, round(2.6 * u)))
        bezel = pygame.Rect(0, 0, 2 * (r + 1.7 * u), 2 * (r + 1.7 * u))
        bezel.center = (cx, cy)
        pygame.draw.arc(d.screen, (151, 164, 171), bezel, math.pi / 4, 5 * math.pi / 4,
                        max(1, round(.7 * u)))
        pygame.draw.arc(d.screen, (48, 57, 64), bezel, 5 * math.pi / 4, 9 * math.pi / 4,
                        max(1, round(.7 * u)))
        pygame.draw.circle(d.screen, (181, 190, 194), (cx, cy), r + .25 * u, max(1, round(.25 * u)))
        # Roll scale and fixed top index; it remains stationary while the globe turns.
        for degrees in range(-60, 61, 10):
            a = math.radians(degrees - 90)
            outer = r + 2.6 * u
            inner = outer - (.9 if degrees % 30 == 0 else .5) * u
            pygame.draw.line(d.screen, INK, (cx + inner * math.cos(a), cy + inner * math.sin(a)),
                             (cx + outer * math.cos(a), cy + outer * math.sin(a)), max(1, round(.15 * u)))
        pygame.draw.polygon(d.screen, GOLD, [(cx, cy - r - 1.5 * u), (cx - .8 * u, cy - r - 3 * u), (cx + .8 * u, cy - r - 3 * u)])

        markers = [('prograde', n.prograde, GOLD), ('normal', n.normal, MAGENTA), ('radial', n.radial, CYAN),
                   ('target', n.target, MAGENTA), ('maneuver', n.maneuver, (65, 140, 255))]
        for kind, vector, color in markers:
            vector = unit(vector) if vector is not None else None
            if vector is None:
                continue
            versions = [(kind, vector)]
            if kind != 'maneuver':
                opposite = {'prograde': 'retrograde', 'normal': 'anti_normal', 'radial': 'anti_radial', 'target': 'anti_target'}[kind]
                versions.append((opposite, tuple(-v for v in vector)))
            for symbol, direction in versions:
                x, y, z = projected(direction, axes)
                if z < .03:
                    continue
                # Keep the entire glyph inside the rim, even near the limb.
                marker_size = self.MARKER_SIZE * u
                limit = min(.91, 1 - 1.2 * marker_size / r)
                length = math.hypot(x, y)
                if length > limit:
                    x, y = x * limit / length, y * limit / length
                self.symbol(d.screen, symbol, color, (cx + x * r, cy + y * r), marker_size)
        # Fixed stock-style gold aircraft / boresight symbol, with dark outline.
        wings = [(cx - 5 * u, cy), (cx - 2.3 * u, cy), (cx - .8 * u, cy + 1.2 * u),
                 (cx + .8 * u, cy + 1.2 * u), (cx + 2.3 * u, cy), (cx + 5 * u, cy)]
        pygame.draw.lines(d.screen, (44, 30, 9), False, wings, max(3, round(.75 * u)))
        pygame.draw.lines(d.screen, GOLD, False, wings, max(2, round(.35 * u)))
        pygame.draw.circle(d.screen, (44, 30, 9), (cx, cy), max(2, .55 * u))
        pygame.draw.circle(d.screen, GOLD, (cx, cy), max(1, .3 * u))

        # Speed mode and heading displays, plus the stock RCS/SAS status lamps.
        box = pygame.Rect(cx - 9 * u, cy - r - 5.4 * u, 18 * u, 5.7 * u)
        pygame.draw.rect(d.screen, (50, 59, 64), box, border_radius=round(.5 * u))
        d.text(n.speed_mode.title(), 1.8, GREEN, (cx, box.y + .4 * u), 'midtop')
        d.text(fmt.speed(n.speed) if n.speed is not None else '-- m/s', 2.8, GREEN, (cx, box.y + 2.4 * u), 'midtop')
        d.text('RCS', 2.4, GREEN if n.rcs else (108, 118, 121), (cx - 14 * u, box.centery), 'center')
        d.text('SAS', 2.4, GREEN if n.sas else (108, 118, 121), (cx + 14 * u, box.centery), 'center')
        d.text('HDG %03d°' % (round(n.heading) % 360), 2.8, GREEN, (cx, cy + r + 2.2 * u), 'midtop')

        # Throttle and G-force arcs, placed like the stock navball bezel.
        g_color = GREEN if n.g_force < 5 else (255, 186, 72) if n.g_force < 10 else (255, 92, 92)
        for side, value, maximum, label, color in ((-1, n.throttle, 1, 'THR', WHITE), (1, n.g_force, 15, 'G', g_color)):
            arc = []
            for i in range(81):
                angle = math.radians((140 + i) if side == -1 else (-40 + i))
                arc.append((cx + (r + 3.7 * u) * math.cos(angle), cy + (r + 3.7 * u) * math.sin(angle)))
            pygame.draw.lines(d.screen, (66, 78, 85), False, arc, max(2, round(.7 * u)))
            if side == 1:
                pygame.draw.lines(d.screen, (144, 64, 57), False, arc[:20], max(2, round(.6 * u)))
            ticks = ((0, 140), (100, 220)) if side == -1 else ((0, 40), (5, 13.333), (10, -13.333), (15, -40))
            for number, degrees in ticks:
                angle = math.radians(degrees)
                inner, outer = r + 3.2 * u, r + 4.2 * u
                pygame.draw.line(d.screen, INK, (cx + inner * math.cos(angle), cy + inner * math.sin(angle)),
                                 (cx + outer * math.cos(angle), cy + outer * math.sin(angle)), max(1, round(.15 * u)))
                d.text(str(number), 1.5, INK,
                       (cx + (r + 5.2 * u) * math.cos(angle), cy + (r + 5.2 * u) * math.sin(angle)), 'center')
            count = max(1, min(80, round(value / maximum * 80)))
            filled = arc[:count + 1] if side == -1 else arc[-count - 1:]
            pygame.draw.lines(d.screen, color, False, filled, max(2, round(.45 * u)))
            x = cx + side * (r + 5.8 * u)
            d.text(label, 2, INK, (x, cy - 2 * u), 'center')
            d.text('%d%%' % round(n.throttle * 100) if side == -1 else '%.1f' % n.g_force,
                   2.1, color, (x, cy + 1 * u), 'center')
        if n.node_remaining is not None:
            arc = [(cx + (r + 8 * u) * math.cos(math.radians(-70 + i)),
                    cy + (r + 8 * u) * math.sin(math.radians(-70 + i))) for i in range(101)]
            pygame.draw.lines(d.screen, (57, 71, 53), False, arc, max(2, round(.6 * u)))
            remaining = max(0, n.node_remaining)
            fraction = min(1, remaining / max(n.node_dv or remaining, 1))
            count = max(1, round(100 * fraction))
            pygame.draw.lines(d.screen, GREEN, False, arc[-count - 1:], max(2, round(.4 * u)))
            d.text(fmt.speed(remaining), 2, GREEN, (cx + r + 7 * u, cy - r - 3 * u), 'center')
        # SAS mode gadgets. Read-only indicators, following the game's selected mode.
        modes = [('stability_assist', 'HOLD'), ('prograde', 'PRO'), ('retrograde', 'RET'), ('normal', 'N+'), ('anti_normal', 'N-'),
                 ('radial', 'R+'), ('anti_radial', 'R-'), ('target', 'TGT'), ('anti_target', 'ANTI'), ('maneuver', 'NODE')]
        y = rect.bottom - 9.1 * u
        gap = .4 * u
        width = min(6.1 * u, (rect.w - 4 * u - 9 * gap) / 10)
        start = rect.centerx - (10 * width + 9 * gap) / 2
        for i, (mode, label) in enumerate(modes):
            button = pygame.Rect(start + i * (width + gap), y, width, 2.7 * u)
            active = n.sas and n.sas_mode == mode
            pygame.draw.rect(d.screen, (62, 91, 63) if active else (37, 46, 54), button, border_radius=max(1, round(.3 * u)))
            d.text(label, 1.6, GREEN if active else INK, button.center, 'center')
        if n.node_remaining is not None:
            remaining = max(n.node_remaining, 0)
            total = max(n.node_dv or remaining, 1)
            bar = pygame.Rect(rect.x + 2 * u, rect.bottom - 5.2 * u, rect.w - 4 * u, .6 * u)
            pygame.draw.rect(d.screen, (53, 65, 62), bar)
            pygame.draw.rect(d.screen, GREEN, (bar.x, bar.y, bar.w * min(1, remaining / total), bar.h))
            burn = fmt.duration(n.burn_seconds) if n.burn_seconds is not None else '--'
            countdown = fmt.duration(n.node_time) if n.node_time is not None else '--'
            d.text('NODE %s   T %s   EST BURN %s' % (fmt.speed(remaining), countdown, burn),
                   2.1, GREEN, (rect.centerx, rect.bottom - 3.8 * u), 'midtop')
            start = n.node_time - n.burn_seconds / 2 if n.burn_seconds is not None and n.node_time is not None else None
            if start is not None:
                d.text('START IN ' + fmt.duration(start), 1.8, INK, (rect.centerx, rect.bottom - 1.6 * u), 'midtop')
        else:
            d.text('NO MANEUVER NODE', 2.1, (108, 126, 141), (rect.centerx, rect.bottom - 3.8 * u), 'midtop')
