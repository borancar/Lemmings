# SPDX-License-Identifier: GPL-2.0-only
"""Check the port's DAT decompressor against the original's, call by call.

This is the differential test the method asks for, and it is a different claim
from "the port renders the level correctly". It stops the original at the entry
to its decompressor (image 0x01BAF), captures the inputs, lets the **original's
own body** run to its `ret` at 0x01C79, captures what it produced, then runs the
port's C on the same inputs and compares byte for byte.

It needs no determinism to mean anything: both sides are the same call inside
one run, so the host clock and whatever the guest does elsewhere cannot make it
flaky.

  uv run python tools/verify_dat.py            # every call in a start-up run
  uv run python tools/verify_dat.py --max 3    # just the first few

Where the inputs actually are, which took a wrong turn worth recording:

0x01BAF is not handed its data. `call 0x1D2E` is a **read** - buffer in DX,
count in CX - and the routine calls it twice: once at 0x01BB9 for the 10-byte
header, then again at 0x01BE8 for the compressed bytes, into the *same* buffer.
Sampling DS:SI at entry therefore reads whatever the previous section left
behind. It did: the first call showed the AdLib driver's opening bytes sitting
in the buffer and a "decompressed size" of 59662, and the port dutifully
refused all three calls. The state is only real once both reads have happened.

So the capture is in three places:

  0x01BC1  the header has just been read.  [SI+8..9] is the section size,
           BIG-endian, and the compressed run is that minus the 10 header bytes
  0x01BF2  the compressed bytes have just been read, into the same buffer at
           DS:SI; SI has been advanced to one past their end.
           CH = bits usable in the last byte, plus one.  DH = the checksum.
           BP = where the output starts, DI = where it ends
  0x01C79  the `ret`: the output buffer holds what the original produced
"""
import argparse
import ctypes
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lemmings
from dos_emulator import set_game_dir, KEYMAP
import pygame
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import (UC_X86_REG_CS, UC_X86_REG_IP, UC_X86_REG_SI,
                               UC_X86_REG_DI, UC_X86_REG_DS, UC_X86_REG_ES,
                               UC_X86_REG_BP, UC_X86_REG_CX, UC_X86_REG_DX)

HDR_READY = 0x01BC1      # the header read has returned
DATA_READY = 0x01BF2     # the compressed bytes are in, decode is about to run
RET = 0x01C79            # the `ret`, after `and dh, dh` and `cld`
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def build_lib(path):
    src = os.path.join(ROOT, "reconstruct", "src", "dat.c")
    cmd = ["cc", "-shared", "-fPIC", "-O2", "-Wall", "-Wextra",
           "-Wconversion", "-o", path, src]
    subprocess.run(cmd, check=True)
    lib = ctypes.CDLL(path)
    lib.dat_decompress.restype = ctypes.c_int
    lib.dat_decompress.argtypes = [
        ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint32,
        ctypes.c_uint8, ctypes.c_uint8,
        ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_char_p)]
    return lib


def be16(b, i):
    return (b[i] << 8) | b[i + 1]


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--max", type=int, default=0, help="stop after N calls")
    ap.add_argument("--rounds", type=int, default=400)
    a = ap.parse_args()

    lib = build_lib(os.path.join(ROOT, "out", "libdat.so"))

    set_game_dir(lemmings.GAME_DIR)
    pygame.init()
    m = lemmings.LemmingsVga(lemmings.EXE, max_insns=1 << 62,
                             psp_seg=lemmings.PSP_SEG)
    base = m.load_seg * 16
    st = {"pending": None, "hdr": None,
          "n": 0, "ok": 0, "bad": 0, "done": False}

    def at(uc, ad, size, _):
        off = ad - base
        if off == HDR_READY:
            ds = uc.reg_read(UC_X86_REG_DS)
            si = uc.reg_read(UC_X86_REG_SI)
            st["hdr"] = bytes(uc.mem_read(ds * 16 + si, 10))
        elif off == DATA_READY and st.get("hdr") is not None:
            hdr = st["hdr"]
            sec_size = be16(hdr, 8)
            if sec_size < 10:
                return
            comp_len = sec_size - 10
            ds = uc.reg_read(UC_X86_REG_DS)
            es = uc.reg_read(UC_X86_REG_ES)
            si = uc.reg_read(UC_X86_REG_SI)
            di = uc.reg_read(UC_X86_REG_DI)
            bp = uc.reg_read(UC_X86_REG_BP)
            cx = uc.reg_read(UC_X86_REG_CX)
            dx = uc.reg_read(UC_X86_REG_DX)
            dec_size = (di - bp) & 0xFFFF
            if dec_size == 0:
                return
            comp = bytes(uc.mem_read(ds * 16 + si - comp_len, comp_len))
            bits_in_first = ((cx >> 8) & 0xFF) - 1
            checksum = (dx >> 8) & 0xFF
            st["pending"] = (es * 16 + bp, bits_in_first, checksum,
                             comp, dec_size, sec_size)
        elif off == RET and st["pending"] is not None:
            out_at, bits_in_first, checksum, comp, dec_size, sec_size = \
                st["pending"]
            st["pending"] = None
            st["n"] += 1
            theirs = bytes(uc.mem_read(out_at, dec_size))

            buf = (ctypes.c_uint8 * dec_size)()
            cbuf = (ctypes.c_uint8 * len(comp)).from_buffer_copy(comp)
            err = ctypes.c_char_p()
            rc = lib.dat_decompress(cbuf, len(comp),
                                    bits_in_first & 0xFF, checksum,
                                    buf, dec_size, ctypes.byref(err))
            ours = bytes(buf)

            if rc != 0:
                st["bad"] += 1
                msg = err.value.decode() if err.value else "?"
                print(f"  call {st['n']:2d}  dec_size {dec_size:6d}  "
                      f"PORT REFUSED: {msg}")
            elif ours == theirs:
                st["ok"] += 1
                print(f"  call {st['n']:2d}  dec_size {dec_size:6d}  "
                      f"comp {len(comp):6d}  AGREE")
            else:
                st["bad"] += 1
                diff = sum(1 for x, y in zip(ours, theirs) if x != y)
                first = next(i for i, (x, y) in enumerate(zip(ours, theirs))
                             if x != y)
                print(f"  call {st['n']:2d}  dec_size {dec_size:6d}  "
                      f"DIFFER in {diff} bytes, first at {first:#x}")
            if a.max and st["n"] >= a.max:
                st["done"] = True
                uc.emu_stop()

    m.uc.hook_add(UC_HOOK_CODE, at)

    keys = [getattr(pygame, f"K_{k}") for k in lemmings.STARTUP_KEYS]
    addr, sent = m.start, 0
    print(f"comparing the port against the original at image "
          f"{DATA_READY:#07x}\n")
    for _ in range(a.rounds):
        m.blocked_on_input = False
        try:
            m.uc.emu_start(addr, 0, count=200_000)
        except Exception as e:
            print("cpu:", e)
            break
        if st["done"]:
            break
        addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
        m.service_keyboard()
        if m.finished:
            break
        if m.blocked_on_input and sent < len(keys):
            sc, asc = KEYMAP[keys[sent]]
            m.press_key(sc, asc, True)
            m.press_key(sc, asc, False)
            sent += 1

    print(f"\n  calls compared : {st['n']}")
    print(f"  agreed         : {st['ok']}")
    print(f"  differed       : {st['bad']}")
    if st["n"] == 0:
        print("\n  NOTHING WAS COMPARED. That is a failure, not a pass -"
              "\n  the run never reached the decompressor.")
        return 2
    return 0 if st["bad"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
