"""What the briefing's preview actually reads, dumped without halting the guest.

The preview is packed 4:1 out of the level bitmap by image 0x07EB2, whose
`si = 0x4F22` is 0x4A40 + 6 * 208 + 2. The question is whether the OBJECTS are
in that bitmap when it runs: the original's preview shows them - taking them
out of the port moves the briefing from 0 pixels differing to 53 - and image
0x03789 says nothing composes them into it.

Breaking there with the control socket does not work: image 0x07EB2 is reached
BEFORE the title screen, so the guest halts with the menus still up and every
F1 the driver sends is dropped. This rides along instead and dumps on every
hit, numbered, so the briefing's own can be picked out afterwards.

    LEM_HOOK=$PWD/tools/probe_pvsrc.py \
    uv run python tools/play.py --auto --start -- --run-seconds 200

Writes out/pvsrc/hit<N>.bin - four planes of 208 x 163 bytes, the rows the
packer walks - plus a one-line index saying how many hits there were.
"""
import atexit
import os

ENTRY = 0x07EB2
BASE = 0x4A40            # image 0x05DEA: the level bitmap in EGA memory
STRIDE = 208
ROWS = 163               # 6 + 39 * 4, everything the packer reads
OUTDIR = os.environ.get("LEM_PVSRC_DIR", "out/pvsrc")


def install(m):
    from unicorn import UC_HOOK_CODE

    base = m.load_seg * 16
    st = {"n": 0}
    try:
        os.makedirs(OUTDIR, exist_ok=True)
    except OSError:
        pass

    def on_entry(uc, ad, size, _):
        if ad - base != ENTRY:
            return
        st["n"] += 1
        try:
            with open(os.path.join(OUTDIR, "hit%d.bin" % st["n"]), "wb") as f:
                for p in range(4):
                    f.write(bytes(m.planes[p][BASE:BASE + STRIDE * ROWS]))
        except (OSError, AttributeError, IndexError):
            pass

    m.uc.hook_add(UC_HOOK_CODE, on_entry)

    def report():
        msg = ("%d hit(s) of image 0x%05X" % (st["n"], ENTRY)) if st["n"] else (
            "NO HITS of image 0x%05X - the run never reached the packer, so "
            "this says nothing about what it reads" % ENTRY)
        try:
            with open(os.path.join(OUTDIR, "index.txt"), "w") as f:
                f.write(msg + "\n")
        except OSError:
            pass
        print(msg, flush=True)

    atexit.register(report)
