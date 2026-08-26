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
ARRAY, COUNT, REC = 0x88, 0x43, 0x2D
SCROLL, MOUSE_X, MOUSE_Y, FOUND = 0x00, 0x10, 0x12, 0x1A


def install(m):
    """Inside the guest, via LEM_HOOK."""
    import atexit
    from unicorn import UC_HOOK_CODE
    from unicorn.x86_const import UC_X86_REG_DS

    base = m.load_seg * 16
    out_path = os.environ.get("LEM_HIT_OUT", "out/hit.json")
    rows, st = [], {"n": 0}

    def at(uc, ad, size, _):
        if ad - base != FRAME_END or len(rows) >= 600:
            return
        st["n"] += 1
        ds = uc.reg_read(UC_X86_REG_DS) * 16

        def b(o):
            return uc.mem_read(ds + o, 1)[0]

        def w(o):
            return int.from_bytes(uc.mem_read(ds + o, 2), "little", signed=True)

        n = b(COUNT)
        lem = []
        for i in range(min(n, 16)):
            o = ARRAY + i * REC
            lem.append(dict(x=w(o), y=w(o + 2), hot_x=w(o + 4), hot_y=w(o + 6),
                            alive=b(o + 0x26)))
        rows.append(dict(scroll=w(SCROLL), mx=w(MOUSE_X), my=w(MOUSE_Y),
                         found=b(FOUND), n=len(lem), lem=lem))

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
        if (got >= 0) == bool(want):
            agree += 1
        else:
            differ += 1
            if first is None:
                first = (r, got)

    print(f"  frames compared : {agree + differ}")
    print(f"  agreed          : {agree}")
    print(f"  differed        : {differ}")
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
