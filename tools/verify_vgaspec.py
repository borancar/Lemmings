"""The four VGASPEC levels, against the original's own bitmap and palette.

Four of the eighty levels have no terrain pieces. Their header's extended
graphics set (ds:0x0FE8) is non-zero, image 0x03742 sends them to image
0x01A7B instead of the piece loop, and that lays one 960 x 160 picture from
vgaspecN.dat into the master - and writes the file's eight colours over the
level's custom eight on the way. The port refused these levels outright until
render_vgaspec transcribed that routine; this is what checks it.

For each plate it drives the original there (record_level.py, which pokes
ds:0x1F6B) with this file riding along as LEM_EXTRA_HOOK, and dumps the
master bitmap - four planes, 208 x 164 at 0x4A40 - at two moments:

  image 0x0376A  where both composition paths END for the level. Terrain
                 only; scored against `lemdev --no-objects --raw`.
  image 0x07EB2  the briefing preview's packer, which reads the bitmap
                 image 0x0394F composed WITH the objects. Scored against
                 `lemdev --baked --raw`, render_objects being that routine.

and scores the level's palette: the recorded DAC 16..23 (where the attribute
controller sends 8..15) and DAC 7 against the sixteen the port's --raw
carries.

    uv run python tools/verify_vgaspec.py            # all four, ~10 min each
    uv run python tools/verify_vgaspec.py --level 21

A plate whose run never reached a dump says so and FAILS; no dump is not a
pass. Each point is dumped on every hit and the LAST is scored; in the runs
made so far each was hit exactly once.

WHAT THIS DOES NOT SCORE: the BRIEFING'S PALETTE. A VGASPEC level's preview
eight come from image 0x01A7B too, and the port shows them, but this run
clicks through the briefing rather than capturing it lit.
"""
import argparse
import glob
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

BASE, STRIDE, ROWS = 0x4A40, 208, 164       # image 0x0368E: 1664 x 164
CANVAS_W, CANVAS_H, MARGIN = 1648, 160, 16  # the port's canvas, game.h
POINTS = (0x0376A, 0x07EB2)

# ds:0x1F6B -> (lemdev file, lemdev --level, vgaspec file number). Read off
# the level headers through LEVEL_ORDER; the names are the check that both
# sides are on the same level.
PLATES = {
    21:  ("level001.dat", 4, 3),    # Fun 22     "A Beast of a level"
    43:  ("level000.dat", 3, 1),    # Tricky 14  "MENACING !!"
    74:  ("level000.dat", 2, 0),    # Taxing 15  "What an AWESOME level"
    111: ("level000.dat", 5, 2),    # Mayhem 22  "A BeastII of a level"
}


def install(m):
    """The guest side: dump the master at each hit, numbered. Capture only."""
    from unicorn import UC_HOOK_CODE

    out = os.environ["LEM_VGASPEC_OUT"]
    base = m.load_seg * 16
    hits = {p: 0 for p in POINTS}

    def hook(uc, ad, size, _):
        a = ad - base
        hits[a] += 1
        with open(os.path.join(out, "%05X_%d.bin" % (a, hits[a])), "wb") as f:
            for p in range(4):
                f.write(bytes(m.planes[p][BASE:BASE + STRIDE * ROWS]))

    for p in POINTS:
        m.uc.hook_add(UC_HOOK_CODE, hook, None, base + p, base + p)


def chunky(path):
    import numpy as np
    b = np.frombuffer(open(path, "rb").read(), np.uint8).reshape(4, ROWS, STRIDE)
    g = np.zeros((ROWS, STRIDE * 8), np.uint8)
    for p in range(4):
        g |= np.unpackbits(b[p], axis=1) << p
    return g


def read_port(path):
    """LEMRW2: tag, w, h, sixteen RGB triples, then the indices."""
    import numpy as np
    b = open(path, "rb").read()
    if b[:6] != b"LEMRW2":
        raise SystemExit(f"{path}: not a LEMRW2 file")
    pal = [tuple(b[10 + i * 3:13 + i * 3]) for i in range(16)]
    pix = np.frombuffer(b[58:], np.uint8)
    return pal, pix[:CANVAS_W * CANVAS_H].reshape(CANVAS_H, CANVAS_W)


def score(orig_path, port_pix, what):
    g = chunky(orig_path)
    if g[CANVAS_H:].any():
        print(f"    {what}: the original has terrain BELOW row {CANVAS_H}, "
              "which the port's canvas cannot hold")
        return False
    orig = g[:CANVAS_H, MARGIN:MARGIN + CANVAS_W]
    d = int((orig != port_pix).sum())
    ink = int((orig != 0).sum())
    print(f"    {what:<34} {orig.size - d} of {orig.size} agree, {d} differ"
          f"  ({ink} non-zero in the original)")
    if ink == 0:
        print("      the original's bitmap is EMPTY - a capture of nothing, "
              "not a pass")
        return False
    return d == 0


def one(n, frames):
    file, sec, specn = PLATES[n]
    out = os.path.join(ROOT, "out", f"vgaspec_{n}")
    ref = os.path.join(out, "ref")
    os.makedirs(out, exist_ok=True)
    for f in glob.glob(os.path.join(out, "*.bin")):
        os.unlink(f)
    print(f"  ds:0x1F6B = {n}: {file} --level {sec}, vgaspec{specn}.dat")
    env = dict(os.environ, LEM_EXTRA_HOOK=os.path.abspath(__file__),
               LEM_VGASPEC_OUT=out)
    subprocess.run([sys.executable, os.path.join(HERE, "record_level.py"),
                    "--level", str(n), "--frames", str(frames), "--out", ref],
                   cwd=ROOT, env=env, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    ok = True
    last = {}
    for p in POINTS:
        hits = sorted(glob.glob(os.path.join(out, "%05X_*.bin" % p)),
                      key=lambda s: int(s.rsplit("_", 1)[1].split(".")[0]))
        if not hits:
            print(f"    NEVER REACHED image 0x{p:05X} - a failed run, not a pass")
            ok = False
        else:
            last[p] = hits[-1]
    if not ok:
        return False

    lemdev = os.path.join(ROOT, "reconstruct", "lemdev")
    raws = {}
    for flag in ("--no-objects", "--baked"):
        raw = os.path.join(out, f"port{flag.replace('-', '_')}.raw")
        r = subprocess.run([lemdev, "--data", "lemming1.pc", "--file", file,
                            "--level", str(sec), flag, "--raw", raw],
                           cwd=ROOT, capture_output=True, text=True)
        if r.returncode != 0 or "VGASPEC level" not in r.stdout:
            print(f"    the port did not take the VGASPEC path for {flag}:")
            print("      " + (r.stderr or r.stdout).strip().replace("\n", "\n      "))
            return False
        raws[flag] = read_port(raw)

    ok &= score(last[0x0376A], raws["--no-objects"][1],
                "composed, image 0x0376A")
    ok &= score(last[0x07EB2], raws["--baked"][1],
                "with objects, image 0x07EB2")

    # The palette, from a lit frame in the middle of the recorded level.
    frames_ = sorted(glob.glob(os.path.join(ref, "*.scrn")))
    lit = []
    for f in frames_:
        b = open(f, "rb").read()
        pal = [tuple(b[8 + i * 3:11 + i * 3]) for i in range(256)]
        if any(any(c) for c in pal[16:24]):
            lit.append(pal)
    if not lit:
        print("    no recorded frame had the level's palette up - "
              "the palette is NOT scored")
        return False
    pal = lit[len(lit) // 2]
    port_pal = raws["--no-objects"][0]
    want = [pal[7]] + pal[16:24]
    got = [port_pal[7]] + port_pal[8:16]
    agree = sum(1 for a, b in zip(want, got) if a == b)
    print(f"    {'palette, DAC 7 and 16..23':<34} {agree} of 9 agree"
          f"  (of {len(lit)} lit frames, the middle one)")
    if agree != 9:
        print(f"      original {want}\n      port     {got}")
    ok &= agree == 9
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--level", type=int, choices=sorted(PLATES), default=None,
                    help="one plate, by the original's ds:0x1F6B")
    ap.add_argument("--frames", type=int, default=40,
                    help="how much of the level to record - only the palette "
                         "needs any of it")
    a = ap.parse_args()
    r = subprocess.run(["make", "-C", "reconstruct"], cwd=ROOT,
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("the port did not build:\n" + r.stderr)
    plates = [a.level] if a.level is not None else sorted(PLATES)
    bad = [n for n in plates if not one(n, a.frames)]
    print("  every VGASPEC plate checked is exact" if not bad
          else f"  FAILED on {bad}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
