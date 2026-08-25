"""Run to instruction N, then dump a linear memory range to a file."""
import os, argparse
from dos_emulator import DosMachine, set_game_dir
from unicorn import UC_HOOK_CODE

ap = argparse.ArgumentParser()
ap.add_argument("exe")
ap.add_argument("--at", type=int, required=True)
ap.add_argument("--lo", type=lambda v: int(v, 0), required=True)
ap.add_argument("--len", type=lambda v: int(v, 0), required=True)
ap.add_argument("-o", required=True)
a = ap.parse_args()
set_game_dir(os.path.dirname(os.path.abspath(a.exe)))
m = DosMachine(a.exe, verbose=False, max_insns=1 << 62)
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
open(a.o, "wb").write(bytes(m.uc.mem_read(a.lo, a.len)))
print(f"insn#{n[0]} wrote {a.len:#x} bytes from {a.lo:#07x} to {a.o}")
