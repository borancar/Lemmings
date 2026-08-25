# SPDX-License-Identifier: GPL-2.0-only
"""Produce a permanently decrypted program image.

The protection keeps exactly one instruction in plaintext at a time and
re-encrypts behind itself (docs/protection.md), so no memory dump ever holds
the protected stretch in the clear - which is why a snapshot disassembles as
garbage around image 0x0D00-0x0E40.

But every instruction *is* plaintext for the moment it executes. So: record
the bytes of each instruction as the CPU runs it, then write those bytes back
into the dumped image. What comes out is an image where the protected code is
readable too, and it never has to be produced again.

  uv run python tools/decrypt_image.py
  uv run python tools/disasm.py out/vgalemmi.decrypted.img 0xdff -n 40

Only code that actually ran can be recovered this way - that is inherent, not
a shortcoming of the method. The report says how many bytes were recovered and
how many of them differed from the raw dump, which is exactly the amount the
protection was hiding.
"""
import argparse
import hashlib
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lemmings
from dos_emulator import set_game_dir, KEYMAP
import pygame
import capstone
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_CS, UC_X86_REG_IP


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", default="out/vgalemmi.decrypted.img")
    ap.add_argument("--len", type=lambda v: int(v, 0), default=0x20000)
    ap.add_argument("--rounds", type=int, default=200)
    a = ap.parse_args()

    set_game_dir(lemmings.GAME_DIR)
    pygame.init()
    m = lemmings.LemmingsVga(lemmings.EXE, max_insns=1 << 62,
                             psp_seg=lemmings.PSP_SEG)
    base = m.load_seg * 16
    top = base + a.len

    # image offset -> the instruction's bytes, as first seen executing.
    seen = {}
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
    read = m.uc.mem_read

    def code(uc, addr, size, _):
        if base <= addr < top:
            off = addr - base
            if off not in seen:
                try:
                    seen[off] = bytes(read(addr, size))
                except Exception:
                    pass

    m.uc.hook_add(UC_HOOK_CODE, code)

    keys = [getattr(pygame, f"K_{k}") for k in lemmings.STARTUP_KEYS]
    addr, sent = m.start, 0
    for _ in range(a.rounds):
        m.blocked_on_input = False
        try:
            m.uc.emu_start(addr, 0, count=200_000)
        except Exception as e:
            print("cpu:", e)
            break
        addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
        m.service_keyboard()
        if m.finished:
            print(f"program exited: {m.finished}")
            break
        if m.blocked_on_input and sent < len(keys):
            sc, asc = KEYMAP[keys[sent]]
            m.press_key(sc, asc, True)
            m.press_key(sc, asc, False)
            sent += 1

    raw = bytearray(m.uc.mem_read(base, a.len))
    patched = bytearray(raw)
    recovered = differed = 0
    ranges = []
    for off in sorted(seen):
        b = seen[off]
        if off + len(b) > len(patched):
            continue
        recovered += len(b)
        if patched[off:off + len(b)] != b:
            differed += len(b)
            ranges.append(off)
        patched[off:off + len(b)] = b

    os.makedirs(os.path.dirname(os.path.abspath(a.o)), exist_ok=True)
    with open(a.o, "wb") as f:
        f.write(patched)

    # Where the protection was actually hiding something, as spans.
    spans = []
    for off in ranges:
        if spans and off <= spans[-1][1] + 16:
            spans[-1][1] = off
        else:
            spans.append([off, off])

    meta = {
        "source": os.path.basename(lemmings.EXE),
        "image_sha256": hashlib.sha256(bytes(patched)).hexdigest(),
        "length": len(patched),
        "load_seg": m.load_seg,
        "psp_seg": m.psp_seg,
        "dgroup_image_offset": lemmings.DGROUP,
        "instructions_recovered": len(seen),
        "bytes_recovered": recovered,
        "bytes_that_differed_from_the_dump": differed,
        "encrypted_spans": [[hex(a_), hex(b_)] for a_, b_ in spans],
        "note": ("Only code that executed is recovered; the rest is the raw "
                 "memory dump. Bytes that differed are exactly what the "
                 "protection was hiding."),
    }
    with open(os.path.splitext(a.o)[0] + ".json", "w") as f:
        json.dump(meta, f, indent=2)
        f.write("\n")

    print(f"\nwrote {a.o}")
    print(f"  distinct instructions executed in the image : {len(seen)}")
    print(f"  bytes recovered                             : {recovered}")
    print(f"  of those, differing from the raw dump       : {differed}")
    print(f"  encrypted spans (image offsets)             : "
          f"{[f'{x:#07x}..{y:#07x}' for x, y in spans]}")


if __name__ == "__main__":
    main()
