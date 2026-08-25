# SPDX-License-Identifier: GPL-2.0-only
"""Record every VGA/EGA register the game programs, and how it writes pixels.

Builds the requirement for a mode 0Dh renderer from what Lemmings actually
does, rather than from a general EGA model: which Graphics Controller and
Sequencer indexes it sets, to what, and where in A000 it writes.
"""
import os
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lemmings
from dos_emulator import set_game_dir, KEYMAP
import pygame
from unicorn import UC_HOOK_MEM_WRITE
from unicorn.x86_const import UC_X86_REG_CS, UC_X86_REG_IP

set_game_dir(lemmings.GAME_DIR)
pygame.init()

gc_index = [0]
seq_index = [0]
gc_writes = defaultdict(Counter)
seq_writes = defaultdict(Counter)
other_out = Counter()
a000_writes = Counter()
a000_lo = [0xFFFFF]
a000_hi = [0]
writers = Counter()


class Probe(lemmings.LemmingsVga):
    def _on_out(self, uc, port, size, value, user):
        if port == 0x3CE:
            gc_index[0] = value & 0xFF
            if size == 2:
                gc_writes[value & 0xFF][(value >> 8) & 0xFF] += 1
        elif port == 0x3CF:
            gc_writes[gc_index[0]][value & 0xFF] += 1
        elif port == 0x3C4:
            seq_index[0] = value & 0xFF
            if size == 2:
                seq_writes[value & 0xFF][(value >> 8) & 0xFF] += 1
        elif port == 0x3C5:
            seq_writes[seq_index[0]][value & 0xFF] += 1
        else:
            other_out[port] += 1
        return super()._on_out(uc, port, size, value, user)


m = Probe(lemmings.EXE, max_insns=1 << 62, psp_seg=lemmings.PSP_SEG)
base = m.load_seg * 16


def on_write(uc, access, addr, size, value, _):
    if 0xA0000 <= addr < 0xB0000:
        a000_writes[size] += 1
        a000_lo[0] = min(a000_lo[0], addr)
        a000_hi[0] = max(a000_hi[0], addr + size)
        pc = uc.reg_read(UC_X86_REG_CS) * 16 + uc.reg_read(UC_X86_REG_IP)
        writers[pc - base] += 1


m.uc.hook_add(UC_HOOK_MEM_WRITE, on_write)

KEYS = [pygame.K_1, pygame.K_1]
addr, sent, rounds = m.start, 0, 0
while rounds < 260:
    rounds += 1
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

GC = {0: "set/reset", 1: "enable set/reset", 2: "colour compare",
      3: "data rotate / function", 4: "read map select", 5: "mode",
      6: "misc (memory map)", 7: "colour don't care", 8: "bit mask"}
SEQ = {0: "reset", 1: "clocking mode", 2: "map mask", 3: "char map",
       4: "memory mode"}

print(f"\nfinal video mode: {m.mode:#04x}")
print("\nGraphics Controller (0x3CE/0x3CF):")
for idx in sorted(gc_writes):
    vals = ", ".join(f"{v:#04x}x{c}" for v, c in gc_writes[idx].most_common(6))
    print(f"  idx {idx:#04x} {GC.get(idx,'?'):<22} <- {vals}")
print("\nSequencer (0x3C4/0x3C5):")
for idx in sorted(seq_writes):
    vals = ", ".join(f"{v:#04x}x{c}" for v, c in seq_writes[idx].most_common(6))
    print(f"  idx {idx:#04x} {SEQ.get(idx,'?'):<22} <- {vals}")
print("\nother OUT ports:", {hex(p): c for p, c in other_out.most_common(12)})
print(f"\nA000 writes: {dict(a000_writes)}  range "
      f"{a000_lo[0]:#07x}..{a000_hi[0]:#07x}")
print("top writers (image offset -> count):")
for off, c in writers.most_common(8):
    print(f"  img {off:#07x}  {c}")
