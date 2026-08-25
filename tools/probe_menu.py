# SPDX-License-Identifier: GPL-2.0-only
"""Drive the start-up menus in-process and report what the guest waits on.

Answers "why does the second menu ignore every key" with measurements rather
than a guess: which input path is live, which DOS and BIOS calls it is making,
and where CS:IP sits while it appears to be stuck.
"""
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lemmings
from dos_emulator import set_game_dir, KEYMAP
from unicorn.x86_const import UC_X86_REG_CS, UC_X86_REG_IP
import pygame

set_game_dir(lemmings.GAME_DIR)
pygame.init()

m = lemmings.LemmingsVga(lemmings.EXE, max_insns=1 << 62,
                         psp_seg=lemmings.PSP_SEG)
base = m.load_seg * 16

KEYS = [pygame.K_1, pygame.K_1, pygame.K_RETURN, pygame.K_RETURN]
sent = 0
addr = m.start
prev_dos = Counter()
prev_int = Counter()

for round_ in range(42):
    m.blocked_on_input = False
    try:
        m.uc.emu_start(addr, 0, count=200_000)
    except Exception as e:
        print("cpu:", e)
        break
    addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
    m.service_keyboard()
    if m.finished:
        print("program exited:", m.finished)
        break

    dos_delta = Counter(m.dos_counts) - prev_dos
    int_delta = Counter(m.int_counts) - prev_int
    prev_dos = Counter(m.dos_counts)
    prev_int = Counter(m.int_counts)

    if round_ % 20 == 0 or m.blocked_on_input:
        cs, ip = m._reg(UC_X86_REG_CS), m._reg(UC_X86_REG_IP)
        print(f"r{round_:<4d} cs:ip={cs:04x}:{ip:04x} img={cs*16+ip-base:#07x} "
              f"blocked={m.blocked_on_input} "
              f"own_kbd={m.guest_owns_keyboard()} "
              f"mode={m.mode:#04x} "
              f"dos={dict(dos_delta)} int={dict(int_delta)}")

    if m.blocked_on_input and sent < len(KEYS):
        k = KEYS[sent]
        sc, asc = KEYMAP[k]
        print(f"      -> pressing {pygame.key.name(k)} "
              f"(scan {sc:#04x} ascii {asc:#04x}) via "
              f"{'IRQ1' if m.guest_owns_keyboard() else 'BIOS buffer'}")
        m.press_key(sc, asc, down=True)
        m.press_key(sc, asc, down=False)
        sent += 1

print("\nfinal dos_counts:", dict(m.dos_counts))
print("final int_counts:", dict(m.int_counts))
print("console:")
print(m.stdout.decode("latin1", "replace"))

# --- what the spin loop actually is -------------------------------------
import capstone
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
cs = m._reg(UC_X86_REG_CS)
from unicorn.x86_const import UC_X86_REG_EFLAGS
fl = m.uc.reg_read(UC_X86_REG_EFLAGS)
print(f"\nflags={fl:#06x}  TF={'SET' if fl & 0x100 else 'clear'} "
      f"IF={'set' if fl & 0x200 else 'clear'}")
import struct as _s
for v in (0x01, 0x09, 0x08, 0x1C):
    off, seg = _s.unpack("<HH", bytes(m.uc.mem_read(v * 4, 4)))
    print(f"  IVT {v:#04x} -> {seg:04x}:{off:04x}")
lo = cs * 16 + (m._reg(UC_X86_REG_IP) & ~0xF) - 0x20
print("\ndisassembly around current PC:")
for i in md.disasm(bytes(m.uc.mem_read(lo, 0x60)), lo):
    print(f"  {cs:04x}:{i.address-cs*16:04x}  img{i.address-base:#08x}  "
          f"{i.bytes.hex():<12} {i.mnemonic} {i.op_str}")

print("\nports read:", {hex(p): c for p, c in m.port_in.items()})
print("ports written:", {hex(p): c for p, c in m.port_out.items()})
import struct
ds = m._reg(__import__("unicorn").x86_const.UC_X86_REG_DS)
for name, seg in (("cs", cs), ("ds", ds)):
    v = struct.unpack("<H", bytes(m.uc.mem_read(seg * 16 + 0x1fda, 2)))[0]
    print(f"  [{name}:0x1fda] = {v:#06x}  -> port {v + 6:#06x}")
print("BIOS 0040:0063 (CRTC base) =",
      hex(struct.unpack("<H", bytes(m.uc.mem_read(0x463, 2)))[0]))
