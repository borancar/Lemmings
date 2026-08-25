"""What works the rating plate? The Up and Down arrows do.

This used to sweep keys and watch the plate's pixels, and concluded "nothing
found works it". That conclusion was wrong twice over, and both mistakes are
worth keeping in view because they are the ordinary ones:

  * The sweep list never contained "up" or "down" - the two keys the docstring
    claimed had been ruled out. A key that is not in the list is not a key that
    was tried.
  * "minus" WAS delivered and did nothing, which looked like evidence. It is
    not: the rating starts at Fun, and the down path bails at
    `and al,al / jne` when it is already 0. A correct key can look inert.

The answer came from the binary rather than from sweeping. The menu translates
scancodes through a table at ds:0x202E (image 0x0BABE) and dispatches on the
result at image 0x0447B:

    scancode 0x48 (Up)   -> 0x8a -> image 0x044EC   rating up
    scancode 0x29 (`)    -> 0x60 -> image 0x044EC   rating up
    scancode 0x50 (Down) -> 0x8b -> image 0x0451C   rating down
    scancode 0x0c (-)    -> 0x2d -> image 0x0451C   rating down
    scancode 0x4a (KP-)  -> 0x2d -> image 0x0451C   rating down

Both paths are gated on `cmp byte [0x1f6f],4 / jne`; on the menu it reads 0,
so the gate is open. Up caps at 3, down floors at 0, and
both then recompute ds:0x1F6B = rating * 30 - the absolute level number, which
is what fixes the level-order table's four blocks of thirty.

So this probe watches the VARIABLE, not the plate. ds:0x1F6E is the rating; a
pixel box cannot tell "the key did nothing" from "the key worked and the
redraw is elsewhere", and a byte can.

  uv run python tools/probe_rating.py
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lemmings                                            # noqa: F401
from play import ask, wait_for

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

RATING = 0x0B9FE          # image address of ds:0x1F6E, the rating
NUMBER = 0x0B9FB          # image address of ds:0x1F6B, the absolute level
GATE   = 0x0B9FF          # image address of ds:0x1F6F, the gate on both paths
NAMES = ["Fun", "Tricky", "Taxing", "Mayhem"]

# Press each key enough times to reach its limit, and check the TRAJECTORY
# rather than a fixed script. A press can land a poll late - the game reads a
# key-state array its own INT 9 fills, so a hold can straddle two samples - and
# a script written as "this press does this" reports that as a failure. The
# invariants do not care when a press lands:
#
#   up   never decreases, never exceeds 3, and reaches 3
#   down never increases, never goes below 0, and reaches 0
#   ds:0x1F6B == rating * 30 at every single sample
PRESSES = 6
# Order matters: a "down" key tested from Fun is indistinguishable from a key
# that does nothing, because the floor bails either way. Each down run is
# therefore preceded by an up run that leaves the rating at Mayhem.
# The names are pygame's own, and pygame names a punctuation key by the
# character rather than by a word: "`", "-", "[-]". "backquote" and "minus"
# are not key names, and asking for them gets "unknown key name" - which the
# first version of this sweep printed as though the key had done nothing.
PLAN = [("up", +1), ("down", -1), ("`", +1), ("[-]", -1),
        ("`", +1), ("-", -1)]


def byte_at(sock, image_off):
    """One byte, through the control socket's own image-relative form."""
    reply = ask(sock, f"read i+{image_off:#x} 1")
    for tok in reply.replace(":", " ").split():
        if len(tok) == 2:
            try:
                return int(tok, 16)
            except ValueError:
                pass
    raise RuntimeError(f"could not read a byte out of {reply!r}")


def main():
    sock = os.path.join(ROOT, "out", "rating.sock")
    if os.path.exists(sock):
        os.unlink(sock)
    argv = [sys.executable, "-c",
            "import sys; sys.path.insert(0, %r); import lemmings; "
            "lemmings.run(sys.argv[1:])" % HERE,
            "--control-socket", sock, "--run-seconds", "400"]
    proc = subprocess.Popen(argv, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    bad = 0
    try:
        wait_for(sock, lambda st: "mode=0x03" in st, "the adapter menu")
        time.sleep(0.6)
        ask(sock, "key 1")
        time.sleep(1.5)
        ask(sock, "key 2")
        wait_for(sock, lambda st: "mode=0x10" in st, "the menu", timeout=200)
        time.sleep(12)

        gate = byte_at(sock, GATE)
        print(f"  gate ds:0x1F6F = {gate:#04x} "
              f"({'open' if gate != 4 else 'CLOSED - both paths bail'})\n")
        r = byte_at(sock, RATING)
        n = byte_at(sock, NUMBER)
        print(f"  start        rating {r} {NAMES[r] if r < 4 else '?':7s}"
              f" level {n:3d}")

        for key, direction in PLAN:
            seq = [byte_at(sock, RATING)]
            for _ in range(PRESSES):
                # The socket answers a key it cannot map with an error
                # STRING, not an exception. Reading that as "the key did
                # nothing" is how "nothing works the rating plate" got into
                # the docs, so it is checked rather than assumed.
                reply = ask(sock, f"key {key} 4")
                if "not a key" in reply or reply.lower().startswith("err"):
                    print(f"  {key:10s} NOT DELIVERED: {reply.strip()}"
                          f"\n             - nothing was tried, which is not"
                          f" the same as nothing happening")
                    bad += 1
                    seq = None
                    break
                time.sleep(1.0)
                r = byte_at(sock, RATING)
                n = byte_at(sock, NUMBER)
                if r < 4 and n != r * 30:
                    print(f"  {key:10s} level {n} is not rating*30 = {r * 30}")
                    bad += 1
                seq.append(r)
            if seq is None:
                continue
            limit = 3 if direction > 0 else 0
            mono = all((b - a) * direction >= 0 for a, b in zip(seq, seq[1:]))
            inrange = all(0 <= v <= 3 for v in seq)
            reached = seq[-1] == limit
            why = []
            if not mono:
                why.append(f"moved the wrong way")
            if not inrange:
                why.append("left 0..3")
            if not reached:
                why.append(f"did not reach {limit} ({NAMES[limit]})")
            if why:
                bad += 1
            print(f"  {key:10s} {'->'.join(str(v) for v in seq)}   "
                  f"{NAMES[seq[-1]]:7s} "
                  f"{'ok' if not why else 'FAIL: ' + ', '.join(why)}")

        if bad == 0:
            print("\n  All four keys work the rating, it stays inside 0..3,"
                  "\n  and ds:0x1F6B == rating*30 at every sample.")
    finally:
        try:
            ask(sock, "quit")
        except Exception:
            pass
        proc.terminate()
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
