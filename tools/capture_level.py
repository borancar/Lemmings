# SPDX-License-Identifier: GPL-2.0-only
"""Drive the original into its first level and capture the screen it draws.

Produces the reference the port is checked against: the guest's own video
memory, decoded to RGB, plus the palette the port should be using.

  uv run python tools/capture_level.py -o out/original-level.npz

Timer pacing, and why it is forced here. The emulator paces INT 08h on the
wall clock so an interactive run goes at about the speed the game was written
for. An in-process driver runs the guest as fast as the host allows, so wall
clock and guest time come apart badly and the front end - which is driven
entirely by that interrupt - advances at a crawl. So this forces one tick per
chunk instead. It changes how fast the game thinks time passes, not what it
draws, and what is being captured here is a picture.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lemmings
from dos_emulator import set_game_dir, KEYMAP
import pygame
import numpy as np
from unicorn.x86_const import UC_X86_REG_CS, UC_X86_REG_IP


def ground_palette(path):
    """The 16 colours the port draws a level in, from GROUNDxO.DAT."""
    buf = open(path, "rb").read()
    custom, standard = buf[0x3D8:0x3F0], buf[0x3F0:0x408]
    pal = np.zeros((16, 3), dtype=np.uint8)
    for i in range(8):
        for c in range(3):
            pal[i][c] = (standard[i * 3 + c] & 0x3F) * 255 // 63
            pal[8 + i][c] = (custom[i * 3 + c] & 0x3F) * 255 // 63
    return pal


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", default="out/original-level.npz")
    ap.add_argument("--png", default="", help="also write the screen as a PNG")
    ap.add_argument("--chunks", type=int, default=4000)
    ap.add_argument("--settle", type=int, default=700,
                    help="chunks to keep running after gameplay starts")
    a = ap.parse_args()

    set_game_dir(lemmings.GAME_DIR)
    pygame.init()
    m = lemmings.LemmingsVga(lemmings.EXE, max_insns=1 << 62,
                             psp_seg=lemmings.PSP_SEG)

    keys = [getattr(pygame, f"K_{k}") for k in lemmings.STARTUP_KEYS]
    addr, sent = m.start, 0
    saw_title = 0
    f1 = clicks = 0
    down_at = None
    playing_since = None

    for r in range(a.chunks):
        m.blocked_on_input = False
        try:
            m.uc.emu_start(addr, 0, count=120_000)
        except Exception as e:
            print("cpu:", e)
            break
        addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
        m.service_keyboard()
        m.pit0_next = 0.0          # force a tick - see the module docstring
        m.service_timer()
        if m.finished:
            print("exited:", m.finished)
            break

        if m.blocked_on_input and sent < len(keys):
            sc, asc = KEYMAP[keys[sent]]
            m.press_key(sc, asc, True)
            m.press_key(sc, asc, False)
            sent += 1
            continue

        if down_at is not None and r >= down_at + 6:
            m.click_mouse(0, None, None, down=False)
            down_at = None

        if m.mode == 0x10:
            saw_title += 1
            if playing_since is None:
                if saw_title > 250 and f1 < 4 and saw_title % 120 == 0:
                    sc, asc = KEYMAP[pygame.K_F1]
                    m.press_key(sc, asc, True)
                    m.press_key(sc, asc, False)
                    f1 += 1
                    print(f"  chunk {r}: F1 ({f1})")
                elif f1 >= 1 and clicks < 5 and saw_title % 150 == 0 \
                        and down_at is None:
                    m.click_mouse(0, 160, 100, down=True)
                    down_at = r
                    clicks += 1
                    print(f"  chunk {r}: click ({clicks})")
        elif m.mode == 0x0D and saw_title > 250:
            if playing_since is None:
                playing_since = r
                print(f"  chunk {r}: in the level")
            elif r - playing_since > a.settle:
                break

    print(f"\nmode {m.mode:#04x}  {m.width}x{m.height}  "
          f"in-level: {playing_since is not None}")
    fb = np.frombuffer(m.framebuffer(), dtype=np.uint8).reshape(m.height, m.width)
    pal = np.array(m.palette, dtype=np.uint8)
    rgb = pal[fb]

    os.makedirs(os.path.dirname(os.path.abspath(a.o)), exist_ok=True)
    gnd = os.path.join(lemmings.GAME_DIR, "ground0o.dat")
    np.savez(a.o, fb=fb, palette=pal, rgb=rgb,
             port_palette=ground_palette(gnd),
             in_level=np.array([playing_since is not None]))
    print(f"wrote {a.o}  ({m.width}x{m.height})")
    if a.png:
        try:
            from PIL import Image
            Image.fromarray(rgb).save(a.png)
            print("wrote", a.png)
        except ImportError:
            print("(--png needs Pillow)")


if __name__ == "__main__":
    main()
