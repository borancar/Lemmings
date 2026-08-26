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
PANEL_TOP = 160
PANEL_ART_X = 32


def dac(v):
    """The mode's attribute map: a 4-bit video value to a DAC index.

    ONE rule for the whole screen - 0..7 to themselves, 8..15 to 16..23.

    It was two for a while, and that hid a real difference. Terrain was
    compared with an unconditional +8, which happens to be right because
    terrain values are all 8..15, and it was wrong for everything that is
    not terrain. The objects proved it: where the port and the original
    disagreed around the exit, the port's raw 1, 3, 4, 5, 6 and 7 came back
    from the original as 1, 3, 4, 5, 6 and 7 - unmapped - while the checker
    was showing them as 9, 11, 12, 13, 14 and 15. Switching to this rule
    takes the play area from 98.61% to 98.76% without touching the port.

    The panel agrees independently: taking its artwork against a recorded
    frame, 0..6 come back as themselves and 10, 11, 12, 14, 15 come back as
    18, 19, 20, 22, 23. The 8..15 half is inference from the five high values
    that actually occur - 7, 8, 9 and 13 appear in neither.
    """
    return v if v < 8 else v + 8


# The panel is the same map; kept as a name so callers read clearly.
panel_dac = dac


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
    """Where the cursor is inside `bad`, or None.

    This used to demand that `bad` be EXACTLY the cursor, which was true only
    while the port drew nothing that moved. Once the lemmings matched, the
    residue became cursor plus the exit's flames and the check silently
    stopped firing - so a known artefact went back to being counted against
    the reconstruction. It looks for the pattern as a SUBSET now, and the
    caller subtracts it.
    """
    if not bad:
        return None
    pts = set(bad)
    want0 = [(c, r) for r, row in enumerate(CURSOR)
             for c, ch in enumerate(row) if ch == "#"]
    # The pattern's own top-left corner is BLANK, so it is never one of the
    # differing pixels and cannot be used as a candidate origin. Anchor on the
    # first pixel the pattern actually sets instead.
    ac, ar = want0[0]
    for px, py in sorted(pts):
        x0, y0 = px - ac, py - ar
        if all((x0 + c, y0 + r) in pts for c, r in want0):
            return (x0, y0)
    return None


def cursor_pixels(at):
    x0, y0 = at
    return {(x0 + c, y0 + r) for r, row in enumerate(CURSOR)
            for c, ch in enumerate(row) if ch == "#"}


def play_diff(cap, cw, lev, lw, lh, scroll):
    bad = []
    for y in range(min(PLAY_H, lh)):
        for x in range(cw):
            cx = scroll + x
            v = lev[y * lw + cx] if cx < lw else 0
            if dac(v) != cap[y * cw + x]:
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
    rest = bad
    if cur:
        pix = cursor_pixels(cur)
        rest = [p for p in bad if p not in pix]
        print(f"    {len(pix)} of them are the MOUSE CURSOR, the game's own"
              f" 14x14 dithered\n    cross-hair, top-left {cur}. It lands"
              f" wherever the pointer is, so it\n    says nothing about the"
              f" reconstruction.")
        print(f"    without it: {100*(1-len(rest)/tot):.2f}%"
              f"  ({len(rest)} of {tot} differ)")
    if rest:
        xs = [p[0] for p in rest]
        ys = [p[1] for p in rest]
        print(f"    what is left: box x {min(xs)}..{max(xs)}"
              f"  y {min(ys)}..{max(ys)}")
    elif cur:
        print("    Everything else in the play area matches exactly.")

    if a.panel:
        pw, ph, pan = read_raw(a.panel)
        # The port writes the panel at its logical width. The screen is the
        # window from PANEL_ART_X on - measured: the status line's own cell 0
        # is 32 pixels off the left edge, which is why the artwork lines up at
        # x=0 while the text does not.
        x0 = PANEL_ART_X if pw >= cw + PANEL_ART_X else 0
        n = bad_p = 0
        for y in range(ph):
            cy = PANEL_TOP + y
            if cy >= ch:
                break
            for x in range(min(pw - x0, cw)):
                n += 1
                if panel_dac(pan[y * pw + x0 + x]) != cap[cy * cw + x]:
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
            return rgb(dac(v))
        emit("original", lambda x, y: rgb(cap[y * cw + x]))
        emit("port", port)
        emit("diff", lambda x, y: (255, 0, 255) if (x, y) in bset
             else tuple(c // 4 for c in port(x, y)))
        for s in ("original", "port", "diff"):
            print(f"    {a.png}-{s}.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
