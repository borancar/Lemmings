"""The miner's masks, read back out of the original's own sprite store.

game.c's MINE_MASK says it was dumped rather than read by eye, and the miner
still clears ONE pixel the original leaves: at (159, 81) on "Just dig!" driven
by out/s8.txt, the original holds terrain 2 and the port holds 0, on 137 of
400 frames - every frame from the first slice until something else covers it.

One pixel is exactly what a single wrong BIT looks like. A misplaced mask
would differ by dozens; a wrong bit only shows where the ground under it is
still solid and no later slice clears it, which happens once in a tunnel.

So dump it again and compare, rather than clearing the bit that would make the
number go to zero - which is fitting, and this project has a note about what
that costs.

Image 0x06085 bases the masks at ds:0x1FE8:0x780 - where the basher's 1,920
bytes end - indexes slice * 312, and image 0x060E2 adds 0x270 for one facing
left. 312 bytes is eight shifts of thirteen rows of three.

    LEM_EXTRA_HOOK=$PWD/tools/probe_mine.py \
    uv run python tools/record_level.py --frames 30 --out out/mine --seconds 240
"""
import atexit
import os

FRAME_END = 0x018F
STORE_SEG = 0x1FE8       # ds:0x1FE8 holds the segment, image 0x06085
BASE      = 0x780
LEFT      = 0x270
SLICE     = 312
ROWS      = 13
OUT = os.environ.get("LEM_MINE_OUT", "out/mine_mask.txt")
DGROUP = 0x9A90


def install(m):
    from unicorn import UC_HOOK_CODE

    base = m.load_seg * 16
    st = {"n": 0, "raw": None}

    def on_frame(uc, ad, size, _):
        if ad - base != FRAME_END:
            return
        st["n"] += 1
        if st["n"] != 20 or st["raw"] is not None:
            return
        w = bytes(uc.mem_read(base + DGROUP + STORE_SEG, 2))
        seg = w[0] | (w[1] << 8)
        st["seg"] = seg
        st["raw"] = bytes(uc.mem_read(seg * 16 + BASE, LEFT + 2 * SLICE))

    m.uc.hook_add(UC_HOOK_CODE, on_frame)

    def report():
        lines = [f"{st['n']} frames inside the level"]
        if st["raw"] is None:
            lines.append("NOTHING READ - the run never reached frame 20 inside "
                         "a level, so this says nothing about the masks")
        else:
            lines.append(f"store segment ds:0x{STORE_SEG:04x} = "
                         f"0x{st['seg']:04x}")
            for d, dn in ((0, "right"), (LEFT, "left")):
                for s in range(2):
                    off = d + s * SLICE
                    lines.append(f"  {dn}, slice {s}:")
                    for r in range(ROWS):
                        b = st["raw"][off + r * 3: off + r * 3 + 3]
                        v = (b[0] << 16) | (b[1] << 8) | b[2]
                        bits = "".join("#" if v >> (23 - i) & 1 else "."
                                       for i in range(24))
                        lines.append(f"    0x{v:06X},  /* {bits} */")
        # THE RAW BYTES TOO. The report prints shift 0, and the question
        # that matters is whether shifts 1..7 are the same shape translated or
        # whether they LOSE bits off the end of the three bytes - image
        # 0x060E6 picks the variant by x & 7, and the port keeps only shift 0
        # and shifts it itself at pixel granularity, which is only equivalent
        # if nothing falls off.
        if st["raw"] is not None:
            try:
                with open(OUT + ".bin", "wb") as f:
                    f.write(st["raw"])
            except OSError:
                pass
        text = "\n".join(lines)
        try:
            with open(OUT, "w") as f:
                f.write(text + "\n")
        except OSError:
            pass
        print(text, flush=True)

    atexit.register(report)
