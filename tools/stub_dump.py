"""Run to the Nth instruction, then dump registers and disassemble around PC."""
import os, argparse
from dos_emulator import DosMachine, set_game_dir
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import *
import capstone

ap = argparse.ArgumentParser()
ap.add_argument("exe")
ap.add_argument("--at", type=int, required=True, help="stop at this instruction count")
ap.add_argument("--back", type=lambda v: int(v,0), default=0x40)
ap.add_argument("--n", type=int, default=48)
a = ap.parse_args()

set_game_dir(os.path.dirname(os.path.abspath(a.exe)))
m = DosMachine(a.exe, verbose=False, max_insns=1 << 62)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
n = [0]
def code(uc, addr, size, _):
    n[0] += 1
    if n[0] >= a.at:
        uc.emu_stop()
m.uc.hook_add(UC_HOOK_CODE, code)
try:
    m.run()
except Exception as e:
    print("stopped:", e)

uc = m.uc
REGS = [("ax",UC_X86_REG_AX),("bx",UC_X86_REG_BX),("cx",UC_X86_REG_CX),
        ("dx",UC_X86_REG_DX),("si",UC_X86_REG_SI),("di",UC_X86_REG_DI),
        ("bp",UC_X86_REG_BP),("sp",UC_X86_REG_SP),("cs",UC_X86_REG_CS),
        ("ds",UC_X86_REG_DS),("es",UC_X86_REG_ES),("ss",UC_X86_REG_SS),
        ("ip",UC_X86_REG_IP)]
print("insn#", n[0])
print("  " + "  ".join(f"{nm}={uc.reg_read(r):04x}" for nm, r in REGS))
cs = uc.reg_read(UC_X86_REG_CS); ip = uc.reg_read(UC_X86_REG_IP)
start = cs * 16 + ip - a.back
code_b = bytes(uc.mem_read(start, a.back + a.n * 6))
here = cs * 16 + ip
for i in md.disasm(code_b, start):
    mark = "->" if i.address == here else "  "
    print(f"{mark} {cs:04x}:{i.address-cs*16:04x}  {i.bytes.hex():<14} {i.mnemonic} {i.op_str}")
