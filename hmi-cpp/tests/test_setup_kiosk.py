"""systemd/setup-kiosk.sh edits the Pi's boot files with python3 (run by ctest when a python3 is found).

The script itself needs root; this test runs only its embedded Python on temporary copies of the boot files.
"""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / 'systemd' / 'setup-kiosk.sh'


class KioskTest(unittest.TestCase):
    def test_kiosk_boot_settings_preserve_root_and_are_idempotent(self):
        python = SCRIPT.read_text().split("<<'PY'\n", 1)[1].split('\nPY\n', 1)[0]
        with tempfile.TemporaryDirectory() as folder:
            cmdline = Path(folder) / 'cmdline.txt'
            config = Path(folder) / 'config.txt'
            original = 'console=ttyS0,115200 console=tty1 root=PARTUUID=test rootwait\n'
            cmdline.write_text(original)
            config.write_text('[all]\ndtoverlay=vc4-kms-v3d\n')
            for _ in range(2):
                subprocess.run([sys.executable, '-c', python, str(cmdline)], check=True)
                args = cmdline.read_text().split()
                self.assertIn('root=PARTUUID=test', args)
                self.assertIn('rootwait', args)
                self.assertIn('console=ttyS0,115200', args)
                self.assertNotIn('console=tty1', args)
                self.assertEqual(args.count('quiet'), 1)
                self.assertEqual(cmdline.read_text().count('\n'), 1)
                self.assertEqual(config.read_text().count('disable_splash=1'), 1)
            self.assertEqual(cmdline.with_name('cmdline.txt.ksphmi-backup').read_text(), original)

    def test_script_names_its_deployed_path(self):
        self.assertIn('~/ksphmi/cpp/systemd/setup-kiosk.sh', SCRIPT.read_text())


if __name__ == '__main__':
    unittest.main()
