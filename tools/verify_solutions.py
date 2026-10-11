"""Every level played to a win, the port against the original's own run.

    uv run python tools/verify_solutions.py               # every solution
    uv run python tools/verify_solutions.py fun01 fun02   # some of them
    uv run python tools/verify_solutions.py --record fun03

A SOLUTION is the input script tools/lockstep.py --in-order keeps when a level
is won: solutions/<rating><NN>.txt, in the format record_level.py and the
port's harness.c both replay. Its REFERENCE is the original's run of it -
the screens record_level.py captures at image 0x018F and the data segment
tools/dg_hook.py takes at image 0x001C8 - in out/solutions/<name>/orig. A
won lockstep session leaves that behind already; --record makes it again
from the script, and a solution with no reference is recorded first.

Each level is then replayed through the port and scored TWICE:

  the screen       tools/compare_screen.py, play area and panel separately
  the data segment tools/verify_dgroup.py's compare(): every field dgroup.h
                   asserts, the lemming array record by record - which is
                   the state no picture shows

A level the port finishes differently will still score on both, frame by
frame, until the two runs part - and the first frame they part on is the
thing to look at, not the totals.

--live DIR scores a lockstep session's own two recordings (DIR/orig against
DIR/port) without running anything, which is what lockstep.py does the
moment a level is won.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import verify_dgroup  # noqa: E402

SOLUTIONS = os.path.join(ROOT, "solutions")
REFS = os.path.join(ROOT, "out", "solutions")
RATINGS = ("fun", "tricky", "taxing", "mayhem")
DG_END = verify_dgroup.DG_END


def level_of(name):
    m = re.fullmatch(r"(fun|tricky|taxing|mayhem)(\d\d)", name)
    if not m or not 1 <= int(m.group(2)) <= 30:
        raise SystemExit(f"{name}: not a level name like fun01")
    return RATINGS.index(m.group(1)), int(m.group(2))


def frames_in(script):
    """How many frames the original ran with this script: lockstep.py writes
    `# frame N` after each frame's lines, so the last one is the length."""
    last = -1
    for line in open(script):
        m = re.match(r"# frame (\d+)", line)
        if m:
            last = int(m.group(1))
    if last < 0:
        raise SystemExit(f"{script}: no `# frame N` lines - not a lockstep "
                         "recording, so its length is unknown")
    return last + 1


def screens(ref_dir, port_dir):
    r = subprocess.run([sys.executable, os.path.join(HERE, "compare_screen.py"),
                        "--ref", ref_dir, "--dir", port_dir],
                       cwd=ROOT, capture_output=True, text=True)
    sys.stdout.write(r.stdout + r.stderr)
    return r.returncode


def score(orig_dir, port_dir):
    """Both comparisons; returns 0 when the screens ran and nothing in the
    data segment differs unexplained."""
    print("  -- the screen --")
    bad = screens(orig_dir, port_dir)
    print("  -- the data segment --")
    try:
        o = open(os.path.join(orig_dir, "dg.bin"), "rb").read()
        p = open(os.path.join(port_dir, "dg.bin"), "rb").read()
    except OSError as e:
        print(f"   NOT SCORED: {e}")
        return 1
    print(f"   original {len(o) // DG_END} frames, port {len(p) // DG_END}")
    if len(o) > len(p):
        # The level's end in both is image 0x002B3, then the FADE: the
        # original runs on through it, and lemdev, which answers 0 to
        # ds:0x200E so a capture does not spend wall clock fading (see
        # verify_dgroup.py's level_phase), leaves after two frames. Those
        # frames are not scored, and that is said rather than hidden.
        print(f"   the original's last {(len(o) - len(p)) // DG_END} frames "
              "are its fade-out, which this harness skips - not scored")
    return bad or verify_dgroup.compare(o, p, scripted=True)


def record(name, script, n):
    """The original's run of the script, as a won lockstep session leaves it."""
    rating, number = level_of(name)
    orig = os.path.join(REFS, name, "orig")
    os.makedirs(orig, exist_ok=True)
    dg = os.path.join(orig, "dg.bin")
    if os.path.exists(dg):
        os.unlink(dg)       # a stale capture reads as a successful run
    print(f"  recording the original: {n} frames")
    r = subprocess.run(
        [sys.executable, os.path.join(HERE, "record_level.py"),
         "--level", str(rating * 30 + number - 1), "--frames", str(n),
         "--script", script, "--out", orig],
        cwd=ROOT, env=dict(os.environ,
                           LEM_EXTRA_HOOK=os.path.join(HERE, "dg_hook.py"),
                           LEM_DG_OUT=dg, LEM_DG_FRAMES=str(n)))
    got = len([f for f in os.listdir(orig) if f.endswith(".scrn")])
    if r.returncode != 0 or got < n:
        print(f"  THE RECORDING IS SHORT: {got} of {n} frames - what follows "
              "scores only those")
    return orig


def replay(name, script, n):
    """The port's run of the same script."""
    rating, number = level_of(name)
    port = os.path.join(REFS, name, "replay")
    shutil.rmtree(port, ignore_errors=True)
    os.makedirs(port)
    subprocess.run(
        [os.path.join(ROOT, "reconstruct", "lemdev"), "--data", "lemming1.pc",
         "--rating", RATINGS[rating], "--number", str(number), "--show"],
        cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        env=dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
                 LEM_INPUT_SCRIPT=script, LEM_FRAMES=str(n + 2),
                 LEM_SCREEN_DIR=port,
                 LEM_DG_FILE=os.path.join(port, "dg.bin")))
    return port


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("levels", nargs="*",
                    help="level names like fun01; default every solution")
    ap.add_argument("--record", action="store_true",
                    help="record the original's run again from the script")
    ap.add_argument("--live", default=None,
                    help="score a lockstep session's own orig/ and port/")
    a = ap.parse_args()

    if a.live:
        return score(os.path.join(a.live, "orig"), os.path.join(a.live, "port"))

    r = subprocess.run(["make", "-C", os.path.join(ROOT, "reconstruct")],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        return r.returncode

    names = a.levels
    if not names and os.path.isdir(SOLUTIONS):
        names = sorted(f[:-4] for f in os.listdir(SOLUTIONS)
                       if f.endswith(".txt"))
    if not names:
        print("  no solutions yet - play some with "
              "`uv run python tools/lockstep.py --in-order`")
        return 1
    worst = 0
    for name in names:
        script = os.path.join(SOLUTIONS, name + ".txt")
        if not os.path.exists(script):
            print(f"\n== {name}: NO SOLUTION at {os.path.relpath(script, ROOT)}")
            worst = 1
            continue
        n = frames_in(script)
        print(f"\n== {name}: {n} frames ==")
        orig = os.path.join(REFS, name, "orig")
        if a.record or not os.path.exists(os.path.join(orig, "dg.bin")):
            record(name, script, n)
        port = replay(name, script, n)
        worst = max(worst, 1 if score(orig, port) else 0)
    return worst


if __name__ == "__main__":
    sys.exit(main())
