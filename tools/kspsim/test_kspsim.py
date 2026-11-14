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


class ManualValuesTest(unittest.TestCase):
    def test_manual_values_reach_the_board_packet_and_freeze_the_physics(self):
        sim = Simulator("orbit")
        sim.start()
        try:
            sim.set_manual_mode(True)
            with sim.lock:
                x0 = sim.vessel.x
            sim.set_manual_values(vertical_speed=123.0, apoapsis=1_234_000.0, time_to_apoapsis=float("inf"),
                                  situation="flying")
            time.sleep(0.3)
            with sim.lock:
                self.assertEqual(sim.vessel.x, x0)  # frozen
            r = sim.reported()
            self.assertEqual((r["vertical_speed"], r["situation"]), (123.0, "flying"))
            f = dict(zip(arduino.VESSEL_FIELDS, arduino.VESSEL_DATA.unpack(arduino.encode_vessel(sim.vessel, 0.2, r))))
            self.assertAlmostEqual(f["VVI"], 123.0, places=3)
            self.assertAlmostEqual(f["AP"], 1_234_000.0, delta=1)
            self.assertEqual(f["TAp"], 0)  # inf is never put on the wire
            with self.assertRaises(ValueError):
                sim.set_manual_values(situation="hovering")
            with self.assertRaises(ValueError):
                sim.set_manual_values(warp_drive=1.0)
            sim.set_manual_mode(False)
            self.assertNotEqual(sim.reported()["vertical_speed"], 123.0)
        finally:
            sim.stop()


def _fields(sim):
    return arduino.unpack_vessel(arduino.encode_vessel(sim.vessel, 0.2, sim.reported()))


def _press(sim, flag):
    """The firmware's SAS / RCS button: each press flips the bit the board sends, and it stays there."""
    sim.board_controls(physics.Controls(**{**vars(sim.last_board), flag: not getattr(sim.last_board, flag)}))


class GameStateTest(unittest.TestCase):
    def test_game_state_reaches_vessel_data(self):
        sim = Simulator("orbit")
        sim.set_game(sas=True, lights=True, abort=True)
        sim.set_game_group(1, True)
        sim.set_game_group(10, True)
        sim.sas_mode = 3
        f = _fields(sim)
        self.assertEqual(f["ActionGroups"], 1 | 1 << 2 | 1 << 5 | 1 << 6 | 1 << 15)
        self.assertEqual(f["NavballSASMode"] & 0x0F, 3)
        sim.set_game(sas=False)
        self.assertEqual(_fields(sim)["NavballSASMode"] & 0x0F, 0)  # low nibble only while SAS is on
        with self.assertRaises(ValueError):
            sim.set_game(warp=True)
        with self.assertRaises(ValueError):
            sim.set_game_group(11, True)

    def test_window_switch_sets_the_game_without_a_board(self):
        sim = Simulator("orbit")
        sim.set_manual(rcs=True)
        self.assertEqual(sim.action_groups(), 2)

    def test_board_buttons_toggle_like_the_plugin(self):
        sim = Simulator("orbit")
        sim.board_controls(physics.Controls())  # the first packet of a link only seeds
        _press(sim, "sas")
        self.assertTrue(sim.game["sas"])
        _press(sim, "sas")
        self.assertFalse(sim.game["sas"])
        _press(sim, "rcs")
        self.assertTrue(sim.game["rcs"] and not sim.game["sas"])
        # A level that does not change is not applied again: the game's own toggle survives the keep-alives
        sim.set_game(rcs=False)
        sim.board_controls(sim.last_board)
        self.assertFalse(sim.game["rcs"])
        # ... and the board's next press still works (0.19.3.14: any bit change toggles), whatever the game did
        _press(sim, "rcs")
        self.assertTrue(sim.game["rcs"])
        _press(sim, "rcs")
        self.assertFalse(sim.game["rcs"])

    def test_first_packet_of_a_link_only_seeds_the_toggle_bits(self):
        sim = Simulator("orbit")
        sim.board_controls(physics.Controls(sas=True, rcs=True))  # bits kept by the board from a past session
        self.assertFalse(sim.game["sas"] or sim.game["rcs"])
        sim.board_linked()
        sim.board_controls(physics.Controls(sas=False))  # re-handshake: seeds again, no toggle
        self.assertFalse(sim.game["sas"])
        _press(sim, "sas")
        self.assertTrue(sim.game["sas"])

    def test_action_group_switch_is_a_level(self):
        sim = Simulator("orbit")
        sim.board_controls(physics.Controls())  # the first packet of a link only seeds
        sim.board_controls(physics.Controls(groups=1 << 1))
        self.assertEqual(sim.action_groups() >> 6 & 1, 1)
        sim.set_game_group(1, False)  # changed in the game: kept until the switch moves
        sim.board_controls(physics.Controls(groups=1 << 1))
        self.assertEqual(sim.action_groups() >> 6 & 1, 0)
        sim.board_controls(physics.Controls(groups=0))
        sim.board_controls(physics.Controls(groups=1 << 1))
        self.assertEqual(sim.action_groups() >> 6 & 1, 1)

    def test_forced_values_light_the_caution_and_warning_leds(self):
        sim = Simulator("orbit")
        e = arduino.expected_leds(_fields(sim))
        self.assertEqual((e["yellow"], e["red"]), ([], []))
        for key, value, yellow, red in (("g_force", 6.0, True, False), ("g_force", 9.5, True, True),
                                        ("stage_fuel", 40.0, True, False), ("stage_fuel", 3.0, True, True)):
            sim.forced.clear()
            sim.set_forced(key, value)
            e = arduino.expected_leds(_fields(sim))
            self.assertEqual((bool(e["yellow"]), bool(e["red"])), (yellow, red), (key, value))
        sim.forced.clear()
        sim.set_manual_mode(True)
        sim.set_manual_values(g_force=1.0)
        sim.set_forced("g_force", 7.0)  # forced wins over manual mode
        self.assertAlmostEqual(_fields(sim)["G"], 7.0, places=4)
        sim.set_forced("max_overheat", 80)
        self.assertEqual(_fields(sim)["MaxOverHeat"], 80)
        sim.set_forced("g_force", None)
        self.assertAlmostEqual(_fields(sim)["G"], 1.0, places=4)
        with self.assertRaises(ValueError):
            sim.set_forced("altitude", 1.0)

    def test_led_mirrors(self):
        sim = Simulator("orbit")
        sim.set_game(rcs=True)
        sim.set_game_group(1, True)
        e = arduino.expected_leds(_fields(sim))
        self.assertEqual((e["sas"], e["rcs"], e["cg1"]), (False, True, True))


class BoardPacketTest(unittest.TestCase):
    def test_display_command_frame(self):
        raw = arduino.display_frame(33)
        self.assertEqual(raw, bytes([0xBE, 0xEF, 2, 2, 33, 2 ^ 2 ^ 33]))
        self.assertEqual(arduino.FrameParser().feed(raw), [bytes([2, 33])])
        self.assertEqual(dict(arduino.DISPLAY_SCREENS)["Automatic"], 255)
        self.assertEqual(sorted(s for _, s in arduino.DISPLAY_SCREENS),
                         [0, 1, 2, 3, 4, 5, 16, 17, 18, 19, 20, 32, 33, 34, 35, 36, 64, 255])
        with self.assertRaises(ValueError):
            arduino.display_frame(256)

    def test_corrupt_frames(self):
        p = arduino.FrameParser()
        self.assertEqual(p.feed(arduino.bad_checksum_frame()), [])
        self.assertEqual(p.bad, 1)
        bad_size = p.feed(arduino.bad_size_frame())
        self.assertEqual(len(bad_size), 1)
        self.assertEqual(bad_size[0][0], arduino.ID_VESSEL_DATA)
        self.assertNotEqual(len(bad_size[0]), arduino.VESSEL_DATA.size)  # the firmware drops it: rx_bad_size

    def test_firmware_constants_match_the_sketch(self):
        import re
        sketch = os.path.join(ROOT, "arduino-sketch", "KSPIODemo9")
        with open(os.path.join(sketch, "KSPIODemo9.ino"), encoding="utf-8") as fh:
            src = fh.read()
        d = {m.group(1): float(m.group(2)) for m in re.finditer(r"^#define (\w+) ([\d.]+)", src, re.M)}
        self.assertEqual((d["GCAUTION"], d["GWARN"]), (arduino.GCAUTION, arduino.GWARN))
        self.assertEqual((d["FUELCAUTION"], d["FUELWARN"]), (arduino.FUELCAUTION, arduino.FUELWARN))
        self.assertEqual(d["IDLETIMER"] / 1000, arduino.IDLETIMER_S)
        with open(os.path.join(sketch, "Input.ino"), encoding="utf-8") as fh:
            mx = fh.read()
        self.assertIn("forcedUntil = now + %d;" % (arduino.DISPLAY_FORCE_S * 1000), mx)
        self.assertIn("#define LINKRESETMS %dUL" % (arduino.LINKRESET_S * 1000), src)

    def test_debug_packet_decode(self):
        cols = bytes([0x01, 0x80] + [0] * 10)
        raw = arduino.DEBUG.pack(102, 12_345, 999, 10, 2, 1, 50, 800, 120, 40, 1 | 1 << 9, 1 | 4 | 8 | 16, 3, 0, 512, cols)
        self.assertEqual(len(raw), 52)
        d = arduino.decode_debug(raw)
        self.assertEqual((d["uptime_ms"], d["loops"], d["rx_ok"], d["rx_bad_cs"], d["rx_bad_size"]), (12_345, 999, 10, 2, 1))
        self.assertEqual((d["tx"], d["loop_max_us"], d["tx_max_us"], d["since_rx_ms"], d["a0"]), (50, 800, 120, 40, 512))
        self.assertEqual(d["reset_causes"], ["power on", "watchdog"])
        self.assertEqual(d["flag_names"], ["connected", "SAS pin high", "watchdog on", "ever connected"])
        rows = arduino.matrix_rows(d["matrix"])
        self.assertEqual(rows[0], "#" + "." * 11)
        self.assertEqual(rows[7], ".#" + "." * 10)
        self.assertIn("bad checksum 2", arduino.debug_summary(d))


class FakeBoard:
    """A serial port with the firmware's handshake reply and a DebugPacket on demand; records every frame."""

    def __init__(self):
        import threading
        self.lock = threading.Lock()
        self.rx = bytearray()
        self.frames = []
        self.parser = arduino.FrameParser()
        self.answer = True

    def write(self, data):
        with self.lock:
            for p in self.parser.feed(data):
                self.frames.append(p)
                if p == bytes([0, 1, 2, 3]) and self.answer:
                    self.rx += arduino.frame(bytes([0, 3, 1, 4]))

    def read(self, n):
        with self.lock:
            out, self.rx[:] = bytes(self.rx[:n]), self.rx[n:]
        if not out:
            time.sleep(0.01)
        return out

    def reset_input_buffer(self):
        pass

    def inject(self, payload):
        with self.lock:
            self.rx += arduino.frame(payload)

    def ids(self):
        with self.lock:
            return [p[0] for p in self.frames]


class BoardLinkTest(unittest.TestCase):
    def test_link_tests_with_a_fake_board(self):
        import threading
        sim = Simulator("orbit")
        link = arduino.BoardLink(sim, "FAKE", refresh=0.05)
        ser = FakeBoard()
        t = threading.Thread(target=link._session, args=(ser, "FAKE"), daemon=True)
        t.start()

        def wait(pred, timeout=3.0):
            end = time.monotonic() + timeout
            while time.monotonic() < end:
                if pred():
                    return True
                time.sleep(0.02)
            return False

        try:
            self.assertTrue(wait(lambda: link.status.state == "linked" and arduino.ID_VESSEL_DATA in ser.ids()))
            self.assertTrue(link.send_display(4).startswith("queued"))
            self.assertTrue(wait(lambda: bytes([2, 4]) in ser.frames))
            link.send_bad_checksum()
            self.assertTrue(wait(lambda: any("wrong checksum" in e for e in link.events)))
            cols = bytes(range(12))
            ser.inject(arduino.DEBUG.pack(102, 5000, 1, 2, 1, 0, 3, 4, 5, 6, 1, 1, 0, 0, 0, cols))
            self.assertTrue(wait(lambda: link.status.debug is not None))
            self.assertEqual(link.status.debug["matrix"], cols)
            ser.inject(arduino.DEBUG.pack(102, 100, 1, 2, 1, 0, 3, 4, 5, 6, 1 << 9, 1, 0, 0, 0, cols))
            self.assertTrue(wait(lambda: any("REBOOTED" in e and "watchdog" in e for e in link.events)))

            link.pause_telemetry(True)
            time.sleep(0.15)
            n = len(ser.frames)
            link.send_display(1)
            time.sleep(0.4)
            self.assertEqual(len(ser.frames), n)  # nothing at all reaches the board while paused
            self.assertIn("PAUSED", link.status.detail)
            self.assertTrue(any("dropped" in e for e in link.events))
            link.pause_telemetry(False)
            self.assertTrue(wait(lambda: len(ser.frames) > n))

            link.hold_screen = 2
            self.assertTrue(wait(lambda: bytes([2, 2]) in ser.frames))
            link.hold_screen = None

            handshakes = ser.ids().count(0)
            link.rehandshake()
            self.assertTrue(wait(lambda: any("re-handshake answered" in e for e in link.events)))
            self.assertEqual(ser.ids().count(0), handshakes + 1)
        finally:
            link._stop.set()
            t.join(timeout=3)

    def test_commands_refused_while_not_linked(self):
        link = arduino.BoardLink(Simulator("orbit"), "NOPE")
        self.assertTrue(link.send_display(0).startswith("not sent"))


class KrpcTest(unittest.TestCase):
    def test_hmi_client_end_to_end(self):
        from kspsim.krpcserver import KrpcServer
        from ksphmi.telemetry import FLIGHT, NO_VESSEL, CONNECTING, KrpcSource

        sim = Simulator("orbit")
        sim.start()
        server = KrpcServer(sim, "127.0.0.1", 0, 0, context_port=0)
        sim.listeners.append(lambda scene: server.stop() if scene == "closed" else server.start())
        server.start()
        src = KrpcSource("127.0.0.1", server.rpc_port, server.stream_port, rate_hz=20, retry_seconds=0.5)
        src.context_port = server.context_port
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

            sim.set_manual_mode(True)  # typed values reach the HMI client exactly
            sim.set_manual_values(vertical_speed=-42.5, periapsis=-12_000.0, situation="sub_orbital")
            s = wait(lambda s: s.vertical_speed == -42.5)
            self.assertEqual((s.periapsis, s.situation), (-12_000.0, "sub_orbital"))
            sim.set_manual_mode(False)

            sim.set_scene("space_center")
            wait(lambda s: s.state == NO_VESSEL and s.scene == "space_center")
            from ksphmi.scenes import VIEWS
            for scene in VIEWS:
                sim.set_scene(scene)
                wait(lambda s: s.state == NO_VESSEL and s.scene == scene)
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
