"""The game's own 320x200 screen against the original's, frame by frame.

This is a different claim from tools/compare_run.py. That one scores a canvas
and a panel that tools/lemdev composed SEPARATELY; this one scores the screen
the window path actually puts on the display, which is what a player sees. The
two were different code for a long time, and every frame of the first matched
while the second showed an empty cave with no panel at all.

Mode 0x0D is two things stacked - the play area at rows 0..159 and the panel
at 160..199 - and they are scored separately, because an average hides which
one is missing.

    uv run python tools/compare_screen.py --ref out/ref5100 --dir out/gamescreen

The port stores ATTRIBUTES 0..15 and the original's capture stores DAC indices;
attributes 8..15 map to DAC 16..23, which tools/probe_dac.py read straight off
the attribute controller.
"""
import argparse
import os
import struct
import sys

PLAY_H = 160
SCREEN_H = 200
SCREEN_W = 320
CURSOR = 28          # the game's own dithered cross-hair, wherever it lands

# attribute -> DAC. Measured: attr_pal reads 00..07 then 10..17.
MAP = bytes((i if i < 8 else i + 8) if i < 16 else 0 for i in range(256))


def read_raw(path):
    with open(path, "rb") as f:
        b = f.read()
    if b[:6] != b"LEMRW2":
        raise SystemExit(f"{path}: not a LEMRW2 file")
    w, h = struct.unpack_from("<HH", b, 6)
    return w, h, b[58:]


def read_scrn(path):
    with open(path, "rb") as f:
        b = f.read()
    w, h = struct.unpack_from("<HH", b, 4)
    return w, h, b[8 + 768:]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ref", required=True, help="directory of .scrn frames")
    ap.add_argument("--dir", required=True, help="directory of the game's .raw frames")
    ap.add_argument("--every", type=int, default=1)
    a = ap.parse_args()

    refs = sorted(f for f in os.listdir(a.ref) if f.endswith(".scrn"))
    if not refs:
        raise SystemExit(f"{a.ref}: no frames - that is a failure, not a pass")

    n = 0
    cur = 0
    play_exact = 0
    panel_exact = 0
    worst_play = (-1, None)
    worst_panel = (-1, None)
    for i in range(0, len(refs), a.every):
        g = os.path.join(a.dir, f"f{i:05d}.raw")
        if not os.path.isfile(g):
            break
        gw, gh, gp = read_raw(g)
        rw, rh, rp = read_scrn(os.path.join(a.ref, refs[i]))
        p = q = 0
        for y in range(PLAY_H):
            row = gp[y * gw:y * gw + SCREEN_W]
            ref = rp[y * rw:y * rw + SCREEN_W]
            p += sum(1 for k in range(SCREEN_W) if MAP[row[k]] != ref[k])
        for y in range(PLAY_H, SCREEN_H):
            row = gp[y * gw:y * gw + SCREEN_W]
            ref = rp[y * rw:y * rw + SCREEN_W]
            q += sum(1 for k in range(SCREEN_W) if MAP[row[k]] != ref[k])
        n += 1
        if p == 0:
            play_exact += 1
        if p == CURSOR:
            cur += 1
        if q == 0:
            panel_exact += 1
        if p > worst_play[0]:
            worst_play = (p, i)
        if q > worst_panel[0]:
            worst_panel = (q, i)

    if n == 0:
        raise SystemExit(f"{a.dir}: no frames to compare - that is a failure")
    print(f"  over {n} frames of the game's own screen:")
    print(f"    play area  exact on {play_exact}; exactly {CURSOR} differing "
          f"(the cursor) on {cur}; worst {worst_play[0]} at frame {worst_play[1]}")
    print(f"    panel      exact on {panel_exact}; worst {worst_panel[0]} "
          f"at frame {worst_panel[1]}")
    print("    The cursor lands wherever the pointer is and says nothing "
          "about the reconstruction.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
