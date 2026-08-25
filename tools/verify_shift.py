# SPDX-License-Identifier: GPL-2.0-only
"""Check the port's shift_row against the original's, call by call.

image 0x09347 is how the game draws at an x that is not a multiple of eight:
it copies a sprite row into a scratch buffer at DS:0x6706, appends a zero byte,
and shifts the whole thing right one bit at a time. The port transcribes it.

This stops the original twice inside one call - once with the buffer copied but
not yet shifted (image 0x09365, where the shift count is read), and once after
the shift and before the blit (image 0x09382) - runs the port's C on the first
capture, and compares with the second.

  uv run python tools/verify_shift.py

It rides along with tools/play.py through LEM_HOOK, because this only happens
on a screen an in-process run never reaches.
"""
import argparse
import ctypes
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

BEFORE = 0x09365          # buffer copied, shift count about to be read
AFTER = 0x09382           # shifted, about to be blitted
BUF = 0x6706              # DS-relative, from `lea di, [0x6706]`
SHIFT_AT = 0xA948         # DS-relative, the bit count


def install(m):
    """Runs inside the guest, via LEM_HOOK."""
    from unicorn import UC_HOOK_CODE
    from unicorn.x86_const import UC_X86_REG_DS, UC_X86_REG_CX

    base = m.load_seg * 16
    out_path = os.environ["LEM_SHIFT_OUT"]
    cases, pending = [], {}

    def at(uc, ad, size, _):
        off = ad - base
        if off not in (BEFORE, AFTER) or getattr(m, "mode", 0) != 0x10:
            return
        if len(cases) >= 200:
            return
        ds = uc.reg_read(UC_X86_REG_DS) * 16
        n = uc.reg_read(UC_X86_REG_CX) & 0xFF
        if off == BEFORE:
            if not (1 <= n <= 120):
                return
            pending.clear()
            pending["len"] = n
            pending["bits"] = uc.mem_read(ds + SHIFT_AT, 1)[0]
            pending["src"] = bytes(uc.mem_read(ds + BUF, n + 1)).hex()
        elif pending:
            pending["after"] = bytes(
                uc.mem_read(ds + BUF, pending["len"] + 1)).hex()
            cases.append(dict(pending))
            pending.clear()

    m.uc.hook_add(UC_HOOK_CODE, at)

    import atexit

    def dump():
        with open(out_path, "w") as f:
            json.dump(cases, f)
        print(f"  [hook] captured {len(cases)} shift calls -> {out_path}")

    atexit.register(dump)


def build_lib(path):
    src = os.path.join(ROOT, "reconstruct", "src")
    sdl = subprocess.run(["pkg-config", "--cflags", "--libs", "sdl3"],
                         capture_output=True, text=True, check=True
                         ).stdout.split()
    subprocess.run(["cc", "-shared", "-fPIC", "-O2", "-Wall", "-Wextra",
                    "-Wconversion", "-I", src, "-o", path,
                    os.path.join(src, "game.c"),
                    os.path.join(src, "sdl_io.c")] + sdl, check=True)
    lib = ctypes.CDLL(path)
    lib.shift_row.restype = ctypes.c_uint8
    lib.shift_row.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint8,
                              ctypes.c_uint8, ctypes.POINTER(ctypes.c_uint8)]
    return lib


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seconds", type=int, default=60)
    a = ap.parse_args()

    lib = build_lib(os.path.join(ROOT, "out", "libshift.so"))
    cap = os.path.join(ROOT, "out", "shift_cases.json")
    if os.path.exists(cap):
        os.unlink(cap)          # a stale capture reads as a successful run
    env = dict(os.environ, LEM_HOOK=os.path.abspath(__file__),
               LEM_SHIFT_OUT=cap)
    subprocess.run([sys.executable, os.path.join(HERE, "play.py"), "--auto",
                    "--", "--run-seconds", str(a.seconds)],
                   cwd=ROOT, env=env, stdout=subprocess.DEVNULL)
    if not os.path.exists(cap):
        print("\n  NOTHING WAS COMPARED. That is a failure, not a pass -\n"
              f"  the run never reached image {BEFORE:#07x}.")
        return 2
    with open(cap) as f:
        cases = json.load(f)
    if not cases:
        print("\n  the run reached the routine but captured no complete call.")
        return 2

    agree = differ = 0
    shifts = set()
    for c in cases:
        src = bytes.fromhex(c["src"])
        want = bytes.fromhex(c["after"])
        n, bits = c["len"], c["bits"]
        shifts.add(bits)
        buf = (ctypes.c_uint8 * (n + 1))()
        cin = (ctypes.c_uint8 * (n + 1)).from_buffer_copy(src)
        drawn = lib.shift_row(cin, n, bits, buf)
        got = bytes(buf)
        if got == want and drawn == n + 1:
            agree += 1
        else:
            differ += 1
            if differ <= 3:
                print(f"  DIFFER len {n} bits {bits}\n"
                      f"    original {want.hex()}\n"
                      f"    port     {got.hex()}")
    print(f"\n  shift calls compared : {len(cases)}")
    print(f"  agreed               : {agree}")
    print(f"  differed             : {differ}")
    print(f"  shift counts seen    : {sorted(shifts)}")
    return 0 if differ == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
