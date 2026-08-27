"""Every instruction that touches one DGROUP byte or word, listed.

The pause flag is why this exists. `ds:0x2B` guards eight places in the
original and the port had seven of them; the eighth - image 0x03944, in front
of the object animations - meant a paused game kept animating its traps, and
the two never came back into step afterwards. No frame-by-frame comparison
could see it, because every long comparison here is of a level nobody touches.

What found it was enumerating every read of the flag and checking each against
the port, one at a time. That is a method rather than a lucky look, and it
generalises to any byte the game uses as state: list the sites, walk them, and
the answer is a COUNT rather than an impression.

    uv run python tools/xref.py 0x2b          # a byte
    uv run python tools/xref.py 0x2b --disasm # with the instruction after it

It matches every 8086 form that names an absolute DGROUP offset directly: the
accumulator short forms, and any modrm opcode whose mod is 00 and rm is 110.
It does NOT find an access made through a register - `mov bx,0x2b / cmp
[bx],al` - so a count of zero means "no direct reference", not "never
touched". Say which you mean when quoting it.
"""
import argparse
import subprocess
import sys
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
IMG = os.path.join(ROOT, "image", "vgalemmi.decrypted.img")


# THE OPCODES THAT TAKE A MODRM. An absolute DGROUP reference is any of
# these followed by a modrm byte whose mod is 00 and rm is 110 - that is
# `modrm & 0xC7 == 0x06` - and then the sixteen-bit offset.
#
# The first version of this matched only the AL/AX short forms (0xA0..0xA3)
# and a hand-written list of `op [x],imm`, and it reported ds:0x4D as having
# ONE reference: the store at image 0x0042C. The read at image 0x06ECC is
# `mov cl,[0x4d]`, which is 8A with a modrm, and it was invisible. A sweep
# that under-reports reads exactly like a sweep that found everything, which
# is the whole reason this file exists - so the matcher is general now, and
# it was checked against a case whose answer was already known.
MODRM_OPS = {
    0x00: "add", 0x01: "add", 0x02: "add", 0x03: "add",
    0x08: "or",  0x09: "or",  0x0A: "or",  0x0B: "or",
    0x10: "adc", 0x11: "adc", 0x12: "adc", 0x13: "adc",
    0x18: "sbb", 0x19: "sbb", 0x1A: "sbb", 0x1B: "sbb",
    0x20: "and", 0x21: "and", 0x22: "and", 0x23: "and",
    0x28: "sub", 0x29: "sub", 0x2A: "sub", 0x2B: "sub",
    0x30: "xor", 0x31: "xor", 0x32: "xor", 0x33: "xor",
    0x38: "cmp", 0x39: "cmp", 0x3A: "cmp", 0x3B: "cmp",
    0x84: "test", 0x85: "test", 0x86: "xchg", 0x87: "xchg",
    0x88: "mov", 0x89: "mov", 0x8A: "mov", 0x8B: "mov",
    0x8C: "mov seg", 0x8D: "lea", 0x8E: "mov seg",
    0x80: "grp1", 0x81: "grp1", 0x83: "grp1",
    0xC6: "mov imm", 0xC7: "mov imm",
    0xF6: "grp3", 0xF7: "grp3",
    0xFE: "grp4", 0xFF: "grp5",
}
# The accumulator short forms, which carry no modrm at all.
SHORT = {0xA0: "mov al,[x]", 0xA1: "mov ax,[x]",
         0xA2: "mov [x],al", 0xA3: "mov [x],ax"}


def find(d, off):
    """Every direct absolute reference to `off`, as (address, mnemonic)."""
    a = bytes([off & 0xFF, (off >> 8) & 0xFF])
    hits = []
    for i in range(len(d) - 3):
        b = d[i]
        if b in SHORT:
            if d[i + 1:i + 3] == a:
                hits.append((i, SHORT[b]))
            continue
        if b in MODRM_OPS and (d[i + 1] & 0xC7) == 0x06:
            if d[i + 2:i + 4] == a:
                hits.append((i, MODRM_OPS[b]))
    return hits


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("offset", help="DGROUP offset, e.g. 0x2b")
    ap.add_argument("--disasm", action="store_true",
                    help="disassemble one instruction at each site")
    a = ap.parse_args()
    off = int(a.offset, 0)
    d = open(IMG, "rb").read()

    hits = find(d, off)
    kind = "word or byte"
    print(f"ds:0x{off:04x} ({kind}): {len(hits)} direct reference(s)")
    if not hits:
        print("  none - which means no DIRECT reference, not that nothing "
              "touches it; an access through a register is invisible here")
        return
    for addr, name in hits:
        print(f"  image 0x{addr:05X}  {name}")
        if a.disasm:
            out = subprocess.run(
                [sys.executable, os.path.join(HERE, "disasm.py"), IMG,
                 hex(addr), "-n", "2"], capture_output=True, text=True)
            for line in out.stdout.splitlines()[-2:]:
                print("      " + line.strip())


if __name__ == "__main__":
    main()
