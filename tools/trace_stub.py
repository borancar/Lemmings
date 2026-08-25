"""Trace the first N instructions of a program, to see what a packer stub does.

Scratch diagnostic: it answers "where does the unpacker go wrong", which is a
question you only ask once per game.
"""
import sys, argparse
from dos_emulator import DosMachine, set_game_dir
from unicorn import UC_HOOK_CODE
import capstone

ap = argparse.ArgumentParser()
ap.add_argument("exe")
ap.add_argument("-n", type=int, default=200)
ap.add_argument("--from-insn", type=int, default=0)
a = ap.parse_args()

set_game_dir(__import__("os").path.dirname(__import__("os").path.abspath(a.exe)))
m = DosMachine(a.exe, verbose=False, max_insns=a.n)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)

R = {}
from unicorn.x86_const import (UC_X86_REG_AX, UC_X86_REG_BX, UC_X86_REG_CX,
    UC_X86_REG_DX, UC_X86_REG_SI, UC_X86_REG_DI, UC_X86_REG_BP, UC_X86_REG_SP,
    UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS)
REGS = [("ax",UC_X86_REG_AX),("bx",UC_X86_REG_BX),("cx",UC_X86_REG_CX),
        ("dx",UC_X86_REG_DX),("si",UC_X86_REG_SI),("di",UC_X86_REG_DI),
        ("bp",UC_X86_REG_BP),("sp",UC_X86_REG_SP),("cs",UC_X86_REG_CS),
        ("ds",UC_X86_REG_DS),("es",UC_X86_REG_ES),("ss",UC_X86_REG_SS)]

n = [0]
base = m.load_seg * 16
def hook(uc, addr, size, _):
    n[0] += 1
    if n[0] < a.from_insn:
        return
    if n[0] > a.n:
        uc.emu_stop(); return
    try:
        code = uc.mem_read(addr, min(size, 15))
    except Exception:
        uc.emu_stop(); return
    cs = uc.reg_read(UC_X86_REG_CS)
    ip = addr - cs * 16
    txt = "??"
    for i in md.disasm(bytes(code), addr):
        txt = f"{i.mnemonic} {i.op_str}"; break
    regs = " ".join(f"{nm}={uc.reg_read(r):04x}" for nm, r in REGS)
    print(f"{n[0]:6d} {cs:04x}:{ip:04x} lin={addr:06x} img={addr-base:+#08x}  {txt:<34} {regs}")

m.uc.hook_add(UC_HOOK_CODE, hook)
print(f"load_seg={m.load_seg:#06x} base={base:#07x} start={m.start:#07x} "
      f"image_off={m.start-base:#x}")
try:
    m.run()
except Exception as e:
    print("stopped:", e)
