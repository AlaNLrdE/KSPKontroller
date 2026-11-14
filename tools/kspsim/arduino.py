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
assert VESSEL_DATA.size == 214 and CONTROL.size == 25 and len(VESSEL_FIELDS) == 62

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
                del self.buf[: i + 1]


def decode_controls(p: bytes) -> physics.Controls:
    (_, main, mode, groups, _sas_mode, _extra, pitch, roll, yaw, _tx, _ty, _tz, _wsteer, throttle, _wthr) = CONTROL.unpack(p)
    bit = lambda n: bool(main >> n & 1)  # noqa: E731
    return physics.Controls(
        sas=bit(BIT_SAS), rcs=bit(BIT_RCS), lights=bit(BIT_LIGHTS), gear=bit(BIT_GEAR), brakes=bit(BIT_BRAKES),
        precision=bit(BIT_PRECISION), abort=bit(BIT_ABORT), stage=bit(BIT_STAGE), groups=groups,
        throttle=max(0, min(1000, throttle)) / 1000.0, pitch=pitch / 1000.0, roll=roll / 1000.0, yaw=yaw / 1000.0,
        mode=mode)


def _f(v: float, limit: float = 3.0e38) -> float:
    """float32-safe: inf/nan as the plugin would never send them, clamp the rest."""
    if not math.isfinite(v):
        return 0.0
    return max(-limit, min(limit, v))


def _i32(v: float) -> int:
    return 0 if not math.isfinite(v) else int(max(-2**31, min(2**31 - 1, round(v))))


def encode_vessel(v: physics.Vessel, delta_time: float) -> bytes:
    el = v.elements()
    c = v.controls
    alt = v.altitude
    rho = physics.density(alt)
    surf = v.surface_speed
    pitch, heading = v.attitude()
    ag = (c.sas << 0 | c.rcs << 1 | c.lights << 2 | c.gear << 3 | c.brakes << 4 | c.abort << 5
          | ((c.groups >> 1) & 0x3FF) << 6)
    total_lf = physics.LIQUID_FUEL * (v.stage + 1) + 0.0
    lf_total_now = v.liquid_fuel + physics.LIQUID_FUEL * v.stage
    ox_total_now = v.oxidizer + physics.OXIDIZER * v.stage
    lon = (math.degrees(math.atan2(v.y, v.x) - physics.OMEGA * v.mission_time) + 180) % 360 - 180
    values = dict(
        id=ID_VESSEL_DATA, AP=_f(el.apoapsis), PE=_f(el.periapsis), SemiMajorAxis=_f(el.a),
        SemiMinorAxis=_f(abs(el.a) * math.sqrt(abs(1 - el.e * el.e))), VVI=v.vertical_speed, e=el.e,
        inc=v.inclination, G=v.g_force(), TAp=_i32(el.time_to_apoapsis), TPe=_i32(el.time_to_periapsis),
        TrueAnomaly=el.nu, Density=rho, period=_i32(el.period), RAlt=max(alt, 0.0), Alt=alt, Vsurf=surf,
        Lat=0.0, Lon=lon,
        LiquidFuelTot=total_lf, LiquidFuel=lf_total_now, OxidizerTot=physics.OXIDIZER * (v.stage + 1),
        Oxidizer=ox_total_now, EChargeTot=150.0, ECharge=v.electric, MonoPropTot=30.0, MonoProp=v.monoprop,
        IntakeAirTot=0.0, IntakeAir=0.0, SolidFuelTot=0.0, SolidFuel=0.0, XenonGasTot=0.0, XenonGas=0.0,
        LiquidFuelTotS=physics.LIQUID_FUEL, LiquidFuelS=v.liquid_fuel, OxidizerTotS=physics.OXIDIZER,
        OxidizerS=v.oxidizer, MissionTime=int(v.mission_time), deltaTime=delta_time, VOrbit=v.orbital_speed,
        MNTime=0, MNDeltaV=0.0, Pitch=pitch, Roll=0.0, Heading=heading, ActionGroups=ag, SOINumber=KERBIN_SOI,
        MaxOverHeat=int(v.heat * 100), MachNumber=surf / 340.0 if rho > 0 else 0.0,
        IAS=surf * math.sqrt(rho / physics.SEA_DENSITY), CurrentStage=v.stage, TotalStage=physics.STAGES,
        TargetDist=0.0, TargetV=0.0, NavballSASMode=(1 + 1) << 4 | (1 if c.sas else 0),
        ProgradePitch=int(max(-90, min(90, pitch)) * 50), ProgradeHeading=int(heading * 50),
        ManeuverPitch=0, ManeuverHeading=0, TargetPitch=0, TargetHeading=0, NormalHeading=0)
    return VESSEL_DATA.pack(*(values[k] for k in VESSEL_FIELDS))


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
                self.status = BoardStatus("searching", detail="no Arduino USB port found")
                self._stop.wait(2)
                continue
            for name in ports:
                try:
                    with serial.Serial(name, self.baud, timeout=0.05, write_timeout=1) as ser:
                        self._session(ser, name)
                except (serial.SerialException, OSError) as e:
                    log.warning("%s: %s", name, e)
                    self.status = BoardStatus("error", name, str(e)[:120])
                if self._stop.is_set():
                    return
            self._stop.wait(2)

    def _handshake(self, ser, parser: FrameParser) -> bool:
        for _ in range(5):
            ser.write(frame(bytes([ID_HANDSHAKE, 1, 2, 3])))
            end = time.monotonic() + 0.7
            while time.monotonic() < end:
                for p in parser.feed(ser.read(256)):
                    if p[0] == ID_HANDSHAKE and p[1:] == bytes([3, 1, 4]):
                        return True
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
        st = BoardStatus("linked", name, "handshake OK")
        self.status = st
        next_send = 0.0
        last_rx = time.monotonic()
        last_mission = None
        while not self._stop.is_set():
            now = time.monotonic()
            if now >= next_send:
                vessel = self.sim.vessel_for_board()
                if vessel is not None:
                    dt = 0.0 if last_mission is None else vessel.mission_time - last_mission
                    last_mission = vessel.mission_time
                    ser.write(frame(encode_vessel(vessel, dt)))
                    st.sent += 1
                next_send = now + self.refresh
            for p in parser.feed(ser.read(256)):
                last_rx = time.monotonic()
                if p[0] == ID_CONTROL and len(p) == CONTROL.size:
                    st.received += 1
                    st.last_controls = last_rx
                    self.sim.board_controls(decode_controls(p))
                elif p[0] == ID_DEBUG and len(p) >= 5:
                    st.uptime_ms = int.from_bytes(p[1:5], "little")
            st.bad = parser.bad
            if self.sim.vessel_for_board() is None:
                st.detail = "not in flight: no telemetry sent (the board goes idle after 2 s)"
                last_rx = now  # silence is expected here
            elif now - last_rx > 5:
                st.state, st.detail = "error", "board silent for 5 s: re-handshaking"
                return
            else:
                st.state, st.detail = "linked", "telemetry every %d ms" % (self.refresh * 1000)
