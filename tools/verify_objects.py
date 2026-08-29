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

AND IT IS FLAKY, SO RETRY BEFORE DIAGNOSING. `play.py` presses F1 with
`--start-tries 3` and CLAUDE.md says a script tuned on one run misses on the
next; this check drives it and inherits that. Three failures and three passes
were once read here as evidence that a change to `probe_pvsrc.py` had broken
it, and the change was innocent - a copy of it carrying the same block
installed and captured normally when driven directly. Counting runs cannot
separate a flaky check from a broken one.

When it reports NEVER REACHED, look at whether the HOOK installed before
anything else. `lemmings.py` prints `[hook] installed <path>` and this file's
own output filter drops that line, so a probe that never loaded and a probe
that loaded and found nothing look identical here. They are not.
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
    baked = subprocess.run([os.path.join(ROOT, "reconstruct", "lemdev"),
                            "--data", "lemming1.pc", "--baked", "--raw", raw],
                           cwd=ROOT, capture_output=True, text=True)

    # AND THE SAME LEVEL WITHOUT THEM, so the object pixels can be scored on
    # their own. The whole bitmap is 243,936 pixels and barely 800 of them are
    # an object, so "everything agrees" would still read as a pass with
    # render_objects broken outright - a 0.3% fall. That is the shape of
    # failure this file has hit three times in three passes: a plausible
    # number from a check that never established it exercised the thing it
    # names. Differencing the two renders gives the footprint for free.
    bare = os.path.join(ROOT, "out", "vo_port_bare.raw")
    subprocess.run([os.path.join(ROOT, "reconstruct", "lemdev"),
                    "--data", "lemming1.pc", "--no-objects", "--raw", bare],
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
    bw, bh, bare_pix = read_raw(bare)
    diff = same = 0
    odiff = osame = 0
    first = []
    for y in range(6, min(ROWS, h)):
        row = pix[y * w:(y + 1) * w]
        brow = bare_pix[y * bw:(y + 1) * bw]
        for x in range(MARGIN, MARGIN + w):
            ok = guest(x, y) == row[x - MARGIN]
            if ok:
                same += 1
            else:
                diff += 1
                if len(first) < 6:
                    first.append((x, y))
            if row[x - MARGIN] != brow[x - MARGIN]:
                if ok:
                    osame += 1
                else:
                    odiff += 1
    total = same + diff
    otot = osame + odiff
    print(f"  the composed level WITH objects, against the original's own "
          f"bitmap at image 0x07EB2:")
    print(f"    whole bitmap    {same} of {total} agree, {diff} differ"
          f"  ({100.0 * same / total:.4f}%)")
    if otot:
        print(f"    OBJECT pixels   {osame} of {otot} agree, {odiff} differ"
              f"  ({100.0 * osame / otot:.4f}%)")
    else:
        print("    OBJECT pixels   NONE - the two renders are identical, so "
              "this run scored no object at all and the figure above is "
              "terrain. That is a failed check, not a pass.")
        return 2
    if diff:
        print(f"    first at level {first}")

    # AND WHAT THIS RUN ACTUALLY EXERCISED. The level is the game's own
    # first, so the score above is a claim about whatever objects "Just dig!"
    # happens to carry and about nothing else. render_objects has rules that
    # only some objects reach - no-overwrite, only-on-terrain, and the
    # constant colour an only-on-terrain blit writes - and a level without
    # one of them scores it as neither passing nor failing.
    #
    # Printing it costs a line and stops this reading as a check of the
    # routine rather than of one level's worth of it. tools/coverage.py makes
    # the same point across the frame-by-frame suite.
    line = ""
    for ln in (baked.stdout or "").splitlines():
        if "object effects" in ln:
            line = ln.strip()
            break
    if line:
        body = line.split(":", 1)[1]
        ids = sorted({t.split("=")[0][t.split("=")[0].index("id"):]
                      for t in body.split() if "id" in t})
        print("    this level carried  %s" % " ".join(ids))
        for flag, letter, what in (("no-overwrite", "N", "image 0x09595"),
                                   ("only-on-terrain", "T", "image 0x09709")):
            n = sum(1 for t in body.split() if t.endswith(letter))
            if n:
                print("    %-16s %d object(s), so %s was exercised"
                      % (flag, n, what))
            else:
                print("    %-16s NOT PRESENT - this run says nothing about %s"
                      % (flag, what))
    else:
        print("    could not read the level's object list, so this run "
              "cannot say what it exercised")
    return 1 if diff else 0


if __name__ == "__main__":
    sys.exit(main())
