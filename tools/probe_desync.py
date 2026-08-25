# SPDX-License-Identifier: GPL-2.0-only
"""Find the first single-step desynchronisation.

The protection decrypts exactly one instruction per #DB, using the return
address in the interrupt frame. If the CPU ever executes an address other than
the one the handler just decrypted, the rolling key is out of step and every
instruction after it is noise. That first mismatch is the bug.

Records, per trap: the return address the handler reads from ss:[sp+6], and the
address actually executed after the IRET.
"""
import os
import sys
from collections import deque

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lemmings
from dos_emulator import set_game_dir, KEYMAP
import pygame
import capstone
import struct
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import (UC_X86_REG_CS, UC_X86_REG_IP, UC_X86_REG_SP,
                               UC_X86_REG_SS, UC_X86_REG_EFLAGS)

set_game_dir(lemmings.GAME_DIR)
pygame.init()
m = lemmings.LemmingsVga(lemmings.EXE, max_insns=1 << 62,
                         psp_seg=lemmings.PSP_SEG)
base = m.load_seg * 16
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)

# The handler reads the next-instruction pointer at 0e1c; snapshot it there.
PROBE_AT = {0x0E1C, 0x0D0C}
IRET_AT = {0x0E39}
pending = [None]        # (linear address the handler decrypted)
history = deque(maxlen=28)
bad = [0]
traps = [0]


def code(uc, addr, size, _):
    # Kept deliberately cheap: a per-instruction mem_read is slow enough that
    # the run never reaches the failure. Bytes are fetched lazily at report
    # time, from the history of offsets.
    off = addr - base
    history.append(off)
    if off == 0x0E1C:
        ss = uc.reg_read(UC_X86_REG_SS)
        sp = uc.reg_read(UC_X86_REG_SP)
        try:
            ip, cs = struct.unpack("<HH", bytes(uc.mem_read(ss * 16 + sp + 6, 4)))
            pending[0] = cs * 16 + ip
            traps[0] += 1
        except Exception:
            pending[0] = None
        return
    if pending[0] is not None and not (0x0E1C <= off < 0x0E3A):
        want = pending[0]
        pending[0] = None
        if addr != want:
            bad[0] += 1
            print(f"\n=== DESYNC after {traps[0]} traps ===")
            print(f"  handler decrypted image {want - base:#08x}")
            print(f"  CPU executed      image {off:#08x}")
            print("\n  the instructions before it:")
            for o in history:
                try:
                    bb = bytes(uc.mem_read(base + o, 8))
                except Exception:
                    bb = b""
                t = "??"
                for i in md.disasm(bb, o):
                    t = f"{i.mnemonic} {i.op_str}"
                    break
                note = ""
                if bb[:1] in (b"\x8e", b"\x17", b"\x1f", b"\x07"):
                    note = "  <-- loads a segment register"
                if bb[:1] in (b"\xf3", b"\xf2"):
                    note = "  <-- REP prefix"
                print(f"    img{o:#08x}  {bb[:6].hex():<12} {t}{note}")
            uc.emu_stop()


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
    if bad[0]:
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
print(f"\ntraps seen: {traps[0]}, desyncs: {bad[0]}")
