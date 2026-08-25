# SPDX-License-Identifier: GPL-2.0-only
"""Disassemble a range of a memory snapshot, in image-offset terms.

  uv run python tools/disasm.py SNAP.bin 0x1116 -n 60
"""
import argparse
import capstone

ap = argparse.ArgumentParser()
ap.add_argument("snap")
ap.add_argument("off", type=lambda v: int(v, 0), help="image offset to start at")
ap.add_argument("-n", type=int, default=40)
ap.add_argument("--bytes", type=lambda v: int(v, 0), default=0)
a = ap.parse_args()

blob = open(a.snap, "rb").read()
span = a.bytes or a.n * 6
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
n = 0
for i in md.disasm(blob[a.off:a.off + span], a.off):
    print(f"  {i.address:#07x}  {i.bytes.hex():<14} {i.mnemonic} {i.op_str}")
    n += 1
    if n >= a.n:
        break
