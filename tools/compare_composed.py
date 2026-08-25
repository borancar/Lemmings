# SPDX-License-Identifier: GPL-2.0-only
"""Compare the port's canvas against the game's OWN composed level.

Far better evidence than comparing screens. The game composes its level into
video memory before it displays anything, so this comparison has no sprites,
no cursor, no palette, no viewport alignment and no fade in it - just two
programs' terrain, as 4-bit indices, over the whole 1584x160 level.

Capture the reference by breaking at the instruction *after* the terrain loop,
which is the exact moment composition is complete:

  uv run python tools/play.py --auto --start --dump-vga --dump-when 0x10 \
      --click-tries 0 --cmd "break i+0x376a" --cmd "sleep 20" \
      --cmd "click down 0 160 100" --cmd "sleep 1" --cmd "click up 0" \
      --cmd "sleep 20" --cmd "planes out/done.bin" -- --run-seconds 210

Timing matters and silently: a dump taken while composition is still running
looks like a port that draws terrain the game does not. That cost a while.
"""
import argparse
import struct
import numpy as np

# Where the game composes, read out of its own registers at the blit:
# [0x66f4] is the destination row stride and [0x1f76] the base.
BASE, STRIDE, HEIGHT = 19008, 208, 160


def decode(path, base=BASE, stride=STRIDE, h=HEIGHT):
    blob = np.frombuffer(open(path, "rb").read(), dtype=np.uint8)
    per = len(blob) // 4
    g = np.zeros((h, stride * 8), np.uint8)
    for p in range(4):
        rows = blob[p * per:(p + 1) * per][base:base + stride * h]
        g |= np.unpackbits(rows.reshape(h, stride), axis=1).astype(np.uint8) << p
    return g


def load_raw(path):
    with open(path, "rb") as f:
        magic = f.read(6)
        w, h = struct.unpack("<HH", f.read(4))
        if magic == b"LEMRW2":
            f.read(48)
        pix = np.frombuffer(f.read(w * h), dtype=np.uint8)
    return pix.reshape(h, w), w, h


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("planes", help="a plane dump taken at composition's end")
    ap.add_argument("raw", nargs="+", help="the port's --raw output(s)")
    ap.add_argument("--confusion", action="store_true")
    a = ap.parse_args()

    g = decode(a.planes)
    for path in a.raw:
        port, W, H = load_raw(path)
        port = port[:HEIGHT]
        best = None
        for off in range(0, 40):
            n = min(W, g.shape[1] - off)
            s = int((g[:, off:off + n] == port[:, :n]).sum())
            if best is None or s > best[1]:
                best = (off, s, n)
        off, s, n = best
        game = g[:, off:off + W]
        d = game != port
        miss = int(((game == 0) & (port != 0)).sum())
        extra = int(((game != 0) & (port == 0)).sum())
        print(f"{path}")
        print(f"  offset {off}   {100.0 * s / (HEIGHT * n):6.2f}%   "
              f"differing {int(d.sum())}   "
              f"port-only {miss}   game-only {extra}")
        if a.confusion and int(d.sum()):
            x = (game ^ port)[d]
            bits = " ".join(f"bit{b}:{100.0*int(((x>>b)&1).sum())/len(x):.0f}%"
                            for b in range(4))
            print(f"  differing bits: {bits}")


if __name__ == "__main__":
    main()
