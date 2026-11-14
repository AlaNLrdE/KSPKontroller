"""Scene protocol and display checks, without KSP or hardware."""
import os
import sys
import unittest
from unittest.mock import patch, MagicMock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ksphmi.scenes import VIEWS, read_context


class ContextTest(unittest.TestCase):




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
