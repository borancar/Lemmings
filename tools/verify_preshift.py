# SPDX-License-Identifier: GPL-2.0-only
"""Check the port's preshift_sprite against the original's, call by call.

image 0x05B11 builds the eight sub-byte alignments of a sprite once, at load
time, so the game never has to shift while drawing. This is the differential
test the method asks for: stop the original at the routine's entry, capture its
inputs, let ITS OWN BODY run to the `ret` at 0x05BAF, then run the port's C on
those inputs and compare the output byte for byte.

  uv run python tools/verify_preshift.py

This one runs in-process: the sprites are built while the level's data is being
loaded, which an ordinary start-up run does reach.
"""
import argparse
import ctypes
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lemmings
from dos_emulator import set_game_dir, KEYMAP
import pygame
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import (UC_X86_REG_CS, UC_X86_REG_IP, UC_X86_REG_DS,
                               UC_X86_REG_ES, UC_X86_REG_SI, UC_X86_REG_DI,
                               UC_X86_REG_DX)

ENTRY = 0x05B11
RET = 0x05BAF
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def install(m):
    """Runs inside the guest via LEM_HOOK, so play.py can drive it into a
    level - an in-process start-up run only ever exercises one sprite shape,
    and 106 calls of one shape is much weaker evidence than it looks."""
    import atexit
    import json
    from unicorn import UC_HOOK_CODE as HC
    from unicorn.x86_const import (UC_X86_REG_DS as RDS, UC_X86_REG_ES as RES,
                                   UC_X86_REG_SI as RSI, UC_X86_REG_DI as RDI,
                                   UC_X86_REG_DX as RDX)
    base = m.load_seg * 16
    out_path = os.environ["LEM_PRESHIFT_OUT"]
    cases, pend = [], {}

    def at(uc, ad, size, _):
        off = ad - base
        if off == ENTRY and not pend:
            ds = uc.reg_read(RDS) * 16
            es = uc.reg_read(RES) * 16
            dx = uc.reg_read(RDX)
            # NAMED BACKWARDS, and it cost a day: DL is the ROW COUNT and
            # DH is the BYTES PER ROW. The only shape a level builds is
            # DL=10, DH=2 - ten rows of two bytes, which is the walker's
            # 16x10 - and the source it reads is rows * bpr * 2 planes = 40,
            # exactly the frame size the port draws it with.
            rows, bpr = dx & 0xFF, (dx >> 8) & 0xFF
            cols = rows            # the old name, kept for the JSON below
            if not (1 <= rows <= 64 and 1 <= bpr <= 200):
                return
            pend.update(cols=rows, rows=bpr, dest=es + uc.reg_read(RDI),
                        src=bytes(uc.mem_read(ds + uc.reg_read(RSI),
                                              rows * bpr * 2)).hex())
        elif off == RET and pend and len(cases) < 400:
            need = 8 * 3 * pend["cols"] * (pend["rows"] + 1)
            pend["after"] = bytes(uc.mem_read(pend["dest"], need)).hex()
            cases.append({k: pend[k] for k in ("cols", "rows", "src", "after")})
            pend.clear()

    m.uc.hook_add(HC, at)
    atexit.register(lambda: (open(out_path, "w").write(json.dumps(cases)),
                             print(f"  [hook] captured {len(cases)} calls")))


def build_lib(path):
    src = os.path.join(ROOT, "reconstruct", "src")
    lib = lemmings.game_lib()
    lib.preshift_sprite.restype = None
    lib.preshift_sprite.argtypes = [
        ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint8, ctypes.c_uint8,
        ctypes.POINTER(ctypes.c_uint8)]
    return lib


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--max", type=int, default=40)
    ap.add_argument("--rounds", type=int, default=400)
    ap.add_argument("--driven", action="store_true",
                    help="drive into a level with play.py, for sprite shapes "
                         "an in-process start-up run never reaches")
    a = ap.parse_args()

    lib = build_lib(os.path.join(ROOT, "out", "libpreshift.so"))

    if a.driven:
        import json
        cap = os.path.join(ROOT, "out", "preshift_cases.json")
        if os.path.exists(cap):
            os.unlink(cap)
        env = dict(os.environ, LEM_HOOK=os.path.abspath(__file__),
                   LEM_PRESHIFT_OUT=cap)
        subprocess.run([sys.executable, os.path.join(HERE, "play.py"),
                        "--auto", "--start", "--start-delay", "4",
                        "--start-tries", "3", "--click-tries", "4",
                        "--", "--run-seconds", "150"],
                       cwd=ROOT, env=env, stdout=subprocess.DEVNULL)
        if not os.path.exists(cap):
            print("\n  NOTHING WAS COMPARED - the run never reached it.")
            return 2
        with open(cap) as f:
            cases = json.load(f)
        shapes, ok, bad = {}, 0, 0
        for c in cases:
            cols, rows = c["cols"], c["rows"]
            need = 8 * 3 * cols * (rows + 1)
            src = bytes.fromhex(c["src"])
            want = bytes.fromhex(c["after"])
            buf = (ctypes.c_uint8 * need)()
            cin = (ctypes.c_uint8 * len(src)).from_buffer_copy(src)
            lib.preshift_sprite(cin, cols, rows, buf)
            good = bytes(buf) == want
            ok += good
            bad += not good
            k = f"{cols}x{rows}"
            shapes.setdefault(k, [0, 0])[0 if good else 1] += 1
        print(f"\n  calls compared : {len(cases)}")
        print(f"  agreed         : {ok}")
        print(f"  differed       : {bad}")
        print(f"  shapes (cols x rows), agreed/differed:")
        for k, (g, b) in sorted(shapes.items(),
                                key=lambda kv: -(kv[1][0] + kv[1][1])):
            print(f"    {k:>9s}  {g:4d} / {b}")
        return 0 if bad == 0 and cases else 2

    set_game_dir(lemmings.GAME_DIR)
    pygame.init()
    m = lemmings.LemmingsVga(lemmings.EXE, max_insns=1 << 62,
                             psp_seg=lemmings.PSP_SEG)
    base = m.load_seg * 16
    st = {"in": None, "n": 0, "ok": 0, "bad": 0, "done": False}

    def at(uc, ad, size, _):
        off = ad - base
        if off == ENTRY and st["in"] is None:
            ds = uc.reg_read(UC_X86_REG_DS) * 16
            es = uc.reg_read(UC_X86_REG_ES) * 16
            si = uc.reg_read(UC_X86_REG_SI)
            di = uc.reg_read(UC_X86_REG_DI)
            dx = uc.reg_read(UC_X86_REG_DX)
            cols, rows = dx & 0xFF, (dx >> 8) & 0xFF
            if not (1 <= cols <= 64 and 1 <= rows <= 200):
                return
            st["in"] = (bytes(uc.mem_read(ds + si, cols * rows * 2)),
                        cols, rows, es + di)
        elif off == RET and st["in"] is not None:
            src, cols, rows, dest = st["in"]
            st["in"] = None
            st["n"] += 1
            need = 8 * 3 * cols * (rows + 1)
            theirs = bytes(uc.mem_read(dest, need))
            buf = (ctypes.c_uint8 * need)()
            cin = (ctypes.c_uint8 * len(src)).from_buffer_copy(src)
            lib.preshift_sprite(cin, cols, rows, buf)
            ours = bytes(buf)
            if ours == theirs:
                st["ok"] += 1
                print(f"  call {st['n']:2d}  {cols}x{rows} cols/rows  "
                      f"{need:6d} bytes  AGREE")
            else:
                st["bad"] += 1
                d = sum(1 for x, y in zip(ours, theirs) if x != y)
                first = next(i for i, (x, y) in enumerate(zip(ours, theirs))
                             if x != y)
                print(f"  call {st['n']:2d}  {cols}x{rows}  DIFFER in {d} of "
                      f"{need}, first at {first:#x}")
            if st["n"] >= a.max:
                st["done"] = True
                uc.emu_stop()

    m.uc.hook_add(UC_HOOK_CODE, at)
    keys = [getattr(pygame, f"K_{k}") for k in lemmings.STARTUP_KEYS]
    addr, sent = m.start, 0
    print(f"comparing the port against the original at image {ENTRY:#07x}\n")
    for _ in range(a.rounds):
        m.blocked_on_input = False
        try:
            m.uc.emu_start(addr, 0, count=200_000)
        except Exception as e:
            print("cpu:", e)
            break
        if st["done"]:
            break
        addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
        m.service_keyboard()
        if m.finished:
            break
        if m.blocked_on_input and sent < len(keys):
            sc, asc = KEYMAP[keys[sent]]
            m.press_key(sc, asc, True)
            m.press_key(sc, asc, False)
            sent += 1

    print(f"\n  calls compared : {st['n']}")
    print(f"  agreed         : {st['ok']}")
    print(f"  differed       : {st['bad']}")
    if st["n"] == 0:
        print("\n  NOTHING WAS COMPARED. That is a failure, not a pass.")
        return 2
    return 0 if st["bad"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
