"""Regression: RCS near a circular orbit must not teleport the active-vessel ring."""
import math
import os
from pathlib import Path
import sys
import unittest
from dataclasses import replace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from ksphmi.orbitview import OrbitState, view_axes
from ksphmi.navigation import dot


class InertialOrbitTest(unittest.TestCase):
    def test_rcs_can_flip_periapsis_without_changing_measured_position(self):
        mu, radius = 3.5316e12, 700000
        speed = math.sqrt(mu / radius)
        axes = view_axes((radius, 0, 0), (0, 0, speed))
        before = OrbitState((radius, 0, 0), (0, 0, speed + .05), mu, axes)
        after = replace(before, velocity=(0, 0, speed - .05))
        self.assertLess(dot(before.conic().peri_direction, after.conic().peri_direction), -.99)
        self.assertEqual(before.vessel_point, after.vessel_point)
        self.assertEqual(before.vessel_point, (radius, 0))

    def test_circular_and_inclined_orbits_have_stable_reference_axes(self):
        mu, radius = 3.5316e12, 700000
        for velocity in ((0, 0, math.sqrt(mu / radius)), (0, math.sqrt(mu / radius), 0)):
            state = OrbitState((radius, 0, 0), velocity, mu, view_axes((radius, 0, 0), velocity))
            self.assertAlmostEqual(state.conic().eccentricity, 0)
            self.assertAlmostEqual(state.conic().periapsis, radius)
            self.assertAlmostEqual(state.conic().apoapsis, radius)
            self.assertEqual(state.vessel_point, (radius, 0))

    def test_rendered_marker_stays_put_and_inertial_escape_stays_clipped(self):
        os.environ.setdefault('SDL_VIDEODRIVER', 'dummy')
        try:
            import pygame
        except ImportError:
            self.skipTest('pygame in build/hmi-venv required')
        from ksphmi import config
        from ksphmi.display import Display, VALUE
        from ksphmi.telemetry import DemoSource
        try:
            mu, radius = 3.5316e12, 700000
            speed = math.sqrt(mu / radius)
            axes = view_axes((radius, 0, 0), (0, 0, speed))
            for size in ((1260, 840), (1920, 1080), (2520, 1680)):
                d = Display(config.load(), windowed=True, size=size)
                u = d.u
                panel = pygame.Rect(d.w * .44 + 1.6 * u, 13 * u, d.w * .56 - 4.1 * u, 22 * u)
                positions = []
                for delta, anomaly in ((.05, 0), (-.05, math.pi)):
                    state = OrbitState((radius, 0, 0), (0, 0, speed + delta), mu, axes)
                    # Deliberately flip true anomaly: the marker must use Cartesian position instead.
                    snap = replace(DemoSource().sample(0), orbit_state=state, true_anomaly=anomaly)
                    with patch('ksphmi.display.pygame.draw.circle', wraps=pygame.draw.circle) as circles:
                        d.orbit(panel, snap)
                        markers = [call.args[2] for call in circles.call_args_list if call.args[1] == VALUE]
                        self.assertEqual(len(markers), 1)
                        positions.append(markers[0])
                self.assertLess(math.dist(*positions), 1)

                # Far out on an actual inertial hyperbola: neither curve nor marker may leak.
                peri = 280000
                state = OrbitState((peri, 0, 0), (0, 0, math.sqrt(6.5e10 * 2.5 / peri)), 6.5e10, axes)
                conic = state.conic()
                anomaly = math.acos(-1 / conic.eccentricity) - .00001
                state = replace(state, position=conic.position(anomaly), velocity=tuple(
                    math.sqrt(state.mu / conic.p) * (-math.sin(anomaly) * p + (conic.eccentricity + math.cos(anomaly)) * q)
                    for p, q in zip(conic.peri_direction, conic.transverse)))
                d.screen.fill((1, 2, 3))
                d.orbit(panel, replace(DemoSource().sample(0), body='Mun', body_radius=200000,
                                      atmosphere_depth=0, orbit_state=state))
                outside = d.screen.copy()
                outside.fill((1, 2, 3), panel)
                expected = pygame.Surface(size)
                expected.fill((1, 2, 3))
                self.assertEqual(pygame.image.tobytes(outside, 'RGB'), pygame.image.tobytes(expected, 'RGB'))
        finally:
            pygame.quit()
