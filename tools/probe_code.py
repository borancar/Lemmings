"""Drive the original's access-code generator over chosen inputs.

Image 0x05042 builds the ten-character code from three bytes - ds:0x1F6B the
level index, ds:0x491D, and ds:0x40 the rescued percentage - by scattering
their bits across ten characters, each added to a base letter, and then
rotating the first seven by 7 - (level & 7). It ends at image 0x051A1.

Reading ten near-identical mask-and-shift blocks off by eye is how this port
has twice put bit positions in the wrong place, and an automatic walk over the
disassembly produced a table whose last rows were stale register state. So
drive the ORIGINAL instead: it is a pure function of three bytes, and a
bit permutation is fully determined by flipping one input bit at a time.

    LEM_HOOK=tools/probe_code.py LEM_CODE_OUT=out/code.json \\
        uv run python tools/play.py --auto -- --run-seconds 60

It rides along with play.py rather than building its own machine: an
in-process run stops in text mode without the start-up menus answered, and
this needs the image unpacked and DS established before it can call anything.
"""
import json
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import lemmings                                      # noqa: E402
from dos_emulator import set_game_dir                # noqa: E402
import pygame                                        # noqa: E402
from unicorn import UC_HOOK_CODE                     # noqa: E402
from unicorn.x86_const import (UC_X86_REG_DS, UC_X86_REG_CS,  # noqa: E402
                               UC_X86_REG_IP, UC_X86_REG_SP)

ENTRY, RET = 0x05042, 0x051A1
LEVEL, OTHER, PERCENT = 0x1F6B, 0x491D, 0x40
OUT, CODE_LEN = 0x4D68, 10
SETTLE = 0x04401          # the menu: by here the image and DS are up


def install(m):
    """Inside the guest, via LEM_HOOK: drive the generator at the menu."""
    base = m.load_seg * 16
    st = {"done": False}

    def at(uc, ad, size, _):
        if ad - base != SETTLE or st["done"]:
            return
        st["done"] = True
        drive(m, os.environ.get("LEM_CODE_OUT", "out/code.json"))
        raise SystemExit(0)

    m.uc.hook_add(UC_HOOK_CODE, at)
    print("  [probe_code] will drive image 0x05042 at the menu")


def _sweep(n=400):
    """A wide multi-bit sweep, to check the single-bit table COMPOSES.

    One bit at a time determines a permutation only if the contributions do
    not interact - if two bits landing in the same character add rather than
    carry into the next, and if the rotation depends on nothing but the level.
    Both are believed and neither was measured, so measure them: a spread of
    inputs with many bits set, over every value of level & 7 so every rotation
    count is exercised.

    Deterministic (a fixed seed), so a failure can be re-run.
    """
    r = random.Random(20260825)
    out = []
    for i in range(n):
        # every rotation count, and level's high nibble spread across it
        lv = (r.randrange(32) << 3) | (i & 7)
        out.append((lv & 0xFF, r.randrange(256), r.randrange(101)))
    return out


def drive(m, out_path):
    ds = m.uc.reg_read(UC_X86_REG_DS) * 16
    cs = m.uc.reg_read(UC_X86_REG_CS)
    sp = m.uc.reg_read(UC_X86_REG_SP)

    def call(level, other, percent):
        m.uc.mem_write(ds + LEVEL, bytes([level]))
        m.uc.mem_write(ds + OTHER, bytes([other]))
        m.uc.mem_write(ds + PERCENT, bytes([percent]))
        m.uc.mem_write(ds + OUT, bytes(CODE_LEN))
        m.uc.reg_write(UC_X86_REG_CS, cs)
        m.uc.reg_write(UC_X86_REG_SP, sp)
        m.uc.emu_start(cs * 16 + ENTRY, cs * 16 + RET, 0, 0)
        return bytes(m.uc.mem_read(ds + OUT, CODE_LEN))

    a_out = out_path
    cases = []
    # The baseline, then every single input bit on its own: a permutation of
    # bits is completely determined by these.
    for lv, ot, pc in ([(0, 0, 0)]
                       + [(1 << b, 0, 0) for b in range(8)]
                       + [(0, 1 << b, 0) for b in range(8)]
                       + [(0, 0, 1 << b) for b in range(8)]
                       + [(37, 5, 62), (119, 3, 100), (7, 7, 7)]
                       + _sweep()):
        cases.append({"level": lv, "other": ot, "percent": pc,
                      "code": call(lv, ot, pc).hex()})
    with open(a_out, "w") as f:
        json.dump(cases, f)
    print(f"  drove the original {len(cases)} times -> {a_out}")
    b0 = bytes.fromhex(cases[0]["code"])
    print(f"  baseline (0,0,0): {b0.decode('latin1')!r}")
