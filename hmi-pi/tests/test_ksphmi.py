"""Unit tests that need neither pygame nor kRPC: ``python -m unittest discover -s hmi-pi/tests``."""
import math
import os
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from ksphmi import config, fmt  # noqa: E402
from ksphmi.telemetry import (FLIGHT, KERBIN_MU, KERBIN_RADIUS, DemoSource, Snapshot,  # noqa: E402
                              kepler_state, stale)


class FormatTest(unittest.TestCase):
    def test_distance(self):
        self.assertEqual(fmt.distance(950), "950 m")
        self.assertEqual(fmt.distance(80_000), "80.0 km")
        self.assertEqual(fmt.distance(-12_400), "-12.4 km")
        self.assertEqual(fmt.distance(12_400_000), "12.40 Mm")
        self.assertEqual(fmt.distance(float("nan")), "--")

    def test_duration_uses_kerbin_days(self):
        self.assertEqual(fmt.duration(45), "45s")
        self.assertEqual(fmt.duration(723), "12m 03s")
        self.assertEqual(fmt.duration(7500), "2h 05m")
        self.assertEqual(fmt.duration(6 * 3600 * 3 + 3600 * 4), "3d 4h")
        self.assertEqual(fmt.duration(25 * 3600, day_hours=24), "1d 1h")
        self.assertEqual(fmt.duration(float("inf")), "--")

    def test_clock(self):
        self.assertEqual(fmt.clock(0), "Y1 D001 00:00:00")
        self.assertEqual(fmt.clock(6 * 3600 * 426 + 6 * 3600 + 61), "Y2 D002 00:01:01")

    def test_situation(self):
        self.assertEqual(fmt.situation("sub_orbital"), "Sub-orbital")
        self.assertEqual(fmt.situation("orbiting"), "Orbiting")


class KeplerTest(unittest.TestCase):
    def test_circular_orbit(self):
        a = KERBIN_RADIUS + 100_000
        r, nu, v, vr = kepler_state(a, 0.0, KERBIN_MU, 1.0)
        self.assertAlmostEqual(r, a)
        self.assertAlmostEqual(nu, 1.0)
        self.assertAlmostEqual(v, math.sqrt(KERBIN_MU / a))
        self.assertAlmostEqual(vr, 0.0)

    def test_apsides(self):
        a, e = 765_000.0, 0.11
        r_pe, nu_pe, v_pe, _ = kepler_state(a, e, KERBIN_MU, 0.0)
        r_ap, nu_ap, v_ap, _ = kepler_state(a, e, KERBIN_MU, math.pi)
        self.assertAlmostEqual(r_pe, a * (1 - e), places=3)
        self.assertAlmostEqual(r_ap, a * (1 + e), places=3)
        self.assertAlmostEqual(nu_ap, math.pi, places=9)
        self.assertGreater(v_pe, v_ap)

    def test_high_eccentricity_converges(self):
        for m in (0.01, 1.0, 3.0, 6.0):
            r, _, _, _ = kepler_state(10e6, 0.97, KERBIN_MU, m)
            self.assertTrue(10e6 * 0.03 - 1 <= r <= 10e6 * 1.97 + 1)


class DemoTest(unittest.TestCase):
    def test_sample_is_consistent(self):
        src = DemoSource()
        s = src.sample(0.0)  # at periapsis
        self.assertEqual(s.state, FLIGHT)
        self.assertAlmostEqual(s.periapsis, 80_000, places=3)
        self.assertAlmostEqual(s.apoapsis, 250_000, places=3)
        self.assertAlmostEqual(s.altitude, s.periapsis, places=3)
        self.assertAlmostEqual(s.time_to_apoapsis, s.period / 2, places=3)
        later = src.sample(s.period / 4)
        self.assertGreater(later.vertical_speed, 0)  # climbing towards apoapsis
        self.assertLess(later.time_to_apoapsis, s.time_to_apoapsis)

    def test_stale(self):
        now = time.monotonic()
        self.assertFalse(stale(Snapshot(state=FLIGHT, received=now), now + 1))
        self.assertTrue(stale(Snapshot(state=FLIGHT, received=now), now + 5))
        self.assertFalse(stale(Snapshot(), now + 100))  # not in flight: nothing to go stale


class ConfigTest(unittest.TestCase):
    def test_layers(self):
        with tempfile.TemporaryDirectory() as d:
            local = os.path.join(d, "local.toml")
            with open(local, "w") as f:
                f.write('[display]\nfps = 5\n')
            old = dict(os.environ)
            try:
                os.environ["KSPHMI_LOCAL_CONFIG"] = local
                os.environ["KSPHMI_KRPC_ADDRESS"] = "10.0.0.9"
                cfg = config.load()
            finally:
                os.environ.clear()
                os.environ.update(old)
        self.assertEqual(cfg["display"]["fps"], 5)
        self.assertTrue(cfg["display"]["fullscreen"])  # untouched keys keep config.toml / defaults
        self.assertEqual(cfg["krpc"]["address"], "10.0.0.9")
        self.assertEqual(cfg["krpc"]["rpc_port"], 50000)


if __name__ == "__main__":
    unittest.main()
