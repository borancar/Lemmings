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
import subprocess
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


def install(m):
    """Runs in the guest via LEM_HOOK, so this can ride along with play.py.

    An in-process run answers the two start-up menus and stops on the main
    menu. Pressing F1 from inside that loop does NOT get to the briefing -
    tried, and it logs the same 156 blits as the menu, which is the failure
    this file's own advice warns about: a run that did not reach the state
    reports success. play.py drives the original properly, waits on the
    guest's own cues, and `--start --click-tries 0` parks it on the briefing.
    """
    import atexit
    import json
    from unicorn import UC_HOOK_CODE as HC
    from unicorn.x86_const import (UC_X86_REG_AX as RAX, UC_X86_REG_BX as RBX,
                                   UC_X86_REG_CX as RCX, UC_X86_REG_DX as RDX,
                                   UC_X86_REG_SI as RSI, UC_X86_REG_DI as RDI,
                                   UC_X86_REG_BP as RBP, UC_X86_REG_DS as RDS,
                                   UC_X86_REG_SS as RSS, UC_X86_REG_SP as RSP)
    base = m.load_seg * 16
    out_path = os.environ["LEM_BLITS_OUT"]
    y0, y1 = (int(v) for v in os.environ.get("LEM_BLITS_ROWS", "0 349").split())
    seen = {}

    def at(uc, ad, size, _):
        if ad - base != BLIT:
            return
        bx = uc.reg_read(RBX)
        if not (y0 <= bx <= y1):
            return
        ds = uc.reg_read(RDS) * 16
        srcseg = int.from_bytes(uc.mem_read(ds + 0x1FE4, 2), "little")
        cx = uc.reg_read(RCX)
        # WHO CALLED. The blitter is entered by `lcall`, so the far return
        # address is on the stack: IP at [SP], CS at [SP+2]. Recording it is
        # the whole difference between a list of rectangles and a call graph -
        # composing this port's screens from measured geometry produced a
        # pixel-exact menu and left 184 of 211 call targets never reached,
        # because nothing here ever followed the original's control flow.
        sp = uc.reg_read(RSS) * 16 + uc.reg_read(RSP)
        rip = int.from_bytes(uc.mem_read(sp, 2), "little")
        rcs = int.from_bytes(uc.mem_read(sp + 2, 2), "little")
        caller = rcs * 16 + rip - base
        key = (uc.reg_read(RAX), bx, cx & 0xFF, (cx >> 8) & 0xFF,
               uc.reg_read(RDX), srcseg, uc.reg_read(RSI), uc.reg_read(RDI),
               uc.reg_read(RBP), caller)
        seen[key] = seen.get(key, 0) + 1

    m.uc.hook_add(HC, at)
    atexit.register(lambda: (
        open(out_path, "w").write(json.dumps(
            [[list(k), n] for k, n in seen.items()])),
        print(f"  [hook] {len(seen)} distinct blits")))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rows", nargs=2, type=int, default=[BAR_Y0 - 32, BAR_Y1],
                    metavar=("Y0", "Y1"),
                    help="only blits whose destination row is in this band")
    ap.add_argument("--dump", default="",
                    help="write the first sprite of each distinct x to a file")
    ap.add_argument("--start", action="store_true",
                    help="go on past the main menu into the level BRIEFING, by "
                         "pressing F1 once the start-up keys are used up. The "
                         "briefing wants a mouse click to leave, so the guest "
                         "sits on it and every blit it makes can be logged")
    ap.add_argument("--seconds", type=int, default=70,
                    help="how long the driven run lasts. Short enough and it "
                         "stops while the briefing is still up; too long and "
                         "the demo starts the level on its own and the log "
                         "mixes the two screens together")
    ap.add_argument("--driven", action="store_true",
                    help="ride along with play.py instead of running in "
                         "process - the only way to reach the briefing")
    a = ap.parse_args()
    y0, y1 = a.rows

    if a.driven:
        import json
        cap = os.path.join(ROOT, "out", "blits.json")
        if os.path.exists(cap):
            os.unlink(cap)
        env = dict(os.environ, LEM_HOOK=os.path.abspath(__file__),
                   LEM_BLITS_OUT=cap, LEM_BLITS_ROWS=f"{y0} {y1}")
        subprocess.run([sys.executable, os.path.join(HERE, "play.py"),
                        "--auto", "--start", "--start-delay", "5",
                        "--start-tries", "3", "--click-tries", "0",
                        "--", "--run-seconds", str(a.seconds)],
                       cwd=ROOT, env=env, stdout=subprocess.DEVNULL)
        if not os.path.exists(cap):
            print("\n  NOTHING WAS CAPTURED - the run never reached the "
                  "blitter.\n  That is a failure, not a pass.")
            return 2
        with open(cap) as f:
            rows = json.load(f)
        print(f"\n  {len(rows)} distinct blits into rows {y0}..{y1}\n")
        print("     x    y  rows planes  width   srcseg     si   bp   calls"
              "   CALLED FROM")
        for k, n in rows:
            ax, by, r, pl, dx, seg, si, mask, bp, caller = k
            print(f"  {ax:4d} {by:4d}  {r:4d} {pl:6d} {dx:6d}  "
                  f"{seg:#07x} {si:#06x} {bp:#04x} {n:6d}   "
                  f"image {caller - 5:#07x}")
        from collections import Counter
        c = Counter(k[9] - 5 for k, n in rows)
        print(f"\n  {len(c)} distinct call sites into the blitter:")
        for site, n in sorted(c.items()):
            print(f"    image {site:#07x}   {n} distinct blits")
        return 0

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
        elif a.start and m.blocked_on_input:
            # F1 from the main menu goes to the briefing. Pressed repeatedly
            # because the menu drops keys while it is still fading in, and
            # because the briefing itself will not take a key at all - it
            # wants a click - so once there, these do nothing and the guest
            # stays put, which is the point.
            sc, asc = KEYMAP[pygame.K_F1]
            m.press_key(sc, asc, True)
            m.press_key(sc, asc, False)

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
