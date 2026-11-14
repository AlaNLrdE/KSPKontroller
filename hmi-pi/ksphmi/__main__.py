"""Entry point: ``python -m ksphmi [--demo] [--windowed] [--address HOST] [--screenshot FILE]``."""
from __future__ import annotations

import argparse
import logging
import sys
import time

from . import __version__, config
from .telemetry import DemoSource, KrpcSource


def parse_args(argv):
    ap = argparse.ArgumentParser(prog="ksphmi", description="KSP flight data display (kRPC client)")
    ap.add_argument("--config", help="settings file instead of config.toml + config.local.toml")
    ap.add_argument("--demo", action="store_true", help="simulated orbit, no game needed")
    ap.add_argument("--address", help="kRPC server address (overrides the settings)")
    ap.add_argument("--windowed", action="store_true", help="window instead of full screen (development on the PC)")
    ap.add_argument("--size", help="window or screen size, for example 1920x1080")
    ap.add_argument("--screenshot", metavar="FILE", help="draw for --seconds, save the frame as PNG and exit")
    ap.add_argument("--seconds", type=float, default=1.5, help="how long --screenshot waits for data")
    return ap.parse_args(argv)


def make_source(cfg: dict, args):
    if args.demo or cfg["source"] == "demo":
        return DemoSource(speed=float(cfg["demo"]["speed"]), rate_hz=float(cfg["krpc"]["rate_hz"]))
    k = cfg["krpc"]
    return KrpcSource(address=args.address or k["address"], rpc_port=int(k["rpc_port"]),
                      stream_port=int(k["stream_port"]), rate_hz=float(k["rate_hz"]),
                      retry_seconds=float(k["retry_seconds"]))


def main(argv=None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    cfg = config.load(args.config)
    size = tuple(int(v) for v in args.size.lower().split("x")) if args.size else None

    import pygame  # after argparse, so --help works without pygame

    from .display import Display

    source = make_source(cfg, args)
    logging.info("ksphmi %s, source %s", __version__, type(source).__name__)
    source.start()
    display = Display(cfg, windowed=args.windowed or bool(args.screenshot), size=size)
    logging.info("screen %dx%d", display.w, display.h)
    clock = pygame.time.Clock()
    fps = max(1, int(cfg["display"]["fps"]))
    deadline = time.monotonic() + args.seconds
    try:
        while True:
            for ev in pygame.event.get():
                if ev.type == pygame.QUIT or (ev.type == pygame.KEYDOWN and ev.key in (pygame.K_ESCAPE, pygame.K_q)):
                    return 0
            display.draw(source.latest())
            if args.screenshot and time.monotonic() >= deadline:
                pygame.image.save(display.screen, args.screenshot)
                logging.info("saved %s", args.screenshot)
                return 0
            clock.tick(fps)
    finally:
        source.stop()
        pygame.quit()


if __name__ == "__main__":
    sys.exit(main())
