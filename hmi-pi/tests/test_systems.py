"""Resource semantics and compact flight layout regressions."""
import os
from pathlib import Path
import sys
import unittest
from dataclasses import replace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from ksphmi.systems import Resource, Stage, Systems, RESOURCE_NAMES


class ResourceTest(unittest.TestCase):
    def test_empty_missing_and_unknown_are_distinct(self):
        self.assertEqual(Resource('ElectricCharge', 0, 100).fraction, 0)
        self.assertIsNone(Resource('ElectricCharge', 0, 0).fraction)
        self.assertIsNone(Resource('ElectricCharge', None, None).fraction)
        self.assertIsNone(Resource('ElectricCharge', float('nan'), 100).fraction)
        self.assertEqual(Resource('ElectricCharge', 125, 100).fraction, 1)
        self.assertEqual(Resource('ElectricCharge', -1, 100).fraction, 0)

    def test_compact_metrics_leave_space_for_resources_and_staging(self):
        os.environ.setdefault('SDL_VIDEODRIVER', 'dummy')
        try:
            import pygame
        except ImportError:
            self.skipTest('pygame in build/hmi-venv required')
        from ksphmi import config
        from ksphmi.display import Display
        from ksphmi.telemetry import DemoSource
        try:
            for size in ((1260, 840), (1920, 1080), (2520, 1680)):
                d = Display(config.load(), windowed=True, size=size)
                original_tile, original_text = d.tile, d.text
                tiles, active = [], []

                def text(*args, **kwargs):
                    rect = original_text(*args, **kwargs)
                    bounds = active[-1] if active else d.screen.get_rect()
                    self.assertTrue(bounds.contains(rect), (size, args[0], rect, bounds))
                    return rect

                def tile(rect, *args, **kwargs):
                    tiles.append(rect)
                    active.append(rect)
                    original_tile(rect, *args, **kwargs)
                    active.pop()

                d.tile, d.text = tile, text
                sample = DemoSource().sample(0)
                for systems in (sample.systems, None, Systems(0),
                                Systems(3, tuple(Resource(name, 7.5, 100) for name in RESOURCE_NAMES),
                                        tuple(Stage(n, 10, 1234) for n in (3, 2, 1, 0)))):
                    tiles.clear()
                    d.draw(replace(sample, systems=systems))
                    self.assertEqual(len(tiles), 10)
                    span = max(r.bottom for r in tiles) - min(r.top for r in tiles)
                    self.assertLess(span, d.h * .40)
                    self.assertLess(max(r.bottom for r in tiles), d.h * .54)
        finally:
            pygame.quit()
