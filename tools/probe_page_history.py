"""Is the original's PAGE showing a pixel its MASTER no longer has?

    LEM_EXTRA_HOOK=tools/probe_page_history.py LEM_REC_SCRIPT=$PWD/out/dig.txt \\
        uv run python tools/record_level.py --frames 136 --seconds 600 \\
            --out out/p799

IT MUST PRINT TO STDERR. record_level.py runs the guest with
stdout=subprocess.DEVNULL, so a probe that prints to stdout reports nothing
and looks exactly like a probe that found nothing - which is what the first
run of this one did.

Canvas (799, 81) - bit 7 of byte 99 - is where the played comparison's whole
play-area residue lives: 103 frames of one pixel, terrain in the original and
cleared in the port. CLAUDE.md's reading is that the port composes from the
master each frame and is right, and that the original's page carries the
history of a lemming's 24-wide background save. This asks the guest.
"""
from unicorn.x86_const import UC_X86_REG_DS
from unicorn import UC_HOOK_CODE

FRAME_END = 0x018F
MASTER = 0x4A40 + 81 * 0xD0 + 99      # image 0x05DEA's base, 0xD0-byte rows
PAGES  = (0x02C2, 0x27E2)             # the two display pages
PAGE_OFF = 81 * 44 + (159 >> 3)       # mode 0x0D, 44-byte rows
BIT = 7                               # 799 & 7, and 159 & 7

def install(m):
    st = {"n": 0}

    def attr(off):
        v = 0
        for pl in range(4):
            v |= ((m.planes[pl][off] >> (7 - BIT)) & 1) << pl
        return v

    def on_frame(uc, address, size, user):
        modes = getattr(m, "video_modes", [])
        if not modes or modes[-1] != 0x0D:
            return
        n = st["n"]; st["n"] += 1
        if n < 100 or n > 135:
            return
        start = m.crtc.get(0x0C, 0) * 256 + m.crtc.get(0x0D, 0)
        pg = min(PAGES, key=lambda p: abs(p - start))
        print(f"  [799,81] frame {n:3d}  master={attr(MASTER)}  "
              f"page@{pg:#06x}={attr(pg + PAGE_OFF)}  "
              f"other={attr((PAGES[1] if pg == PAGES[0] else PAGES[0]) + PAGE_OFF)}",
              flush=True, file=__import__("sys").stderr)

    base = m.load_seg * 16
    m.uc.hook_add(UC_HOOK_CODE, on_frame, None, base + FRAME_END, base + FRAME_END)
