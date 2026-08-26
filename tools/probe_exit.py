"""Where does the original decide a lemming has reached the exit?

Image 0x02F3B sets the whole state word to 0x2000 when a lemming is over an
exit and not falling, and image 0x02326 then animates eight frames, increments
the saved count at ds:0x46 and removes it. What is NOT obvious is the trigger
area: GROUNDxO gives the exit a rectangle at (6,4) of 1x2, and whether those
are pixels or four-pixel units is exactly the kind of guess this project keeps
getting caught by.

So measure it. This records every lemming's position on the frame its state
becomes 0x2000, which is the trigger area in level coordinates, and the
saved count either side.

    LEM_HOOK=tools/probe_exit.py LEM_EXIT_OUT=out/exit.txt \\
        uv run python tools/play.py --auto --machine 2 --start \\
        --click-tries 4 -- --run-seconds 900

IT HAS TO DIG FIRST. A plain run of "Just dig!" never reaches the exit -
the lemmings walk the upper ledge and that is the whole point of the level -
so this assigns a digger the same way tools/probe_digger.py does, waits for
the shaft to break through, and then watches. The first version did not, and
reported "0 lemmings reached it" after four hundred seconds, which says
nothing about the trigger at all.
"""
import os

FRAME_END = 0x018F
ARRAY, COUNT, REC = 0x88, 0x43, 0x2D
SAVED, TOTAL = 0x46, 0x47


# The digger button: image 0x03F07 takes the index from (mouse_x - 8) >> 4
# and image 0x03180 sends 9 to the digger; image 0x03EFF wants
# mouse_y + 8 >= 0xC0 for the click to count as the panel at all.
DIG_BTN_X = 8 + 9 * 16 + 8
DIG_BTN_Y = 190
WAIT = 90               # let a few walk out before digging


def install(m):
    from unicorn import UC_HOOK_CODE
    from unicorn.x86_const import UC_X86_REG_DS
    import atexit

    out = open(os.environ.get("LEM_EXIT_OUT", "out/exit.txt"), "w")
    base = m.load_seg * 16
    st = {"n": 0, "was": {}, "hits": 0}

    def at(uc, ad, size, _):
        if ad - base != FRAME_END:
            return
        st["n"] += 1
        ds = uc.reg_read(UC_X86_REG_DS) * 16

        def b(o):
            return uc.mem_read(ds + o, 1)[0]

        def w(o):
            return int.from_bytes(uc.mem_read(ds + o, 2), "little", signed=True)

        # Dig a way down, or nothing ever reaches the exit.
        n = b(COUNT)
        if WAIT <= st["n"] < WAIT + 8:
            m.click_mouse(0, DIG_BTN_X, DIG_BTN_Y, True)
        elif st["n"] == WAIT + 8:
            m.click_mouse(0, DIG_BTN_X, DIG_BTN_Y, False)
        elif st["n"] == WAIT + 12 and n:
            o = ARRAY
            # Image 0x03D0A: the pointer's level position is
            # scroll - 16 + mouse_x across and mouse_y + 8 down; image
            # 0x03D27 puts the box at (x + hot_x - 4, y + hot_y - 1).
            lx = w(o) + w(o + 4) + 2
            ly = w(o + 2) + w(o + 6) + 5
            st["click"] = (lx - w(0x00) + 16, ly - 8)
            m.click_mouse(0, st["click"][0], st["click"][1], True)
        elif WAIT + 12 < st["n"] < WAIT + 22 and st.get("click"):
            m.click_mouse(0, st["click"][0], st["click"][1], True)
        elif st["n"] == WAIT + 22 and st.get("click"):
            m.click_mouse(0, st["click"][0], st["click"][1], False)

        if st["n"] % 400 == 0:
            out.write(f"  ... frame {st['n']}, {n} out, saved {b(SAVED)}\n")
            out.flush()

        for i in range(n):
            o = ARRAY + i * REC
            fl = w(o + 8) & 0xFFFF
            if st["was"].get(i) != 0x2000 and fl == 0x2000:
                out.write(f"  frame {st['n']:5d}: lemming {i} entered the exit "
                          f"at ({w(o)}, {w(o + 2)}) - saved {b(SAVED)} of "
                          f"{b(TOTAL)}\n")
                out.flush()
                st["hits"] += 1
            st["was"][i] = fl

        if st["hits"] >= 6:
            out.close()
            raise SystemExit(0)

    m.uc.hook_add(UC_HOOK_CODE, at)
    atexit.register(lambda: out.write(f"  {st['hits']} lemmings reached it\n"))
    print("  [probe_exit] watching for the exit state")
