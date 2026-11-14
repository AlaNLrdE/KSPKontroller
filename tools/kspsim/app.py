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
        for w in (0, 1, 5, 10, 50, 100, 1000):
            ttk.Radiobutton(wf, text="Pause" if w == 0 else "%dx" % w, value=w, variable=self.warp,
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

        # Edit the current vessel: the same vessel keeps flying from the new state (no vessel switch)
        ef = ttk.LabelFrame(self.root, text="Set the current vessel (Enter or Apply)")
        ef.pack(fill="x", **pad)
        self.edit = {}
        fields = (("apoapsis", "Apoapsis km", "250"), ("periapsis", "Periapsis km", "80"),
                  ("inclination", "Inclination °", "0.4"), ("anomaly", "Position ° from Pe", "0"),
                  ("name", "Name", ""), ("fuel", "Stage fuel %", "100"))
        for col, (key, label, default) in enumerate(fields):
            ttk.Label(ef, text=label).grid(row=0, column=col, sticky="w", padx=4)
            var = tk.StringVar(value=default)
            entry = ttk.Entry(ef, textvariable=var, width=12 if key == "name" else 9)
            entry.grid(row=1, column=col, sticky="w", padx=4)
            entry.bind("<Return>", lambda _e: self._apply())
            self.edit[key] = var
        ttk.Button(ef, text="Apply orbit", command=self._apply).grid(row=1, column=len(fields), padx=6)
        ttk.Button(ef, text="Apply name / fuel", command=self._apply_vessel).grid(row=1, column=len(fields) + 1, padx=2)
        ttk.Button(ef, text="Read current", command=self._read_current).grid(row=1, column=len(fields) + 2, padx=2)
        self.edit_msg = tk.StringVar(value="Position 0 = at the periapsis, 180 = at the apoapsis.")
        ttk.Label(ef, textvariable=self.edit_msg).grid(row=2, column=0, columnspan=len(fields) + 3, sticky="w", padx=4)

        vf = ttk.LabelFrame(self.root, text="Vessel")
        vf.pack(fill="both", expand=True, **pad)
        self.vessel_text = tk.StringVar()
        ttk.Label(vf, textvariable=self.vessel_text, justify="left", font=("Consolas", 11)).pack(anchor="w", padx=6, pady=4)

        self.root.after(200, self._refresh)

    def _number(self, key: str) -> float:
        text = self.edit[key].get().strip().replace(",", ".")
        try:
            return float(text)
        except ValueError:
            raise ValueError("'%s' is not a number" % text) from None

    def _apply(self) -> None:
        try:
            ap, pe = self._number("apoapsis") * 1e3, self._number("periapsis") * 1e3
            self.sim.set_orbit(ap, pe, self._number("inclination"), self._number("anomaly"))
            self.edit_msg.set("Orbit set: Ap %.1f km, Pe %.1f km. The vessel keeps flying from there." % (ap / 1e3, pe / 1e3))
        except ValueError as e:
            self.edit_msg.set("Not applied: %s" % e)

    def _apply_vessel(self) -> None:
        try:
            name = self.edit["name"].get().strip() or None
            fuel = self._number("fuel") if self.edit["fuel"].get().strip() else None
            self.sim.set_vessel(name, fuel)
            self.edit_msg.set("Vessel updated.")
        except ValueError as e:
            self.edit_msg.set("Not applied: %s" % e)

    def _read_current(self) -> None:
        with self.sim.lock:
            v = self.sim.vessel
            el = v.elements()
            values = {"apoapsis": el.apoapsis / 1e3, "periapsis": el.periapsis / 1e3, "inclination": v.inclination,
                      "anomaly": math.degrees(el.nu), "fuel": 100 * v.liquid_fuel / physics.LIQUID_FUEL}
            name = v.name
        for key, value in values.items():
            self.edit[key].set("%.1f" % value if math.isfinite(value) else "")
        self.edit["name"].set(name)
        self.edit_msg.set("Current values loaded; change any and Apply.")

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
