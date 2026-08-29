"""Drive the original's access-code VALIDATOR over chosen codes.

Image 0x051A2 takes a ten-character code back: it checks the checksum,
recovers the level index from characters 7 and 8 as two nibbles, undoes image
0x05042's rotation, rebuilds the index a SECOND time a bit at a time out of
the unrotated characters, and refuses unless the two agree. Then it sets
ds:0x1F6B, divides by 30 into ds:0x1F6E, and decodes a percentage into
ds:0x491D. "BILLANDTED" at image 0x051A6 sets ds:0x1F74.

The port's level_code_ok checked only the checksum, so a typed code changed
nothing - a player found that. This is what the transcription of the rest gets
checked against, because the alternative is the round trip, and CLAUDE.md
already records three deliberate faults that round-tripped happily.

    LEM_HOOK=tools/probe_codein.py LEM_CODEIN_OUT=out/codein.json \\
        uv run python tools/play.py --auto -- --run-seconds 60

Rides along with play.py for the reason probe_code.py does: an in-process run
stops in text mode, and this needs the image unpacked and DS established.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from unicorn import UC_HOOK_CODE                                  # noqa: E402
from unicorn.x86_const import (UC_X86_REG_CS, UC_X86_REG_DS,      # noqa: E402
                               UC_X86_REG_SP, UC_X86_REG_EFLAGS)

ENTRY, RET = 0x051A2, 0x052D3
CELLS = 0x4DF2            # the ten typed cells image 0x051A2 reads
LEVEL, RATING = 0x1F6B, 0x1F6E
PCT, CHEAT = 0x491D, 0x1F74
CODE_LEN = 10
SETTLE = 0x04401          # the menu: by here the image and DS are up
ZF = 0x40


# The generator, image 0x05042, and its return - so the codes this feeds to
# the validator come from THE ORIGINAL rather than from a reimplementation
# here. The first version of this file built them in Python from the measured
# bit table and the original refused 108 of 123: the rotation was the
# DECODER's count, `7 - (index & 7)`, applied while encoding. That is the
# mistake this repository keeps writing down and it wrote it again.
GEN, GEN_RET = 0x05042, 0x051A1
GEN_LEVEL, GEN_OTHER, GEN_PCT = 0x1F6B, 0x491D, 0x40
GEN_OUT = 0x4D68


def drive(m, out_path):
    ds = m.uc.reg_read(UC_X86_REG_DS) * 16
    cs = m.uc.reg_read(UC_X86_REG_CS)
    sp = m.uc.reg_read(UC_X86_REG_SP)

    def call(code):
        m.uc.mem_write(ds + CELLS, code[:CODE_LEN])
        for a in (LEVEL, RATING, PCT, CHEAT):
            m.uc.mem_write(ds + a, b"\x00")
        m.uc.reg_write(UC_X86_REG_CS, cs)
        m.uc.reg_write(UC_X86_REG_SP, sp)
        m.uc.emu_start(cs * 16 + ENTRY, cs * 16 + RET, 0, 0)
        fl = m.uc.reg_read(UC_X86_REG_EFLAGS)
        rd = lambda a: m.uc.mem_read(ds + a, 1)[0]
        return {"ok": 1 if (fl & ZF) else 0, "level": rd(LEVEL),
                "rating": rd(RATING), "pct": rd(PCT), "cheat": rd(CHEAT)}

    def generate(level):
        m.uc.mem_write(ds + GEN_LEVEL, bytes([level]))
        m.uc.mem_write(ds + GEN_OTHER, b"\x00")
        m.uc.mem_write(ds + GEN_PCT, bytes([100]))
        m.uc.mem_write(ds + GEN_OUT, bytes(CODE_LEN))
        m.uc.reg_write(UC_X86_REG_CS, cs)
        m.uc.reg_write(UC_X86_REG_SP, sp)
        m.uc.emu_start(cs * 16 + GEN, cs * 16 + GEN_RET, 0, 0)
        return bytes(m.uc.mem_read(ds + GEN_OUT, CODE_LEN))

    codes = [generate(i) for i in range(120)]
    # and three the validator should REFUSE, plus the cheat
    codes += [b"BILLANDTED", b"AAAAAAAAAA", b".........."]
    # lemmings.cod's Mayhem 2, which the PORT refuses: its characters 7 and 8
    # say index 91 and its bits say 83, so it fails image 0x05292's agreement
    # test. Its checksum is correct by the file's own formula, and its
    # neighbours pass - so either the shipped list has one bad character or
    # both readings of image 0x0523A share a mistake. The original settles it.
    codes.append(b"KNOHGONMHS")
    codes.append(b"NKMFLGCLHP")     # Mayhem 1, which the port accepts
    codes.append(b"ONHGGKONHM")     # Mayhem 3, likewise
    bad = bytearray(codes[12]); bad[3] = bad[3] ^ 1
    codes.append(bytes(bad))

    cases = []
    for code in codes:
        r = call(code)
        r["code"] = code.decode("latin1")
        cases.append(r)
    with open(out_path, "w") as f:
        json.dump(cases, f)
    accepted = sum(c["ok"] for c in cases)
    print(f"  drove image 0x051A2 {len(cases)} times -> {out_path}")
    print(f"    accepted {accepted}, refused {len(cases) - accepted}")


def install(m):
    """Inside the guest, via LEM_HOOK: drive the validator at the menu."""
    base = m.load_seg * 16
    st = {"done": False}

    def at(uc, ad, size, _):
        if ad - base != SETTLE or st["done"]:
            return
        st["done"] = True
        drive(m, os.environ.get("LEM_CODEIN_OUT", "out/codein.json"))
        raise SystemExit(0)

    m.uc.hook_add(UC_HOOK_CODE, at)
