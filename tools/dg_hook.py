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

# IMAGE 0x001C8 - THE END OF THE PASS, which is where the PORT dumps.
#
# Getting this right took three tries and each was the same mistake one level
# down: capturing at a moment that is not the moment the other side captures.
#
#   0x018F  the game's own end-of-frame, where record_level.py saves the
#           SCREEN - but the three instructions AT it are housekeeping, so
#           ds:0x1FCD and ds:0x1FD0 read as they were during the frame just
#           finished. ds:0x1FCD came out 7 against 0 on every frame.
#   0x019D  after those, before the frame's work. Now everything image
#           0x0019D..0x001C5 writes reads one frame behind - ds:0x0052 is
#           bumped at image 0x001A3 and came out 0 against 1.
#   0x01C8  after the whole chain - but BEFORE the end-of-frame tests, and
#           those are not inert: image 0x00213 is `dec byte ptr [0x85]`, the
#           clock. The port dumps after its flip, which is past that point in
#           its own frame, so ds:0x85 and anything else image 0x001C8..0x00105
#           touches was being compared across a seam.
#   0x0105  THE PAGE FLIP, which looks like the one instruction both sides
#           share and is not: the guest's flip is at the TOP of the pass, so
#           its first capture precedes any work while the port's follows a
#           step. Measured, it scored 34 of 51 where 0x01C8 scores 40 - four
#           fields came back that 0x01C8 had settled.
#
# So 0x01C8 it is, with the port dumping BEFORE its flip so image 0x00213's
# `dec ds:0x85` is on the same side of both captures.
#
# A difference that is really a difference of MOMENT looks exactly like a
# difference of behaviour, and there is no percentage that tells them apart.
# Three of the four attempts above produced a plausible, wrong answer.
#
# AND THE RECORDS ARE ONE FRAME AHEAD OF record_level.py's SCREENS, which is a
# fifth way to get this wrong and cost a whole investigation. That tool
# captures the screen at image 0x018F - after image 0x00105's flip, so the
# picture it saves was composed by the PREVIOUS pass - while this captures
# DGROUP at image 0x001C8, which is this pass's state. So
#
#     record frame N  describes  screen frame N + 1
#
# Pairing them N to N made isolated walkers score 100% against MAIN.DAT frame
# anim + 1, seven observations of every value, and that is what a wrong
# alignment looks like: not noise, a clean and completely consistent wrong
# answer. Shifted by one the same sweep gives the IDENTITY - anim N draws frame
# N - which is what the port already does.
#
# 0x018F is the game's own end-of-frame and is where record_level.py captures
# the SCREEN. But the three instructions AT it are housekeeping - ds:0x1FCD
# cleared, ds:0x1FD0 cleared, ds:0x1FD3 bumped - so a hook that fires at 0x018F
# reads those bytes as they were during the frame just finished, while the port
# dumps its own after clearing them. That is not a disagreement, it is two
# different moments, and it showed up as ds:0x1FCD reading 7 against 0 on every
# frame. Capturing at 0x0019D - the first call of the frame's real work - puts
# both sides at the same point.
FRAME_END = 0x001C8
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

    # On the one address: a code hook with no range runs Python on every
    # instruction the guest executes, which a played level cannot afford.
    m.uc.hook_add(UC_HOOK_CODE, on_frame, None,
                  base + FRAME_END, base + FRAME_END)

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
