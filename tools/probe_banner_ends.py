"""What draws the two lemmings holding the banner?

The bar itself is understood - a 16x16 tile scrolled in a 35-column strip, see
docs/menu.md - but the strip is blitted at x=48 for 560 pixels, and the painted
bar runs 36..602. Something else paints the ends, and it animates: four
captures of the same screen carry 288, 292, 276 and 252 non-black pixels in the
margins (x<36 and x>604) of rows 334..349.

This finds it by hooking the SHARED BLITTER rather than by looking at pixels.
Image 0x8822 is the routine every draw on this screen goes through, and its
entry fixes the convention:

    ax  destination x in pixels        cl  rows
    bx  destination y                  ch  planes
    dx  width in pixels                si  ink, di mask

So every blit can be logged with its geometry, and the ones landing in the
banner's rows separated from the 1,584 that draw the bar.

  uv run python tools/probe_banner_ends.py
"""
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pygame
import lemmings
from dos_emulator import set_game_dir

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

BLIT = 0x8822           # image offset of the shared blitter
BAR_Y0, BAR_Y1 = 334, 349


def main():
    set_game_dir(lemmings.GAME_DIR)
    pygame.init()
    m = lemmings.LemmingsVga(lemmings.EXE, max_insns=1 << 62,
                             psp_seg=lemmings.PSP_SEG)
    base = m.load_seg * 16
    from unicorn import UC_HOOK_CODE as HC
    from unicorn.x86_const import (UC_X86_REG_AX as RAX, UC_X86_REG_BX as RBX,
                                   UC_X86_REG_CX as RCX, UC_X86_REG_DX as RDX,
                                   UC_X86_REG_SI as RSI, UC_X86_REG_DI as RDI,
                                   UC_X86_REG_BP as RBP, UC_X86_REG_DS as RDS)
    seen = Counter()
    order = []
    dumps = []

    def at(uc, ad, size, _):
        if ad - base != BLIT:
            return
        ax = uc.reg_read(RAX); bx = uc.reg_read(RBX)
        cx = uc.reg_read(RCX); dx = uc.reg_read(RDX)
        # bx is the destination row. Only the banner's rows are wanted, and
        # only the START of a blit - the routine is re-entered per row in some
        # paths, which is how counting hook fires once reported 132,588 draws
        # where there were 4,760.
        if not (BAR_Y0 - 32 <= bx <= BAR_Y1):
            return
        # The source is NOT in DS. The blitter reads it from the segment the
        # caller parks in ds:0x1FE4 - image 0x04F96 sets it to [0x1FEE]+0x800
        # for these, and image 0x07CE6 to [0x1FF0] for the strip. Using DS
        # here gives a plausible-looking address that is not the sprite.
        ds = uc.reg_read(RDS) * 16
        srcseg = int.from_bytes(uc.mem_read(ds + 0x1FE4, 2), "little")
        si = uc.reg_read(RSI)
        key = (ax, bx, cx & 0xFF, (cx >> 8) & 0xFF, dx, srcseg, si,
               uc.reg_read(RDI), uc.reg_read(RBP))
        if ax != 48 and len(dumps) < 2 and ax not in [d[0] for d in dumps]:
            # Keep one frame of each end sprite, straight out of guest memory,
            # so it can be matched against the data files.
            n = ((dx // 8) * (cx & 0xFF)) * 4
            dumps.append((ax, srcseg, si,
                          bytes(uc.mem_read(srcseg * 16 + si, n))))
        if key not in seen:
            order.append(key)
        seen[key] += 1

    m.uc.hook_add(HC, at)
    # An in-process run answers the two start-up menus and stops on the main
    # menu, which is exactly where the banner is. It has to be fed those keys
    # from the guest's own cue - m.run() alone sits on the first one forever.
    from unicorn.x86_const import UC_X86_REG_CS as RCS, UC_X86_REG_IP as RIP
    from dos_emulator import KEYMAP
    keys = [getattr(pygame, f"K_{k}") for k in lemmings.STARTUP_KEYS]
    addr, sent = m.start, 0
    for _ in range(600):
        m.blocked_on_input = False
        try:
            m.uc.emu_start(addr, 0, count=200_000)
        except Exception as e:
            print(f"  cpu: {e}")
            break
        addr = m._reg(RCS) * 16 + m._reg(RIP)
        m.service_keyboard()
        if m.finished:
            break
        if m.blocked_on_input and sent < len(keys):
            sc, asc = KEYMAP[keys[sent]]
            m.press_key(sc, asc, True)
            m.press_key(sc, asc, False)
            sent += 1

    if not seen:
        print("\n  NOTHING WAS SEEN at the blitter in the banner's rows.\n"
              "  That is a failure, not a pass - check the run reached "
              "mode 10h.")
        return 2

    print(f"\n  {len(seen)} distinct blits into rows "
          f"{BAR_Y0 - 32}..{BAR_Y1}, {sum(seen.values())} calls\n")
    print("     x    y  rows planes  width   srcseg     si   mask   bp   calls")
    for k in order:
        ax, by, rows, planes, dx, seg, si, mask, bp = k
        print(f"  {ax:4d} {by:4d}  {rows:4d} {planes:6d} {dx:6d}  "
              f"{seg:#07x} {si:#06x} {mask:#06x} {bp:#04x} {seen[k]:7d}")

    if dumps:
        out = os.path.join(ROOT, "out", "banner_ends.bin")
        with open(out, "wb") as f:
            for ax, seg, si, blob in dumps:
                f.write(blob)
        print(f"\n  wrote {len(dumps)} sprite frames to {out}")
        for ax, seg, si, blob in dumps:
            print(f"    x={ax}  seg={seg:#07x} si={si:#06x}  {len(blob)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
