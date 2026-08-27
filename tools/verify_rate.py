# SPDX-License-Identifier: GPL-2.0-only
"""The release-rate buttons against the original's own handler, click by click.

The panel's minus and plus did nothing for a long time: the port read image
0x03F27 as refusing every button below 2, where it actually ROUTES them -
`cmp al,2 / jge 0x3f2e / jmp 0x4032`. A player found that, because nothing
here presses those buttons and no recorded run contains one.

So this drives image 0x04032 itself, the way tools/verify_digits.py drives the
digit routine: the rate is a pure function of three bytes, so set them, point
CS:IP at the handler and read back what it did. No part of the answer is the
port's arithmetic.

The walk is the one a player makes - from the level's own rate up to the
ceiling and back down to the floor - because that is the only way the port's
rate can be moved too: it lives in a pair of globals, not an argument.

    uv run python tools/verify_rate.py

Compares, for every click: ds:0x67 (the rate the panel shows) and ds:0x49 (the
interval the release counts down from, image 0x04053).
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lemmings
from unicorn.x86_const import UC_X86_REG_AX, UC_X86_REG_DS

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

ENTRY, RET = 0x04032, 0x03FEE
PAUSE, FLOOR, RATE, LOCK, STEP = 0x2B, 0x64, 0x67, 0x69, 0x49
CLICKS = 120


def port_walk():
    """The port's own answers, through lemdev --rate-probe."""
    out = subprocess.run(
        [os.path.join(ROOT, "reconstruct", "lemdev"), "--data", "lemming1.pc",
         "--rating", "fun", "--number", "1", "--rate-probe"],
        cwd=ROOT, capture_output=True, text=True,
        env=dict(os.environ, SDL_VIDEODRIVER="dummy")).stdout
    floor, seq = None, []
    for line in out.splitlines():
        f = line.split()
        if len(f) == 2 and f[0] == "floor":
            floor = int(f[1])
        elif len(f) == 3 and f[0] in ("up", "down"):
            seq.append((f[0], int(f[1]), int(f[2])))
    return floor, seq


def guest_walk(m, base, floor):
    """The ORIGINAL's answers: set the bytes, call image 0x04032, read back."""
    ds = m.uc.reg_read(UC_X86_REG_DS) * 16
    def poke(off, v):
        m.uc.mem_write(ds + off, bytes([v]))
    def peek(off):
        return m.uc.mem_read(ds + off, 1)[0]

    poke(PAUSE, 0)          # not paused, image 0x04032
    poke(LOCK, 0)           # the divider clear, image 0x0403D
    poke(FLOOR, floor)      # ds:0x64, image 0x036C1
    poke(RATE, floor)       # ds:0x67, image 0x036C4
    seq = []
    for what, al in (("up", 1),) * CLICKS + (("down", 0),) * CLICKS:
        poke(LOCK, 0)       # the port has no divider; keep it out of the way
        m.uc.reg_write(UC_X86_REG_AX, al)
        try:
            m.uc.emu_start(base + ENTRY, base + RET)
        except Exception as e:
            return seq, f"cpu error after {len(seq)} clicks: {e}"
        seq.append((what, peek(RATE), peek(STEP)))
    return seq, None


def main():
    floor, port = port_walk()
    if floor is None or not port:
        print("  the port printed no walk - that is a failed run, not a pass")
        return 2

    # A machine far enough in that DS holds DGROUP. The handler only ever
    # touches its own bytes, so the menus are far enough - the level does not
    # have to be reached at all, which is what makes this ninety seconds
    # rather than fifteen minutes.
    from dos_emulator import set_game_dir
    import pygame
    from unicorn.x86_const import UC_X86_REG_CS, UC_X86_REG_IP
    set_game_dir(lemmings.GAME_DIR)
    pygame.init()
    m = lemmings.LemmingsVga(lemmings.EXE, max_insns=1 << 62,
                             psp_seg=lemmings.PSP_SEG)
    addr = m.start
    for _ in range(60):
        m.blocked_on_input = False
        try:
            m.uc.emu_start(addr, 0, count=200_000)
        except Exception as e:
            print(f"  the guest would not run: {e}")
            return 2
        addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
        m.service_keyboard()
        if m.uc.reg_read(UC_X86_REG_DS) * 16 == m.load_seg * 16 + 0x9A90:
            break
    else:
        print("  DS never held DGROUP, so nothing was driven. Not a pass.")
        return 2

    guest, err = guest_walk(m, m.load_seg * 16, floor)
    if err:
        print("  " + err)
        return 2

    agree = sum(1 for a, b in zip(port, guest) if a[1:] == b[1:])
    print(f"  the release rate, port against image 0x04032, {len(port)} clicks"
          f" from the level's own {floor}:")
    print(f"    {agree} of {len(port)} agree on both the rate and the interval")
    bad = [(i, a, b) for i, (a, b) in enumerate(zip(port, guest))
           if a[1:] != b[1:]]
    for i, a, b in bad[:6]:
        print(f"    click {i} {a[0]}: port rate {a[1]} step {a[2]}, "
              f"original rate {b[1]} step {b[2]}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
