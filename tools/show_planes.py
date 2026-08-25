# SPDX-License-Identifier: GPL-2.0-only
"""Render a whole plane dump as a picture, to find where a game drew.

A screenshot shows the window the CRTC start address points at. This shows all
64K of video memory laid out at whatever row stride the game is using, so
"nothing was drawn" and "it was drawn somewhere else" stop looking alike.

  uv run python tools/show_planes.py planes.bin --stride 44 -o all.png
"""
import argparse
import numpy as np
from PIL import Image

ap = argparse.ArgumentParser(description=__doc__,
                             formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("dump")
ap.add_argument("--stride", type=int, default=44, help="bytes per row")
ap.add_argument("--rows", type=int, default=0, help="0 = as many as fit")
ap.add_argument("--start", type=lambda v: int(v, 0), default=0)
ap.add_argument("-o", required=True)
ap.add_argument("--mark", type=lambda v: int(v, 0), action="append", default=[],
                help="draw a line at this byte offset (e.g. a start address)")
a = ap.parse_args()

blob = np.frombuffer(open(a.dump, "rb").read(), dtype=np.uint8)
per = len(blob) // 4
planes = [blob[i * per:(i + 1) * per] for i in range(4)]

rows = a.rows or (per - a.start) // a.stride
w = a.stride * 8
img = np.zeros((rows, w), dtype=np.uint8)
for p in range(4):
    pl = planes[p][a.start:a.start + rows * a.stride].reshape(rows, a.stride)
    bits = np.unpackbits(pl, axis=1)          # MSB first, which is pixel order
    img |= bits.astype(np.uint8) << p

# A readable 16-colour ramp; the real palette does not matter for "where is it".
ramp = np.array([(0, 0, 0)] + [(40 + 14 * i, 60 + 12 * i, 90 + 10 * i)
                               for i in range(15)], dtype=np.uint8)
rgb = ramp[img]
for off in a.mark:
    y = (off - a.start) // a.stride
    if 0 <= y < rows:
        rgb[y, :] = (255, 0, 0)
Image.fromarray(rgb).save(a.o)
print(f"wrote {a.o}  {w}x{rows} (stride {a.stride}, from {a.start:#x})")
for off in a.mark:
    print(f"  red line at byte {off:#x} = row {(off - a.start) // a.stride}")
