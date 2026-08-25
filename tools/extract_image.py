# SPDX-License-Identifier: GPL-2.0-only
"""Extract the unpacked, decrypted program image once, so nothing has to
re-run the PKLITE stub and the copy protection to look at the code.

VGALEMMI.EXE is PKLITE-compressed, and parts of it are encrypted until the
protection's single-step stub has run. Neither the file on disk nor a
disassembly of it is the program. This drives the machine far enough that the
image in memory is the real thing, and writes it out with the metadata needed
to make sense of the offsets.

  uv run python tools/extract_image.py              # -> out/vgalemmi.img
  uv run python tools/extract_image.py --stop gfx

What comes out is a **flat image at a known load segment**, not a relocatable
EXE. PKLITE has already applied the relocation table by this point, so absolute
segment values are baked into the code and there is no record of which words
they are. A flat image is what a disassembler needs anyway; `image.json` says
which segment it was built for so the numbers can be reproduced.

The protection stub itself is NOT fully decrypted here and cannot be: it keeps
exactly one instruction in plaintext at a time and re-encrypts behind itself
(see docs/protection.md). Use tools/probe_protected.py to record that stretch
as it executes. Everything outside it - which is nearly all of the game - is
plaintext in this image.
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
from unicorn.x86_const import UC_X86_REG_CS, UC_X86_REG_IP, UC_X86_REG_DS

# Where to stop. Later is better for coverage of decrypted code, but the
# program exits on its own, so "settled" means "as late as we can still get".
STOPS = {
    "menu2": lambda m, s: s["blocked"] == 2,
    "gfx": lambda m, s: m.mode == 0x0D,
    "settled": lambda m, s: m.mode == 0x0D and s["insns"] > 6_000_000,
}


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stop", default="settled", choices=sorted(STOPS))
    ap.add_argument("-o", default="out/vgalemmi.img")
    ap.add_argument("--len", type=lambda v: int(v, 0), default=0x20000)
    a = ap.parse_args()

    set_game_dir(lemmings.GAME_DIR)
    pygame.init()
    m = lemmings.LemmingsVga(lemmings.EXE, max_insns=1 << 62,
                             psp_seg=lemmings.PSP_SEG)
    base = m.load_seg * 16

    from unicorn import UC_HOOK_CODE
    st = {"insns": 0, "blocked": 0}
    m.uc.hook_add(UC_HOOK_CODE, lambda uc, ad, sz, _: st.__setitem__(
        "insns", st["insns"] + 1))

    want = STOPS[a.stop]
    keys = [getattr(pygame, f"K_{k}") for k in lemmings.STARTUP_KEYS]
    addr, sent = m.start, 0
    reached = False
    for _ in range(4000):
        m.blocked_on_input = False
        try:
            m.uc.emu_start(addr, 0, count=200_000)
        except Exception as e:
            print("cpu:", e)
            break
        addr = m._reg(UC_X86_REG_CS) * 16 + m._reg(UC_X86_REG_IP)
        m.service_keyboard()
        if m.blocked_on_input:
            st["blocked"] += 1
        if want(m, st):
            reached = True
            break
        if m.finished:
            print(f"note: program exited ({m.finished}) before '{a.stop}'; "
                  f"dumping what it left behind")
            break
        if m.blocked_on_input and sent < len(keys):
            sc, asc = KEYMAP[keys[sent]]
            m.press_key(sc, asc, True)
            m.press_key(sc, asc, False)
            sent += 1

    blob = bytes(m.uc.mem_read(base, a.len))
    os.makedirs(os.path.dirname(os.path.abspath(a.o)), exist_ok=True)
    with open(a.o, "wb") as f:
        f.write(blob)

    meta = {
        "source": os.path.basename(lemmings.EXE),
        "source_sha256": hashlib.sha256(open(lemmings.EXE, "rb").read()).hexdigest(),
        "image_sha256": hashlib.sha256(blob).hexdigest(),
        "stop": a.stop,
        "reached_stop": reached,
        "instructions_run": st["insns"],
        "length": len(blob),
        "psp_seg": m.psp_seg,
        "load_seg": m.load_seg,
        "image_base_linear": base,
        "dgroup_image_offset": lemmings.DGROUP,
        "video_mode_at_dump": m.mode,
        "note": ("Flat image at load_seg, not a relocatable EXE: PKLITE has "
                 "already applied relocations and there is no record of which "
                 "words they were. cs: offsets are image offsets; a ds: "
                 "reference is dgroup_image_offset + the offset."),
        "protection_note": ("The single-step protection keeps one instruction "
                            "plaintext at a time and re-encrypts behind "
                            "itself, so its own stub is NOT plaintext here. "
                            "See docs/protection.md and "
                            "tools/probe_protected.py."),
    }
    with open(os.path.splitext(a.o)[0] + ".json", "w") as f:
        json.dump(meta, f, indent=2)
        f.write("\n")
    print(f"wrote {len(blob):#x} bytes -> {a.o}")
    for k, v in meta.items():
        if k != "note" and k != "protection_note":
            print(f"  {k} = {v if not isinstance(v, int) else hex(v) if k.endswith(('seg','linear','offset')) else v}")


if __name__ == "__main__":
    main()
