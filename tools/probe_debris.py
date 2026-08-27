"""What draws the explosion's debris - by watching the writes, not reading.

After a bomber goes off the original has ink ON TOP of the crater that thins
away over forty frames, in the terrain's own colours. Two candidates were
ruled out by reading: the dirty-block repaint at image 0x080FD drains every
marked run each frame, and [si+0x22] - the fifteen image 0x02D4F sets, the
obvious particle count - is written in four places and read in none.

So stop reading and watch. This buckets every write into the EGA window by the
instruction that made it, over two windows of the same run:

    QUIET   frames 400..450, nothing exploding
    BLAST   frames 505..545, the debris thinning

An instruction that writes in the second and not the first is drawing the
debris. Differencing two windows of ONE run is what makes that a fair
comparison - the level, the scroll and the lemmings are otherwise the same.

    LEM_HOOK=tools/probe_debris.py uv run python tools/record_level.py \
        --frames 560 --out out/dbg --script out/nuke.txt --seconds 700
"""
import collections
import os

FRAME_END = 0x018F
VGA_LO, VGA_HI = 0xA0000, 0xB0000
QUIET = (400, 450)
BLAST = (505, 545)


def install(m):
    from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_WRITE
    from unicorn.x86_const import UC_X86_REG_CS, UC_X86_REG_IP

    base = m.load_seg * 16
    st = {"n": 0}
    quiet = collections.Counter()
    blast = collections.Counter()

    def on_frame(uc, ad, size, _):
        if ad - base != FRAME_END:
            return
        st["n"] += 1
        # STOP AS SOON AS THE WINDOW IS PAST. record_level runs on to its
        # --seconds ceiling after the last frame it wants, and with a hook on
        # every guest write that is a very long time for nothing. A probe
        # should end when its question is answered.
        if st["n"] > BLAST[1] + 2:
            report()
            raise SystemExit(0)

    def on_write(uc, access, address, size, value, _):
        if not (VGA_LO <= address < VGA_HI):
            return
        n = st["n"]
        if QUIET[0] <= n < QUIET[1]:
            bucket = quiet
        elif BLAST[0] <= n < BLAST[1]:
            bucket = blast
        else:
            return
        pc = uc.reg_read(UC_X86_REG_CS) * 16 + uc.reg_read(UC_X86_REG_IP)
        bucket[pc - base] += 1

    m.uc.hook_add(UC_HOOK_CODE, on_frame)
    m.uc.hook_add(UC_HOOK_MEM_WRITE, on_write)
    # BOTH: the frame trigger ends the run as soon as the answer is in, and
    # the exit handler catches a run that ends some other way. Either alone
    # has already lost a report here - a SystemExit from inside a Unicorn
    # callback does not reliably reach the interpreter, and a run killed at
    # its ceiling never reaches the trigger.
    import atexit
    atexit.register(lambda: report() if not st.get("said") else None)

    def report():
        st["said"] = True
        # TO A FILE, and flushed. A SystemExit raised inside a Unicorn
        # callback does not always reach the interpreter, and stdout
        # redirected to a file is block-buffered, so a report printed and then
        # killed is a report that never existed. The first run of this probe
        # captured all 550 frames and left an empty log.
        out = open(os.environ.get("LEM_DEBRIS_OUT", "out/debris.txt"), "w")
        def say(t):
            print(t)
            out.write(t + "\n")
        say(f"\n  [probe_debris] reached frame {st['n']}")
        if st["n"] < BLAST[1]:
            say("    THE RUN DID NOT REACH THE BLAST WINDOW - not a result.")
            out.close()
            return
        keys = set(quiet) | set(blast)
        rows = sorted(keys, key=lambda k: -(blast[k] - quiet[k]))
        say(f"    {'image':>9}  {'quiet':>8}  {'blast':>8}   difference")
        for k in rows[:14]:
            say(f"    {k:#09x}  {quiet[k]:8d}  {blast[k]:8d}   {blast[k]-quiet[k]:+d}")
        only = [k for k in rows if quiet[k] == 0 and blast[k]]
        say("    writes ONLY in the blast window: "
            + (", ".join(f"{k:#07x}" for k in only[:10]) or "none"))
        out.close()

    print("  [probe_debris] watching EGA writes in two windows")
