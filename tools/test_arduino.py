#!/usr/bin/env python3
"""Hardware test for the KSPIODemo9 firmware.

Talks to the Arduino with the real KSPSerialIO wire protocol and checks that it behaves like the
plugin expects:

  1. handshake      KSP sends 1, 2, 3 and the board must answer 3, 1, 4
  2. telemetry      after a VesselData packet the board must stream ControlPackets (id 101, 25 bytes)
  3. reopen         close and reopen the port (DTR toggles): handshake answered again, ControlPackets
                    resume within 2 s; also after an abrupt close with 60 frames in flight
  4. idle timeout   with no more telemetry the board must stop sending after about 2 seconds

If the board does not answer on the requested port (or no port is given), every USB serial port is
tried, Arduino vendor ids first. Exit code: 0 all passed, 1 a test failed, 2 no board found.

Run it through ``./ci test_arduino [PORT]``.
"""

import argparse
import sys
import time

import serial
from serial.tools import list_ports

# Frame: [0xBE][0xEF][size][payload ... size bytes][checksum = size XOR every payload byte]
HEADER = b"\xBE\xEF"
ID_HANDSHAKE = 0
ID_VESSEL_DATA = 1
ID_CONTROL = 101
VESSEL_DATA_SIZE = 214
CONTROL_PACKET_SIZE = 25
ID_DEBUG = 102
DEBUG_PACKET_SIZE = 52
ID_DISPLAY = 2

# USB vendor ids tried first (Arduino SA, Arduino LLC)
ARDUINO_VIDS = (0x2341, 0x2A03)


def frame(payload):
    """Wrap a payload in the wire framing."""
    checksum = len(payload)
    for b in payload:
        checksum ^= b
    return HEADER + bytes([len(payload)]) + bytes(payload) + bytes([checksum])


def read_frames(ser, duration):
    """Read for `duration` seconds and return (valid payloads, number of bad frames)."""
    frames, bad = [], 0
    buf = bytearray()
    end = time.monotonic() + duration
    while time.monotonic() < end:
        buf += ser.read(256)
        while True:
            i = buf.find(HEADER)
            if i < 0:
                # keep a trailing 0xBE, it may be the first half of a header
                del buf[: len(buf) - 1 if buf[-1:] == b"\xBE" else len(buf)]
                break
            if len(buf) < i + 3:
                del buf[:i]
                break
            size = buf[i + 2]
            if size == 0:
                del buf[: i + 2]
                continue
            total = i + 3 + size + 1
            if len(buf) < total:
                del buf[:i]
                break
            payload = bytes(buf[i + 3 : i + 3 + size])
            checksum = size
            for b in payload:
                checksum ^= b
            if checksum == buf[total - 1]:
                frames.append(payload)
                del buf[:total]
            else:
                bad += 1
                del buf[: i + 1]  # resync on the next header
    return frames, bad


def candidate_ports(requested):
    """Requested port first, then every USB serial port with Arduino boards ahead of the rest."""
    usb = [p for p in list_ports.comports() if p.vid is not None]
    usb.sort(key=lambda p: (p.vid not in ARDUINO_VIDS, p.device))
    ordered = []
    if requested:
        ordered.append((requested, "requested port"))
    for p in usb:
        if p.device != requested:
            ordered.append((p.device, p.description))
    return ordered


def test_handshake(ser):
    for _ in range(3):
        ser.write(frame([ID_HANDSHAKE, 1, 2, 3]))
        frames, _ = read_frames(ser, 0.7)
        for p in frames:
            if p[0] == ID_HANDSHAKE and len(p) == 4:
                if tuple(p[1:]) == (3, 1, 4):
                    return True, "board answered 3, 1, 4"
                return False, "handshake reply was %s, expected 3, 1, 4" % list(p[1:])
    return False, "no handshake reply"


def test_telemetry(ser):
    ser.reset_input_buffer()
    ser.write(frame([ID_VESSEL_DATA] + [0] * (VESSEL_DATA_SIZE - 1)))
    frames, bad = read_frames(ser, 1.0)
    controls = [p for p in frames if p[0] == ID_CONTROL]
    wrong_size = [p for p in controls if len(p) != CONTROL_PACKET_SIZE]
    if wrong_size:
        return False, "ControlPacket has %d bytes, the plugin expects %d" % (
            len(wrong_size[0]), CONTROL_PACKET_SIZE)
    if len(controls) < 3:
        return False, "only %d ControlPackets in 1 s (bad frames: %d)" % (len(controls), bad)
    throttle = int.from_bytes(controls[-1][21:23], "little", signed=True)
    if not 0 <= throttle <= 1000:
        return False, "throttle %d is outside 0..1000" % throttle
    return True, "%d ControlPackets in 1 s, %d bad frames, throttle %d" % (len(controls), bad, throttle)


def test_debug(ser):
    """The board must send a 40-byte DebugPacket about once per second while it is being fed telemetry."""
    ser.reset_input_buffer()
    vessel = frame([ID_VESSEL_DATA] + [0] * (VESSEL_DATA_SIZE - 1))
    debug = []
    end = time.monotonic() + 3.0
    while time.monotonic() < end:
        ser.write(vessel)
        frames, _ = read_frames(ser, 0.2)
        debug += [p for p in frames if p[0] == ID_DEBUG]
    if not debug:
        return False, "no DebugPacket in 3 s (old firmware?)"
    wrong = [p for p in debug if len(p) != DEBUG_PACKET_SIZE]
    if wrong:
        return False, "DebugPacket has %d bytes, the plugin expects %d" % (len(wrong[0]), DEBUG_PACKET_SIZE)
    uptime = int.from_bytes(debug[-1][1:5], "little")
    return True, "%d DebugPackets in 3 s, uptime %d ms" % (len(debug), uptime)


def _feed_and_wait_controls(ser, seconds):
    """Feed telemetry for `seconds` and return how many ControlPackets came back."""
    vessel = frame([ID_VESSEL_DATA] + [0] * (VESSEL_DATA_SIZE - 1))
    controls = 0
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        ser.write(vessel)
        frames, _ = read_frames(ser, 0.1)
        controls += sum(1 for p in frames if p[0] == ID_CONTROL)
    return controls


def _reopen(ser, wait=2.0):
    """Close and reopen the same port (DTR drops and rises), then give the board `wait` seconds."""
    ser.close()
    time.sleep(0.5)
    ser.open()
    time.sleep(0.3)
    ser.reset_input_buffer()


def test_reopen(ser):
    """The host may close and reopen the port at any time; the board must answer the handshake again
    and resume ControlPackets within 2 s while telemetry is fed. Done twice, then once after an abrupt
    close with 60 telemetry frames in flight (a stress case found by hand)."""
    vessel = frame([ID_VESSEL_DATA] + [0] * (VESSEL_DATA_SIZE - 1))
    steps = []
    for n in (1, 2, 3):
        if n == 3:
            for _ in range(60):  # unread frames in flight, then close without waiting for anything
                ser.write(vessel)
        try:
            _reopen(ser)
        except (serial.SerialException, OSError) as e:
            return False, "reopen %d: port could not be reopened: %s" % (n, e)
        t0 = time.monotonic()
        ok, detail = test_handshake(ser)
        if not ok:
            return False, "reopen %d%s: %s" % (n, " (after abrupt close)" if n == 3 else "", detail)
        controls = _feed_and_wait_controls(ser, 2.0)
        if controls < 3:
            return False, "reopen %d: only %d ControlPackets within 2 s after the handshake" % (n, controls)
        steps.append("%d (%.1f s)" % (controls, time.monotonic() - t0))
    return True, "handshake answered and ControlPackets resumed after 3 reopens: %s" % ", ".join(steps)


def test_idle(ser):
    # The firmware drops to idle after IDLETIMER (2000 ms) without a valid packet
    time.sleep(2.6)
    ser.reset_input_buffer()
    frames, _ = read_frames(ser, 0.6)
    controls = [p for p in frames if p[0] == ID_CONTROL]
    if controls:
        return False, "board kept sending ControlPackets after the idle timeout"
    return True, "board went quiet after the idle timeout"


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("port", nargs="?", help="serial port to try first, for example COM3")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--delay", type=float, default=2.0,
                        help="seconds to wait after opening a port, for boards that reset on open")
    args = parser.parse_args()

    candidates = candidate_ports(args.port)
    if not candidates:
        print("No USB serial ports found. Is the Arduino plugged in?")
        return 2

    for device, description in candidates:
        print("Trying %s (%s)" % (device, description))
        try:
            ser = serial.Serial(device, args.baud, timeout=0.05, write_timeout=1)
        except serial.SerialException as e:
            print("  skipped: %s" % e)
            continue
        with ser:
            time.sleep(args.delay)
            ser.reset_input_buffer()
            ok, detail = test_handshake(ser)
            print("  [%s] handshake: %s" % ("PASS" if ok else "FAIL", detail))
            if not ok:
                continue

            print("Controller found on %s" % device)
            failed = False
            for name, test in (("telemetry", test_telemetry), ("debug", test_debug),
                              ("reopen", test_reopen), ("idle timeout", test_idle)):
                ok, detail = test(ser)
                print("  [%s] %s: %s" % ("PASS" if ok else "FAIL", name, detail))
                failed = failed or not ok
            return 1 if failed else 0

    print("No board answered the handshake on any USB serial port.")
    print("Check that the sketch is uploaded (./ci build_sketch only compiles it), that the baud")
    print("rate is %d, and that no other program (Arduino IDE, KSP) holds the port." % args.baud)
    return 2


if __name__ == "__main__":
    sys.exit(main())
