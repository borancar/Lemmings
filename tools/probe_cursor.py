"""The cursor's two shapes, read out of the original's own sprite store.

game.c has had both of them MEASURED off captured frames - "one capture, so
the sequence is what that frame held rather than something read out of the
store". That is the weakest kind of evidence this project accepts, and the
store makes it unnecessary, because image 0x05827 says exactly where they are:

    bx = ds:0x10 + ds:0x1FE0, & 7          the sub-byte shift
    si = (bx << 4) + (bx << 7)             144 bytes a shift
    if ds:0x1A == 0xFF: si += 0x480        the BOX, eight shifts further on
    bx = si + 0x60                         the mask
    ds = ds:0x1FEA                         the store's segment
    call image 0x7218                      the blitter

144 bytes a shift splits as 0x60 of ink and 0x30 of mask, and 0x30 is 16 rows
of three bytes - a sixteen wide sprite pre-shifted into three - so the ink is
those same 16x3 in TWO planes and the whole thing is 16x16 in four colours.

  LEM_HOOK=tools/probe_cursor.py uv run python tools/play.py --auto --start \
      --click-tries 4 -- --run-seconds 90

It has to ride into a level: the store is built when one is loaded, and a
start-up run stops at the menu with the segment still zero.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

FRAME_END = 0x018F
STORE_SEG = 0x1FEA        # ds: the segment image 0x05862 loads
SHIFT = 144               # bytes per shift, image 0x0583B..0x0584B
BOX = 0x480               # image 0x05854
INK, MASK = 0x00, 0x60    # image 0x0585A
W_BYTES, ROWS, PLANES = 3, 16, 2


def decode(buf, base):
    """One shape, as 16 rows of 24 two-bit pixels plus its mask."""
    ink, mask = [], []
    for r in range(ROWS):
        row, mrow = [], []
        for c in range(W_BYTES * 8):
            v = 0
            for p in range(PLANES):
                byte = buf[base + INK + p * ROWS * W_BYTES
                           + r * W_BYTES + (c >> 3)]
                v |= ((byte >> (7 - (c & 7))) & 1) << p
            row.append(v)
            mb = buf[base + MASK + r * W_BYTES + (c >> 3)]
            mrow.append((mb >> (7 - (c & 7))) & 1)
        ink.append(row)
        mask.append(mrow)
    return ink, mask


def install(m):
    import atexit
    from unicorn import UC_HOOK_CODE
    from unicorn.x86_const import UC_X86_REG_DS

    base = m.load_seg * 16
    out = os.environ.get("LEM_CURSOR_OUT", "out/cursor.json")
    st = {"done": False}

    def at(uc, ad, size, _):
        if ad - base != FRAME_END or st["done"]:
            return
        ds = uc.reg_read(UC_X86_REG_DS) * 16
        seg = int.from_bytes(uc.mem_read(ds + STORE_SEG, 2), "little")
        if not seg:
            return                     # not in a level yet
        st["done"] = True
        buf = bytes(uc.mem_read(seg * 16, SHIFT * 16))
        shapes = {}
        for name, off in (("cross", 0), ("box", BOX)):
            ink, mask = decode(buf, off)
            shapes[name] = {"ink": ink, "mask": mask}
        # Shift 1 as well, as a control: the eight are the same picture moved
        # along, so if shift 0 decoded wrongly they will not look related.
        ink1, _ = decode(buf, SHIFT)
        shapes["cross_shift1"] = {"ink": ink1}
        with open(out, "w") as f:
            json.dump({"seg": seg, "shapes": shapes}, f)
        print(f"  [probe_cursor] store at segment {seg:#06x} -> {out}")
        for name in ("cross", "box"):
            print(f"\n  {name}:")
            for row in shapes[name]["ink"]:
                print("    " + "".join(f"{v:x}" if v else "." for v in row))
        raise SystemExit(0)

    m.uc.hook_add(UC_HOOK_CODE, at)
    atexit.register(lambda: None)
    print("  [probe_cursor] will dump the cursor store at the first frame")
