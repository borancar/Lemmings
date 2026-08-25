"""Every blit the menu makes into a chosen band of rows.

Image 0x8822 is the shared blitter - everything drawn on this screen goes
through it - and its own entry fixes the convention, which is what makes this
useful:

    ax  destination x in pixels    cl  rows
    bx  destination y              ch  planes
    dx  width in PIXELS            si  ink, di mask, bp flags

and the SOURCE SEGMENT is not DS. The blitter takes it from ds:0x1FE4, which
the caller sets: image 0x04F96 to [0x1FEE]+0x800 and image 0x07CE6 to
[0x1FF0]. Reading DS instead gives a plausible address that is not the sprite,
which cost a round here.

This is how the two lemmings holding the banner were found, after a pixel
search had failed: they are simply two more calls, at x=0 and x=592, 48x16,
and their sprites carry the bar's end caps.

  uv run python tools/probe_blits.py                 # the banner's rows
  uv run python tools/probe_blits.py --rows 120 260  # the plates
  uv run python tools/probe_blits.py --dump out/x.bin

Counting hook fires instead of blit STARTS reports bytes, not draws - 132,588
where there were 4,760 - so this keys on the entry instruction only.
"""
import argparse
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
BAR_Y0, BAR_Y1 = 334, 349      # the default band: the banner


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rows", nargs=2, type=int, default=[BAR_Y0 - 32, BAR_Y1],
                    metavar=("Y0", "Y1"),
                    help="only blits whose destination row is in this band")
    ap.add_argument("--dump", default="",
                    help="write the first sprite of each distinct x to a file")
    a = ap.parse_args()
    y0, y1 = a.rows

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
        if not (y0 <= bx <= y1):
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
        if a.dump and len(dumps) < 12 and ax not in [d[0] for d in dumps]:
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
          f"{y0}..{y1}, {sum(seen.values())} calls\n")
    print("     x    y  rows planes  width   srcseg     si   mask   bp   calls")
    for k in order:
        ax, by, rows, planes, dx, seg, si, mask, bp = k
        print(f"  {ax:4d} {by:4d}  {rows:4d} {planes:6d} {dx:6d}  "
              f"{seg:#07x} {si:#06x} {mask:#06x} {bp:#04x} {seen[k]:7d}")

    if dumps and a.dump:
        out = a.dump
        with open(out, "wb") as f:
            for ax, seg, si, blob in dumps:
                f.write(blob)
        print(f"\n  wrote {len(dumps)} sprite frames to {out}")
        for ax, seg, si, blob in dumps:
            print(f"    x={ax}  seg={seg:#07x} si={si:#06x}  {len(blob)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
