"""The TRIGGER MAP, the port's against the original's own.

This is the map that answers "what is under this lemming" - the only thing
that does - and two passes build it before a level starts:

    image 0x03839   the STEEL areas, stamping the cell value 9 over each
                    entry, (width + 1) cells by (height + 1)
    image 0x037C2   the OBJECTS, stamping (index << 4) | effect

It lives at ds:0x12 with image 0x03874's `mul 0x1a0` for a row stride, and
image 0x023A6 reads it as `[0x12 + (y - 0x10 >> 2) * 0x1A0 + (x >> 2)] & 0x0F`.

WHY IT EXISTS. The port parsed the LVL's steel entries and never stamped
them - trig_stamp was called from the object pass alone - so a steel area
read as 0 and every test against it was dead code. A bomber's crater ate
through a steel floor, and a player found it, because no level compared
frame by frame here is one anybody bombs. A picture cannot show this map;
only reading it can.

    uv run python tools/verify_triggers.py --level 77

`--level` is ds:0x1F6B, the index into the game's running order, and the
file and section are printed beside it so a wrong one is visible rather
than silent.
"""
import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

# THE MAP IS IN THE STACK SEGMENT, not DGROUP. Image 0x023A6 reads it as
# `mov al, byte ptr ss:[bx]` and image 0x03886 writes it as
# `mov byte ptr ss:[bx], 9` - both SS-relative. Assuming DGROUP, because a
# small-model DOS program usually has SS == DS, produced a dump of uniform
# noise: all sixteen effect values spread evenly. That scored the port's map
# as 47% wrong, and it was a fact about the address rather than about the
# port. Reading a segment REGISTER beats assuming which segment a program
# keeps a thing in.
TRIG_AT = 0x12          # image 0x023A3 and image 0x0387C
STRIDE = 0x1A0          # image 0x03874
ROWS = 64
FRAME_END = 0x018F


def install(m):
    """Runs in the guest via LEM_EXTRA_HOOK."""
    import atexit
    from unicorn import UC_HOOK_CODE

    base = m.load_seg * 16
    out = os.environ["LEM_TRIG_OUT"]
    st = {"done": False, "frames": 0}

    def on_frame(uc, address, size, user):
        if address - base != FRAME_END:
            return
        st["frames"] += 1
        # The SECOND mode 0x0D is the level; the first is the loading screen,
        # and the map is not built until the level is.
        modes = getattr(m, "video_modes", [])
        if sum(1 for v in modes if v == 0x0D) < 2:
            return
        if st["done"]:
            return
        st["done"] = True
        from unicorn.x86_const import UC_X86_REG_SS
        ss = uc.reg_read(UC_X86_REG_SS)
        buf = uc.mem_read(ss * 16 + TRIG_AT, ROWS * STRIDE)
        print("  [trig] SS = %04X" % ss, flush=True)
        with open(out, "wb") as f:
            f.write(bytes(buf))
        print("  [trig] dumped the original's map at frame %d" % st["frames"],
              flush=True)

    m.uc.hook_add(UC_HOOK_CODE, on_frame)

    def report():
        if not st["done"]:
            print("  [trig] NEVER DUMPED - the run did not reach a level, so "
                  "there is nothing to compare. That is a failed run, not a "
                  "pass.", flush=True)

    atexit.register(report)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--level", type=int, required=True,
                    help="ds:0x1F6B, the index into the running order")
    ap.add_argument("--seconds", type=int, default=420)
    a = ap.parse_args()

    # WHICH FILE AND SECTION that index is, printed rather than assumed - the
    # order table is a genuine permutation and getting it wrong is silent.
    import re
    src = open(os.path.join(ROOT, "reconstruct", "src", "game.c")).read()
    mm = re.search(r'LEVEL_ORDER\s*\[[^\]]*\]\s*=\s*\{(.*?)\};', src, re.S)
    body = re.sub(r'/\*.*?\*/', '', mm.group(1), flags=re.S)
    body = re.sub(r'//[^\n]*', '', body)
    order = [int(x) for x in re.findall(r'\d+', body)]
    k = order[a.level] - 1
    lfile = "level%03d.dat" % (k >> 4)
    lsec = ((k >> 1) & 7) + 1
    print("  index %d is %s section %d" % (a.level, lfile, lsec))

    # PER-LEVEL FILENAMES, because two runs at once shared these and the
    # second silently overwrote the first. Three measurements were then taken
    # of level004/3's port map against level002/6's ORIGINAL map, and they
    # looked like a regression the port had just acquired - 16,040 of 16,040
    # agreeing, then 14,666, from a change that turned out to be innocent.
    #
    # A fixed output path is a shared mutable global with a slow writer.
    ref = os.path.join(ROOT, "out", "orig_trig_%d.bin" % a.level)
    port = os.path.join(ROOT, "out", "port_trig_%d.bin" % a.level)
    for f in (ref, port):
        if os.path.exists(f):
            os.unlink(f)

    env = dict(os.environ,
               LEM_EXTRA_HOOK=os.path.abspath(__file__),
               LEM_TRIG_OUT=ref)
    subprocess.run([sys.executable, os.path.join(HERE, "record_level.py"),
                    "--level", str(a.level), "--frames", "4",
                    "--seconds", str(a.seconds),
                    "--out", os.path.join(ROOT, "out", "trig_shots")],
                   cwd=ROOT, env=env, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    if not os.path.exists(ref):
        print("  the original's map was never dumped - a failed run, not a "
              "comparison.")
        return 2

    r = subprocess.run([os.path.join(ROOT, "reconstruct", "lemdev"),
                        "--data", "lemming1.pc", "--file", lfile,
                        "--level", str(lsec), "--triggers", port,
                        "--raw", os.devnull],
                       cwd=ROOT, capture_output=True, text=True)
    if not os.path.exists(port):
        print("  the port wrote no map:\n" + r.stderr)
        return 2

    A = open(ref, "rb").read()
    B = open(port, "rb").read()
    # ONLY THE PART THE GAME CAN REACH. The dump is 64 rows of 0x1A0 because
    # that is the port's array, but the original's map lives in its STACK
    # segment and whatever is past the live extent is other variables - which
    # compare as differences and are not. Image 0x02EC5 refuses any lemming
    # at y >= 0xB0 and the row is (y - 0x10) >> 2, so row 39 is the last one
    # anything can ask about; the canvas is 1,600 wide and the column is
    # x >> 2, so column 400 is the last.
    #
    # Unbounded, this reported 3,180 differing cells on a map that agrees
    # completely. A comparison has to be bounded by what the program can
    # actually read, not by how much of it was convenient to dump.
    LAST_ROW, LAST_COL = 39, 400
    idx = [r * STRIDE + c
           for r in range(LAST_ROW + 1) for c in range(LAST_COL + 1)]
    n = len(idx)
    A = bytes(A[i] for i in idx)
    B = bytes(B[i] for i in idx)
    # THE EFFECT NIBBLE is what every caller reads - image 0x023A6 and image
    # 0x034DC both mask with 0x0F - and the high nibble is the object index,
    # which only matters for an object cell. Both are reported, separately,
    # because a port that agreed on the nibble and not the index would be
    # right about steel and wrong about which trap fired.
    same_lo = sum(1 for i in range(n) if (A[i] & 0x0F) == (B[i] & 0x0F))
    same_all = sum(1 for i in range(n) if A[i] == B[i])
    steel_ref = sum(1 for i in range(n) if (A[i] & 0x0F) == 9)
    steel_port = sum(1 for i in range(n) if (B[i] & 0x0F) == 9)
    nz_ref = sum(1 for i in range(n) if A[i])
    print("  the trigger map, %d cells (rows 0..%d, cols 0..%d - "
          "what image 0x02EC5 and the canvas allow):"
          % (n, LAST_ROW, LAST_COL))
    print("    effect nibble   %d agree, %d differ" % (same_lo, n - same_lo))
    print("    whole byte      %d agree, %d differ" % (same_all, n - same_all))
    print("    STEEL cells     original %d, port %d" % (steel_ref, steel_port))
    if not nz_ref:
        print("    THE ORIGINAL'S MAP IS EMPTY - nothing was compared. That is "
              "a failed capture, not a pass.")
        return 2
    if same_lo != n:
        bad = [i for i in range(n) if (A[i] & 0x0F) != (B[i] & 0x0F)][:8]
        for i in bad:
            print("      row %2d col %3d: original %02X port %02X"
                  % (i // (LAST_COL + 1), i % (LAST_COL + 1), A[i], B[i]))
    return 0 if same_lo == n else 1


if __name__ == "__main__":
    sys.exit(main())
