# SPDX-License-Identifier: GPL-2.0-only
"""Catch the moment the guest stops making progress, and show how it got there.

Keeps a ring buffer of executed addresses and dumps it the first time the PC
stays inside a small window for a long stretch - which is what a derailed guest
looks like from outside.
"""
import argparse
import os
import sys
from collections import deque

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lemmings
from dos_emulator import set_game_dir, KEYMAP
import pygame
import capstone
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_CS, UC_X86_REG_IP

ap = argparse.ArgumentParser()
ap.add_argument("--lo", type=lambda v: int(v, 0), default=0x00D00)
ap.add_argument("--hi", type=lambda v: int(v, 0), default=0x00E00)
ap.add_argument("--stuck", type=int, default=20000,
                help="consecutive instructions inside [lo,hi) that mean stuck")
ap.add_argument("--ring", type=int, default=120)
a = ap.parse_args()

set_game_dir(lemmings.GAME_DIR)
pygame.init()
m = lemmings.LemmingsVga(lemmings.EXE, max_insns=1 << 62,
                         psp_seg=lemmings.PSP_SEG)
base = m.load_seg * 16
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
ring = deque(maxlen=a.ring)
pre = deque(maxlen=a.ring)
run = [0]
done = [False]


def code(uc, addr, size, _):
    off = addr - base
    try:
        b = bytes(uc.mem_read(addr, min(size, 15)))
    except Exception:
        b = b""
    ring.append((off, b))
    if a.lo <= off < a.hi:
        if run[0] == 0:
            # First instruction of this visit. Freeze how we arrived, because
            # once the loop starts spinning it fills the ring with itself.
            pre.clear()
            pre.extend(ring)
        run[0] += 1
        if run[0] >= a.stuck and not done[0]:
            done[0] = True
            print(f"\n=== stuck: {run[0]} consecutive instructions inside "
                  f"[{a.lo:#07x},{a.hi:#07x}) ===")
            print(f"the {len(pre)} instructions leading into this "
                  f"visit:\n")
            for o, bb in pre:
                txt = "??"
                for i in md.disasm(bb, o):
                    txt = f"{i.mnemonic} {i.op_str}"
                    break
                mark = " " if a.lo <= o < a.hi else ">"
                print(f" {mark} img{o:#08x}  {bb.hex():<14} {txt}")
            uc.emu_stop()
    else:
        run[0] = 0


m.uc.hook_add(UC_HOOK_CODE, code)
KEYS = [getattr(pygame, f"K_{k}") for k in lemmings.STARTUP_KEYS]
addr, sent = m.start, 0
for _ in range(400):
    m.blocked_on_input = False
    try:
        m.uc.emu_start(addr, 0, count=200_000)
    except Exception as e:
        print("cpu:", e)
        break
    if done[0]:
        break
    addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
    m.service_keyboard()
    if m.finished:
        print("exited:", m.finished)
        break
    if m.blocked_on_input and sent < len(KEYS):
        sc, asc = KEYMAP[KEYS[sent]]
        m.press_key(sc, asc, True)
        m.press_key(sc, asc, False)
        sent += 1
if not done[0]:
    print("never got stuck in that window")
