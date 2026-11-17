#!/usr/bin/env python3
"""Compare round-trip latency from the HMI host to KSP1: UDP context responder vs kRPC (stdlib only).

Paths measured, one after the other so they never load the game at the same time:

``udp``     KSPSerialIO's context responder (docs/communication.rst "UDP scene context and discovery"): the exact
            datagram ``KSPHMI/1`` (no newline), reply ``KSPHMI/1 <scene>``. Paced, one request outstanding,
            per-request timeout; the plugin reads at most four datagrams per Unity frame, so the RTT is
            frame-bound (about 0.5-1.5 frames plus the network) and never floods it.
``rpc``     kRPC RPC port round trips of ``SpaceCenter.get_UT`` (and, with ``--vessel``, ``get_ActiveVessel`` and
            ``Vessel_get_Name`` as separate samples). kRPC also serves requests from Unity's update loop.
``stream``  a kRPC stream of ``SpaceCenter.get_UT`` at the HMI's rate (``rate_hz`` 10): update arrival interval,
            UT step per update and relative age (local arrival clock minus game UT, after a linear fit so a game
            running slower than real time does not count as age; minimum shifted to 0).

The kRPC handshake mirrors hmi-cpp/src/krpc.cpp (ConnectionRequest with protocol_version 1, then the stream port
with the RPC client identifier; AddStream + SetStreamRate). The client name is ``KSP HMI latency probe`` so it is
easy to tell from the real HMI in kRPC's client list.

Usage in a game session (run on the Pi from the repo checkout; KSP1 running on the PC, kRPC server started)::

    python3 tools/latency/udp_vs_krpc.py --host Alan-TR.local --label flight --fps 60
    python3 tools/latency/udp_vs_krpc.py --host Alan-TR.local --label space_center --paths udp --fps 60

Note KSP's FPS by hand (``--fps``, e.g. from the in-game counter): the summary then also gives the medians in
frames. ``get_UT`` and its stream need the flight scene in kspsim; in KSP it works everywhere kRPC is up. Results go
to ``build/latency/<UTC timestamp>_<label>.json`` (summary + raw samples) and ``.csv`` (raw samples).

Local check against the simulator (127.0.0.1 and non-standard ports only, no discovery; never 50000-50002 on the
Pi, whose deployed HMI discovers servers on UDP 50002)::

    python3 tools/latency/local_sim.py &          # 127.0.0.1:51000/51001, UDP 51002
    python3 tools/latency/udp_vs_krpc.py --host 127.0.0.1 --rpc-port 51000 --stream-port 51001 --udp-port 51002 \\
        --label kspsim
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import math
import os
import platform
import socket
import statistics
import struct
import subprocess
import sys
import threading
import time

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
CLIENT_NAME = "KSP HMI latency probe"
PROTOCOL_VERSION = 1
CONTEXT_REQUEST = b"KSPHMI/1"


# ---------------------------------------------------------------------------------------------- statistics
def percentile(sorted_values, q: float) -> float:
    """Linear interpolation between closest ranks (numpy's default); ``sorted_values`` must be sorted."""
    if not sorted_values:
        return math.nan
    pos = (len(sorted_values) - 1) * q
    lo = int(math.floor(pos))
    hi = min(lo + 1, len(sorted_values) - 1)
    return sorted_values[lo] + (sorted_values[hi] - sorted_values[lo]) * (pos - lo)


def summarize(values, timeouts: int = 0, errors: int = 0) -> dict:
    """n, min, mean, median, p90, p99, max, jitter (stdev and mean |successive diff|) of ``values`` in time order."""
    vals = [float(v) for v in values]
    s = sorted(vals)
    diffs = [abs(b - a) for a, b in zip(vals, vals[1:])]
    nan = math.nan
    return {
        "n": len(vals), "timeouts": timeouts, "errors": errors,
        "min": s[0] if s else nan, "mean": statistics.fmean(vals) if vals else nan,
        "median": percentile(s, 0.5), "p90": percentile(s, 0.9), "p99": percentile(s, 0.99),
        "max": s[-1] if s else nan,
        "stdev": statistics.stdev(vals) if len(vals) > 1 else nan,
        "mean_abs_diff": statistics.fmean(diffs) if diffs else nan,
    }


def linear_fit(xs, ys):
    """Least squares ``y = a + b x``; returns (a, b)."""
    mx, my = statistics.fmean(xs), statistics.fmean(ys)
    sxx = sum((x - mx) ** 2 for x in xs)
    b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx if sxx else 1.0
    return my - b * mx, b


# ---------------------------------------------------------------------------------------------- protobuf
def varint(n: int) -> bytes:
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def read_varint(data: bytes, i: int = 0):
    n = shift = 0
    while True:
        if i >= len(data):
            raise ValueError("truncated varint")
        b = data[i]
        i += 1
        n |= (b & 0x7F) << shift
        if not b & 0x80:
            return n, i
        shift += 7


def field_varint(num: int, value: int) -> bytes:
    return varint(num << 3) + varint(value)


def field_bytes(num: int, value) -> bytes:
    b = value.encode("utf-8") if isinstance(value, str) else bytes(value)
    return varint(num << 3 | 2) + varint(len(b)) + b


def fields(data: bytes):
    """Yield (field number, wire type, value) of a protobuf message (varint, 64-bit, length-delimited, 32-bit)."""
    i = 0
    while i < len(data):
        tag, i = read_varint(data, i)
        num, wire = tag >> 3, tag & 7
        if wire == 0:
            value, i = read_varint(data, i)
        elif wire == 1:
            value, i = data[i:i + 8], i + 8
        elif wire == 2:
            n, i = read_varint(data, i)
            value, i = data[i:i + n], i + n
        elif wire == 5:
            value, i = data[i:i + 4], i + 4
        else:
            raise ValueError("unsupported wire type %d" % wire)
        yield num, wire, value


def connection_request(typ: int, name: str, identifier: bytes) -> bytes:
    """ConnectionRequest: type 1 (RPC 0 / STREAM 1), client_name 2, client_identifier 3, protocol_version 4."""
    out = b""
    if typ:
        out += field_varint(1, typ)
    if name:
        out += field_bytes(2, name)
    if identifier:
        out += field_bytes(3, identifier)
    return out + field_varint(4, PROTOCOL_VERSION)


def encode_call(service: str, procedure: str, args=()) -> bytes:
    """ProcedureCall: service 1, procedure 2, arguments 3 (Argument: position 1, value 2), as encode_call()."""
    out = field_bytes(1, service) + field_bytes(2, procedure)
    for i, arg in enumerate(args):
        out += field_bytes(3, (field_varint(1, i) if i else b"") + field_bytes(2, arg))
    return out


class RpcError(Exception):
    pass


def decode_error(data: bytes) -> RpcError:
    parts = {num: bytes(v).decode("utf-8", "replace") for num, wire, v in fields(data) if wire == 2}
    return RpcError("%s.%s: %s" % (parts.get(1, ""), parts.get(2, ""), parts.get(3, "")))


def decode_result(data: bytes):
    """ProcedureResult: error 1, value 2, is_null 3. Returns the value bytes or None."""
    value, null = b"", False
    for num, wire, v in fields(data):
        if num == 1 and wire == 2:
            raise decode_error(v)
        if num == 2 and wire == 2:
            value = bytes(v)
        if num == 3 and wire == 0:
            null = v != 0
    return None if null else value


def dec_double(b: bytes) -> float:
    return struct.unpack("<d", b)[0]


def dec_string(b: bytes) -> str:
    n, i = read_varint(b)
    return b[i:i + n].decode("utf-8")


def enc_float(v: float) -> bytes:
    return struct.pack("<f", v)


# ---------------------------------------------------------------------------------------------- kRPC client
def recv_exact(sock, n: int) -> bytes:
    data = bytearray()
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise ConnectionError("connection closed by the server")
        data += chunk
    return bytes(data)


def recv_message(sock) -> bytes:
    prefix = bytearray()
    while True:
        prefix += recv_exact(sock, 1)
        if not prefix[-1] & 0x80:
            break
        if len(prefix) > 10:
            raise ConnectionError("malformed message size")
    size, _ = read_varint(bytes(prefix))
    if size > 64 << 20:
        raise ConnectionError("message too large")
    return recv_exact(sock, size)


def send_message(sock, data: bytes) -> None:
    sock.sendall(varint(len(data)) + data)


def check_response(data: bytes, want_version: bool) -> bytes:
    status, text, ident, version = 0, "", b"", 0
    for num, wire, v in fields(data):
        if num == 1 and wire == 0:
            status = v
        elif num == 2 and wire == 2:
            text = bytes(v).decode("utf-8", "replace")
        elif num == 3 and wire == 2:
            ident = bytes(v)
        elif num == 4 and wire == 0:
            version = v
    if status:
        raise ConnectionError(text or "connection refused by kRPC (status %d)" % status)
    if want_version and version != PROTOCOL_VERSION:
        raise ConnectionError("server protocol version %d, expected %d" % (version, PROTOCOL_VERSION))
    return ident


class KrpcClient:
    """RPC + stream connections as the HMI opens them; a reader thread timestamps every StreamUpdate."""

    def __init__(self, host: str, rpc_port: int, stream_port: int, timeout: float) -> None:
        self.rpc = socket.create_connection((host, rpc_port), timeout=timeout)
        self.rpc.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        send_message(self.rpc, connection_request(0, CLIENT_NAME, b""))
        ident = check_response(recv_message(self.rpc), True)
        self.stream = socket.create_connection((host, stream_port), timeout=timeout)
        self.stream.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        send_message(self.stream, connection_request(1, "", ident))
        check_response(recv_message(self.stream), False)
        self.stream.settimeout(None)
        self.updates = []  # (monotonic, wall, stream id, result bytes)
        self.updates_lock = threading.Lock()
        self.closed = False
        self.stream_error = ""
        threading.Thread(target=self._stream_loop, daemon=True, name="krpc-stream").start()

    def invoke(self, service: str, procedure: str, args=()):
        send_message(self.rpc, field_bytes(1, encode_call(service, procedure, args)))  # Request.calls 1
        response = recv_message(self.rpc)
        results = []
        for num, wire, v in fields(response):
            if num == 1 and wire == 2:
                raise decode_error(v)
            if num == 2 and wire == 2:
                results.append(v)
        if not results:
            raise ConnectionError("kRPC response without a result")
        return decode_result(results[0])

    def add_stream(self, service: str, procedure: str, rate: float, args=()) -> int:
        value = self.invoke("KRPC", "AddStream", [encode_call(service, procedure, args)])
        sid = next((v for num, wire, v in fields(value or b"") if num == 1 and wire == 0), 0)
        if rate > 0:
            try:
                self.invoke("KRPC", "SetStreamRate", [varint(sid), enc_float(rate)])
            except RpcError:
                pass  # very old servers have no per-stream rate
        return sid

    def remove_stream(self, sid: int) -> None:
        self.invoke("KRPC", "RemoveStream", [varint(sid)])

    def _stream_loop(self) -> None:
        try:
            while not self.closed:
                update = recv_message(self.stream)
                now, wall = time.monotonic(), time.time()
                with self.updates_lock:
                    for num, wire, v in fields(update):  # StreamUpdate.results 1: StreamResult id 1, result 2
                        if num != 1 or wire != 2:
                            continue
                        sid, result = 0, b""
                        for n2, w2, v2 in fields(v):
                            if n2 == 1 and w2 == 0:
                                sid = v2
                            elif n2 == 2 and w2 == 2:
                                result = bytes(v2)
                        self.updates.append((now, wall, sid, result))
        except (OSError, ConnectionError, ValueError) as e:
            if not self.closed:
                self.stream_error = str(e)

    def close(self) -> None:
        self.closed = True
        for s in (self.rpc, self.stream):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            s.close()


# ---------------------------------------------------------------------------------------------- measurements
class Pacer:
    """Fixed-rate schedule; a late tick starts the next interval from now (never bursts to catch up)."""

    def __init__(self, rate: float) -> None:
        self.period = 1.0 / rate
        self.next = time.monotonic()

    def wait(self) -> None:
        now = time.monotonic()
        if self.next > now:
            time.sleep(self.next - now)
            self.next += self.period
        else:
            self.next = now + self.period


def measure_udp(host: str, port: int, count: int, rate: float, timeout: float, log) -> dict:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("", 0))
    samples, late, scenes = [], 0, {}
    pacer = Pacer(rate)
    try:
        for seq in range(count):
            pacer.wait()
            sock.setblocking(False)  # drop replies that arrived after their request timed out
            try:
                while True:
                    sock.recvfrom(512)
                    late += 1
            except (BlockingIOError, InterruptedError):
                pass
            except OSError:  # e.g. ICMP port unreachable reported on the next call
                pass
            sock.setblocking(True)
            wall = time.time()
            t0 = time.perf_counter()
            status, rtt, reply = "timeout", None, ""
            try:
                sock.sendto(CONTEXT_REQUEST, (host, port))
                deadline = t0 + timeout
                while True:
                    left = deadline - time.perf_counter()
                    if left <= 0:
                        break
                    sock.settimeout(left)
                    try:
                        data, peer = sock.recvfrom(512)
                    except socket.timeout:
                        break
                    t1 = time.perf_counter()
                    if peer[0] == host and data.startswith(b"KSPHMI/1 "):
                        status, rtt = "ok", (t1 - t0) * 1000.0
                        reply = data.decode("ascii", "replace")
                        scene = reply[9:].split(" ")[0]
                        scenes[scene] = scenes.get(scene, 0) + 1
                        break
            except OSError as e:
                status, reply = "error", str(e)
            samples.append({"seq": seq, "wall": wall, "status": status, "rtt_ms": rtt, "reply": reply})
            if seq and seq % 100 == 0:
                log("udp %d/%d" % (seq, count))
    finally:
        sock.close()
    rtts = [s["rtt_ms"] for s in samples if s["status"] == "ok"]
    summary = summarize(rtts, sum(s["status"] == "timeout" for s in samples), sum(s["status"] == "error" for s in samples))
    summary.update(late_replies=late, scenes=scenes, unit="ms")
    return {"summary": summary, "samples": samples}


def connect(args, log):
    for attempt in range(args.reconnects + 1):
        try:
            return KrpcClient(args.ip, args.rpc_port, args.stream_port, args.rpc_timeout)
        except (OSError, ConnectionError) as e:
            log("kRPC connect failed (%s)%s" % (e, ", retrying in 3 s" if attempt < args.reconnects else ""))
            if attempt < args.reconnects:
                time.sleep(3)
    return None


def measure_rpc(args, log) -> dict:
    metrics = ["get_UT"] + (["get_ActiveVessel", "Vessel_get_Name"] if args.vessel else [])
    samples, counts = [], {m: [0, 0] for m in metrics}  # timeouts, errors
    client = connect(args, log)
    pacer = Pacer(args.rpc_rate)
    reconnects = args.reconnects
    try:
        for seq in range(args.count):
            if client is None:
                break
            pacer.wait()
            vessel = None
            for metric in metrics:
                if metric == "Vessel_get_Name" and vessel is None:
                    continue
                call_args = [vessel] if metric == "Vessel_get_Name" else []
                wall = time.time()
                t0 = time.perf_counter()
                status, rtt, value = "ok", None, ""
                try:
                    result = client.invoke("SpaceCenter", metric, call_args)
                    rtt = (time.perf_counter() - t0) * 1000.0
                    if metric == "get_UT":
                        value = "%.3f" % dec_double(result)
                    elif metric == "get_ActiveVessel":
                        vessel = result
                        value = "null" if result is None else str(read_varint(result)[0])
                    else:
                        value = dec_string(result)
                except RpcError as e:
                    status, value = "rpc_error", str(e)
                    rtt = (time.perf_counter() - t0) * 1000.0
                    counts[metric][1] += 1
                except socket.timeout:
                    status = "timeout"
                    counts[metric][0] += 1
                except (OSError, ConnectionError) as e:
                    status, value = "error", str(e)
                    counts[metric][1] += 1
                samples.append({"seq": seq, "metric": metric, "wall": wall, "status": status, "rtt_ms": rtt,
                                "value": value})
                if status in ("timeout", "error"):  # the RPC channel is out of step: start a new session
                    client.close()
                    client = None
                    if reconnects > 0:
                        reconnects -= 1
                        time.sleep(3)
                        client = connect(args, log)
                    break
            if seq and seq % 100 == 0:
                log("rpc %d/%d" % (seq, args.count))
    finally:
        if client:
            client.close()
    result = {"samples": samples, "summary": {}}
    for metric in metrics:
        rtts = [s["rtt_ms"] for s in samples if s["metric"] == metric and s["status"] == "ok"]
        result["summary"][metric] = dict(summarize(rtts, *counts[metric]), unit="ms")
    return result


def measure_stream(args, log) -> dict:
    client = connect(args, log)
    if client is None:
        return {"summary": {"error": "no kRPC connection"}, "samples": []}
    gap_limit = max(0.5, 5.0 / args.stream_rate)
    try:
        try:
            sid = client.add_stream("SpaceCenter", "get_UT", args.stream_rate)
        except RpcError as e:
            return {"summary": {"error": "AddStream get_UT: %s" % e}, "samples": []}
        deadline = time.monotonic() + args.count / args.stream_rate * 3 + 10
        while time.monotonic() < deadline and not client.stream_error:
            with client.updates_lock:
                n = sum(1 for u in client.updates if u[2] == sid)
            if n >= args.count + 1:
                break
            time.sleep(0.05)
        try:
            client.remove_stream(sid)
        except (RpcError, OSError, ConnectionError):
            pass
        with client.updates_lock:
            updates = [u for u in client.updates if u[2] == sid][1:args.count + 1]  # first one: AddStream's value
        error = client.stream_error
    finally:
        client.close()
    samples = []
    for i, (mono, wall, _, result) in enumerate(updates):
        try:
            ut = dec_double(decode_result(result))
        except (RpcError, struct.error, TypeError):
            continue
        samples.append({"seq": i, "mono": mono, "wall": wall, "ut": ut})
    out = {"samples": samples, "summary": {}}
    if len(samples) < 3:
        out["summary"]["error"] = error or "fewer than 3 stream updates"
        return out
    intervals = [(b["mono"] - a["mono"]) * 1000.0 for a, b in zip(samples, samples[1:])]
    steps = [(b["ut"] - a["ut"]) * 1000.0 for a, b in zip(samples, samples[1:])]
    a, b = linear_fit([s["ut"] for s in samples], [s["mono"] for s in samples])
    resid = [s["mono"] - (a + b * s["ut"]) for s in samples]
    base = min(resid)
    raw = [s["mono"] - s["ut"] for s in samples]
    for s, r, w in zip(samples, resid, raw):
        s["age_ms"] = (r - base) * 1000.0
        s["raw_age_ms"] = (w - min(raw)) * 1000.0
    for s, iv, st in zip(samples[1:], intervals, steps):
        s["interval_ms"], s["ut_step_ms"] = iv, st
    gaps = sum(iv > gap_limit * 1000.0 for iv in intervals)
    out["summary"] = {
        "interval": dict(summarize(intervals, gaps), unit="ms", gap_limit_ms=gap_limit * 1000.0,
                         requested_rate_hz=args.stream_rate),
        "ut_step": dict(summarize(steps), unit="game ms", repeated_values=sum(st == 0 for st in steps)),
        "age": dict(summarize([s["age_ms"] for s in samples]), unit="ms",
                    note="arrival minus UT after a linear fit, minimum shifted to 0 (relative, not absolute)"),
        "wall_seconds_per_ut_second": b,
        "stream_error": error,
    }
    return out


# ---------------------------------------------------------------------------------------------- environment
def interface_for(ip: str) -> dict:
    """Local address and interface used to reach ``ip`` (Linux ``ip route get``; else the local address only)."""
    info = {"local_address": "", "interface": ""}
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect((ip, 9))  # no packet is sent: this only selects the route
        info["local_address"] = s.getsockname()[0]
        s.close()
    except OSError:
        pass
    try:
        out = subprocess.run(["ip", "route", "get", ip], capture_output=True, text=True, timeout=2).stdout.split()
        if "dev" in out:
            info["interface"] = out[out.index("dev") + 1]
        if "via" in out:
            info["gateway"] = out[out.index("via") + 1]
    except (OSError, subprocess.SubprocessError):
        pass
    return info


def fmt_row(name: str, s: dict, fps: float | None) -> str:
    if "n" not in s:
        return "%-24s %s" % (name, s.get("error", "no data"))
    row = "%-24s n %4d  med %7.2f  p90 %7.2f  p99 %7.2f  max %7.2f  sd %6.2f  |d| %6.2f  timeouts %d" % (
        name, s["n"], s["median"], s["p90"], s["p99"], s["max"], s["stdev"], s["mean_abs_diff"], s["timeouts"])
    if s.get("errors"):
        row += "  errors %d" % s["errors"]
    if fps and s.get("unit") == "ms" and not math.isnan(s["median"]):
        row += "  (%.2f frames)" % (s["median"] * fps / 1000.0)
    return row


def write_outputs(report: dict, out_dir: str, stem: str) -> tuple:
    os.makedirs(out_dir, exist_ok=True)
    json_path = os.path.join(out_dir, stem + ".json")
    csv_path = os.path.join(out_dir, stem + ".csv")
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=1, allow_nan=True)
    cols = ["path", "metric", "seq", "wall", "status", "value_ms", "detail"]
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(cols)
        res = report["results"]
        for s in res.get("udp", {}).get("samples", []):
            w.writerow(["udp", "context", s["seq"], "%.6f" % s["wall"], s["status"], s["rtt_ms"], s["reply"]])
        for s in res.get("rpc", {}).get("samples", []):
            w.writerow(["rpc", s["metric"], s["seq"], "%.6f" % s["wall"], s["status"], s["rtt_ms"], s["value"]])
        for s in res.get("stream", {}).get("samples", []):
            for metric in ("interval_ms", "age_ms", "ut_step_ms"):
                if metric in s:
                    w.writerow(["stream", metric[:-3], s["seq"], "%.6f" % s["wall"], "ok", s[metric],
                                "ut=%.4f" % s["ut"]])
    return json_path, csv_path


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--host", required=True, help="KSP PC (e.g. Alan-TR.local) or 127.0.0.1 for kspsim")
    ap.add_argument("--udp-port", type=int, default=50002)
    ap.add_argument("--rpc-port", type=int, default=50000)
    ap.add_argument("--stream-port", type=int, default=50001)
    ap.add_argument("--paths", default="udp,rpc,stream", help="comma list of udp, rpc, stream (run in this order)")
    ap.add_argument("--count", type=int, default=500, help="samples per path (default 500)")
    ap.add_argument("--udp-rate", type=float, default=20.0, help="UDP requests per second (default 20)")
    ap.add_argument("--udp-timeout", type=float, default=0.5, help="per-request UDP timeout, s (default 0.5)")
    ap.add_argument("--rpc-rate", type=float, default=20.0, help="RPC ticks per second (default 20)")
    ap.add_argument("--rpc-timeout", type=float, default=2.0, help="kRPC socket timeout, s (default 2)")
    ap.add_argument("--vessel", action="store_true", help="also time get_ActiveVessel + Vessel_get_Name per tick")
    ap.add_argument("--stream-rate", type=float, default=10.0, help="UT stream rate, Hz (default 10, the HMI's)")
    ap.add_argument("--reconnects", type=int, default=2, help="kRPC reconnect attempts (scene changes)")
    ap.add_argument("--label", default="", help="scene/session label, e.g. flight or space_center")
    ap.add_argument("--fps", type=float, help="KSP FPS noted by hand; medians are also shown in frames")
    ap.add_argument("--note", default="", help="free text stored with the results")
    ap.add_argument("--out-dir", default=os.path.join(REPO, "build", "latency"))
    args = ap.parse_args(argv)
    paths = [p.strip() for p in args.paths.split(",") if p.strip()]
    if any(p not in ("udp", "rpc", "stream") for p in paths) or args.count < 1:
        ap.error("--paths takes udp, rpc, stream; --count must be positive")
    try:
        args.ip = socket.getaddrinfo(args.host, None, socket.AF_INET)[0][4][0]  # resolve mDNS once
    except OSError as e:
        print("cannot resolve %s: %s" % (args.host, e), file=sys.stderr)
        return 2

    def log(text: str) -> None:
        print(text, file=sys.stderr, flush=True)

    started = dt.datetime.now(dt.timezone.utc)
    report = {
        "tool": "tools/latency/udp_vs_krpc.py", "host": args.host, "address": args.ip,
        "timestamp": started.isoformat(timespec="seconds"), "label": args.label, "fps": args.fps, "note": args.note,
        "client": platform.node(), "network": interface_for(args.ip),
        "ports": {"udp": args.udp_port, "rpc": args.rpc_port, "stream": args.stream_port},
        "settings": {k: getattr(args, k) for k in ("count", "udp_rate", "udp_timeout", "rpc_rate", "rpc_timeout",
                                                    "vessel", "stream_rate")},
        "notes": ["UDP context replies come from Unity's frame loop (<= 4 datagrams per frame): RTT is frame-bound.",
                  "kRPC RPCs are also served from Unity's update loop; stream age is relative (linear fit)."],
        "results": {},
    }
    log("%s (%s) via %s, label %r" % (args.host, args.ip, report["network"].get("interface") or "?", args.label))
    for path in paths:
        log("measuring %s ..." % path)
        if path == "udp":
            report["results"]["udp"] = measure_udp(args.ip, args.udp_port, args.count, args.udp_rate,
                                                   args.udp_timeout, log)
        elif path == "rpc":
            report["results"]["rpc"] = measure_rpc(args, log)
        else:
            report["results"]["stream"] = measure_stream(args, log)
    stem = started.strftime("%Y%m%dT%H%M%SZ") + ("_" + "".join(c if c.isalnum() or c in "-_" else "_"
                                                                for c in args.label) if args.label else "")
    json_path, csv_path = write_outputs(report, args.out_dir, stem)

    print("host %s (%s)  %s  iface %s (%s)  label %s  fps %s" % (
        args.host, args.ip, report["timestamp"], report["network"].get("interface") or "?",
        report["network"].get("local_address") or "?", args.label or "-", args.fps or "-"))
    res = report["results"]
    if "udp" in res:
        s = res["udp"]["summary"]
        print(fmt_row("udp context RTT ms", s, args.fps) + "  late %d  scenes %s" % (s["late_replies"], s["scenes"]))
    for metric, s in res.get("rpc", {}).get("summary", {}).items():
        print(fmt_row("rpc %s ms" % metric, s, args.fps))
    st = res.get("stream", {}).get("summary", {})
    if "interval" in st:
        print(fmt_row("stream interval ms", st["interval"], None) + " (gaps > %.0f ms)" % st["interval"]["gap_limit_ms"])
        print(fmt_row("stream UT step game ms", st["ut_step"], None) + "  repeated %d" % st["ut_step"]["repeated_values"])
        print(fmt_row("stream rel. age ms", st["age"], None) + "  wall s per UT s %.4f" % st["wall_seconds_per_ut_second"])
    elif "stream" in res:
        print("stream: %s" % st.get("error"))
    print("UDP RTT is frame-bound (the plugin answers from Unity's frame loop); so are kRPC calls.")
    print("raw samples: %s\n             %s" % (json_path, csv_path))
    return 0


if __name__ == "__main__":
    sys.exit(main())
