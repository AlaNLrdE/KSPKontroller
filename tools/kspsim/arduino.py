"""The KSPSerialIO side of the serial link, for the real board: handshake, VesselData out, ControlPackets in.

Same framing and packets as the plugin (docs/protocol.rst, docs/vesseldata.rst):
``[0xBE][0xEF][size][payload][checksum = size XOR payload bytes]``.
"""
from __future__ import annotations

import logging
import math
import struct
import threading
import time
import queue
from dataclasses import dataclass

from . import physics

log = logging.getLogger("kspsim.board")

HEADER = b"\xBE\xEF"
ID_HANDSHAKE, ID_VESSEL_DATA, ID_CONTROL, ID_DEBUG = 0, 1, 101, 102
ARDUINO_VIDS = (0x2341, 0x2A03)

# VesselData, packed little endian, 214 bytes (field order of the firmware struct)
VESSEL_DATA = struct.Struct("<B8f2i2fi5f18fI2fI4fHBB2fBB2fB7h")
VESSEL_FIELDS = (
    "id AP PE SemiMajorAxis SemiMinorAxis VVI e inc G TAp TPe TrueAnomaly Density period RAlt Alt Vsurf Lat Lon "
    "LiquidFuelTot LiquidFuel OxidizerTot Oxidizer EChargeTot ECharge MonoPropTot MonoProp IntakeAirTot IntakeAir "
    "SolidFuelTot SolidFuel XenonGasTot XenonGas LiquidFuelTotS LiquidFuelS OxidizerTotS OxidizerS MissionTime "
    "deltaTime VOrbit MNTime MNDeltaV Pitch Roll Heading ActionGroups SOINumber MaxOverHeat MachNumber IAS "
    "CurrentStage TotalStage TargetDist TargetV NavballSASMode ProgradePitch ProgradeHeading ManeuverPitch "
    "ManeuverHeading TargetPitch TargetHeading NormalHeading").split()
CONTROL = struct.Struct("<BBBHBB9h")  # 25 bytes
# DebugPacket (board -> PC once per second while connected), 52 bytes, field order of the firmware struct
DEBUG = struct.Struct("<BIIHHHIIIIHBhHH12s")
DEBUG_FIELDS = ("id uptime_ms loops rx_ok rx_bad_cs rx_bad_size tx loop_max_us tx_max_us since_rx_ms reset flags "
                "rx_avail tx_skipped a0 matrix").split()
assert VESSEL_DATA.size == 214 and CONTROL.size == 25 and DEBUG.size == 52 and len(VESSEL_FIELDS) == 62

# DisplayCommand (id 2, PC -> board, test tools only): forces a LED matrix screen for 4 s (matrix.ino Screen enum;
# 16 + field = label page, 32 + field = value page of the data fields AP PE INC ALT SPD; 255 = automatic).
ID_DISPLAY = 2
DISPLAY_FORCE_S = 4.0
DISPLAY_SCREENS = (
    ("Fuel bars", 0), ("Stage", 1), ("Horizon", 2), ("Vertical speed", 3), ("SAS mode", 4),
    ("Data rotation", 5), ("Waiting animation", 64),
    *(("%s label" % n, 16 + i) for i, n in enumerate(("Ap", "Pe", "Inc", "Alt", "Speed"))),
    *(("%s value" % n, 32 + i) for i, n in enumerate(("Ap", "Pe", "Inc", "Alt", "Speed"))),
    ("Automatic", 255))

# Firmware constants the window shows next to the controls that exercise them (KSPIODemo9.ino)
GCAUTION, GWARN = 5, 9               # g, strictly greater
FUELCAUTION, FUELWARN = 50.0, 5.0    # percent of the stage tank, strictly lower
IDLETIMER_S, LINKRESET_S = 2.0, 30.0
RESET_CAUSES = ((0, "power on"), (8, "IWDT"), (9, "watchdog"), (10, "software"))
DEBUG_FLAGS = ((0, "connected"), (2, "SAS pin high"), (3, "watchdog on"), (4, "ever connected"))

# MainControls bits and ActionGroups bits, as in the firmware
BIT_SAS, BIT_RCS, BIT_LIGHTS, BIT_GEAR, BIT_BRAKES, BIT_PRECISION, BIT_ABORT, BIT_STAGE = 7, 6, 5, 4, 3, 2, 1, 0
KERBIN_SOI = 130


def frame(payload: bytes) -> bytes:
    checksum = len(payload)
    for b in payload:
        checksum ^= b
    return HEADER + bytes([len(payload)]) + payload + bytes([checksum])


class FrameParser:
    """Incremental parser: feed bytes, get complete valid payloads back."""

    def __init__(self) -> None:
        self.buf = bytearray()
        self.bad = 0
        self.last_bad = b""   # the rejected frame plus the 16 bytes after it, for the log

    def feed(self, data: bytes) -> list[bytes]:
        self.buf += data
        out = []
        while True:
            i = self.buf.find(HEADER)
            if i < 0:
                del self.buf[: len(self.buf) - 1 if self.buf[-1:] == b"\xBE" else len(self.buf)]
                return out
            if len(self.buf) < i + 3:
                del self.buf[:i]
                return out
            size = self.buf[i + 2]
            total = i + 3 + size + 1
            if size == 0:
                del self.buf[: i + 2]
                continue
            if len(self.buf) < total:
                del self.buf[:i]
                return out
            payload = bytes(self.buf[i + 3: i + 3 + size])
            checksum = size
            for b in payload:
                checksum ^= b
            if checksum == self.buf[total - 1]:
                out.append(payload)
                del self.buf[:total]
            else:
                self.bad += 1
                self.last_bad = bytes(self.buf[i: total + 16])
                del self.buf[: i + 1]


def decode_controls(p: bytes) -> physics.Controls:
    (_, main, mode, groups, _sas_mode, _extra, pitch, roll, yaw, _tx, _ty, _tz, _wsteer, throttle, _wthr) = CONTROL.unpack(p)
    bit = lambda n: bool(main >> n & 1)  # noqa: E731
    return physics.Controls(
        sas=bit(BIT_SAS), rcs=bit(BIT_RCS), lights=bit(BIT_LIGHTS), gear=bit(BIT_GEAR), brakes=bit(BIT_BRAKES),
        precision=bit(BIT_PRECISION), abort=bit(BIT_ABORT), stage=bit(BIT_STAGE), groups=groups,
        throttle=max(0, min(1000, throttle)) / 1000.0, pitch=pitch / 1000.0, roll=roll / 1000.0, yaw=yaw / 1000.0,
        mode=mode)


def display_frame(screen: int) -> bytes:
    """DisplayCommand frame: the board shows ``screen`` for 4 s (it also counts as a valid packet)."""
    if not 0 <= screen <= 255:
        raise ValueError("screen must be 0-255")
    return frame(bytes([ID_DISPLAY, screen]))


def bad_checksum_frame() -> bytes:
    """A handshake-sized frame with a wrong checksum: the board drops it and counts rx_bad_cs."""
    f = bytearray(frame(bytes([ID_HANDSHAKE, 1, 2, 3])))
    f[-1] ^= 0xFF
    return bytes(f)


def bad_size_frame() -> bytes:
    """A VesselData id announcing 10 bytes instead of 214: the board drops it and counts rx_bad_size."""
    return frame(bytes([ID_VESSEL_DATA]) + bytes(9))


def unpack_vessel(raw: bytes) -> dict:
    return dict(zip(VESSEL_FIELDS, VESSEL_DATA.unpack(raw)))


def expected_leds(f: dict) -> dict:
    """What Indicators() in utilities.ino lights for the VesselData fields ``f`` (unpack_vessel), with the reason."""
    fuel = f["LiquidFuelS"] / f["LiquidFuelTotS"] * 100 if f["LiquidFuelTotS"] else float("nan")  # NaN: no tank
    caution = [r for c, r in ((f["G"] > GCAUTION, "G %.1f > %d" % (f["G"], GCAUTION)),
                              (fuel < FUELCAUTION, "fuel %.0f%% < %.0f%%" % (fuel, FUELCAUTION))) if c]
    warning = [r for c, r in ((f["G"] > GWARN, "G %.1f > %d" % (f["G"], GWARN)),
                              (fuel < FUELWARN, "fuel %.0f%% < %.0f%%" % (fuel, FUELWARN))) if c]
    ag = f["ActionGroups"]
    return dict(yellow=caution, red=warning, sas=bool(ag & 1), rcs=bool(ag >> 1 & 1), cg1=bool(ag >> 6 & 1))


def decode_debug(p: bytes) -> dict:
    d = dict(zip(DEBUG_FIELDS, DEBUG.unpack(p[:DEBUG.size])))
    d["reset_causes"] = [name for bit, name in RESET_CAUSES if d["reset"] >> bit & 1]
    d["flag_names"] = [name for bit, name in DEBUG_FLAGS if d["flags"] >> bit & 1]
    return d


def matrix_rows(cols: bytes) -> list[str]:
    """The 12x8 LED matrix from the DebugPacket (one byte per column, bit r = row r), top row first."""
    return ["".join("#" if c >> r & 1 else "." for c in cols) for r in range(8)]


def debug_summary(d: dict) -> str:
    return ("uptime %.1f s, loops %d, loop max %d us, send max %d us\n"
            "rx ok %d, bad checksum %d, bad size %d, tx %d, since rx %d ms\n"
            "reset cause 0x%04X (%s)\nflags: %s; rx waiting %d, a0 %d") % (
        d["uptime_ms"] / 1000, d["loops"], d["loop_max_us"], d["tx_max_us"], d["rx_ok"], d["rx_bad_cs"],
        d["rx_bad_size"], d["tx"], d["since_rx_ms"], d["reset"], ", ".join(d["reset_causes"]) or "none",
        ", ".join(d["flag_names"]) or "none", d["rx_avail"], d["a0"])


def _f(v: float, limit: float = 3.0e38) -> float:
    """float32-safe: inf/nan as the plugin would never send them, clamp the rest."""
    if not math.isfinite(v):
        return 0.0
    return max(-limit, min(limit, v))


def _i32(v: float) -> int:
    return 0 if not math.isfinite(v) else int(max(-2**31, min(2**31 - 1, round(v))))


def _nav(une) -> tuple[int, int]:
    """(pitch, heading) x 50 of a direction in (up, north, east), as the plugin's WorldVecToNavHeading and
    FloatAngleToFixed_16; 0, 0 for a zero vector."""
    une = tuple(_f(c) for c in une)
    n = math.sqrt(sum(c * c for c in une))
    if n < 1e-9:
        return 0, 0
    pitch = math.degrees(math.asin(max(-1.0, min(1.0, une[0] / n))))
    heading = math.degrees(math.atan2(une[2], une[1])) % 360.0 if math.hypot(une[1], une[2]) > 1e-9 else 0.0
    return int(round(pitch * 50)), int(round(heading * 50)) % 18000


def _prograde(r: dict) -> tuple[float, float, float]:
    """The NavBall prograde of the reported speed mode (0 orbit, 1 surface, 2 target), eastward in the plane."""
    mode = int(_f(r.get("speed_mode", 0)))
    if mode == 2:
        return tuple(r["relative_" + a] for a in ("up", "north", "east"))
    speed = _f(r["surface_speed" if mode == 1 else "orbital_speed"])
    vs = _f(r["vertical_speed"])
    return vs, 0.0, math.sqrt(max(0.0, speed * speed - vs * vs))


def encode_vessel(v: physics.Vessel, delta_time: float, r: dict | None = None) -> bytes:
    """VesselData for ``v``; the orbit / altitude / speed values come from ``r`` (Simulator.reported(): the
    flight model, or the values typed in manual mode), the rest (fuel, stages, action groups) from the vessel."""
    r = r if r is not None else v.report()
    c = v.controls
    alt = r["altitude"]
    rho = physics.density(alt) if math.isfinite(alt) else 0.0
    surf = r["surface_speed"] if math.isfinite(r["surface_speed"]) else 0.0
    e, a = r["eccentricity"], r["semi_major_axis"]
    pitch, heading = r["pitch"], r["heading"]
    ag = r.get("action_groups")  # the game state (Simulator.reported()); without it, echo the controls
    if ag is None:
        ag = (c.sas << 0 | c.rcs << 1 | c.lights << 2 | c.gear << 3 | c.brakes << 4 | c.abort << 5
              | ((c.groups >> 1) & 0x3FF) << 6)
    sas_on = bool(ag & 1)
    pro = _prograde(r)
    pro_pitch, pro_heading = _nav(pro)
    horizontal = math.hypot(pro[1], pro[2])  # normal: perpendicular to the prograde, south for an eastward orbit
    normal_heading = _nav((0.0, -pro[2] / horizontal, pro[1] / horizontal))[1] if horizontal > 1e-9 else 0
    node = bool(r.get("node_enabled"))
    mn_pitch, mn_heading = _nav(tuple(r["burn_" + a] for a in ("up", "north", "east"))) if node else (0, 0)
    target = bool(r.get("target_enabled"))
    target_vec = tuple(_f(r["target_" + a]) for a in ("up", "north", "east"))
    tg_pitch, tg_heading = _nav(target_vec) if target else (0, 0)
    lf_s, ox_s = v.liquid_fuel, v.oxidizer
    if "stage_fuel" in r:  # percent of the stage tank (a forced value pins it)
        pct = max(0.0, min(100.0, _f(r["stage_fuel"]))) / 100
        lf_s, ox_s = physics.LIQUID_FUEL * pct, physics.OXIDIZER * pct
    overheat = int(max(0, min(255, _f(r.get("max_overheat", v.heat * 100)))))
    total_lf = physics.LIQUID_FUEL * (v.stage + 1) + 0.0
    lf_total_now = lf_s + physics.LIQUID_FUEL * v.stage
    ox_total_now = ox_s + physics.OXIDIZER * v.stage
    lon = (math.degrees(math.atan2(v.y, v.x) - physics.OMEGA * v.mission_time) + 180) % 360 - 180
    values = dict(
        id=ID_VESSEL_DATA, AP=_f(r["apoapsis"]), PE=_f(r["periapsis"]), SemiMajorAxis=_f(a),
        SemiMinorAxis=_f(abs(a) * math.sqrt(abs(1 - e * e))) if math.isfinite(a) and math.isfinite(e) else 0.0,
        VVI=_f(r["vertical_speed"]), e=_f(e), inc=_f(r["inclination"]), G=_f(r["g_force"]),
        TAp=_i32(r["time_to_apoapsis"]), TPe=_i32(r["time_to_periapsis"]), TrueAnomaly=_f(r["true_anomaly"]),
        Density=rho, period=_i32(r["period"]), RAlt=_f(max(alt, 0.0)), Alt=_f(alt), Vsurf=_f(surf),
        Lat=0.0, Lon=lon,
        LiquidFuelTot=total_lf, LiquidFuel=lf_total_now, OxidizerTot=physics.OXIDIZER * (v.stage + 1),
        Oxidizer=ox_total_now, EChargeTot=150.0, ECharge=v.electric, MonoPropTot=30.0, MonoProp=v.monoprop,
        IntakeAirTot=0.0, IntakeAir=0.0, SolidFuelTot=0.0, SolidFuel=0.0, XenonGasTot=0.0, XenonGas=0.0,
        LiquidFuelTotS=physics.LIQUID_FUEL, LiquidFuelS=lf_s, OxidizerTotS=physics.OXIDIZER,
        OxidizerS=ox_s, MissionTime=int(v.mission_time), deltaTime=delta_time, VOrbit=_f(r["orbital_speed"]),
        MNTime=max(0, _i32(r["node_time"])) if node else 0, MNDeltaV=_f(r["node_remaining"]) if node else 0.0,
        Pitch=pitch, Roll=r["roll"], Heading=heading, ActionGroups=ag, SOINumber=KERBIN_SOI,
        MaxOverHeat=overheat, MachNumber=surf / 340.0 if rho > 0 else 0.0,
        IAS=surf * math.sqrt(rho / physics.SEA_DENSITY), CurrentStage=v.stage, TotalStage=physics.STAGES,
        TargetDist=math.sqrt(sum(c * c for c in target_vec)) if target else 0.0,
        TargetV=math.sqrt(sum(_f(r["relative_" + a]) ** 2 for a in ("up", "north", "east"))) if target else 0.0,
        NavballSASMode=(int(_f(r.get("speed_mode", 0))) + 1) << 4 | (int(r.get("sas_mode", 1)) & 0x0F if sas_on else 0),
        ProgradePitch=pro_pitch, ProgradeHeading=pro_heading, ManeuverPitch=mn_pitch, ManeuverHeading=mn_heading,
        TargetPitch=tg_pitch, TargetHeading=tg_heading, NormalHeading=normal_heading)
    return VESSEL_DATA.pack(*(values[k] for k in VESSEL_FIELDS))


def _line(ser, attr):
    """A modem line or counter for the log; some drivers raise instead of answering."""
    try:
        return getattr(ser, attr)
    except Exception as e:  # noqa: BLE001 - diagnostics only
        return "?(%s)" % type(e).__name__


@dataclass
class BoardStatus:
    state: str = "idle"          # idle | searching | handshake | linked | error
    port: str = ""
    detail: str = ""
    sent: int = 0
    received: int = 0
    bad: int = 0
    uptime_ms: int = 0           # from the board's DebugPacket
    last_controls: float = 0.0   # time.monotonic() of the last ControlPacket
    debug: dict | None = None    # last DebugPacket, decoded (decode_debug)
    debug_at: float = 0.0        # time.monotonic() when it arrived


class BoardLink:
    """Owns the serial port in a thread; reconnects (and re-handshakes) whenever the board goes away."""

    def __init__(self, sim, port: str | None = None, refresh: float = 0.2, baud: int = 115200) -> None:
        self.sim = sim
        self.requested = port
        self.refresh = refresh
        self.baud = baud
        self.status = BoardStatus()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="BoardLink", daemon=True)
        # Link tests from the window (written by the window thread, read by the link thread)
        self._out: queue.Queue = queue.Queue()  # (description, raw frame) sent as is by the session
        self.paused_since: float | None = None  # telemetry paused (time.monotonic()), None = streaming
        self.hold_screen: int | None = None     # DisplayCommand repeated before the 4 s expire
        self._rehandshake = threading.Event()
        self.events: list[str] = []

    # -- link tests (callable from any thread) -------------------------------------------------------
    def _queue(self, what: str, raw: bytes) -> str:
        if self.status.state != "linked":
            return "not sent: the board is not linked (%s)" % self.status.state
        self._out.put((what, raw))
        return "queued: " + what

    def send_display(self, screen: int) -> str:
        return self._queue("DisplayCommand screen %d" % screen, display_frame(screen))

    def send_bad_checksum(self) -> str:
        return self._queue("frame with a wrong checksum", bad_checksum_frame())

    def send_bad_size(self) -> str:
        return self._queue("frame with a wrong size", bad_size_frame())

    def pause_telemetry(self, on: bool) -> None:
        """Stop every frame to the board (VesselData, DisplayCommand): idle after 2 s, board reset after 30 s."""
        self.paused_since = time.monotonic() if on else None
        self._event("telemetry paused" if on else "telemetry resumed")

    def rehandshake(self) -> None:
        """Handshake again on the open port, as the plugin does after a reconnect."""
        self._rehandshake.set()

    def _event(self, text: str) -> None:
        """Short log of the link tests and what the board did, newest last (shown in the window)."""
        self.events.append("%s %s" % (time.strftime("%H:%M:%S"), text))
        del self.events[:-6]
        log.info("board: %s", text)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=3)

    def _ports(self) -> list[str]:
        from serial.tools import list_ports
        if self.requested:
            return [self.requested]
        usb = [p for p in list_ports.comports() if p.vid in ARDUINO_VIDS]
        return sorted(p.device for p in usb)

    def _run(self) -> None:
        import serial
        while not self._stop.is_set():
            if self.sim.ksp_running():
                self.status = BoardStatus("error", detail="KSP is running and owns the serial port: close it")
                self._stop.wait(3)
                continue
            ports = self._ports()
            if not ports:
                if self.status.state != "searching":
                    log.info("no Arduino USB port found (VIDs %s)", ", ".join("%04x" % v for v in sorted(ARDUINO_VIDS)))
                self.status = BoardStatus("searching", detail="no Arduino USB port found")
                self._stop.wait(2)
                continue
            for name in ports:
                try:
                    log.info("opening %s at %d baud", name, self.baud)
                    with serial.Serial(name, self.baud, timeout=0.05, write_timeout=1) as ser:
                        self._session(ser, name)
                except (serial.SerialException, OSError) as e:
                    log.warning("%s: %s", name, e)
                    self.status = BoardStatus("error", name, str(e)[:120])
                if self._stop.is_set():
                    return
            self._stop.wait(2)

    def _handshake(self, ser, parser: FrameParser) -> bool:
        seen = bytearray()   # everything the board sent during the handshake, for the log
        packets = []
        for attempt in range(1, 6):
            written = ser.write(frame(bytes([ID_HANDSHAKE, 1, 2, 3])))
            end = time.monotonic() + 0.7
            while time.monotonic() < end:
                chunk = ser.read(256)
                seen += chunk
                for p in parser.feed(chunk):
                    packets.append("id %d len %d" % (p[0], len(p)))
                    if p[0] == ID_HANDSHAKE and p[1:] == bytes([3, 1, 4]):
                        log.info("handshake answered on attempt %d (%d bytes received)", attempt, len(seen))
                        return True
            log.info("handshake attempt %d: wrote %s bytes, received %d bytes so far, packets %s, "
                     "cts=%s dsr=%s in_waiting=%s", attempt, written, len(seen), packets[-5:] or "none",
                     _line(ser, "cts"), _line(ser, "dsr"), _line(ser, "in_waiting"))
        log.warning("no handshake reply after 5 attempts: %d bytes received%s, bad frames %d",
                    len(seen), (" (first: %s)" % seen[:48].hex(" ")) if seen else " (port completely silent)", parser.bad)
        return False

    def _session(self, ser, name: str) -> None:
        time.sleep(0.3)  # the board may reset when DTR rises
        ser.reset_input_buffer()
        parser = FrameParser()
        self.status = BoardStatus("handshake", name, "sending 1, 2, 3")
        if not self._handshake(ser, parser):
            self.status = BoardStatus("error", name, "no handshake reply (3, 1, 4)")
            return
        log.info("board on %s answered the handshake", name)
        if hasattr(self.sim, "board_linked"):
            self.sim.board_linked()
        st = BoardStatus("linked", name, "handshake OK")
        self.status = st
        self._event("handshake OK on %s" % name)
        if self.paused_since is not None:
            self.pause_telemetry(False)  # a new session (after the board reset itself) ends the pause test
        was_paused = False
        next_send = next_hold = 0.0
        last_rx = time.monotonic()
        last_mission = None
        while not self._stop.is_set():
            now = time.monotonic()
            if self._rehandshake.is_set():
                self._rehandshake.clear()
                st.state, st.detail = "handshake", "re-handshaking (requested)"
                ok = self._handshake(ser, parser)
                self._event("re-handshake " + ("answered 3, 1, 4" if ok else "NOT answered"))
                if not ok:
                    self.status = BoardStatus("error", name, "no reply to the requested re-handshake")
                    return
                last_rx = time.monotonic()
                st.state = "linked"
            since = self.paused_since  # read once: the window thread may clear it
            paused = since is not None
            if was_paused and not paused:
                last_rx = now  # any valid packet reconnects an idle board: give the telemetry 5 s to do it
            was_paused = paused
            while not self._out.empty():
                what, raw = self._out.get_nowait()
                if paused:
                    self._event("dropped (telemetry paused): " + what)
                    continue
                ser.write(raw)
                self._event("sent " + what)
            if not paused and self.hold_screen is not None and now >= next_hold:
                ser.write(display_frame(self.hold_screen))
                next_hold = now + DISPLAY_FORCE_S - 1.0
            if not paused and now >= next_send:
                vessel = self.sim.vessel_for_board()
                if vessel is not None:
                    dt = 0.0 if last_mission is None else vessel.mission_time - last_mission
                    last_mission = vessel.mission_time
                    payload = (bytes([ID_VESSEL_DATA]) + bytes(VESSEL_DATA.size - 1) if self.sim.scene in ("vab", "sph")
                               else encode_vessel(vessel, dt, self.sim.reported()))
                    ser.write(frame(payload))
                    st.sent += 1
                next_send = now + self.refresh
            bad_before = parser.bad
            packets = parser.feed(ser.read(256))
            if parser.bad != bad_before:
                lb = parser.last_bad
                log.warning("bad frame #%d (size byte %d, %d bytes kept): %s", parser.bad,
                            lb[2] if len(lb) > 2 else -1, len(lb), lb.hex(" "))
            for p in packets:
                last_rx = time.monotonic()
                if p[0] == ID_CONTROL and len(p) == CONTROL.size:
                    st.received += 1
                    st.last_controls = last_rx
                    self.sim.board_controls(decode_controls(p))
                elif p[0] == ID_DEBUG and len(p) == DEBUG.size:
                    d = decode_debug(p)
                    if st.debug and d["uptime_ms"] < st.debug["uptime_ms"]:
                        self._event("board REBOOTED (reset cause %s)" % (", ".join(d["reset_causes"]) or "none"))
                    if st.debug and d["rx_bad_cs"] > st.debug["rx_bad_cs"]:
                        self._event("board counted %d bad checksum(s)" % (d["rx_bad_cs"] - st.debug["rx_bad_cs"]))
                    if st.debug and d["rx_bad_size"] > st.debug["rx_bad_size"]:
                        self._event("board counted %d bad size(s)" % (d["rx_bad_size"] - st.debug["rx_bad_size"]))
                    st.debug, st.debug_at, st.uptime_ms = d, last_rx, d["uptime_ms"]
            st.bad = parser.bad
            if paused:
                quiet = now - last_rx
                into = max(0.0, last_rx - since)
                st.detail = ("PAUSED %.0f s: board quiet %.1f s, last packet %.1f s into it\n"
                             "(expected: idle after %.0f s, board resets itself after %.0f s)" % (
                                 now - since, quiet, into, IDLETIMER_S, LINKRESET_S))
                continue  # silence is the point: no automatic re-handshake
            if self.sim.vessel_for_board() is None:
                st.detail = "not in flight: no telemetry sent (the board goes idle after 2 s)"
                last_rx = now  # silence is expected here
            elif now - last_rx > 5:
                st.state, st.detail = "error", "board silent for 5 s: re-handshaking"
                return
            else:
                st.state, st.detail = "linked", "telemetry every %d ms" % (self.refresh * 1000)
