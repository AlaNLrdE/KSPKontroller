#!/usr/bin/env python3
"""Serve kspsim's kRPC stand-in and UDP context responder on 127.0.0.1 only, on non-standard ports.

For checking tools/latency/udp_vs_krpc.py without the game. It never binds 50000-50002 and never broadcasts, so the
Pi's deployed HMI (which discovers servers on UDP 50002) cannot find or be redirected to it. Without the ``krpc``
package (the Pi has no pip) it uses the pure-Python schema in krpc_pb2_lite.py::

    python3 tools/latency/local_sim.py --seconds 120     # 127.0.0.1:51000 RPC, 51001 stream, UDP 51002
"""
from __future__ import annotations

import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))

import krpc_pb2_lite  # noqa: E402

STANDARD_PORTS = (50000, 50001, 50002, 50003)


def start(rpc_port: int = 51000, stream_port: int = 51001, udp_port: int = 51002, scene: str = "flight"):
    """Start a Simulator and its KrpcServer on 127.0.0.1; returns (sim, server). Port 0 = any free port."""
    if {rpc_port, stream_port, udp_port} & set(STANDARD_PORTS):
        raise ValueError("refusing the standard KSP ports %s: the deployed HMI listens/discovers there" % (STANDARD_PORTS,))
    krpc_pb2_lite.install()
    from kspsim.krpcserver import KrpcServer
    from kspsim.sim import Simulator
    sim = Simulator("orbit", 1.0)
    server = KrpcServer(sim, "127.0.0.1", rpc_port, stream_port, context_port=udp_port)
    server.start()
    if server.error:
        raise OSError(server.error)
    if scene != "flight":
        sim.set_scene(scene)
    sim.start()
    return sim, server


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--rpc-port", type=int, default=51000)
    ap.add_argument("--stream-port", type=int, default=51001)
    ap.add_argument("--udp-port", type=int, default=51002)
    ap.add_argument("--scene", default="flight")
    ap.add_argument("--seconds", type=float, default=0, help="quit after this long (0 = until Ctrl+C)")
    args = ap.parse_args(argv)
    sim, server = start(args.rpc_port, args.stream_port, args.udp_port, args.scene)
    print("kspsim on 127.0.0.1: RPC %d, stream %d, UDP context %d (scene %s)" % (
        server.rpc_port, server.stream_port, server.context_port, sim.scene), flush=True)
    end = time.monotonic() + args.seconds if args.seconds else float("inf")
    try:
        while time.monotonic() < end:
            time.sleep(min(1.0, max(0.0, end - time.monotonic())))
    except KeyboardInterrupt:
        pass
    finally:
        server.stop()
        sim.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
