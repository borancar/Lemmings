"""Who writes to video memory, and from where.

The menu draws through one shared blitter at image 0x8822, and
tools/probe_blits.py maps that completely. The LEVEL does not: it runs in mode
0x0D, 320x200, and a driven probe of the blitter logs no call into the panel's
rows at all. So mode 0x0D has a drawing path of its own, and this finds it the
only way that cannot miss - by watching the writes themselves.

Every write into the EGA window is attributed to the instruction that made it,
and the instructions are bucketed by the routine they sit in (the nearest
preceding `ret` + 1, which is crude but separates them well enough to rank).

  uv run python tools/probe_writes.py --play --seconds 120
  uv run python tools/probe_writes.py --play --rows 176 199

`--rows` filters by the DESTINATION row, worked out from the write's offset
and the mode's stride, so "what draws the panel" is one question rather than a
haystack.
"""
import argparse
import collections
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

VGA_LO, VGA_HI = 0xA0000, 0xB0000


def install(m):
    """Runs in the guest via LEM_HOOK."""
    import atexit
    import json
    from unicorn import UC_HOOK_MEM_WRITE
    from unicorn.x86_const import UC_X86_REG_CS, UC_X86_REG_IP

    base = m.load_seg * 16
    out_path = os.environ["LEM_WRITES_OUT"]
    stride = int(os.environ.get("LEM_WRITES_STRIDE", "40"))
    # THE LEVEL'S CANVAS DOES NOT START AT THE SEGMENT. Image 0x05DEA adds
    # 0x4A40 and its rows are 0xD0 bytes, not the screen's 40 - so watching a
    # canvas row needs the base as well as the stride, or the division smears
    # every row across the wrong bucket (0x4A40 / 0xD0 is not an integer).
    # Defaults to 0, which is what every existing caller wants.
    wbase = int(os.environ.get("LEM_WRITES_BASE", "0"), 0)
    # AND ONLY ONCE THE LEVEL IS RUNNING. A run recorded by
    # tools/record_level.py passes through the title, the menu and the
    # briefing first, and those are mode 0x10 at 80 bytes a row - so a filter
    # expressed as "canvas row 81" is a LINEAR OFFSET that means something
    # else entirely on every screen before the level. Counting the game's own
    # end-of-frame at image 0x018F, which only runs inside a level, and
    # ignoring everything before the Nth, is what makes the answer about the
    # level. Without it a briefing blit is reported as a level one, and it
    # looks exactly like a finding.
    after = int(os.environ.get("LEM_WRITES_AFTER_FRAME", "0"))
    seen_frames = [0]
    y0, y1 = (int(v) for v in
              os.environ.get("LEM_WRITES_ROWS", "0 349").split())
    cols = os.environ.get("LEM_WRITES_COLS")
    cx0, cx1 = ((int(v) for v in cols.split()) if cols else (None, None))
    hits = collections.Counter()

    FRAME_END = 0x018F

    def on_frame(uc, ad, size, _):
        if ad - base == FRAME_END:
            seen_frames[0] += 1

    def on_write(uc, access, address, size, value, _):
        if seen_frames[0] < after:
            return
        if not (VGA_LO <= address < VGA_HI):
            return
        off = address - VGA_LO - wbase
        if off < 0:
            return
        row = (off // stride) if stride else 0
        if not (y0 <= row <= y1):
            return
        if cx0 is not None:
            # The byte holds eight pixels, so a column filter is a range of
            # bytes: anything overlapping [cx0, cx1].
            b = off % stride
            if not (cx0 // 8 <= b <= cx1 // 8):
                return
        pc = uc.reg_read(UC_X86_REG_CS) * 16 + uc.reg_read(UC_X86_REG_IP)
        hits[pc - base] += size

    from unicorn import UC_HOOK_CODE
    m.uc.hook_add(UC_HOOK_CODE, on_frame)
    m.uc.hook_add(UC_HOOK_MEM_WRITE, on_write)
    atexit.register(lambda: (
        open(out_path, "w").write(json.dumps(sorted(hits.items()))),
        print(f"  [hook] {len(hits)} distinct writing instructions, "
              f"{seen_frames[0]} level frames seen"
              f"{'' if seen_frames[0] >= after else ' - FEWER THAN ASKED FOR, '
                 'so nothing was recorded and that is a failure'}")))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rows", nargs=2, type=int, default=[0, 349],
                    metavar=("Y0", "Y1"))
    ap.add_argument("--stride", type=int, default=40,
                    help="bytes per row in the mode being watched: 40 for "
                         "320-wide four-plane, 80 for 640-wide")
    ap.add_argument("--seconds", type=int, default=120)
    ap.add_argument("--play", action="store_true",
                    help="click past the briefing into the level")
    ap.add_argument("--top", type=int, default=25)
    a = ap.parse_args()

    cap = os.path.join(ROOT, "out", "writes.json")
    if os.path.exists(cap):
        os.unlink(cap)
    env = dict(os.environ, LEM_HOOK=os.path.abspath(__file__),
               LEM_WRITES_OUT=cap, LEM_WRITES_STRIDE=str(a.stride),
               LEM_WRITES_ROWS=f"{a.rows[0]} {a.rows[1]}")
    subprocess.run([sys.executable, os.path.join(HERE, "play.py"),
                    "--auto", "--start", "--start-delay", "5",
                    "--start-tries", "3",
                    "--click-tries", "4" if a.play else "0",
                    "--", "--run-seconds", str(a.seconds)],
                   cwd=ROOT, env=env, stdout=subprocess.DEVNULL)
    if not os.path.exists(cap):
        print("\n  NOTHING WAS CAPTURED. That is a failure, not a pass.")
        return 2
    import json
    with open(cap) as f:
        hits = json.load(f)
    if not hits:
        print(f"\n  no writes into rows {a.rows[0]}..{a.rows[1]} at stride "
              f"{a.stride}. If that is a surprise, check the stride: a "
              f"320-wide four-plane mode is 40 bytes a row, not 80.")
        return 1
    hits.sort(key=lambda kv: -kv[1])
    print(f"\n  {len(hits)} instructions wrote into rows "
          f"{a.rows[0]}..{a.rows[1]}, by bytes written:\n")
    for pc, n in hits[:a.top]:
        print(f"    image {pc:#07x}   {n:9d} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
