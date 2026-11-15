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
            original = ('console=serial0,115200 console=tty1 root=PARTUUID=test fsck.repair=yes rootwait '
                        'ds=nocloud;i=rpi-imager-1 loglevel=7 cfg80211.ieee80211_regdom=MX\n')
            cmdline.write_text(original)
            config.write_text('[all]\ndtoverlay=vc4-kms-v3d\n')
            for _ in range(2):
                subprocess.run([sys.executable, '-c', python, str(cmdline)], check=True)
                args = cmdline.read_text().split()
                self.assertIn('root=PARTUUID=test', args)
                self.assertIn('rootwait', args)
                self.assertIn('ds=nocloud;i=rpi-imager-1', args)
                self.assertIn('cfg80211.ieee80211_regdom=MX', args)
                self.assertNotIn('console=tty1', args)
                # ttynull first, serial0 after it: /dev/console (the last console=) stays the serial port
                consoles = [a for a in args if a.startswith('console=')]
                self.assertEqual(consoles, ['console=ttynull', 'console=serial0,115200'])
                for flag in ('quiet', 'loglevel=3', 'logo.nologo', 'vt.global_cursor_default=0', 'consoleblank=0',
                             'systemd.show_status=false', 'plymouth.enable=0'):
                    self.assertEqual(args.count(flag), 1, flag)
                self.assertNotIn('loglevel=7', args)
                self.assertEqual(cmdline.read_text().count('\n'), 1)
                self.assertEqual(config.read_text().count('disable_splash=1'), 1)
                self.assertEqual(config.read_text().count('avoid_warnings=1'), 1)
                self.assertIn('dtoverlay=vc4-kms-v3d', config.read_text())
            self.assertEqual(cmdline.with_name('cmdline.txt.ksphmi-backup').read_text(), original)

    def test_script_names_its_deployed_path(self):
        self.assertIn('~/ksphmi/cpp/systemd/setup-kiosk.sh', SCRIPT.read_text())

    def test_script_installs_the_splash_and_can_undo(self):
        text = SCRIPT.read_text()
        for needle in ('--undo', 'ksphmi-splash.service', 'TAG+="systemd"', 'mask --now getty@tty1',
                       'unmask getty@tty1', 'enable-linger', '.ksphmi-backup'):
            self.assertIn(needle, text)

    def test_splash_unit_is_an_early_oneshot_as_the_hmi_user(self):
        unit = (SCRIPT.parent / 'ksphmi-splash.service').read_text()
        for line in ('DefaultDependencies=no', 'After=dev-fb0.device', 'Type=oneshot', 'User=@USER@', 'Group=video',
                     'ExecStart=@HOME@/ksphmi/cpp/build/ksphmi --splash', 'WantedBy=sysinit.target',
                     'Before=systemd-user-sessions.service'):
            self.assertIn(line, unit)
        hmi = (SCRIPT.parent / 'ksphmi.service').read_text()
        self.assertIn('WantedBy=default.target', hmi)
        self.assertIn('Restart=always', hmi)
        self.assertIn('StartLimitIntervalSec=0', hmi)


if __name__ == '__main__':
    unittest.main()
