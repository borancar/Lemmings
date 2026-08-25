# SPDX-License-Identifier: GPL-2.0-only
"""Find what calls a given image offset.

Recursive descent needs this constantly: you land in the middle of a routine
by watching memory or by a breakpoint, and the next question is always who
reaches it. Scans for near and far CALLs whose target is the address, and for
JMPs, which matter because a tail call is still a caller.

  uv run python tools/callers.py 0x8b93
"""
import argparse
import struct

ap = argparse.ArgumentParser(
    description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("target", type=lambda v: int(v, 0))
ap.add_argument("--image", default="out/vgalemmi.decrypted.img")
ap.add_argument("--window", type=lambda v: int(v, 0), default=0,
                help="also report targets within this many bytes")
a = ap.parse_args()

img = open(a.image, "rb").read()
lo = a.target - a.window
hi = a.target + a.window

hits = []
for i in range(len(img) - 3):
    op = img[i]
    if op == 0xE8:                                  # call near, rel16
        rel = struct.unpack_from("<h", img, i + 1)[0]
        dst = (i + 3 + rel) & 0xFFFF
        kind = "call"
    elif op == 0xE9:                                # jmp near, rel16
        rel = struct.unpack_from("<h", img, i + 1)[0]
        dst = (i + 3 + rel) & 0xFFFF
        kind = "jmp"
    elif op == 0xEB:                                # jmp short
        rel = struct.unpack_from("<b", img, i + 1)[0]
        dst = (i + 2 + rel) & 0xFFFF
        kind = "jmp short"
    else:
        continue
    # The image is larger than 64K, so a near target is within the same
    # segment - compare on the low 16 bits of both.
    if lo & 0xFFFF <= dst <= hi & 0xFFFF:
        hits.append((i, kind, dst))

print(f"{len(hits)} reference(s) to {a.target:#07x}"
      f"{'' if not a.window else f' +/- {a.window}'}:")
for at, kind, dst in hits:
    print(f"  img{at:#08x}  {kind:<9} -> {dst:#07x}")
