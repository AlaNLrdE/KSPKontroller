"""The simulator window (tkinter, standard library): scene, scenario, time warp, the vessel, and both links."""
from __future__ import annotations

from .sim import SCENES

import math
import time
import tkinter as tk
from tkinter import ttk

from . import arduino, physics

SAS_MODES = ("Stability assist", "Prograde", "Retrograde", "Normal", "Anti-normal", "Radial in", "Radial out",
             "Target", "Anti-target", "Maneuver")  # NavballSASMode low nibble 1-10


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
        self.root.minsize(900, 760)
        pad = dict(padx=8, pady=4)

        top = ttk.Frame(self.root)
        top.pack(fill="x", **pad)
        self.scene = tk.StringVar(value=sim.scene)
        game = ttk.LabelFrame(top, text="Game (simulated KSP)")
        game.pack(side="left", fill="both", expand=True, padx=4)
        for value, label in ((v, v.replace("_", " ").title()) for v in SCENES):
            ttk.Radiobutton(game, text=label, value=value, variable=self.scene,
                            command=lambda: sim.set_scene(self.scene.get())).pack(anchor="w")
        self.board_text = tk.StringVar()
        self.krpc_text = tk.StringVar()
        for title, var, width in (("Arduino (KSPSerialIO protocol)", self.board_text, 64),
                                  ("Raspberry Pi (kRPC)", self.krpc_text, 40)):
            f = ttk.LabelFrame(top, text=title)
            f.pack(side="left", fill="both", expand=True, padx=4)
            ttk.Label(f, textvariable=var, justify="left", width=width).pack(anchor="w", padx=4, pady=2)

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
        self.use_board_throttle = tk.BooleanVar(value=sim.board_throttle)
        ttk.Checkbutton(ctl, text="Use the board's throttle (untick: the engine follows this slider)",
                        variable=self.use_board_throttle, command=self._throttle_source).pack(anchor="w", padx=4)
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
        stick = ttk.Frame(ctl)
        stick.pack(fill="x", padx=4)
        ttk.Label(stick, text="Stick (hold)").pack(side="left")
        self.stick_btns = []
        for text, axis, value in (("Pitch up", "pitch", 1.0), ("down", "pitch", -1.0), ("Yaw left", "yaw", -1.0),
                                  ("right", "yaw", 1.0), ("Roll left", "roll", -1.0), ("right", "roll", 1.0)):
            b = ttk.Button(stick, text=text, width=len(text) + 1)
            b.bind("<ButtonPress-1>", lambda _e, a=axis, x=value: self._stick(a, x))
            b.bind("<ButtonRelease-1>", lambda _e, a=axis: self._stick(a, 0.0))
            b.pack(side="left")
            self.stick_btns.append(b)
        self.groups_text = tk.StringVar()
        ttk.Label(ctl, textvariable=self.groups_text).pack(anchor="w", padx=4)

        nb = ttk.Notebook(self.root)
        nb.pack(fill="x", **pad)
        self.notebook = nb

        # Edit the current vessel: the same vessel keeps flying from the new state (no vessel switch)
        ef = ttk.Frame(nb)
        nb.add(ef, text="Set the current vessel")
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

        # Maneuver node: what the HMI reads through kRPC Node / Node.orbit (dashed orbit, NavBall node marker)
        node_tab = ttk.Frame(nb)
        nb.add(node_tab, text="Maneuver node")
        self.node_fields = {}
        current = dict(sim.node) if sim.node else {}
        for col, (key, label, default) in enumerate((
                ("prograde", "Prograde m/s", current.get("prograde", 100.0)),
                ("normal", "Normal m/s", current.get("normal", 0.0)),
                ("radial", "Radial out m/s", current.get("radial", 0.0)),
                ("time", "Time to node s", current["ut"] - sim.ut if current else 300.0))):
            ttk.Label(node_tab, text=label).grid(row=0, column=col, sticky="w", padx=4)
            var = tk.StringVar(value="%g" % default)
            entry = ttk.Entry(node_tab, textvariable=var, width=10)
            entry.grid(row=1, column=col, sticky="w", padx=4)
            entry.bind("<Return>", lambda _e: self._node_apply())
            self.node_fields[key] = var
        ttk.Button(node_tab, text="Create / update node", command=self._node_apply).grid(row=1, column=4, padx=6)
        ttk.Button(node_tab, text="Delete node", command=self._node_delete).grid(row=1, column=5, padx=2)
        self.node_text = tk.StringVar()
        ttk.Label(node_tab, textvariable=self.node_text).grid(row=2, column=0, columnspan=6, sticky="w", padx=4)

        # Manual values: physics frozen, every reported value typed by hand (formats, colours, edge cases)
        mf = ttk.Frame(nb)
        nb.add(mf, text="Manual values (sent exactly as typed)")
        self.manual_on = tk.BooleanVar(value=False)
        ttk.Checkbutton(mf, text="Manual mode: freeze the physics and send these values", variable=self.manual_on,
                        command=self._manual_toggle).grid(row=0, column=0, columnspan=4, sticky="w", padx=4)
        self.manual_vars = {}
        self.manual_entries = []
        per_row = 7
        for i, (key, label, unit, _f) in enumerate(physics.REPORTED_FIELDS):
            r, col = 1 + 2 * (i // per_row), i % per_row
            ttk.Label(mf, text="%s%s" % (label, " (%s)" % unit if unit else "")).grid(row=r, column=col, sticky="w", padx=4)
            var = tk.StringVar()
            entry = ttk.Entry(mf, textvariable=var, width=12)
            entry.grid(row=r + 1, column=col, sticky="w", padx=4)
            entry.bind("<Return>", lambda _e: self._manual_apply())
            self.manual_vars[key] = var
            self.manual_entries.append(entry)
        last = 1 + 2 * ((len(physics.REPORTED_FIELDS) - 1) // per_row) + 2
        ttk.Label(mf, text="Situation").grid(row=last, column=0, sticky="w", padx=4)
        self.manual_situation = tk.StringVar()
        self.situation_box = ttk.Combobox(mf, textvariable=self.manual_situation, values=physics.SITUATIONS,
                                          state="readonly", width=12)
        self.situation_box.grid(row=last + 1, column=0, sticky="w", padx=4)
        self.situation_box.bind("<<ComboboxSelected>>", lambda _e: self._manual_apply())
        self.manual_apply_btn = ttk.Button(mf, text="Apply values", command=self._manual_apply)
        self.manual_apply_btn.grid(row=last + 1, column=1, sticky="w", padx=4)
        self.manual_msg = tk.StringVar(value="Tick the box to start from the current values. "
                                             "'inf' and 'nan' are accepted, to test how the screens cope.")
        ttk.Label(mf, textvariable=self.manual_msg).grid(row=last + 1, column=2, columnspan=5, sticky="w", padx=4)
        self._manual_enable(False)
        self._build_game_tab(nb)
        self._build_board_tab(nb)

        vf =ttk.LabelFrame(self.root, text="Vessel")
        vf.pack(fill="both", expand=True, **pad)
        self.vessel_text = tk.StringVar()
        ttk.Label(vf, textvariable=self.vessel_text, justify="left", font=("Consolas", 11)).pack(anchor="w", padx=6, pady=4)

        self.root.after(200, self._refresh)

    # -- game state and caution / warning triggers --------------------------------------------------
    def _build_game_tab(self, nb) -> None:
        sim = self.sim
        gf = ttk.Frame(nb)
        nb.add(gf, text="Game state / LEDs")
        ttk.Label(gf, text="What KSP reports as active (VesselData.ActionGroups). Change it here as keys in the game "
                           "would; the board's SAS / RCS buttons send the opposite of it and the plugin applies a bit "
                           "only when it changes.", wraplength=880, justify="left").grid(
            row=0, column=0, columnspan=12, sticky="w", padx=4)
        self.game_vars = {}
        for col, name in enumerate(("sas", "rcs", "lights", "gear", "brakes", "abort")):
            var = tk.BooleanVar(value=sim.game[name])
            ttk.Checkbutton(gf, text=name.upper(), variable=var,
                            command=lambda n=name, v=var: sim.set_game(**{n: v.get()})).grid(row=1, column=col, sticky="w", padx=4)
            self.game_vars[name] = var
        ttk.Label(gf, text="SAS mode").grid(row=1, column=6, sticky="e", padx=4)
        self.sas_mode = tk.StringVar(value=SAS_MODES[sim.sas_mode - 1])
        box = ttk.Combobox(gf, textvariable=self.sas_mode, values=SAS_MODES, state="readonly", width=16)
        box.grid(row=1, column=7, columnspan=3, sticky="w", padx=4)
        box.bind("<<ComboboxSelected>>", lambda _e: setattr(sim, "sas_mode", SAS_MODES.index(self.sas_mode.get()) + 1))
        self.group_vars = {}
        ag = ttk.Frame(gf)
        ag.grid(row=2, column=0, columnspan=12, sticky="w", padx=4)
        ttk.Label(ag, text="Action groups").pack(side="left")
        for n in range(1, 11):
            var = tk.BooleanVar(value=bool(sim.game_groups >> n & 1))
            ttk.Checkbutton(ag, text=str(n), variable=var,
                            command=lambda n=n, v=var: sim.set_game_group(n, v.get())).pack(side="left")
            self.group_vars[n] = var
        self.board_bits = tk.StringVar()
        ttk.Label(gf, textvariable=self.board_bits).grid(row=3, column=0, columnspan=12, sticky="w", padx=4)

        # Forced values: pinned on top of the flight model and manual mode, to light the caution / warning LEDs
        tf = ttk.LabelFrame(gf, text="Caution / warning triggers (tick Force to pin the value)")
        tf.grid(row=4, column=0, columnspan=12, sticky="we", padx=4, pady=4)
        self.force_on, self.force_val = {}, {}
        rows = (("g_force", "G force (g)", 0, 12, "yellow above %d g, red above %d g" % (arduino.GCAUTION, arduino.GWARN)),
                ("stage_fuel", "Stage fuel (%)", 0, 100, "yellow below %.0f %%, red below %.0f %% (LiquidFuelS / LiquidFuelTotS)"
                 % (arduino.FUELCAUTION, arduino.FUELWARN)),
                ("max_overheat", "Max overheat (%)", 0, 100, "MaxOverHeat: sent, but no LED or screen of the firmware uses it"))
        for r, (key, label, lo, hi, note) in enumerate(rows):
            on = tk.BooleanVar(value=False)
            val = tk.DoubleVar(value=lo)
            ttk.Checkbutton(tf, text="Force " + label, variable=on,
                            command=lambda k=key: self._force(k)).grid(row=r, column=0, sticky="w", padx=4)
            ttk.Scale(tf, from_=lo, to=hi, variable=val, orient="horizontal", length=220,
                      command=lambda _v, k=key: self._force(k)).grid(row=r, column=1, padx=4)
            ttk.Label(tf, textvariable=val, width=6).grid(row=r, column=2, sticky="w")
            ttk.Label(tf, text=note).grid(row=r, column=3, sticky="w", padx=4)
            self.force_on[key], self.force_val[key] = on, val
        self.leds_text = tk.StringVar()
        ttk.Label(gf, textvariable=self.leds_text, font=("Consolas", 10)).grid(row=5, column=0, columnspan=12, sticky="w", padx=4)

    def _force(self, key: str) -> None:
        val = self.force_val[key]
        val.set(round(val.get(), 1))
        self.sim.set_forced(key, val.get() if self.force_on[key].get() else None)

    def _refresh_game(self) -> None:
        sim = self.sim
        with sim.lock:
            for name, var in self.game_vars.items():
                if var.get() != sim.game[name]:
                    var.set(sim.game[name])
            for n, var in self.group_vars.items():
                if var.get() != bool(sim.game_groups >> n & 1):
                    var.set(bool(sim.game_groups >> n & 1))
            b = sim.last_board
            self.board_bits.set("Last ControlPacket bits from the board: SAS %d  RCS %d  lights %d  gear %d  "
                                "brakes %d  AG1 %d%s" % (b.sas, b.rcs, b.lights, b.gear, b.brakes, b.groups >> 1 & 1,
                                                         "" if sim.board_driven else "   (board not driving)"))
            f = arduino.unpack_vessel(arduino.encode_vessel(sim.vessel, 0.0, sim.reported()))
        e = arduino.expected_leds(f)
        on = lambda x: "ON " if x else "off"  # noqa: E731
        self.leds_text.set("The board should show: green ON while linked, yellow %s%s, red %s%s\n"
                           "SAS LED %s  RCS LED %s  CG1 LED %s   (stage fuel %.0f %%, G %.2f, MaxOverHeat %d)" % (
                               on(e["yellow"]), " (" + ", ".join(e["yellow"]) + ")" if e["yellow"] else "",
                               on(e["red"]), " (" + ", ".join(e["red"]) + "; the matrix flashes inverted)" if e["red"] else "",
                               on(e["sas"]), on(e["rcs"]), on(e["cg1"]),
                               100 * f["LiquidFuelS"] / f["LiquidFuelTotS"], f["G"], f["MaxOverHeat"]))

    # -- board tests: LED matrix screens, link tests, DebugPacket --------------------------------------
    def _build_board_tab(self, nb) -> None:
        bf = ttk.Frame(nb)
        nb.add(bf, text="Board tests")
        mx = ttk.LabelFrame(bf, text="LED matrix: DisplayCommand (id 2) forces a screen for %.0f s" % arduino.DISPLAY_FORCE_S)
        mx.grid(row=0, column=0, sticky="nw", padx=4, pady=2)
        self.hold = tk.BooleanVar(value=False)
        for i, (label, screen) in enumerate(arduino.DISPLAY_SCREENS):
            ttk.Button(mx, text="%s (%d)" % (label, screen), width=19,
                       command=lambda s=screen: self._display(s)).grid(row=i // 4, column=i % 4, padx=1, pady=1)
        last = (len(arduino.DISPLAY_SCREENS) - 1) // 4 + 1
        ttk.Checkbutton(mx, text="Keep it: repeat the command every 3 s", variable=self.hold,
                        command=self._hold_toggle).grid(row=last, column=0, columnspan=4, sticky="w", padx=4)

        lk = ttk.LabelFrame(bf, text="Link tests")
        lk.grid(row=0, column=1, sticky="nw", padx=4, pady=2)
        self.pause = tk.BooleanVar(value=False)
        ttk.Checkbutton(lk, text="Pause telemetry (idle after %.0f s, board reset after %.0f s)" % (
            arduino.IDLETIMER_S, arduino.LINKRESET_S), variable=self.pause, command=self._pause_toggle).pack(anchor="w", padx=4)
        for text, cmd in (("Send a frame with a bad checksum", lambda b: b.send_bad_checksum()),
                          ("Send a frame with a wrong size", lambda b: b.send_bad_size()),
                          ("Re-handshake now", lambda b: (b.rehandshake(), "re-handshake requested")[1])):
            ttk.Button(lk, text=text, command=lambda c=cmd: self._board_cmd(c)).pack(fill="x", padx=4, pady=1)
        self.board_msg = tk.StringVar(value="")
        ttk.Label(lk, textvariable=self.board_msg, wraplength=330, justify="left").pack(anchor="w", padx=4)

        self.matrix_text = tk.StringVar()
        ttk.Label(bf, textvariable=self.matrix_text, font=("Consolas", 10), justify="left").grid(
            row=0, column=2, sticky="nw", padx=6)
        self.events_text = tk.StringVar()
        ttk.Label(bf, textvariable=self.events_text, font=("Consolas", 9), justify="left").grid(
            row=1, column=0, columnspan=3, sticky="w", padx=4)

    def _board_cmd(self, cmd) -> None:
        self.board_msg.set("disabled (--no-board)" if self.board is None else cmd(self.board))

    def _display(self, screen: int) -> None:
        if self.board is not None and self.hold.get():
            self.board.hold_screen = None if screen == 255 else screen
        self._board_cmd(lambda b: b.send_display(screen))

    def _hold_toggle(self) -> None:
        if self.board is not None and not self.hold.get():
            self.board.hold_screen = None

    def _pause_toggle(self) -> None:
        if self.board is None:
            self.board_msg.set("disabled (--no-board)")
            self.pause.set(False)
            return
        self.board.pause_telemetry(self.pause.get())

    def _refresh_board_tab(self) -> None:
        b = self.board
        if b is None:
            self.matrix_text.set("no board (--no-board)")
            return
        if self.pause.get() != (b.paused_since is not None):
            self.pause.set(b.paused_since is not None)  # the link ends the pause when the board reconnects
        d = b.status.debug
        if d is None:
            self.matrix_text.set("LED matrix (from the DebugPacket):\nno DebugPacket yet")
        else:
            age = time.monotonic() - b.status.debug_at
            self.matrix_text.set("LED matrix, %.1f s ago:\n" % age + "\n".join(arduino.matrix_rows(d["matrix"])))
        self.events_text.set("\n".join(b.events) if b.events else "No link test yet.")

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

    def _node_apply(self) -> None:
        try:
            f = {k: float(v.get()) for k, v in self.node_fields.items()}
            if not all(math.isfinite(x) for x in f.values()):
                raise ValueError("numbers only")
            self.sim.set_node(f["prograde"], f["time"], f["normal"], f["radial"])
        except ValueError as e:
            self.node_text.set("Not applied: %s" % e)

    def _node_delete(self) -> None:
        self.sim.set_node(None)

    def _refresh_node(self) -> None:
        n = self.sim.node_preview()
        if n is None:
            self.node_text.set("No maneuver node. The HMI hides its dashed orbit within about 2 s.")
            return
        when = "T- %s" % _time(n["time"]) if n["time"] >= 0 else "T+ %s" % _time(-n["time"])
        after = ("Ap %s, Pe %s" % (_dist(n["apoapsis"]), _dist(n["periapsis"])) if n["eccentricity"] < 1
                 else "escape (e %.2f), Pe %s" % (n["eccentricity"], _dist(n["periapsis"])))
        self.node_text.set("Node: dV %.1f m/s, %s  ->  after the burn: %s" % (n["dv"], when, after))

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

    def _manual_enable(self, on: bool) -> None:
        for e in self.manual_entries:
            e.state(["!disabled" if on else "disabled"])
        self.situation_box.state(["!disabled", "readonly"] if on else ["disabled"])
        self.manual_apply_btn.state(["!disabled" if on else "disabled"])

    def _manual_toggle(self) -> None:
        on = self.manual_on.get()
        self.sim.set_manual_mode(on)
        self._manual_enable(on)
        if on:
            r = self.sim.reported()
            for key, _label, _unit, factor in physics.REPORTED_FIELDS:
                v = r[key] / factor
                self.manual_vars[key].set(("%.6g" % v) if math.isfinite(v) else str(v))
            self.manual_situation.set(r["situation"])
            self.manual_msg.set("Physics frozen. Change any value and press Enter or Apply.")
        else:
            self.manual_msg.set("Physics running again from where the vessel was.")

    def _manual_apply(self) -> None:
        values, bad = {}, []
        for key, label, _unit, factor in physics.REPORTED_FIELDS:
            text = self.manual_vars[key].get().strip().replace(",", ".")
            try:
                values[key] = float(text) * factor
            except ValueError:
                bad.append(label)
        if bad:
            self.manual_msg.set("Not applied, not a number: " + ", ".join(bad))
            return
        if self.manual_situation.get():
            values["situation"] = self.manual_situation.get()
        try:
            self.sim.set_manual_values(**values)
            self.manual_msg.set("Applied. The board and the HMI now receive these values.")
        except ValueError as e:
            self.manual_msg.set("Not applied: %s" % e)

    def _throttle_source(self) -> None:
        with self.sim.lock:
            self.sim.board_throttle = self.use_board_throttle.get()
        if not self.sim.board_throttle:
            self.throttle.set(0)  # the slider was mirroring the board: start from engine off
        self.sim.set_manual(throttle=self.throttle.get() / 100)  # re-apply at once

    def _stick(self, axis: str, value: float) -> None:
        if not self.sim.board_driven:  # with the board linked its axes steer
            self.sim.set_manual(**{axis: value})

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
                                    _time(b.uptime_ms / 1000) if b.uptime_ms else "--")
                                + ("\n" + arduino.debug_summary(b.debug) if b.debug else "\nno DebugPacket yet"))
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
            slider_free = not sim.board_driven or not sim.board_throttle
            self.throttle_scale.state(["!disabled" if slider_free else "disabled"])
            self.stage_btn.state([state])
            for b in self.stick_btns:
                b.state([state])
            for name, (var, cb) in self.flags.items():
                cb.state([state])
                if sim.board_driven:
                    var.set(getattr(c, name))
            if sim.board_driven and sim.board_throttle:
                self.throttle.set(c.throttle * 100)
            self.groups_text.set("Action groups: " + " ".join(
                str(n) if c.groups >> n & 1 else "." for n in range(1, 11))
                + "   engine %d%%  board throttle %d%%  pitch %+.2f roll %+.2f yaw %+.2f" % (
                    c.throttle * 100, sim.last_board.throttle * 100, c.pitch, c.roll, c.yaw))
            r = sim.reported()  # what the board and the HMI receive (manual values included)
            lf = 100 * v.liquid_fuel / physics.LIQUID_FUEL
            self.vessel_text.set(
                "%-14s %s   (%s)%s\n" % ("Vessel", r["name"], r["situation"],
                                         "   MANUAL VALUES, physics frozen" if sim.manual_values is not None else "")
                + "%-14s %-14s %-14s %s\n" % ("Altitude", _dist(r["altitude"]), "Vertical",
                                              "%.1f m/s" % r["vertical_speed"])
                + "%-14s %-14s %-14s %s\n" % ("Apoapsis", _dist(r["apoapsis"]) if r["eccentricity"] < 1 else "escape",
                                              "in", _time(r["time_to_apoapsis"]))
                + "%-14s %-14s %-14s %s\n" % ("Periapsis", _dist(r["periapsis"]), "in", _time(r["time_to_periapsis"]))
                + "%-14s %-14s %-14s %s\n" % ("Orbital speed", "%.1f m/s" % r["orbital_speed"],
                                              "Surface", "%.1f m/s" % r["surface_speed"])
                + "%-14s %-14s %-14s %s\n" % ("Stage", "%d of %d" % (v.stage, physics.STAGES - 1),
                                              "Stage fuel", "%.0f %%" % lf)
                + "%-14s %-14s %-14s %s\n" % ("G", "%.2f" % r["g_force"], "Heat", "%.0f %%" % (v.heat * 100))
                + "%-14s pitch %+6.1f  heading %5.1f  roll %+6.1f   %s\n" % (
                    "Attitude", r["pitch"], r["heading"], r["roll"], self._sas_text(sim, v))
                + "%-14s %-14s %-14s %s" % ("Mission time", _time(v.mission_time), "UT", _time(sim.ut)))
        self._refresh_game()
        self._refresh_board_tab()
        self._refresh_node()
        self.root.after(200, self._refresh)

    @staticmethod
    def _sas_text(sim, v) -> str:
        if not sim.game["sas"]:
            return "SAS off" + ("" if v.steered else ", launch guidance")
        mode = SAS_MODES[sim.sas_mode - 1]
        if isinstance(v.autopilot, tuple) and math.isfinite(v.sas_error):
            return "SAS %s, %.1f deg off" % (mode, v.sas_error)
        return "SAS %s (holding)" % mode

    def run(self) -> None:
        self.root.mainloop()
