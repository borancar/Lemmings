# SPDX-License-Identifier: GPL-2.0-only
"""Does the protected code re-encrypt behind itself?

Decides the whole approach. If the plaintext window moves - each instruction
decrypted just before it runs and scrambled again after - then no single memory
snapshot holds the program, static disassembly of the unpacked image is worth
nothing for the protected region, and the code map has to be recorded as
instructions execute rather than read out of memory.

Compares the same byte range at two points in one run.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lemmings
from dos_emulator import set_game_dir
from unicorn import UC_HOOK_CODE
import pygame

set_game_dir(lemmings.GAME_DIR)
pygame.init()

LO, LEN = 0x00600, 0x400          # a slice of the protected region
SHOTS = (2_000_000, 3_000_000, 4_000_000)

m = lemmings.LemmingsVga(lemmings.EXE, max_insns=1 << 62,
                         psp_seg=lemmings.PSP_SEG)
base = m.load_seg * 16
n = [0]
snaps = []
want = list(SHOTS)


def code(uc, addr, size, _):
    n[0] += 1
    if want and n[0] >= want[0]:
        want.pop(0)
        snaps.append((n[0], bytes(uc.mem_read(base + LO, LEN))))
        if not want:
            uc.emu_stop()


m.uc.hook_add(UC_HOOK_CODE, code)

# The start-up menus block on input, so the run has to be driven in chunks
# with the two keypresses fed in - a bare emu_start stops at the first menu.
from dos_emulator import KEYMAP
KEYS = [pygame.K_1, pygame.K_1]
sent = 0
addr = m.start
while want:
    m.blocked_on_input = False
    try:
        m.uc.emu_start(addr, 0, count=200_000)
    except Exception as e:
        print("stopped:", e)
        break
    addr = m._reg(__import__("unicorn").x86_const.UC_X86_REG_CS) * 16 + \
        m._reg(__import__("unicorn").x86_const.UC_X86_REG_IP)
    m.service_keyboard()
    if m.finished:
        print("exited:", m.finished)
        break
    if m.blocked_on_input and sent < len(KEYS):
        sc, asc = KEYMAP[KEYS[sent]]
        m.press_key(sc, asc, True)
        m.press_key(sc, asc, False)
        sent += 1

print(f"range: image {LO:#07x}..{LO+LEN:#07x} ({LEN} bytes)\n")
for i in range(1, len(snaps)):
    (na, a), (nb, b) = snaps[i - 1], snaps[i]
    diff = [j for j in range(LEN) if a[j] != b[j]]
    print(f"insn {na:,} -> {nb:,}: {len(diff)} of {LEN} bytes differ")
    if diff:
        lo, hi = min(diff), max(diff)
        print(f"  first change at image {LO+lo:#07x}, last at {LO+hi:#07x}")
        for j in diff[:6]:
            print(f"    {LO+j:#07x}: {a[j]:02x} -> {b[j]:02x}")
if len(snaps) < 2:
    print("not enough snapshots - the run ended early")
