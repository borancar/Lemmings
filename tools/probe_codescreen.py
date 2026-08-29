# SPDX-License-Identifier: GPL-2.0-only
"""Capture the original's code screen AFTER a code has been typed.

tools/verify_menu.py --screen code scores the screen as it opens: a prompt and
ten dots. What it cannot reach is the state that follows Enter - image 0x0499E
draws the level and the rating it named, image 0x049EC draws "Incorrect Code" -
and that is the half the port got wrong, drawing both over the input field
instead of below it.

So type one. F2 to the screen, ten letters, Enter, then dump.

    uv run python tools/probe_codescreen.py --code MDLCAJNNCT
    uv run python tools/probe_codescreen.py --code AAAAAAAAAA   # refused

The keys go through the control socket a letter at a time and HELD, for the
reason verify_menu.py gives about the rating: the game reads a key-state array
its own INT 9 fills, and a press between two of its samples is never seen.

The capture is named after the code, not after the screen. A fixed output path
is a shared global, and this repository has already taken three measurements
of one level against another level's dump.
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# ds:0x4DF2, the ten cells - read back before Enter so a run that typed
# nothing says so instead of capturing a screen and calling it a result.
CELLS = 0x0E882


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--code", default="MDLCAJNNCT",
                    help="ten letters to type (default: Fun 13)")
    ap.add_argument("--out", default="",
                    help="where to write the capture")
    ap.add_argument("--seconds", type=int, default=240)
    ap.add_argument("--hold", type=int, default=6,
                    help="frames to hold each letter; long enough to be "
                         "sampled, short enough not to repeat")
    ap.add_argument("--delay", type=int, default=10,
                    help="frames to let the menu settle after mode 0x10")
    a = ap.parse_args()

    code = a.code.upper()
    if len(code) != 10 or not code.isalpha():
        sys.exit("a code is ten letters")
    out = a.out or os.path.join(ROOT, "out", f"code_{code}.scrn")
    if os.path.exists(out):
        os.unlink(out)          # never compare against yesterday's

    # WAIT FOR THE GUEST'S OWN CUE, not a sleep. --dump-when 0x10 holds the
    # command list until the game has actually reached mode 0x10, which is the
    # menu; the first version slept three seconds and pressed F2 while the
    # loading screen was still up in mode 0x0D, and reported no capture.
    # verify_menu.py has used this from the start and reaches the code screen
    # every run - copying its recipe beat inventing timings, twice over.
    cmd = [sys.executable, os.path.join(HERE, "play.py"), "--auto",
           "--dump-vga", "--dump-when", "0x10",
           "--dump-delay", str(a.delay),
           "--cmd", "sleep 3.0",
           "--cmd", "key f2 24", "--cmd", "sleep 4.0"]
    for ch in code:
        # A SHORT hold. The game's key read is image 0x04D1A, which counts a
        # key DOWN and fires again when its counter runs out - reload 0x86,
        # fire again at 0x93 - so it auto-repeats. Held 24 frames the way
        # verify_menu holds the rating keys, MDLCAJNNCT arrived as
        # MMMDMMDDLT, and only reading ds:0x4DF2 back showed it.
        cmd += ["--cmd", f"key {ch.lower()} {a.hold}", "--cmd", "sleep 0.8"]
    # AND CAPTURE INSIDE THE WINDOW. A refused code holds its message for
    # image 0x049F6's 0x3C retraces and an accepted one for image 0x049CC's
    # 0x78, and then BOTH go back to the menu at image 0x04401. Sleeping two
    # seconds captured a screen the game had already left.
    cmd += ["--cmd", f"read i+{CELLS:#x} 10",          # what got typed
            # ENTER IS HELD LONG, where the letters are not. A letter that
            # repeats types itself twice; Enter that repeats just presses
            # Enter again, so the two want opposite holds - and at 8 frames
            # it was never sampled at all, which read as "Enter does not
            # work" for two runs. The empty-field test that DID fire used 24.
            "--cmd", "key return 24", "--cmd", "sleep 0.4",
            "--cmd", f"screen {out}",
            "--", "--run-seconds", str(a.seconds)]

    print(f"typing {code} into the original ...")
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    for ln in r.stdout.splitlines():
        low = ln.lower()
        if "error" in low or "unknown key" in low or "mode=" in low:
            print("  " + ln.strip())
    # What the guest actually has in its cells. A key the socket would not
    # send, or one the game's sampling missed, shows up HERE rather than as a
    # screen that looks plausible - which is the trap CLAUDE.md records for
    # the rating sweep.
    hexb = []
    grab = False
    for ln in r.stdout.splitlines():
        if f"read i+{CELLS:#x}" in ln:
            grab = True
            continue
        if grab and ln.strip():
            hexb = [t for t in ln.replace(":", " ").split() if len(t) == 2]
            break
    if hexb:
        typed = "".join(chr(int(t, 16)) if 32 <= int(t, 16) < 127 else "?"
                        for t in hexb[:10])
        print(f"  the guest's cells before Enter: {typed!r}")
        if typed.replace(".", "").replace("_", "") == "":
            sys.exit("  nothing was typed - the socket did not deliver the\n"
                     "  letters, so any capture would be of an empty field")
    if not os.path.exists(out):
        sys.exit("  no capture - the run never reached the code screen, which\n"
                 "  is a failure and not a result")
    print(f"  wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
