#!/usr/bin/env python3
"""Who starts the music, and with which tune.

Image 0x003AD asks the driver for a tune (AH=3) and then runs one tick
(AH=0); image 0x04840 picks the next tune from a seventeen-entry rotation at
image 0x04877, and image 0x04866 picks one by number from image 0x04888. None
of the three has a single near call anywhere in the image, so a static search
cannot say when the game plays what - and the port, which never transcribed
any of them, simply never asks for music at all.

So ask the machine. Each address gets a one-instruction UC_HOOK_CODE range -
Unicorn only pays for the addresses named, unlike a whole-image code hook,
which costs so much throughput here that the guest never reaches the music -
and the near return address is read off the stack the way probe_blits.py
reads the blitter's caller.

    LEM_HOOK=tools/probe_music.py \
        uv run python tools/play.py --auto --start -- --run-seconds 240
"""
import atexit
import collections

DGROUP = 0x9A90

WATCH = {
    0x00380: "effect 3        (image 0x00391)",
    0x00394: "effect 2        (image 0x003AA), and sets ds:0x55 = 0xFF",
    0x003AD: "START THE MUSIC (image 0x003B9, then a tick at 0x003BE)",
    0x04840: "next tune in the rotation  (ds:0x1F6C -> image 0x04877)",
    0x04866: "tune for a number          (image 0x04888)",
}


def install(m):
    from unicorn import UC_HOOK_CODE
    from unicorn.x86_const import (UC_X86_REG_SS, UC_X86_REG_SP,
                                   UC_X86_REG_DS, UC_X86_REG_AL)
    seen = collections.Counter()
    order = []
    base = m.load_seg * 16

    def make(off, what):
        def hit(uc, address, size, user):
            ss = uc.reg_read(UC_X86_REG_SS)
            sp = uc.reg_read(UC_X86_REG_SP)
            ret = int.from_bytes(uc.mem_read((ss << 4) + sp, 2), "little")
            ds = uc.reg_read(UC_X86_REG_DS)
            base = ds << 4
            tune = uc.mem_read(base + 0x54, 1)[0]
            gate = uc.mem_read(base + 0x25A7, 1)[0]
            rot = uc.mem_read(base + 0x1F6C, 2)
            key = (off, ret)
            if key not in seen:
                order.append((off, what, ret, tune, gate, rot[0], rot[1],
                              uc.reg_read(UC_X86_REG_AL)))
            seen[key] += 1
        return hit

    # begin/end are LINEAR addresses, not image offsets. Passing the offset
    # raw watches five bytes near the bottom of conventional memory that this
    # program never executes, and the probe then reports - perfectly
    # truthfully - that none of the routines ran. It is the same class of
    # mistake as reading an image offset in a file and forgetting the EXE
    # header: the instrument is wrong and the result looks like a finding.
    base = m.load_seg * 16
    for off, what in WATCH.items():
        m.uc.hook_add(UC_HOOK_CODE, make(off, what),
                      begin=base + off, end=base + off)

    def report():
        print()
        print("  [music] first time each caller reached each routine:")
        if not order:
            print("  [music] NONE OF THEM RAN. That is a fact about this run -")
            print("  [music] it never got as far as any music - and not a")
            print("  [music] finding about the game.")
            return
        for off, what, ret, tune, gate, i6c, i6d, al in order:
            print("  [music]  image 0x%05X  %s" % (off, what))
            print("  [music]      called from 0x%05X, al=%02X, "
                  "ds:0x54=%02X ds:0x25A7=%02X ds:0x1F6C=%02X ds:0x1F6D=%02X"
                  % (ret, al, tune, gate, i6c, i6d))
        print()
        print("  [music] call counts:")
        for (off, ret), n in seen.most_common():
            print("  [music]   0x%05X from 0x%05X : %d" % (off, ret, n))

    atexit.register(report)
    print("  [music] watching %d addresses" % len(WATCH))
