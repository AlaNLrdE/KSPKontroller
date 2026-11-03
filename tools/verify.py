#!/usr/bin/env python3
"""Run every check we use to keep the plugin, the firmware, the install and the shared KSP folder consistent.

One command instead of many ad-hoc greps (saves tokens, and the checks are always the same):

    build/venv/Scripts/python.exe tools/verify.py            # static + install + log checks
    build/venv/Scripts/python.exe tools/verify.py --board    # also talk to the Arduino (KSP must be closed)
    build/venv/Scripts/python.exe tools/verify.py --snapshot # remember the GameData mods that exist now
    build/venv/Scripts/python.exe tools/verify.py --log PATH # analyse a saved KSP.log instead of the live one

Exit code: 0 all good (warnings allowed), 1 at least one FAIL.
RULE FOR WHOEVER EDITS THIS REPO: every time a new kind of check is needed (a bug found, a thing that had to be
verified by hand), add it here as a check_* function and register it in CHECKS. Do not repeat manual greps.
Only the Python standard library is used, plus tools/test_arduino.py (pyserial) for --board.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_KSP = r"C:\Users\alanl\wk\wk-rea\Kerbal Space Program"

SKETCH_DIR = os.path.join(ROOT, "arduino-sketch", "KSPIODemo9")
CS = os.path.join(ROOT, "KSPSerialIO", "KSPSerialIO", "KSPIO.cs")
ASSEMBLY_INFO = os.path.join(ROOT, "KSPSerialIO", "KSPSerialIO", "Properties", "AssemblyInfo.cs")
CHANGELOG = os.path.join(ROOT, "KSPSerialIO", "CHANGELOG.md")
REPO_CONFIG = os.path.join(ROOT, "KSPSerialIO", "config.xml")
BUILT_DLL = os.path.join(ROOT, "KSPSerialIO", "KSPSerialIO", "bin", "Release", "KSPSerialIO.dll")
SNAPSHOT = os.path.join(ROOT, "build", "gamedata_snapshot.json")

results = []  # (status, name, detail)


def report(status, name, detail=""):
    results.append((status, name, detail))


def read(path):
    with open(path, encoding="utf-8", errors="replace", newline="") as f:
        return f.read()


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def ksp_running():
    out = subprocess.run(["tasklist"], capture_output=True, text=True).stdout
    return "KSP_x64.exe" in out


def sketch_text():
    return "\n".join(read(os.path.join(SKETCH_DIR, f)) for f in sorted(os.listdir(SKETCH_DIR)) if f.endswith(".ino"))


# --------------------------------------------------------------------------------------------- static checks
def check_protocol_sizes(ctx):
    """Struct sizes asserted in the sketch must equal the constants used by the board test script."""
    ino = sketch_text()
    test = read(os.path.join(ROOT, "tools", "test_arduino.py"))
    for struct, const in (("VesselData", "VESSEL_DATA_SIZE"), ("ControlPacket", "CONTROL_PACKET_SIZE")):
        a = re.search(r"static_assert\(sizeof\(%s\)\s*==\s*(\d+)" % struct, ino)
        t = re.search(r"%s\s*=\s*(\d+)" % const, test)
        if not a or not t:
            report("FAIL", "protocol sizes: " + struct, "static_assert or test constant not found")
        elif a.group(1) != t.group(1):
            report("FAIL", "protocol sizes: " + struct, "sketch %s != tools/test_arduino.py %s" % (a.group(1), t.group(1)))
        else:
            report("PASS", "protocol sizes: " + struct, a.group(1) + " bytes")


def check_pins(ctx):
    """No two pin defines may share a pin number (a swapped LED once collided with another one)."""
    ino = read(os.path.join(SKETCH_DIR, "KSPIODemo9.ino"))
    pins = {}
    for name, num in re.findall(r"^#define\s+(\w+(?:LED|PIN))\s+(\d+)\b", ino, re.M):
        if name == "THROTTLEPIN":  # analog A0 shares the number 0 with nothing digital
            continue
        pins.setdefault(num, []).append(name)
    dup = {n: names for n, names in pins.items() if len(names) > 1}
    if dup:
        report("FAIL", "pins unique", "; ".join("%s: %s" % (n, ", ".join(v)) for n, v in dup.items()))
    else:
        report("PASS", "pins unique", "%d pins" % len(pins))
    ctx["pins"] = pins
    # the pin table in the docs must mention every digital pin in use
    overview = read(os.path.join(ROOT, "docs", "overview.rst"))
    missing = [n for n in pins if not re.search(r"^\s+\*\s+-\s+%s\s*$" % n, overview, re.M)]
    if missing:
        report("FAIL", "docs pin table", "pins not in docs/overview.rst: " + ", ".join(sorted(missing)))
    else:
        report("PASS", "docs pin table", "all pins documented")
    # and the docs must agree on which function sits on a pin
    for name, label in (("RCSLED", "RCS state LED"), ("SASLED", "SAS state LED"), ("GLED", "Green LED"),
                        ("STAGEPIN", "Stage push button")):
        m = re.search(r"^#define\s+%s\s+(\d+)" % name, ino, re.M)
        if m and not re.search(r"^\s+\*\s+-\s+%s\s*\n[^\n]*\n\s+-\s+%s" % (m.group(1), re.escape(label)), overview, re.M):
            report("FAIL", "docs pin function: " + name, "pin %s should be '%s' in docs/overview.rst" % (m.group(1), label))


def check_baud(ctx):
    sketch = re.search(r"Serial\.begin\((\d+)\)", sketch_text())
    cfg = re.search(r'name="BaudRate">(\d+)<', read(REPO_CONFIG))
    if not sketch or not cfg:
        report("FAIL", "baud rate", "Serial.begin or BaudRate not found")
    elif sketch.group(1) != cfg.group(1):
        report("FAIL", "baud rate", "sketch %s != config.xml %s (board only answers at the sketch rate)" % (sketch.group(1), cfg.group(1)))
    else:
        report("PASS", "baud rate", sketch.group(1))


def check_versions(ctx):
    asm = re.search(r'AssemblyVersion\("([\d.]+)"\)', read(ASSEMBLY_INFO))
    fileasm = re.search(r'AssemblyFileVersion\("([\d.]+)"\)', read(ASSEMBLY_INFO))
    log = re.search(r"\*\*([\d.]+)\*\*", read(CHANGELOG))
    if not (asm and fileasm and log):
        report("FAIL", "version", "could not read AssemblyInfo or CHANGELOG")
        return
    ctx["version"] = asm.group(1)
    if not (asm.group(1) == fileasm.group(1) == log.group(1)):
        report("FAIL", "version", "AssemblyVersion %s, FileVersion %s, CHANGELOG top %s" % (asm.group(1), fileasm.group(1), log.group(1)))
    else:
        report("PASS", "version", asm.group(1))


def check_config_defaults(ctx):
    cfg = read(REPO_CONFIG)
    thr = re.search(r'name="ThrottleEnable">(\d)<', cfg)
    # No potentiometer is wired: a floating analog input must never drive the throttle (modes 1-3).
    if thr and thr.group(1) in "123":
        report("WARN", "config ThrottleEnable", "mode %s lets the A0 input drive the throttle; fine only with a potentiometer" % thr.group(1))
    else:
        report("PASS", "config ThrottleEnable", "mode %s" % (thr.group(1) if thr else "?"))


def check_plugin_safety(ctx):
    """Source-level guards for bugs we already had."""
    cs = read(CS)
    if re.search(r"SetGroup\(KSPActionGroup\.Stage", cs):
        report("FAIL", "stage like the space bar", "KSPIO.cs toggles the Stage action group; only ActivateNextStage may be used")
    else:
        report("PASS", "stage like the space bar", "no Stage action group toggling")
    unguarded = [m for m in re.finditer(r"ActiveVessel\.ActionGroups\.SetGroup", cs)]
    start = cs.find("void Start()")
    start_end = cs.find("void Update()", start) if start >= 0 else -1
    in_start = [m for m in unguarded if start >= 0 and start < m.start() < (start_end if start_end > 0 else len(cs))]
    if in_start:
        report("FAIL", "Start() sync guarded", "%d direct SetGroup calls in Start(); use SafeSetGroup" % len(in_start))
    else:
        report("PASS", "Start() sync guarded", "uses SafeSetGroup")
    if "OnApplicationQuit" not in cs or "KSPSerialIO[PORT]" not in cs:
        report("FAIL", "port lifecycle logging", "KSPSerialIO[PORT] / OnApplicationQuit logging removed")
    else:
        report("PASS", "port lifecycle logging", "present")
    if "BuildFrame(" in cs and "TrySendPacket" in cs and "SenderWorker" in cs:
        report("PASS", "background writer", "present")
    else:
        report("FAIL", "background writer", "main-thread serial writes would block the game ~20 ms per packet")


# --------------------------------------------------------------------------------------------- install checks
def check_install(ctx):
    ksp = ctx["ksp"]
    gd = os.path.join(ksp, "GameData", "KSPSerialIO")
    dll = os.path.join(gd, "KSPSerialIO.dll")
    if not os.path.isdir(gd):
        report("FAIL", "plugin installed", gd + " missing")
        return
    for f in ("KSPSerialIO.dll", "PsimaxSerial.dll"):
        if not os.path.isfile(os.path.join(gd, f)):
            report("FAIL", "plugin installed", f + " missing")
            return
    if not os.path.isfile(BUILT_DLL):
        report("WARN", "installed == built", "no build output; run ./ci build_KSPSerialIO")
    elif sha(dll) != sha(BUILT_DLL):
        report("FAIL", "installed == built", "installed DLL differs from build/bin/Release (run ./ci deploy, KSP closed)")
    else:
        report("PASS", "installed == built", "identical DLL")
    cfg = os.path.join(gd, "PluginData", "KSPSerialIO", "config.xml")
    if not os.path.isfile(cfg):
        report("WARN", "installed config", "missing (the plugin will create the defaults)")
        return
    inst, repo = read(cfg), read(REPO_CONFIG)
    diffs = []
    for key in ("BaudRate", "ThrottleEnable", "PerfLog", "HandshakeDelay"):
        a = re.search(r'name="%s">([^<]*)<' % key, inst)
        b = re.search(r'name="%s">([^<]*)<' % key, repo)
        if (a.group(1) if a else None) != (b.group(1) if b else None):
            diffs.append("%s installed=%s repo=%s" % (key, a.group(1) if a else None, b.group(1) if b else None))
    report("WARN" if diffs else "PASS", "installed config", "; ".join(diffs) if diffs else "matches repo defaults")


def gamedata_mods(ksp):
    gd = os.path.join(ksp, "GameData")
    return sorted(n for n in os.listdir(gd))


def check_gamedata_snapshot(ctx):
    """Other mods must never disappear when someone deploys (the user asked for no replaced/omitted mods)."""
    now = gamedata_mods(ctx["ksp"])
    if ctx["args"].snapshot:
        os.makedirs(os.path.dirname(SNAPSHOT), exist_ok=True)
        with open(SNAPSHOT, "w") as f:
            json.dump(now, f, indent=1)
        report("PASS", "GameData snapshot", "saved %d entries" % len(now))
        return
    if not os.path.isfile(SNAPSHOT):
        report("WARN", "GameData snapshot", "none yet; run with --snapshot while the mod set is known good")
        return
    with open(SNAPSHOT) as f:
        before = json.load(f)
    gone = sorted(set(before) - set(now))
    new = sorted(set(now) - set(before))
    if gone:
        report("FAIL", "GameData mods intact", "missing since snapshot: " + ", ".join(gone[:15]))
    else:
        report("PASS", "GameData mods intact", "%d entries, none missing%s" % (len(now), (", new: " + ", ".join(new[:8])) if new else ""))


# --------------------------------------------------------------------------------------------- log checks
def check_log(ctx):
    path = ctx["log"]
    if not os.path.isfile(path):
        report("WARN", "KSP.log", "not found: " + path)
        return
    lines = read(path).splitlines()
    plugin = [l for l in lines if "KSPSerialIO" in l]
    if not plugin:
        report("WARN", "log: plugin loaded", "no KSPSerialIO lines (plugin disabled or KSP not run yet)")
        return
    ver = re.search(r"KSPSerialIO v([\d.]+)", "\n".join(lines))
    want = ctx.get("version")
    if ver and want and ver.group(1) != want:
        report("FAIL", "log: plugin version", "KSP loaded %s but the repo is %s (stale install?)" % (ver.group(1), want))
    else:
        report("PASS", "log: plugin version", ver.group(1) if ver else "?")
    size = re.search(r"Output packet size: (\d+)/", "\n".join(plugin))
    if size:
        ino = re.search(r"static_assert\(sizeof\(VesselData\)\s*==\s*(\d+)", sketch_text())
        ok = ino and ino.group(1) == size.group(1)
        report("PASS" if ok else "FAIL", "log: VesselData size", "plugin %s, sketch %s" % (size.group(1), ino.group(1) if ino else "?"))
    text = "\n".join(plugin)
    if "KSP Display not found" in text:
        report("FAIL", "log: board found", "'KSP Display not found' (board hung, wrong baud, port busy or not plugged)")
    elif "found KSP Display" in text:
        report("PASS", "log: board found", re.search(r"found KSP Display at (\S+)", text).group(1))
    else:
        report("WARN", "log: board found", "no discovery result in this log")
    bad = [l for l in plugin if re.search(r"send failed|too many send failures|could not queue", l)]
    report("FAIL" if bad else "PASS", "log: send failures", ("%d, first: %s" % (len(bad), bad[0][:140])) if bad else "none")
    # port lifecycle: every close must finish
    closing = [i for i, l in enumerate(plugin) if "KSPSerialIO[PORT] closing port (Close()" in l]
    hung = [i for i in closing if not any("KSPSerialIO[PORT] closed in" in l for l in plugin[i + 1:i + 6])]
    if hung:
        report("FAIL", "log: port Close() finished", "%d close(s) without 'closed in' (Close() hung)" % len(hung))
    elif closing:
        report("PASS", "log: port Close() finished", "%d clean close(s)" % len(closing))
    else:
        report("WARN", "log: port Close() finished", "no close recorded (still in flight, or KSP crashed)")
    # exceptions whose stack contains our plugin
    exc = []
    for i, l in enumerate(lines):
        if l.startswith("[EXC") and any("KSPSerialIO.KSPSerial" in x for x in lines[i + 1:i + 12]):
            exc.append(l[:120])
    report("FAIL" if exc else "PASS", "log: exceptions from the plugin", ("%d, first: %s" % (len(exc), exc[0])) if exc else "none")
    # performance: the plugin must stay a tiny share of the frame
    perf = [l for l in plugin if " perf window_s=" in l]
    if perf:
        def vals(key):
            return [float(m.group(1)) for l in perf for m in [re.search(r"\b%s=([-\d.]+)" % key, l)] if m]
        share, wavg = vals("upd_share_pct"), vals("write_avg_ms")
        worst = max(share) if share else 0
        report("FAIL" if worst > 2 else "PASS", "log: plugin frame share", "max %.2f%% over %d windows" % (worst, len(perf)))
        if wavg and max(wavg) > 5:
            report("WARN", "log: serial write time", "max avg write %.1f ms (background thread, so it does not cost frames)" % max(wavg))
    else:
        report("WARN", "log: perf lines", "none (PerfLog=0, or a short session)")
    ctrl = [l for l in plugin if "KSPSerialIO[CTRL]" in l]
    report("PASS", "log: stage presses", "%d" % len(ctrl))


def check_git(ctx):
    def git(*a):
        return subprocess.run(["git", "-C", ROOT] + list(a), capture_output=True, text=True).stdout.strip()
    dirty = [l for l in git("status", "--short").splitlines() if not l.startswith("??")]
    ahead = git("rev-list", "--count", "@{u}..HEAD") if git("rev-parse", "--abbrev-ref", "@{u}") else ""
    report("WARN" if dirty else "PASS", "git: uncommitted changes", "%d file(s)" % len(dirty) if dirty else "clean")
    if ahead.isdigit() and int(ahead) > 0:
        report("WARN", "git: unpushed commits", "%s commit(s) not pushed" % ahead)
    else:
        report("PASS", "git: unpushed commits", "none" if ahead.isdigit() else "no upstream")


# --------------------------------------------------------------------------------------------- hardware
def check_board(ctx):
    if ksp_running():
        report("WARN", "board", "skipped: KSP is running and owns the serial port")
        return
    port = ctx["args"].port
    cmd = [sys.executable, os.path.join(ROOT, "tools", "test_arduino.py")] + ([port] if port else [])
    p = subprocess.run(cmd, capture_output=True, text=True)
    out = p.stdout
    for name in ("handshake", "telemetry", "idle timeout"):
        m = re.search(r"\[(PASS|FAIL)\] %s: (.*)" % name, out)
        report(m.group(1) if m else "FAIL", "board: " + name, m.group(2) if m else "no result (board silent: press RESET once, or double-tap RESET and re-flash)")


CHECKS = [check_protocol_sizes, check_pins, check_baud, check_versions, check_config_defaults, check_plugin_safety,
          check_install, check_gamedata_snapshot, check_log, check_git]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--ksp", default=os.environ.get("KSPDIR", DEFAULT_KSP), help="KSP install directory")
    ap.add_argument("--log", help="analyse this KSP.log instead of <ksp>/KSP.log")
    ap.add_argument("--board", action="store_true", help="also test the Arduino over serial (KSP must be closed)")
    ap.add_argument("--port", help="serial port for --board, for example COM5")
    ap.add_argument("--snapshot", action="store_true", help="record the GameData entries now present")
    args = ap.parse_args()

    ctx = {"ksp": os.path.normpath(args.ksp), "args": args}
    ctx["log"] = args.log or os.path.join(ctx["ksp"], "KSP.log")
    report("PASS" if not ksp_running() else "WARN", "KSP process", "closed" if not ksp_running() else "running (install/flash/board checks need it closed)")
    for chk in CHECKS:
        try:
            chk(ctx)
        except Exception as e:  # a broken check must never hide the others
            report("FAIL", chk.__name__, "check crashed: %r" % e)
    if args.board:
        try:
            check_board(ctx)
        except Exception as e:
            report("FAIL", "check_board", "check crashed: %r" % e)

    width = max(len(n) for _, n, _ in results)
    for status, name, detail in results:
        print("%-4s %-*s  %s" % (status, width, name, detail))
    fails = sum(1 for s, _, _ in results if s == "FAIL")
    warns = sum(1 for s, _, _ in results if s == "WARN")
    print("\n%d checks: %d FAIL, %d WARN" % (len(results), fails, warns))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
