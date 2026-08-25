# SPDX-License-Identifier: GPL-2.0-only
"""Record every INT 13h the game makes, and what it does with the answer."""
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lemmings
from dos_emulator import set_game_dir, KEYMAP
import pygame
from unicorn.x86_const import (UC_X86_REG_AX, UC_X86_REG_BX, UC_X86_REG_CX,
                               UC_X86_REG_DX, UC_X86_REG_ES, UC_X86_REG_CS,
                               UC_X86_REG_IP)

set_game_dir(lemmings.GAME_DIR)
pygame.init()

AH = {0x00: "reset", 0x01: "get status", 0x02: "read sectors",
      0x03: "write sectors", 0x04: "verify", 0x05: "format track",
      0x08: "get drive params", 0x15: "get disk type", 0x16: "change line",
      0x17: "set disk type", 0x18: "set media type"}
calls = []


class Probe(lemmings.LemmingsVga):
    def _on_intr(self, uc, intno, user):
        if intno == 0x13:
            ax = uc.reg_read(UC_X86_REG_AX)
            bx = uc.reg_read(UC_X86_REG_BX)
            cx = uc.reg_read(UC_X86_REG_CX)
            dx = uc.reg_read(UC_X86_REG_DX)
            es = uc.reg_read(UC_X86_REG_ES)
            cs = uc.reg_read(UC_X86_REG_CS)
            ip = uc.reg_read(UC_X86_REG_IP)
            calls.append((
                (ax >> 8) & 0xFF, ax & 0xFF, cx, dx, es, bx,
                cs * 16 + ip - self.load_seg * 16))
        return super()._on_intr(uc, intno, user)


m = Probe(lemmings.EXE, max_insns=1 << 62, psp_seg=lemmings.PSP_SEG)
KEYS = [getattr(pygame, f"K_{k}") for k in lemmings.STARTUP_KEYS]
addr, sent = m.start, 0
for _ in range(200):
    m.blocked_on_input = False
    try:
        m.uc.emu_start(addr, 0, count=200_000)
    except Exception as e:
        print("cpu:", e)
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

print(f"\nINT 13h calls: {len(calls)}")
print(f"{'#':>3}  {'AH':<22} AL  CH CL DH DL  ES:BX        from")
seen = Counter()
for n, (ah, al, cx, dx, es, bx, pc) in enumerate(calls):
    seen[ah] += 1
    if seen[ah] <= 4 or n >= len(calls) - 6:
        print(f"{n:>3}  {AH.get(ah, '?'):<22} {al:02x}  "
              f"{(cx >> 8) & 0xFF:02x} {cx & 0xFF:02x} "
              f"{(dx >> 8) & 0xFF:02x} {dx & 0xFF:02x}  "
              f"{es:04x}:{bx:04x}   img{pc:#08x}")
print("\nby function:", {AH.get(k, hex(k)): v for k, v in seen.most_common()})
