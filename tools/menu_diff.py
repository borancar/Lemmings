"""Where does the port's menu differ from the original's? Always freshly.

This exists because a stale `--raw` file has produced three misleading
measurements in this project: plates that appeared to match 18-25% when they
matched 93%, and a "regression" that was a capture taken before a fix. The
port's raw output is cheap to regenerate and analysis that does not regenerate
it is analysis of whatever was on disk.

So this regenerates before it reads, every time, and there is no flag to skip
it.

  uv run python tools/menu_diff.py                       # the menu
  uv run python tools/menu_diff.py --screen code
  uv run python tools/menu_diff.py --screen menu --rows 140 180 --cols 60 130
"""
import argparse
import collections
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# The BIOS attribute palette for mode 10h; the emulator reports DAC indices.
ATTR = [0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
        0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F]
INV = {v: i for i, v in enumerate(ATTR)}

CAPTURES = {"menu": "menu10.scrn", "code": "mk_f2_1.scrn",
            "input": "mk_f4_1.scrn", "briefing": "menu.scrn"}


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--screen", default="menu", choices=sorted(CAPTURES))
    ap.add_argument("--capture", default="")
    ap.add_argument("--data", default="lemming1.pc")
    ap.add_argument("--rows", nargs=2, type=int, metavar=("Y0", "Y1"))
    ap.add_argument("--cols", nargs=2, type=int, metavar=("X0", "X1"))
    ap.add_argument("--map", action="store_true",
                    help="print a character map of the differing pixels")
    a = ap.parse_args()

    raw = os.path.join(ROOT, "out", f"diff_{a.screen}.raw")
    if os.path.exists(raw):
        os.unlink(raw)                      # never read yesterday's
    subprocess.run(["make", "-C", os.path.join(ROOT, "reconstruct")],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run([os.path.join(ROOT, "reconstruct", "lemdev"),
                    "--data", os.path.join(ROOT, a.data),
                    "--screen", a.screen, "--raw", raw],
                   check=True, stdout=subprocess.DEVNULL)
    if not os.path.exists(raw):
        raise SystemExit("the port wrote no raw output")

    cap = a.capture or os.path.join(ROOT, "out", CAPTURES[a.screen])
    with open(cap, "rb") as f:
        blob = f.read()
    if blob[:4] != b"SCRN":
        raise SystemExit(f"{cap}: not a SCRN capture")
    w, h = struct.unpack_from("<HH", blob, 4)
    if not any(blob[8:8 + 768]):
        raise SystemExit(f"{cap}: taken mid-fade, all-black palette - useless")
    theirs = blob[8 + 768:]
    with open(raw, "rb") as f:
        rb = f.read()
    ours = rb[10 + 48:]

    y0, y1 = a.rows if a.rows else (0, h)
    x0, x1 = a.cols if a.cols else (0, w)
    d = [(x, y) for y in range(y0, y1) for x in range(x0, x1)
         if INV.get(theirs[y * w + x], 0) != (ours[y * w + x] & 0x0F)]
    total = (y1 - y0) * (x1 - x0)
    print(f"\n  {a.screen}: {len(d)} of {total} differ "
          f"({100 * (total - len(d)) / total:.2f}% match)")
    if not d:
        return 0

    bands = collections.Counter(y // 16 * 16 for _, y in d)
    print("\n  by 16-row band:")
    for band in sorted(bands):
        xs = [x for x, y in d if band <= y < band + 16]
        print(f"    {band:3d}-{band+15:3d}: {bands[band]:5d}   "
              f"x {min(xs):3d}..{max(xs):3d}")

    pairs = collections.Counter(
        (INV.get(theirs[y * w + x], 0), ours[y * w + x] & 0x0F) for x, y in d)
    print("\n  (original, port) value pairs, commonest first:")
    print("   ", dict(pairs.most_common(8)))

    if a.map:
        print("\n  '#' where they differ:")
        step = max(1, (x1 - x0) // 120)
        for y in range(y0, y1, max(1, (y1 - y0) // 40)):
            row = "".join(
                "#" if INV.get(theirs[y*w+x], 0) != (ours[y*w+x] & 0x0F)
                else "." for x in range(x0, x1, step))
            print(f"   {y:3d} {row}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
