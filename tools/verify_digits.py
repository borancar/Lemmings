# SPDX-License-Identifier: GPL-2.0-only
"""Check the port's digits3 against the original's, call by call.

image 0x04BC2 turns a byte into three decimal digits, by subtracting 100 until
it borrows rather than dividing. The port writes it as division, which is what
it means - so this checks the two agree on every call the game makes, which is
the only way to be sure the rewrite did not change behaviour at some edge.

  uv run python tools/verify_digits.py

The routine runs while the game builds "levelNNN.dat", which a start-up run
reaches, so this needs no driver.
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
                               UC_X86_REG_SI, UC_X86_REG_AX)

ENTRY, RET = 0x04BC2, 0x04BE6
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


SCRATCH = 0x0400      # a DS-relative scratch offset the game is not using


def drive_all_values(m, base, lib):
    """Run the ORIGINAL's routine for every byte value and compare.

    The routine is pure - AL in, three bytes at DS:SI out - so it can be called
    directly: set the registers, start the CPU at its entry, stop at its `ret`.
    No part of this is the port's arithmetic; it is the original's own code,
    asked 256 questions instead of the one the game happens to ask.
    """
    from unicorn.x86_const import UC_X86_REG_AX as RAX, UC_X86_REG_SI as RSI
    from unicorn.x86_const import UC_X86_REG_DS as RDS
    ds = m.uc.reg_read(RDS) * 16
    agree, differ = 0, []
    for v in range(256):
        m.uc.reg_write(RAX, v)
        m.uc.reg_write(RSI, SCRATCH)
        m.uc.mem_write(ds + SCRATCH, b"\x00\x00\x00")
        try:
            m.uc.emu_start(base + ENTRY, base + RET)
        except Exception:
            differ.append((v, "cpu error"))
            continue
        theirs = bytes(m.uc.mem_read(ds + SCRATCH, 3))
        buf = ctypes.create_string_buffer(4)
        lib.digits3(v, buf)
        if buf.raw[:3] == theirs:
            agree += 1
        else:
            differ.append((v, theirs, buf.raw[:3]))
    return agree, differ


def install(m):
    """Runs in the guest via LEM_HOOK. digits3 is called while the game builds
    "levelNNN.dat", which happens on the way INTO a level - an in-process
    start-up run stops at the menu and reaches it zero times."""
    import atexit
    import json
    from unicorn import UC_HOOK_CODE as HC
    from unicorn.x86_const import (UC_X86_REG_DS as RDS, UC_X86_REG_SI as RSI,
                                   UC_X86_REG_AX as RAX)
    base = m.load_seg * 16
    out_path = os.environ["LEM_DIGITS_OUT"]
    cases, pend = [], {}

    def at(uc, ad, size, _):
        off = ad - base
        if off == ENTRY and not pend:
            pend["v"] = uc.reg_read(RAX) & 0xFF
            pend["at"] = uc.reg_read(RDS) * 16 + uc.reg_read(RSI)
        elif off == RET and pend and len(cases) < 200:
            cases.append({"v": pend["v"],
                          "out": bytes(uc.mem_read(pend["at"], 3)).hex()})
            pend.clear()

    m.uc.hook_add(HC, at)
    atexit.register(lambda: (open(out_path, "w").write(json.dumps(cases)),
                             print(f"  [hook] captured {len(cases)} calls")))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--max", type=int, default=40)
    ap.add_argument("--driven", action="store_true",
                    help="drive into a level with play.py; a start-up run "
                         "never calls this routine at all")
    a = ap.parse_args()

    src = os.path.join(ROOT, "reconstruct", "src")
    so = os.path.join(ROOT, "out", "libdigits.so")
    lib = lemmings.game_lib()
    lib.digits3.restype = None
    lib.digits3.argtypes = [ctypes.c_uint8, ctypes.c_char_p]

    if a.driven:
        import json
        cap = os.path.join(ROOT, "out", "digits_cases.json")
        if os.path.exists(cap):
            os.unlink(cap)
        env = dict(os.environ, LEM_HOOK=os.path.abspath(__file__),
                   LEM_DIGITS_OUT=cap)
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
        ok = bad = 0
        vals = set()
        for c in cases:
            vals.add(c["v"])
            buf = ctypes.create_string_buffer(4)
            lib.digits3(c["v"], buf)
            if buf.raw[:3] == bytes.fromhex(c["out"]):
                ok += 1
            else:
                bad += 1
                print(f"  DIFFER for {c['v']}: original "
                      f"{bytes.fromhex(c['out'])!r}, port {buf.raw[:3]!r}")
        print(f"\n  calls compared : {len(cases)}")
        print(f"  agreed         : {ok}")
        print(f"  differed       : {bad}")
        print(f"  values seen    : {sorted(vals)}")
        if not cases:
            print("\n  NOTHING WAS COMPARED. That is a failure, not a pass.")
            return 2
        return 0 if bad == 0 else 1

    set_game_dir(lemmings.GAME_DIR)
    pygame.init()
    m = lemmings.LemmingsVga(lemmings.EXE, max_insns=1 << 62,
                             psp_seg=lemmings.PSP_SEG)
    base = m.load_seg * 16
    st = {"in": None, "n": 0, "ok": 0, "bad": 0, "done": False, "vals": set()}

    def at(uc, ad, size, _):
        off = ad - base
        if off == ENTRY and st["in"] is None:
            ds = uc.reg_read(UC_X86_REG_DS) * 16
            st["in"] = (uc.reg_read(UC_X86_REG_AX) & 0xFF,
                        ds + uc.reg_read(UC_X86_REG_SI))
        elif off == RET and st["in"] is not None:
            v, dest = st["in"]
            st["in"] = None
            st["n"] += 1
            st["vals"].add(v)
            # SI has advanced by two; the digits start where it began.
            theirs = bytes(uc.mem_read(dest, 3))
            buf = ctypes.create_string_buffer(4)
            lib.digits3(v, buf)
            ours = buf.raw[:3]
            if ours == theirs:
                st["ok"] += 1
            else:
                st["bad"] += 1
                print(f"  DIFFER for {v}: original {theirs!r}, port {ours!r}")
            if st["n"] >= a.max:
                st["done"] = True
                uc.emu_stop()

    m.uc.hook_add(UC_HOOK_CODE, at)
    keys = [getattr(pygame, f"K_{k}") for k in lemmings.STARTUP_KEYS]
    addr, sent = m.start, 0
    for _ in range(400):
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

    # The game only ever asks for one value, so the calls it makes are thin
    # evidence. The routine is PURE - it reads AL and writes three bytes at
    # DS:SI - so the original's own code can be driven with inputs of our
    # choosing: point CS:IP at it, set AL, run to its `ret`, read the result.
    # That turns one sample into an exhaustive check against the real thing.
    exhaustive = drive_all_values(m, base, lib)
    print(f"\n  calls compared : {st['n']}")
    print(f"  agreed         : {st['ok']}")
    print(f"  differed       : {st['bad']}")
    print(f"  values seen    : {sorted(st['vals'])}")
    if st["n"] == 0:
        print("  (a start-up run stops at the menu; the game calls this on the"
              " way INTO a level, so incidental calls here are expected to be"
              " zero - see --driven)")
    agree, differ = exhaustive
    print(f"\n  driven exhaustively: {agree} of 256 values agree")
    for d in differ[:8]:
        print(f"    DIFFER {d}")

    if st["n"] == 0 and agree == 0:
        print("\n  NOTHING WAS COMPARED. That is a failure, not a pass.")
        return 2
    return 0 if st["bad"] == 0 and not differ else 1


if __name__ == "__main__":
    sys.exit(main())
