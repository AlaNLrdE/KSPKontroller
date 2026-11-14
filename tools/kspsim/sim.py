"""Simulator state shared by the board link, the kRPC stand-in and the window: one lock, one clock."""
from __future__ import annotations

import logging
import subprocess
import threading
import time

from . import physics

log = logging.getLogger("kspsim")

SCENES = ("flight", "space_center", "closed")  # closed = KSP not running: kRPC ports closed, no telemetry


class Simulator:
    def __init__(self, scenario: str = "orbit", warp: float = 1.0, tick: float = 0.05) -> None:
        self.lock = threading.RLock()
        self.vessel: physics.Vessel | None = None
        self.vessel_uid = 9  # object ids derive from it; a new vessel gets a new uid, as a switch in KSP does
        self.scene = "flight"
        self.warp = warp
        self.ut = 0.0
        self.tick = tick
        self.board_driven = False       # True while ControlPackets arrive (the window's controls step aside)
        self.manual = physics.Controls()
        self.board_throttle = True      # False: the engine follows the window's throttle even with the board linked
        self.last_board = physics.Controls()  # what the board sent last, shown in the window as received
        self.manual_values: dict | None = None  # manual mode: physics frozen, these values reported as typed
        self._last_board = 0.0
        self._ksp_check = (0.0, False)
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="Simulator", daemon=True)
        self.listeners: list = []       # callables(scene) told when the scene changes (the kRPC server)
        self.load(scenario)

    # -- control from the window --------------------------------------------------------------------
    def load(self, scenario: str) -> None:
        with self.lock:
            self.vessel = physics.scenario(scenario)
            self.vessel_uid += 1
            if not self.board_driven:
                self.vessel.apply_controls(physics.Controls(**vars(self.manual)))
        log.info("scenario %s (vessel uid %d)", scenario, self.vessel_uid)

    def set_orbit(self, apoapsis: float, periapsis: float, inclination: float, true_anomaly: float) -> None:
        """Move the current vessel (same vessel, no switch) onto a new orbit; ValueError explains a bad request."""
        with self.lock:
            physics.set_orbit(self.vessel, apoapsis, periapsis, inclination, true_anomaly)
        log.info("orbit set: Ap %.1f km, Pe %.1f km, inc %.2f deg, true anomaly %.0f deg",
                 apoapsis / 1e3, periapsis / 1e3, inclination, true_anomaly)

    def set_vessel(self, name: str | None = None, stage_fuel: float | None = None) -> None:
        with self.lock:
            if name:
                self.vessel.name = name
            if stage_fuel is not None:
                f = max(0.0, min(100.0, stage_fuel)) / 100
                self.vessel.liquid_fuel = physics.LIQUID_FUEL * f
                self.vessel.oxidizer = physics.OXIDIZER * f

    # -- manual values --------------------------------------------------------------------------------
    def reported(self) -> dict:
        """The values the board and the HMI receive: typed ones in manual mode, else the flight model's."""
        with self.lock:
            values = self.vessel.report()
            if self.manual_values is not None:
                values.update(self.manual_values)
            return values

    def set_manual_mode(self, on: bool) -> None:
        """Manual mode starts from the current values, so only what is changed differs."""
        with self.lock:
            if on and self.manual_values is None:
                self.manual_values = {k: v for k, v in self.vessel.report().items() if k != "name"}
            elif not on:
                self.manual_values = None
        log.info("manual values %s", "on (physics frozen)" if on else "off")

    def set_manual_values(self, **values) -> None:
        with self.lock:
            if self.manual_values is None:
                raise ValueError("manual mode is off")
            situation = values.get("situation")
            if situation is not None and situation not in physics.SITUATIONS:
                raise ValueError("situation must be one of " + ", ".join(physics.SITUATIONS))
            unknown = set(values) - {k for k, *_ in physics.REPORTED_FIELDS} - {"situation"}
            if unknown:
                raise ValueError("unknown value(s): " + ", ".join(sorted(unknown)))
            self.manual_values.update(values)

    def set_scene(self, scene: str) -> None:
        assert scene in SCENES
        with self.lock:
            self.scene = scene
        for f in self.listeners:
            f(scene)
        log.info("scene %s", scene)

    def set_manual(self, **changes) -> None:
        with self.lock:
            for k, v in changes.items():
                setattr(self.manual, k, v)
            if self.vessel and not self.board_driven:
                self.vessel.apply_controls(physics.Controls(**vars(self.manual)))
            elif self.vessel and not self.board_throttle:  # board linked, engine on the window's throttle
                self.vessel.apply_controls(physics.Controls(**{**vars(self.last_board), "throttle": self.manual.throttle}))

    # -- used by the links --------------------------------------------------------------------------
    def board_controls(self, c: physics.Controls) -> None:
        with self.lock:
            self._last_board = time.monotonic()
            self.board_driven = True
            self.last_board = c
            if not self.board_throttle:
                c = physics.Controls(**{**vars(c), "throttle": self.manual.throttle})
            if self.vessel:
                self.vessel.apply_controls(c)

    def vessel_for_board(self) -> physics.Vessel | None:
        with self.lock:
            return self.vessel if self.scene == "flight" else None

    def ksp_running(self) -> bool:
        """KSP owns the serial port when it runs; checked at most every 5 s (tasklist is slow)."""
        t, running = self._ksp_check
        if time.monotonic() - t > 5:
            try:
                out = subprocess.run(["tasklist"], capture_output=True, text=True, timeout=10).stdout
                running = "KSP_x64.exe" in out
            except (OSError, subprocess.SubprocessError):
                running = False
            self._ksp_check = (time.monotonic(), running)
        return running

    # -- clock ------------------------------------------------------------------------------------------
    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=2)

    def _run(self) -> None:
        last = time.monotonic()
        while not self._stop.wait(self.tick):
            now = time.monotonic()
            dt = (now - last) * self.warp
            last = now
            with self.lock:
                if self.board_driven and now - self._last_board > 3:  # board gone: the window drives again
                    self.board_driven = False
                    if self.vessel:
                        self.vessel.apply_controls(physics.Controls(**vars(self.manual)))
                if self.scene == "closed":
                    continue
                self.ut += dt
                if self.vessel and self.manual_values is None:  # on rails outside flight too, as in KSP
                    self.vessel.step(dt)
