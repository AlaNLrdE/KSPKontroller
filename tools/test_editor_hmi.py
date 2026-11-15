#!/usr/bin/env python3
"""Real C++ HMI against kspsim's editor reports, no KSP or serial hardware.

Run on Linux/WSL after building: python3 tools/test_editor_hmi.py build/hmi-cpp/ksphmi
The ci test_hmi --sim target runs this after the flight scenario.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import time

from kspsim.krpcserver import KrpcServer
from kspsim.sim import EDITOR_MODES, Simulator


def main():
    root = Path(__file__).resolve().parent.parent
    binary = Path(sys.argv[1]).resolve()
    out = root / "build" / "editor-hmi-sim"
    out.mkdir(parents=True, exist_ok=True)
    (out / "smoke.json").unlink(missing_ok=True)
    sim = Simulator()
    sim.set_scene("vab")
    server = KrpcServer(sim, "127.0.0.1", 0, 0)
    server.start()
    if server.error:
        raise RuntimeError(server.error)
    config = out / "isolated.toml"
    config.write_text("[krpc]\ndiscovery = false\nrpc_port = %d\nstream_port = %d\n" %
                      (server.rpc_port, server.stream_port), encoding="utf-8")
    checked = []
    try:
        for skin in ("classic", "apollo"):
            sim.set_skin(skin)
            sim.set_editor_mode("construction")
            path = out / (skin + ".log")
            screenshot = out / (skin + "_cargo.png")
            screenshot.unlink(missing_ok=True)
            with path.open("w", encoding="utf-8") as log:
                environment = dict(os.environ, KSPHMI_LOCAL_CONFIG="/dev/null")
                process = subprocess.Popen([str(binary), "--address", "127.0.0.1", "--config", str(config),
                    "--skin", skin, "--offscreen", "--size", "1260x840", "--seconds", "12",
                    "--screenshot", str(out / (skin + "_cargo.png"))], stdout=log, stderr=log, env=environment)
                try:
                    for mode in EDITOR_MODES:
                        sim.set_editor_mode(mode)
                        deadline = time.monotonic() + 7
                        while "mode " + mode not in path.read_text(encoding="utf-8"):
                            if process.poll() is not None or time.monotonic() > deadline:
                                raise RuntimeError(skin + " never displayed " + mode + ":\n" + path.read_text(encoding="utf-8"))
                            time.sleep(.1)
                        checked.append(skin + ":" + mode)
                    # Let the final snapshot pass through the renderer before requesting shutdown/screenshot.
                    time.sleep(.6)
                    if process.poll() is not None:
                        raise RuntimeError(skin + " exited before all screens were reviewed")
                    code = process.wait(timeout=15)
                    if not screenshot.is_file() or screenshot.stat().st_size < 24:
                        raise RuntimeError(skin + " did not present/save its final screen")
                    if code != 0:
                        raise RuntimeError(skin + " exited with " + str(code))
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait(timeout=5)
    finally:
        server.stop()
    (out / "smoke.json").write_text(json.dumps({"passed": True, "screens": checked}, indent=2), encoding="utf-8")
    print("PASS editor HMI: " + ", ".join(checked))


if __name__ == "__main__":
    main()
