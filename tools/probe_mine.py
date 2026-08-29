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

AND THE SOLIDITY UNDER THE PIXEL, which is the half that settles it. The
masks turned out to be faithful, so the disagreement is not what either side
CLEARS - it is what the original still shows afterwards. A screen carries
terrain, sprites, objects and cursor and cannot tell them apart, so this also
reads the guest's own solidity map: image 0x05DC6 probes (y - 0x10) * 0xD0 +
(x >> 3) in plane 3 and image 0x05DEA adds 0x4A40, the same read
tools/probe_digger.py makes for the digger's shaft. If the map says CLEAR
where the screen shows 2, the 2 is drawn by something and the port is simply
not drawing it; if it says SOLID, the original really did leave terrain and
the two sides clear differently after all.

    LEM_EXTRA_HOOK=$PWD/tools/probe_mine.py LEM_MINE_FRAME=200 \
    uv run python tools/record_level.py --frames 260 --out out/mine_solid \
        --script out/s8.txt --skills 20 --lemmings --seconds 400

    LEM_EXTRA_HOOK=$PWD/tools/probe_mine.py \
    uv run python tools/record_level.py --frames 30 --out out/mine --seconds 240
"""
import atexit
import json
import os

FRAME_END = 0x018F
STORE_SEG = 0x1FE8       # ds:0x1FE8 holds the segment, image 0x06085
BASE      = 0x780
LEFT      = 0x270
SLICE     = 312
ROWS      = 13
OUT = os.environ.get("LEM_MINE_OUT", "out/mine_mask.txt")
DGROUP = 0x9A90

# The frame to read the solidity map at, and the window to read. The default
# is a frame on which the original's screen shows 2 at (159, 81) and the
# port shows 0 - the whole of the disagreement.
SOLID_FRAMES = sorted(int(v) for v in
                      os.environ.get("LEM_MINE_FRAME", "200").split(",") if v)
SOLID_X0, SOLID_X1 = 700, 900
SOLID_Y0, SOLID_Y1 = 60, 130


def install(m):
    from unicorn import UC_HOOK_CODE

    base = m.load_seg * 16
    st = {"n": 0, "raw": None, "solid": {}, "attr": {}}

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

    def on_solid(uc, ad, size, _):
        # A SECOND hook rather than more work in the first: the mask read
        # happens once at frame 20 and this has to happen at a frame deep
        # enough into the tunnel for there to BE a tunnel.
        #
        # SEVERAL frames, not one. The first version read a single frame and
        # landed on one where the two sides AGREE - the disagreement at
        # (159, 81) is on 134 frames of 400 and absent from the rest, so one
        # capture has a two-in-three chance of saying nothing while looking
        # like an answer.
        if ad - base != FRAME_END:
            return
        if st["n"] not in SOLID_FRAMES or st["n"] in st["solid"]:
            return
        rows = []
        for yy in range(SOLID_Y0, SOLID_Y1):
            line = []
            for xx in range(SOLID_X0, SOLID_X1):
                off = (yy - 0x10) * 0xD0 + (xx >> 3) + 0x4A40
                if off < 0 or off >= 0x10000:
                    line.append(0)
                    continue
                line.append((m.planes[3][off] >> (7 - (xx & 7))) & 1)
            rows.append(line)
        st["solid"][st["n"]] = rows
        # AND THE MASTER'S FULL ATTRIBUTE, all four planes. Plane 3 alone is
        # the solidity bit and cannot tell attribute 0 from attribute 2 -
        # terrain carries plane 3 and a lemming's colours do not, so "plane 3
        # is clear" is true of cleared ground AND of anything drawn in 1..7.
        # Reading the other three says which, and that is what separates "the
        # master still holds something" from "the page does".
        att = []
        for yy in range(SOLID_Y0, SOLID_Y1):
            line = []
            for xx in range(SOLID_X0, SOLID_X1):
                off = (yy - 0x10) * 0xD0 + (xx >> 3) + 0x4A40
                if off < 0 or off >= 0x10000:
                    line.append(0)
                    continue
                v = 0
                for pl in range(4):
                    v |= ((m.planes[pl][off] >> (7 - (xx & 7))) & 1) << pl
                line.append(v)
            att.append(line)
        st["attr"][st["n"]] = att

    m.uc.hook_add(UC_HOOK_CODE, on_frame)
    m.uc.hook_add(UC_HOOK_CODE, on_solid)

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
        # AND ANSWER THE QUESTION, rather than leaving the bytes for a
        # reader. The report prints shift 0, and what matters is whether
        # shifts 1..7 are the same shape translated or whether they LOSE bits
        # off the end of the three bytes - image 0x060E6 picks the variant by
        # x & 7, and the port keeps only shift 0 and shifts it itself at pixel
        # granularity, which is only equivalent if nothing falls off.
        #
        # It is equivalent: all 4 entries x 7 shifts x 13 rows are exactly
        # shift 0 >> s. That is what licenses data.c keeping one shift of the
        # four, and it had been ASSERTED in that comment and never measured -
        # so this prints the count of variants that are not a pure shift, and
        # a non-zero there is the assumption breaking.
        if st["raw"] is not None:
            odd = 0
            checked = 0
            for d in (0, LEFT):
                for s0 in range(2):
                    off = d + s0 * SLICE
                    zero = [int.from_bytes(
                        st["raw"][off + r * 3: off + r * 3 + 3], "big")
                        for r in range(ROWS)]
                    for sh in range(1, 8):
                        for r in range(ROWS):
                            o = off + sh * 39 + r * 3
                            v = int.from_bytes(st["raw"][o:o + 3], "big")
                            checked += 1
                            if v != zero[r] >> sh:
                                odd += 1
            lines.append(f"  variants that are not shift0 >> s: "
                         f"{odd} of {checked}")
        if st["raw"] is not None:
            try:
                with open(OUT + ".bin", "wb") as f:
                    f.write(st["raw"])
            except OSError:
                pass
        # THE SOLIDITY, and it must say when it read nothing rather than
        # printing a window of zeros that reads as "all clear".
        if not st["solid"]:
            lines.append(f"  solidity: NOT READ - the run never reached any of "
                         f"{SOLID_FRAMES} inside a level, so this says nothing")
        else:
            missed = [f for f in SOLID_FRAMES if f not in st["solid"]]
            for f in sorted(st["solid"]):
                rows = st["solid"][f]
                solid = sum(sum(r) for r in rows)
                cells = len(rows) * len(rows[0])
                lines.append(f"  solidity at frame {f}, window "
                             f"x {SOLID_X0}..{SOLID_X1} y {SOLID_Y0}.."
                             f"{SOLID_Y1}: {solid} solid of {cells}")
            if missed:
                lines.append(f"  frames asked for and NOT reached: {missed}")
            try:
                with open(OUT + ".solid.json", "w") as f:
                    json.dump({"x0": SOLID_X0, "y0": SOLID_Y0,
                               "frames": {str(k): v
                                          for k, v in st["solid"].items()},
                               "attr": {str(k): v
                                        for k, v in st["attr"].items()}}, f)
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
