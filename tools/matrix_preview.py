#!/usr/bin/env python3
"""Show what the LED matrix of the board displays, as ASCII art, without flying.

Two modes:

    matrix_preview.py [PORT]            Feed the board test telemetry for every screen, using the real firmware,
                                        and print the frame it reports back in its DebugPacket.
    matrix_preview.py --log KSP.log     Decode the matrix= field of the KSPSerialIO[BOARD] lines of a saved log
                                        (what the matrix showed, for example just before the board froze).

The frame is 12 bytes, one per column; bit r of byte c is the pixel at row r, column c.
KSP must be closed for the first mode (it needs the serial port).
"""
import argparse
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

SCREENS = {"fuel": 0, "stage": 1, "horizon": 2, "vsi": 3, "sas": 4}

# VesselData wire layout (see docs/vesseldata.rst); little-endian, packed, 214 bytes
FORMAT = ("<B" + "f" * 8 + "ii" + "ff" + "i" + "f" * 5 + "f" * 14 + "ffff" + "I" + "ff" + "I" + "f" + "fff" +
          "H" + "BB" + "ff" + "BB" + "ff" + "B" + "hhhhhhh")
assert struct.calcsize(FORMAT) == 214, struct.calcsize(FORMAT)

FIELDS = ("id AP PE SemiMajorAxis SemiMinorAxis VVI e inc G TAp TPe TrueAnomaly Density period RAlt Alt Vsurf Lat Lon "
          "LiquidFuelTot LiquidFuel OxidizerTot Oxidizer EChargeTot ECharge MonoPropTot MonoProp IntakeAirTot IntakeAir "
          "SolidFuelTot SolidFuel XenonGasTot XenonGas LiquidFuelTotS LiquidFuelS OxidizerTotS OxidizerS MissionTime "
          "deltaTime VOrbit MNTime MNDeltaV Pitch Roll Heading ActionGroups SOINumber MaxOverHeat MachNumber IAS "
          "CurrentStage TotalStage TargetDist TargetV NavballSASMode ProgradePitch ProgradeHeading ManeuverPitch "
          "ManeuverHeading TargetPitch TargetHeading NormalHeading").split()
assert len(FIELDS) == 62


def vessel(**values):
    """Pack a VesselData payload (id 1) with the given fields, all others zero."""
    data = dict.fromkeys(FIELDS, 0)
    data["id"] = 1
    data.update(values)
    return struct.pack(FORMAT, *[data[f] for f in FIELDS])


# name -> (screen, VesselData fields)
SCENARIOS = {
    "fuel full":         ("fuel", dict(LiquidFuelTot=100, LiquidFuel=100, OxidizerTot=120, Oxidizer=120, MonoPropTot=20, MonoProp=20, EChargeTot=50, ECharge=50)),
    "fuel half":         ("fuel", dict(LiquidFuelTot=100, LiquidFuel=50, OxidizerTot=120, Oxidizer=60, MonoPropTot=20, MonoProp=20, EChargeTot=50, ECharge=12)),
    "fuel no monoprop":  ("fuel", dict(LiquidFuelTot=100, LiquidFuel=100, OxidizerTot=120, Oxidizer=120, EChargeTot=50, ECharge=50)),
    "stage 5 on pad":    ("stage", dict(CurrentStage=5, TotalStage=6, RAlt=5)),
    "stage 12":          ("stage", dict(CurrentStage=12, TotalStage=12)),
    "horizon level":     ("horizon", dict(Pitch=0, Roll=0)),
    "horizon pitch +20": ("horizon", dict(Pitch=20, Roll=0)),
    "horizon roll 30":   ("horizon", dict(Pitch=0, Roll=30)),
    "vsi climbing":      ("vsi", dict(VVI=60)),
    "vsi falling slow":  ("vsi", dict(VVI=-5)),
    "vsi level":         ("vsi", dict(VVI=0)),
    "sas off":           ("sas", dict(ActionGroups=0)),
    "sas prograde":      ("sas", dict(ActionGroups=1, NavballSASMode=2)),
    "sas maneuver":      ("sas", dict(ActionGroups=1, NavballSASMode=10)),
}


def render(frame):
    """12 column bytes -> 8 text rows."""
    return ["".join("#" if (frame[c] >> r) & 1 else "." for c in range(12)) for r in range(8)]


def from_log(path):
    lines = [l for l in open(path, encoding="utf-8", errors="replace") if "KSPSerialIO[BOARD] uptime_ms=" in l]
    if not lines:
        print("no KSPSerialIO[BOARD] lines with matrix= in", path)
        return 1
    shown = lines[-3:]
    for l in shown:
        m = re.search(r"\[LOG ([\d:.]+)\].*uptime_ms=(\d+).*matrix=([0-9A-F]{24})", l)
        if not m:
            continue
        print("%s  uptime %s ms" % (m.group(1), m.group(2)))
        print("\n".join("  " + row for row in render(bytes.fromhex(m.group(3)))))
    return 0


def from_board(port):
    import serial
    import test_arduino as t
    device = port or next((d for d, _ in t.candidate_ports(None)), None)
    if not device:
        print("no USB serial port found")
        return 2
    ser = serial.Serial(device, 115200, timeout=0.05, write_timeout=1)
    ser.dtr = True
    time.sleep(2.0)
    ser.reset_input_buffer()
    ser.write(t.frame([0, 1, 2, 3]))
    time.sleep(0.3)
    print("Using", device)
    failed = 0
    for name, (screen, fields) in SCENARIOS.items():
        payload = vessel(**fields)
        cmd = t.frame([t.ID_DISPLAY, SCREENS[screen]])
        frame, end = None, time.monotonic() + 3.5
        while time.monotonic() < end and frame is None:
            ser.write(t.frame(payload))
            ser.write(cmd)
            frames, _ = t.read_frames(ser, 0.25)
            for p in frames:
                if p[0] == t.ID_DEBUG and len(p) == t.DEBUG_PACKET_SIZE:
                    frame = p[-12:]
        print("\n%s  (screen %s)" % (name, screen))
        if frame is None:
            print("  no DebugPacket received (old firmware?)")
            failed += 1
            continue
        print("\n".join("  " + row for row in render(frame)))
    ser.close()
    return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("port", nargs="?")
    ap.add_argument("--log", help="decode the matrix frames of a saved KSP.log")
    args = ap.parse_args()
    return from_log(args.log) if args.log else from_board(args.port)


if __name__ == "__main__":
    sys.exit(main())
