"""The simulator window (tkinter, standard library): scene, scenario, time warp, the vessel, and both links."""
from __future__ import annotations

import math
import time
import tkinter as tk
from tkinter import ttk

from . import physics


def _dist(m: float) -> str:
    if not math.isfinite(m):
        return "--"
    return "%.0f m" % m if abs(m) < 10_000 else "%.1f km" % (m / 1e3) if abs(m) < 1e7 else "%.2f Mm" % (m / 1e6)


def _time(s: float) -> str:
    if not math.isfinite(s):
        return "--"
    s = int(s)
    return "%dh %02dm %02ds" % (s // 3600, s % 3600 // 60, s % 60) if s >= 3600 else "%dm %02ds" % (s // 60, s % 60)


class App:
    def __init__(self, sim, board, server) -> None:
        self.sim, self.board, self.server = sim, board, server
        self.root = tk.Tk()
        self.root.title("KSP simulator: Arduino + Raspberry Pi HMI")
        self.root.minsize(760, 560)
        pad = dict(padx=8, pady=4)

        top = ttk.Frame(self.root)
        top.pack(fill="x", **pad)
        self.scene = tk.StringVar(value=sim.scene)
        game = ttk.LabelFrame(top, text="Game (simulated KSP)")
        game.pack(side="left", fill="both", expand=True, padx=4)
        for value, label in (("flight", "Flight"), ("space_center", "Space center"), ("closed", "KSP closed")):
            ttk.Radiobutton(game, text=label, value=value, variable=self.scene,
                            command=lambda: sim.set_scene(self.scene.get())).pack(anchor="w")
        self.board_text = tk.StringVar()
        self.krpc_text = tk.StringVar()
        for title, var in (("Arduino (KSPSerialIO protocol)", self.board_text), ("Raspberry Pi (kRPC)", self.krpc_text)):
            f = ttk.LabelFrame(top, text=title)
            f.pack(side="left", fill="both", expand=True, padx=4)
            ttk.Label(f, textvariable=var, justify="left", width=40).pack(anchor="w", padx=4, pady=2)

        mid = ttk.Frame(self.root)
        mid.pack(fill="x", **pad)
        sc = ttk.LabelFrame(mid, text="Scenario (loads a new vessel)")
        sc.pack(side="left", fill="y", padx=4)
        for name, label in (("pad", "On the pad"), ("orbit", "Orbit 80 x 250 km"),
                            ("suborbital", "Sub-orbital, falling"), ("escape", "Escape trajectory")):
            ttk.Button(sc, text=label, command=lambda n=name: sim.load(n)).pack(fill="x", padx=4, pady=1)
        wf = ttk.LabelFrame(mid, text="Time warp")
        wf.pack(side="left", fill="y", padx=4)
        self.warp = tk.DoubleVar(value=sim.warp)
        for w in (1, 5, 10, 50, 100, 1000):
            ttk.Radiobutton(wf, text="%dx" % w, value=w, variable=self.warp,
                            command=lambda: setattr(sim, "warp", self.warp.get())).pack(anchor="w")

        ctl = ttk.LabelFrame(mid, text="Controls")
        ctl.pack(side="left", fill="both", expand=True, padx=4)
        self.driver = tk.StringVar()
        ttk.Label(ctl, textvariable=self.driver).pack(anchor="w", padx=4)
        self.throttle = tk.DoubleVar(value=0)
        self.throttle_scale = ttk.Scale(ctl, from_=0, to=100, variable=self.throttle, orient="horizontal",
                                        command=lambda _v: sim.set_manual(throttle=self.throttle.get() / 100))
        self.throttle_scale.pack(fill="x", padx=4)
        row = ttk.Frame(ctl)
        row.pack(fill="x", padx=4)
        self.flags = {}
        for name in ("sas", "rcs", "lights", "gear", "brakes"):
            var = tk.BooleanVar()
            cb = ttk.Checkbutton(row, text=name.upper(), variable=var,
                                 command=lambda n=name, v=var: sim.set_manual(**{n: v.get()}))
            cb.pack(side="left")
            self.flags[name] = (var, cb)
        self.stage_btn = ttk.Button(ctl, text="STAGE", command=self._stage)
        self.stage_btn.pack(anchor="w", padx=4, pady=2)
        self.groups_text = tk.StringVar()
        ttk.Label(ctl, textvariable=self.groups_text).pack(anchor="w", padx=4)

        vf = ttk.LabelFrame(self.root, text="Vessel")
        vf.pack(fill="both", expand=True, **pad)
        self.vessel_text = tk.StringVar()
        ttk.Label(vf, textvariable=self.vessel_text, justify="left", font=("Consolas", 11)).pack(anchor="w", padx=6, pady=4)

        self.root.after(200, self._refresh)

    def _stage(self) -> None:
        self.sim.set_manual(stage=True)
        self.root.after(300, lambda: self.sim.set_manual(stage=False))

    def _refresh(self) -> None:
        sim = self.sim
        b = self.board.status if self.board else None
        if b is None:
            self.board_text.set("disabled (--no-board)")
        else:
            age = time.monotonic() - b.last_controls if b.last_controls else float("inf")
            self.board_text.set("%s  %s\n%s\nVesselData sent %d, ControlPackets %d (last %s), bad frames %d\n"
                                "board uptime %s" % (
                                    b.state.upper(), b.port, b.detail, b.sent, b.received,
                                    "%.1f s ago" % age if math.isfinite(age) else "never", b.bad,
                                    _time(b.uptime_ms / 1000) if b.uptime_ms else "--"))
        if self.server is None:
            self.krpc_text.set("disabled (--no-krpc)")
        elif self.server.error:
            self.krpc_text.set("ERROR\n" + self.server.error)
        elif not self.server.listening:
            self.krpc_text.set("ports closed (KSP closed)")
        else:
            lines = ["listening on port %d / %d" % (self.server.rpc_port, self.server.stream_port)]
            for c in list(self.server.clients.values()):
                lines.append("%s @ %s: %d calls, %d streams" % (c.name, c.address, c.calls, len(c.streams)))
            if len(lines) == 1:
                lines.append("no client yet (is the Pi pointed at this PC?)")
            self.krpc_text.set("\n".join(lines))

        with sim.lock:
            v = sim.vessel
            c = v.controls
            self.driver.set("Driven by the Arduino (window controls disabled)" if sim.board_driven
                            else "Driven by this window (no Arduino controls)")
            state = "disabled" if sim.board_driven else "!disabled"
            self.throttle_scale.state([state])
            self.stage_btn.state([state])
            for name, (var, cb) in self.flags.items():
                cb.state([state])
                if sim.board_driven:
                    var.set(getattr(c, name))
            if sim.board_driven:
                self.throttle.set(c.throttle * 100)
            self.groups_text.set("Action groups: " + " ".join(
                str(n) if c.groups >> n & 1 else "." for n in range(1, 11))
                + "   throttle %d%%  pitch %+.2f roll %+.2f yaw %+.2f" % (c.throttle * 100, c.pitch, c.roll, c.yaw))
            el = v.elements()
            lf = 100 * v.liquid_fuel / physics.LIQUID_FUEL
            self.vessel_text.set(
                "%-14s %s   (%s)\n" % ("Vessel", v.name, physics.Vessel.situation(v))
                + "%-14s %-14s %-14s %s\n" % ("Altitude", _dist(v.altitude), "Vertical", "%.1f m/s" % v.vertical_speed)
                + "%-14s %-14s %-14s %s\n" % ("Apoapsis", _dist(el.apoapsis) if el.e < 1 else "escape",
                                              "in", _time(el.time_to_apoapsis))
                + "%-14s %-14s %-14s %s\n" % ("Periapsis", _dist(el.periapsis), "in", _time(el.time_to_periapsis))
                + "%-14s %-14s %-14s %s\n" % ("Orbital speed", "%.1f m/s" % v.orbital_speed,
                                              "Surface", "%.1f m/s" % v.surface_speed)
                + "%-14s %-14s %-14s %s\n" % ("Stage", "%d of %d" % (v.stage, physics.STAGES - 1),
                                              "Stage fuel", "%.0f %%" % lf)
                + "%-14s %-14s %-14s %s\n" % ("G", "%.2f" % v.g_force(), "Heat", "%.0f %%" % (v.heat * 100))
                + "%-14s %-14s %-14s %s" % ("Mission time", _time(v.mission_time), "UT", _time(sim.ut)))
        self.root.after(200, self._refresh)

    def run(self) -> None:
        self.root.mainloop()
