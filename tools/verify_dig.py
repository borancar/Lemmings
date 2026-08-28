"""The port's digger against the original's, frame by frame.

The frame-by-frame comparisons here score a RECORDED run, and no recorded run
ever assigns a skill - so everything a digger does is invisible to them. Two
bugs got through that way and both were found by a person playing: the fall
condition tested the wrong row, and the shaft started a row low.

This is the differential test that catches them. tools/probe_digger.py drives
the original into assigning a digger and records its array every frame; this
takes the state at the moment it is assigned, runs the PORT's own simulation
forward on a copy of the same terrain, and compares y, the animation and the
state word frame by frame.

    LEM_HOOK=tools/probe_digger.py LEM_DIG_TRACE=out/digtrace.json \
        uv run python tools/play.py --auto --machine 2 --start \
        --click-tries 4 -- --run-seconds 150
    uv run python tools/verify_dig.py --check out/digtrace.json

WHAT THIS DOES NOT YET CATCH, and it matters
--------------------------------------------
It compares the lemming's y, animation and state word, and those AGREE with
both of the bugs a player found put back - the shaft starting a row low, and
the fall testing the row below. Neither changes y or the animation while the
digger is still inside solid ground, so state alone cannot separate a digger
cutting the right rows from one cutting the wrong ones. The control said so:
reintroduce either bug and this still reports 138 of 138.

What separates them is the TERRAIN, and the reference for that is not right
yet. Reading plane 3 at image 0x05DC6's own `(y - 0x10) * 0xD0 + (x >> 3)`
gives whole rows uniformly solid or uniformly empty - which no dug shaft
looks like - so either the stride, the page or the plane is wrong, and a
comparison against it would be reporting the instrument. The capture is still
recorded in the trace, and LEM_DIG_SHAFT=1 turns the comparison on for
whoever fixes the read.

The first version of this file also filled the patch with 8s and passed with
both bugs in place, for the same reason in a simpler form. A control is the
only thing that showed either.
"""
import argparse
import ctypes
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import lemmings   # noqa: E402  (after sys.path)



def build():
    src = os.path.join(ROOT, "reconstruct", "src")
    so = os.path.join(ROOT, "out", "libdig.so")
    lib = lemmings.game_lib()
    global Lem
    Lem = lemmings.lem_struct(lib)   # checked against the C
    lib.lemmings_step.argtypes = [ctypes.POINTER(Lem), ctypes.c_uint8,
                                  ctypes.POINTER(ctypes.c_uint8),
                                  ctypes.c_uint16, ctypes.c_uint16]
    return lib


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", required=True)
    a = ap.parse_args()
    tr = json.load(open(a.check))
    rows = tr["frames"]
    if len(rows) < 8:
        raise SystemExit("too few frames captured - that is a failure")

    lib = build()
    start = rows[0]

    # THE REAL TERRAIN, not a solid block.
    #
    # The first version of this filled a patch with 8s, and it passed with
    # BOTH of the bugs a player had already found put back - because in solid
    # ground neither the row it clears nor the row it tests can change y or
    # the animation. That is the "agreed, but every call was an early return"
    # trap, and a control is the only thing that shows it.
    #
    # tools/prove.sh writes the composed terrain, which is what the original's
    # lemmings probe, so the fall lands where it really lands.
    raw = os.path.join(ROOT, "out", "port.raw")
    if not os.path.isfile(raw):
        raise SystemExit(f"{raw}: run tools/prove.sh first - this test needs\n"
                         "the composed terrain, and a solid block cannot fail")
    with open(raw, "rb") as f:
        blob = f.read()
    W = int.from_bytes(blob[6:8], "little")
    H = int.from_bytes(blob[8:10], "little")
    pix = blob[58:]
    ground = (ctypes.c_uint8 * (W * H)).from_buffer_copy(pix[:W * H])

    L = (Lem * 1)()
    L[0].x, L[0].y = start["x"], start["y"]
    L[0].hot_x, L[0].hot_y = start["hot_x"], start["hot_y"]
    L[0].flags = start["flags"]
    L[0].anim = start["anim"]
    L[0].state = start["state"]
    L[0].dir = start["dir"]

    dy0 = start["y"]
    agree = differ = 0
    first = None
    stopped = None
    for r in rows[1:]:
        # WHERE THIS CHECK STOPS, and it has to say so.
        #
        # The port is re-simulated on TERRAIN ALONE - out/port.raw is the
        # composed level with no trigger map behind it - so it has no exit to
        # walk into and no trap to be caught by. The original's own record
        # does: image 0x02F42 assigns the whole state word 0x2000 when a
        # lemming reaches the exit, and from that frame on the two sides are
        # not simulating the same thing.
        #
        # out/digtrace2.json is exactly that run. Scored to its end it
        # reported 286 of 614 frames differing from frame 432 - the frame the
        # original's digger, having broken through, walked out - and every
        # one of those was this gap rather than anything about the digger.
        # A number that large reads as a broken port; it is a broken claim.
        if r["flags"] & 0x2000:
            stopped = r["f"]
            break
        lib.lemmings_step(L, 1, ground, W, H)
        want_dy = r["y"] - dy0
        got_dy = L[0].y - dy0
        ok = (got_dy == want_dy and L[0].anim == r["anim"]
              and L[0].flags == r["flags"])
        if ok:
            agree += 1
        else:
            differ += 1
            if first is None:
                first = (r, want_dy, got_dy, L[0].anim, L[0].flags)

    # AND THE SHAFT, which is the half that can actually catch a digger
    # cutting the wrong rows - see the note at the top of this file.
    sh = tr.get("shaft")
    sagree = sdiffer = 0
    sfirst = None
    if sh:
        for r, line in enumerate(sh["map"]):
            for c, want in enumerate(line):
                gx = sh["x0"] + c - 16
                gy = sh["y0"] + r - 16
                if not (0 <= gx < W and 0 <= gy < H):
                    continue
                got = 1 if (ground[gy * W + gx] & 8) else 0
                if got == want:
                    sagree += 1
                else:
                    sdiffer += 1
                    if sfirst is None:
                        sfirst = (sh["x0"] + c, sh["y0"] + r, want, got)

    print(f"  digging frames compared : {agree + differ}")
    if stopped is not None:
        print(f"  stopped at frame {stopped}: the original's lemming reached "
              "the EXIT,\n"
              "  which this comparison has no objects to reproduce. Frames "
              "after it\n"
              "  are not scored, and are not evidence either way.")
    print(f"  agreed                  : {agree}")
    print(f"  differed                : {differ}")
    if sh:
        print(f"  shaft pixels compared   : {sagree + sdiffer}")
        print(f"  agreed                  : {sagree}")
        print(f"  differed                : {sdiffer}")
        if sfirst:
            x, y, want, got = sfirst
            print(f"\n  first shaft difference at ({x}, {y}): the original "
                  f"has it {'solid' if want else 'dug'}, the port "
                  f"{'solid' if got else 'dug'}")
            return 1
    else:
        print("  the shaft is NOT compared - see the note in this file. State\n"
              "  alone cannot catch a digger cutting the wrong rows, so this\n"
              "  check is weaker than it looks.")

    if first:
        r, wdy, gdy, ganim, gflags = first
        print(f"\n  first disagreement at the original's frame {r['f']}:")
        print(f"    the original: dy {wdy:+d}  anim {r['anim']:3d}  "
              f"flags {r['flags']:#06x}")
        print(f"    the port    : dy {gdy:+d}  anim {ganim:3d}  "
              f"flags {gflags:#06x}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
