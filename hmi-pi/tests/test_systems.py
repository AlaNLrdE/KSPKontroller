"""Resource semantics and compact flight layout regressions."""
import os
from pathlib import Path
import sys
import unittest
from dataclasses import replace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from ksphmi.systems import Resource, Stage, Systems, RESOURCE_NAMES


class ResourceTest(unittest.TestCase):
    def test_empty_missing_and_unknown_are_distinct(self):
        self.assertEqual(Resource('ElectricCharge', 0, 100).fraction, 0)
        self.assertIsNone(Resource('ElectricCharge', 0, 0).fraction)
        self.assertIsNone(Resource('ElectricCharge', None, None).fraction)
        self.assertIsNone(Resource('ElectricCharge', float('nan'), 100).fraction)
        self.assertEqual(Resource('ElectricCharge', 125, 100).fraction, 1)
        self.assertEqual(Resource('ElectricCharge', -1, 100).fraction, 0)

    def test_compact_metrics_leave_space_for_resources_and_staging(self):
        os.environ.setdefault('SDL_VIDEODRIVER', 'dummy')
        try:
            import pygame
        except ImportError:
            self.skipTest('pygame in build/hmi-venv required')
        from ksphmi import config
        from ksphmi.display import Display
        from ksphmi.telemetry import DemoSource
        try:
            for size in ((1260, 840), (1920, 1080), (2520, 1680)):
                d = Display(config.load(), windowed=True, size=size)
                original_tile, original_text = d.tile, d.text
                tiles, active = [], []

                def text(*args, **kwargs):
                    rect = original_text(*args, **kwargs)
                    bounds = active[-1] if active else d.screen.get_rect()
                    self.assertTrue(bounds.contains(rect), (size, args[0], rect, bounds))
                    return rect

                def tile(rect, *args, **kwargs):
                    tiles.append(rect)
                    active.append(rect)
                    original_tile(rect, *args, **kwargs)
                    active.pop()

                d.tile, d.text = tile, text
                sample = DemoSource().sample(0)
                for systems in (sample.systems, None, Systems(0),
                                Systems(3, tuple(Resource(name, 7.5, 100) for name in RESOURCE_NAMES),
                                        tuple(Stage(n, 10, 1234) for n in (3, 2, 1, 0)))):
                    tiles.clear()
                    d.draw(replace(sample, systems=systems))
                    self.assertEqual(len(tiles), 10)
                    span = max(r.bottom for r in tiles) - min(r.top for r in tiles)
                    self.assertLess(span, d.h * .40)
                    self.assertLess(max(r.bottom for r in tiles), d.h * .54)
        finally:
            pygame.quit()


class GaugeGeometryTest(unittest.TestCase):
    def test_angle_mapping_sweeps_the_fuel_gauge_arc_from_e_to_f(self):
        import math
        from ksphmi.gauges import SWEEP, gauge_angle, pivot, point
        self.assertTrue(100 <= SWEEP <= 120)
        self.assertAlmostEqual(math.degrees(gauge_angle(0)), 90 + SWEEP / 2)
        self.assertAlmostEqual(math.degrees(gauge_angle(.5)), 90)
        self.assertAlmostEqual(math.degrees(gauge_angle(1)), 90 - SWEEP / 2)
        self.assertEqual(gauge_angle(-1), gauge_angle(0))
        self.assertEqual(gauge_angle(7), gauge_angle(1))
        px, py = pivot(100, 100, 10)
        self.assertAlmostEqual(px, 100)
        self.assertGreater(py, 100)  # the hub is in the lower half of the face
        x, y = point(px, py, 5, .5)  # screen y grows downwards: the middle of the scale is straight up
        self.assertAlmostEqual(x, px)
        self.assertAlmostEqual(y, py - 5)
        x0, y0 = point(0, 0, 1, 0)
        x1, y1 = point(0, 0, 1, 1)
        self.assertLess(x0, 0)  # E on the left
        self.assertGreater(x1, 0)  # F on the right
        self.assertAlmostEqual(y0, y1)
        self.assertLess(y0, 0)  # both ends above the hub: the arc is across the upper half

    def test_scale_needle_and_window_stay_on_the_face(self):
        import math
        from ksphmi import gauges
        px, py = gauges.pivot(0, 0, 1)
        for step in range(41):
            f = step / 40
            for radius in (gauges.ZONE[1], gauges.SCALE, gauges.LETTERS):
                x, y = gauges.point(px, py, radius, f)
                self.assertLess(math.hypot(x, y), gauges.FACE, (f, radius))
            for x, y in gauges.needle(px, py, 1, f):
                self.assertLess(math.hypot(x, y), gauges.FACE, f)
        top, bottom = gauges.WINDOW
        self.assertLess(math.hypot(gauges.WINDOW_W / 2, bottom), gauges.FACE)
        self.assertGreater(top, gauges.PIVOT)  # the window is under the hub

    def test_sector_closes_between_the_two_radii(self):
        import math
        from ksphmi.gauges import sector
        pts = sector(0, 0, 10, 8, 0, .5)
        radii = [math.hypot(x, y) for x, y in pts]
        half = len(pts) // 2
        self.assertTrue(all(abs(r - 10) < 1e-9 for r in radii[:half]))
        self.assertTrue(all(abs(r - 8) < 1e-9 for r in radii[half:]))

    def test_layout_fits_the_panel_at_every_resolution(self):
        from ksphmi.gauges import BELOW_CENTRE, gauge_layout
        for width, height in ((1260, 840), (1920, 1080), (2520, 1680)):
            u = height / 100
            x, y, w, h = 2.5 * u, 58 * u, width * .44 - 3.7 * u, 20.4 * u  # the resources panel interior
            text_h, pad = 4.0 * u, .4 * u
            for count in range(1, 7):
                radius, cells = gauge_layout(x, y, w, h, count, text_h, pad)
                self.assertEqual(len(cells), count)
                self.assertGreater(radius, 3.5 * u, (width, count))
                for cx, cy, cell_x, cell_w in cells:
                    self.assertGreaterEqual(cx - radius, x - 1e-6)
                    self.assertLessEqual(cx + radius, x + w + 1e-6)
                    self.assertGreaterEqual(cy - radius, y - 1e-6)
                    self.assertLessEqual(cy + BELOW_CENTRE * radius + text_h, y + h + 1e-6)
                    self.assertLessEqual(cell_w, w + 1e-6)
                for (a, _, _, _), (b, _, _, _) in zip(cells, cells[1:]):
                    self.assertGreaterEqual(b - a, 2 * radius - 1e-6)  # neighbours never overlap

    def test_dials_and_their_text_stay_inside_the_resources_panel(self):
        os.environ.setdefault('SDL_VIDEODRIVER', 'dummy')
        try:
            import pygame
        except ImportError:
            self.skipTest('pygame in build/hmi-venv required')
        from ksphmi import config
        from ksphmi.display import Display
        from ksphmi.telemetry import DemoSource
        resources = (Resource('ElectricCharge', 4, 200), Resource('LiquidFuel', 300, 1440),
                     Resource('Oxidizer', None, None), Resource('MonoPropellant', 0, 0),
                     Resource('SolidFuel', 12345.6, 12345.6), Resource('XenonGas', 0, 5700))
        try:
            for size in ((1260, 840), (1920, 1080), (2520, 1680)):
                d = Display(config.load(), windowed=True, size=size)
                panel, drawn = [], []
                original_resources, original_text, original_gauge = d.resources, d.text, d.gauge

                def resources_spy(rect, snap):
                    panel.append(rect)
                    original_resources(rect, snap)

                def text(*args, **kwargs):
                    rect = original_text(*args, **kwargs)
                    if panel and len(drawn) < 99:
                        drawn.append((args[0], rect))
                    return rect

                def gauge(resource, label, cx, cy, radius, *rest):
                    self.assertTrue(panel[-1].contains(pygame.Rect(cx - radius, cy - radius, 2 * radius,
                                                                   2 * radius)), (size, label))
                    original_gauge(resource, label, cx, cy, radius, *rest)

                d.resources, d.text, d.gauge = resources_spy, text, gauge
                sample = DemoSource().sample(0)
                for systems in (sample.systems, Systems(0, resources)):
                    panel.clear()
                    drawn.clear()
                    d.draw(replace(sample, systems=systems))
                    self.assertEqual(len(panel), 1)
                    texts = [(s, r) for s, r in drawn if panel[0].colliderect(r)]
                    self.assertGreater(len(texts), 6)
                    for s, r in texts:
                        self.assertTrue(panel[0].contains(r), (size, s, r, panel[0]))
                    labels = [r for s, r in texts if s in ('ELECTRICITY', 'XENON GAS', 'MONOPROPELLANT')]
                    for a in labels:
                        for b in labels:
                            self.assertTrue(a is b or not a.colliderect(b), (size, a, b))
        finally:
            pygame.quit()
