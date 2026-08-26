"""One frame of a level, the original beside the port.

The level is mode 0x0D - 320x200 - and it is two different things stacked:

    rows   0..159   the play area, a window onto the 1584-wide canvas
    rows 160..175   blank
    rows 176..199   the control panel

They are scored SEPARATELY, because they are reconstructed to different
degrees and an average would hide which. That is the same reason the
briefing's preview is scored apart from its text.

The port's canvas carries palette indices 0..15; the capture carries DAC
indices, and for this mode the terrain's map is simply +8 - measured, not
assumed: the value histograms line up 19/11, 18/10, 17/9, 16/8, and 15/23 and
14/22 agree to the exact pixel count.

  uv run python tools/compare_frame.py --capture out/level.scrn \\
      --raw out/lvl_t.raw [--scroll N] [--png out/frame]

With no --scroll it FITS one and says which it used, because the view scrolls
and a frame compared at the wrong offset looks like a broken port.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

PLAY_H  = 160
PANEL_Y = 176
DAC_BIAS = 8            # the mode's attribute -> DAC map for terrain

# The panel is drawn at screen row 160 and does NOT share the terrain's map.
# Measured, not assumed: taking every pixel of the artwork (main.dat section
# 2, 320x40, four plane-major planes) against a recorded frame and asking
# what the original shows at each of the sixteen values, 0..6 come back as
# themselves at 96-100% and 10, 11, 12, 14, 15 come back as 18, 19, 20, 22,
# 23 - so 0..7 map to themselves and 8..15 to 16..23. 7, 8, 9 and 13 never
# occur in the artwork at all, so that half of the rule is inference from the
# five values that do.
PANEL_TOP = 160


def panel_dac(v):
    return v if v < 8 else v + 8


def read_capture(path):
    with open(path, "rb") as f:
        b = f.read()
    w, h = struct.unpack_from("<HH", b, 4)
    return w, h, b[8:8 + 768], b[8 + 768:]


def read_raw(path):
    with open(path, "rb") as f:
        b = f.read()
    if b[:6] != b"LEMRW2":
        raise SystemExit(f"{path}: not a LEMRW2 file")
    w, h = struct.unpack_from("<HH", b, 6)
    return w, h, b[10 + 48:]


# The game draws a 14x14 dithered cross-hair for the mouse, and it lands
# wherever the pointer happens to be - so it pollutes every frame comparison
# with a cursor-shaped blob that has nothing to do with the reconstruction.
# Its rows are: an arm every other row, and two solid rows across the middle
# with the horizontal arm dithered every other pixel.
CURSOR = [
    "......##......", "..............", "......##......", "..............",
    "......##......", "..............", "#.#.#.##.#.#.#", "#.#.#.##.#.#.#",
    "..............", "......##......", "..............", "......##......",
    "..............", "......##......",
]


def cursor_at(bad):
    """If `bad` is exactly the cursor, where its top-left is; else None."""
    if not bad:
        return None
    x0 = min(p[0] for p in bad)
    y0 = min(p[1] for p in bad)
    want = {(x0 + c, y0 + r)
            for r, row in enumerate(CURSOR)
            for c, ch in enumerate(row) if ch == "#"}
    return (x0, y0) if want == set(bad) else None


def play_diff(cap, cw, lev, lw, lh, scroll):
    bad = []
    for y in range(min(PLAY_H, lh)):
        for x in range(cw):
            cx = scroll + x
            v = lev[y * lw + cx] if cx < lw else 0
            got = v + DAC_BIAS if v else 0
            if got != cap[y * cw + x]:
                bad.append((x, y))
    return bad


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--capture", required=True)
    ap.add_argument("--raw", required=True)
    ap.add_argument("--scroll", type=int, default=-1)
    ap.add_argument("--panel", default="",
                    help="the port's 320x40 panel, from"
                         " lemdev --panel")
    ap.add_argument("--png", default="", help="write three images to PREFIX")
    a = ap.parse_args()

    cw, ch, dac, cap = read_capture(a.capture)
    lw, lh, lev = read_raw(a.raw)
    if ch < PANEL_Y:
        raise SystemExit(f"{a.capture}: {cw}x{ch} is not a mode 0x0D frame")

    # IS THE PALETTE UP? A screen sampled mid-fade has an all-black DAC with
    # every index still in place, so the index comparison below stays perfectly
    # valid while the pictures come out solid black - which reads as a broken
    # port rather than a bad capture. CLAUDE.md has the two occasions that cost
    # here; this makes it three, and now it is checked.
    lit = sum(1 for i in range(256) if any(dac[i*3:i*3+3]))
    if lit == 0:
        print("  THE DAC READS ALL BLACK in this capture. Mid-fade is the\n"
              "  usual cause and was the first guess here, but captures taken\n"
              "  sixteen seconds apart are both black, so that is NOT\n"
              "  established - mode 0x0D may set its colours somewhere this\n"
              "  capture path does not read. Either way the INDEX comparison\n"
              "  below is sound, and the images are drawn in greys by index\n"
              "  so nothing here can be mistaken for the original's colours.")

    scroll = a.scroll
    if scroll < 0:
        best = None
        for s in range(0, lw - cw, 8):
            n = len(play_diff(cap, cw, lev, lw, lh, s))
            if best is None or n < best[1]:
                best = (s, n)
        for s in range(max(0, best[0] - 8), best[0] + 9):
            n = len(play_diff(cap, cw, lev, lw, lh, s))
            if n < best[1]:
                best = (s, n)
        scroll = best[0]
        print(f"  fitted scroll {scroll}")
    bad = play_diff(cap, cw, lev, lw, lh, scroll)
    tot = cw * PLAY_H
    print(f"  play area, rows 0..{PLAY_H-1} : "
          f"{100*(1-len(bad)/tot):.2f}%  ({len(bad)} of {tot} differ)")
    cur = cursor_at(bad)
    if cur:
        print(f"    ALL of them are the MOUSE CURSOR - the game's own 14x14"
              f" dithered\n    cross-hair, here with its top-left at {cur}."
              f" It lands wherever the\n    pointer is, so it says nothing"
              f" about the reconstruction. Everything\n    else in the play"
              f" area matches exactly.")
    elif bad:
        xs = [p[0] for p in bad]
        ys = [p[1] for p in bad]
        print(f"    differing box x {min(xs)}..{max(xs)}  y {min(ys)}..{max(ys)}")

    if a.panel:
        pw, ph, pan = read_raw(a.panel)
        n = bad_p = 0
        for y in range(ph):
            cy = PANEL_TOP + y
            if cy >= ch:
                break
            for x in range(min(pw, cw)):
                n += 1
                if panel_dac(pan[y * pw + x]) != cap[cy * cw + x]:
                    bad_p += 1
        print(f"  panel, rows {PANEL_TOP}..{PANEL_TOP + ph - 1} : "
              f"{100.0 * (1 - bad_p / n):.2f}%  ({bad_p} of {n} differ)")
        print("    what is left is drawn ON the artwork - the status counts,"
              " the minimap,\n    the selected button - by routines not read"
              " yet, not by the artwork.")
    else:
        panel = sum(1 for y in range(PANEL_Y, ch) for x in range(cw)
                    if cap[y * cw + x])
        print(f"  panel, rows {PANEL_Y}..{ch-1}   : "
              f"{panel} pixels of ink in the original, and the port was not"
              f" asked for one (--panel FILE)")

    if a.png:
        from diff_png import write_png     # noqa: E402
        def emit(name, pick):
            rows = []
            for y in range(ch):
                r = bytearray()
                for x in range(cw):
                    r += bytes(pick(x, y))
                rows.append(r)
            write_png(f"{a.png}-{name}.png", cw, ch, rows)
        if lit:
            rgb = lambda v: (dac[v*3], dac[v*3+1], dac[v*3+2])
        else:
            # A stand-in so the shapes can be seen at all. Greys by index, so
            # nothing here can be mistaken for the original's colours.
            rgb = lambda v: ((v * 11) % 256,) * 3
        bset = set(bad)
        def port(x, y):
            if y >= PLAY_H:
                return (0, 0, 0)
            cx = scroll + x
            v = lev[y * lw + cx] if cx < lw else 0
            return rgb(v + DAC_BIAS) if v else (0, 0, 0)
        emit("original", lambda x, y: rgb(cap[y * cw + x]))
        emit("port", port)
        emit("diff", lambda x, y: (255, 0, 255) if (x, y) in bset
             else tuple(c // 4 for c in port(x, y)))
        for s in ("original", "port", "diff"):
            print(f"    {a.png}-{s}.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
