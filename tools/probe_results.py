"""The results screen: its composed picture, and the font it draws with.

The level's recorder cannot reach this screen. tools/record_level.py captures
at image 0x018F, which is the LEVEL loop's own end-of-frame, and the results
screen has a loop of its own - so a recording of a level that was actually
finished stops dead at the fade and looks exactly like a stalled run. It cost
a round here.

The screen is composed by the run of calls at image 0x046EB:

    0x046EB  0x4DEF 0x4BE7 0x53FC 0x531E 0x4DEF 0x5028 0x5017   set-up
    0x04700  0x48BD                        assemble the text
    0x04703  ds:0x4C19 or ds:0x4C2E        "Your time is up!" / "All lemmings
                                           accounted for.", chosen by whether
                                           ds:0x20A7 and ds:0x20A9 are "0:00"
    0x0471A  0x4D4D                        draw it
    0x0471D  ds:0x40 against ds:0x41       won or lost
    0x0472E  the level index advances, and image 0x05042 makes its code

Image 0x04D4D's records carry their own position, which is what makes the
LAYOUT readable at all: one byte of column plus one, one of row, one of
style, then the text, and 0x0D ends a line. A zero column ends the list.

Image 0x04D78 draws a character as 16x16 in three planes from
`(ds:0x1FEE + 0x1000):0x5BC0 + (ch - 0x21) * 96`, at (col * 16, row * 16) -
so the screen is a 20 by 12 grid of cells. The font is BSS in the image and
only exists once a resource has been loaded, which is why it is dumped from
a run rather than read out of the file.

    LEM_HOOK=tools/probe_results.py LEM_RESULTS_OUT=out/results \\
    LEM_REC_SCRIPT=out/dig.txt \\
        uv run python tools/play.py --auto --start --click-tries 4 \\
            -- --run-seconds 900

It needs the script, because a level nobody plays never finishes and the
screen it is after is the one that says you did.
"""
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

FRAME_DONE = 0x018F        # the LEVEL's end-of-frame, for the input script
RESULTS = 0x046EB          # the screen's first instruction
# THE RESULTS SCREEN DOES NOT WAIT FOR A RETRACE.
#
# Its loop at image 0x047A9 polls ds:0x14 for a mouse button and nothing
# else - no call to image 0x01550 - so a probe that captures on a retrace,
# the way the menu ones do, waits for ever and reports nothing. Capture at
# the top of that loop instead: by the time it is reached the screen is
# composed and the only thing left to happen is a click.
WAIT_LOOP = 0x047A9
SAVED, NEEDED, LEVEL = 0x40, 0x41, 0x1F6B
FONT_SEG = 0x1FEE          # ds: the resource segment, image 0x04D99
FONT_OFF = 0x5BC0          # image 0x04D90
GLYPH = 96                 # image 0x04D8E: 16x16 in three planes
FIRST_CH = 0x21            # image 0x04D7C


def install(m):
    import atexit
    from unicorn import UC_HOOK_CODE
    from unicorn.x86_const import UC_X86_REG_DS

    base = m.load_seg * 16
    out_dir = os.environ.get("LEM_RESULTS_OUT", "out/results")
    os.makedirs(out_dir, exist_ok=True)
    st = {"n": 0, "arrived": False, "shots": 0, "retrace": 0,
          "script_at": 0, "at": None, "down": None}

    script = []
    sp = os.environ.get("LEM_REC_SCRIPT")
    if sp:
        for line in open(sp):
            f = line.split("#")[0].split()
            if len(f) >= 2:
                script.append((int(f[0]), f[1], [int(v) for v in f[2:]]))
        script.sort(key=lambda a: a[0])
        print(f"  [probe_results] {len(script)} scripted actions")

    def at(uc, ad, size, _):
        off = ad - base
        if off == FRAME_DONE:
            # THE SAME GUARD tools/record_level.py USES, and leaving it out
            # cost a fifteen-minute run that looked like a slow one.
            #
            # Image 0x018F is reached before the level as well as inside it,
            # so a frame counter without this starts early; the script's click
            # then lands before a single lemming is out, no digger is ever
            # assigned, the level never finishes, and the screen this probe
            # exists to capture never appears. Nothing says so - the run just
            # plays on quietly until its ceiling.
            #
            # The level is the SECOND time the guest enters mode 0x0D. The
            # first is the loading screen.
            modes = getattr(m, "video_modes", [])
            if (not modes or modes[-1] != 0x0D
                    or sum(1 for v in modes if v == 0x0D) < 2):
                return
            st["n"] += 1
            if st["n"] % 100 == 0:
                print(f"  [probe_results] level frame {st['n']}")
            while (st["script_at"] < len(script)
                   and script[st["script_at"]][0] <= st["n"]):
                _, verb, arg = script[st["script_at"]]
                st["script_at"] += 1
                if verb == "move" and len(arg) == 2:
                    st["down"] = None
                    st["at"] = (arg[0] + 8, arg[1] + 8)
                elif verb == "down":
                    st["down"] = st["at"]
                elif verb == "up":
                    st["down"] = None
            if st["at"]:
                m.click_mouse(0, st["at"][0], st["at"][1], bool(st["down"]))
            return

        if off == RESULTS and not st["arrived"]:
            st["arrived"] = True
            ds = uc.reg_read(UC_X86_REG_DS) * 16
            seg = int.from_bytes(uc.mem_read(ds + FONT_SEG, 2), "little")
            seg = (seg + 0x1000) & 0xFFFF          # image 0x04D9D
            n = 0x7F - FIRST_CH
            font = bytes(uc.mem_read(seg * 16 + FONT_OFF, GLYPH * n))
            with open(os.path.join(out_dir, "font.bin"), "wb") as f:
                f.write(font)
            print(f"  [probe_results] reached it; font from segment "
                  f"{seg:#06x} - {len(font)} bytes, {n} glyphs")
            return

        # Once it is up, capture on the screen's own wait - but NOT on the
        # first one. Image 0x046EB is reached before the mode is even set to
        # 0x10, let alone before anything is drawn, so a shot taken at the
        # first retrace after it is a picture of the transition: assembled
        # from planes mid-write, it came out as noise with a dead palette,
        # which reads as a broken capture rather than as one taken too early.
        # Wait for the composition and the fade-in, then take several.
        if off == WAIT_LOOP and st["arrived"]:
            st["retrace"] = st.get("retrace", 0) + 1
            if st["retrace"] % 40 or st["shots"] >= 4:
                return
            ds = uc.reg_read(UC_X86_REG_DS) * 16
            st["saved"] = uc.mem_read(ds + SAVED, 1)[0]
            st["needed"] = uc.mem_read(ds + NEEDED, 1)[0]
            st["level"] = uc.mem_read(ds + LEVEL, 1)[0]
            try:
                fb = m.framebuffer()
            except Exception:
                return
            i = st["shots"]
            st["shots"] += 1
            with open(os.path.join(out_dir, f"r{i}.scrn"), "wb") as f:
                f.write(b"SCRN")
                f.write(struct.pack("<HH", m.width, m.height))
                for c in m.palette:
                    f.write(bytes(c))
                f.write(fb)
            lit = sum(1 for c in m.palette if any(c))
            print(f"  [probe_results] shot {i}: {m.width}x{m.height}, "
                  f"{lit} lit DAC entries"
                  + ("   (FADED - do not score colour)" if not lit else ""))
            if st["shots"] >= 4:
                with open(os.path.join(out_dir, "info.json"), "w") as f:
                    json.dump({"frames": st["n"], "saved": st.get("saved"),
                               "needed": st.get("needed"),
                               "level": st.get("level")}, f)
                print(f"  [probe_results] saved={st.get('saved')} "
                      f"needed={st.get('needed')} "
                      f"level index now {st.get('level')}")
                raise SystemExit(0)

    m.uc.hook_add(UC_HOOK_CODE, at)
    atexit.register(lambda: print(f"  [probe_results] level frames "
                                  f"{st['n']}, arrived={st['arrived']}"))
    print("  [probe_results] will play the level and then dump its results")
