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


def r_anim_ok(r):
    # Only the frames image 0x023F3 does NOT dig on: the counter is on 1..7,
    # so the terrain is frozen and anything that moves is the lemming.
    return 1 <= r["anim"] <= 7


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

        # THE BOX CURSOR. Image 0x05854 picks a cursor 0x480 further into
        # the store when ds:0x1A is 0xFF, and image 0x03DE8 sets that when
        # the scan finds a lemming under the pointer. So the shape is
        # whatever is on screen at the pointer while that flag is up.
        if st.get("boxshot") is None and n:
            flag = b(0x1A)
            if flag == 0xFF:
                fb = m.framebuffer()
                mx = w(0x10) - 8       # the pointer, in screen pixels
                my = w(0x12) - 8
                out.write(f"\n  the BOX cursor (ds:0x1A={flag:#04x}), "
                          f"pointer at screen ({mx}, {my}):\n")
                for yy in range(max(0, my - 12), min(160, my + 12)):
                    out.write("    " + "".join(
                        ("%x" % fb[yy * 320 + xx] if fb[yy * 320 + xx] < 8
                         else ".") if 0 <= xx < 320 else " "
                        for xx in range(mx - 14, mx + 14)) + "\n")
                st["boxshot"] = 1
                out.flush()

        # THE SPRITE, separated from the terrain by holding still.
        #
        # Image 0x023F3 digs only on the two animation frames where the
        # counter comes round to 0 and to 8; on 1..7 it just animates. So
        # across those the terrain does not change and the lemming does -
        # and a pixel that varies is the sprite while one that does not is
        # the level. That is what makes the shape readable: dumping a single
        # frame gives a digger tangled up in the earth it is removing.
        if WAIT + 15 <= st["n"] <= WAIT + 21 and n and r_anim_ok(rec(0)):
            fb = m.framebuffer()
            r = rec(0)
            sx = r["x"] - w(SCROLL)
            st.setdefault("shots", []).append(
                (st["n"], r["anim"], r["y"],
                 [fb[y * 320 + x]
                  for y in range(r["y"] - 22, r["y"] + 4)
                  for x in range(sx - 16, sx + 16)]))

        if st["n"] == WAIT + 23 and st.get("shots"):
            shots = st["shots"]
            W2, H2 = 32, 26
            out.write(f"\n  the sprite, from {len(shots)} frames that do not "
                      f"dig (anims {[a for _, a, _, _ in shots]}):\n")
            for yy in range(H2):
                line = ""
                for xx in range(W2):
                    i = yy * W2 + xx
                    vals = {sh[3][i] for sh in shots}
                    if len(vals) == 1:
                        line += "."          # never changed: the level
                    else:
                        v = shots[0][3][i]
                        line += "%x" % v if v < 16 else "#"
                out.write(f"    {line}\n")
            out.flush()

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

        # A TRACE for tools/verify_dig.py: every frame from the moment the
        # digger is assigned, so the port's own simulation can be run
        # forward on the same start state and compared.
        if n:
            r0 = rec(0)
            if r0["flags"] & 0x10 or st.get("trace"):
                st.setdefault("trace", []).append(
                    dict(f=st["n"], y=r0["y"], x=r0["x"], anim=r0["anim"],
                         flags=r0["flags"], state=r0["state"],
                         dir=r0["dirn"], hot_x=w(ARRAY + 4),
                         hot_y=w(ARRAY + 6)))

        if WAIT - 2 <= st["n"] <= WAIT + 45 and n:
            r = rec(0)
            out.write(f"  f{st['n']:4d} n={n} lem0 x={r['x']:5d} y={r['y']:4d} "
                      f"flags={r['flags']:#06x} anim={r['anim']:3d} "
                      f"state={r['state']:3d} dir={r['dirn']} "
                      f"| sel[0x82]={b(0x82)} drawn[0x83]={b(0x83)} "
                      f"btn[0x14]={w(0x14)} diggers[0x7f]={b(0x7f)} "
                      f"under[0x58]={w(0x58) & 0xffff:#06x}\n")
            out.flush()

        if st["n"] > WAIT + 150:
            st["shaft"] = shaft(uc, ds)
            out.close()
            raise SystemExit(0)
        if st.get("trace") and len(st["trace"]) % 40 == 0:
            st["shaft"] = shaft(uc, ds)     # keep the latest, in case of a
                                            # run that ends early

    # WHAT THE BLIT ACTUALLY READS.
    #
    # Three rounds of factoring the record's 112 into a width and a height
    # got nowhere, and image 0x070BF says why: it multiplies the frame index
    # by [di+0x10], splits the product into a byte offset and a four-bit
    # remainder, and hands the blitter registers that are not pointers of the
    # shape this port uses. So stop deducing where the bytes ought to be and
    # record the ones it reads.
    #
    # 0x070E7 is inside the addressing, with DI still on the lemming's
    # record; 0x07218 is the blit itself. Catching the pair says which
    # lemming the next blit belongs to.
    ADDR, BLIT = 0x070E7, 0x07218
    from unicorn.x86_const import (UC_X86_REG_SI, UC_X86_REG_BX,
                                   UC_X86_REG_DI, UC_X86_REG_BP,
                                   UC_X86_REG_DX, UC_X86_REG_CX)
    bl = {"who": None, "done": 0}

    def at_blit(uc, ad, size, _):
        off = ad - base
        if off == ADDR:
            bl["who"] = uc.reg_read(UC_X86_REG_DI)
            bl["si"] = uc.reg_read(UC_X86_REG_SI)
            bl["bx"] = uc.reg_read(UC_X86_REG_BX)
            bl["bp"] = uc.reg_read(UC_X86_REG_BP)
            if bl["who"] == ARRAY and bl["done"] < 6:
                ds = uc.reg_read(UC_X86_REG_DS) * 16
                fl = int.from_bytes(uc.mem_read(ds + ARRAY + 8, 2), "little")
                if fl & 0x10:
                    dseg = uc.reg_read(UC_X86_REG_DS)
                    out.write(f"  addressing reached for the DIGGER: "
                              f"DS={dseg:#06x} SI={bl['si']:#06x} "
                              f"BX={bl['bx']:#06x} BP={bl['bp']:#06x} "
                              f"flags={fl:#06x}\n")
                    # Image 0x07112: cx = [0x1FE6] + bp. So ds:0x1FE6 is the
                    # store's SEGMENT and BP a paragraph offset into it -
                    # which is what the `>> 4` at image 0x070C6 produces.
                    # The addressing's own DS is not the store's, which is
                    # why reading DS:BP*16 gave nonsense.
                    seg = int.from_bytes(uc.mem_read(ds + 0x1FE6, 2), "little")
                    where = (seg + bl["bp"]) * 16 + bl["si"]
                    out.write(f"    store segment ds:0x1FE6 = {seg:#06x}, "
                              f"+ BP {bl['bp']:#06x}\n")
                    raw = bytes(uc.mem_read(where, 112))
                    out.write(f"    frame at {where:#08x}, decoded as "
                              f"4 bytes x 7 rows x 4 planes:\n")
                    ROWS, BPR = 7, 4

                    def bit(pl, rr, cc):
                        b = raw[pl * ROWS * BPR + rr * BPR + (cc >> 3)]
                        return (b >> (7 - (cc & 7))) & 1

                    for rr in range(ROWS):
                        ink = "".join(
                            ".1234567"[sum(bit(pl, rr, cc) << pl
                                           for pl in range(3))]
                            for cc in range(32))
                        msk = "".join("#" if bit(3, rr, cc) else "."
                                      for cc in range(32))
                        out.write(f"      {ink}   {msk}\n")

                    # THE WHOLE ANIMATION, once. BP steps seven paragraphs a
                    # frame, so the sixteen frames are BP, BP+7, BP+14 ... -
                    # and watching them in a row is what says whether the two
                    # clusters in each are one figure or two.
                    if bl["done"] == 0:
                        out.write("\n    the sixteen frames in a row:\n")
                        # The bytes themselves, so they can be looked for
                        # in the data files.
                        out.write("\n      RAW frame 0: "
                                  + bytes(uc.mem_read(
                                      (seg + bl["bp"]) * 16, 112)).hex()
                                  + "\n")
                        # TWO layouts give 112 with the mask at 84:
                        # 4 bytes x 7 rows and 2 bytes x 14 rows, both in
                        # four planes. The screen showed seven rows of
                        # CHANGING pixels, which a 14-row sprite with a
                        # static top half also produces - so try both and
                        # let the picture decide.
                        for R2, B2 in ((7, 4), (14, 2)):
                            out.write(f"\n      as {B2*8}x{R2}:\n")
                            for fr in range(4):
                                w2 = (seg + bl["bp"] + fr * 7) * 16
                                fb = bytes(uc.mem_read(w2, 112))

                                def bt(pl, rr, cc, d=fb, R=R2, B=B2):
                                    v = d[pl * R * B + rr * B + (cc >> 3)]
                                    return (v >> (7 - (cc & 7))) & 1

                                out.write(f"        frame {fr}:\n")
                                for rr in range(R2):
                                    out.write("          " + "".join(
                                        ".1234567"[sum(bt(pl, rr, cc) << pl
                                                       for pl in range(3))]
                                        for cc in range(B2 * 8)) + "\n")
                    out.flush()
                    bl["done"] += 1
            return
        if off != BLIT or bl["who"] is None or bl["done"] >= 3:
            return
        # Only the record we gave a digger to, and only ONCE IT IS DIGGING.
        # Without the second half this catches the walker's blits at frame 88
        # and stops, which is what the first attempt did.
        if bl["who"] != ARRAY:
            bl["who"] = None
            return
        ds = uc.reg_read(UC_X86_REG_DS) * 16
        flags = int.from_bytes(uc.mem_read(ds + ARRAY + 8, 2), "little")
        if not (flags & 0x10):
            bl["who"] = None
            return
        si = uc.reg_read(UC_X86_REG_SI)
        bx = uc.reg_read(UC_X86_REG_BX)
        out.write(f"\n  blit for the digger: DS={ds >> 4:#06x} "
                  f"SI={si:#06x} BX={bx:#06x} "
                  f"DX={uc.reg_read(UC_X86_REG_DX):#06x} "
                  f"CX={uc.reg_read(UC_X86_REG_CX):#06x} "
                  f"BP={uc.reg_read(UC_X86_REG_BP):#06x}\n")
        out.write(f"    at the addressing: SI={bl['si']:#06x} "
                  f"BX={bl['bx']:#06x} BP={bl['bp']:#06x}\n")
        for name, ptr in (("ink  DS:SI", ds + si), ("mask DS:BX", ds + bx)):
            raw = bytes(uc.mem_read(ptr, 48))
            out.write(f"    {name} {ptr:#08x}: "
                      + " ".join(f"{v:02x}" for v in raw) + "\n")
        bl["done"] += 1
        bl["who"] = None
        out.flush()

    # THE SHAFT ITSELF. Comparing only the lemming's y and animation is not
    # enough: neither the row it clears nor the row it tests changes those
    # while it is still inside solid ground, so a test on state alone agrees
    # with a digger that is cutting the wrong rows. What separates them is
    # the terrain.
    #
    # The guest probes solidity out of the EGA planes - image 0x05DC6 reads
    # (y - 0x10) * 0xD0 + (x >> 3) and tests a bit - so plane 3 at that
    # stride is the map, and this records it over the shaft.
    def shaft(uc, ds):
        r0 = None
        try:
            r0 = st["trace"][0]
        except (KeyError, IndexError):
            return None
        cols = range(r0["x"] - 12, r0["x"] + 12)
        rows = range(r0["y"] - 4, r0["y"] + 40)
        out_map = []
        for yy in rows:
            line = []
            for xx in cols:
                # Image 0x05DEA adds 0x4A40: the solidity map does not
                # start at the segment, it sits after the display page.
                # Leaving it out reads 19,008 bytes too early and gives
                # whole rows uniformly solid or empty, which no dug shaft
                # looks like - that is what the first attempt saw.
                off = (yy - 0x10) * 0xD0 + (xx >> 3) + 0x4A40
                if off < 0 or off >= 0x10000:
                    line.append(0)
                    continue
                line.append((m.planes[3][off] >> (7 - (xx & 7))) & 1)
            out_map.append(line)
        return dict(x0=r0["x"] - 12, y0=r0["y"] - 4, map=out_map)

    # Written on the way out, not at a frame count: a run that ends early -
    # and one did - then still leaves what it managed to record.
    import atexit

    def dump():
        tp = os.environ.get("LEM_DIG_TRACE")
        if tp and st.get("trace"):
            import json
            with open(tp, "w") as f:
                json.dump({"frames": st["trace"],
                           "shaft": st.get("shaft")}, f)
            print(f"  [probe_digger] traced {len(st['trace'])} digging frames")

    atexit.register(dump)
    m.uc.hook_add(UC_HOOK_CODE, at_blit)
    m.uc.hook_add(UC_HOOK_CODE, at)
    print("  [probe_digger] will select the digger and click a lemming")
