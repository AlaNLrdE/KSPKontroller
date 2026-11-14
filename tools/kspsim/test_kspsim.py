"""Tests of the simulator, no board needed: ``./ci test_sim``.

The kRPC test runs the real HMI client (hmi-pi/ksphmi/telemetry.py, krpc 0.7) against the stand-in server on
free local ports, so the Pi's code path is checked end to end on the PC.
"""
import math
import os
import sys
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path[:0] = [os.path.dirname(HERE), os.path.join(ROOT, "hmi-pi")]

from kspsim import arduino, physics  # noqa: E402
from kspsim.sim import Simulator  # noqa: E402


class WireTest(unittest.TestCase):
    def test_sizes_match_the_firmware(self):
        self.assertEqual(arduino.VESSEL_DATA.size, 214)
        self.assertEqual(arduino.CONTROL.size, 25)

    def test_frames_survive_garbage(self):
        p = arduino.FrameParser()
        data = b"\x00\xBE" + arduino.frame(b"\x00\x03\x01\x04") + b"\xBE\xEF\x03\x01\x02\x03\x99" + arduino.frame(b"\x65" * 25)
        out = p.feed(data[:7]) + p.feed(data[7:])
        self.assertEqual(out, [b"\x00\x03\x01\x04", b"\x65" * 25])
        self.assertEqual(p.bad, 1)

    def test_vessel_data_fields(self):
        v = physics.scenario("orbit")
        v.controls = physics.Controls(sas=True, rcs=True, groups=1 << 1 | 1 << 10)
        raw = arduino.encode_vessel(v, 0.2)
        self.assertEqual(len(raw), 214)
        f = dict(zip(arduino.VESSEL_FIELDS, arduino.VESSEL_DATA.unpack(raw)))
        self.assertEqual(f["id"], 1)
        self.assertAlmostEqual(f["AP"], 250_000, delta=1)
        self.assertAlmostEqual(f["PE"], 80_000, delta=1)
        self.assertEqual(f["SOINumber"], 130)
        self.assertEqual(f["ActionGroups"], 1 | 2 | 1 << 6 | 1 << 15)  # SAS, RCS, AG1, AG10

    def test_vessel_data_never_breaks_on_escape(self):
        raw = arduino.encode_vessel(physics.scenario("escape"), 0.2)  # inf times, negative Ap
        f = dict(zip(arduino.VESSEL_FIELDS, arduino.VESSEL_DATA.unpack(raw)))
        self.assertEqual(f["TAp"], 0)
        self.assertTrue(all(math.isfinite(x) for x in f.values()))

    def test_control_bits(self):
        raw = arduino.CONTROL.pack(101, 1 << 7 | 1 << 0, 0, 1 << 3, 0, 0, 100, -200, 300, 0, 0, 0, 0, 750, 0)
        c = arduino.decode_controls(raw)
        self.assertTrue(c.sas and c.stage)
        self.assertFalse(c.rcs)
        self.assertEqual(c.groups, 8)
        self.assertAlmostEqual(c.throttle, 0.75)
        self.assertAlmostEqual(c.roll, -0.2)


class PhysicsTest(unittest.TestCase):
    def test_coasting_orbit_keeps_its_shape(self):
        v = physics.scenario("orbit")
        before = v.elements()
        t = 0.0
        while t < before.period:
            v.step(5.0)
            t += 5.0
        after = v.elements()
        self.assertAlmostEqual(after.apoapsis, before.apoapsis, delta=50)
        self.assertAlmostEqual(after.periapsis, before.periapsis, delta=50)

    def test_launch_leaves_the_pad_and_gains_altitude(self):
        v = physics.scenario("pad")
        v.apply_controls(physics.Controls(throttle=1.0))
        for _ in range(120):
            v.step(1.0)
        self.assertFalse(v.landed)
        self.assertGreater(v.altitude, 10_000)
        self.assertLess(v.liquid_fuel, physics.LIQUID_FUEL)

    def test_low_throttle_stays_on_the_pad(self):
        v = physics.scenario("pad")
        v.apply_controls(physics.Controls(throttle=0.18))
        v.step(10.0)
        self.assertTrue(v.landed)
        self.assertEqual(v.situation(), "pre_launch")

    def test_suborbital_falls_and_lands(self):
        v = physics.scenario("suborbital")
        for _ in range(2000):
            v.step(2.0)
            if v.landed:
                break
        self.assertTrue(v.landed)
        self.assertEqual(v.situation(), "landed")

    def test_set_orbit(self):
        v = physics.scenario("pad")
        physics.set_orbit(v, 500_000, 100_000, 12.0, 90)
        el = v.elements()
        self.assertAlmostEqual(el.apoapsis, 500_000, delta=1)
        self.assertAlmostEqual(el.periapsis, 100_000, delta=1)
        self.assertAlmostEqual(math.degrees(el.nu), 90, places=6)
        self.assertFalse(v.landed)
        self.assertEqual(v.situation(), "orbiting")
        for ap, pe, nu in ((100_000, 200_000, 0), (100_000, -700_000, 180), (70_000, -300_000, 0)):
            with self.assertRaises(ValueError):  # Ap below Pe, Pe below the centre, a point under the surface
                physics.set_orbit(v, ap, pe, 0.0, nu)

    def test_stage_on_rising_edge_only(self):
        v = physics.scenario("pad")
        for level in (True, True, False, True):
            v.apply_controls(physics.Controls(stage=level))
        self.assertEqual(v.stage, physics.STAGES - 3)


class SimulatorTest(unittest.TestCase):
    def test_board_throttle_can_be_ignored(self):
        sim = Simulator("orbit")
        board = physics.Controls(throttle=0.19, rcs=True, sas=True)
        sim.board_controls(board)
        self.assertAlmostEqual(sim.vessel.controls.throttle, 0.19)
        sim.board_throttle = False
        sim.set_manual(throttle=0.0)
        self.assertEqual(sim.vessel.controls.throttle, 0.0)  # engine off at once
        self.assertTrue(sim.vessel.controls.rcs and sim.vessel.controls.sas)  # the rest still from the board
        sim.board_controls(board)
        self.assertEqual(sim.vessel.controls.throttle, 0.0)
        sim.set_manual(throttle=0.5)
        self.assertAlmostEqual(sim.vessel.controls.throttle, 0.5)
        self.assertAlmostEqual(sim.last_board.throttle, 0.19)


class KrpcTest(unittest.TestCase):
    def test_hmi_client_end_to_end(self):
        from kspsim.krpcserver import KrpcServer
        from ksphmi.telemetry import FLIGHT, NO_VESSEL, CONNECTING, KrpcSource

        sim = Simulator("orbit")
        sim.start()
        server = KrpcServer(sim, "127.0.0.1", 0, 0)
        sim.listeners.append(lambda scene: server.stop() if scene == "closed" else server.start())
        server.start()
        src = KrpcSource("127.0.0.1", server.rpc_port, server.stream_port, rate_hz=20, retry_seconds=0.5)
        src.start()

        def wait(pred, timeout=10.0):
            end = time.monotonic() + timeout
            while time.monotonic() < end:
                s = src.latest()
                if pred(s):
                    return s
                time.sleep(0.05)
            self.fail("timeout, last snapshot %r" % (src.latest(),))

        try:
            s = wait(lambda s: s.state == FLIGHT)
            self.assertEqual((s.vessel, s.body, s.situation), ("Orbiter", "Kerbin", "orbiting"))
            self.assertAlmostEqual(s.apoapsis, 250_000, delta=5)
            self.assertAlmostEqual(s.periapsis, 80_000, delta=5)
            self.assertAlmostEqual(s.inclination, 0.4, places=6)

            sim.set_orbit(400_000, 120_000, 5.0, 180)  # edited in the window: same vessel, new values
            sim.set_vessel(name="Renamed")
            s = wait(lambda s: s.vessel == "Renamed" and abs(s.periapsis - 120_000) < 5)
            self.assertAlmostEqual(s.apoapsis, 400_000, delta=5)
            self.assertAlmostEqual(s.altitude, 400_000, delta=50)  # at the apoapsis
            self.assertAlmostEqual(s.inclination, 5.0, places=6)

            sim.set_scene("space_center")
            wait(lambda s: s.state == NO_VESSEL)
            sim.set_scene("flight")
            sim.load("escape")  # a new vessel: the client must re-open its streams on it
            s = wait(lambda s: s.state == FLIGHT and s.vessel == "Voyager")
            self.assertGreater(s.eccentricity, 1)
            self.assertEqual(s.situation, "escaping")

            sim.set_scene("closed")
            wait(lambda s: s.state == CONNECTING)
            sim.set_scene("flight")  # the ports reopen; the client reconnects by itself
            wait(lambda s: s.state == FLIGHT)
        finally:
            src.stop()
            server.stop()
            sim.stop()


if __name__ == "__main__":
    unittest.main()
