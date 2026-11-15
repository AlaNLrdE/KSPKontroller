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


if __name__ == "__main__":
    unittest.main()
