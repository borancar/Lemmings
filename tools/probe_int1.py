# SPDX-License-Identifier: GPL-2.0-only
"""Stop at the first INT 01h and say what caused it.

An interrupt count is not evidence of a trap flag: `int 1` is an instruction a
program can execute outright. This reads EFLAGS at the moment the hook fires
and disassembles the instruction that was executing, so the two cases can be
told apart instead of assumed.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lemmings
from dos_emulator import set_game_dir, KEYMAP
from unicorn import UC_HOOK_CODE, UC_HOOK_INTR
from unicorn.x86_const import (UC_X86_REG_CS, UC_X86_REG_IP, UC_X86_REG_EFLAGS,
                               UC_X86_REG_SP, UC_X86_REG_SS)
import capstone
import pygame
import struct

set_game_dir(lemmings.GAME_DIR)
pygame.init()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)

last = [0, b""]
hits = [0]


class Probe(lemmings.LemmingsVga):
    """Override the hook rather than adding a second one.

    `hook_add(UC_HOOK_INTR, self._on_intr)` captures the bound method at
    construction, so patching the attribute afterwards changes nothing and
    registering another hook makes every interrupt run twice - which looks
    exactly like the game misbehaving.
    """

    def _on_intr(self, uc, intno, user):
        if intno == 0x01 and hits[0] < 3:
            hits[0] += 1
            fl = uc.reg_read(UC_X86_REG_EFLAGS)
            cs, ip = uc.reg_read(UC_X86_REG_CS), uc.reg_read(UC_X86_REG_IP)
            txt = "??"
            for i in md.disasm(last[1], last[0]):
                txt = f"{i.mnemonic} {i.op_str}"
                break
            off, seg = struct.unpack("<HH", bytes(uc.mem_read(0x01 * 4, 4)))
            print(f"\n--- INT 01h #{hits[0]} ---")
            print(f"  EFLAGS = {fl:#07x}   TF={'SET' if fl & 0x100 else 'clear'}  "
                  f"IF={'set' if fl & 0x200 else 'clear'}")
            print(f"  CS:IP now      = {cs:04x}:{ip:04x}  (image {cs*16+ip-base:#07x})")
            print(f"  last executed  = {last[0]:#07x} (image {last[0]-base:#07x})"
                  f"  {last[1].hex()}  {txt}")
            print(f"  IVT[01h]       = {seg:04x}:{off:04x} "
                  f"(image {seg*16+off-base:#07x})")
            was_int1 = last[1][:2] == b"\xcd\x01"
            print("  cause          = " + ("an `int 1` INSTRUCTION" if was_int1
                                           else "the TRAP FLAG (no int 1 in the stream)"))
        return super()._on_intr(uc, intno, user)


m = Probe(lemmings.EXE, max_insns=1 << 62, psp_seg=lemmings.PSP_SEG)
base = m.load_seg * 16


def code(uc, addr, size, _):
    last[0] = addr
    try:
        last[1] = bytes(uc.mem_read(addr, min(size, 15)))
    except Exception:
        last[1] = b""


m.uc.hook_add(UC_HOOK_CODE, code)

KEYS = [pygame.K_1, pygame.K_1]
sent = 0
addr = m.start
for r in range(4000):
    m.blocked_on_input = False
    try:
        m.uc.emu_start(addr, 0, count=200_000)
    except Exception as e:
        print("cpu:", e); break
    addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
    m.service_keyboard()
    if hits[0] >= 3:
        break
    if m.finished:
        print("exited:", m.finished); break
    if m.blocked_on_input and sent < len(KEYS):
        sc, asc = KEYMAP[KEYS[sent]]
        m.press_key(sc, asc, True); m.press_key(sc, asc, False)
        sent += 1
print("\ndone; int_counts:", dict(m.int_counts))
