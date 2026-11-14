"""Settings: ``config.toml`` next to the package, then ``config.local.toml`` (per machine, not in git; on the Pi
``~/ksphmi/config.local.toml`` through ``KSPHMI_LOCAL_CONFIG``, so deploys keep it) on top,
then the ``KSPHMI_KRPC_ADDRESS`` environment variable (written by ``./ci deploy_hmi`` with the PC address)."""
from __future__ import annotations

import os
import tomllib
from pathlib import Path

APP_DIR = Path(__file__).resolve().parent.parent

DEFAULTS = {
    "source": "krpc",  # krpc | demo
    "krpc": {"address": "localhost", "rpc_port": 50000, "stream_port": 50001, "rate_hz": 10,
             "retry_seconds": 3, "discovery": True},
    "demo": {"speed": 20},
    "display": {"output": "sdl", "fbdev": "/dev/fb0", "fps": 10, "fullscreen": True, "width": 0, "height": 0},
    "format": {"day_hours": 6},
}


def _merge(base: dict, extra: dict) -> dict:
    out = dict(base)
    for k, v in extra.items():
        out[k] = _merge(out[k], v) if isinstance(v, dict) and isinstance(out.get(k), dict) else v
    return out


def load(path: str | os.PathLike | None = None) -> dict:
    cfg = DEFAULTS
    local = Path(os.environ.get("KSPHMI_LOCAL_CONFIG", APP_DIR / "config.local.toml"))
    files = [Path(path)] if path else [APP_DIR / "config.toml", local]
    for f in files:
        if f.is_file():
            with open(f, "rb") as fh:
                cfg = _merge(cfg, tomllib.load(fh))
    address = os.environ.get("KSPHMI_KRPC_ADDRESS")
    if address:
        cfg = _merge(cfg, {"krpc": {"address": address}})
    return cfg
