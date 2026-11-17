"""Tests of the simulator, no board needed: ``./ci test_sim``.

The kRPC test runs the stock krpc 0.7 Python client against the stand-in server on free local ports, over the
procedures the C++ HMI reads; ./ci test_hmi --sim runs the HMI itself against the simulator.
"""
import math
import os
import re
import sys
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path[:0] = [os.path.dirname(HERE)]

from kspsim import arduino, physics  # noqa: E402
from kspsim.sim import Simulator  # noqa: E402


class KeplerTest(unittest.TestCase):
    def test_propagate_returns_after_one_period_and_conserves_energy(self):
        v = physics.scenario("orbit")
        el = v.elements()
        x, y, vx, vy = physics.propagate(v.x, v.y, v.vx, v.vy, el.period)
        self.assertLess(math.hypot(x - v.x, y - v.y), 1.0)
        for dt in (100.0, -100.0, 5000.0):
            x, y, vx, vy = physics.propagate(v.x, v.y, v.vx, v.vy, dt)
            self.assertAlmostEqual(physics.elements(x, y, vx, vy).a, el.a, delta=1)
        r0, v0 = (v.x, 0.0, v.y), (v.vx, 30.0, v.vy)  # 3D: energy and extremes kept
        r, w = physics.propagate3(r0, v0, 777)
        self.assertAlmostEqual(physics.extremes(r, w)[3], physics.extremes(r0, v0)[3], delta=1)
        e = physics.scenario("escape")  # hyperbolic: still propagates, energy kept
        x, y, vx, vy = physics.propagate(e.x, e.y, e.vx, e.vy, 3000)
        self.assertAlmostEqual(physics.elements(x, y, vx, vy).a, e.elements().a, delta=abs(e.elements().a) * 1e-6)


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
    def test_editor_link_keeps_neutral_telemetry_and_routes_button_frames(self):
        import threading
        sim = Simulator("orbit")
        sim.set_scene("vab")
        before = dict(sim.game)
        link = arduino.BoardLink(sim, "FAKE", refresh=.05)
        serial = FakeBoard()
        worker = threading.Thread(target=link._session, args=(serial, "FAKE"), daemon=True)
        worker.start()

        def wait(predicate):
            end = time.monotonic() + 3
            while time.monotonic() < end:
                if predicate():
                    return True
                time.sleep(.01)
            return False

        def controls(bits):
            serial.inject(arduino.CONTROL.pack(101, bits, 0, 0, 0, 0, *([0] * 9)))

        try:
            self.assertTrue(wait(lambda: serial.ids().count(1) >= 2), "VAB must keep sending the board heartbeat")
            controls(0)
            self.assertTrue(wait(lambda: link.status.received >= 1))
            controls(128)
            self.assertTrue(wait(lambda: sim.editor_mode == "actions"))
            controls(0)
            self.assertTrue(wait(lambda: sim.editor_mode == "crew"))
            controls(64)
            self.assertTrue(wait(lambda: sim.editor_mode == "actions"))
            self.assertEqual(sim.game, before)
            telemetry = [payload for payload in serial.frames if payload[0] == 1]
            self.assertTrue(telemetry)
            self.assertTrue(all(payload == bytes([1]) + bytes(213) for payload in telemetry))
        finally:
            link._stop.set()
            worker.join(timeout=3)
        self.assertFalse(worker.is_alive())

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
    """The stand-in server against the stock krpc 0.7 Python client: the procedures and streams the C++ HMI reads
    (hmi-cpp/src/telemetry.cpp). The C++ client itself runs against kspsim in ./ci test_hmi --sim (Linux/WSL)."""

    @staticmethod
    def context(port, request=b"KSPHMI/1"):
        import socket
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.settimeout(1.0)
            sock.sendto(request, ("127.0.0.1", port))
            return sock.recv(256).decode("ascii")

    def test_context_reply_carries_the_simulator_skin(self):
        from kspsim.krpcserver import KrpcServer
        from kspsim.sim import SKINS

        sim = Simulator("orbit")
        server = KrpcServer(sim, "127.0.0.1", 0, 0, context_port=0)
        server.start()
        try:
            self.assertEqual(self.context(server.context_port), "KSPHMI/1 flight")  # as KSPSerialIO: no field
            for skin in ("apollo", "classic"):
                sim.set_skin(skin)
                self.assertEqual(self.context(server.context_port), "KSPHMI/1 flight skin=" + skin)
            sim.set_scene("vab")
            self.assertEqual(self.context(server.context_port), "KSPHMI/1 vab skin=classic")
            sim.set_skin("")
            self.assertEqual(self.context(server.context_port), "KSPHMI/1 vab")
            # every skin the simulator offers is one the HMI knows (hmi-cpp/src/display.cpp known_skin)
            with open(os.path.join(ROOT, "hmi-cpp", "src", "display.cpp"), encoding="utf-8") as f:
                display = f.read()
            for skin in SKINS[1:]:
                self.assertIn('name == "%s"' % skin, display)
            with self.assertRaises(AssertionError):
                sim.set_skin("chrome")
        finally:
            server.stop()

    def test_mun_transfer_finds_the_encounter_and_the_closest_approach(self):
        from kspsim import krpcserver as ks
        r0 = physics.RADIUS + 100_000
        a = (r0 + ks.MUN_ORBIT_RADIUS) / 2
        transfer = math.pi * math.sqrt(a ** 3 / physics.MU)
        n = math.sqrt(physics.MU / ks.MUN_ORBIT_RADIUS ** 3)
        # Leave at angle 0 so that the Mun reaches the apoapsis (angle pi) when the vessel does.
        t0 = ((math.pi - ks.MUN_PHASE) / n - transfer) % (2 * math.pi / n)
        v0 = math.sqrt(physics.MU * (2 / r0 - 1 / a))
        events = ks.path_events(t0, (r0, 0.0, 0.0), (0.0, 0.0, v0))
        self.assertIn('soi', events)
        self.assertLess(events['soi'], t0 + transfer)
        self.assertGreater(events['soi'], t0 + transfer * .5)  # the sphere is 2.4 Mm wide
        p, _ = physics.propagate3((r0, 0.0, 0.0), (0.0, 0.0, v0), events['soi'] - t0)
        self.assertAlmostEqual(math.dist(p, ks.mun_state(events['soi'])[0]), ks.MUN_SOI, delta=10)
        p, _ = physics.propagate3((r0, 0.0, 0.0), (0.0, 0.0, v0), events['approach'] - t0)
        self.assertLess(math.dist(p, ks.mun_state(events['approach'])[0]), ks.MUN_SOI)
        # Low orbit: no encounter, an approach somewhere ahead within one period.
        low = ks.path_events(0, (r0, 0.0, 0.0), (0.0, 0.0, math.sqrt(physics.MU / r0)))
        self.assertNotIn('soi', low)
        self.assertGreaterEqual(low['approach'], 0)

    def test_cockpit_panel_procedures(self):
        """What the HMI cockpit panel reads and clicks (hmi-cpp/src/telemetry.cpp CockpitStreams, send_command)."""
        import krpc
        from kspsim.krpcserver import KrpcServer

        sim = Simulator("orbit")
        server = KrpcServer(sim, "127.0.0.1", 0, 0, context_port=0)
        server.start()
        conn = krpc.connect(name="test", address="127.0.0.1", rpc_port=server.rpc_port, stream_port=server.stream_port)
        try:
            vessel = conn.space_center.active_vessel
            control, parts = vessel.control, vessel.parts
            self.assertEqual((len(parts.solar_panels), len(parts.radiators), len(parts.legs)), (1, 0, 0))
            self.assertFalse(control.solar_panels)
            control.solar_panels = True
            self.assertTrue(control.solar_panels and sim.panel["SolarPanels"])
            self.assertTrue(control.reaction_wheels)
            control.set_action_group(3, True)
            self.assertTrue(control.get_action_group(3))
            self.assertFalse(control.get_action_group(0))  # kRPC group 0 = custom group 10
            control.speed_mode = conn.space_center.SpeedMode.surface
            self.assertEqual(control.speed_mode.name, "surface")
            chutes = parts.parachutes
            self.assertEqual([c.state.name for c in chutes], ["stowed", "stowed"])
            self.assertEqual(chutes[0].safe_state.name, "unsafe")  # in orbit
            control.parachutes = True
            self.assertEqual([c.state.name for c in parts.parachutes], ["semi_deployed", "semi_deployed"])
            parts.parachutes[0].cut()
            self.assertEqual([c.state.name for c in parts.parachutes], ["cut", "semi_deployed"])
            current = control.current_stage
            nxt = [p.title for p in parts.all if p.stage == current - 1]
            self.assertTrue(nxt and all(t.startswith("Simulated part") for t in nxt))
            self.assertEqual(conn.space_center.warp_rate, sim.warp)
        finally:
            conn.close()
            server.stop()

    def test_mechjeb_procedures(self):
        """The HMI MechJeb scene's procedures (hmi-cpp/src/telemetry.cpp MechJebPoller, send_mechjeb): the report
        starts as the shared fixture, MechJebSet validates like the plugin, actions change ascent.enabled/status."""
        import krpc
        from kspsim.krpcserver import KrpcServer
        from kspsim.mechjeb import fixture_values

        sim = Simulator("orbit")
        server = KrpcServer(sim, "127.0.0.1", 0, 0, context_port=0)
        server.start()
        conn = krpc.connect(name="test", address="127.0.0.1", rpc_port=server.rpc_port, stream_port=server.stream_port)
        try:
            names = {"MechJebRevision": "mech_jeb_revision", "MechJebReport": "mech_jeb_report",
                     "MechJebSet": "mech_jeb_set", "MechJebAction": "mech_jeb_action"}

            def call(name, *args):
                return getattr(conn.ksphmi_flight, names[name])(*args)

            with open(os.path.join(ROOT, "hmi-cpp", "tests", "data", "mechjeb_report_v1.txt"), encoding="utf-8") as f:
                fixture = f.read().replace("\r\n", "\n")
            self.assertEqual(call("MechJebReport"), fixture)  # byte for byte at start
            self.assertEqual(fixture_values()[1]["ascent.status"], "Awaiting liftoff")
            rev = call("MechJebRevision")
            self.assertEqual(rev, 42)
            self.assertEqual(call("MechJebSet", "ascent.altitude", "90000"), "ok")
            self.assertEqual(call("MechJebRevision"), rev + 1)
            self.assertIn("\nascent.altitude=90000\n", call("MechJebReport"))
            self.assertEqual(call("MechJebSet", "ascent.altitude", "90000"), "ok")
            self.assertEqual(call("MechJebRevision"), rev + 1)  # unchanged text: same revision
            self.assertEqual(call("MechJebSet", "ascent.classic.turn_shape_exponent", "0.45"), "ok")
            self.assertIn("\nascent.classic.turn_shape_exponent=0.45\n", call("MechJebReport"))
            self.assertEqual(call("MechJebSet", "ascent.no_such", "1"), "unknown key")
            self.assertEqual(call("MechJebSet", "ascent.enabled", "0"), "read only")
            for key, value in (("ascent.altitude", "abc"), ("ascent.altitude", "nan"), ("ascent.force_roll", "2"),
                               ("ascent.path", "3"), ("ascent.last_stage", "1.5")):
                self.assertEqual(call("MechJebSet", key, value), "invalid value", (key, value))
            self.assertEqual(call("MechJebSet", "ascent.path", "2"), "ok")
            self.assertEqual(call("MechJebAction", "ascent.disengage"), "ok")
            self.assertIn("\nascent.enabled=0\n", call("MechJebReport"))
            self.assertEqual(call("MechJebAction", "ascent.launch_rendezvous"), "ok")
            report = call("MechJebReport")
            self.assertIn("\nascent.enabled=1\n", report)
            self.assertIn("\nascent.t_minus=30\n", report)
            self.assertIn("\nascent.status=Launching%20to%20rendezvous\n", report)
            self.assertEqual(call("MechJebAction", "ascent.abort_timed_launch"), "ok")
            self.assertIn("\nascent.t_minus=0\n", call("MechJebReport"))
            self.assertEqual(call("MechJebAction", "ascent.fly_me_to_the_mun"), "unknown key")
            sim.mechjeb.set_state("nocore")
            self.assertEqual(call("MechJebReport"), "v=1 rev=%d mj=nocore version=2.15.3.0\n" % sim.mechjeb.revision)
            self.assertEqual(call("MechJebSet", "ascent.altitude", "80000"), "unavailable")
            sim.mechjeb.set_state("missing")
            self.assertTrue(call("MechJebReport").endswith("mj=missing\n"))
            self.assertEqual(call("MechJebAction", "ascent.engage"), "unavailable")
            sim.set_scene("vab")
            with self.assertRaises(Exception):
                call("MechJebRevision")
        finally:
            conn.close()
            server.stop()

    def test_krpc_client_end_to_end(self):
        import krpc
        from kspsim.krpcserver import KrpcServer

        sim = Simulator("orbit")
        sim.start()
        server = KrpcServer(sim, "127.0.0.1", 0, 0, context_port=0)
        sim.listeners.append(lambda scene: server.stop() if scene == "closed" else server.start())
        server.start()
        rpc_port, stream_port = server.rpc_port, server.stream_port

        def connect():
            return krpc.connect(name="test", address="127.0.0.1", rpc_port=rpc_port, stream_port=stream_port)

        def wait(pred, timeout=10.0):
            end = time.monotonic() + timeout
            while time.monotonic() < end:
                if pred():
                    return
                time.sleep(0.05)
            self.fail("timeout")

        conn = connect()
        try:
            sc = conn.space_center
            vessel = sc.active_vessel
            orbit = vessel.orbit
            body = orbit.body
            self.assertEqual((vessel.name, body.name, vessel.situation.name), ("Orbiter", "Kerbin", "orbiting"))
            self.assertAlmostEqual(orbit.apoapsis_altitude, 250_000, delta=5)
            self.assertAlmostEqual(orbit.periapsis_altitude, 80_000, delta=5)
            self.assertAlmostEqual(math.degrees(orbit.inclination), 0.4, places=6)
            name = conn.add_stream(getattr, vessel, "name")
            altitude = conn.add_stream(getattr, vessel.flight(body.reference_frame), "mean_altitude")
            wait(lambda: altitude() > 70_000)

            sim.set_manual_mode(True)
            sim.set_manual_values(pitch=35, heading=135, roll=-45, speed_mode=2, target_enabled=1, relative_up=3,
                                  relative_north=0, relative_east=4, node_enabled=1, node_dv=200, node_remaining=125,
                                  node_time=240, node_prograde=200, node_normal=0, node_radial=0)
            surface = vessel.flight(vessel.surface_reference_frame)
            self.assertEqual((surface.pitch, surface.heading, surface.roll), (35, 135, -45))
            self.assertEqual(vessel.control.speed_mode.name, "target")
            self.assertAlmostEqual(vessel.flight(vessel.target_speed_reference_frame).speed, 5)
            self.assertEqual(len(vessel.control.nodes), 1)
            self.assertEqual(vessel.control.nodes[0].remaining_delta_v, 125)
            # Node.orbit: the post-burn state at the node, in the frame of the current orbit (the HMI's dashed orbit)
            node = vessel.control.nodes[0]
            frame = body.non_rotating_reference_frame
            self.assertAlmostEqual(node.ut - sc.ut, 240, delta=1)
            self.assertEqual((node.prograde, node.normal, node.radial), (200, 0, 0))
            after = node.orbit
            self.assertEqual(after.body.name, "Kerbin")
            r = sim.reported()
            x, y, vx, vy = physics.propagate(r["position_x"], r["position_z"], r["velocity_x"], r["velocity_z"], 240)
            position = after.position_at(node.ut, frame)
            velocity = after.velocity_at(node.ut, frame)
            self.assertLess(math.dist(position, (x, 0, y)), 5_000)  # the node lies on the current orbit
            self.assertAlmostEqual(math.hypot(*velocity), math.hypot(vx, vy) + 200, delta=5)
            self.assertGreater(after.apoapsis_altitude, r["apoapsis"] + 100_000)  # a prograde burn raises Ap
            self.assertAlmostEqual(after.periapsis, after.periapsis_altitude + physics.RADIUS, delta=1)
            self.assertGreater(body.sphere_of_influence, 8e7)

            sim.set_manual_values(electric_charge=0, electric_capacity=200, liquid_fuel=10, liquid_capacity=100,
                                  monoprop_capacity=0, stage_number=1, stage_delta_v=450, stage_parts=3)
            resources = vessel.resources
            self.assertEqual(resources.amount("ElectricCharge"), 0)
            self.assertEqual(resources.max("ElectricCharge"), 200)
            self.assertAlmostEqual(resources.amount("LiquidFuel") / resources.max("LiquidFuel"), .1, places=6)
            self.assertEqual(resources.max("MonoPropellant"), 0)
            self.assertEqual(vessel.control.current_stage, 1)
            self.assertEqual(sorted(s.number for s in vessel.stages), [0, 1])
            # The target body is the Mun (ORBIT: its orbit, the closest approach and Mun encounters)
            mun = sc.target_body
            self.assertEqual((mun.name, mun.orbit.body.name), ("Mun", "Kerbin"))
            self.assertAlmostEqual(math.hypot(*mun.orbit.position_at(sc.ut, frame)), 12e6, delta=1)
            approach = after.next_closest_approach(mun.orbit)
            self.assertGreaterEqual(approach.ut, node.ut - 1)
            self.assertAlmostEqual(approach.distance, math.dist(approach.position(frame), approach.target_position(frame)),
                                   delta=1)
            self.assertTrue(math.isnan(orbit.time_to_soi_change))
            self.assertIsNone(orbit.next_orbit)
            self.assertIsNone(body.orbit)
            sim.set_manual_values(target_enabled=0, node_enabled=0, speed_mode=1)
            self.assertIsNone(sc.target_vessel)
            self.assertEqual(len(vessel.control.nodes), 0)
            sim.set_manual_mode(False)

            # The window's Maneuver node tab (Simulator.set_node, also --node): a node on the running flight model
            sim.set_node(150, 600)
            self.assertEqual(vessel.control.nodes[0].orbit.body.name, "Kerbin")
            ut = vessel.control.nodes[0].ut
            self.assertAlmostEqual(ut - sc.ut, 600, delta=1)
            time.sleep(0.3)
            self.assertEqual(vessel.control.nodes[0].ut, ut)  # a fixed UT: the node counts down
            sim.set_node(100, 400, normal=50, radial=-20)  # update: all three axes reach Node and Node.orbit
            node = vessel.control.nodes[0]
            self.assertEqual((node.prograde, node.normal, node.radial), (100, 50, -20))
            self.assertAlmostEqual(node.delta_v, math.sqrt(100 ** 2 + 50 ** 2 + 20 ** 2), places=6)
            before = sim.reported()
            r0, v0 = physics.propagate3(tuple(before["position_" + a] for a in "xyz"),
                                        tuple(before["velocity_" + a] for a in "xyz"), node.ut - sc.ut)
            velocity = node.orbit.velocity_at(node.ut, frame)
            self.assertAlmostEqual(abs(velocity[1]), 50, delta=1)  # the normal part leaves the planar orbit
            self.assertAlmostEqual(math.dist(velocity, v0), node.delta_v, delta=2)
            preview = sim.node_preview()
            self.assertAlmostEqual(preview["periapsis"], node.orbit.periapsis_altitude, delta=1)
            sim.set_node(None)  # delete: Node and Node.orbit disappear
            self.assertEqual(len(vessel.control.nodes), 0)
            self.assertIsNone(sim.node_preview())

            sim.set_vessel(name="Renamed")  # streamed: the HMI shows a rename at once
            wait(lambda: name() == "Renamed")

            # every scene the HMI has a view for is served on the UDP context channel; elsewhere no vessel
            with open(os.path.join(ROOT, "hmi-cpp", "src", "scenes.cpp"), encoding="utf-8") as f:
                views = re.findall(r'^        \{"(\w+)", "', f.read(), re.M)
            self.assertIn("space_center", views)
            for scene in views:
                sim.set_scene(scene)
                self.assertEqual(self.context(server.context_port), "KSPHMI/1 " + scene)
                self.assertIsNone(sc.active_vessel)
            sim.set_scene("flight")
            sim.load("escape")  # a new vessel: a client must re-open its streams on it
            wait(lambda: sc.active_vessel is not None and sc.active_vessel.name == "Voyager")
            self.assertGreater(sc.active_vessel.orbit.eccentricity, 1)
            self.assertEqual(sc.active_vessel.situation.name, "escaping")
        finally:
            conn.close()

        try:
            sim.set_scene("closed")  # KSP closed: the ports refuse connections
            self.assertRaises(OSError, connect)
            sim.set_scene("flight")  # the ports reopen; a client reconnects
            conn = connect()
            self.assertEqual(conn.space_center.active_vessel.name, "Voyager")
            conn.close()
        finally:
            server.stop()
            sim.stop()


def _point(v, pitch, heading, roll=0.0):
    """Steer the vessel to this attitude (degrees, NavBall), at rest."""
    f, t = physics.attitude_axes(pitch, heading, roll)
    v.forward, v.top, v.omega, v.steered = v.from_une(f), v.from_une(t), (0.0, 0.0, 0.0), True


def _sas_report(v):
    """A reported dict with a target and a maneuver node, so every SAS mode has a direction."""
    return dict(v.report(), target_enabled=1.0, target_up=0.3, target_north=-0.5, target_east=0.8,
                node_enabled=1.0, burn_up=0.2, burn_north=0.6, burn_east=-0.7)


class ShipReportTest(unittest.TestCase):
    """KSPSerialIO.Hmi's KSPHMI service as kspsim serves it: same text as the plugin, same rules as kRPC."""

    def test_editor_fixture_and_serial_navigation_are_separate_from_flight_controls(self):
        from kspsim import ships
        sample = ships.editor_sample(ships.ship("kerbal_x"), "actions")
        with open(os.path.join(ROOT, "hmi-cpp", "tests", "data", "editor_report_v1.txt"), encoding="utf-8") as file:
            self.assertEqual(ships.report_text(57, sample), file.read())
        sim = Simulator()
        sim.set_scene("vab")
        before_game = dict(sim.game)
        before_controls = dict(vars(sim.vessel.controls))
        self.assertIsNotNone(sim.vessel_for_board())
        sim.board_linked()
        sim.board_controls(physics.Controls())
        sim.board_controls(physics.Controls(sas=True, stage=True, throttle=1))
        self.assertEqual(sim.editor_mode, "actions")
        sim.board_controls(physics.Controls(sas=False))
        self.assertEqual(sim.editor_mode, "crew")
        sim.board_controls(physics.Controls(rcs=True))
        self.assertEqual(sim.editor_mode, "actions")
        sim.set_editor_mode("construction")
        sim.board_controls(physics.Controls(rcs=False))
        self.assertEqual(sim.editor_mode, "cargo")
        self.assertEqual(sim.game, before_game)
        self.assertEqual(vars(sim.vessel.controls), before_controls)
        sim.board_linked()
        sim.board_controls(physics.Controls(sas=True, rcs=True))
        self.assertEqual(sim.editor_mode, "cargo")
        self.assertIn("editor=cargo,custom1", sim.ship_report())
        revision = sim.ship_revision
        sim.set_editor_group("sas")
        self.assertGreater(sim.ship_revision, revision)
        self.assertIn("editor=cargo,sas", sim.ship_report())

    def test_sample_matches_the_plugin_format_and_the_pi_fixture(self):
        from kspsim import ships
        # hmi-cpp/tests/data/ship_report_v1.txt is also the plugin formatter's expected output
        # (tools/plugin-shipreport-tests) and the Pi parser's input (hmi-cpp/tests/test_core.cpp)
        with open(os.path.join(ROOT, "hmi-cpp", "tests", "data", "ship_report_v1.txt"), encoding="utf-8", newline="") as f:
            golden = f.read()
        self.assertEqual(ships.report_text(57, ships.ship("kerbal_x")), golden)
        self.assertEqual(ships.escape("a b=c,d%"), "a%20b%3Dc%2Cd%25")
        self.assertEqual(ships.escape("Señal"), "Se%C3%B1al")
        self.assertEqual(ships.number(float("nan"), 2), "nan")
        self.assertEqual(ships.number(-0.001, 2), "0.00")

    def test_samples_cover_pending_heavy_and_no_service(self):
        from kspsim import ships
        pending = ships.report_text(3, ships.ship("pending"))
        self.assertTrue(pending.endswith("engines=5\ndv=pending\n"), pending)
        self.assertNotIn("\nst=", pending)
        heavy = ships.report_text(4, ships.ship("heavy"))
        self.assertNotIn("funds=", heavy)  # sandbox
        launch = [line for line in heavy.splitlines() if line.startswith("st=")][0].split(",")
        self.assertLess(float(launch[3]), 1.0)  # the launch stage cannot leave the pad
        self.assertIsNone(ships.ship(""))
        with self.assertRaises(ValueError):
            ships.ship("saturn_v")

    def test_ksphmi_service_with_the_krpc_client(self):
        import krpc
        from kspsim.krpcserver import KrpcServer

        sim = Simulator("orbit")
        server = KrpcServer(sim, "127.0.0.1", 0, 0, context_port=0)
        server.start()

        def connect():
            return krpc.connect(name="test", address="127.0.0.1", rpc_port=server.rpc_port,
                                stream_port=server.stream_port)
        try:
            sim.set_scene("vab")
            self.assertEqual(KrpcTest.context(server.context_port), "KSPHMI/1 vab")
            conn = connect()
            try:
                rev = conn.ksphmi.revision()
                self.assertEqual(conn.ksphmi.ship_report(), sim.ship_report())
                self.assertTrue(conn.ksphmi.ship_report().startswith("v=1 rev=%d facility=vab name=Kerbal%%20X" % rev))
                sim.set_ship("heavy")  # every editor change is a new revision
                self.assertEqual(conn.ksphmi.revision(), rev + 1)
                self.assertIn("name=Heavy%20Lifter", conn.ksphmi.ship_report())
                self.assertEqual(conn.ksphmi.revision(), rev + 1)  # polling alone changes nothing
                # The HMI keyboard: T (SAS) next editor screen, R (RCS) previous, as the board in the VAB
                self.assertEqual(sim.editor_mode, "construction")
                conn.ksphmi.press("SAS")
                self.assertEqual(sim.editor_mode, "actions")
                conn.ksphmi.press("RCS")
                conn.ksphmi.press("RCS")
                self.assertEqual(sim.editor_mode, "cargo")
                self.assertEqual(conn.ksphmi.revision(), rev + 4)  # every mode change is a new report
                with self.assertRaises(Exception):
                    conn.ksphmi.press("STAGE")
                sim.set_scene("flight")  # GameScene.Editor: kRPC refuses the call elsewhere
                with self.assertRaises(Exception) as err:
                    conn.ksphmi.revision()
                self.assertIn("not available in game scene", str(err.exception))
                control = conn.space_center.active_vessel.control  # in flight T / R toggle the vessel's SAS / RCS
                for flag in ("sas", "rcs", "gear", "lights", "brakes"):
                    before = getattr(control, flag)
                    setattr(control, flag, not before)
                    self.assertEqual(getattr(control, flag), not before)
                    self.assertEqual(sim.game[flag], not before)
                control.toggle_action_group(0)  # key 0 = custom group 10
                self.assertTrue(sim.game_groups >> 10 & 1)
                control.sas_mode = conn.space_center.SASMode.prograde
                self.assertEqual(sim.sas_mode, 2)
                # KSPHMIFlight: held keys and the one-shot throttle keys (Z / X / Shift / Ctrl)
                flight = conn.ksphmi_flight
                flight.set_axes(-1, 0, 2, 0, 0, 0.5)
                self.assertEqual(sim.keyboard_axes, (-1.0, 0.0, 1.0, 0.0, 0.0, 0.5))
                flight.set_throttle(1)
                self.assertEqual(sim.manual.throttle, 1.0)
                flight.nudge_throttle(-0.25)
                self.assertAlmostEqual(sim.manual.throttle, 0.75, places=5)
                self.assertTrue(flight.vessel_shape().startswith("v=1 rev=12 parts=3\n"))  # the shared fixture
                self.assertEqual(flight.vessel_state(), "v=1 rev=12\ns=-1:30,48:20,-1:93\n")
                sim.set_scene("sph")
                self.assertEqual(conn.ksphmi.revision(), rev + 4)
                conn.ksphmi.press("SAS")  # the SPH ignores presses, like KSPSerialIO.Hmi
                self.assertEqual(sim.editor_mode, "cargo")
                sim.set_ship("")  # as without KSPSerialIO.Hmi.dll: the service is gone
                with self.assertRaises(Exception):
                    conn.ksphmi.revision()
            finally:
                conn.close()
            conn = connect()  # a new client no longer even sees the service
            try:
                self.assertFalse(hasattr(conn, "ksphmi"))
            finally:
                conn.close()
        finally:
            server.stop()


class AttitudeTest(unittest.TestCase):
    def test_axes_and_angles_round_trip_and_match_the_rotation(self):
        for p, h, r in ((0, 0, 0), (12, 90, -25), (-40, 300, 170), (89.9, 45, 10), (90, 90, 0), (-90, 200, 0)):
            f, t = physics.attitude_axes(p, h, r)
            p2, h2, r2 = physics.attitude_angles(f, t)
            f2, t2 = physics.attitude_axes(p2, h2, r2)
            for a, b in zip(f + t, f2 + t2):
                self.assertAlmostEqual(a, b, places=6, msg=(p, h, r))
            q = physics.attitude_rotation(p2, h2, r2)
            x, y, z, w = q  # the rotation maps the vessel's forward axis (0, 1, 0) to f
            fq = (2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x))
            for a, b in zip(f, fq):
                self.assertAlmostEqual(a, b, places=6)

    def test_unsteered_vessel_follows_the_guidance(self):
        v = physics.scenario("pad")
        self.assertAlmostEqual(v.attitude()[0], 90.0)
        v.apply_controls(physics.Controls(throttle=1.0))
        v.step(60)
        self.assertFalse(v.steered)
        ux, uy = v.guidance_direction()
        self.assertEqual(v.thrust_direction(), (ux, uy))

    def test_sas_slews_to_every_mode_and_settles(self):
        for mode in range(2, 11):
            v = physics.scenario("orbit")
            _point(v, -30, 300)  # at least 10 degrees from every target
            errors, rates = [], []
            for _ in range(int(30 / 0.1)):
                v.autopilot = v.sas_target(mode, _sas_report(v))
                self.assertIsInstance(v.autopilot, tuple, physics.SAS_MODES[mode - 1])
                v.step(0.1)
                errors.append(v.sas_error)
                rates.append(math.sqrt(sum(w * w for w in v.omega)))
            name = physics.SAS_MODES[mode - 1]
            self.assertGreater(errors[0], 10, name)
            self.assertLess(errors[10], errors[0], name)
            self.assertTrue(all(b <= a + 0.05 for a, b in zip(errors, errors[1:]) if a > 2), name)  # no overshoot
            self.assertLess(max(errors[-50:]), 0.5, name)  # settled for the last 5 s
            self.assertLess(max(rates), physics.SAS_RATE * 1.01, name)
            self.assertLess(rates[0], physics.SAS_RATE / 2, name)  # eases in

    def test_sas_mode_directions(self):
        v = physics.scenario("orbit")  # at periapsis: velocity horizontal and eastward
        r = _sas_report(v)
        for mode, (pitch, heading) in ((2, (0, 90)), (3, (0, 270)), (4, (0, 180)), (5, (0, 0)), (6, (-90, None)),
                                       (7, (90, None))):
            d = v.to_une(v.sas_target(mode, r))
            self.assertAlmostEqual(math.degrees(math.asin(d[0])), pitch, places=3)
            if heading is not None:
                self.assertAlmostEqual(math.degrees(math.atan2(d[2], d[1])) % 360, heading, places=3)
        self.assertEqual(v.sas_target(1, r), physics.HOLD)
        self.assertEqual(v.sas_target(8, v.report()), physics.HOLD)   # no target
        self.assertEqual(v.sas_target(10, v.report()), physics.HOLD)  # no node

    def test_stability_assist_stops_and_holds(self):
        v = physics.scenario("orbit")
        _point(v, 10, 30, 5)
        v.omega = (0.2, -0.1, 0.05)
        v.autopilot = physics.HOLD
        for _ in range(50):
            v.step(0.1)
        self.assertLess(math.sqrt(sum(w * w for w in v.omega)), 1e-6)
        held = v.forward
        for _ in range(20):
            v.step(0.1)
        for a, b in zip(held, v.forward):
            self.assertAlmostEqual(a, b, places=9)

    def test_stick_rotates_the_vessel(self):
        for axis, index, sign in (("pitch", 0, 1), ("yaw", 1, 1), ("roll", 2, 1)):
            v = physics.scenario("orbit")
            _point(v, 0, 90, 0)
            v.steered = False  # the stick alone takes over from the guidance
            start = v.attitude()
            v.apply_controls(physics.Controls(**{axis: 1.0}))
            for _ in range(10):
                v.step(0.1)
            self.assertTrue(v.steered)
            moved = v.attitude()
            delta = (moved[index] - start[index] + 180) % 360 - 180
            self.assertGreater(delta * sign, 5, axis)
            v.apply_controls(physics.Controls())
            for _ in range(60):
                v.step(0.1)
            self.assertLess(math.sqrt(sum(w * w for w in v.omega)), 0.01, axis)  # damped without SAS

    def test_thrust_follows_the_attitude_once_steered(self):
        v = physics.scenario("orbit")
        _point(v, 90, 90)  # radial out
        v.apply_controls(physics.Controls(throttle=1.0))
        vs0 = v.vertical_speed
        v.step(5)
        self.assertGreater(v.vertical_speed - vs0, 0.9 * physics.MAX_ACCEL * 5 - 20)
        w = physics.scenario("orbit")
        _point(w, 0, 180)  # normal: out of the plane, no effect on the planar orbit
        el = w.elements()
        w.apply_controls(physics.Controls(throttle=1.0))
        w.step(5)
        self.assertAlmostEqual(w.elements().apoapsis, el.apoapsis, delta=1.0)

    def test_simulator_sas_normal_reaches_the_board_and_krpc_values(self):
        sim = Simulator("orbit")
        sim.set_game(sas=True)
        sim.sas_mode = 4  # normal
        with self.assertLogs("kspsim", "INFO") as logs:
            for _ in range(400):
                sim.step_vessel(0.05)
        text = "\n".join(logs.output)
        self.assertIn("SAS on, mode normal", text)
        self.assertIn("SAS normal: on target", text)
        r = sim.reported()
        self.assertAlmostEqual(r["pitch"], 0, delta=1)
        self.assertAlmostEqual(r["heading"], 180, delta=1)
        f = _fields(sim)
        self.assertAlmostEqual(f["Heading"], 180, delta=1)
        self.assertAlmostEqual(f["Pitch"], 0, delta=1)
        self.assertEqual(f["NavballSASMode"] & 0x0F, 4)
        self.assertAlmostEqual(f["NormalHeading"] / 50, 180, delta=1)
        self.assertAlmostEqual(f["ProgradeHeading"] / 50, 90, delta=1)
        sim.set_game(sas=False)
        with self.assertLogs("kspsim", "INFO") as logs:
            sim.step_vessel(0.05)
        self.assertIn("SAS off", "\n".join(logs.output))


class ReduxOrbitTest(unittest.TestCase):
    """KSP2ORB1 v1 (docs/hmi.rst "KSP 2 Redux source"): the encoder against the fixtures the HMI's parser reads
    (hmi-cpp/tests/data/redux_orb1_*_v1.txt, from the KSP2 workbench's spec) and the UDP responder."""

    @staticmethod
    def fixture(name):
        with open(os.path.join(ROOT, "hmi-cpp", "tests", "data", name), encoding="ascii", newline="") as f:
            return f.read().rstrip("\r\n")

    def test_golden_bodies_round_trip(self):
        from kspsim import redux
        for name in ("redux_orb1_v1.txt", "redux_orb1_maneuver_v1.txt"):
            body = self.fixture(name)
            self.assertEqual(redux.encode_body(redux.decode_body(body)), body, name)
        golden = redux.decode_body(self.fixture("redux_orb1_v1.txt"))
        self.assertEqual((golden["body"], golden["mu"], golden["target"]["name"]), ("Kerbin", 3531600000000.0, "Mun"))
        self.assertNotIn("maneuver", golden)
        self.assertEqual(golden["encounters"][0]["center"], (-8000000.0, 8000000.0, 0.0))
        self.assertEqual(redux.encode_body(None), "-")
        self.assertIsNone(redux.decode_body("-"))

    def test_numbers_use_the_csharp_round_trip_format(self):
        from kspsim import redux
        cases = {1000.5: "1000.5", -358.8: "-358.8", 3531600000000.0: "3531600000000", 1e-05: "1E-05",
                 1.5e20: "1.5E+20", 1e15: "1E+15", -2.5e300: "-2.5E+300", -0.0: "0", 0.1: "0.1", 1e-300: "1E-300"}
        for value, text in cases.items():
            self.assertEqual(redux.number(value), text)
            self.assertEqual(float(text), value)
        for bad in (float("nan"), float("inf")):
            with self.assertRaises(ValueError):
                redux.number(bad)
        self.assertEqual(redux.hexname("Mun"), "4d756e")
        self.assertEqual(redux.hexname(""), "-")
        golden = self.fixture("redux_orb1_v1.txt")
        for bad in (golden + " 0", golden + " ", golden.replace(" 6 ", " 8 ", 1), golden.replace("4b657262696e", "4B657262696E"),
                    golden[:-2], "1 - 1 0 0 0 0 0 0 0 0"):
            with self.assertRaises(ValueError):
                redux.decode_body(bad)

    def test_responder_reports_the_simulated_orbit_in_the_zup_frame(self):
        import socket
        from kspsim import redux

        sim = Simulator("orbit")
        sim.set_manual_mode(True)
        sim.set_manual_values(target_enabled=1, node_enabled=1, node_time=240, node_prograde=600, node_normal=0,
                              node_radial=0)
        server = redux.ReduxOrbitServer(sim, "127.0.0.1", 0)
        server.start()
        try:
            def ask(request):
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                    sock.settimeout(1.0)
                    sock.sendto(request, ("127.0.0.1", server.port))
                    try:
                        return sock.recv(8192).decode("ascii")
                    except socket.timeout:
                        return None
            answer = ask(b"KSP2ORB1 42")
            self.assertTrue(answer.startswith("KSP2ORB1 42 "), answer)
            self.assertLessEqual(len(answer), 4096)
            report = redux.decode_body(answer[len("KSP2ORB1 42 "):])
            r = sim.reported()
            self.assertEqual(report["body"], "Kerbin")
            self.assertEqual(report["position"], redux.zup([r["position_" + a] for a in "xyz"]))
            self.assertEqual(report["velocity"], redux.zup([r["velocity_" + a] for a in "xyz"]))
            self.assertEqual(report["position"][2], 0.0)  # the equatorial orbit lies in Zup's x/y plane
            self.assertEqual(report["target"]["name"], "Mun")
            self.assertTrue(report["target"]["body"])
            self.assertTrue(report["approach"]["planned"])  # on the post-burn patch, as with a node in KSP
            self.assertAlmostEqual(report["maneuver"]["ut"], sim.ut + 240)
            self.assertEqual(report["maneuver"]["soi"], physics.SOI_RADIUS)
            for e in report["encounters"]:
                self.assertEqual((e["body"], e["soi"]), ("Mun", 2_429_559.1))
                self.assertAlmostEqual(math.dist(e["position"], (0, 0, 0)), e["soi"], delta=10)
            # Malformed or other requests get no reply, as from the plugin.
            for request in (b"KSP2ORB1", b"KSP2ORB1 x", b"KSP2ORB1 12345678901234567", b"KSP2ORB1 1 2", b"KSP2HMI2 1"):
                self.assertIsNone(ask(request), request)
            sim.set_scene("vab")
            self.assertEqual(ask(b"KSP2ORB1 7"), "KSP2ORB1 7 -")
        finally:
            server.stop()


if __name__ == "__main__":
    unittest.main()
