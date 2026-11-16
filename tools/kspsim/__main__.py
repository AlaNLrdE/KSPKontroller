"""``python -m kspsim``: stand in for KSP + KSPSerialIO + kRPC, to test the Arduino and the Pi HMI without the game.

    ./ci sim                     window, board found by USB id, kRPC on 0.0.0.0:50000/50001
    ./ci sim --port COM5         a given serial port
    ./ci sim --no-board          only the Pi;  --no-krpc: only the board
    ./ci sim --headless          no window, status lines in the console
    ./ci sim --sas normal        start with SAS on in that mode (the vessel slews to it)
"""
from __future__ import annotations

import argparse
import logging
import logging.handlers
import os
import sys
import threading
import time

from . import physics, ships
from .arduino import BoardLink
from .krpcserver import KrpcServer
from .sim import SCENES, Simulator


LOG_FILE = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
                        "build", "kspsim.log")


def setup_logging() -> str:
    """Console plus build/kspsim.log (rotated, 3 x 2 MB), so a session can be read after the fact."""
    fmt = logging.Formatter("%(asctime)s.%(msecs)03d %(name)s: %(message)s", datefmt="%Y-%m-%d %H:%M:%S")
    root = logging.getLogger()
    root.setLevel(logging.INFO)
    console = logging.StreamHandler()
    console.setFormatter(fmt)
    root.addHandler(console)
    try:
        os.makedirs(os.path.dirname(LOG_FILE), exist_ok=True)
        handler = logging.handlers.RotatingFileHandler(LOG_FILE, maxBytes=2_000_000, backupCount=3, encoding="utf-8")
        handler.setFormatter(fmt)
        root.addHandler(handler)
    except OSError as e:
        root.warning("no log file %s: %s", LOG_FILE, e)
    return LOG_FILE


def log_status(board, server, period: float = 5.0) -> None:
    """What the window shows, in the log: on every change, and at least every 30 s."""
    log = logging.getLogger("kspsim.status")
    last, last_at = None, 0.0
    while True:
        time.sleep(period)
        parts = []
        if board:
            b = board.status
            d = b.debug or {}
            age = "never" if not b.last_controls else "%.1f s ago" % (time.monotonic() - b.last_controls)
            parts.append("board %s %s [%s] sent %d recv %d bad %d last ControlPacket %s uptime %s" % (
                b.state, b.port or "-", b.detail.replace("\n", " "), b.sent, b.received, b.bad, age,
                "%d ms" % b.uptime_ms if b.uptime_ms else "--"))
            if d:
                parts.append("debug " + " ".join("%s=%s" % kv for kv in sorted(d.items())))
        if server:
            parts.append("kRPC clients %d" % len(server.clients))
        line = " | ".join(parts)
        key = line.split(" sent ")[0] if board else line  # state/port/detail changes; counters alone do not
        now = time.monotonic()
        if line and (key != last or now - last_at >= 30):
            log.info("%s", line)
            last, last_at = key, now


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
    ap.add_argument("--node", metavar="DV[,SECONDS]",
                    help="start with a prograde maneuver node of DV m/s, SECONDS (default 300) ahead: the HMI's "
                         "dashed orbit (the window's Maneuver node tab changes or deletes it)")
    sas_names = [m.replace(" ", "-") for m in physics.SAS_MODES]
    ap.add_argument("--sas", choices=sas_names, help="start with SAS on in this mode")
    ap.add_argument("--skin", choices=("classic", "apollo", "steampunk", "ksp1", "ksp2"),
                    help="ask the Pi HMI for this look on the UDP 50002 context reply (default: its own config)")
    ap.add_argument("--scene", choices=SCENES, help="start in this game scene (default flight)")
    ap.add_argument("--editor-mode", choices=("construction", "actions", "crew", "cargo"), default="construction")
    ap.add_argument("--ship", choices=[n for n in ships.NAMES if n] + ["none"],
                    help="editor ship the KSPHMI service reports in the VAB (default kerbal_x; none = no service, "
                         "as without KSPSerialIO.Hmi.dll)")
    ap.add_argument("--no-board-throttle", action="store_true",
                    help="ignore the board's throttle axis (engine off unless the window's slider says otherwise)")
    ap.add_argument("--headless", action="store_true", help="no window; print the status every 2 s")
    ap.add_argument("--seconds", type=float, default=0, help="with --headless: quit after this long (0 = never)")
    args = ap.parse_args(argv)
    log_path = setup_logging()
    logging.getLogger("kspsim").info("kspsim started: %s (log %s)", " ".join(sys.argv[1:] if argv is None else argv), log_path)

    sim = Simulator(args.scenario, args.warp)
    sim.board_throttle = not args.no_board_throttle
    if args.skin:
        sim.set_skin(args.skin)
    if args.ship:
        sim.set_ship("" if args.ship == "none" else args.ship)
    sim.set_editor_mode(args.editor_mode)
    if args.node:
        dv, _, seconds = args.node.partition(",")
        sim.set_node(float(dv), float(seconds or 300))
    if args.sas:
        sim.set_game(sas=True)
        sim.sas_mode = sas_names.index(args.sas) + 1
    server = None if args.no_krpc else KrpcServer(sim, args.address, args.rpc_port, args.stream_port)
    board = None if args.no_board else BoardLink(sim, args.port, args.refresh)
    if server:
        sim.listeners.append(lambda scene: server.stop() if scene == "closed" else server.start())
        server.start()
    if args.scene:
        sim.set_scene(args.scene)
    sim.start()
    if board:
        board.start()
    threading.Thread(target=log_status, args=(board, server), name="StatusLog", daemon=True).start()
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
