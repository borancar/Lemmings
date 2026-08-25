# SPDX-License-Identifier: GPL-2.0-only
"""Catch the INT 21h AH=4Ch that ends the run, and show how it got there."""
import os
import sys
from collections import deque

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lemmings
from dos_emulator import set_game_dir, KEYMAP
import pygame
import capstone
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import (UC_X86_REG_AX, UC_X86_REG_CS, UC_X86_REG_IP,
                               UC_X86_REG_SP, UC_X86_REG_SS, UC_X86_REG_BX,
                               UC_X86_REG_CX, UC_X86_REG_DX)

set_game_dir(lemmings.GAME_DIR)
pygame.init()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
hist = deque(maxlen=400)
fired = [False]


class Probe(lemmings.LemmingsVga):
    def _on_intr(self, uc, intno, user):
        ax = uc.reg_read(UC_X86_REG_AX)
        if intno == 0x21 and (ax >> 8) == 0x4C and not fired[0]:
            fired[0] = True
            base = self.load_seg * 16
            print(f"\n=== INT 21h AH=4Ch (exit {ax & 0xFF}) ===")
            print(f"  at image {uc.reg_read(UC_X86_REG_CS)*16 + uc.reg_read(UC_X86_REG_IP) - base:#08x}")
            print(f"  bx={uc.reg_read(UC_X86_REG_BX):04x} "
                  f"cx={uc.reg_read(UC_X86_REG_CX):04x} "
                  f"dx={uc.reg_read(UC_X86_REG_DX):04x}")
            print("\n  the instructions before it:")
            for o in hist:
                try:
                    b = bytes(uc.mem_read(base + o, 8))
                except Exception:
                    b = b""
                t = "??"
                for i in md.disasm(b, o):
                    t = f"{i.mnemonic} {i.op_str}"
                    break
                print(f"    img{o:#08x}  {b[:5].hex():<10} {t}")
            uc.emu_stop()
        return super()._on_intr(uc, intno, user)


m = Probe(lemmings.EXE, max_insns=1 << 62, psp_seg=lemmings.PSP_SEG)
base = m.load_seg * 16
m.uc.hook_add(UC_HOOK_CODE, lambda uc, a, sz, _: hist.append(a - base))

KEYS = [getattr(pygame, f"K_{k}") for k in lemmings.STARTUP_KEYS]
addr, sent = m.start, 0
for _ in range(200):
    m.blocked_on_input = False
    try:
        m.uc.emu_start(addr, 0, count=200_000)
    except Exception as e:
        print("cpu:", e)
        break
    if fired[0]:
        break
    addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
    m.service_keyboard()
    if m.finished:
        break
    if m.blocked_on_input and sent < len(KEYS):
        sc, asc = KEYMAP[KEYS[sent]]
        m.press_key(sc, asc, True)
        m.press_key(sc, asc, False)
        sent += 1
if not fired[0]:
    print("no AH=4Ch seen")
