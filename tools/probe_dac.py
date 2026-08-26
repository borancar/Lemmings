"""Does the game reprogram the palette part-way down the screen?

The PC version runs EGA mode 0Dh in sixteen colours, and the High Performance
build is documented as changing the palette in the black scanlines between the
play area and the status font - which is why the font can be shades of green
while the terrain is browns. A DAC capture holds ONE palette, so neither
tools/record_level.py nor anything scored against it can see that happen: the
port renders the whole 320x200 from a single palette, and the status line
comes out with the terrain's oranges in it.

It SAMPLES the emulator's palette state rather than hooking the OUT
instruction. Wrapping `m._on_out` does nothing - Unicorn was handed the bound
method when the machine was built, so replacing the attribute afterwards
leaves the original registered and the probe silently records no writes at
all. That is what the first version of this file did, and "no port writes"
looked like an answer.

    LEM_HOOK=tools/probe_dac.py LEM_DAC_OUT=out/dac.txt \
        uv run python tools/play.py --auto --machine 2 --start --click-tries 4 \
        -- --run-seconds 90

--machine picks the class: 2 is "High Performance PCs", the build that is
supposed to do this at all, and 1 is "PC compatibles", which is not.
"""
import os

FRAME_END = 0x018F          # image offset: the game's own end of frame

# The level has a frame boundary to bracket samples with; the briefing does
# not - it is a still screen that the game paints, fades in, and leaves up. So
# this also logs every DISTINCT palette it ever sees, with the instruction
# count, which works on any screen. On the briefing that is the whole answer:
# a fade is a long run of values that never repeat, and a mid-screen swap is
# two that alternate forever.
EVERY = 512                 # instructions between samples
MAX_FRAMES = 196


def install(m):
    from unicorn import UC_HOOK_CODE

    out = open(os.environ.get("LEM_DAC_OUT", "out/dac.txt"), "w")
    base = m.load_seg * 16
    st = {"frame": 0, "n": 0, "last": None, "changes": 0,
          "in_frame": [], "seen": set(), "trace": [], "everseen": {}}

    def snapshot():
        return (tuple(m.attr_pal), tuple(m.palette[i] for i in range(24)))

    def at(uc, ad, size, _):
        st["n"] += 1
        if st["n"] % EVERY == 0:
            cur = snapshot()
            if st["last"] is not None and cur != st["last"]:
                st["changes"] += 1
                st["in_frame"].append(st["n"])
            st["last"] = cur
            # A mid-screen swap shows up as the SAME two palettes alternating
            # within one frame, and the frame ending on the one it started
            # with. A fade shows up as a long run of distinct values that
            # never repeat. Counting distinct snapshots tells them apart.
            st["seen"].add(cur)
            if cur not in st["everseen"]:
                st["everseen"][cur] = st["n"]
                a, pp = cur
                out.write(f"  new palette at insn {st['n']}: "
                          + " ".join("%02x%02x%02x" % pp[i] for i in range(8))
                          + " | "
                          + " ".join("%02x%02x%02x" % pp[i] for i in range(16, 24))
                          + "\n")
                out.flush()
            if st["frame"] >= 190:
                st["trace"].append((st["n"], cur))

        if ad - base != FRAME_END:
            return
        out.write(f"frame {st['frame']}: {len(st['in_frame'])} change(s), "
                  f"{len(st['seen'])} distinct palette(s) in the frame\n")
        if st["frame"] >= 190 and st["trace"]:
            out.write("    within the frame, in order:\n")
            prev = None
            for n, (a, p) in st["trace"]:
                if p == prev:
                    continue
                prev = p
                out.write(f"      +{n - st['trace'][0][0]:6d} insns  "
                          + " ".join("%02x%02x%02x" % p[i] for i in range(8))
                          + " | "
                          + " ".join("%02x%02x%02x" % p[i] for i in range(16, 24))
                          + "\n")
        st["trace"] = []
        if st["in_frame"]:
            a, p = snapshot()
            out.write("    attr_pal: " + " ".join(f"{v:02x}" for v in a) + "\n")
            out.write("    DAC 0..7 : "
                      + " ".join("%02x%02x%02x" % p[i] for i in range(8)) + "\n")
            out.write("    DAC16..23: "
                      + " ".join("%02x%02x%02x" % p[i] for i in range(16, 24))
                      + "\n")
        out.flush()
        st["in_frame"] = []
        st["seen"] = set()
        st["frame"] += 1
        if st["frame"] >= MAX_FRAMES:
            out.close()
            raise SystemExit(0)

    m.uc.hook_add(UC_HOOK_CODE, at)
    print("  [probe_dac] sampling the palette every "
          f"{EVERY} instructions")
