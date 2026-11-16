"""Simulator state shared by the board link, the kRPC stand-in and the window: one lock, one clock."""
from __future__ import annotations

import logging
import math
import subprocess
import threading
import time

from . import physics, ships

log = logging.getLogger("kspsim")

EDITOR_MODES = ("construction", "actions", "crew", "cargo")

SKINS = ("", "classic", "apollo")  # "" sends no skin field (as KSPSerialIO): the HMI's config decides
SCENES = ("flight", "space_center", "vab", "sph", "mission_control", "astronaut_complex",
          "research", "administration", "tracking_station", "closed")  # closed = KSP not running: kRPC ports closed, no telemetry

# Game state KSP reports back in VesselData.ActionGroups, bit = position (AGSAS .. AGAbort in the firmware);
# custom groups 1-10 follow at bits 6-15.
GAME_FLAGS = ("sas", "rcs", "lights", "gear", "brakes", "abort")
# Values the window can pin regardless of the flight model and of manual mode (caution / warning LED tests):
# g_force in g, stage_fuel and max_overheat in percent.
FORCEABLE = ("g_force", "stage_fuel", "max_overheat")


class Simulator:
    def __init__(self, scenario: str = "orbit", warp: float = 1.0, tick: float = 0.05) -> None:
        self.lock = threading.RLock()
        self.vessel: physics.Vessel | None = None
        self.vessel_uid = 9  # object ids derive from it; a new vessel gets a new uid, as a switch in KSP does
        self.scene = "flight"
        self.skin = ""  # HMI look sent on the UDP context channel: "" = none (the HMI's [display] skin decides)
        # Editor ship served by the KSPHMI kRPC service in the VAB/SPH (ships.NAMES; "" = no service, as without
        # KSPSerialIO.Hmi.dll). Every change is a new revision, as the plugin's onEditorShipModified.
        self.ship = "kerbal_x"
        self.ship_revision = 1
        self.editor_mode = "construction"
        self.editor_group = "custom1"
        self.warp = warp
        self.ut = 0.0
        self.tick = tick
        self.board_driven = False       # True while ControlPackets arrive (the window's controls step aside)
        self.manual = physics.Controls()
        self.board_throttle = True      # False: the engine follows the window's throttle even with the board linked
        self.last_board = physics.Controls()  # what the board sent last, shown in the window as received
        self.manual_values: dict | None = None  # manual mode: physics frozen, these values reported as typed
        # What the game reports as active (ActionGroups), changed by the window (as keys in KSP would) or by the
        # board. Like KSPSerialIO, a board bit is applied as a level only when it differs from the bit the board
        # sent before (VControlsOld), so a toggle pressed in the game is kept until the board's bit changes.
        self.game = {f: False for f in GAME_FLAGS}
        self.game_groups = 0            # custom action groups, bit n = group n (1-10)
        self._sas_mode = 1              # autopilot mode + 1 (1 stability assist .. 10 maneuver), sent while SAS is on
        self._sas_state = (False, 1)    # (SAS on, mode) last logged
        self._sas_since = 0.0           # UT of the last SAS change, for the "on target" log line
        self._sas_settled = True
        self._board_prev = physics.Controls()  # the plugin's VControlsOld
        self._board_seeded = False      # the plugin's StageSeeded: the first packet of a link only seeds the bits
        self.node: dict = {}            # a maneuver node on the flight model (set_node), under manual values
        self.forced: dict = {}          # FORCEABLE key -> pinned value (window overrides)
        self._last_board = 0.0
        self._ksp_check = (0.0, False)
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="Simulator", daemon=True)
        self.listeners: list = []       # callables(scene) told when the scene changes (the kRPC server)
        self.load(scenario)

    @property
    def sas_mode(self) -> int:
        return self._sas_mode

    @sas_mode.setter
    def sas_mode(self, mode: int) -> None:
        """Select the SAS mode (1 stability assist .. 10 maneuver, physics.SAS_MODES); the vessel slews to it
        while SAS is on."""
        mode = int(mode)
        if not 1 <= mode <= len(physics.SAS_MODES):
            raise ValueError("SAS mode must be 1-%d" % len(physics.SAS_MODES))
        with self.lock:
            self._sas_mode = mode

    def _update_autopilot(self) -> None:
        """Give the vessel this tick's SAS direction (None with SAS off) and log SAS changes."""
        v = self.vessel
        state = (self.game["sas"], self._sas_mode)
        if state != self._sas_state:
            if state[0]:
                log.info("SAS on, mode %s", physics.SAS_MODES[state[1] - 1])
            else:
                log.info("SAS off")
            self._sas_state = state
            self._sas_since, self._sas_settled = self.ut, False
        v.autopilot = v.sas_target(self._sas_mode, self.reported()) if state[0] else None
        if state[0] and self._sas_mode > 1 and v.autopilot == physics.HOLD and not self._sas_settled:
            log.info("SAS %s: no direction (no target, node or speed), holding the attitude",
                     physics.SAS_MODES[self._sas_mode - 1])
            self._sas_settled = True

    def _after_step(self, was_steered: bool) -> None:
        v = self.vessel
        if v.steered and not was_steered:
            log.info("attitude: vessel steered (SAS or stick), launch guidance off, thrust along the nose")
        if (self.game["sas"] and not self._sas_settled and isinstance(v.autopilot, tuple)
                and v.sas_error < 1.0):
            log.info("SAS %s: on target (%.2f deg) after %.1f s, attitude pitch %.1f heading %.1f roll %.1f",
                     physics.SAS_MODES[self._sas_mode - 1], v.sas_error, self.ut - self._sas_since, *v.attitude())
            self._sas_settled = True

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

    def set_node(self, prograde: float | None, seconds: float = 300.0, normal: float = 0.0,
                 radial: float = 0.0) -> None:
        """Create or replace the maneuver node, `seconds` from now (a fixed UT: it counts down as in KSP), with
        its delta-v in m/s along KSP's node axes; prograde None deletes it."""
        with self.lock:
            had = bool(self.node)
            if prograde is None:
                self.node = {}
            else:
                self.node = dict(prograde=float(prograde), normal=float(normal), radial=float(radial),
                                 ut=self.ut + float(seconds))
        if prograde is None:
            log.info("maneuver node deleted" if had else "maneuver node: none")
        else:
            log.info("maneuver node %s: prograde %.1f normal %.1f radial %.1f m/s, in %.0f s",
                     "updated" if had else "created", prograde, normal, radial, seconds)

    def node_preview(self) -> dict | None:
        """The node as reported, with the orbit it results in (window line), or None."""
        with self.lock:
            r = self.reported()
            if not r["node_enabled"]:
                return None
            ap, pe, e, _a = physics.extremes(*physics.maneuver_state(r))
            return dict(dv=r["node_dv"], time=r["node_time"], apoapsis=ap, periapsis=pe, eccentricity=e)

    # -- manual values --------------------------------------------------------------------------------
    def reported(self) -> dict:
        """The values the board and the HMI receive: typed ones in manual mode, else the flight model's, plus the
        game state (action_groups, the VesselData bit field) and the window's forced values on top of both."""
        with self.lock:
            values = self.vessel.report()
            if self.node:
                n = self.node
                dv = math.sqrt(n["prograde"] ** 2 + n["normal"] ** 2 + n["radial"] ** 2)
                v = values["orbital_speed"] or 1.0
                vs = values["vertical_speed"]
                vh = math.sqrt(max(0.0, v * v - vs * vs))
                # NavBall burn vector (up, north, east): prograde along the velocity, radial out perpendicular to
                # it in the up/east plane, normal to the south (the window's planar orbits have northward
                # anti-normal, as TransformDirection)
                values.update(node_enabled=1.0, node_prograde=n["prograde"], node_normal=n["normal"],
                              node_radial=n["radial"], node_dv=dv, node_remaining=dv, node_time=n["ut"] - self.ut,
                              burn_up=(n["prograde"] * vs + n["radial"] * vh) / v, burn_north=-n["normal"],
                              burn_east=(n["prograde"] * vh - n["radial"] * vs) / v)
            if self.manual_values is not None:
                values.update(self.manual_values)
            values["stage_fuel"] = 100.0 * self.vessel.liquid_fuel / physics.LIQUID_FUEL
            values["max_overheat"] = int(self.vessel.heat * 100)
            values.update(self.forced)
            values["action_groups"] = self.action_groups()
            values["sas_mode"] = self.sas_mode
            return values

    # -- game state (what KSP reports as active) ------------------------------------------------------
    def action_groups(self) -> int:
        """VesselData.ActionGroups: bits 0-5 SAS RCS Light Gear Brakes Abort, bits 6-15 custom groups 1-10."""
        with self.lock:
            bits = sum(1 << i for i, f in enumerate(GAME_FLAGS) if self.game[f])
            return bits | ((self.game_groups >> 1) & 0x3FF) << 6

    def set_game(self, **flags) -> None:
        """Change the game state directly, as a key press in KSP would (T for SAS, R for RCS ...)."""
        with self.lock:
            for k, v in flags.items():
                if k not in self.game:
                    raise ValueError("unknown game flag %r" % k)
                self.game[k] = bool(v)

    def set_game_group(self, n: int, on: bool) -> None:
        if not 1 <= n <= 10:
            raise ValueError("action group must be 1-10")
        with self.lock:
            self.game_groups = self.game_groups | 1 << n if on else self.game_groups & ~(1 << n)

    def set_forced(self, key: str, value: float | None) -> None:
        """Pin a reported value (None releases it); wins over the flight model and manual mode."""
        if key not in FORCEABLE:
            raise ValueError("cannot force %r" % key)
        with self.lock:
            if value is None:
                self.forced.pop(key, None)
            else:
                self.forced[key] = value

    def _apply_levels(self, c: physics.Controls, prev: physics.Controls) -> None:
        """KSPSerialIO 0.19.3.14: a change of the SAS / RCS bit is a button press and toggles the game state; the
        other bits set the game state to their level when they changed since the last packet."""
        for f in GAME_FLAGS:
            if getattr(c, f) != getattr(prev, f):
                self.game[f] = (not self.game[f]) if f in ("sas", "rcs") else getattr(c, f)
        for n in range(1, 11):
            if (c.groups ^ prev.groups) >> n & 1:
                self.game_groups = self.game_groups | 1 << n if c.groups >> n & 1 else self.game_groups & ~(1 << n)

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
            self._board_seeded = False
        for f in self.listeners:
            f(scene)
        log.info("scene %s", scene)

    def set_ship(self, name: str) -> None:
        """Put a sample ship in the editor (ships.NAMES), or "" to answer as if KSPSerialIO.Hmi.dll were missing."""
        ships.ship(name)  # raises for an unknown name
        with self.lock:
            self.ship = name
            self.ship_revision += 1
        log.info("editor ship %s (revision %d)", name or "none: no KSPHMI service", self.ship_revision)

    def set_editor_mode(self, mode: str) -> None:
        if mode not in EDITOR_MODES:
            raise ValueError("unknown editor mode: " + mode)
        with self.lock:
            if self.editor_mode != mode:
                self.editor_mode = mode
                self.ship_revision += 1
        log.info("editor mode %s", mode)

    def editor_press(self, button: str) -> None:
        """KSPHMI.Press from the HMI keyboard: in the VAB, SAS steps to the next editor screen and RCS to the
        previous one, like a board press in KSPSerialIO.Hmi (the SPH ignores it)."""
        if button not in ("SAS", "RCS"):
            raise ValueError("button must be SAS or RCS")
        with self.lock:
            if self.scene == "vab":
                direction = 1 if button == "SAS" else -1
                self.set_editor_mode(EDITOR_MODES[(EDITOR_MODES.index(self.editor_mode) + direction) % 4])

    def set_editor_group(self, group: str) -> None:
        valid = ("stage", "gear", "lights", "brakes", "abort", "sas", "rcs") + tuple("custom%d" % n for n in range(1,11))
        if group not in valid:
            raise ValueError("unknown editor group: " + group)
        with self.lock:
            if self.editor_group != group:
                self.editor_group = group
                self.ship_revision += 1

    def ship_report(self) -> str:
        """What KSPHMI.ShipReport() answers now ("" when the sample has no ship)."""
        with self.lock:
            sample = ships.ship(self.ship)
            return ships.report_text(self.ship_revision, ships.editor_sample(sample, self.editor_mode, self.editor_group)) if sample else ""

    def set_skin(self, skin: str) -> None:
        """Ask the Raspberry Pi HMI for a look ("classic", "apollo"), or "" to leave it to its config."""
        assert skin in SKINS
        with self.lock:
            changed, self.skin = skin != self.skin, skin
        if changed:
            log.info("HMI skin %s", skin or "from the HMI config")

    def set_manual(self, **changes) -> None:
        with self.lock:
            for k, v in changes.items():
                setattr(self.manual, k, v)
                if k in self.game and not self.board_driven:  # the window's switch acts on the game at once
                    self.game[k] = bool(v)
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
            if self.scene in ("vab", "sph"):
                if self._board_seeded:
                    for field, direction in (("rcs", -1), ("sas", 1)):
                        if getattr(c, field) != getattr(self._board_prev, field):
                            self.set_editor_mode(EDITOR_MODES[(EDITOR_MODES.index(self.editor_mode) + direction) % 4])
                self._board_seeded = True
                self._board_prev = c
                return
            if self._board_seeded:
                self._apply_levels(c, self._board_prev)
            self._board_seeded = True
            self._board_prev = c
            if not self.board_throttle:
                c = physics.Controls(**{**vars(c), "throttle": self.manual.throttle})
            if self.vessel:
                self.vessel.apply_controls(c)

    def board_linked(self) -> None:
        """A new link (handshake): like the plugin, the next packet only seeds the toggle bits."""
        with self.lock:
            self._board_seeded = False

    def vessel_for_board(self) -> physics.Vessel | None:
        with self.lock:
            return self.vessel if self.scene in ("flight", "vab", "sph") else None

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
                    self.step_vessel(dt)

    def step_vessel(self, dt: float) -> None:
        """One tick of the flight model with this tick's SAS direction (the clock thread; tests call it)."""
        with self.lock:
            self._update_autopilot()
            was = self.vessel.steered
            self.vessel.step(dt)
            self._after_step(was)
