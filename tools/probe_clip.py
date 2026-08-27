"""What the blitter clips against, read out of a running level.

Image 0x09997 refuses a blit whose left edge is at or past `ds:0x66F2` and
image 0x099A1 trims one that crosses it a whole BYTE at a time. The value of
that word decides where the play area's right edge is, and ASSUMING it was
the visible 320 cost two rounds: applying the trim to every lemming sprite
took level002/4 from 1,071 frames exact to 875, and refusing the last column
outright took it to 1,015. Both are refuted, so the thing to do is read it.

This samples the word every frame from inside a level, at the game's own
end-of-frame (image 0x018F), and reports the distinct values it took. It also
samples the two words either side - 0x66F0 and 0x66F4 - because a limit that
turns out to be a pair of bounds is a different rule from a single ceiling,
and one word cannot tell you which.

    LEM_EXTRA_HOOK=$PWD/tools/probe_clip.py \
    uv run python tools/record_level.py --frames 60 --out out/clip --seconds 240

Written to a file AND printed at exit, because a SystemExit raised from a
Unicorn callback does not propagate and buffered stdout dies with the run -
both of which made earlier probes here report silence as a result.
"""
import atexit
import collections
import os

FRAME_END = 0x018F
DGROUP = 0x9A90          # fixed image offset, never the live DS
WORDS = (0x66F0, 0x66F2, 0x66F4, 0x66FA, 0x66FC)
OUT = os.environ.get("LEM_CLIP_OUT", "out/clip.txt")


def install(m):
    from unicorn import UC_HOOK_CODE

    base = m.load_seg * 16
    st = {"n": 0}
    seen = {a: collections.Counter() for a in WORDS}

    def on_frame(uc, ad, size, _):
        if ad - base != FRAME_END:
            return
        st["n"] += 1
        for a in WORDS:
            raw = bytes(uc.mem_read(base + DGROUP + a, 2))
            seen[a][raw[0] | (raw[1] << 8)] += 1

    m.uc.hook_add(UC_HOOK_CODE, on_frame)

    def report():
        lines = [f"{st['n']} frames sampled inside the level"]
        if not st["n"]:
            lines.append("NO FRAMES - the run never reached image 0x018F, so "
                         "this says nothing about the game")
        for a in WORDS:
            vals = ", ".join(f"{v} (0x{v:04x}) x{c}"
                             for v, c in seen[a].most_common())
            lines.append(f"  ds:0x{a:04x} = {vals or 'never sampled'}")
        text = "\n".join(lines)
        try:
            with open(OUT, "w") as f:
                f.write(text + "\n")
        except OSError:
            pass
        print(text, flush=True)

    atexit.register(report)
