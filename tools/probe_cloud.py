"""The bomber's cloud, read out of the original's own pre-shifted store.

The explosion has no picture in the port. A byte search of every MAIN.DAT
section for a 32x40 three-plane frame found nothing, and that is not a
mystery: image 0x02D45 gives the cloud its segment from ds:0x3C, not the
ds:0x2E the walker and its relatives use, so it is a different resource - and
what the blitter at image 0x070BF reads is the PRE-SHIFTED store anyway, not
the file.

So read the store, the way tools/probe_cursor.py reads the cursor's and
tools/verify_results.py reads the crater's masks. Image 0x02D1B..0x02D33
says exactly what to read:

    [si+0x0a] = 8          the frame index
    [si+0x0c] = 0x180b     where the group starts in the store
    [si+0x10] = 0x280      640 bytes a frame, which for 8 shifts of 40 rows
                           is 4 bytes a row across four planes
    [si+0x12] = 0x1e0      480 in the FILE - three planes, so the fourth in
                           the store is the mask
    hot spot (-16, -25)

  LEM_HOOK=tools/probe_cloud.py uv run python tools/play.py --auto --start \
      --click-tries 4 -- --run-seconds 90

It has to ride into a level: the store is built when one is loaded and a
start-up run stops at the menu with the segment still zero.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

FRAME_END = 0x018F
# THE STORE'S SEGMENT IS ds:0x1FE6, image 0x07112 - not the ds:0x3C that
# image 0x02D45 puts in [si+0x14], which is something else the blit carries.
# And image 0x070C6 divides the frame's byte offset by SIXTEEN before image
# 0x070D6 adds [si+0x0c], so both are PARAGRAPH counts and image 0x07116 adds
# the result to the segment rather than to an offset. Reading ds:0x3C as a
# segment and [si+0x0c] as a byte offset gives dense noise that looks like a
# sprite decoded with the wrong geometry.
STORE_SEG = 0x1FE6        # image 0x07112
GROUP = 0x180B            # image 0x02D24, in PARAGRAPHS
STRIDE = 0x280            # image 0x02D29, bytes a frame
INK = 8                   # image 0x02D1F's [si+0x0a] - a byte offset, and
                          # image 0x070DF adds it to (index * stride) % 16
MASK = INK + 0x1E0        # image 0x070E4 adds [si+0x12]
# FIVE bytes a row, not four. [si+0x10] = 0x280 is the whole frame and
# [si+0x12] = 0x1E0 is where the MASK starts, so the ink is 480 bytes in
# three planes - 160 a plane - and the mask is the remaining 160. 160 as
# 5 x 32 makes the sprite 32 wide PRE-SHIFTED into 40 bits, which is what a
# store built for eight sub-byte positions holds. Reading it as 4 x 40 gives
# a picture that drifts sideways a byte every row and looks like a diagonal
# weave - which is what it did.
ROWS, ROW_BYTES = 32, 5


def decode(buf, base):
    """One stored frame: 40 rows of 32 pixels, three planes plus a mask."""
    plane = ROW_BYTES * ROWS
    ink, mask = [], []
    for r in range(ROWS):
        row, mrow = [], []
        for c in range(ROW_BYTES * 8):
            v = 0
            for p in range(3):
                byte = buf[base + INK + p * plane + r * ROW_BYTES + (c >> 3)]
                v |= ((byte >> (7 - (c & 7))) & 1) << p
            row.append(v)
            mb = buf[base + MASK + r * ROW_BYTES + (c >> 3)]
            mrow.append((mb >> (7 - (c & 7))) & 1)
        ink.append(row)
        mask.append(mrow)
    return ink, mask


def install(m):
    from unicorn import UC_HOOK_CODE
    from unicorn.x86_const import UC_X86_REG_DS

    base = m.load_seg * 16
    out = os.environ.get("LEM_CLOUD_OUT", "out/cloud.json")
    st = {"done": False}

    def at(uc, ad, size, _):
        if ad - base != FRAME_END or st["done"]:
            return
        ds = uc.reg_read(UC_X86_REG_DS) * 16
        seg = int.from_bytes(uc.mem_read(ds + STORE_SEG, 2), "little")
        if not seg:
            return                     # not in a level yet
        st["done"] = True
        # AND THE RAW BYTES BESIDE THE DECODE. Every derivation of where in
        # this store the cloud sits has been wrong so far, so dump a wide
        # window and let a search against the original's own screen say where
        # it is - the arithmetic can be checked against the answer afterwards.
        raw = os.environ.get("LEM_CLOUD_RAW", "out/cloud_store.bin")
        span = 512 * 1024
        with open(raw, "wb") as rf:
            rf.write(bytes(uc.mem_read(seg * 16, span)))
        print(f"  [probe_cloud] {span} bytes from segment {seg:#06x} -> {raw}")
        frames = {}
        # Frame 8 is what image 0x02D1F asks for, but take a spread: the
        # index is fixed while the animation counter runs, so which frame is
        # actually on screen is a question the pictures answer and a reading
        # of one instruction does not.
        for f in range(0, 53):
            # Image 0x070B6: the index is frame * 8 + the sub-byte shift, and
            # shift 0 is the one this port wants - it works in whole pixels.
            para = (f * 8) * STRIDE // 16 + GROUP
            lin = seg * 16 + para * 16
            buf = bytes(uc.mem_read(lin, MASK + ROW_BYTES * ROWS))
            ink, mask = decode(buf, 0)
            frames[f] = {"para": para, "ink": ink, "mask": mask}
        with open(out, "w") as fh:
            json.dump({"seg": seg, "group": GROUP, "stride": STRIDE,
                       "frames": frames}, fh)
        print(f"  [probe_cloud] store at segment {seg:#06x} -> {out}")
        for f in (0, 4, 8):
            if f not in frames:
                continue
            print(f"\n  frame {f} (paragraph {frames[f]['para']:#07x}):")
            for row in frames[f]["ink"][:40]:
                print("    " + "".join(f"{v:x}" if v else "." for v in row))
        raise SystemExit(0)

    m.uc.hook_add(UC_HOOK_CODE, at)
    print("  [probe_cloud] will dump the cloud's store at the first frame")
