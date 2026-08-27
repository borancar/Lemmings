"""The composed level WITH its objects, against the original's own bitmap.

tools/prove.sh compares the composed level at image 0x0376A and gets 100.00%,
and it passes --no-objects because at that instant the objects are not in the
bitmap yet. They are drawn later, by image 0x03981 in a SECOND composition
path at image 0x0394F - so nothing here compared the bitmap once they were.

That gap is why "the original never composes objects into the level bitmap"
stood as a claim for so long: it is true at prove.sh's break and false a few
routines later, and there was no check that could tell.

This closes it. The level bitmap is captured at image 0x07EB2 - the briefing
preview's packer, which reads that bitmap and is hit exactly once - and
compared against the port's canvas rendered WITH objects.

    uv run python tools/verify_objects.py

A run that never reaches the packer reports that rather than a score: no hit
is a failed capture, not a pass.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

STRIDE, ROWS = 208, 163      # what image 0x07EB2's packer walks
MARGIN = 16                  # the bitmap's left margin; the port's canvas x 0


def main():
    out = os.path.join(ROOT, "out", "vo")
    dump = os.path.join(out, "hit1.bin")
    for f in (dump, os.path.join(out, "index.txt")):
        if os.path.exists(f):
            os.unlink(f)
    env = dict(os.environ,
               LEM_HOOK=os.path.join(HERE, "probe_pvsrc.py"),
               LEM_PVSRC_DIR=out)
    subprocess.run([sys.executable, os.path.join(HERE, "play.py"),
                    "--auto", "--start", "--start-delay", "5",
                    "--start-tries", "3", "--", "--run-seconds", "180"],
                   cwd=ROOT, env=env, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    if not os.path.exists(dump):
        print("  NEVER REACHED image 0x07EB2 - no bitmap was captured, so "
              "there is no comparison. That is a failed run, not a pass.")
        return 2

    raw = os.path.join(ROOT, "out", "vo_port.raw")
    # --baked, because the thing being compared is the original's
    # COMPOSITION of the objects into the level bitmap - image 0x03981 - and
    # only render_objects is that. Plain --raw draws them the display-time
    # way, through level_anim_draw and image 0x07BB8, which is a different
    # routine with different rules; comparing that against a composed bitmap
    # scores two things that were never meant to agree.
    #
    # The first version of this file did exactly that, so three rules
    # transcribed into render_objects from image 0x0953E moved the score by
    # nothing - not because they were unverifiable, but because the check did
    # not reach them.
    subprocess.run([os.path.join(ROOT, "reconstruct", "lemdev"),
                    "--data", "lemming1.pc", "--baked", "--raw", raw],
                   cwd=ROOT, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)

    from compare_screen import read_raw
    d = open(dump, "rb").read()
    planes = [d[p * STRIDE * ROWS:(p + 1) * STRIDE * ROWS] for p in range(4)]

    def guest(x, y):
        b = y * STRIDE + (x >> 3)
        k = 7 - (x & 7)
        v = 0
        for p in range(4):
            v |= ((planes[p][b] >> k) & 1) << p
        return v

    w, h, pix = read_raw(raw)
    diff = same = 0
    first = []
    for y in range(6, min(ROWS, h)):
        row = pix[y * w:(y + 1) * w]
        for x in range(MARGIN, MARGIN + w):
            if guest(x, y) == row[x - MARGIN]:
                same += 1
            else:
                diff += 1
                if len(first) < 6:
                    first.append((x, y))
    total = same + diff
    print(f"  the composed level WITH objects, against the original's own "
          f"bitmap at image 0x07EB2:")
    print(f"    {same} of {total} pixels agree, {diff} differ"
          f"  ({100.0 * same / total:.4f}%)")
    if diff:
        print(f"    first at level {first}")
    return 1 if diff else 0


if __name__ == "__main__":
    sys.exit(main())
