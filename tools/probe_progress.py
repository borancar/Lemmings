# SPDX-License-Identifier: GPL-2.0-only
"""Is the guest still making progress late in a run, or has it derailed?

Samples CS:IP and the interrupt/file activity per round, and reports which
interrupt vectors the program has taken over. A game that paces on the PIT
will have hooked INT 08h, and this emulator does not deliver one.
"""
import os
import struct
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lemmings
from dos_emulator import set_game_dir, KEYMAP
import pygame
from unicorn.x86_const import UC_X86_REG_CS, UC_X86_REG_IP

set_game_dir(lemmings.GAME_DIR)
pygame.init()
m = lemmings.LemmingsVga(lemmings.EXE, max_insns=1 << 62,
                         psp_seg=lemmings.PSP_SEG)
base = m.load_seg * 16
boot = {}
for v in range(0x100):
    off, seg = struct.unpack("<HH", bytes(m.uc.mem_read(v * 4, 4)))
    boot[v] = (seg, off)

KEYS = [getattr(pygame, f"K_{k}") for k in lemmings.STARTUP_KEYS]
addr, sent = m.start, 0
prev = Counter()
pcs = Counter()
for r in range(260):
    m.blocked_on_input = False
    try:
        m.uc.emu_start(addr, 0, count=200_000)
    except Exception as e:
        print("cpu:", e)
        break
    addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
    m.service_keyboard()
    pcs[addr - base] += 1
    if m.finished:
        print("exited:", m.finished)
        break
    if r % 40 == 0 or r > 250:
        d = Counter(m.int_counts) - prev
        prev = Counter(m.int_counts)
        print(f"r{r:<4d} pc=img{addr-base:#08x} mode={m.mode:#04x} "
              f"int_delta={dict(d)}")
    if m.blocked_on_input and sent < len(KEYS):
        sc, asc = KEYMAP[KEYS[sent]]
        m.press_key(sc, asc, True)
        m.press_key(sc, asc, False)
        sent += 1

print("\nvectors the program took over:")
for v in range(0x100):
    off, seg = struct.unpack("<HH", bytes(m.uc.mem_read(v * 4, 4)))
    if (seg, off) != boot[v]:
        tag = ""
        if seg == m.load_seg:
            tag = f"  (image {seg*16+off-base:#07x})"
        print(f"  INT {v:#04x}: {boot[v][0]:04x}:{boot[v][1]:04x} -> "
              f"{seg:04x}:{off:04x}{tag}")
print("\nwhere chunks ended (image offset -> times):")
for off, c in pcs.most_common(6):
    print(f"  img {off:#08x}  {c}")
print("\nfiles read:", list(m.files_read))
print("guest interrupt dispatches:", dict(m.guest_dispatch))
