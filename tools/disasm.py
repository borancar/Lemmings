# SPDX-License-Identifier: GPL-2.0-only
"""Disassemble a range of a program image, in image-offset terms.

Defaults to out/vgalemmi.decrypted.img, which is the one to use: the raw
dump has the protection's own code still encrypted. Build it once with
tools/decrypt_image.py.

  uv run python tools/decrypt_image.py
  uv run python tools/disasm.py 0x1116 -n 60
  uv run python tools/disasm.py out/vgalemmi.img 0x1116 -n 60
"""
import argparse
import capstone

ap = argparse.ArgumentParser(
    description=__doc__,
    formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("snap", nargs="?", default="out/vgalemmi.decrypted.img",
                help="image to read (default: the decrypted image, if built)")
ap.add_argument("off", type=lambda v: int(v, 0), default=0, nargs="?",
                help="image offset to start at")
ap.add_argument("-n", type=int, default=40)
ap.add_argument("--bytes", type=lambda v: int(v, 0), default=0)
a = ap.parse_args()

# `disasm.py 0x1116` - a bare offset, with the default image.
if a.snap is not None and a.snap.lower().startswith(("0x", "0X")) or (
        a.snap and a.snap.isdigit() and a.off == 0):
    a.snap, a.off = "out/vgalemmi.decrypted.img", int(a.snap, 0)

blob = open(a.snap, "rb").read()
span = a.bytes or a.n * 6
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
n = 0
for i in md.disasm(blob[a.off:a.off + span], a.off):
    print(f"  {i.address:#07x}  {i.bytes.hex():<14} {i.mnemonic} {i.op_str}")
    n += 1
    if n >= a.n:
        break
