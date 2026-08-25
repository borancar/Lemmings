# SPDX-License-Identifier: GPL-2.0-only
"""Drive the original all the way into the level, then report its video state.

Reaching gameplay needs the whole start-up chain - two menus, F1 after the
palette fade, and a HELD mouse click past the briefing - so this exists rather
than being repeated in every probe.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lemmings
from dos_emulator import set_game_dir, KEYMAP
import pygame
from unicorn.x86_const import UC_X86_REG_CS, UC_X86_REG_IP


def drive(m, rounds=2600, on_frame=None):
    """Run the machine to gameplay. Returns True if it got there."""
    keys = [getattr(pygame, f"K_{k}") for k in lemmings.STARTUP_KEYS]
    addr, sent = m.start, 0
    saw10 = 0
    pressed_f1 = 0
    clicked = 0
    held = 0
    for r in range(rounds):
        m.blocked_on_input = False
        try:
            m.uc.emu_start(addr, 0, count=120_000)
        except Exception as e:
            print("cpu:", e)
            return False
        addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
        m.service_keyboard()
        m.service_timer()
        if m.finished:
            print("exited:", m.finished)
            return False
        if m.blocked_on_input and sent < len(keys):
            sc, asc = KEYMAP[keys[sent]]
            m.press_key(sc, asc, True)
            m.press_key(sc, asc, False)
            sent += 1
            continue
        if m.mode == 0x10:
            saw10 += 1
            # Let the palette fade finish, then F1 a few times.
            if saw10 > 60 and pressed_f1 < 3 and saw10 % 40 == 0:
                sc, asc = KEYMAP[pygame.K_F1]
                m.press_key(sc, asc, True)
                m.press_key(sc, asc, False)
                pressed_f1 += 1
            # The briefing wants a HELD click, not a tap.
            elif pressed_f1 >= 1 and clicked < 4 and saw10 % 55 == 0:
                m.click_mouse(0, 160, 100, down=True)
                held = r
                clicked += 1
        if held and r == held + 3:
            m.click_mouse(0, None, None, down=False)
            held = 0
        if on_frame:
            on_frame(m, r)
        if m.mode == 0x0D and saw10 > 60:
            # Back in 0Dh after the title: that is the game itself.
            if r > (held or 0) + 400:
                return True
    return m.mode == 0x0D and saw10 > 60


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", default="")
    a = ap.parse_args()
    set_game_dir(lemmings.GAME_DIR)
    pygame.init()
    m = lemmings.LemmingsVga(lemmings.EXE, max_insns=1 << 62,
                             psp_seg=lemmings.PSP_SEG)
    ok = drive(m)
    print(f"\nreached gameplay: {ok}   mode={m.mode:#04x} "
          f"{m.width}x{m.height}")
    NAMES = {0x06: "v total", 0x07: "overflow", 0x09: "max scanline",
             0x0C: "start hi", 0x0D: "start lo", 0x12: "v display end",
             0x13: "offset", 0x14: "underline", 0x17: "mode ctrl",
             0x18: "LINE COMPARE"}
    print("CRTC registers written:")
    for i in sorted(m.crtc):
        print(f"  {i:#04x} {NAMES.get(i, ''):<14} = {m.crtc[i]:#04x} ({m.crtc[i]})")
    print(f"crtc_offset={m.crtc_offset} start_addr={m.start_addr:#x}")
    print(f"attr_pal={[hex(x) for x in m.attr_pal]}")
    print(f"non-black DAC entries: {sum(1 for c in m.palette if c != (0,0,0))}")
    nz = [i for i, pl in enumerate(m.planes)
          if any(pl[:0x4000])]
    print(f"planes with data in the first 16K: {nz}")
    for p in range(4):
        pl = m.planes[p]
        print(f"  plane {p}: nonzero bytes in 0..8000 = "
              f"{sum(1 for b in pl[:8000] if b)}, "
              f"8000..16000 = {sum(1 for b in pl[8000:16000] if b)}")
    if a.dump:
        import numpy as np
        fb = np.frombuffer(m.framebuffer(), dtype=np.uint8)
        pal = np.array(m.palette, dtype=np.uint8)
        np.savez(a.dump, fb=fb.reshape(m.height, m.width), palette=pal)
        print("wrote", a.dump)


if __name__ == "__main__":
    main()
