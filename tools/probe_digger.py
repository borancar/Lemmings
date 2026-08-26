"""Drive the original into assigning a DIGGER, and record what it does.

Nothing in a recorded run ever assigns a skill, so the fourteen handlers the
state dispatch at image 0x01F5E can reach have never been exercised - which is
why the port leaves them alone rather than stepping them wrongly. This reaches
one of them on purpose, so that transcribing it is a differential exercise
against the original rather than a guess.

The digger is the one to reach first: level 1 is "Just dig!" and gives ten of
them. Image 0x03180 dispatches the selected skill by button index and sends 9
to image 0x0320D, which spends one, sets bit 0x10 in [si+8], and points the
record at the digging sprite. Bit 0x10's handler is image 0x023F3.

    LEM_HOOK=tools/probe_digger.py LEM_DIG_OUT=out/dig.txt \
        uv run python tools/play.py --auto --machine 2 --start \
        --click-tries 4 -- --run-seconds 120

It clicks the panel's digger button, then a lemming, and prints the array
around the moment so the before and after are both visible.
"""
import os

FRAME_END = 0x018F      # the game's own end of frame
ARRAY = 0x88            # ds:0x88 - the lemming records
COUNT = 0x43            # ds:0x43 - how many are out
REC = 0x2D              # 45 bytes each, image 0x1FEC adds it
SCROLL = 0x00           # ds:0 - the view's scroll

# The digger's button. Image 0x03F07 takes the index from (mouse_x - 8) >> 4,
# and image 0x03180 sends index 9 to the digger; image 0x03EFF wants
# mouse_y + 8 >= 0xC0 for the click to be on the panel at all.
DIG_BTN_X = 8 + 9 * 16 + 8
DIG_BTN_Y = 190

WAIT = 90               # frames to let some lemmings walk out first


def install(m):
    from unicorn import UC_HOOK_CODE
    from unicorn.x86_const import UC_X86_REG_DS

    out = open(os.environ.get("LEM_DIG_OUT", "out/dig.txt"), "w")
    base = m.load_seg * 16
    st = {"n": 0, "picked": False, "clicked": 0}

    def at(uc, ad, size, _):
        if ad - base != FRAME_END:
            return
        st["n"] += 1
        ds = uc.reg_read(UC_X86_REG_DS) * 16

        def b(off):
            return uc.mem_read(ds + off, 1)[0]

        def w(off):
            v = uc.mem_read(ds + off, 2)
            return int.from_bytes(v, "little", signed=True)

        n = b(COUNT)

        def rec(i):
            o = ARRAY + i * REC
            return dict(x=w(o), y=w(o + 2), flags=w(o + 8) & 0xFFFF,
                        anim=b(o + 0x28), state=b(o + 0x29),
                        alive=b(o + 0x26), dirn=b(o + 0x27))

        # Select the digger, then click the first lemming that is out.
        # HELD, not tapped. The game samples the button through its own
        # handler at image 0x04255 and a press that lands between two of its
        # samples is simply not seen - the same reason verify_menu.py holds
        # keys for two dozen frames rather than pressing them once.
        if WAIT <= st["n"] < WAIT + 8:
            m.click_mouse(0, DIG_BTN_X, DIG_BTN_Y, True)
            if st["n"] == WAIT:
                out.write(f"frame {st['n']}: holding the digger button at "
                          f"({DIG_BTN_X}, {DIG_BTN_Y})\n")
        elif st["n"] == WAIT + 8:
            m.click_mouse(0, DIG_BTN_X, DIG_BTN_Y, False)
        elif st["n"] == WAIT + 12 and n:
            r = rec(0)
            # Image 0x03D0A: the pointer's LEVEL position is
            # `scroll - 16 + mouse_x` across and `mouse_y + 8` down, and
            # image 0x03D27 puts the lemming's 12x12 hit box at
            # (x + hot_x - 4, y + hot_y - 1). Aim at its middle.
            hot_x, hot_y = -8, -10
            lx = r["x"] + hot_x + 2
            ly = r["y"] + hot_y + 5
            sx = lx - w(SCROLL) + 16
            sy = ly - 8
            st["target"] = (sx, sy)
            m.click_mouse(0, sx, sy, True)
            out.write(f"frame {st['n']}: lemming 0 is at level ({r['x']}, "
                      f"{r['y']}), scroll {w(SCROLL)} -> clicking ({sx}, {sy})\n")
        elif WAIT + 12 < st["n"] < WAIT + 22:
            t = st.get("target", (DIG_BTN_X, DIG_BTN_Y))
            m.click_mouse(0, t[0], t[1], True)
        elif st["n"] == WAIT + 22:
            t = st.get("target", (DIG_BTN_X, DIG_BTN_Y))
            m.click_mouse(0, t[0], t[1], False)

        # The whole record, either side of the assignment: the sprite fields
        # the port does not carry are in here - [si+0x0A] and [si+0x0C] are
        # the ink and mask bases, [si+0x10] the bytes a frame and [si+0x12]
        # the mask's offset within it.
        if st["n"] in (WAIT + 11, WAIT + 13) and n:
            raw = uc.mem_read(ds + ARRAY, REC)
            out.write(f"  record at f{st['n']}: "
                      + " ".join(f"{v:02x}" for v in raw) + "\n")
            for off, name in ((0x0A, "ink base"), (0x0C, "mask base"),
                              (0x10, "frame bytes"), (0x12, "mask delta"),
                              (0x14, "?0x14"), (0x1F, "?0x1F")):
                out.write(f"      +{off:#04x} {name:12s} = "
                          f"{w(ARRAY + off) & 0xffff:#06x}\n")

        # The SCREEN while it digs. The sprite's size is the open question -
        # its record says 112 bytes a frame, which is neither the walker's
        # shape nor obviously anything else - so look at the pixels instead
        # of dividing the number again.
        if st["n"] in (WAIT + 3, WAIT + 14, WAIT + 18) and n:
            fb = m.framebuffer()
            r = rec(0)
            sx = r["x"] - w(SCROLL)
            out.write(f"\n  screen at f{st['n']}: lemming 0 at level "
                      f"({r['x']}, {r['y']}), screen x {sx}, anim {r['anim']}\n")
            for y in range(max(0, r["y"] - 24), min(160, r["y"] + 8)):
                # The sprite is drawn in attributes 1..7, which the
                # attribute controller sends to DAC 1..7; the terrain is
                # 8..15, which it sends to 16..23. So anything below 8 is
                # the lemming and everything else is the level.
                line = "".join(
                    ("%x" % fb[y * 320 + x] if fb[y * 320 + x] < 8 else ".")
                    if 0 <= x < 320 else " "
                    for x in range(sx - 24, sx + 24))
                out.write(f"    y{y:3d} {line}\n")
            out.flush()

        if WAIT - 2 <= st["n"] <= WAIT + 40 and n:
            r = rec(0)
            out.write(f"  f{st['n']:4d} n={n} lem0 x={r['x']:5d} y={r['y']:4d} "
                      f"flags={r['flags']:#06x} anim={r['anim']:3d} "
                      f"state={r['state']:3d} dir={r['dirn']} "
                      f"| sel[0x82]={b(0x82)} drawn[0x83]={b(0x83)} "
                      f"btn[0x14]={w(0x14)} diggers[0x7f]={b(0x7f)} "
                      f"under[0x58]={w(0x58) & 0xffff:#06x}\n")
            out.flush()

        if st["n"] > WAIT + 40:
            out.close()
            raise SystemExit(0)

    m.uc.hook_add(UC_HOOK_CODE, at)
    print("  [probe_digger] will select the digger and click a lemming")
