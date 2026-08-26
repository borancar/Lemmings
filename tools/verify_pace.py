"""How long the port's loops take, against the rates the original sets.

Every other check here compares PICTURES. compare_screen.py scores frame N
against frame N and compare_run.py does the same across a whole run, and
neither says anything about how long a frame is meant to LAST - so a port
running at four times speed matches the original perfectly on all 5,100
frames. It did: the level ran at sixty frames a second where the game wants
seventeen, and what caught it was a person watching the clock fall down its
own display.

So this is a wall-clock check, and it is the only one:

    N frames of the level should take N / 17 seconds   (image 0x0418)
    N passes of the menu  should take N / 70 seconds   (image 0x01550, a
                                                        retrace wait)

Both are the port's OWN loops timed on the host, not the guest's - the
emulator's throughput has nothing to do with it and measuring against that
gives an answer between 3 and 8 frames a second, which is the instrument.

    uv run python tools/verify_pace.py
"""
import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LEMDEV = os.path.join(ROOT, "reconstruct", "lemdev")

# The two rates, and where each comes from.
LEVEL_FPS = 17          # image 0x0418: ds:0x85 and ds:0x86 start at 0x11
MENU_HZ = 70            # image 0x01550 waits a retrace; the VGA modes are 70

TOLERANCE = 0.08        # 8%: a scheduler is not a crystal


def timed(args, frames, env=None):
    e = dict(os.environ)
    e["SDL_VIDEODRIVER"] = "dummy"
    e["LEM_FRAMES"] = str(frames)
    e.pop("LEM_SCREEN_DIR", None)     # the sink deliberately runs flat out
    if env:
        e.update(env)
    t = time.perf_counter()
    subprocess.run([LEMDEV] + args, env=e, cwd=ROOT,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return time.perf_counter() - t


def check(what, args, frames, rate):
    want = frames / rate
    got = timed(args, frames)
    off = abs(got - want) / want
    ok = off <= TOLERANCE
    print(f"  {what:26s} {frames:4d} at {rate:3d}/s: want {want:5.2f}s  "
          f"got {got:5.2f}s  ({off*100:4.1f}% off)  {'ok' if ok else 'TOO ' + ('SLOW' if got > want else 'FAST')}")
    return ok


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", default="lemming1.pc")
    a = ap.parse_args()
    if not os.path.isfile(LEMDEV):
        raise SystemExit(f"{LEMDEV}: build it first (make -C reconstruct)")

    print("  the port's own loops, timed on the host:")
    ok = True
    ok &= check("the level", ["--data", a.data, "--rating", "fun",
                              "--number", "1", "--show"], 170, LEVEL_FPS)
    ok &= check("the menu", ["--data", a.data, "--screen", "menu", "--show"],
                350, MENU_HZ)
    if not ok:
        print("\n  A PACE THAT IS WRONG LOOKS PERFECT to every other check\n"
              "  here - they compare pictures, not durations.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
