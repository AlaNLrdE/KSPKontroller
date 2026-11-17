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
import shutil
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
HMI_PLUGIN_DIR = os.path.join(ROOT, "KSPSerialIO", "KSPSerialIO.Hmi")
HMI_ASSEMBLY_INFO = os.path.join(HMI_PLUGIN_DIR, "Properties", "AssemblyInfo.cs")
HMI_BUILT_DLL = os.path.join(HMI_PLUGIN_DIR, "bin", "Release", "KSPSerialIO.Hmi.dll")
HMI_BUILT_XML = os.path.join(HMI_PLUGIN_DIR, "bin", "Release", "KSPSerialIO.Hmi.xml")

results = []  # (status, name, detail)


def report(status, name, detail=""):
    results.append((status, name, detail))


def read(path):
    """Text of a repo or game file with universal newlines: CRLF and CR become LF, so content checks behave the
    same in the main checkout (core.autocrlf=true, CRLF files) and in agent worktrees (LF files). Regexes may use
    `$` with re.M and literal "\\n" anchors; never test for "\\r" in the result."""
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def ksp_running():
    if os.name == "nt":
        out = subprocess.run(["tasklist"], capture_output=True, text=True).stdout
        return "KSP_x64.exe" in out
    # Linux runs KSP.x86_64, macOS KSP.app/Contents/MacOS/KSP (same patterns as ./ci ksp_running)
    return subprocess.run(["pgrep", "-f", "KSP.x86_64|KSP.app"], capture_output=True).returncode == 0


def sketch_text():
    return "\n".join(read(os.path.join(SKETCH_DIR, f)) for f in sorted(os.listdir(SKETCH_DIR)) if f.endswith(".ino"))


# --------------------------------------------------------------------------------------------- static checks
def check_protocol_sizes(ctx):
    """Struct sizes asserted in the sketch must equal the constants used by the board test script."""
    ino = sketch_text()
    test = read(os.path.join(ROOT, "tools", "test_arduino.py"))
    for struct, const in (("VesselData", "VESSEL_DATA_SIZE"), ("ControlPacket", "CONTROL_PACKET_SIZE"),
                          ("DebugPacket", "DEBUG_PACKET_SIZE")):
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
    # KSPSerialIO.Hmi.dll is deployed with the plugin and carries the same version.
    hmi = re.findall(r'Assembly(?:File)?Version\("([\d.]+)"\)', read(HMI_ASSEMBLY_INFO))
    if not (asm.group(1) == fileasm.group(1) == log.group(1)):
        report("FAIL", "version", "AssemblyVersion %s, FileVersion %s, CHANGELOG top %s" % (asm.group(1), fileasm.group(1), log.group(1)))
    elif len(hmi) != 2 or set(hmi) != {asm.group(1)}:
        report("FAIL", "version", "KSPSerialIO %s but KSPSerialIO.Hmi AssemblyInfo %s" % (asm.group(1), ", ".join(hmi) or "?"))
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


def check_no_direct_debug_log(ctx):
    """Debug.Log from a worker thread crashes KSP's debug console (Unity UI off the main thread)."""
    cs = read(CS)
    start = cs.find("public static class KSPLogQueue")
    end = cs.find("[KSPAddon", start) if start >= 0 else -1
    if start < 0 or end < 0:
        report("FAIL", "no direct Debug.Log", "KSPLogQueue class not found in KSPIO.cs")
        return
    rest = cs[:start] + cs[end:]
    hits = [i + 1 for i, line in enumerate(rest.splitlines())
            if re.search(r"Debug\.Log(Warning|Error|Format|Exception)?\(", line) and not line.lstrip().startswith(("//", "*", "///"))]
    if hits:
        report("FAIL", "no direct Debug.Log", "Debug.Log outside KSPLogQueue (unsafe from worker threads), lines %s" % hits[:10])
    else:
        report("PASS", "no direct Debug.Log", "all logging goes through KSPLogQueue")


def check_ci_deploy(ctx):
    """Exercise deployment ordering and failure guards with stubbed hardware operations."""
    result = subprocess.run([sys.executable, '-m', 'unittest', 'discover', '-s', 'tools', '-p', 'test_ci_deploy.py'],
                            cwd=ROOT, capture_output=True, text=True, timeout=30)
    report('PASS' if result.returncode == 0 else 'FAIL', 'ci: deploy all projects',
           'deployment guards, cold-cache reference paths and WDT metadata/source preservation' if result.returncode == 0
           else (result.stdout + result.stderr)[-1000:])


def check_verify_newlines(ctx):
    """verify.py itself: content checks pass with LF (agent worktrees) and CRLF (core.autocrlf=true checkout) files."""
    result = subprocess.run([sys.executable, "-m", "unittest", "discover", "-s", "tools", "-p", "test_verify.py"],
                            cwd=ROOT, capture_output=True, text=True, timeout=60,
                            env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"))
    report("PASS" if result.returncode == 0 else "FAIL", "verify: LF/CRLF agnostic",
           "ship report contract with LF and CRLF copies" if result.returncode == 0 else (result.stdout + result.stderr)[-1000:])


def run_firmware_test(name, sanitize=False):
    """Compile actual firmware functions with fake peripherals; never touch hardware."""
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        report("WARN", "firmware " + name, "native C++ compiler missing")
        return
    sketch = os.path.join(SKETCH_DIR, "KSPIODemo9.ino")
    source = read(sketch)
    start = source.index("#pragma pack(push, 1)")
    end = source.index("#pragma pack(pop)", start) + len("#pragma pack(pop)")
    build = os.path.join(ROOT, "build", "firmware-tests")
    os.makedirs(build, exist_ok=True)
    with open(os.path.join(build, "firmware_packets.hpp"), "w", encoding="utf-8") as out:
        out.write(source[start:end])
    binary = os.path.join(build, "test_" + name + (".exe" if os.name == "nt" else ""))
    test = os.path.join(ROOT, "tools", "firmware-tests", "test_" + name + ".cpp")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-I", build, "-I", os.path.dirname(test)]
    if sanitize:
        flags += ["-fsanitize=undefined,float-cast-overflow", "-fno-sanitize-recover=all"]
    result = subprocess.run([compiler] + flags + [test, "-o", binary],
                            capture_output=True, text=True, timeout=60)
    if result.returncode == 0:
        result = subprocess.run([binary], capture_output=True, text=True, timeout=10)
    report("PASS" if result.returncode == 0 else "FAIL", "firmware " + name, (result.stdout + result.stderr).strip())



def check_firmware_receiver(ctx):
    run_firmware_test("receiver")


def check_firmware_matrix(ctx):
    run_firmware_test("matrix", sanitize=True)


def check_button_events(ctx):
    """Exercise the actual C# receive-time event queue without Unity or a serial device."""
    dotnet = shutil.which("dotnet")
    if not dotnet:
        report("WARN", "button events", "dotnet SDK missing; cannot run the standalone C# regression tests")
        return
    project = os.path.join(ROOT, "tools", "button-events-tests", "ButtonEvents.Tests.csproj")
    artifacts = os.path.join(ROOT, "build", "button-events-tests")
    cmd = [dotnet, "build", project, "--configuration", "Release", "--artifacts-path", artifacts, "--nologo"]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    if result.returncode == 0:
        dll = os.path.join(artifacts, "bin", "ButtonEvents.Tests", "release", "ButtonEvents.Tests.dll")
        result = subprocess.run([dotnet, dll], capture_output=True, text=True, timeout=30)
    detail = (result.stdout + result.stderr).strip()
    report("PASS" if result.returncode == 0 else "FAIL", "button events", detail)


def run_plugin_assembly_test(kind, project_name):
    """Exercise real methods of the built plugin without Unity callbacks or hardware."""
    dotnet = shutil.which("dotnet")
    if not dotnet or not os.path.isfile(BUILT_DLL):
        report("WARN", "plugin " + kind, "requires .NET 8 SDK and ./ci build_KSPSerialIO output")
        return
    sources = [CS, os.path.join(os.path.dirname(CS), "ButtonEvents.cs")]
    if max(os.path.getmtime(p) for p in sources) > os.path.getmtime(BUILT_DLL):
        report("FAIL", "plugin " + kind, "plugin assembly is stale: run ./ci build_KSPSerialIO first")
        return
    project = os.path.join(ROOT, "tools", "plugin-" + kind + "-tests", project_name + ".csproj")
    artifacts = os.path.join(ROOT, "build", "plugin-" + kind + "-tests")
    result = subprocess.run([dotnet, "build", project, "--configuration", "Release", "--artifacts-path", artifacts,
                             "--nologo", "-p:NuGetAudit=false", "--ignore-failed-sources"], capture_output=True, text=True, timeout=120)
    if result.returncode == 0:
        dll = os.path.join(artifacts, "bin", project_name, "release", project_name + ".dll")
        result = subprocess.run([dotnet, dll, ROOT], capture_output=True, text=True, timeout=30)
    report("PASS" if result.returncode == 0 else "FAIL", "plugin " + kind, (result.stdout + result.stderr).strip())



def check_plugin_receiver(ctx):
    run_plugin_assembly_test("receiver", "PluginReceiver.Tests")


def check_plugin_connection(ctx):
    run_plugin_assembly_test("connection", "PluginConnection.Tests")


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


def check_reconnect_guards(ctx):
    """Source guards for the automatic reconnection (WARN, not FAIL, while the code has not landed yet)."""
    cs = read(CS)
    has_tag = "KSPSerialIO[LINK]" in cs
    has_sup = re.search(r"Supervisor|LinkSupervisor|SuperviseLink", cs) and re.search(r"new Thread\(|ThreadPool|Task\.Run", cs)
    if has_tag and has_sup:
        report("PASS", "reconnect: plugin supervisor", "KSPSerialIO[LINK] logging and a supervisor thread present")
    else:
        report("WARN", "reconnect: plugin supervisor", "missing in KSPIO.cs: " + ", ".join(
            n for n, ok in (("KSPSerialIO[LINK] tag", has_tag), ("supervisor + thread", has_sup)) if not ok))
    ino = sketch_text()
    m = re.search(r"#define\s+LINKRESETMS\s+(\d+)", ino)
    if not m:
        report("FAIL", "reconnect: sketch LINKRESETMS", "LINKRESETMS not defined in the sketch")
    elif int(m.group(1)) > 45000:
        report("FAIL", "reconnect: sketch LINKRESETMS", "%s ms is above 45000: the board stays wedged too long" % m.group(1))
    elif int(m.group(1)) < 20000:
        report("FAIL", "reconnect: sketch LINKRESETMS", "%s ms is below 20000: a scene load (the plugin sends nothing) resets the board" % m.group(1))
    else:
        report("PASS", "reconnect: sketch LINKRESETMS", "%s ms" % m.group(1))
    wdt = re.search(r"\bWDT\.begin\(", ino)
    report("PASS" if wdt else "FAIL", "reconnect: sketch watchdog", "WDT started" if wdt else "WDT.begin() not found: a hung loop() would never reset")


def check_reconnect_log(ctx):
    """KSPSerialIO[LINK] lines: every loss must be followed by a reconnect, and searching must not give up."""
    path = ctx["log"]
    if not os.path.isfile(path):
        return
    events = [l for l in read(path).splitlines() if "KSPSerialIO[LINK]" in l]
    if not events:
        report("PASS", "log: link recovery", "no link loss recorded")
        return
    losses, recovered, downs = 0, 0, []
    pending, attempts, worst = False, 0, 0
    for l in events:
        if re.search(r"\] lost:", l):
            losses += 1
            pending, attempts = True, 0
        m = re.search(r"searching attempt (\d+)", l)
        if m:
            pending = True
            attempts = int(m.group(1))
            worst = max(worst, attempts)
        m = re.search(r"reconnected on \S+ after (\d+) attempts?, (\d+) ms down", l)
        if m:
            recovered += 1
            pending, attempts = False, 0
            downs.append(int(m.group(2)))
    if pending and attempts > 10:
        report("FAIL", "log: link recovery", "still searching after %d attempts when the log ends (no candidate answered the handshake)" % attempts)
    elif pending:
        report("FAIL", "log: link recovery", "log ends while the link is lost/searching (attempt %d)" % attempts)
    elif losses > recovered:
        report("FAIL", "log: link recovery", "%d loss(es) but only %d reconnect(s)" % (losses, recovered))
    elif losses:
        report("WARN", "log: link recovery", "%d loss(es), all recovered, longest down %d ms" % (losses, max(downs) if downs else 0))
    else:
        report("PASS", "log: link recovery", "searching only at startup, reconnected %d time(s)" % recovered)


# --------------------------------------------------------------------------------------------- install checks
def ksp_missing(ctx, name):
    """Install checks need a KSP folder: without one (another OS, no KSPDIR) they are skipped, not failed."""
    if os.path.isdir(os.path.join(ctx["ksp"], "GameData")):
        return False
    report("WARN", name, "skipped: no KSP install at %s (set KSPDIR, ./ci ksp_list)" % (ctx["ksp"] or "(none found)"))
    return True


def windows_firewall_rule(name):
    """netsh output for an inbound rule, or None off Windows (no netsh there)."""
    if os.name != "nt":
        return None
    try:
        return subprocess.run(["netsh", "advfirewall", "firewall", "show", "rule", "name=" + name],
                              capture_output=True, text=True, timeout=15).stdout
    except (OSError, subprocess.SubprocessError):
        return ""


def check_install(ctx):
    if ksp_missing(ctx, "plugin installed"):
        return
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
    if ksp_missing(ctx, "GameData snapshot"):
        return
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
    recovered = "KSPSerialIO[LINK] reconnected on" in text
    if "KSP Display not found" in text and recovered:
        report("PASS", "log: board found", "not found by the first discovery, then the link supervisor connected it")
    elif "KSP Display not found" in text:
        report("FAIL", "log: board found", "'KSP Display not found' and never connected (board hung, wrong baud, port busy or not plugged)")
    elif "found KSP Display" in text:
        report("PASS", "log: board found", re.search(r"found KSP Display at (\S+)", text).group(1))
    else:
        report("WARN", "log: board found", "no discovery result in this log")
    bad = [l for l in plugin if re.search(r"send failed|too many send failures|could not queue", l)]
    if bad and recovered:
        # a loss that the supervisor recovered is the mechanism working; the LINK checks judge whether it did
        report("WARN", "log: send failures", "%d, all followed by a reconnection (first: %s)" % (len(bad), bad[0][:100]))
    else:
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
        # A staging makes KSP itself spend hundreds of ms inside ActivateNextStage (counted in our Update), so single
        # windows can spike; judge the 90th percentile and only report the max.
        ordered = sorted(share)
        p90 = ordered[int(len(ordered) * 0.9)] if ordered else 0
        worst = ordered[-1] if ordered else 0
        report("FAIL" if p90 > 2 else "PASS", "log: plugin frame share", "p90 %.2f%%, max %.2f%% over %d windows" % (p90, worst, len(perf)))
        rx = vals("rx_bytes")
        if rx:
            # the board sends a ControlPacket 40 times a second; a window with writes but no bytes means it went silent
            silent = [i for i, (r, w) in enumerate(zip(rx, vals("writes"))) if r == 0 and w > 0]
            if silent:
                report("FAIL", "log: board kept talking", "%d window(s) of %d with writes but 0 bytes received (first at window %d)" % (len(silent), len(rx), silent[0] + 1))
            else:
                report("PASS", "log: board kept talking", "bytes received in every window")
            # The supervisor must notice a silent board: rx_age far above LinkTimeoutMs (3 s) while link_state=up means
            # the loss detection did not fire (board-dead-1923: up for 7 minutes with no byte received).
            stuck = [l for l in perf if "link_state=up" in l
                     for m in [re.search(r"\brx_age_ms=(\d+)", l)] if m and int(m.group(1)) > 10000]
            report("FAIL" if stuck else "PASS", "log: link loss detected",
                   ("%d window(s) with link_state=up and rx_age_ms > 10000, first: %s" % (len(stuck), stuck[0][5:17]))
                   if stuck else "no window stayed up on a silent board")
        if wavg and max(wavg) > 5:
            report("WARN", "log: serial write time", "max avg write %.1f ms (background thread, so it does not cost frames)" % max(wavg))
    else:
        report("WARN", "log: perf lines", "none (PerfLog=0, or a short session)")
    board = [l for l in plugin if "KSPSerialIO[BOARD] uptime_ms=" in l]
    if board:
        def bvals(key):
            return [int(m.group(1)) for l in board for m in [re.search(r"\b%s=(\d+)" % key, l)] if m]
        reboots = [l for l in plugin if "KSPSerialIO[BOARD] REBOOT" in l]
        report("WARN" if reboots else "PASS", "log: board reboots", ("%d, first: %s" % (len(reboots), reboots[0][:140])) if reboots else "none")
        bad = bvals("rx_bad_cs")[-1:] + bvals("rx_bad_size")[-1:]
        report("WARN" if any(bad) else "PASS", "log: board dropped packets", "bad checksum/size at the end: %s" % bad)
        loop_us, tx_us = bvals("loop_max_us"), bvals("tx_max_us")
        report("WARN" if (loop_us and max(loop_us) > 100000) or (tx_us and max(tx_us) > 50000) else "PASS",
               "log: board loop/send time", "max loop %d us, max send %d us" % (max(loop_us or [0]), max(tx_us or [0])))
        last = bvals("uptime_ms")[-1]
        report("PASS", "log: board diagnostics", "%d reports, last uptime %d ms, last: %s" % (len(board), last, board[-1].split("] ", 1)[-1][:150]))
    elif perf:
        report("WARN", "log: board diagnostics", "no KSPSerialIO[BOARD] lines (old firmware, or no link)")
    ctrl = [l for l in plugin if "KSPSerialIO[CTRL]" in l]
    report("PASS", "log: stage presses", "%d" % len(ctrl))


def check_lan_names(ctx):
    """Resolve the three peer names with a bounded wait; local addresses/config never enter the repo."""
    import ipaddress
    import queue
    import socket
    import threading
    import time
    path = ctx["args"].lan_config
    if not os.path.isfile(path):
        report("FAIL" if ctx["args"].lan_only else "WARN", "LAN peer names", "local manifest missing: " + path)
        return
    try:
        with open(path, encoding="utf-8") as f:
            hosts = json.load(f)["hosts"]
        if not isinstance(hosts, dict) or set(hosts) != {"mac", "pc", "pi"}:
            raise ValueError("hosts must contain mac, pc and pi")
        for role, host in hosts.items():
            if not isinstance(host, str) or not host or any(c.isspace() for c in host):
                raise ValueError("invalid hostname for " + role)
            try:
                ipaddress.ip_address(host)
            except ValueError:
                continue
            raise ValueError("numeric endpoint for " + role + "; use a stable hostname")
    except (ValueError, KeyError, TypeError, OSError) as error:
        report("FAIL", "LAN peer names", str(error))
        return
    results_queue = queue.Queue()
    def resolve(role, host):
        try:
            addresses = sorted({r[4][0] for r in socket.getaddrinfo(host, None, socket.AF_INET)})
            usable = [a for a in addresses if not ipaddress.ip_address(a).is_loopback]
            results_queue.put((role, host, usable, "" if usable else "no non-loopback IPv4 address"))
        except OSError as error:
            results_queue.put((role, host, [], str(error)))
    for role, host in hosts.items():
        threading.Thread(target=resolve, args=(role, host), daemon=True).start()
    deadline = time.monotonic() + getattr(ctx["args"], "lan_timeout", 4.0)
    received = {}
    while len(received) < len(hosts):
        try:
            role, host, addresses, error = results_queue.get(timeout=max(0, deadline - time.monotonic()))
            received[role] = (host, addresses, error)
        except queue.Empty:
            break
    for role, host in hosts.items():
        _, addresses, error = received.get(role, (host, [], "name-resolution timeout"))
        report("FAIL" if error else "PASS", "LAN name: " + role,
               host + ": " + (error or ", ".join(addresses)))


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


# --------------------------------------------------------------------------------------------- Raspberry Pi HMI
HMI_DIR = os.path.join(ROOT, "hmi-cpp")


def check_hmi_render(ctx):
    """Draw time of the installed HMI on the Pi: ksphmi --bench draws demo frames in memory only (--offscreen never
    replaces the live framebuffer). The layout and clipping checks themselves are ctest's (./ci test_hmi)."""
    host = os.environ.get('HMI_HOST', 'ksphmi')
    bench = ('KSPHMI_APP_DIR=~/ksphmi/cpp KSPHMI_LOCAL_CONFIG=/dev/null '
             '~/ksphmi/cpp/build/ksphmi --bench 5 --offscreen --size 2520x1680')
    try:
        remote = subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=3', host, bench],
                                capture_output=True, text=True, timeout=30)
        match = re.search(r'HMI_RENDER_MS=([\d.]+)', remote.stdout)
    except (OSError, subprocess.SubprocessError):
        match = None
    if match:
        milliseconds = float(match.group(1))
        report('PASS' if milliseconds < 75 else 'WARN', 'hmi: Pi render budget',
               '%.1f ms/frame at 2520x1680, excluding framebuffer copy (~21 ms)' % milliseconds)
    else:
        report('WARN', 'hmi: Pi render budget', 'ksphmi --bench unavailable over SSH (./ci deploy_hmi)')


def check_hmi_context(ctx):
    """Keep the optional read-only scene channel consistent across game, HMI and simulator."""
    source = read(os.path.join(ROOT, "KSPSerialIO", "KSPSerialIO", "HmiContext.cs"))
    client = read(os.path.join(HMI_DIR, "src", "scenes.cpp")) + read(os.path.join(HMI_DIR, "include", "scenes.hpp"))
    server = read(os.path.join(ROOT, "tools", "kspsim", "krpcserver.py"))
    ok = all("KSPHMI/1" in s and "50002" in s for s in (source, client, server))
    for facility in ("AstronautComplex", "MissionControl", "RnDComplex", "AdministrationFacility"):
        for event in ("Spawn", "Despawn"):
            name = "onGUI" + facility + event
            ok = ok and name + ".Add(" in source and name + ".Remove(" in source
    project = read(os.path.join(ROOT, "KSPSerialIO", "KSPSerialIO", "KSPSerialIO.csproj"))
    ok = ok and 'Compile Include="HmiContext.cs"' in project
    # Optional " skin=<name>" field: only the simulator sends it, the HMI parses it; the plugin never does.
    skin_ok = "skin=" in client and "skin=" in server and "skin=" not in source
    report("PASS" if ok and skin_ok else "FAIL", "hmi: scene context contract",
           "UDP 50002 protocol, facility event subscriptions, optional skin field (simulator only)"
           if skin_ok else "skin= field must be parsed by hmi-cpp, sent by kspsim, and absent from HmiContext.cs")
    out = windows_firewall_rule("KSP HMI scene context")
    if out is None:
        report("PASS", "hmi: scene context firewall", "not Windows: no netsh rule (allow KSP in this OS's firewall)")
        return
    report("PASS" if "50002" in out else "WARN", "hmi: scene context firewall",
           "UDP 50002 rule present" if "50002" in out else "missing KSP HMI scene context rule (UDP 50002)")


def check_hmi_cpp_layout(ctx):
    """hmi-cpp keeps .cpp sources in src/ and .hpp/.h headers in include/, nothing else in either."""
    bad = []
    for sub, allowed in (("src", (".cpp",)), ("include", (".hpp", ".h"))):
        folder = os.path.join(HMI_DIR, sub)
        for name in sorted(os.listdir(folder)) if os.path.isdir(folder) else ["(missing)"]:
            if not name.endswith(allowed):
                bad.append("%s/%s" % (sub, name))
    # The broad HMI LF rule must never normalize image/font bytes during git add or a Windows checkout.
    attributes = read(os.path.join(ROOT, ".gitattributes"))
    for extension in ("png", "bmp", "ttf"):
        if "hmi-cpp/assets/**/*.%s binary" % extension not in attributes:
            bad.append("HMI .%s assets need a binary Git attribute" % extension)
    # deploy_hmi tars a fixed list: a new top-level folder (include/ on 2026-11-14) broke the Pi build when missing.
    m = re.search(r"\btar\b[^|\n]*?-cf - ([^)]*)\)", read(os.path.join(ROOT, "ci")))  # options may precede -cf
    shipped = set(m.group(1).split()) if m else set()
    for name in sorted(os.listdir(HMI_DIR)):
        if os.path.isdir(os.path.join(HMI_DIR, name)) and name != "build" and name not in shipped:
            bad.append("deploy_hmi does not ship %s/" % name)
    report("FAIL" if bad else "PASS", "hmi: source layout",
           ", ".join(bad) if bad else "src/ only .cpp, include/ only headers, deploy_hmi ships every folder")


def check_hmi_cpp(ctx):
    """The HMI (hmi-cpp/) must have a view for every scene the plugin and the simulator report, read the resources
    the simulator serves, and keep its LF rule (it is built on the Pi)."""
    src = os.path.join(HMI_DIR, "src")
    cpp_scenes = read(os.path.join(src, "scenes.cpp"))
    views = re.findall(r'^        \{"(\w+)", "', cpp_scenes, re.M)
    problems = []
    sim = re.search(r"^SCENES = \(([^)]*)\)", read(os.path.join(ROOT, "tools", "kspsim", "sim.py")), re.M)
    sim_scenes = set(re.findall(r'"(\w+)"', sim.group(1)) if sim else ()) - {"flight", "closed"}
    if not sim_scenes or sim_scenes != set(views):
        problems.append("views %s vs kspsim scenes %s" % (sorted(views), sorted(sim_scenes)))
    plugin = read(os.path.join(ROOT, "KSPSerialIO", "KSPSerialIO", "HmiContext.cs"))
    for scene in set(re.findall(r'(?:facility|scene) = "(\w+)"', plugin)) - {"spacecenter", "trackstation"}:
        if scene not in views:
            problems.append("plugin scene %s has no view" % scene)
    model = read(os.path.join(HMI_DIR, "include", "model.hpp"))
    names = re.findall(r'"(\w+)"', model.split("RESOURCE_NAMES", 1)[1].split(";", 1)[0])
    served = re.search(r"fields = \{(.*?)\}\r?\n", read(os.path.join(ROOT, "tools", "kspsim", "krpcserver.py")), re.S)
    if not served or names != re.findall(r"'(\w+)': \(", served.group(1)):
        problems.append("RESOURCE_NAMES order vs kspsim")
    if "hmi-cpp/** text eol=lf" not in read(os.path.join(ROOT, ".gitattributes")):
        problems.append(".gitattributes LF rule")
    report("FAIL" if problems else "PASS", "hmi: views and resources",
           ", ".join(problems) if problems else "%d scene views, %d resources, LF rule" % (len(views), len(names)))


INTRO_VIDEO = "assets/intro/KSPKontroller_pi_Intro.mp4"


def check_hmi_intro(ctx):
    """The Pi's boot intro: the video asset exists and is an MP4, setup-kiosk.sh installs mpv, the kiosk unit runs
    ksphmi-intro.sh in RuntimeDirectory=ksphmi-intro, and the script and hmi-cpp (intro.hpp) agree on the mpv IPC
    socket /run/ksphmi-intro/mpv.sock that the HMI uses to take the display over from mpv."""
    problems = []
    video = os.path.join(HMI_DIR, *INTRO_VIDEO.split("/"))
    if not os.path.isfile(video):
        problems.append("%s missing" % INTRO_VIDEO)
    else:
        with open(video, "rb") as f:
            head = f.read(12)
        if head[4:8] != b"ftyp":
            problems.append("%s is not an MP4 (git LFS pointer or text conversion?)" % INTRO_VIDEO)
    systemd = os.path.join(HMI_DIR, "systemd")
    kiosk = read(os.path.join(systemd, "setup-kiosk.sh"))
    unit = read(os.path.join(systemd, "ksphmi-splash.service"))
    script_path = os.path.join(systemd, "ksphmi-intro.sh")
    script = read(script_path) if os.path.isfile(script_path) else ""
    header = read(os.path.join(HMI_DIR, "include", "intro.hpp"))
    if "apt-get install -y --no-install-recommends mpv" not in kiosk:
        problems.append("setup-kiosk.sh does not install mpv")
    if "ksphmi-intro.sh" not in kiosk:
        problems.append("setup-kiosk.sh does not mention ksphmi-intro.sh")
    if not script:
        problems.append("systemd/ksphmi-intro.sh missing")
    if not re.search(r"^ExecStart=/bin/sh @HOME@/ksphmi/cpp/systemd/ksphmi-intro\.sh$", unit, re.M):
        problems.append("ksphmi-splash.service does not run ksphmi-intro.sh")
    run_dir = re.search(r"^RuntimeDirectory=(\S+)$", unit, re.M)
    hdr_dir = re.search(r'INTRO_DIR = "/run/([^"]+)"', header)
    hdr_sock = re.search(r'INTRO_SOCKET = "([^"]+)"', header)
    hdr_start = re.search(r'INTRO_STARTING = "([^"]+)"', header)
    if not (run_dir and hdr_dir and run_dir.group(1) == hdr_dir.group(1)):
        problems.append("RuntimeDirectory vs intro.hpp INTRO_DIR")
    if not hdr_dir or '${RUNTIME_DIRECTORY:-/run/%s}' % hdr_dir.group(1) not in script:
        problems.append("ksphmi-intro.sh default directory vs intro.hpp")
    if not hdr_sock or 'sock="$dir/%s"' % hdr_sock.group(1) not in script:
        problems.append("ksphmi-intro.sh socket name vs intro.hpp INTRO_SOCKET")
    if not hdr_start or 'starting="$dir/%s"' % hdr_start.group(1) not in script:
        problems.append("ksphmi-intro.sh starting marker vs intro.hpp INTRO_STARTING")
    if "$app/%s" % INTRO_VIDEO not in script:
        problems.append("ksphmi-intro.sh default video path")
    if "--keep-open=always" not in script or "--input-ipc-server=\"$sock\"" not in script:
        problems.append("mpv must hold the last frame and serve the IPC socket")
    if not re.search(r"\btar\b[^|\n]*?-cf - [^|]*\bassets\b", read(os.path.join(ROOT, "ci"))):
        problems.append("./ci deploy_hmi does not copy assets/")
    report("FAIL" if problems else "PASS", "hmi: boot intro",
           ", ".join(problems) if problems else "video asset, mpv install, unit, socket /run/%s/%s agree" %
           (hdr_dir.group(1), hdr_sock.group(1)))


# Boot command line flags setup-kiosk.sh adds so that nothing but the splash and the HMI reaches the screen.
KIOSK_CMDLINE = ("quiet", "logo.nologo", "vt.global_cursor_default=0", "consoleblank=0", "systemd.show_status=false")


def kiosk_problems(env):
    """What the Pi's kiosk boot lacks, from check_hmi's probe values (kcmd = cmdline.txt words joined by '|')."""
    args = [a for a in env.get("kcmd", "").split("|") if a]
    missing = []
    if not args:
        return ["cmdline.txt unreadable"]
    # console=ttynull hung the Pi 4 early in boot (black screen, no network, no journal; 2026-11-15)
    if "console=ttynull" in args:
        missing.append("console=ttynull removed (it hangs the boot)")
    missing += [flag for flag in KIOSK_CMDLINE if flag not in args]
    if env.get("journal") != "yes":
        missing.append("persistent journal")
    if env.get("kioskwant") and env.get("kioskwant") != env.get("kioskhave"):
        missing.append("setup-kiosk.sh applied in its deployed version")
    if env.get("nosplash") != "yes":
        missing.append("disable_splash=1")
    if env.get("tty1") != "masked":
        missing.append("getty@tty1 masked (is %s)" % (env.get("tty1") or "?"))
    if env.get("splashunit") != "enabled" or env.get("fbrule") != "yes":
        missing.append("ksphmi-splash.service enabled (is %s)" % (env.get("splashunit") or "absent"))
    if env.get("mpv") == "no":
        missing.append("mpv installed (boot intro video; the fallback loading screen shows instead)")
    return missing


def hmi_screen_status(env, state):
    """(status, detail) for 'hmi: screen' from the Pi probe, or None when there is nothing to judge.

    The output comes from the HMI's latest 'screen WxH (<kind>)' journal line (spaces sent as '_'). fbdev output:
    a blanked fb0 (4 = powered down) hides every frame while the service and its log look healthy. SDL output
    (KMSDRM) leaves fb0/blank at 4 by design, so the screen is judged by the HDMI connectors' status and dpms."""
    blank = env.get("fbblank", "")
    line = env.get("screenline", "").replace("_", " ")
    connectors = [c.split(":") for c in env.get("drm", "").split(",") if c.count(":") == 2]
    m = re.search(r"screen (\d+x\d+) \((SDL [^ )]+|fbdev)", line)
    if m and m.group(2) == "fbdev":
        if blank and blank != "0":
            return ("FAIL" if state == "active" else "WARN",
                    "fbdev output, framebuffer blanked (fb0/blank=%s): nothing shows; the HMI unblanks it, "
                    "redeploy or restart it" % blank)
        if blank == "0":
            return ("PASS", "fbdev output, framebuffer on")
        return ("WARN", "fbdev output but fb0/blank unreadable")
    if m:
        size, driver = m.group(1), m.group(2)
        connected = [c for c in connectors if c[1] == "connected"]
        lit = [c for c in connected if c[2] == "On"]
        bad = "FAIL" if state == "active" else "WARN"
        if lit:
            return ("PASS", "%s %s on %s (connected, dpms On)" % (driver, size, ", ".join(c[0] for c in lit)))
        if connected:
            return (bad, "%s output but HDMI dpms is %s on %s: the display is powered down"
                    % (driver, "/".join(c[2] for c in connected), ", ".join(c[0] for c in connected)))
        return (bad, "%s output but no HDMI connector is connected (%s): check the cable, or the monitor was "
                "off at boot" % (driver, env.get("drm") or "no connectors found"))
    if not (line or blank or connectors):
        return None
    return ("WARN", "unknown HMI output (screen line %r, fb0/blank=%s, drm=%s): ./ci hmi_logs"
            % (line or "none", blank or "?", env.get("drm") or "?"))


def check_hmi(ctx):
    """Local C++ test build is current, then (if the Pi answers over SSH) its service state and the kRPC address."""
    # Only check that ./ci test_hmi ran after the last source change: the native Windows build (MSYS2,
    # build/hmi-cpp-win) or the Linux/WSL one (build/hmi-cpp), whichever is newer.
    builds = [[os.path.join(ROOT, "build", "hmi-cpp-win", n + ".exe") for n in ("ksphmi_tests", "ksphmi_render_tests")],
              [os.path.join(ROOT, "build", "hmi-cpp", n) for n in ("ksphmi_tests", "ksphmi_render_tests")]]
    built = [b for b in builds if all(os.path.isfile(t) for t in b)]

    def tested(b):  # ctest's log when it ran (binaries a change did not relink stay old), else the binaries
        last = os.path.join(os.path.dirname(b[0]), "Testing", "Temporary", "LastTest.log")
        stamp = min(os.path.getmtime(t) for t in b)
        return max(stamp, os.path.getmtime(last)) if os.path.isfile(last) else stamp

    sources = [os.path.join(d, f) for d, _, files in os.walk(HMI_DIR) for f in files]
    newest = max((os.path.getmtime(f) for f in sources), default=0)
    if not built:
        report("WARN", "hmi: C++ tests", "never built here: ./ci test_hmi")
    elif max(tested(b) for b in built) < newest:
        report("WARN", "hmi: C++ tests", "hmi-cpp changed since the last build: ./ci test_hmi")
    else:
        report("PASS", "hmi: C++ tests", "test binaries newer than hmi-cpp/ (./ci test_hmi)")

    host = os.environ.get("HMI_HOST", "ksphmi")
    probe = ("systemctl --user is-active ksphmi.service; systemctl --user show ksphmi.service -p NRestarts --value; "
             "cat ~/ksphmi/env 2>/dev/null; echo \"client=${SSH_CLIENT%% *}\"; "
             "echo \"fbblank=$(cat /sys/class/graphics/fb0/blank 2>/dev/null)\"; "
             # The output the running HMI chose (its 'screen WxH (<kind>)' line) and every HDMI connector's state
             "echo \"screenline=$(journalctl --user -u ksphmi.service -b --no-pager -o cat 2>/dev/null "
             "| grep -E 'screen [0-9]+x[0-9]+ \\(' | tail -n 1 | tr -s ' \\t' '__')\"; "
             "echo \"drm=$(for c in /sys/class/drm/card*-HDMI-*; do test -r $c/status && printf '%s:%s:%s,' "
             "${c##*/} $(cat $c/status) $(cat $c/dpms 2>/dev/null || echo unknown); done)\"; "
             "echo \"cursorblink=$(cat /sys/class/graphics/fbcon/cursor_blink 2>/dev/null)\"; "
             "echo \"cursorrule=$(test -f /etc/tmpfiles.d/ksphmi-cursor.conf && echo yes || echo no)\"; "
             "echo \"linger=$(loginctl show-user $(id -un) -p Linger --value)\"; "
             "echo \"autostart=$(systemctl --user is-enabled ksphmi.service)\"; "
             "echo \"execstart=$(systemctl --user show ksphmi.service -p ExecStart --value | tr -d ' ')\"; "
             "echo \"oldunit=$(test -e ~/.config/systemd/user/ksphmi-cpp.service && echo yes || echo no)\"; "
             "echo \"tty1=$(systemctl is-enabled getty@tty1.service)\"; "
             "bootfile=/boot/firmware/cmdline.txt; test -r $bootfile || bootfile=/boot/cmdline.txt; "
             "echo \"kcmd=$(tr -s ' \\n' '||' < $bootfile 2>/dev/null)\"; "
             "echo \"nosplash=$(grep -qsx '[[:space:]]*disable_splash=1[[:space:]]*' $(dirname $bootfile)/config.txt "
             "&& echo yes || echo no)\"; "
             "echo \"splashunit=$(systemctl is-enabled ksphmi-splash.service 2>/dev/null || true)\"; "
             "echo \"fbrule=$(test -f /etc/udev/rules.d/99-ksphmi-fb.rules && echo yes || echo no)\"; "
             "echo \"mpv=$(command -v mpv >/dev/null && echo yes || echo no)\"; "
             "echo \"journal=$(test -f /etc/systemd/journald.conf.d/50-ksphmi-persistent.conf && echo yes || echo no)\"; "
             "echo \"kioskwant=$(sha256sum ~/ksphmi/cpp/systemd/setup-kiosk.sh 2>/dev/null | cut -d' ' -f1)\"; "
             "echo \"kioskhave=$(cat /etc/ksphmi-kiosk.applied 2>/dev/null)\"")
    try:
        r = subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=3", host, probe],
                           capture_output=True, text=True, timeout=15)
    except (OSError, subprocess.TimeoutExpired) as e:
        report("WARN", "hmi: Pi", "skipped: ssh %s failed (%s)" % (host, e))
        return
    lines = r.stdout.split()
    if r.returncode not in (0, 3) or len(lines) < 2:  # is-active exits 3 when the unit is not running
        report("WARN", "hmi: Pi", "skipped: %s not reachable over SSH (%s)" % (host, (r.stderr.strip() or "no answer")[:120]))
        return
    state, restarts = lines[0], lines[1]
    if state == "active" and restarts in ("0", ""):
        report("PASS", "hmi: service", "active on %s" % host)
    elif state == "active":
        report("WARN", "hmi: service", "active but restarted %s time(s): ./ci hmi_logs" % restarts)
    else:
        report("FAIL" if state == "failed" else "WARN", "hmi: service", "%s on %s (./ci deploy_hmi, ./ci hmi_logs)" % (state, host))
    env = dict(l.split("=", 1) for l in lines[2:] if "=" in l)
    addr, client = env.get("KSPHMI_KRPC_ADDRESS"), env.get("client")
    if addr and client and addr != client and not os.environ.get("HMI_KRPC_ADDRESS"):
        report("WARN", "hmi: kRPC address", "seed %s differs from this PC %s; HMI discovers KSP via UDP 50002" % (addr, client))
    elif addr:
        report("PASS", "hmi: kRPC address", addr)
    screen = hmi_screen_status(env, state)
    if screen:
        report(screen[0], "hmi: screen", screen[1])
    # The text console's cursor blinks over the frames unless fbcon's blinking is off (root only, docs/hmi.rst).
    # The live value alone is not enough: it reads 0 while fbcon's cursor timer happens to be stopped, and the
    # blinking comes back after a reboot or console output; only the tmpfiles rule makes it stick.
    if env.get("cursorrule") == "no":
        report("WARN", "hmi: console cursor", "no /etc/tmpfiles.d/ksphmi-cursor.conf: the cursor can blink over the "
               "HMI (now: blink=%s; docs/hmi.rst)" % env.get("cursorblink", "?"))
    elif env.get("cursorrule") == "yes":
        report("PASS", "hmi: console cursor", "blinking off at every boot (blink=%s)" % env.get("cursorblink", "?"))
    autostart = env.get("linger") == "yes" and env.get("autostart") == "enabled"
    report("PASS" if autostart else "WARN", "hmi: boot autostart",
           "service enabled with lingering" if autostart else "enable service and loginctl lingering")
    missing = kiosk_problems(env)
    report("WARN" if missing else "PASS", "hmi: kiosk boot",
           "missing: %s; ./ci deploy_hmi applies it (or sudo sh ~/ksphmi/cpp/systemd/setup-kiosk.sh on the Pi), "
           "then reboot" % ", ".join(missing)
           if missing else "intro unit and mpv, quiet console, no firmware splash, login prompt masked")
    cpp = "/ksphmi/cpp/build/ksphmi" in env.get("execstart", "") and env.get("oldunit") == "no"
    report("PASS" if cpp else "WARN", "hmi: C++ unit",
           "ksphmi.service runs ~/ksphmi/cpp/build/ksphmi" if cpp else
           "ksphmi.service is not the C++ unit or ksphmi-cpp.service is left over: ./ci deploy_hmi")


FIREWALL_RULE = "KSP kRPC (wk-rea)"
KRPC_TEMPLATE = os.path.join(ROOT, "tools", "krpc", "settings.cfg")
# UI state kRPC rewrites on its own: not compared against the template.
KRPC_UI_KEYS = ("mainwindowvisible", "infowindowvisible", "id")
KRPC_UI_NODES = ("mainwindowposition", "infowindowposition")


def krpc_values(cfg):
    """Flatten a kRPC settings.cfg (ConfigNode) into {lower-case key: value}: top-level fields, server fields and
    the server's key/value Items (address, rpc_port, stream_port). Window position nodes and UI flags are skipped."""
    vals, stack, last, pending = {}, [], "", None
    for line in cfg.splitlines():
        t = line.strip()
        if t == "{":
            stack.append(last.lower())
        elif t == "}":
            if stack:
                stack.pop()
        elif "=" in t:
            if any(n in KRPC_UI_NODES for n in stack):
                continue
            k, v = (x.strip() for x in t.split("=", 1))
            k = k.lower()
            if k == "key":
                pending = v
            elif k == "value" and pending:
                vals[pending.lower()] = v
                pending = None
            elif k not in KRPC_UI_KEYS:
                vals[k] = v
        elif t:
            last = t
    return vals


def check_krpc(ctx):
    """The kRPC mod the Pi HMI needs: installed, same version as the Pi's client, server set to start by itself,
    accept clients and listen beyond localhost, and an inbound firewall rule (the wired LAN is a Public network)."""
    if ksp_missing(ctx, "kRPC: installed"):
        return
    mod = os.path.join(ctx["ksp"], "GameData", "kRPC")
    if not os.path.isdir(mod):
        report("FAIL", "kRPC: installed", "no GameData/kRPC: the Pi HMI gets no game data (docs/hmi.rst)")
        return
    # The HMI's own client speaks the same protocol as this krpc version (hmi-cpp/include/krpc.hpp); kspsim uses it too.
    want = re.search(r"^krpc==([\d.]+)", read(os.path.join(ROOT, "tools", "requirements.txt")), re.M)
    try:
        v = json.loads(read(os.path.join(mod, "kRPC.version")))["VERSION"]
        have = "%d.%d.%d" % (v["MAJOR"], v["MINOR"], v["PATCH"])
    except (OSError, ValueError, KeyError):
        have = "?"
    if want and have != want.group(1):
        report("FAIL", "kRPC: version", "mod %s but the HMI and kspsim speak krpc==%s (protocol mismatch)" % (have, want.group(1)))
    else:
        report("PASS", "kRPC: version", have)
    cfg_path = os.path.join(mod, "PluginData", "settings.cfg")
    cfg = read(cfg_path) if os.path.isfile(cfg_path) else ""
    problems = []
    if not re.search(r"autoStartServers\s*=\s*True", cfg, re.I):
        problems.append("server does not auto-start")
    if not re.search(r"autoAcceptConnections\s*=\s*True", cfg, re.I):
        problems.append("clients not auto-accepted")
    addr = re.search(r"key\s*=\s*address\s*value\s*=\s*(\S+)", cfg)
    if not addr or addr.group(1) in ("127.0.0.1", "localhost"):
        problems.append("listens on %s only: the Pi cannot reach it" % (addr.group(1) if addr else "127.0.0.1 (default)"))
    report("WARN" if problems else "PASS", "kRPC: server settings",
           "; ".join(problems) if problems else "auto-start, auto-accept, address %s" % addr.group(1))
    # kRPC's ConfigurationFile.AfterLoad copies every persistent field, so a key missing from a hand-written file
    # becomes 0/False, not the default: maxTimePerUpdate = 0 accepts clients but never answers an RPC.
    vals = krpc_values(cfg)
    dead = ["%s %s" % (k, "missing" if k not in vals else "= 0")
            for k in ("maxtimeperupdate", "recvtimeout") if vals.get(k, "0") in ("0", "")]
    if dead:
        report("FAIL", "kRPC: update budget", "%s: the server accepts the Pi but never answers any RPC "
               "(install tools/krpc/settings.cfg, docs/hmi.rst)" % ", ".join(dead))
    else:
        report("PASS", "kRPC: update budget", "maxTimePerUpdate %s, recvTimeout %s"
               % (vals["maxtimeperupdate"], vals["recvtimeout"]))
    off = [k for k in ("adaptiveratecontrol", "blockingrecv") if vals.get(k, "False").lower() != "true"]
    if off:
        report("WARN", "kRPC: rate control", "%s False (kRPC default True): slower or stalled RPCs" % ", ".join(off))
    else:
        report("PASS", "kRPC: rate control", "adaptiveRateControl, blockingRecv True")
    want_vals = krpc_values(read(KRPC_TEMPLATE)) if os.path.isfile(KRPC_TEMPLATE) else {}
    diff = ["%s=%s (want %s)" % (k, vals.get(k, "missing"), v) for k, v in sorted(want_vals.items())
            if vals.get(k, "").lower() != v.lower()]
    diff += ["%s=%s (not in template)" % (k, vals[k]) for k in sorted(vals) if k not in want_vals]
    if not want_vals:
        report("WARN", "kRPC: matches template", "tools/krpc/settings.cfg missing")
    elif diff:
        report("WARN", "kRPC: matches template", "installed settings.cfg differs from tools/krpc/settings.cfg: "
               + ", ".join(diff))
    else:
        report("PASS", "kRPC: matches template", "%d keys equal to tools/krpc/settings.cfg" % len(want_vals))
    out = windows_firewall_rule(FIREWALL_RULE)
    if out is None:
        report("PASS", "kRPC: firewall", "not Windows: no netsh rule (allow KSP in this OS's firewall)")
    elif "50000" in out:
        report("PASS", "kRPC: firewall", "rule '%s' present" % FIREWALL_RULE)
    else:
        report("WARN", "kRPC: firewall", "no inbound rule '%s' for TCP 50000-50001: the Pi may be blocked "
               "(docs/hmi.rst, needs an administrator)" % FIREWALL_RULE)


def check_sim(ctx):
    """kspsim tests: wire packing against the firmware sizes, the flight model, and the krpc client against the
    simulated server (catches a krpc client upgrade that the stand-in no longer serves). The C++ HMI against the
    simulator is ./ci test_hmi --sim (needs WSL)."""
    p = subprocess.run([sys.executable, "-m", "unittest", os.path.join("tools", "kspsim", "test_kspsim.py")],
                       cwd=ROOT, capture_output=True, text=True, timeout=300,
                       env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"))
    tail = [l for l in p.stderr.strip().splitlines() if l.startswith(("Ran ", "OK", "FAILED"))]
    report("PASS" if p.returncode == 0 else "FAIL", "sim: tests", " ".join(tail) if p.returncode == 0 else p.stderr.strip()[-300:])


def check_vessel_view_contract(ctx):
    """The VESSEL panel and HMI keyboard in flight (kRPC service KSPHMIFlight, docs/hmi.rst): plugin, Pi HMI and kspsim
    agree on the service, its Flight scene and procedures, and all three use the shared format-v1 fixtures
    (hmi-cpp/tests/data/vessel_shape_v1.txt and vessel_state_v1.txt); only KSPSerialIO.Hmi.dll touches kRPC."""
    service = read(os.path.join(HMI_PLUGIN_DIR, "HmiFlightService.cs"))
    fmt = read(os.path.join(HMI_PLUGIN_DIR, "VesselViewFormat.cs"))
    addon = read(os.path.join(HMI_PLUGIN_DIR, "VesselViewReport.cs"))
    project = read(os.path.join(HMI_PLUGIN_DIR, "KSPSerialIO.Hmi.csproj"))
    telemetry = read(os.path.join(HMI_DIR, "src", "telemetry.cpp"))
    hmi_tests = read(os.path.join(HMI_DIR, "tests", "test_core.cpp"))
    server = read(os.path.join(ROOT, "tools", "kspsim", "krpcserver.py"))
    data = os.path.join(HMI_DIR, "tests", "data")
    procedures = ("SetAxes", "SetThrottle", "NudgeThrottle", "Throttle", "VesselShape", "VesselState")
    # MechJeb scene (docs/hmi.rst): plugin side first; the HMI and kspsim join this list when they call them.
    mechjeb = {"MechJebRevision": "uint", "MechJebReport": "string", "MechJebSet": "string", "MechJebAction": "string"}
    bridge = read(os.path.join(HMI_PLUGIN_DIR, "MechJebBridge.cs"))
    members = read(os.path.join(HMI_PLUGIN_DIR, "MechJebMembers.cs"))
    problems = []
    for what, ok in (
            ("service KSPHMIFlight in the Flight scene",
             re.search(r'KRPCService\(Name = "KSPHMIFlight", GameScene = GameScene\.Flight\)', service)),
            ("plugin procedures " + "/".join(procedures), all("public static %s %s(" % ("string" if n.startswith("Vessel") else "float" if n == "Throttle" else "void", n) in service for n in procedures)),
            ("plugin MechJeb procedures " + "/".join(mechjeb),
             all("public static %s %s(" % (t, n) in service for n, t in mechjeb.items())),
            ("MechJeb stays optional (no MechJeb2 reference, found by reflection)",
             "MechJeb2" not in project and '"MuMech.MechJebCore"' in bridge and "KRPC" not in bridge and "KRPC" not in members),
            ("HMI calls every KSPHMIFlight procedure", '"KSPHMIFlight"' in telemetry and all('"%s"' % n in telemetry for n in procedures)),
            ("kspsim serves every KSPHMIFlight procedure", 'name="KSPHMIFlight"' in server and all('"%s"' % n in server for n in procedures)),
            ("shared fixtures exist", all(os.path.isfile(os.path.join(data, f)) for f in ("vessel_shape_v1.txt", "vessel_state_v1.txt"))),
            ("HMI tests and kspsim read the fixtures", "vessel_shape_v1.txt" in hmi_tests and "vessel_shape_v1.txt" in server),
            ("only HmiFlightService.cs references kRPC", "KRPC" not in addon and "KRPC" not in fmt),
            ("project builds every VESSEL source",
             all('Compile Include="%s"' % f in project for f in ("HmiFlightService.cs", "VesselViewFormat.cs", "VesselViewReport.cs",
                                                                  "MechJebMembers.cs", "MechJebBridge.cs")))):
        if not ok:
            problems.append(what)
    if problems:
        report("FAIL", "vessel view: contract", "; ".join(problems))
    else:
        report("PASS", "vessel view: contract", "KSPHMIFlight (Flight) keys and VesselShape/VesselState, format v1 in plugin, HMI and kspsim; plugin MechJeb procedures")


def check_ship_report_contract(ctx):
    """The VAB ship report (kRPC service KSPHMI, docs/hmi.rst): plugin, Pi HMI and kspsim agree on the service, its
    procedures, the Editor scene and text format version 1; KSPSerialIO.dll itself never depends on kRPC; and the
    plugin's formatter writes exactly hmi-cpp/tests/data/ship_report_v1.txt (the Pi parser's and kspsim's fixture)."""
    service = read(os.path.join(HMI_PLUGIN_DIR, "HmiService.cs"))
    fmt = read(os.path.join(HMI_PLUGIN_DIR, "ShipReportFormat.cs"))
    addon = read(os.path.join(HMI_PLUGIN_DIR, "EditorShipReport.cs"))
    project = read(os.path.join(HMI_PLUGIN_DIR, "KSPSerialIO.Hmi.csproj"))
    hpp = read(os.path.join(HMI_DIR, "include", "shipreport.hpp"))
    telemetry = read(os.path.join(HMI_DIR, "src", "telemetry.cpp"))
    server = read(os.path.join(ROOT, "tools", "kspsim", "krpcserver.py"))
    ships = read(os.path.join(ROOT, "tools", "kspsim", "ships.py"))
    problems = []
    for what, ok in (
            ("service KSPHMI in the Editor scene", re.search(r'KRPCService\(Name = "KSPHMI", GameScene = GameScene\.Editor\)', service)),
            ("Revision() -> uint", "public static uint Revision()" in service),
            ("ShipReport() -> string", "public static string ShipReport()" in service),
            ("plugin format version 1", "public const int Version = 1;" in fmt),
            ("HMI format version 1", "SHIP_REPORT_VERSION = 1;" in hpp),
            ("kspsim format version 1", re.search(r"^VERSION = 1$", ships, re.M)),
            ("HMI calls KSPHMI.Revision / ShipReport", all('"%s"' % n in telemetry for n in ("KSPHMI", "Revision", "ShipReport"))),
            ("kspsim serves KSPHMI", 'name="KSPHMI"' in server and '"Revision"' in server and '"ShipReport"' in server),
            ("only HmiService.cs references kRPC", "KRPC" not in addon and "KRPC" not in fmt),
            ("project builds all sources against KRPC.Core",
             all('Compile Include="%s"' % f in project for f in ("ShipReportFormat.cs", "EditorShipReport.cs", "HmiService.cs"))
             and 'Include="KRPC.Core"' in project)):
        if not ok:
            problems.append(what)
    plugin_project = read(os.path.join(ROOT, "KSPSerialIO", "KSPSerialIO", "KSPSerialIO.csproj"))
    plugin_sources = "".join(read(os.path.join(os.path.dirname(CS), f)) for f in ("KSPIO.cs", "ButtonEvents.cs", "HmiContext.cs"))
    if "KRPC" in plugin_project or "KRPC" in plugin_sources:
        problems.append("KSPSerialIO.dll must not reference kRPC (only KSPSerialIO.Hmi.dll does)")
    if os.path.isfile(BUILT_DLL):
        with open(BUILT_DLL, "rb") as f:
            if b"KRPC" in f.read():
                problems.append("built KSPSerialIO.dll references kRPC")
    if problems:
        report("FAIL", "ship report: contract", "; ".join(problems))
    else:
        report("PASS", "ship report: contract", "KSPHMI Revision/ShipReport (Editor), format v1 in plugin, HMI and kspsim")
    dotnet = shutil.which("dotnet")
    if not dotnet:
        report("WARN", "ship report: plugin format", "requires the .NET 8 SDK (tools/plugin-shipreport-tests)")
        return
    project_name = "PluginShipReport.Tests"
    artifacts = os.path.join(ROOT, "build", "plugin-shipreport-tests")
    result = subprocess.run([dotnet, "build", os.path.join(ROOT, "tools", "plugin-shipreport-tests", project_name + ".csproj"),
                             "--configuration", "Release", "--artifacts-path", artifacts, "--nologo", "-p:NuGetAudit=false",
                             "--ignore-failed-sources"], capture_output=True, text=True, timeout=120)
    if result.returncode == 0:
        dll = os.path.join(artifacts, "bin", project_name, "release", project_name + ".dll")
        result = subprocess.run([dotnet, dll, ROOT], capture_output=True, text=True, timeout=30)
    report("PASS" if result.returncode == 0 else "FAIL", "ship report: plugin format", (result.stdout + result.stderr).strip()[-600:])


def check_editor_modes(ctx):
    """Editor screens preserve the shared wire extension and isolate serial controls from flight."""
    import runpy
    fixture = read(os.path.join(ROOT, "hmi-cpp", "tests", "data", "editor_report_v1.txt"))
    ships = runpy.run_path(os.path.join(ROOT, "tools", "kspsim", "ships.py"))
    expected = ships["report_text"](57, ships["editor_sample"](ships["ship"]("kerbal_x"), "actions"))
    errors = []
    if expected != fixture:
        errors.append("simulator editor fixture differs")
    sources = {
        "KSPSerialIO/KSPSerialIO/EditorSerialLink.cs": ("Startup.EditorAny", "EditorNavigation.Consume", "TrySendPacket(VData)", "PortCleanup()"),
        "KSPSerialIO/KSPSerialIO.Hmi/EditorDetails.cs": ("SelectPanelParts", "SelectPanelActions", "SelectPanelCrew", "SelectPanelCargo", "GetCrewSeat"),
        "KSPSerialIO/KSPSerialIO.Hmi/EditorShipReport.cs": ("onEditorScreenChange.Add", "onEditorScreenChange.Remove", "EditorNavigation.StepRequested +=", "EditorNavigation.StepRequested -="),
        "hmi-cpp/src/apollo.cpp": ("editor(c, art, s)", "stage_detail_lines"),
        "hmi-cpp/src/display.cpp": ("editor_report(snap)", "stage_detail_lines"),
    }
    for path, tokens in sources.items():
        source = read(os.path.join(ROOT, path))
        for token in tokens:
            if token not in source:
                errors.append(path + " missing " + token)
    for kind in ("editor=", "ac=", "seat=", "inv=", "cargo=", "sp="):
        if not any(line.startswith(kind) for line in fixture.splitlines()):
            errors.append("editor fixture missing " + kind)
    report("FAIL" if errors else "PASS", "editor modes: contract", "; ".join(errors) if errors else
           "four modes, real skins, named staging, shared fixture and editor-only button routing")

    smoke = os.path.join(ROOT, "build", "editor-hmi-sim", "smoke.json")
    if os.path.isfile(smoke):
        with open(smoke, encoding="utf-8") as stream:
            result = json.load(stream)
        expected = {skin + ":" + mode for skin in ("classic", "apollo") for mode in ("construction", "actions", "crew", "cargo")}
        ok = result.get("passed") is True and set(result.get("screens", [])) == expected
        sources = [os.path.join(ROOT, folder, file) for folder, _, files in os.walk(os.path.join(ROOT, "hmi-cpp"))
                   for file in files if file.endswith((".cpp", ".hpp"))]
        sources += [os.path.join(ROOT, "tools", "test_editor_hmi.py")]
        stale = any(os.path.getmtime(source) > os.path.getmtime(smoke) for source in sources)
        report("FAIL" if not ok else "WARN" if stale else "PASS", "editor modes: end-to-end",
               "C++ HMI followed all four modes in both real skins" + ("; sources changed: rerun test_hmi --sim" if stale else ""))
    else:
        report("WARN", "editor modes: end-to-end", "not tested: ./ci test_hmi --sim")


def check_install_hmi_service(ctx):
    """KSPSerialIO.Hmi.dll (the Pi's VAB report) installed in GameData/KSPSerialIO/Plugins and identical to the build,
    with its KSPSerialIO.Hmi.xml (kRPC reads the service documentation from it and warns at every launch without it).
    It needs kRPC in the game; without it only that DLL fails to load and the Pi keeps its static VAB cards."""
    gd = os.path.join(ctx["ksp"], "GameData")
    installed = os.path.join(gd, "KSPSerialIO", "Plugins", "KSPSerialIO.Hmi.dll")
    krpc = os.path.isfile(os.path.join(gd, "kRPC", "KRPC.Core.dll"))
    built = os.path.isfile(HMI_BUILT_DLL)
    if os.path.isfile(os.path.join(gd, "KSPSerialIO", "KSPSerialIO.Hmi.dll")):
        report("WARN", "ship report: installed", "stray GameData/KSPSerialIO/KSPSerialIO.Hmi.dll (it belongs in Plugins/): KSP loads both")
    if not os.path.isfile(installed):
        if built:
            report("FAIL", "ship report: installed", "built but not installed: ./ci deploy (KSP closed, peer informed)")
        elif krpc:
            report("WARN", "ship report: installed", "not built: ./ci build_KSPSerialIO, then ./ci deploy")
        else:
            report("WARN", "ship report: installed", "kRPC not installed: no VAB ship report on the Pi")
        return
    if not krpc:
        report("WARN", "ship report: installed", "installed, but kRPC is missing: the DLL cannot load")
    elif not built:
        report("WARN", "ship report: installed", "installed; no build output to compare (./ci build_KSPSerialIO)")
    elif sha(installed) != sha(HMI_BUILT_DLL):
        report("FAIL", "ship report: installed", "installed KSPSerialIO.Hmi.dll differs from the build (./ci deploy, KSP closed)")
    elif os.path.isfile(HMI_BUILT_XML) and (not os.path.isfile(installed[:-4] + ".xml") or
                                             sha(installed[:-4] + ".xml") != sha(HMI_BUILT_XML)):
        report("FAIL", "ship report: installed", "Plugins/KSPSerialIO.Hmi.xml missing or differs from the build "
                                                 "(kRPC warns 'No documentation found'; ./ci deploy, KSP closed)")
    else:
        report("PASS", "ship report: installed", "Plugins/KSPSerialIO.Hmi.dll and .xml identical to the build, kRPC present")


# --------------------------------------------------------------------------------------------- hardware
def check_skill_mirrors(ctx):
    """Codex and Claude must receive the same repository skill instructions on LF and CRLF checkouts."""
    def skill_paths(folder):
        return {os.path.relpath(os.path.join(base, name), folder)
                for base, _, names in os.walk(folder) for name in names if name == "SKILL.md"}
    canonical = os.path.join(ROOT, ".claude", "skills")
    mirror = os.path.join(ROOT, ".agents", "skills")
    expected, actual = skill_paths(canonical), skill_paths(mirror)
    problems = []
    if not expected:
        problems.append("canonical repository skills missing")
    for path in sorted(expected ^ actual):
        problems.append("missing counterpart: " + path)
    for path in sorted(expected & actual):
        if read(os.path.join(canonical, path)) != read(os.path.join(mirror, path)):
            problems.append("different instructions: " + path)
    report("FAIL" if problems else "PASS", "skills: Codex mirror",
           "; ".join(problems) if problems else "%d SKILL.md files agree" % len(expected))


def check_documentation(ctx):
    """Keep narrative guides, editable UML and generated API coverage aligned with all three projects."""
    docs = os.path.join(ROOT, "docs")
    required = ("index.rst", "architecture.rst", "communication.rst", "protocol.rst", "vesseldata.rst",
                "development/arduino.rst", "development/plugin.rst", "development/raspberry-pi.rst",
                "testing.rst", "documentation.rst")
    missing = [p for p in required if not os.path.isfile(os.path.join(docs, p))]
    referenced = set()
    errors = []
    for folder, _, files in os.walk(docs):
        for name in files:
            if not name.endswith(".rst"):
                continue
            for target in re.findall(r"^\.\. uml::\s+(\S+)\s*$", read(os.path.join(folder, name)), re.M):
                path = os.path.normpath(os.path.join(folder, target))
                referenced.add(path)
                if not os.path.isfile(path):
                    errors.append("missing UML " + target)
    diagrams = os.path.join(docs, "diagrams")
    count = 0
    for name in os.listdir(diagrams) if os.path.isdir(diagrams) else ():
        if not name.endswith(".puml"):
            continue
        count += 1
        path = os.path.normpath(os.path.join(diagrams, name))
        source = read(path)
        if source.count("@startuml") != 1 or source.count("@enduml") != 1:
            errors.append("invalid UML delimiters " + name)
        if path not in referenced:
            errors.append("unreferenced UML " + name)
        if name.startswith("seq_"):
            participants = set(re.findall(r'^participant .* as (\w+)\s*$', source, re.M))
            balance = dict.fromkeys(participants, 0)
            activated = set()
            for operation, participant in re.findall(r'^\s*(activate|deactivate) (\w+)\s*$', source, re.M):
                if participant not in balance:
                    errors.append("undeclared activation " + participant + " in " + name)
                    continue
                if operation == "activate":
                    activated.add(participant)
                    balance[participant] += 1
                else:
                    balance[participant] -= 1
                    if balance[participant] < 0:
                        errors.append("activation ends before start in " + name)
            if not participants or participants != activated or any(balance.values()):
                errors.append("missing/unbalanced lifeline activations in " + name)
    doxygen = read(os.path.join(ROOT, "Doxyfile"))
    inputs = re.search(r"^INPUT\s*=([^\r\n]*)", doxygen, re.M)
    api = inputs.group(1).split() if inputs else []
    for path in ("KSPSerialIO/KSPSerialIO/KSPIO.cs", "KSPSerialIO/KSPSerialIO/HmiContext.cs", "KSPSerialIO/KSPSerialIO.Hmi",
                 "arduino-sketch/KSPIODemo9", "hmi-cpp/include", "hmi-cpp/src"):
        if path not in api:
            errors.append("Doxygen omits " + path)
    patterns = re.search(r"^FILE_PATTERNS\s*=([^\r\n]*)", doxygen, re.M)
    if not patterns or not {"*.cs", "*.ino", "*.hpp", "*.cpp"}.issubset(patterns.group(1).split()):
        errors.append("Doxygen file patterns omit a project language")
    readme = read(os.path.join(ROOT, "README.md"))
    for target in re.findall(r"!?\[[^\]]*\]\(([^)\s]+)\)", readme):
        if "://" not in target and not target.startswith("#"):
            relative = target.split("#", 1)[0]
            if not os.path.exists(os.path.join(ROOT, relative)):
                errors.append("missing README link target " + target)
    for target in re.findall(r"!\[[^\]]*\]\(([^)\s]+)\)", readme):
        if "://" in target:
            continue
        path = os.path.join(ROOT, target)
        if not os.path.isfile(path):
            errors.append("missing README screenshot " + target)
        elif target.lower().endswith(".png"):
            with open(path, "rb") as stream:
                header = stream.read(24)
            if len(header) != 24 or header[:8] != b"\x89PNG\r\n\x1a\n" or not all(int.from_bytes(header[i:i + 4], "big") > 0 for i in (16, 20)):
                errors.append("invalid README PNG " + target)
    errors.extend("missing guide " + p for p in missing)
    report("FAIL" if errors else "PASS", "docs: architecture, UML and API coverage",
           "; ".join(errors) if errors else "%d required guides, %d referenced PUML sources, three-project API inputs" % (len(required), count))


def check_hmi_desktop(ctx):
    """Record the interactive simulator GUI -> real C++ HMI smoke check, without needing Docker here."""
    base = os.path.join(ROOT, "build", "hmi-container")
    records = []
    if os.path.isdir(base):
        for platform in os.listdir(base):
            path = os.path.join(base, platform, "desktop", "smoke.json")
            if os.path.isfile(path):
                records.append((platform, path))
    if not records:
        report("WARN", "hmi: desktop communication", "not tested: ./ci test_hmi_desktop")
        return
    sources = [os.path.join(ROOT, "ci")]
    for folder in ("hmi-cpp", "tools/kspsim", "tools/hmi-container"):
        for root, dirs, files in os.walk(os.path.join(ROOT, folder)):
            dirs[:] = [d for d in dirs if d != "__pycache__"]
            sources.extend(os.path.join(root, f) for f in files if not f.endswith(".pyc"))
    newest = max(os.path.getmtime(p) for p in sources)
    for platform, path in records:
        with open(path, encoding="utf-8") as stream:
            data = json.load(stream)
        ok = data.get("passed") is True and data.get("skin") == "classic" and abs(data.get("throttle", 0) - .37) < .001 and data.get("browser") is True
        stale = os.path.getmtime(path) < newest
        report("FAIL" if not ok else "WARN" if stale else "PASS", "hmi: desktop communication " + platform,
               "GUI skin -> C++ display (UDP), GUI throttle -> kRPC" + ("; sources changed: rerun ./ci test_hmi_desktop" if stale else ""))


def check_board(ctx):
    if ksp_running():
        report("WARN", "board", "skipped: KSP is running and owns the serial port")
        return
    port = ctx["args"].port
    cmd = [sys.executable, os.path.join(ROOT, "tools", "test_arduino.py")] + ([port] if port else [])
    p = subprocess.run(cmd, capture_output=True, text=True)
    out = p.stdout
    for name in ("handshake", "telemetry", "debug", "reopen", "truncated reopen", "idle timeout"):
        m = re.search(r"\[(PASS|FAIL)\] %s: (.*)" % name, out)
        report(m.group(1) if m else "FAIL", "board: " + name, m.group(2) if m else "no result (board silent: press RESET once, or double-tap RESET and re-flash)")

    if p.returncode != 0:
        report("FAIL", "board: test process", "hardware test exited with code %d" % p.returncode)


CHECKS = [check_protocol_sizes, check_pins, check_baud, check_versions, check_config_defaults, check_ci_deploy, check_firmware_receiver, check_firmware_matrix, check_button_events, check_plugin_receiver, check_plugin_connection, check_plugin_safety, check_no_direct_debug_log,
          check_reconnect_guards, check_install, check_install_hmi_service, check_gamedata_snapshot, check_log, check_reconnect_log, check_hmi_context, check_hmi_cpp_layout, check_hmi_cpp, check_hmi_intro, check_hmi, check_hmi_render, check_krpc, check_sim, check_ship_report_contract, check_vessel_view_contract, check_editor_modes, check_verify_newlines, check_documentation, check_skill_mirrors, check_hmi_desktop, check_lan_names, check_git]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    # ./ci verify exports KSPDIR from ./ci ksp_list off Windows; the shared wk-rea install is the Windows default
    ap.add_argument("--ksp", default=os.environ.get("KSPDIR") or (DEFAULT_KSP if os.name == "nt" else ""),
                    help="KSP install directory")
    ap.add_argument("--log", help="analyse this KSP.log instead of <ksp>/KSP.log")
    ap.add_argument("--board", action="store_true", help="also test the Arduino over serial (KSP must be closed)")
    ap.add_argument("--port", help="serial port for --board, for example COM5")
    ap.add_argument("--snapshot", action="store_true", help="record the GameData entries now present")
    ap.add_argument("--lan-only", action="store_true", help="resolve configured Mac/PC/Pi names without game or hardware checks")
    ap.add_argument("--lan-config", default=os.environ.get("KSPHMI_LAN_CONFIG") or os.path.join(ROOT, "build", "agent-mailbox", "lan.json"),
                    help="private JSON manifest: hosts with mac, pc and pi hostname values")
    ap.add_argument("--docs-only", action="store_true", help="check documentation guides, UML lifelines and API coverage without KSP or hardware")
    ap.add_argument("--firmware-only", action="store_true", help="run native firmware tests without KSP or hardware")
    ap.add_argument("--plugin-only", action="store_true", help="run plugin button, receiver and connection tests without KSP or hardware")
    args = ap.parse_args()
    if args.plugin_only and (args.docs_only or args.firmware_only or args.lan_only or args.board or args.snapshot):
        ap.error("--plugin-only cannot be combined with --docs-only, --firmware-only, --board or --snapshot")
    if args.firmware_only and (args.docs_only or args.lan_only or args.board or args.snapshot):
        ap.error("--firmware-only cannot be combined with --docs-only, --board or --snapshot")
    if args.docs_only and (args.lan_only or args.board or args.snapshot):
        ap.error("--docs-only cannot be combined with --board or --snapshot")

    if args.lan_only and (args.board or args.snapshot):
        ap.error("--lan-only cannot be combined with --board or --snapshot")

    ctx = {"ksp": os.path.normpath(args.ksp) if args.ksp else "", "args": args}
    ctx["log"] = args.log or (os.path.join(ctx["ksp"], "KSP.log") if ctx["ksp"] else "")
    if not (args.docs_only or args.firmware_only or args.plugin_only or args.lan_only):
        report("PASS" if not ksp_running() else "WARN", "KSP process", "closed" if not ksp_running() else "running (install/flash/board checks need it closed)")
    checks = CHECKS
    if args.lan_only:
        checks = [check_lan_names]
    elif args.docs_only:
        checks = [check_documentation, check_skill_mirrors]
    elif args.firmware_only:
        checks = [check_firmware_receiver, check_firmware_matrix]
    elif args.plugin_only:
        checks = [check_button_events, check_plugin_receiver, check_plugin_connection]
    for chk in checks:
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
