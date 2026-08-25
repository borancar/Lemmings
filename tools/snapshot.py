# SPDX-License-Identifier: GPL-2.0-only
"""Capture the running program's memory, decrypted, for static analysis.

The image on disk is PKLITE-compressed and parts of it are encrypted until the
trap-flag stub has run, so the file is not the program. This drives the machine
to a chosen point and writes out what is actually in memory there.

  uv run python tools/snapshot.py --stop menu2  -o out/menu2.bin
  uv run python tools/snapshot.py --stop gfx    -o out/gfx.bin
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lemmings
from dos_emulator import set_game_dir, KEYMAP
import pygame
from unicorn.x86_const import UC_X86_REG_CS, UC_X86_REG_IP, UC_X86_REG_DS

STOPS = {
    # blocked at the adapter menu, before any key
    "menu1": lambda m, s: s["blocked"] == 1,
    # blocked at the machine-type menu, after answering the first
    "menu2": lambda m, s: s["blocked"] == 2,
    # the game has left text mode for its 16-colour planar screen
    "gfx": lambda m, s: m.mode == 0x0D,
    # well into the graphics stage, past the protection stub
    "settled": lambda m, s: m.mode == 0x0D and s["insns"] > 4_000_000,
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stop", default="settled", choices=sorted(STOPS))
    ap.add_argument("-o", required=True)
    ap.add_argument("--len", type=lambda v: int(v, 0), default=0x20000,
                    help="bytes from the image base (default 128K)")
    ap.add_argument("--keys", default="1,1",
                    help="keys to feed as the menus block (default 1,1)")
    a = ap.parse_args()

    set_game_dir(lemmings.GAME_DIR)
    pygame.init()
    m = lemmings.LemmingsVga(lemmings.EXE, max_insns=1 << 62,
                             psp_seg=lemmings.PSP_SEG)
    base = m.load_seg * 16
    keys = [getattr(pygame, f"K_{k.strip()}") for k in a.keys.split(",") if k.strip()]

    from unicorn import UC_HOOK_CODE
    st = {"insns": 0, "blocked": 0}

    def code(uc, addr, size, _):
        st["insns"] += 1

    m.uc.hook_add(UC_HOOK_CODE, code)
    want = STOPS[a.stop]
    addr, sent = m.start, 0
    for _ in range(6000):
        m.blocked_on_input = False
        try:
            m.uc.emu_start(addr, 0, count=200_000)
        except Exception as e:
            print("cpu:", e)
            break
        addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
        m.service_keyboard()
        if m.finished:
            print("exited:", m.finished)
            break
        if m.blocked_on_input:
            st["blocked"] += 1
        if want(m, st):
            break
        if m.blocked_on_input and sent < len(keys):
            sc, asc = KEYMAP[keys[sent]]
            m.press_key(sc, asc, True)
            m.press_key(sc, asc, False)
            sent += 1
    else:
        print("!! never reached", a.stop)

    os.makedirs(os.path.dirname(os.path.abspath(a.o)), exist_ok=True)
    blob = bytes(m.uc.mem_read(base, a.len))
    open(a.o, "wb").write(blob)
    meta = {
        "stop": a.stop, "insns": st["insns"], "mode": m.mode,
        "image_base": base, "load_seg": m.load_seg, "psp_seg": m.psp_seg,
        "cs": m._reg(UC_X86_REG_CS), "ip": m._reg(UC_X86_REG_IP),
        "ds": m._reg(UC_X86_REG_DS),
    }
    with open(a.o + ".meta", "w") as f:
        for k, v in meta.items():
            f.write(f"{k}={v if isinstance(v, str) else hex(v) if isinstance(v, int) else v}\n")
    print(f"wrote {len(blob):#x} bytes from image base {base:#07x} to {a.o}")
    for k, v in meta.items():
        print(f"  {k} = {v if isinstance(v, str) else (hex(v) if isinstance(v, int) else v)}")


if __name__ == "__main__":
    main()
