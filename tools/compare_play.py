"""The port and the original PLAYING the same level, frame by frame.

Every frame-by-frame number this project had before was taken from a level
nobody touches, and a level nobody touches exercises only the part of the game
that needs no player. The skills, the panel's behaviour, the cursor's box, a
lemming reaching the exit and the level's own ending are all behind a click,
and none of them was ever scored against the original.

So: give BOTH sides the same input at the same frames.

  1. a passive pass, to find where lemming 0 will be - the level is
     deterministic, so a position read at frame F in one run is where it is at
     frame F in the next
  2. an input script, written once and read by both sides
  3. the original recorded with it, tools/record_level.py --script
  4. the port run with it, LEM_INPUT_SCRIPT, writing its own screens
  5. tools/compare_screen.py over the two

    uv run python tools/compare_play.py --frames 1200

THE CURSOR IS NOW PART OF THE COMPARISON, and that is the point rather than a
side effect. compare_screen.py has always said its 28 differing pixels "land
wherever the pointer is and say nothing about the reconstruction" - true when
one side's pointer was parked and the other's was somewhere else. Driven from
one script the two pointers are in the same place, so what the cursor LOOKS
like is scored like anything else, and the box it wears over a lemming with
it.

A run that never assigns the digger has not tested anything this is for, so
it checks: the original's own digger count has to fall, and the tool says
plainly if it did not rather than reporting a happy 100% of a level where
nothing happened.
"""
import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# The digger's panel button. Image 0x03F07 takes the index from
# (mouse_x - 8) >> 4, and the digger is button 9.
DIG_BTN_X, DIG_BTN_Y = 8 + 9 * 16, 190


def positions(frames, out_dir):
    """Pass 1: where lemming 0 walks, with nobody touching it.

    This is an ordinary tools/record_level.py run - deliberately, because its
    manifest counts frames the same way the comparison does. Working the
    positions out from a tool with its own frame origin is how a scripted
    click lands on empty sky."""
    subprocess.run([sys.executable, os.path.join(HERE, "record_level.py"),
                    "--frames", str(frames), "--out", out_dir],
                   cwd=ROOT, stdout=subprocess.DEVNULL)
    man = os.path.join(out_dir, "manifest.json")
    if not os.path.exists(man):
        sys.exit("  pass 1 recorded nothing - it never reached the level")
    return json.load(open(man))


def write_script(rows, at, path):
    """The script both sides read. Aim at lemming 0 as it stands at frame
    `at`, in SCREEN pixels."""
    row = next((r for r in rows
                if r["frame"] >= at and r.get("lem0")), None)
    if row is None:
        sys.exit(f"  no lemming was out at frame {at} - nothing to click")
    lx, ly = row["lem0"]["x"], row["lem0"]["y"]
    hx, hy = row["lem0"]["hot_x"], row["lem0"]["hot_y"]
    scroll = row["scroll"]
    at = row["frame"]
    # AIM AT WHERE THE LEMMING IS DRAWN, not at where the hit test says it
    # can be clicked. Those are the same place if the port is right and
    # different places if it is not, and pointing at the second would confirm
    # the port's arithmetic with the port's arithmetic.
    #
    # The sprite goes down at canvas (x + hot_x - 16, y + hot_y - 16) - the
    # sixteen of image 0x5DC6 - and the canvas is shown from `scroll - 16`,
    # so its top-left lands on screen at (x + hot_x - scroll, y + hot_y - 16).
    # It is about sixteen wide and fourteen tall; aim at the middle.
    sx = lx + hx - scroll + 8
    sy = ly + hy - 16 + 7
    a = {"x": lx, "y": ly}
    lines = [
        "# written by tools/compare_play.py - read by record_level.py --script",
        "# and by the port through LEM_INPUT_SCRIPT.",
        f"# lemming 0 is at level ({a['x']}, {a['y']}), scroll {scroll}",
        "",
        "# pick the digger off the panel, held so the game's own handler",
        "# samples it - see tools/probe_digger.py",
        f"20 move {DIG_BTN_X} {DIG_BTN_Y}",
        "22 down",
        "34 up",
        "",
        "# then the lemming",
        f"{at} move {sx} {sy}",
        f"{at + 2} down",
        f"{at + 14} up",
        "",
    ]
    open(path, "w").write("\n".join(lines))
    print(f"  script -> {path}   (click lemming 0 at screen ({sx}, {sy}) "
          f"on frame {at})")
    return path


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--frames", type=int, default=1200)
    ap.add_argument("--dig-at", type=int, default=110,
                    help="the frame to click a lemming on")
    ap.add_argument("--ref", default=os.path.join(ROOT, "out", "refplay"))
    ap.add_argument("--dir", default=os.path.join(ROOT, "out", "playscreen"))
    ap.add_argument("--script", default=os.path.join(ROOT, "out", "dig.txt"))
    ap.add_argument("--data", default="lemming1.pc")
    ap.add_argument("--rating", default="fun")
    ap.add_argument("--number", type=int, default=1,
                    help='the level the port is pointed at; Fun 1, "Just '
                         'dig!", is the one tools/play.py --start reaches')
    ap.add_argument("--reuse", action="store_true",
                    help="keep pass 1's positions and the script")
    ap.add_argument("--reuse-ref", action="store_true",
                    help="keep the recorded original as well, and only "
                         "re-run the port and the comparison")
    a = ap.parse_args()

    pos = os.path.join(ROOT, "out", "playpos")
    if not (a.reuse and os.path.exists(a.script)):
        print("\n  pass 1: where the lemmings walk, with nobody touching it")
        write_script(positions(a.dig_at + 40, pos), a.dig_at, a.script)

    if a.reuse_ref and os.path.isdir(a.ref) and os.listdir(a.ref):
        print(f"\n  pass 2: SKIPPED, reusing {a.ref}")
        r = None
    else:
        print("\n  pass 2: the ORIGINAL, with that script")
        r = subprocess.run([sys.executable, os.path.join(HERE, "record_level.py"),
                        "--frames", str(a.frames), "--out", a.ref,
                        "--script", a.script], cwd=ROOT)
    if r is not None and r.returncode != 0:
        return r.returncode

    print("\n  pass 3: the PORT, with the same script")
    for d in (a.dir,):
        os.makedirs(d, exist_ok=True)
        for n in os.listdir(d):
            if n.endswith(".raw"):
                os.unlink(os.path.join(d, n))
    env = dict(os.environ, LEM_SCREEN_DIR=a.dir, LEM_FRAMES=str(a.frames),
               LEM_INPUT_SCRIPT=os.path.abspath(a.script),
               SDL_VIDEODRIVER="dummy")
    # lemdev --show, not ./lemmings. Both go through level_show and then the
    # same level_run, so this is still the window path and not a second
    # arrangement of it - but ./lemmings opens on the MENU and would sit
    # there headless writing nothing until the timeout. It did, once.
    subprocess.run([os.path.join(ROOT, "reconstruct", "lemdev"),
                    "--data", a.data, "--rating", a.rating,
                    "--number", str(a.number), "--show"],
                   cwd=ROOT, env=env, stdout=subprocess.DEVNULL)
    if not os.listdir(a.dir):
        sys.exit("  the port wrote no frames - nothing to compare")

    print("\n  pass 4: the two, frame by frame")
    return subprocess.run(
        [sys.executable, os.path.join(HERE, "compare_screen.py"),
         "--ref", a.ref, "--dir", a.dir], cwd=ROOT).returncode


if __name__ == "__main__":
    sys.exit(main())
