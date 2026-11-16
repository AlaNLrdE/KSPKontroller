"""systemd/setup-kiosk.sh edits the Pi's boot files with python3 (run by ctest when a python3 is found).

The script itself needs root; this test runs only its embedded Python on temporary copies of the boot files.
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / 'systemd' / 'setup-kiosk.sh'
INTRO = SCRIPT.parent / 'ksphmi-intro.sh'

# Stands in for mpv: records its arguments, serves --input-ipc-server, answers eof-reached true, exits on quit.
FAKE_MPV = r"""#!/usr/bin/env python3
import json, os, socket, sys
args = sys.argv[1:]
open(os.environ['FAKE_MPV_ARGS'], 'w').write(json.dumps(args))
path = [a.split('=', 1)[1] for a in args if a.startswith('--input-ipc-server=')][0]
server = socket.socket(socket.AF_UNIX)
server.bind(path)
server.listen(1)
conn, _ = server.accept()
for line in conn.makefile():
    msg = json.loads(line)
    if msg['command'] == ['quit']:
        break
    conn.sendall((json.dumps({'data': True, 'request_id': msg.get('request_id'), 'error': 'success'}) + '\n').encode())
conn.close()
os.unlink(path)
"""


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
                # the consoles stay as they were: console=ttynull hung the Pi 4 at boot
                consoles = [a for a in args if a.startswith('console=')]
                self.assertEqual(consoles, ['console=serial0,115200', 'console=tty1'])
                for flag in ('quiet', 'loglevel=3', 'logo.nologo', 'vt.global_cursor_default=0', 'consoleblank=0',
                             'systemd.show_status=false', 'plymouth.enable=0'):
                    self.assertEqual(args.count(flag), 1, flag)
                self.assertNotIn('loglevel=7', args)
                self.assertEqual(cmdline.read_text().count('\n'), 1)
                self.assertEqual(config.read_text().count('disable_splash=1'), 1)
                self.assertEqual(config.read_text().count('avoid_warnings=1'), 1)
                self.assertIn('dtoverlay=vc4-kms-v3d', config.read_text())
            self.assertEqual(cmdline.with_name('cmdline.txt.ksphmi-backup').read_text(), original)

    def test_ttynull_from_an_older_kiosk_is_removed(self):
        python = SCRIPT.read_text().split("<<'PY'\n", 1)[1].split('\nPY\n', 1)[0]
        with tempfile.TemporaryDirectory() as folder:
            cmdline = Path(folder) / 'cmdline.txt'
            cmdline.write_text('console=ttynull console=serial0,115200 root=PARTUUID=test rootwait quiet\n')
            subprocess.run([sys.executable, '-c', python, str(cmdline)], check=True)
            args = cmdline.read_text().split()
            self.assertNotIn('console=ttynull', args)
            self.assertEqual([a for a in args if a.startswith('console=')], ['console=serial0,115200'])
            self.assertEqual(args.count('quiet'), 1)

    def test_script_names_its_deployed_path(self):
        self.assertIn('~/ksphmi/cpp/systemd/setup-kiosk.sh', SCRIPT.read_text())

    def test_script_installs_the_splash_and_can_undo(self):
        text = SCRIPT.read_text()
        for needle in ('--undo', 'ksphmi-splash.service', 'TAG+="systemd"', 'mask --now getty@tty1',
                       'apt-get install -y --no-install-recommends mpv', '/etc/ksphmi-mpv.installed',
                       'unmask getty@tty1', 'enable-linger', '.ksphmi-backup', 'Storage=persistent',
                       '/etc/ksphmi-kiosk.applied', 'ATTR{name}=="vc4drmfb"', 'disable --now ksphmi-splash'):
            self.assertIn(needle, text)

    def test_splash_unit_is_early_long_running_and_never_holds_up_the_boot(self):
        unit = (SCRIPT.parent / 'ksphmi-splash.service').read_text()
        lines = [l.strip() for l in unit.splitlines() if l.strip() and not l.startswith('#')]
        for line in ('DefaultDependencies=no', 'After=local-fs.target', 'Type=simple', 'User=@USER@',
                     'Group=video', 'SupplementaryGroups=video render audio', 'TimeoutStopSec=3',
                     'RuntimeDirectory=ksphmi-intro', 'ExecStart=/bin/sh @HOME@/ksphmi/cpp/systemd/ksphmi-intro.sh',
                     'WantedBy=sysinit.target'):
            self.assertIn(line, lines)
        # dev-fb0.device timed out on late monitors and the unit never ran: the script waits for the screen itself.
        self.assertFalse([l for l in lines if 'dev-fb0' in l])
        # A long-running splash ordered before the user sessions would delay SSH and the HMI itself.
        self.assertFalse([l for l in lines if l.startswith('Before=') and 'shutdown.target' not in l])
        self.assertNotIn('Type=oneshot', lines)
        runtime = [int(l.split('=', 1)[1]) for l in lines if l.startswith('RuntimeMaxSec=')]
        self.assertEqual(len(runtime), 1)
        self.assertGreater(runtime[0], 600)  # the splash's own 10 minute cap comes first
        hmi = (SCRIPT.parent / 'ksphmi.service').read_text()
        self.assertIn('WantedBy=default.target', hmi)
        self.assertIn('Restart=always', hmi)
        self.assertIn('StartLimitIntervalSec=0', hmi)


    @unittest.skipIf(os.name == 'nt' or not shutil.which('sh'), 'POSIX sh and AF_UNIX sockets only')
    def test_intro_script_plays_holds_the_last_frame_and_quits_on_request(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'bin').mkdir()
            mpv = root / 'bin' / 'mpv'
            mpv.write_text(FAKE_MPV.replace('/usr/bin/env python3', sys.executable, 1))
            mpv.chmod(0o755)
            video = root / 'intro.mp4'
            video.write_bytes(b'')
            status = root / 'card1-HDMI-A-1-status'
            status.write_text('connected\n')
            run = root / 'run'
            run.mkdir()
            env = dict(os.environ, PATH='%s:%s' % (root / 'bin', os.environ.get('PATH', '')),
                       RUNTIME_DIRECTORY=str(run), XDG_RUNTIME_DIR=str(root), KSPHMI_INTRO_VIDEO=str(video),
                       KSPHMI_INTRO_DRM=str(status), KSPHMI_INTRO_CONNECTORS=str(status),
                       KSPHMI_INTRO_LOG=str(root / 'mpv.log'), KSPHMI_APP_DIR=str(root), HOME=str(root),
                       FAKE_MPV_ARGS=str(root / 'args.json'))
            proc = subprocess.Popen(['sh', str(INTRO)], env=env, stderr=subprocess.DEVNULL)
            try:
                sock = run / 'mpv.sock'
                deadline = time.time() + 10
                while not sock.exists() and time.time() < deadline:
                    time.sleep(0.05)
                self.assertTrue(sock.exists(), 'mpv socket')
                client = socket.socket(socket.AF_UNIX)
                client.connect(str(sock))
                client.sendall(b'{"command":["get_property","eof-reached"],"request_id":1}\n')
                self.assertTrue(json.loads(client.makefile().readline())['data'])
                deadline = time.time() + 3
                while (run / 'starting').exists() and time.time() < deadline:
                    time.sleep(0.05)
                self.assertFalse((run / 'starting').exists(), 'the script removes "starting" once mpv listens')
                client.sendall(b'{"command":["quit"]}\n')
                self.assertEqual(proc.wait(timeout=5), 0)
                client.close()
            finally:
                if proc.poll() is None:
                    proc.kill()
                proc.wait()
            args = json.loads((root / 'args.json').read_text())
            for flag in ('--keep-open=always', '--no-terminal', '--osd-level=0', '--no-osc', '--profile=fast', '--hwdec=v4l2m2m-copy',
                         '--audio-fallback-to-null=yes', '--audio-device=alsa/hdmi:CARD=vc4hdmi0,DEV=0',
                         '--input-ipc-server=%s' % (run / 'mpv.sock'), '--drm-device=%s' % status, str(video)):
                self.assertIn(flag, args)

    def test_intro_script_falls_back_to_the_loading_screen(self):
        text = INTRO.read_text()
        self.assertIn('command -v mpv', text)
        self.assertIn('exec "$app/build/ksphmi" --splash', text)
        self.assertIn('$dir/mpv.sock', text)


if __name__ == '__main__':
    unittest.main()
