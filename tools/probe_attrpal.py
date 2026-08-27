"""The EGA attribute palette registers, per level.

tools/compare_screen.py maps a port attribute to a DAC index with a fixed
table - "attr_pal reads 00..07 then 10..17" - measured once, on one level. If
the game programs those registers per graphics set, that table is right for
the levels it was measured on and wrong elsewhere.

level004.dat section 3 is the case that raised it: the port draws the one-way
arrows with attribute 1 and the original's screen holds DAC 5, and DAC 1 is
blue where DAC 5 is red. Everything else about that sprite checks out - the
data decodes to 1, the ink and mask offsets match image 0x07BEF and image
0x07BF2, the terrain around it agrees to the pixel - so the attribute-to-DAC
step is what is left.

    LEM_EXTRA_HOOK=$PWD/tools/probe_attrpal.py \
    uv run python tools/record_level.py --frames 30 --out out/ap --level 77 \
        --seconds 300

Prints the sixteen registers and how often each value was seen, so a run that
never reached the level says so instead of reporting the menu's.
"""
import atexit
import collections
import os

FRAME_END = 0x018F
OUT = os.environ.get("LEM_ATTRPAL_OUT", "out/attrpal.txt")


def install(m):
    from unicorn import UC_HOOK_CODE

    base = m.load_seg * 16
    seen = collections.Counter()
    st = {"n": 0}

    def on_frame(uc, ad, size, _):
        if ad - base != FRAME_END:
            return
        st["n"] += 1
        pal = getattr(m, "attr_pal", None)
        if pal is None:
            return
        seen[bytes(pal[:16])] += 1

    m.uc.hook_add(UC_HOOK_CODE, on_frame)

    def report():
        lines = ["%d frames sampled inside the level" % st["n"]]
        if not seen:
            lines.append("NO attr_pal WAS READ - either the run never reached "
                         "a level or the emulator does not expose it. Either "
                         "way this says nothing.")
        for raw, c in seen.most_common(4):
            lines.append("  x%-5d %s" % (c, " ".join("%02X" % b for b in raw)))
            lines.append("         attr 1 -> DAC %d, attr 5 -> DAC %d"
                         % (raw[1], raw[5]))
        text = "\n".join(lines)
        try:
            with open(OUT, "w") as f:
                f.write(text + "\n")
        except OSError:
            pass
        print(text, flush=True)

    atexit.register(report)
