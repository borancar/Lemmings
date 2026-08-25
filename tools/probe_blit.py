# SPDX-License-Identifier: GPL-2.0-only
"""What the play-area blit actually does, once the level is running.

Records, for the A000 aperture: where reads come from and writes go, and the
Graphics Controller write mode in force at the time. Answers whether the level
is copied video-to-video through the latches - which would need reads to
return plane data - or written from main memory, which would not.
"""
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lemmings
from dos_emulator import set_game_dir, KEYMAP
import pygame
from unicorn import UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.x86_const import UC_X86_REG_CS, UC_X86_REG_IP, UC_X86_REG_DS

set_game_dir(lemmings.GAME_DIR)
pygame.init()
m = lemmings.LemmingsVga(lemmings.EXE, max_insns=1 << 62,
                         psp_seg=lemmings.PSP_SEG)
base = m.load_seg * 16
armed = [False]
reads = Counter()
writes = Counter()
modes_on_write = Counter()
read_pcs = Counter()
write_pcs = Counter()
ds_at_write = Counter()

def on_read(uc, access, addr, size, value, user):
    if not armed[0]:
        return
    reads[(addr - 0xA0000) >> 12] += 1
    pc = uc.reg_read(UC_X86_REG_CS) * 16 + uc.reg_read(UC_X86_REG_IP)
    read_pcs[pc - base] += 1

def on_write(uc, access, addr, size, value, user):
    if not armed[0]:
        return
    writes[(addr - 0xA0000) >> 12] += 1
    modes_on_write[m.gc[5] & 3] += 1
    pc = uc.reg_read(UC_X86_REG_CS) * 16 + uc.reg_read(UC_X86_REG_IP)
    write_pcs[pc - base] += 1
    ds_at_write[uc.reg_read(UC_X86_REG_DS)] += 1

m.uc.hook_add(UC_HOOK_MEM_READ, on_read, None, 0xA0000, 0xAFFFF)
m.uc.hook_add(UC_HOOK_MEM_WRITE, on_write, None, 0xA0000, 0xAFFFF)

def on_chunk(mm, r, in_level):
    armed[0] = in_level


playing = lemmings.drive_to_level(m, on_chunk=on_chunk, settle=600)

print(f"\nmode {m.mode:#04x}  in-level: {playing is not None}")
print(f"A000 reads : {sum(reads.values())}   writes: {sum(writes.values())}")
print("read  offsets by 4K page :",
      {f"{k*0x1000:#07x}": v for k, v in sorted(reads.items())})
print("write offsets by 4K page :",
      {f"{k*0x1000:#07x}": v for k, v in sorted(writes.items())})
print("GC write mode when writing:", dict(modes_on_write))
print("top reading  code (image offset):", [f"{o:#07x}:{c}" for o, c in read_pcs.most_common(5)])
print("top writing  code (image offset):", [f"{o:#07x}:{c}" for o, c in write_pcs.most_common(5)])
print("DS at write:", {hex(k): v for k, v in ds_at_write.most_common(5)})
