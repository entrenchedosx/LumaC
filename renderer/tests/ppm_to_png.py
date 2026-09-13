#!/usr/bin/env python3
"""Convert the GPU-rendered pbr_scene.ppm (written by test_pbr_vulkan
into its working directory) to docs/images/pbr-scene.png, and print
content statistics proving the frame is a real varied scene (not
blank/uniform). Stdlib only. Run from the repository root:

    py -3 renderer/tests/ppm_to_png.py [path/to/pbr_scene.ppm]
"""
import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__))))


def find_ppm(cli_arg):
    if cli_arg is not None:
        return cli_arg
    for cand in (os.path.join(ROOT, "pbr_scene.ppm"),
                 os.path.join(ROOT, "build", "renderer",
                              "pbr_scene.ppm")):
        if os.path.exists(cand):
            return cand
    raise SystemExit("pbr_scene.ppm not found; run test_pbr_vulkan first")


def main():
    src = find_ppm(sys.argv[1] if len(sys.argv) > 1 else None)
    with open(src, "rb") as f:
        magic = f.readline().strip()
        wh = f.readline().split()
        maxval = f.readline()
        w, h = int(wh[0]), int(wh[1])
        data = f.read()
    assert magic == b"P6", magic
    assert len(data) == w * h * 3, (len(data), w, h)

    px = [data[i:i + 3] for i in range(0, len(data), 3)]
    lum = sorted(0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]
                 for p in px)
    n = len(lum)
    mean = sum(lum) / n
    var = sum((x - mean) ** 2 for x in lum) / n
    std = var ** 0.5
    nonblack = sum(1 for p in px if p[0] + p[1] + p[2] > 12)
    print("frame %dx%d quints L: %.0f %.0f %.0f %.0f %.0f" %
          (w, h, lum[0], lum[n // 10], lum[n // 2], lum[9 * n // 10],
           lum[-1]))
    print("mean %.1f std %.1f non-black %.3f" %
          (mean, std, nonblack / n))
    # A genuine varied scene: spread brightness, decisive non-black
    # majority, no single-bin dominance.
    assert std > 15.0, "suspiciously flat frame"
    assert nonblack / n > 0.25, "suspiciously empty frame"
    assert lum[9 * n // 10] > 40.0, "no lit regions"

    def chunk(ctype, payload):
        c = ctype + payload
        return (struct.pack(">I", len(payload)) + c +
                struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF))

    raw = b"".join(
        b"\x00" + b"".join(bytes(px[y * w + x]) for x in range(w))
        for y in range(h))
    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0,
                                      0)) +
           chunk(b"IDAT", zlib.compress(raw, 9)) +
           chunk(b"IEND", b""))
    dst = os.path.join(ROOT, "docs", "images", "pbr-scene.png")
    with open(dst, "wb") as f:
        f.write(png)
    print("wrote", os.path.relpath(dst, ROOT), len(png), "bytes")


if __name__ == "__main__":
    main()
