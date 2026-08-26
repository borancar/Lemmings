"""Score the port against a whole recorded run, frame by frame.

tools/compare_frame.py answers "how close is the port on THIS frame".  The
question that matters for a playthrough is how close it stays over hundreds of
them, because a port can match the first frame of a level exactly and diverge
the moment anything moves.

The reference comes from tools/record_level.py, which captures on the game's
own end-of-frame rather than on a clock. The port's side can be either:

  --raw FILE           one static render, scored against every frame - which
                       is what to use while the port has no play loop, and it
                       shows exactly where the original leaves it behind
  --port-dir DIR       f00000.raw, f00001.raw, ... one per frame, for when
                       the port steps too

Play area and panel are scored separately, for the same reason
compare_frame.py does it: they are reconstructed to different degrees and an
average would hide which.

  uv run python tools/compare_run.py --ref out/ref --raw out/lvl_port.raw \\
      --panel out/panel_port.raw

A frame whose palette is still dark is reported but not scored on colour: the
level fades in, and scoring a fade as a failure is how a real effect gets
recorded as a bug.
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from compare_frame import (read_capture, read_raw, play_diff, panel_dac,
                           PLAY_H, PANEL_TOP, PANEL_ART_X)


def score_frame(cap, cw, ch, lev, lw, lh, scroll, pan, pw, ph):
    bad = play_diff(cap, cw, lev, lw, lh, scroll)
    play_bad = len(bad)
    play_tot = cw * PLAY_H
    pan_bad = pan_tot = 0
    if pan is not None:
        x0 = PANEL_ART_X if pw >= cw + PANEL_ART_X else 0
        for y in range(ph):
            cy = PANEL_TOP + y
            if cy >= ch:
                break
            for x in range(min(pw - x0, cw)):
                pan_tot += 1
                if panel_dac(pan[y * pw + x0 + x]) != cap[cy * cw + x]:
                    pan_bad += 1
    return play_bad, play_tot, pan_bad, pan_tot


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ref", default="out/ref")
    ap.add_argument("--raw", default="")
    ap.add_argument("--port-dir", default="")
    ap.add_argument("--panel", default="")
    ap.add_argument("--panel-dir", default="",
                    help="one panel per frame, named like the reference "
                         "frames - for when the clock in it is running")
    ap.add_argument("--scroll", type=int, default=-1,
                    help="fixed scroll; the default fits one on the first "
                         "frame and holds it, so a drift shows up as a score "
                         "falling rather than being fitted away")
    ap.add_argument("--every", type=int, default=1)
    ap.add_argument("--limit", type=int, default=0)
    a = ap.parse_args()

    if not a.raw and not a.port_dir:
        print("  give --raw (one static render) or --port-dir (one per frame)")
        return 2
    names = sorted(n for n in os.listdir(a.ref) if n.endswith(".scrn"))
    if not names:
        print(f"  no frames in {a.ref} - run tools/record_level.py first.")
        return 2
    if a.limit:
        names = names[:a.limit]

    pan = None
    pw = ph = 0
    if a.panel:
        pw, ph, pan = read_raw(a.panel)
    lw = lh = 0
    lev = None
    if a.raw:
        lw, lh, lev = read_raw(a.raw)

    scroll = a.scroll
    rows = []
    for i, n in enumerate(names):
        if i % a.every:
            continue
        cw, ch, pal, cap = read_capture(os.path.join(a.ref, n))
        lit = sum(1 for k in range(0, 768, 3)
                  if pal[k] or pal[k+1] or pal[k+2])
        if a.port_dir:
            p = os.path.join(a.port_dir, n.replace(".scrn", ".raw"))
            if not os.path.exists(p):
                print(f"  {n}: no port frame at {p} - not scored")
                continue
            lw, lh, lev = read_raw(p)
        if a.panel_dir:
            q = os.path.join(a.panel_dir, n.replace(".scrn", ".raw"))
            if os.path.exists(q):
                pw, ph, pan = read_raw(q)
        if scroll < 0:
            best = None
            for s in range(0, max(1, lw - cw), 8):
                d = len(play_diff(cap, cw, lev, lw, lh, s))
                if best is None or d < best[1]:
                    best = (s, d)
            scroll = best[0]
            print(f"  fitted scroll {scroll} on {n}, and held for the rest")
        rows.append((n, lit) + score_frame(cap, cw, ch, lev, lw, lh,
                                           scroll, pan, pw, ph))

    if not rows:
        print("  nothing scored.")
        return 1
    print(f"\n  {'frame':>10} {'lit':>4}  {'play area':>12}  {'panel':>12}")
    step = max(1, len(rows) // 20)
    for k in range(0, len(rows), step):
        n, lit, pb, pt, qb, qt = rows[k]
        pa = f"{100.0*(1-pb/pt):6.2f}%" if pt else "    -  "
        pn = f"{100.0*(1-qb/qt):6.2f}%" if qt else "    -  "
        print(f"  {n:>10} {lit:>4}  {pa:>12}  {pn:>12}")

    def stats(bi, ti):
        vals = [100.0*(1-r[bi]/r[ti]) for r in rows if r[ti]]
        if not vals:
            return None
        return min(vals), sum(vals)/len(vals), max(vals)

    print(f"\n  over {len(rows)} frames:")
    for name, bi, ti in (("play area", 2, 3), ("panel", 4, 5)):
        st = stats(bi, ti)
        if st is None:
            print(f"    {name:10s} not scored (the port was not asked for one)")
        else:
            print(f"    {name:10s} worst {st[0]:6.2f}%   mean {st[1]:6.2f}%"
                  f"   best {st[2]:6.2f}%")
    dark = sum(1 for r in rows if r[1] == 0)
    if dark:
        print(f"    {dark} frame(s) captured with a dark palette - the level's"
              f" fade-in, real and not an error")
    return 0


if __name__ == "__main__":
    sys.exit(main())
