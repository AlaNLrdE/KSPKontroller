"""Exercise real CLI screenshots after warm-up, including SDL's deferred software command queue."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib


def navball_blue_pixels(path):
    png = path.read_bytes()
    assert png[:8] == b"\x89PNG\r\n\x1a\n"
    compressed = bytearray()
    offset = 8
    width = height = 0
    while offset < len(png):
        size = struct.unpack_from(">I", png, offset)[0]
        kind = png[offset + 4:offset + 8]
        payload = png[offset + 8:offset + 8 + size]
        if kind == b"IHDR":
            width, height, depth, color, compression, filtering, interlace = struct.unpack(">IIBBBBB", payload)
            assert (depth, color, compression, filtering, interlace) == (8, 2, 0, 0, 0)
        elif kind == b"IDAT":
            compressed.extend(payload)
        offset += size + 12
    assert (width, height) == (1920, 1080)
    raw = zlib.decompress(compressed)
    stride = width * 3 + 1
    assert len(raw) == stride * height
    count = 0
    for y in range(int(height * .50), int(height * .93)):
        assert raw[y * stride] == 0  # ksphmi's lossless RGB writer uses the unfiltered PNG row format
        for x in range(int(width * .50), int(width * .95)):
            p = y * stride + 1 + x * 3
            red, green, blue = raw[p:p + 3]
            count += green > red + 20 and blue > red + 30
    return count


def main(binary):
    with tempfile.TemporaryDirectory(prefix="ksphmi-shots-") as folder:
        for batching in ("1", "0"):
            for skin in ("steampunk", "apollo", "classic"):
                path = Path(folder) / f"{skin}-{batching}.png"
                env = dict(os.environ, SDL_RENDER_BATCHING=batching)
                result = subprocess.run(
                    [binary, "--demo", "--skin", skin, "--size", "1920x1080", "--seconds", "1.2",
                     "--screenshot", str(path)], env=env, capture_output=True, text=True, timeout=30)
                assert result.returncode == 0, result.stdout + result.stderr
                count = navball_blue_pixels(path)
                assert count > 2000, f"{skin}, batching={batching}: incomplete screenshot ({count} NavBall sky pixels)"
                print(f"{skin}, batching={batching}: complete warmed-up software frame")


if __name__ == "__main__":
    main(str(Path(sys.argv[1]).resolve()))
