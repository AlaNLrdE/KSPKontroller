"""Attitude conventions, marker geometry and flight layout regressions."""
import math
import os
from pathlib import Path
import sys
import unittest
from dataclasses import replace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from ksphmi.navigation import Navigation, attitude, basis, burn_time, dot, unit


class AttitudeTest(unittest.TestCase):
    def assertVector(self, actual, expected):
        for a, b in zip(actual, expected):
            self.assertAlmostEqual(a, b, places=6)

    def test_compass_pitch_roll_and_poles(self):
        self.assertVector(basis(attitude(0, 0, 0))[1], (0, 1, 0))
        self.assertVector(basis(attitude(0, 90, 0))[1], (0, 0, 1))
        self.assertVector(basis(attitude(90, 45, 120))[1], (1, 0, 0))
        self.assertVector(basis(attitude(-90, 45, -120))[1], (-1, 0, 0))
        self.assertVector(basis(attitude(0, 90, 90))[0], (-1, 0, 0))
        for pitch in (-90, -45, 0, 45, 90):
            for roll in (-180, -90, 0, 90, 180):
                right, forward, up = basis(attitude(pitch, 125, roll))
                self.assertAlmostEqual(dot(right, forward), 0)
                self.assertAlmostEqual(dot(up, forward), 0)
                for axis in (right, forward, up):
                    self.assertAlmostEqual(dot(axis, axis), 1)

    def test_krpc_rotation_axes_match_observed_flight_direction(self):
        # Paired read-only Flight.rotation / Flight.direction sample from real kRPC.
        quaternion = (-.2744391398, -.6535272784, -.6527750457, -.2673387262)
        self.assertVector(basis(quaternion)[1], (.0096828239, -.0028641836, .9999490184))

    def test_invalid_and_zero_vectors(self):
        self.assertIsNone(unit((0, 0, 0)))
        self.assertIsNone(unit((float('nan'), 0, 0)))
        with self.assertRaises(ValueError):
            basis((0, 0, 0, 0))

    def test_burn_estimate_and_unavailable_engine(self):
        self.assertIsNone(burn_time(100, 1000, 0, 300))
        self.assertIsNone(burn_time(float('inf'), 1000, 100, 300))
        # Small delta-v approaches delta-v / acceleration.
        self.assertAlmostEqual(burn_time(.01, 1000, 100, 300), .1, places=5)
        self.assertLess(burn_time(1000, 1000, 100, 300), 10000)


class FlightLayoutTest(unittest.TestCase):
    def test_degree_labels_remain_visible_during_roll_and_fade_smoothly_at_limb(self):
        try:
            import pygame
        except ImportError:
            self.skipTest('pygame in build/hmi-venv required')
        from ksphmi.navball import label_projection
        # A front-facing bearing must stay fully visible through an entire roll.
        for roll in range(-180, 181):
            projection = label_projection(5, 90, basis(attitude(0, 90, roll)))
            self.assertIsNotNone(projection)
            self.assertEqual(projection[-1], 255)
        # Sweep the same label past the limb in quarter-degree increments.
        alphas = []
        for step in range(361):
            projection = label_projection(5, 0, basis(attitude(0, step / 4, 0)))
            alphas.append(projection[-1] if projection is not None else 0)
        self.assertGreater(sum(0 < a < 255 for a in alphas), 20)
        self.assertTrue(all(0 <= a - b <= 9 for a, b in zip(alphas, alphas[1:])))


    def test_navball_labels_fit_and_globe_never_leaks_into_other_panels(self):
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
                u = d.u
                panel = pygame.Rect(d.w * .44 + 1.6 * u, 36.6 * u,
                                    d.w * .56 - 4.1 * u, 60.9 * u)
                original = d.text
                labels = {}

                def text(label, *args, **kwargs):
                    rect = original(label, *args, **kwargs)
                    self.assertTrue(panel.contains(rect), (size, label, rect, panel))
                    labels[label] = rect
                    return rect

                d.text = text
                snap = DemoSource().sample(0)
                for pitch, heading, roll in ((0, 0, 0), (90, 0, 90), (-90, 270, -90), (10, 125, 180)):
                    d.screen.fill((1, 2, 3))
                    nav = replace(snap.nav, rotation=attitude(pitch, heading, roll), heading=heading,
                                  pitch=pitch, roll=roll)
                    d.navball.draw(d, panel, replace(snap, nav=nav))
                    heading_rect = labels['HDG %03d°' % heading]
                    for label in ('HOLD', 'PRO', 'RET', 'N+', 'N-', 'R+', 'R-', 'TGT', 'ANTI', 'NODE'):
                        self.assertFalse(heading_rect.colliderect(labels[label]), (size, label))
                    self.assertEqual(d.screen.get_at((panel.x - 1, panel.centery))[:3], (1, 2, 3))
                d.navball.draw(d, panel, replace(snap, nav=Navigation()))
                d.navball.draw(d, panel, replace(snap, nav=None))
        finally:
            pygame.quit()
