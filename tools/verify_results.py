"""The results screen, driven rather than played to.

Reaching this screen by playing takes fifteen minutes of emulator time: the
level has to be entered, a digger assigned from a script, and every lemming
walked out of the exit. Three runs were spent that way and two produced
nothing, because the screen was captured at the wrong moment or the frame
counter had started before the level did.

It does not have to be reached at all. Image 0x046EB is a plain routine and
the screen is a pure function of a handful of bytes:

    ds:0x40    the rescued percentage      image 0x0471D
    ds:0x41    the level's requirement     image 0x04720
    ds:0x1F6B  the level index             image 0x0472E
    ds:0x20A7, ds:0x20A9   the clock, and image 0x04710 compares it against
               "0:00" to choose between "Your time is up!" and "All lemmings
               accounted for."
    ds:0x25A6  which input the player chose, which picks the prompt wording

So SET those and CALL it, the way tools/verify_digits.py points CS:IP at
image 0x04BC2 rather than waiting for the game to reach it. It stops at image
0x047A9 - the screen's own wait loop, which is the moment everything is drawn
and nothing has happened yet.

    LEM_HOOK=tools/verify_results.py LEM_RESULTS_OUT=out/results \
        uv run python tools/play.py --auto -- --run-seconds 90

What comes back: the composed 640x350 screen, the DAC, the attribute map the
screen is shown through (image 0x053FC picks index 2 of the table at
cs:0x52D4), and the 16x16 font at ds:0x5BC0.
"""
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

ENTRY, STOP = 0x046EB, 0x047A9
SAVED, NEEDED, LEVEL = 0x40, 0x41, 0x1F6B
CLOCK_MIN, CLOCK_SEC = 0x20A7, 0x20A9
INPUT_KIND = 0x25A6
ATTR_MAP = 0x4920          # image 0x053FC's index 2, for the usual adapter
FONT_SEG, FONT_OFF = 0x1FEE, 0x5BC0
GLYPH, FIRST_CH = 96, 0x21
SETTLE = 0x04401           # the menu: by here the image and DS are up


def install(m):
    from unicorn import UC_HOOK_CODE
    from unicorn.x86_const import (UC_X86_REG_DS, UC_X86_REG_CS,
                                   UC_X86_REG_SP)

    base = m.load_seg * 16
    out_dir = os.environ.get("LEM_RESULTS_OUT", "out/results")
    os.makedirs(out_dir, exist_ok=True)
    st = {"done": False}

    def at(uc, ad, size, _):
        if ad - base != SETTLE or st["done"]:
            return
        st["done"] = True
        ds = uc.reg_read(UC_X86_REG_DS) * 16
        cs = uc.reg_read(UC_X86_REG_CS)
        sp = uc.reg_read(UC_X86_REG_SP)

        saved = int(os.environ.get("LEM_RES_SAVED", "100"))
        needed = int(os.environ.get("LEM_RES_NEEDED", "10"))
        level = int(os.environ.get("LEM_RES_LEVEL", "0"))
        timed = os.environ.get("LEM_RES_TIMEUP") == "1"

        uc.mem_write(ds + SAVED, bytes([saved]))
        uc.mem_write(ds + NEEDED, bytes([needed]))
        uc.mem_write(ds + LEVEL, bytes([level]))
        uc.mem_write(ds + INPUT_KIND, bytes([0]))
        # Image 0x04710 reads these as words and compares against 0x3030 -
        # "00". The clock at zero is what "time up" means.
        uc.mem_write(ds + CLOCK_MIN, b"0" if timed else b"4")
        uc.mem_write(ds + CLOCK_SEC, b"00" if timed else b"53")

        # The font, before anything runs - the menu has already loaded it.
        seg = int.from_bytes(uc.mem_read(ds + FONT_SEG, 2), "little")
        seg = (seg + 0x1000) & 0xFFFF                 # image 0x04D9D
        n = 0x7F - FIRST_CH
        font = bytes(uc.mem_read(seg * 16 + FONT_OFF, GLYPH * n))
        open(os.path.join(out_dir, "font.bin"), "wb").write(font)

        attr = list(uc.mem_read(ds + ATTR_MAP, 16))

        # THE BACKGROUND TILE. Image 0x07E76 sets ds:0x1FE4 from ds:0x1FEE and
        # blits from si = 0 - offset zero of that resource - eight times: two
        # 320-wide pieces on each of four rows at y = 0, 104, 208 and 312.
        # Dump enough of it to find in the files the way the font was found.
        rseg = int.from_bytes(uc.mem_read(ds + FONT_SEG, 2), "little")
        tile = bytes(uc.mem_read(rseg * 16, 0x8000))
        open(os.path.join(out_dir, "tile.bin"), "wb").write(tile)

        # THE CURSOR'S STORE, ds:0x1FEA - image 0x05862 loads it as the blit's
        # source segment and image 0x0583B indexes it 144 bytes a shift, with
        # image 0x05854 adding 0x480 for the box. Eight shifts of each shape
        # is 0x900. game.c carries both shapes as tables MEASURED off the
        # original; if the unshifted one is in a file the way the font and the
        # background tile are, it can be read instead.
        # THE CRATER'S MASKS. Image 0x05E73 offsets 0xC60 into the resource
        # at ds:0x1FE8, indexed shift * 66 + row * 3 - eight shifts of
        # twenty-two rows of three bytes, which is 528.
        mseg = int.from_bytes(uc.mem_read(ds + 0x1FE8, 2), "little")
        open(os.path.join(out_dir, "crater.bin"), "wb").write(
            bytes(uc.mem_read(mseg * 16 + 0xC60, 528)))
        # THE BASHER'S, in the same resource at offset 0. Image 0x06025 adds
        # nothing but slice * 240 and shift * 30, and image 0x0600B adds
        # 0x3C0 for a basher facing left - so two directions of four slices
        # of eight shifts of ten rows of three bytes, 1,920 in all.
        open(os.path.join(out_dir, "bash.bin"), "wb").write(
            bytes(uc.mem_read(mseg * 16, 0x780)))

        cseg = int.from_bytes(uc.mem_read(ds + 0x1FEA, 2), "little")
        open(os.path.join(out_dir, "cursor_store.bin"), "wb").write(
            bytes(uc.mem_read(cseg * 16, 0x900)))

        # IS THE BACKGROUND DRAWN, OR IS IT WHAT WAS ALREADY THERE?
        #
        # Image 0x046EB calls the clear at image 0x04DEF twice, once before
        # the mode set at image 0x04BE7 and once after, and image 0x04E32
        # fills with AL = 0 - so the screen ought to be black. Driving the
        # routine from the MENU leaves the menu's pixels in video memory,
        # and anything the clear misses shows through as if the game had
        # drawn it. Wiping first tells the two apart: black means leftover,
        # a texture means the game.
        if os.environ.get("LEM_RES_WIPE") == "1":
            for p in getattr(m, "planes", []):
                p[:] = bytes(len(p))
            print("  [verify_results] video memory wiped before the call")

        uc.reg_write(UC_X86_REG_CS, cs)
        uc.reg_write(UC_X86_REG_SP, sp)

        # WHICH CALL DRAWS WHAT. Image 0x046EB is seven set-up calls and then
        # the text, and reading them one at a time to find which puts the
        # background up is slower than stopping after each and looking. The
        # return addresses are the instruction after each call.
        if os.environ.get("LEM_RES_STEP") == "1":
            import collections
            stops = [0x046EE, 0x046F1, 0x046F4, 0x046F7, 0x046FA,
                     0x046FD, 0x04700, 0x04703, STOP]
            fine = os.environ.get("LEM_RES_FINE")
            if fine:
                # Inside the text pass. Image 0x048BD writes two numbers into
                # strings in DGROUP - which cannot touch the screen - and
                # then jumps to the drawer at image 0x04D4D. If the texture
                # is already there when the DRAWER is entered, nothing that
                # draws put it there.
                stops = [int(v, 0) for v in fine.split(",")]
            at_ip = cs * 16 + ENTRY
            for stop in stops:
                uc.emu_start(at_ip, cs * 16 + stop, 0, 0)
                at_ip = cs * 16 + stop
                try:
                    fb = m.framebuffer()
                except Exception:
                    print(f"    after {stop:#07x}: no framebuffer"); continue
                hist = collections.Counter(fb).most_common(3)
                lit = sum(1 for c in m.palette if any(c))
                print(f"    after image {stop:#07x}: {m.width}x{m.height}, "
                      f"{lit} lit, top indices {hist}")
        else:
            uc.emu_start(cs * 16 + ENTRY, cs * 16 + STOP, 0, 0)

        fb = m.framebuffer()
        with open(os.path.join(out_dir, "results.scrn"), "wb") as f:
            f.write(b"SCRN")
            f.write(struct.pack("<HH", m.width, m.height))
            for c in m.palette:
                f.write(bytes(c))
            f.write(fb)
        lit = sum(1 for c in m.palette if any(c))
        info = {"saved": saved, "needed": needed, "level": level,
                "timed_out": timed, "w": m.width, "h": m.height,
                "lit_dac": lit, "attr_map": attr,
                "attr_pal": list(getattr(m, "attr_pal", [])),
                "dac": [list(c) for c in m.palette]}
        json.dump(info, open(os.path.join(out_dir, "info.json"), "w"))
        print(f"  [verify_results] drove image 0x046EB: {m.width}x{m.height},"
              f" {lit} lit DAC entries")
        print("    attribute map (ds:0x4920): "
              + " ".join(f"{v:02x}" for v in attr))
        if not lit:
            print("    THE PALETTE IS DEAD - do not score colour against"
                  " this; it was captured before the screen was shown.")
        raise SystemExit(0)

    m.uc.hook_add(UC_HOOK_CODE, at)
    print("  [verify_results] will drive image 0x046EB at the menu")
