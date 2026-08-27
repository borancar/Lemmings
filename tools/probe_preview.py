"""Who builds the briefing's preview, watched where it is actually written.

The briefing's whole upper area - the little map of the level inside it - goes
up in ONE blit: image 0x04EEF sets x=0, y=0x14, dx=0x278 (632), cl=0x5E (94
rows), si=0x2080, and image 0x04F0A is `lcall 0x1010:0x8822` reading the
segment in ds:0x1FEE. So nothing draws the preview to the SCREEN, and
tools/probe_writes.py - which watches 0xA0000..0xB0000 - can only ever see the
page being cleared and that one blit landing.

The routine that is owed writes to ds:0x1FEE:0x2080 instead. This watches
there, and buckets every write by the instruction that made it.

    LEM_EXTRA_HOOK=$PWD/tools/probe_preview.py \
    uv run python tools/play.py --auto -- --run-seconds 100

or as LEM_HOOK on its own. Written to a file AND printed at exit, because a
SystemExit from a Unicorn callback does not propagate and buffered stdout dies
with the run - both of which have made probes here report silence as a result.
"""
import atexit
import collections
import os

DGROUP = 0x9A90
SEG_AT = 0x1FEE          # ds:0x1FEE holds the segment, image 0x04EEF
LO, HI = 0x2080, 0x2080 + 94 * 80 * 4   # the blit's own window, four planes
OUT = os.environ.get("LEM_PREVIEW_OUT", "out/preview_writes.txt")


def install(m):
    from unicorn import UC_HOOK_MEM_WRITE
    from unicorn.x86_const import UC_X86_REG_CS, UC_X86_REG_IP

    base = m.load_seg * 16
    st = {"seg": 0, "n": 0}
    by = collections.Counter()

    def on_write(uc, access, address, size, value, _):
        if not st["seg"]:
            w = bytes(uc.mem_read(base + DGROUP + SEG_AT, 2))
            st["seg"] = w[0] | (w[1] << 8)
            if not st["seg"]:
                return
        lo = st["seg"] * 16 + LO
        hi = st["seg"] * 16 + HI
        if not (lo <= address < hi):
            return
        st["n"] += size
        cs = uc.reg_read(UC_X86_REG_CS)
        ip = uc.reg_read(UC_X86_REG_IP)
        by[(cs * 16 + ip) - base] += size

    m.uc.hook_add(UC_HOOK_MEM_WRITE, on_write)

    def report():
        lines = [f"segment ds:0x{SEG_AT:04x} = 0x{st['seg']:04x}",
                 f"{st['n']} bytes written into +0x{LO:04X}..+0x{HI:04X} "
                 f"by {len(by)} instruction(s)"]
        if not by:
            lines.append("NOTHING WROTE THERE. Check the segment was resolved "
                         "at all - a zero above means the run never reached a "
                         "screen that sets it, and says nothing about who "
                         "builds the preview.")
        for ad, n in by.most_common(25):
            lines.append(f"  image 0x{ad:05X}  {n} bytes")
        text = "\n".join(lines)
        try:
            with open(OUT, "w") as f:
                f.write(text + "\n")
        except OSError:
            pass
        print(text, flush=True)

    atexit.register(report)
