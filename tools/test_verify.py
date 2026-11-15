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


if __name__ == "__main__":
    unittest.main()
