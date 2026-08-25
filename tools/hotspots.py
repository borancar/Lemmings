# SPDX-License-Identifier: GPL-2.0-only
"""Find the routines that do the work, by counting executions.

A code map says what is code. It does not say what matters. This runs the
program and counts how often each instruction executes, then reports call
targets by weight - which is how you find a decompressor without guessing at
it, because a decompressor is a small routine that runs an enormous number of
times immediately after a file read.

  uv run python tools/hotspots.py                 # top routines by weight
  uv run python tools/hotspots.py --after-read    # only what runs after reads

`--after-read` restricts counting to the window between an INT 21h AH=3Fh
returning and the next one being issued, which is exactly where unpacking of
whatever was just read has to happen.
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lemmings
from dos_emulator import set_game_dir, KEYMAP
import pygame
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_CS, UC_X86_REG_IP, UC_X86_REG_AX


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--after-read", action="store_true")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--rounds", type=int, default=400)
    ap.add_argument("-o", default="")
    a = ap.parse_args()

    set_game_dir(lemmings.GAME_DIR)
    pygame.init()

    state = {"in_read": not a.after_read, "reads": 0}

    class Profiled(lemmings.LemmingsVga):
        def _dos(self):
            ah = (self._reg(UC_X86_REG_AX) >> 8) & 0xFF
            r = super()._dos()
            if ah == 0x3F:
                state["reads"] += 1
                state["in_read"] = True
            elif a.after_read:
                state["in_read"] = False
            return r

    m = Profiled(lemmings.EXE, max_insns=1 << 62, psp_seg=lemmings.PSP_SEG)
    base = m.load_seg * 16
    top = base + lemmings.DGROUP
    count = {}

    def ran(uc, ad, size, _):
        if state["in_read"] and base <= ad < top:
            off = ad - base
            count[off] = count.get(off, 0) + 1

    m.uc.hook_add(UC_HOOK_CODE, ran)

    keys = [getattr(pygame, f"K_{k}") for k in lemmings.STARTUP_KEYS]
    addr, sent = m.start, 0
    for _ in range(a.rounds):
        m.blocked_on_input = False
        try:
            m.uc.emu_start(addr, 0, count=200_000)
        except Exception as e:
            print("cpu:", e)
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

    cm = json.load(open("image/codemap.json"))
    targets = sorted(int(r["at"], 16) for r in cm["routines"])

    # Attribute each executed instruction to the nearest call target at or
    # below it. Crude, and right often enough to point at the routine: this
    # binary has no prologues to delimit functions by, so "the nearest thing
    # something called" is the best available notion of which routine an
    # address is in.
    import bisect
    weight = {}
    for off, n in count.items():
        i = bisect.bisect_right(targets, off) - 1
        if i >= 0:
            weight[targets[i]] = weight.get(targets[i], 0) + n

    total = sum(count.values())
    print(f"\ninstructions counted : {total}"
          f"{' (inside read windows only)' if a.after_read else ''}")
    print(f"INT 21h AH=3Fh reads : {state['reads']}")
    print(f"\n  {'routine':>9}  {'executions':>12}  {'share':>7}  extent")
    rows = sorted(weight.items(), key=lambda kv: -kv[1])[:a.top]
    for at, n in rows:
        i = targets.index(at)
        end = targets[i + 1] if i + 1 < len(targets) else lemmings.DGROUP
        print(f"  {at:#09x}  {n:12d}  {100.0*n/total:6.2f}%  "
              f"{at:#07x}..{end:#07x} ({end-at} bytes)")
    if a.o:
        with open(a.o, "w") as f:
            json.dump({"total": total, "reads": state["reads"],
                       "weight": {hex(k): v for k, v in rows}}, f, indent=2)
            f.write("\n")
        print(f"\nwrote {a.o}")


if __name__ == "__main__":
    main()
