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
STORE_SEG = 0x003C        # ds: image 0x02D45's word
GROUP = 0x180B            # image 0x02D24
STRIDE = 0x280            # image 0x02D29, a frame of the store
ROWS, ROW_BYTES, PLANES = 40, 4, 4


def decode(buf, base):
    """One stored frame: 40 rows of 32 pixels, three planes plus a mask."""
    plane = ROW_BYTES * ROWS
    ink, mask = [], []
    for r in range(ROWS):
        row, mrow = [], []
        for c in range(ROW_BYTES * 8):
            v = 0
            for p in range(3):
                byte = buf[base + p * plane + r * ROW_BYTES + (c >> 3)]
                v |= ((byte >> (7 - (c & 7))) & 1) << p
            row.append(v)
            mb = buf[base + 3 * plane + r * ROW_BYTES + (c >> 3)]
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
        # Enough for the group and a good many frames past it.
        want = GROUP + STRIDE * 8 * 16
        buf = bytes(uc.mem_read(seg * 16, want))
        frames = {}
        # Frame 8 is what image 0x02D1F asks for, but take a spread: the
        # index is fixed while the animation counter runs, so which frame is
        # actually on screen is a question the pictures answer and a reading
        # of one instruction does not.
        for f in range(0, 12):
            off = GROUP + (f * 8) * STRIDE // 16
            if off + STRIDE > len(buf):
                break
            ink, mask = decode(buf, off)
            frames[f] = {"off": off, "ink": ink, "mask": mask}
        with open(out, "w") as fh:
            json.dump({"seg": seg, "group": GROUP, "stride": STRIDE,
                       "frames": frames}, fh)
        print(f"  [probe_cloud] store at segment {seg:#06x} -> {out}")
        for f in (0, 4, 8):
            if f not in frames:
                continue
            print(f"\n  frame {f} (store offset {frames[f]['off']:#07x}):")
            for row in frames[f]["ink"][:40]:
                print("    " + "".join(f"{v:x}" if v else "." for v in row))
        raise SystemExit(0)

    m.uc.hook_add(UC_HOOK_CODE, at)
    print("  [probe_cloud] will dump the cloud's store at the first frame")
