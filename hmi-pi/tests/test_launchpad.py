"""kRPC raises plain RuntimeError (InvalidOperationException) on the launch pad; optional panels must survive it."""
import os
import sys
import unittest
from types import SimpleNamespace

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ksphmi.navigation import NavigationStreams
from ksphmi.systems import SystemsStreams


class FakeStream:
    def __init__(self, func, args):
        self.func, self.args, self.rate = func, args, 0

    def __call__(self):
        return self.func(*self.args)

    def remove(self):
        pass


class FakeConn:
    def add_stream(self, func, *args):
        return FakeStream(func, args)


def singular(*_):
    raise RuntimeError("VesselSurfaceSpeed reference frame is singular when surface velocity is near zero")


class PadVessel:
    """Stationary on the pad: the surface speed frame and stage delta-v are unavailable."""
    surface_reference_frame = "surface"
    mass, available_thrust, specific_impulse = 10000.0, 0.0, 0.0
    control = SimpleNamespace(speed_mode=SimpleNamespace(name="surface"), sas=False,
                              sas_mode=SimpleNamespace(name="stability_assist"), rcs=False,
                              throttle=0.0, nodes=(), current_stage=1)
    resources = SimpleNamespace(amount=lambda name: 50.0, max=lambda name: 100.0)
    parts = SimpleNamespace(all=("p1", "p2"))

    def __init__(self):
        self.stages = (SimpleNamespace(number=1, parts=("p1",)), SimpleNamespace(number=0, parts=("p2",)))

    @property
    def surface_speed_reference_frame(self):
        singular()

    def flight(self, frame):
        return SimpleNamespace(rotation=(0.0, 0.0, 0.0, 1.0), pitch=90.0, heading=90.0, roll=0.0, g_force=1.0)


class NoDeltaVStage:
    number, parts = 1, ("p1",)

    @property
    def delta_v(self):
        raise RuntimeError("Delta-v information is not available for activation stage 10.")


class LaunchPadTest(unittest.TestCase):
    def test_navball_attitude_survives_singular_surface_frame(self):
        sc = SimpleNamespace(target_docking_port=None, target_vessel=None, target_body=None,
                             transform_direction=singular)
        streams = NavigationStreams(FakeConn(), sc, PadVessel(), 10)
        for _ in range(3):  # repeated samples must neither raise nor rebuild every time
            nav = streams.sample()
            self.assertIsNotNone(nav)
            self.assertEqual(nav.pitch, 90.0)
            self.assertIsNone(nav.prograde)
            self.assertIsNone(nav.speed)

    def test_resources_survive_stage_without_delta_v(self):
        vessel = PadVessel()
        vessel.stages = (NoDeltaVStage(),)
        streams = SystemsStreams(FakeConn(), vessel)
        first = streams.sample()
        key = streams.key
        second = streams.sample()
        self.assertEqual(streams.key, key)  # unknown delta-v does not force a rebuild
        for systems in (first, second):
            self.assertEqual(systems.stages[0].parts, 1)
            self.assertIsNone(systems.stages[0].delta_v)
            self.assertEqual(systems.resources[0].fraction, 0.5)


if __name__ == "__main__":
    unittest.main()
