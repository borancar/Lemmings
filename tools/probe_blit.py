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

keys = [getattr(pygame, f"K_{k}") for k in lemmings.STARTUP_KEYS]
addr, sent = m.start, 0
saw_title = f1 = clicks = 0
down_at = None
playing = None
for r in range(4000):
    m.blocked_on_input = False
    try:
        m.uc.emu_start(addr, 0, count=120_000)
    except Exception as e:
        print("cpu:", e); break
    addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
    m.service_keyboard()
    m.pit0_next = 0.0
    m.service_timer()
    if m.finished:
        print("exited:", m.finished); break
    if m.blocked_on_input and sent < len(keys):
        sc, asc = KEYMAP[keys[sent]]
        m.press_key(sc, asc, True); m.press_key(sc, asc, False); sent += 1
        continue
    if down_at is not None and r >= down_at + 6:
        m.click_mouse(0, None, None, down=False); down_at = None
    if m.mode == 0x10:
        saw_title += 1
        if playing is None:
            if saw_title > 250 and f1 < 4 and saw_title % 120 == 0:
                sc, asc = KEYMAP[pygame.K_F1]
                m.press_key(sc, asc, True); m.press_key(sc, asc, False); f1 += 1
            elif f1 >= 1 and clicks < 5 and saw_title % 150 == 0 and down_at is None:
                m.click_mouse(0, 160, 100, down=True); down_at = r; clicks += 1
    elif m.mode == 0x0D and saw_title > 250:
        if playing is None:
            playing = r
            armed[0] = True          # start recording only in the level
            print(f"  chunk {r}: in the level - recording")
        elif r - playing > 400:
            break

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
