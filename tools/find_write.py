"""Report the first guest write into a linear range, with the instruction that did it."""
import os, argparse
from dos_emulator import DosMachine, set_game_dir
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.x86_const import UC_X86_REG_CS, UC_X86_REG_IP
import capstone

ap = argparse.ArgumentParser()
ap.add_argument("exe")
ap.add_argument("--lo", type=lambda v: int(v, 0), default=0xA0000)
ap.add_argument("--hi", type=lambda v: int(v, 0), default=0xC0000)
ap.add_argument("--max", type=int, default=40_000_000)
ap.add_argument("--hits", type=int, default=12)
a = ap.parse_args()

set_game_dir(os.path.dirname(os.path.abspath(a.exe)))
m = DosMachine(a.exe, verbose=False, max_insns=a.max)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
base = m.load_seg * 16
cur = [0, 0]
n = [0]
def code(uc, addr, size, _):
    n[0] += 1
    cur[0] = addr
def wr(uc, access, addr, size, value, _):
    if not (a.lo <= addr < a.hi):
        return
    hits[0] += 1
    pc = cur[0]
    try:
        txt = next(iter(md.disasm(bytes(uc.mem_read(pc, 15)), pc)))
        txt = f"{txt.mnemonic} {txt.op_str}"
    except Exception:
        txt = "??"
    cs = uc.reg_read(UC_X86_REG_CS)
    print(f"insn#{n[0]:<10d} pc={cs:04x}:{pc-cs*16:04x} (img{pc-base:+#x})  "
          f"write [{addr:06x}] <- {value:#x} ({size}B)   {txt}")
    if hits[0] >= a.hits:
        uc.emu_stop()
hits = [0]
m.uc.hook_add(UC_HOOK_CODE, code)
m.uc.hook_add(UC_HOOK_MEM_WRITE, wr)
print(f"load_seg={m.load_seg:#06x} base={base:#07x}")
try:
    m.run()
except Exception as e:
    print("stopped:", e)
print(f"total instructions: {n[0]}")
