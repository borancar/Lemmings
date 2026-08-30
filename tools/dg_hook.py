"""The original's DATA SEGMENT, frame by frame - the guest half of
tools/verify_dgroup.py.

It captures at image 0x018F, the game's OWN end-of-frame, for the reason
record_level.py captures there: a wall-clock sample lands mid-redraw and a
torn read does not look like a mistake, it looks like a difference.

DS is read out of the guest rather than assumed. tools/verify_triggers.py
records what assuming a segment cost - the trigger map is SS-relative and
reading it from DGROUP scored the port 47% wrong, which was a fact about the
address. The same rule applies here even though DS is the obvious answer.
"""
import os

FRAME_END = 0x018F          # the game's own end-of-frame
DG_END = 0x4DFD             # reconstruct/src/dgroup.h


def install(m):
    """Runs in the guest via LEM_EXTRA_HOOK."""
    import atexit
    from unicorn import UC_HOOK_CODE
    from unicorn.x86_const import UC_X86_REG_DS

    base = m.load_seg * 16
    out = open(os.environ["LEM_DG_OUT"], "wb")
    cap = int(os.environ.get("LEM_DG_FRAMES", "0"))
    st = {"n": 0, "ds": None}

    def on_frame(uc, address, size, user):
        if address - base != FRAME_END:
            return
        # The SECOND mode 0x0D is the level; the first is the loading screen,
        # and none of this segment means anything until a level is running.
        modes = getattr(m, "video_modes", [])
        if sum(1 for v in modes if v == 0x0D) < 2:
            return
        if cap and st["n"] >= cap:
            return
        ds = uc.reg_read(UC_X86_REG_DS)
        st["ds"] = ds
        out.write(bytes(uc.mem_read(ds * 16, DG_END)))
        st["n"] += 1

    m.uc.hook_add(UC_HOOK_CODE, on_frame)

    def report():
        out.close()
        if st["n"] == 0:
            print("  [dg] NEVER CAPTURED - the run did not reach a level, so "
                  "there is nothing to compare. That is a failed run, not a "
                  "pass.", flush=True)
        else:
            print("  [dg] DS = %04X, %d frames captured"
                  % (st["ds"], st["n"]), flush=True)

    atexit.register(report)
