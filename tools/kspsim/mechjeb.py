"""A simulated MechJeb behind KSPHMIFlight.MechJeb* (KSPSerialIO.Hmi.dll's reflection bridge, docs/hmi.rst
"MechJeb scene"): report format v1, starting from the shared fixture hmi-cpp/tests/data/mechjeb_report_v1.txt,
with the plugin's MechJebSet validation and MechJebAction effects. Thread safe: kRPC connections and the window
call it from their own threads."""
from __future__ import annotations

import math
import os
import threading
import urllib.parse

FIXTURE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "hmi-cpp", "tests", "data",
                       "mechjeb_report_v1.txt")

# Values only MechJeb itself changes (MechJebSet answers "read only").
READ_ONLY = {"ascent.enabled", "ascent.status", "ascent.t_minus", "ascent.classic.auto_turn_start_altitude"}
# Reported only while armed (MechJeb's TimedLaunch): absent from the fixture, inserted after ascent.status.
T_MINUS, T_MINUS_AFTER = "ascent.t_minus", "ascent.status"
# 0/1 settings: anything else is an invalid value.
FLAGS = {"ascent.classic.auto", "ascent.force_roll", "ascent.limit_aoa", "ascent.limit_qa",
         "ascent.corrective_steering", "ascent.autostage", "ascent.autodeploy_solar_panels",
         "ascent.autodeploy_antennas", "ascent.skip_circularization", "thrust.limit_q", "thrust.limit_overheat",
         "thrust.limit_acceleration", "thrust.limit_throttle", "thrust.limit_min_throttle", "thrust.smooth_throttle",
         "thrust.prevent_flameout", "thrust.prevent_unstable_ignition", "thrust.differential_throttle",
         "staging.hot_staging", "staging.drop_solids", "staging.autostaging_once"}
# Whole numbers with their allowed range.
INTEGERS = {"ascent.path": (0, 2), "ascent.last_stage": (0, 99), "staging.autostage_limit": (0, 99)}
ACTIONS = ("ascent.engage", "ascent.disengage", "ascent.launch_rendezvous", "ascent.launch_target_plane",
           "ascent.abort_timed_launch", "all.disengage")
STATES = ("ok", "missing", "nocore")


def fixture_values() -> tuple[str, dict]:
    """(version, ordered values) of the shared fixture, values decoded."""
    with open(FIXTURE, encoding="utf-8") as f:
        lines = f.read().replace("\r\n", "\n").split("\n")
    head = dict(w.split("=", 1) for w in lines[0].split() if "=" in w)
    values = {}
    for line in lines[1:]:
        if "=" in line:
            key, value = line.split("=", 1)
            values[key] = urllib.parse.unquote(value)
    return head.get("version", ""), values


def number_text(value: float) -> str:
    """As the plugin writes numbers: invariant culture, shortest round trip, integers without ".0"."""
    if value == int(value) and abs(value) < 1e15:
        return str(int(value))
    return repr(value)


class MechJebSim:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.version, self.values = fixture_values()
        self.state = "ok"  # ok, missing (MechJeb not installed) or nocore (no MechJeb part on the vessel)
        self.revision = 42
        self.t_minus: str | None = None  # ascent.t_minus while a timed launch is armed, else omitted
        self.calls: list[tuple[str, ...]] = []  # every Set / Action, for tests and the log

    def set_state(self, state: str) -> None:
        assert state in STATES, state
        with self.lock:
            if state != self.state:
                self.state = state
                self.revision += 1

    def report(self) -> str:
        with self.lock:
            if self.state == "missing":
                return "v=1 rev=%d mj=missing\n" % self.revision
            head = "v=1 rev=%d mj=%s version=%s\n" % (self.revision, self.state, self.version)
            if self.state != "ok":
                return head
            items = []
            for k, v in self.values.items():
                items.append((k, v))
                if k == T_MINUS_AFTER and self.t_minus is not None:
                    items.append((T_MINUS, self.t_minus))
            return head + "".join("%s=%s\n" % (k, urllib.parse.quote(v, safe=".-_")) for k, v in items)

    def set(self, key: str, value: str) -> str:
        """KSPHMIFlight.MechJebSet: ok, unknown key, read only, invalid value or unavailable."""
        with self.lock:
            self.calls.append(("set", key, value))
            if self.state != "ok":
                return "unavailable"
            if key in READ_ONLY:
                return "read only"
            if key not in self.values:
                return "unknown key"
            try:
                number = float(value)
            except ValueError:
                return "invalid value"
            if not math.isfinite(number):
                return "invalid value"
            if key in FLAGS and number not in (0, 1):
                return "invalid value"
            if key in INTEGERS:
                low, high = INTEGERS[key]
                if number != int(number) or not low <= number <= high:
                    return "invalid value"
            text = number_text(number)
            if self.values[key] != text:
                self.values[key] = text
                self.revision += 1
            return "ok"

    def action(self, action: str) -> str:
        """KSPHMIFlight.MechJebAction: engaging, disengaging and timed launches change ascent.enabled/status."""
        with self.lock:
            self.calls.append(("action", action))
            if self.state != "ok":
                return "unavailable"
            if action not in ACTIONS:
                return "unknown key"
            v = self.values
            if action == "ascent.engage":
                v["ascent.enabled"], v["ascent.status"] = "1", "Awaiting liftoff"
            elif action in ("ascent.disengage", "all.disengage"):
                v["ascent.enabled"], v["ascent.status"], self.t_minus = "0", "Off", None
            elif action in ("ascent.launch_rendezvous", "ascent.launch_target_plane"):
                v["ascent.enabled"], self.t_minus = "1", "30"
                v["ascent.status"] = "Launching to rendezvous" if action.endswith("rendezvous") else \
                    "Launching to target plane"
            elif action == "ascent.abort_timed_launch":
                self.t_minus, v["ascent.status"] = None, "Timed launch aborted"
            self.revision += 1
            return "ok"
