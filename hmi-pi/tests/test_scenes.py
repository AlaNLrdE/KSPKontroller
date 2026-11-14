"""Scene protocol and display checks, without KSP or hardware."""
import os
import sys
import unittest
from unittest.mock import patch, MagicMock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ksphmi.scenes import VIEWS, read_context


class ContextTest(unittest.TestCase):
    def test_discovery_uses_scene_protocol_and_ignores_unrelated_replies(self):
        import socket
        import threading
        from ksphmi.discovery import discover
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
            server.bind(('127.0.0.1', 0))
            server.settimeout(2)
            port = server.getsockname()[1]
            requests = []

            def respond():
                request, peer = server.recvfrom(256)
                requests.append(request)
                server.sendto(b'unrelated packet', peer)
                server.sendto(b'KSPHMI/1 mainmenu', peer)

            worker = threading.Thread(target=respond, daemon=True)
            worker.start()
            self.assertEqual(discover(port, targets=('127.0.0.1',)), '127.0.0.1')
            worker.join(2)
            self.assertFalse(worker.is_alive())
            self.assertEqual(requests, [b'KSPHMI/1'])
            self.assertEqual(discover(port, timeout=0.02, targets=('127.0.0.1',)), '')

    def test_discovery_accepts_only_local_peers(self):
        from ksphmi.discovery import is_local
        for address in ('127.0.0.1', '192.168.100.10', '10.0.0.5', '172.16.1.1', '169.254.3.4'):
            self.assertTrue(is_local(address), address)
        for address in ('8.8.8.8', '1.1.1.1', 'not-an-ip'):
            self.assertFalse(is_local(address), address)

    def test_reconnect_discovers_server_after_configured_address_fails(self):
        from ksphmi.telemetry import KrpcSource
        source = KrpcSource('old-server.invalid')
        connection = MagicMock()
        with patch.object(source, '_connect', side_effect=[OSError('address changed'), connection]), \
                patch('ksphmi.discovery.discover', return_value='127.0.0.1') as discovery, \
                patch.object(source, '_session', side_effect=lambda conn: source._stop.set()):
            source._run()
        self.assertEqual(source.address, '127.0.0.1')
        self.assertEqual(source.latest().server, 'kRPC 127.0.0.1:50000')
        discovery.assert_called_once_with(50002)
        connection.close.assert_called_once()

    def test_kiosk_boot_settings_preserve_root_and_are_idempotent(self):
        from pathlib import Path
        import subprocess
        import tempfile
        script = Path(__file__).resolve().parents[1] / 'systemd' / 'setup-kiosk.sh'
        python = script.read_text().split("<<'PY'\n", 1)[1].split('\nPY\n', 1)[0]
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

    def test_apsis_labels_stay_inside_orbit_panel_as_orbit_grows(self):
        os.environ.setdefault('SDL_VIDEODRIVER', 'dummy')
        try:
            import pygame
        except ImportError:
            self.skipTest('pygame available in build/hmi-venv for render checks')
        from dataclasses import replace
        from ksphmi import config
        from ksphmi.display import Display
        from ksphmi.telemetry import DemoSource
        try:
            for size in ((1260, 840), (1920, 1080), (2520, 1680)):
                display = Display(config.load(), windowed=True, size=size)
                u = display.u
                panel = pygame.Rect(display.w * 0.44 + 1.6 * u, 13 * u,
                                    display.w * 0.56 - 4.1 * u, 22 * u)
                original = display.text
                labels = []

                def text(label, *args, **kwargs):
                    rect = original(label, *args, **kwargs)
                    if label in ('Ap', 'Pe'):
                        self.assertTrue(panel.inflate(-2 * u, -2 * u).contains(rect),
                                        (size, label, rect, panel))
                        labels.append(label)
                    return rect

                display.text = text
                for apoapsis in (90_000, 250_000, 2_000_000, 50_000_000, 1e12):
                    display.orbit(panel, DemoSource(apoapsis=apoapsis).sample(0))
                self.assertEqual(labels, ['Ap', 'Pe'] * 5)
                # Escape trajectories still draw Pe but have no apoapsis.
                hyperbolic = replace(DemoSource().sample(0), eccentricity=1.5, semi_major_axis=-1e6, orbit_state=None)
                display.orbit(panel, hyperbolic)
                self.assertEqual(labels[-1], 'Pe')
        finally:
            pygame.quit()

    def test_accepts_only_versioned_known_scenes(self):
        sock = MagicMock()
        sock.__enter__.return_value = sock
        with patch('ksphmi.scenes.socket.socket', return_value=sock):
            for scene in VIEWS:
                sock.recv.return_value = ('KSPHMI/1 ' + scene).encode('ascii')
                self.assertEqual(read_context('localhost'), scene)
            for reply in (b'', b'KSPHMI/2 vab', b'KSPHMI/1 unknown', b'\xff'):
                sock.recv.return_value = reply
                self.assertEqual(read_context('localhost'), '')
            sock.recv.side_effect = TimeoutError()
            self.assertEqual(read_context('localhost'), '')

    def test_all_views_render_within_screen(self):
        os.environ.setdefault('SDL_VIDEODRIVER', 'dummy')
        try:
            import pygame
        except ImportError:
            self.skipTest('pygame available in build/hmi-venv for render checks')
        from ksphmi import config
        from ksphmi.display import Display
        from ksphmi.telemetry import Snapshot, NO_VESSEL
        try:
            for size in ((1260, 840), (1920, 1080)):
                display = Display(config.load(), windowed=True, size=size)
                original = display.text

                def text(*args, **kwargs):
                    rect = original(*args, **kwargs)
                    self.assertTrue(display.screen.get_rect().contains(rect), (size, args[0], rect))
                    return rect

                display.text = text
                for scene in VIEWS:
                    display.draw(Snapshot(state=NO_VESSEL, scene=scene))
        finally:
            pygame.quit()
