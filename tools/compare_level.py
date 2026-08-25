# SPDX-License-Identifier: GPL-2.0-only
"""Check the port's level rendering against the original's video memory.

This is the check the reconstruction rests on. Everything else - that the
level name matches, that the briefing screen's numbers agree - compares one
reading of a document against another. This compares two programs' pixels.

  # capture the original's screen (control socket verb `screen`)
  uv run python tools/play.py --auto --start --dump-vga --dump-delay 60 \
      --dump-screen out/screen.bin -- --run-seconds 210 --shots 1

  # render the same level with the port, as raw palette indices
  reconstruct/lemdev --data lemming1.pc --raw out/level.raw

  uv run python tools/compare_level.py out/screen.bin out/level.raw \
      --report out/compare.png

The original shows a 320x160 viewport onto the level with its control panel
below, scrolled to wherever the game put it. The port renders the whole
1584x160 level. So this slides the port's canvas across the original's
viewport and reports the best alignment: a correct renderer matches at exactly
one offset, and that offset is itself a result.
"""
import argparse
import struct
import numpy as np


def _dac8(v):
    """6-bit DAC to 8 bits by bit replication, as the hardware does."""
    v &= 0x3F
    return (v << 2) | (v >> 4)


def ground_palette(path):
    """The 16 colours the port draws a level in, from GROUNDxO.DAT."""
    buf = open(path, "rb").read()
    custom, standard = buf[0x3D8:0x3F0], buf[0x3F0:0x408]
    pal = np.zeros((16, 3), dtype=np.uint8)
    for i in range(8):
        for c in range(3):
            pal[i][c] = _dac8(standard[i * 3 + c])
            pal[8 + i][c] = _dac8(custom[i * 3 + c])
    return pal


def load_screen(path):
    """The original's screen, from the control socket's `screen` verb."""
    with open(path, "rb") as f:
        if f.read(4) != b"SCRN":
            raise SystemExit(f"{path}: not a SCRN file")
        w, h = struct.unpack("<HH", f.read(4))
        pal = np.frombuffer(f.read(768), dtype=np.uint8).reshape(256, 3)
        idx = np.frombuffer(f.read(w * h), dtype=np.uint8).reshape(h, w)
    return idx, pal, w, h


def load_raw(path):
    """The port's output: magic, width, height, 16 RGB triples, indices.

    The palette comes from the file rather than being re-derived here on
    purpose. A checker that computes the port's palette itself is comparing
    its own arithmetic against itself, and would have agreed with a port that
    displays something else entirely.
    """
    with open(path, "rb") as f:
        magic = f.read(6)
        if magic not in (b"LEMRAW", b"LEMRW2"):
            raise SystemExit(f"{path}: not a LEMRAW/LEMRW2 file")
        w, h = struct.unpack("<HH", f.read(4))
        pal = None
        if magic == b"LEMRW2":
            pal = np.frombuffer(f.read(48), dtype=np.uint8).reshape(16, 3)
        pix = np.frombuffer(f.read(w * h), dtype=np.uint8)
    return pix.reshape(h, w), pal, w, h


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture", help="the original's screen (.bin, `screen` verb)")
    ap.add_argument("raw", help="the port's --raw output")
    ap.add_argument("--rows", type=int, default=160,
                    help="rows of the original that are level, not panel")
    ap.add_argument("--confusion", action="store_true",
                    help="print which port index landed on which original one")
    ap.add_argument("--dy", type=int, default=8,
                    help="vertical offsets to try, plus and minus")
    ap.add_argument("--ground",
                    default="/home/boran/git/Lemmings/lemming1.pc/ground0o.dat")
    ap.add_argument("--report", default="",
                    help="write a stacked PNG of the best alignment")
    a = ap.parse_args()

    oidx, opal, ow, oh = load_screen(a.capture)

    orig_rgb = opal[oidx]

    # Refuse a degenerate capture, judged on COLOUR rather than on index.
    # Both halves of that matter and the second one bit:
    #
    #   A screen caught before the level is drawn is nearly all one index, and
    #   the left edge of a level is nearly all background, so the two line up
    #   at 100% and the check reports a perfect match for two blank images.
    #
    #   Worse, a screen caught mid-FADE has a perfectly varied set of indices
    #   and a palette that is entirely black. Checking the indices passes it;
    #   every pixel still renders [0,0,0], the port's empty left edge matches
    #   it exactly, and the run prints EXACT MATCH. That happened here, and it
    #   is the reason this test looks at the rendered colours.
    view_rgb = orig_rgb[:a.rows]
    flat = (view_rgb[..., 0].astype(np.uint32) << 16
            | view_rgb[..., 1].astype(np.uint32) << 8
            | view_rgb[..., 2].astype(np.uint32))
    counts = np.bincount(flat.ravel())
    dominant = counts.max() / flat.size
    distinct = int(np.count_nonzero(counts))
    if dominant > 0.9 or distinct < 4:
        raise SystemExit(
            f"{a.capture}: the captured viewport renders as {dominant*100:.1f}%"
            f" one colour across {distinct} distinct colour(s) - it was caught"
            f" blank or mid-fade, and comparing against it would be"
            f" meaningless. Capture again when the palette is up.")
    port_idx, port_pal, pw, ph = load_raw(a.raw)
    if port_pal is None:
        port_pal = ground_palette(a.ground)
    port_rgb = port_pal[port_idx]

    view = orig_rgb[:a.rows]
    vh, vw, _ = view.shape
    if port_rgb.shape[0] < vh:
        raise SystemExit("the port's canvas is shorter than the viewport")

    # Search vertically as well as horizontally. The original's viewport need
    # not start at the level's row 0, and a one-row slip looks exactly like a
    # renderer that is subtly wrong - scattered differences everywhere - so
    # ruling it out is worth the extra loop.
    best = None
    for dy in range(-a.dy, a.dy + 1):
        lo = max(0, dy)
        hi = min(vh, ph + dy)
        if hi - lo < vh // 2:
            continue
        for off in range(0, pw - vw + 1):
            window = port_rgb[lo - dy:hi - dy, off:off + vw]
            diff = int(np.count_nonzero(
                np.any(window != view[lo:hi], axis=2)))
            diff += (vh - (hi - lo)) * vw       # rows with no counterpart
            if best is None or diff < best[2]:
                best = (off, dy, diff)

    off, dy, diff = best
    lo = max(0, dy)
    hi = min(vh, ph + dy)
    total = vh * vw
    print(f"viewport         : {vw}x{vh} = {total} pixels")
    print(f"best alignment   : x = {off}, y = {dy}")
    print(f"pixels matching  : {total - diff}  "
          f"({100.0 * (total - diff) / total:.2f}%)")
    print(f"pixels differing : {diff}  ({100.0 * diff / total:.2f}%)")

    window = np.zeros_like(view)
    window[lo:hi] = port_rgb[lo - dy:hi - dy, off:off + vw]
    if diff == 0:
        print("\nEXACT MATCH.")
    else:
        bad = np.any(window != view, axis=2)
        rows = np.flatnonzero(bad.any(axis=1))
        cols = np.flatnonzero(bad.any(axis=0))
        print(f"\ndiffering rows   : {rows.min()}..{rows.max()} "
              f"({len(rows)} of {vh})")
        print(f"differing cols   : {cols.min()}..{cols.max()} "
              f"({len(cols)} of {vw})")
        print("\nNOTE: the original's screen has lemmings, the cursor and any\n"
              "      terrain they have already dug through on it; the port\n"
              "      draws the level as loaded. Some difference is expected\n"
              "      and only its shape and size say whether it is only that.")
    if a.confusion:
        # Which index the port produced where the original had another. If the
        # renderer had, say, two bitplanes swapped, this comes out as a clean
        # permutation rather than a smear - which tells the two apart without
        # guessing.
        attr = [0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17]
        back = {d: i for i, d in enumerate(attr)}
        oi = np.full(oidx.shape, -1, dtype=np.int16)
        for d, i in back.items():
            oi[oidx == d] = i
        o = oi[lo:hi]
        pp = port_idx[lo - dy:hi - dy, off:off + vw].astype(np.int16)
        print("\nconfusion (rows = original index, cols = port index), "
              "counts in the viewport:")
        header = "      " + "".join(f"{c:>7}" for c in range(16))
        print(header)
        for r in range(16):
            row = [int(np.count_nonzero((o == r) & (pp == c)))
                   for c in range(16)]
            if not any(row):
                continue
            print(f"  {r:>3} " + "".join(f"{v:>7}" if v else "      ."
                                         for v in row))
        bad = int(np.count_nonzero(o < 0))
        if bad:
            print(f"  ({bad} original pixels used a DAC entry outside the "
                  f"mode 0Dh attribute map)")

    if a.report:
        from PIL import Image
        gap = np.zeros((4, vw, 3), dtype=np.uint8)
        gap[:] = (255, 0, 0)
        mask = np.any(window != view, axis=2)
        diffimg = np.zeros_like(view)
        diffimg[mask] = (255, 0, 255)          # magenta where they disagree
        Image.fromarray(np.concatenate(
            [view, gap, window, gap, diffimg], axis=0)).save(a.report)
        print(f"\nwrote {a.report} (original above, port below)")


if __name__ == "__main__":
    main()
