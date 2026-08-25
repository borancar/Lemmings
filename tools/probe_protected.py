# SPDX-License-Identifier: GPL-2.0-only
"""Record the protected instruction stream, in plaintext, as it executes.

The protection keeps exactly one instruction decrypted at a time, so this is
the only way to read that stretch: catch each instruction at the moment the
handler has decrypted it and the CPU is about to run it.

This is the "record it as it executes" code map the protected region needs.
"""
import os
import sys
import struct

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lemmings
from dos_emulator import set_game_dir, KEYMAP
import pygame
import capstone
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import (UC_X86_REG_SP, UC_X86_REG_SS, UC_X86_REG_AX,
                               UC_X86_REG_BX, UC_X86_REG_CX, UC_X86_REG_DX,
                               UC_X86_REG_SI, UC_X86_REG_DI, UC_X86_REG_ES,
                               UC_X86_REG_DS, UC_X86_REG_CS, UC_X86_REG_IP)

set_game_dir(lemmings.GAME_DIR)
pygame.init()
m = lemmings.LemmingsVga(lemmings.EXE, max_insns=1 << 62,
                         psp_seg=lemmings.PSP_SEG)
base = m.load_seg * 16
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)

pending = [None]
seq = []
R = [("ax", UC_X86_REG_AX), ("bx", UC_X86_REG_BX), ("cx", UC_X86_REG_CX),
     ("dx", UC_X86_REG_DX), ("si", UC_X86_REG_SI), ("di", UC_X86_REG_DI),
     ("ds", UC_X86_REG_DS), ("es", UC_X86_REG_ES)]


def code(uc, addr, size, _):
    off = addr - base
    if off == 0x0E1C:
        ss, sp = uc.reg_read(UC_X86_REG_SS), uc.reg_read(UC_X86_REG_SP)
        try:
            ip, cs = struct.unpack("<HH", bytes(uc.mem_read(ss * 16 + sp + 6, 4)))
            pending[0] = cs * 16 + ip
        except Exception:
            pending[0] = None
        return
    if pending[0] is not None and not (0x0E1C <= off < 0x0E3A):
        want, pending[0] = pending[0], None
        if addr == want:
            try:
                b = bytes(uc.mem_read(addr, 12))
            except Exception:
                b = b""
            regs = " ".join(f"{n}={uc.reg_read(r):04x}" for n, r in R)
            seq.append((off, b, regs))


m.uc.hook_add(UC_HOOK_CODE, code)
KEYS = [getattr(pygame, f"K_{k}") for k in lemmings.STARTUP_KEYS]
addr, sent = m.start, 0
for _ in range(120):
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
    if len(seq) >= 108 and m.mode == 0x0D:
        break
    if m.blocked_on_input and sent < len(KEYS):
        sc, asc = KEYMAP[KEYS[sent]]
        m.press_key(sc, asc, True)
        m.press_key(sc, asc, False)
        sent += 1

print(f"protected instructions recorded: {len(seq)}\n")
for off, b, regs in seq:
    t = "??"
    for i in md.disasm(b, off):
        t = f"{i.mnemonic} {i.op_str}"
        n = i.size
        break
    else:
        n = 1
    print(f"  img{off:#08x}  {b[:n].hex():<14} {t:<34} {regs}")
