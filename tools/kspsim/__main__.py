"""``python -m kspsim``: stand in for KSP + KSPSerialIO + kRPC, to test the Arduino and the Pi HMI without the game.

    ./ci sim                     window, board found by USB id, kRPC on 0.0.0.0:50000/50001
    ./ci sim --port COM5         a given serial port
    ./ci sim --no-board          only the Pi;  --no-krpc: only the board
    ./ci sim --headless          no window, status lines in the console
"""
from __future__ import annotations

import argparse
import logging
import sys
import time

from . import physics
from .arduino import BoardLink
from .krpcserver import KrpcServer
from .sim import Simulator


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="kspsim", description=__doc__.splitlines()[0])
    ap.add_argument("--port", help="Arduino serial port (default: the first Arduino USB port)")
    ap.add_argument("--no-board", action="store_true", help="do not open the serial port")
    ap.add_argument("--no-krpc", action="store_true", help="do not open the kRPC ports")
    ap.add_argument("--address", default="0.0.0.0", help="kRPC listen address (default all interfaces)")
    ap.add_argument("--rpc-port", type=int, default=50000)
    ap.add_argument("--stream-port", type=int, default=50001)
    ap.add_argument("--refresh", type=float, default=0.2, help="seconds between VesselData packets (plugin default)")
    ap.add_argument("--scenario", default="orbit", choices=physics.SCENARIOS)
    ap.add_argument("--warp", type=float, default=1.0)
    ap.add_argument("--headless", action="store_true", help="no window; print the status every 2 s")
    ap.add_argument("--seconds", type=float, default=0, help="with --headless: quit after this long (0 = never)")
    args = ap.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s: %(message)s", datefmt="%H:%M:%S")

    sim = Simulator(args.scenario, args.warp)
    server = None if args.no_krpc else KrpcServer(sim, args.address, args.rpc_port, args.stream_port)
    board = None if args.no_board else BoardLink(sim, args.port, args.refresh)
    if server:
        sim.listeners.append(lambda scene: server.stop() if scene == "closed" else server.start())
        server.start()
    sim.start()
    if board:
        board.start()
    try:
        if args.headless:
            end = time.monotonic() + args.seconds if args.seconds else float("inf")
            while time.monotonic() < end:
                time.sleep(min(2.0, max(0.0, end - time.monotonic())))
                with sim.lock:
                    v = sim.vessel
                    el = v.elements()
                    line = "%s alt %.1f km Ap %.1f km Pe %.1f km thr %d%%" % (
                        v.situation(), v.altitude / 1e3, el.apoapsis / 1e3, el.periapsis / 1e3, v.controls.throttle * 100)
                if board:
                    b = board.status
                    line += " | board %s %s sent %d recv %d" % (b.state, b.port, b.sent, b.received)
                if server:
                    line += " | kRPC clients %d" % len(server.clients)
                print(line, flush=True)
        else:
            from .app import App
            App(sim, board, server).run()
    except KeyboardInterrupt:
        pass
    finally:
        if board:
            board.stop()
        if server:
            server.stop()
        sim.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
