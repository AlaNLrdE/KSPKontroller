"""verify.py content checks must give the same answer for LF files (agent worktrees) and CRLF files (the main
checkout with core.autocrlf=true). Run: build/venv/Scripts/python.exe -m unittest tools/test_verify.py"""
import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import verify  # noqa: E402

# Every repo file check_ship_report_contract reads (relative to the repo root).
SHIP_REPORT_FILES = (
    "KSPSerialIO/KSPSerialIO.Hmi/HmiService.cs",
    "KSPSerialIO/KSPSerialIO.Hmi/ShipReportFormat.cs",
    "KSPSerialIO/KSPSerialIO.Hmi/EditorShipReport.cs",
    "KSPSerialIO/KSPSerialIO.Hmi/KSPSerialIO.Hmi.csproj",
    "hmi-cpp/include/shipreport.hpp",
    "hmi-cpp/src/telemetry.cpp",
    "tools/kspsim/krpcserver.py",
    "tools/kspsim/ships.py",
    "KSPSerialIO/KSPSerialIO/KSPSerialIO.csproj",
    "KSPSerialIO/KSPSerialIO/KSPIO.cs",
    "KSPSerialIO/KSPSerialIO/ButtonEvents.cs",
    "KSPSerialIO/KSPSerialIO/HmiContext.cs",
)


def copy_with_eol(root, eol):
    for rel in SHIP_REPORT_FILES:
        with open(os.path.join(verify.ROOT, rel), "rb") as f:
            data = f.read().replace(b"\r\n", b"\n").replace(b"\n", eol)
        dest = os.path.join(root, rel)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as f:
            f.write(data)


class NewlineAgnosticTest(unittest.TestCase):
    def test_read_normalises_crlf(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "x.py")
            with open(path, "wb") as f:
                f.write(b"A = 1\r\nVERSION = 1\r\n")
            self.assertEqual(verify.read(path), "A = 1\nVERSION = 1\n")

    def test_ship_report_contract_with_lf_and_crlf(self):
        for eol in (b"\n", b"\r\n"):
            with self.subTest(eol=eol), tempfile.TemporaryDirectory() as d:
                copy_with_eol(d, eol)
                patches = {
                    "ROOT": d,
                    "HMI_PLUGIN_DIR": os.path.join(d, "KSPSerialIO", "KSPSerialIO.Hmi"),
                    "HMI_DIR": os.path.join(d, "hmi-cpp"),
                    "CS": os.path.join(d, "KSPSerialIO", "KSPSerialIO", "KSPIO.cs"),
                    "BUILT_DLL": os.path.join(d, "missing.dll"),
                    "results": [],
                }
                with mock.patch.multiple(verify, **patches), mock.patch.object(verify.shutil, "which", return_value=None):
                    verify.check_ship_report_contract({})
                    contract = [r for r in verify.results if r[1] == "ship report: contract"]
                self.assertEqual(len(contract), 1)
                self.assertEqual(contract[0][0], "PASS", contract[0][2])


class KspProcessPatternTest(unittest.TestCase):
    """pgrep -f matches the whole command line, so the pattern must not hit agent paths (2026-11-19 false positive)."""

    GAME = ("/Users/a/Library/Application Support/Steam/steamapps/common/Kerbal Space Program/KSP.app/Contents/MacOS/KSP",
            "/home/a/.steam/steam/steamapps/common/Kerbal Space Program/KSP.x86_64 -single-instance")
    NOT_GAME = ("python3 tools/agent_daemon.py --config /Users/a/wk/devKSP/run/agent-mailbox/mac-agentsapp.json "
                "--repo /Users/a/wk/devKSP/apps/iKSPModAgentsApp run",
                "python3 /Users/a/wk/devKSP/ksp1/KSPKontroller/tools/agent_mailbox.py watch --once",
                "open /Users/a/Downloads/KSPxapp.zip")

    def test_matches_the_game_and_not_agent_command_lines(self):
        import re
        for line in self.GAME:
            self.assertTrue(re.search(verify.KSP_PROCESS, line), line)
        for line in self.NOT_GAME:
            self.assertFalse(re.search(verify.KSP_PROCESS, line), line)

    def test_wsl_sees_the_windows_game(self):
        """Under WSL the game is a Windows process: pgrep misses it, tasklist.exe sees it (2026-11-21)."""
        from unittest import mock
        calls = []

        def fake_run(cmd, **kw):
            calls.append(cmd[0])
            out = "KSP_x64.exe                  1234 Console    1  9,000,000 K\n" if cmd[0] == "tasklist.exe" else ""
            return mock.Mock(stdout=out, returncode=1)

        with mock.patch.dict(os.environ, {"KSP_WSL": "1"}), mock.patch.object(verify.os, "name", "posix"), \
                mock.patch.object(verify.subprocess, "run", side_effect=fake_run):
            self.assertTrue(verify.ksp_running())
        self.assertEqual(calls, ["tasklist.exe"])
        with mock.patch.dict(os.environ, {"KSP_WSL": "0"}), mock.patch.object(verify.os, "name", "posix"), \
                mock.patch.object(verify.subprocess, "run", side_effect=fake_run):
            self.assertFalse(verify.ksp_running())             # plain Linux: only pgrep, which finds nothing

    def test_ci_uses_the_same_pattern(self):
        with open(os.path.join(verify.ROOT, "ci"), encoding="utf-8") as f:
            self.assertIn("pgrep -f '%s'" % verify.KSP_PROCESS, f.read())


class BoardResultTest(unittest.TestCase):
    def test_hardware_failure_cannot_be_hidden_by_other_passing_checks(self):
        passing = "".join("  [PASS] %s: good\n" % name for name in
                          ("handshake", "telemetry", "debug", "reopen", "idle timeout"))
        for status, exit_code in (("FAIL", 1), (None, 0), ("PASS", 1), ("PASS", 0)):
            with self.subTest(status=status, exit_code=exit_code):
                output = passing + ("  [%s] truncated reopen: result\n" % status if status else "")
                process = mock.Mock(stdout=output, returncode=exit_code)
                with mock.patch.object(verify, "ksp_running", return_value=False), \
                     mock.patch.object(verify.subprocess, "run", return_value=process), \
                     mock.patch.object(verify, "results", []):
                    verify.check_board({"args": mock.Mock(port="COM5")})
                    failed = any(result[0] == "FAIL" for result in verify.results)
                self.assertEqual(failed, status != "PASS" or exit_code != 0)


class HmiScreenTest(unittest.TestCase):
    """hmi: screen judges the output the HMI logged: fb0/blank for fbdev, HDMI connectors for SDL KMSDRM."""
    KMSDRM = "screen_2520x1680_(SDL_KMSDRM_/_opengl)"
    FBDEV = "screen_2520x1680_(fbdev_/dev/fb0_16_bpp)"
    HDMI_ON = "card0-HDMI-A-1:connected:On,card0-HDMI-A-2:disconnected:Off,"

    def status(self, state="active", **env):
        return verify.hmi_screen_status(env, state)

    def test_kmsdrm_with_blanked_fb0_and_lit_hdmi_passes(self):
        result = self.status(fbblank="4", screenline=self.KMSDRM, drm=self.HDMI_ON)
        self.assertEqual(result[0], "PASS", result[1])
        self.assertIn("card0-HDMI-A-1", result[1])

    def test_kmsdrm_with_dpms_off_fails(self):
        result = self.status(fbblank="4", screenline=self.KMSDRM, drm="card0-HDMI-A-1:connected:Off,")
        self.assertEqual(result[0], "FAIL")
        self.assertIn("dpms", result[1])

    def test_kmsdrm_without_connected_hdmi_fails(self):
        drm = "card0-HDMI-A-1:disconnected:Off,card0-HDMI-A-2:disconnected:Off,"
        for value in (drm, ""):
            with self.subTest(drm=value):
                result = self.status(fbblank="4", screenline=self.KMSDRM, drm=value)
                self.assertEqual(result[0], "FAIL")
                self.assertIn("no HDMI connector is connected", result[1])

    def test_kmsdrm_problem_only_warns_when_service_is_down(self):
        result = self.status("inactive", screenline=self.KMSDRM, drm="")
        self.assertEqual(result[0], "WARN")

    def test_fbdev_keeps_blank_rule(self):
        self.assertEqual(self.status(fbblank="4", screenline=self.FBDEV, drm=self.HDMI_ON)[0], "FAIL")
        self.assertEqual(self.status(fbblank="0", screenline=self.FBDEV, drm=self.HDMI_ON)[0], "PASS")

    def test_unknown_output_warns_with_raw_values(self):
        result = self.status(fbblank="4", screenline="", drm=self.HDMI_ON)
        self.assertEqual(result[0], "WARN")
        self.assertIn("fb0/blank=4", result[1])
        self.assertIn("HDMI-A-1", result[1])

    def test_nothing_probed_reports_nothing(self):
        self.assertIsNone(self.status())

    def test_check_hmi_reports_screen_from_ssh_probe(self):
        stdout = "\n".join(["active", "0", "fbblank=4", "screenline=" + self.KMSDRM, "drm=" + self.HDMI_ON]) + "\n"
        process = mock.Mock(stdout=stdout, stderr="", returncode=0)
        with mock.patch.object(verify.subprocess, "run", return_value=process), \
             mock.patch.object(verify, "results", []):
            verify.check_hmi({})
            screen = [r for r in verify.results if r[1] == "hmi: screen"]
        self.assertEqual(len(screen), 1)
        self.assertEqual(screen[0][0], "PASS", screen[0][2])


if __name__ == "__main__":
    unittest.main()
