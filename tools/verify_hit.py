"""The port's under-pointer test against the original's, frame by frame.

This exists because the unit test for it PASSED while the code was sixteen
pixels wrong. Both were written from the same reading of image 0x03D0A - that
the pointer's record x is `scroll - 16 + mouse_x` - and both used the guest's
scroll where the port's view_x is sixteen less. A test derived from the same
misunderstanding as the code agrees with it and proves nothing.

So this compares against the ORIGINAL rather than against a reading of it.
The guest clears ds:0x1A at image 0x03DAF and sets it at image 0x03DE8 when
its own scan finds a lemming under the pointer; that byte is the answer, and
the port's level_lemming_at has to agree with it on the same inputs.

    LEM_HOOK=tools/verify_hit.py LEM_HIT_OUT=out/hit.json \
        uv run python tools/play.py --auto --machine 2 --start \
        --click-tries 4 -- --run-seconds 90
    uv run python tools/verify_hit.py --check out/hit.json

A PASSIVE RIDE-ALONG IS NOT ENOUGH, and for a long time this tool did one.
It compared 326 frames, agreed on all 326, and the pointer had not been over
a lemming in ANY of them - so what it actually checked was that both sides
say "nothing" when the pointer is in open sky. The positive case, the only
one the sixteen-pixel bug lived in, was never reached. It now reports the two
directions separately and says so when there is no positive case.

So LEM_HIT_SWEEP=1 makes the hook DRIVE the pointer: every frame it puts the
mouse somewhere in a 21x21 block around a lemming, through the emulator's own
mouse so the game's handler at image 0x0426D sees it exactly as it would a
real one. The block is deliberately bigger than the hit box and is aimed with
nothing but the lemming's raw x and y - no reading of image 0x03D27 goes into
choosing where to point, because a sweep aimed by the same arithmetic under
test would only ever confirm it. Whatever ds:0x1A says is the answer.

WHAT THIS STILL DOES NOT CHECK, and it matters: the trip from the SCREEN to
ds:0x12. Both sides are handed the guest's own ds:0x10 and ds:0x12, so this
scores the relation from those two words to the lemming array and nothing
before it. If the port turns a mouse at screen row S into the wrong ds:0x12 -
and the window path has to, since SDL reports a screen position and the game
works in its own clamped range - every frame here still agrees while a click
lands nowhere near the lemming the player aimed at. That is a different
claim, and tools/compare_play.py is what makes it: one input script, the same
screen positions on both sides, scored as pixels.
"""
import argparse
import ctypes
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

FRAME_END = 0x018F
# The scan itself, not the end of the frame. Image 0x03DAF is the instruction
# that clears ds:0x1A, immediately before image 0x03D1E walks the array - so
# it is the one moment the inputs and the answer belong to each other.
#
# Sampling at the end of the frame instead cost a round: it agreed on 544 of
# 545 frames and the one disagreement was at a box edge, because a walker has
# moved a pixel by the time the frame ends and the array captured there is not
# the array the scan read. An off-by-one in the INSTRUMENT looks exactly like
# an off-by-one in the port.
SCAN = 0x03DAF
ARRAY, COUNT, REC = 0x88, 0x43, 0x2D
SCROLL, MOUSE_X, MOUSE_Y, FOUND = 0x00, 0x10, 0x12, 0x1A


def install(m):
    """Inside the guest, via LEM_HOOK."""
    import atexit
    from unicorn import UC_HOOK_CODE
    from unicorn.x86_const import UC_X86_REG_DS

    base = m.load_seg * 16
    out_path = os.environ.get("LEM_HIT_OUT", "out/hit.json")
    sweep = os.environ.get("LEM_HIT_SWEEP") == "1"
    # A block wider than the 12x12 hit box, so the sweep straddles its edges.
    SWEEP = [(dx, dy) for dy in range(-10, 11) for dx in range(-10, 11)]
    rows, st = [], {"n": 0}

    def at(uc, ad, size, _):
        if ad - base != SCAN or len(rows) >= 600:
            return
        ds = uc.reg_read(UC_X86_REG_DS) * 16

        def b(o):
            return uc.mem_read(ds + o, 1)[0]

        def w(o):
            return int.from_bytes(uc.mem_read(ds + o, 2), "little", signed=True)

        # ds:0x1A still holds the PREVIOUS scan's answer - this hook runs
        # before the instruction that clears it - so pair it with the inputs
        # captured last time round.
        if st.get("pending") is not None:
            st["pending"]["found"] = b(FOUND)
            rows.append(st["pending"])

        st["n"] += 1
        n = b(COUNT)
        lem = []
        for i in range(min(n, 16)):
            o = ARRAY + i * REC
            lem.append(dict(x=w(o), y=w(o + 2), hot_x=w(o + 4), hot_y=w(o + 6),
                            alive=b(o + 0x26)))
        st["pending"] = dict(scroll=w(SCROLL), mx=w(MOUSE_X), my=w(MOUSE_Y),
                             found=0, n=len(lem), lem=lem)

        # Then aim for the next scan. Read first, moved second, so the mouse
        # in each row is the one the game scanned with.
        if sweep and lem:
            a = lem[0]
            dx, dy = SWEEP[(st["n"] - 1) % len(SWEEP)]
            # Screen position from the lemming's RAW x and y and the scroll -
            # deliberately not from image 0x03D27's box arithmetic, which is
            # the thing under test.
            m.click_mouse(0, a["x"] - w(SCROLL) + 16 + dx, a["y"] - 8 + dy,
                          False)

    m.uc.hook_add(UC_HOOK_CODE, at)
    atexit.register(lambda: (open(out_path, "w").write(json.dumps(rows)),
                             print(f"  [hook] captured {len(rows)} frames")))


class Lem(ctypes.Structure):
    _fields_ = [("x", ctypes.c_int16), ("y", ctypes.c_int16),
                ("hot_x", ctypes.c_int16), ("hot_y", ctypes.c_int16),
                ("flags", ctypes.c_uint16), ("fall", ctypes.c_uint8),
                ("alive", ctypes.c_uint8), ("dir", ctypes.c_int8),
                ("anim", ctypes.c_uint8), ("state", ctypes.c_uint8)]


def build():
    src = os.path.join(ROOT, "reconstruct", "src")
    so = os.path.join(ROOT, "out", "libhit.so")
    sdl = subprocess.run(["pkg-config", "--cflags", "--libs", "sdl3"],
                         capture_output=True, text=True, check=True
                         ).stdout.split()
    subprocess.run(["cc", "-shared", "-fPIC", "-O2", "-I", src, "-o", so,
                    os.path.join(src, "game.c"),
                    os.path.join(src, "sdl_io.c")] + sdl, check=True)
    lib = ctypes.CDLL(so)
    lib.level_lemming_at.restype = ctypes.c_int16
    lib.level_lemming_at.argtypes = [ctypes.POINTER(Lem), ctypes.c_uint8,
                                     ctypes.c_uint16]
    lib.lem_mouse_move.argtypes = [ctypes.c_int32, ctypes.c_int32]
    return lib


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", required=True)
    a = ap.parse_args()
    rows = json.load(open(a.check))
    if not rows:
        raise SystemExit("no frames captured - that is a failure, not a pass")
    lib = build()

    agree = differ = skipped = 0
    pos = neg = pos_ok = neg_ok = 0
    first = None
    for r in rows:
        if not r["n"]:
            skipped += 1
            continue
        arr = (Lem * 16)()
        for i, L in enumerate(r["lem"]):
            arr[i].x, arr[i].y = L["x"], L["y"]
            arr[i].hot_x, arr[i].hot_y = L["hot_x"], L["hot_y"]
            arr[i].alive = L["alive"]
        lib.lem_mouse_move(r["mx"], r["my"])
        # The port's view_x is the guest's scroll less sixteen - the offset
        # tools/prove.sh prints and compare_run.py fits.
        got = lib.level_lemming_at(arr, r["n"], r["scroll"] - 16)
        want = 1 if r["found"] == 0xFF else 0
        if want:
            pos += 1
        else:
            neg += 1
        if (got >= 0) == bool(want):
            agree += 1
            if want:
                pos_ok += 1
            else:
                neg_ok += 1
        else:
            differ += 1
            if first is None:
                first = (r, got)

    print(f"  frames compared : {agree + differ}")
    print(f"  agreed          : {agree}")
    print(f"  differed        : {differ}")
    # A run where the pointer never touched a lemming agrees on every frame
    # and has checked nothing that matters. Say which way each frame went.
    print(f"  over a lemming  : {pos_ok}/{pos} agree"
          + ("   <- NO POSITIVE CASE: this run proves nothing about a hit"
             if pos == 0 else ""))
    print(f"  over nothing    : {neg_ok}/{neg} agree")
    if skipped:
        print(f"  skipped (no lemmings out yet) : {skipped}")
    if first:
        r, got = first
        print(f"\n  first disagreement: mouse ({r['mx']}, {r['my']}) "
              f"scroll {r['scroll']} - the original says "
              f"{'a lemming' if r['found'] == 0xFF else 'nothing'}, "
              f"the port says {got}")
        for L in r["lem"][:4]:
            print(f"    lemming at ({L['x']}, {L['y']}) hot "
                  f"({L['hot_x']}, {L['hot_y']})")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
