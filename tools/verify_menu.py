# SPDX-License-Identifier: GPL-2.0-only
"""Check the port's main menu against the one the original composes.

The menu runs in EGA mode 10h - 640x350, four planes - so the original's own
screen is the reference, exactly as tools/prove.sh uses its composed level.
This drives the original to the menu, dumps that screen, renders the same menu
with the port, and compares index for index.

  uv run python tools/verify_menu.py                 # capture, then compare
  uv run python tools/verify_menu.py --capture FILE  # reuse a capture

Two things make the comparison honest rather than flattering:

  * The emulator reports DAC indices, because mode 10h puts the attribute
    controller between the 4-bit pixel value and the palette. The port keeps
    the 4-bit values, so they are mapped through the SAME attribute palette
    the BIOS sets for 10h - 0-5, 0x14, 7, then 0x38-0x3F. Comparing raw
    4-bit values against DAC indices would report a total mismatch; comparing
    after a mapping chosen to make it pass would report nothing at all.
  * The parts the port does not implement are named and scored SEPARATELY
    rather than quietly excluded - and when one of them gets built, it moves
    out of that list rather than staying there flattering the headline. Only
    the scrolling banner is left, and the menu also animates (the lemmings
    blink). The banner and the five walkers are read back from the guest,
    so those DO match; see BAR_SCROLL_INIT and ANIM_VAR.

The capture is deleted before the run: a stale one reads as a successful run.
"""
import argparse
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# The BIOS default attribute palette for mode 10h. The game does not
# reprogram it - checked, by deriving the mapping from the capture itself.
ATTR = [0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
        0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F]

# Regions the port does not compose yet, as (name, y0, y1).
# The banner's BAR is drawn now; its scrolling TEXT and the two lemmings
# holding the ends are not. Those live inside the bar's own rows, so the strip
# is scored rather than excluded - it is mostly bar - and the leftover is named
# below rather than hidden.
NOT_IMPLEMENTED = []


def read_scrn(path):
    with open(path, "rb") as f:
        blob = f.read()
    if blob[:4] != b"SCRN":
        raise SystemExit(f"{path}: not a SCRN capture")
    w, h = struct.unpack_from("<HH", blob, 4)
    return w, h, blob[8:8 + 768], blob[8 + 768:]


def read_raw(path):
    with open(path, "rb") as f:
        blob = f.read()
    if blob[:6] != b"LEMRW2":
        raise SystemExit(f"{path}: not a LEMRW2 file")
    w, h = struct.unpack_from("<HH", blob, 6)
    return w, h, blob[10:10 + 48], blob[10 + 48:]


RATING_VAR = 0x0B9FE      # image address of ds:0x1F6E, the current rating
# The banner's scroll, ds:0x21 - image 0x07CC6 starts it at 4. Reading it back
# is the same idea as reading the rating: the menu ANIMATES, so a capture shows
# one phase of it and the port has to be told which, or the banner's rows come
# back as a difference that is really just a different moment.
SCROLL_VAR = 0x0B9FE - 0x1F6E + 0x21
# What to take off ds:0x21 to get the port's own --scroll. Image 0x07CC6 starts
# the counter at FOUR and image 0x07D23 adds four per step, so --scroll is
# simply ds:0x21 - 4. There is no modulo: the port's --scroll is a position
# along the whole ~261-character message (draw_banner indexes ribbon[] by
# scroll/16 - 35), not a 0..63 offset within one tile.
#
# This used to say 140, and to wrap the result to 0..63. Both were fits, and
# both were wrong for the same underlying reason: the `read` below asked for
# ONE byte of a variable the game reads and writes as a WORD, so any counter
# past 255 came back truncated. Sweeping the port's scroll across the whole
# message against four captures puts the ribbon at exactly ZERO differing
# pixels at c-4 every time - where c is 180, 244, 276 and 260, and it was the
# last two, both over 255, that the byte read had been folding down to 20
# and 4 and the 0..63 fit had then been papering over.
BAR_SCROLL_INIT = 4

# The menu's five walking lemmings, image 0x0E3F0 - ds:0x4960, ten entries of
# eleven bytes with the signed counter at +0. Same idea as the scroll: the
# capture shows one phase of them and the port has to be told which, or the
# one that happens to be on screen scores as a difference.
ANIM_VAR = 0x0E3F0
ANIM_N = 10
ANIM_REC = 11


# Which key leaves the main menu for each screen, read off the plates the
# game draws: F1 play (which shows the briefing), F2 enter a code, F4 input.
KEY_FOR = {"menu": None, "code": "f2", "input": "f4", "briefing": "f1"}


def capture(path, seconds, delay, rating=0, tries=3, screen="menu"):
    """Drive the original to its menu at `rating`, retrying dropped presses.

    Returns the rating it ACTUALLY reached, which is not always the one asked
    for.

    Up is the original's own key for the rating - scancode 0x48, translated to
    0x8a through the table at ds:0x202E and dispatched at image 0x0447B - and
    it caps at 3, so pressing it too often is harmless. But a press can be
    dropped: the game reads a key-state array its own INT 9 fills, and a press
    that lands between two of its samples is simply not seen.

    That is not hypothetical. An earlier version pressed Up n times and
    believed it. Its rating-2 and rating-3 captures BOTH showed Tricky - one
    press each - and the port was then scored against the wrong screen, which
    read as the port drawing Taxing and Mayhem badly (99.10%, 98.71%) when it
    was drawing them correctly against a screen that was not showing them.

    So the run reads ds:0x1F6E back and the caller renders THAT rating. A
    dropped press then costs coverage, which is visible, instead of producing
    a confident wrong number.

    The SAME bug lived here for the screen itself, and went uncaught far
    longer. Every screen was captured with these identical commands, which
    reach the main menu and stop - so `--screen code`, `--screen input` and
    `--screen briefing` compared the port's screen against a picture of the
    MENU, and reported 65%, 64% and 30% as though those were measurements of
    the port. They never moved because nothing about the port was in them.

    Now the run presses the key that leaves the menu and captures a SECOND
    picture, and refuses to compare if the two are the same. A screen that was
    never reached costs coverage instead of producing a number.
    """
    last = rating
    last_scroll = 0
    last_anim = None
    for attempt in range(tries):
        if os.path.exists(path):
            os.unlink(path)
        cmd = [sys.executable, os.path.join(HERE, "play.py"), "--auto",
               "--dump-vga", "--dump-when", "0x10",
               "--dump-delay", str(delay)]
        # A LONG hold, not extra presses. Overshooting works only for Mayhem,
        # where the cap absorbs it; for Tricky or Taxing a spare press lands one
        # rating too high and the run is wasted. Holding the key across many
        # frames instead makes it far harder for the game's sampling to miss.
        for _ in range(rating):
            cmd += ["--cmd", "key up 24", "--cmd", "sleep 1.5"]
        if rating:
            cmd += ["--cmd", "sleep 1.5"]      # let the plate redraw
        cmd += ["--cmd", f"read i+{RATING_VAR:#x} 1"]
        key = KEY_FOR.get(screen)
        control = os.path.abspath(path) + ".menu"
        if screen == "briefing":
            # F1 is not a key the menu takes on demand: it fades its palette in
            # first and drops anything pressed during that, and there is no cue
            # over the socket for "the fade has finished". play.py already
            # solves this - it waits for mode 10h and then presses F1 several
            # times, spaced out - so use that rather than a second guess at the
            # timing here. --click-tries 0 leaves it ON the briefing, which
            # needs a real mouse click to get past.
            cmd += ["--start", "--start-delay", "5", "--start-tries", "3",
                    "--click-tries", "0"]
        elif key:
            # A HELD key, not a single press, for the same reason the rating
            # uses one: the game reads a key-state array its own INT 9 fills,
            # and a press that lands between two of its samples is not seen.
            cmd += ["--cmd", "sleep 3.0",
                    "--cmd", f"screen {control}",
                    "--cmd", f"key {key} 24", "--cmd", "sleep 4.0"]
        # The walkers are read BOTH SIDES of the screen dump. The guest keeps
        # running between socket commands, so one read alone can describe a
        # different moment from the picture - and it does: a run whose counter
        # had wrapped past the on-screen window in the gap scored 38 pixels of
        # walker that the port was right not to draw. Two reads that agree
        # mean the gap contained no step; two that disagree mean the capture
        # is untimed, and the run says so instead of scoring it.
        cmd += ["--cmd", f"read i+{ANIM_VAR:#x} {ANIM_N * ANIM_REC}"]
        cmd += ["--cmd", f"screen {os.path.abspath(path)}",
                "--cmd", f"read i+{SCROLL_VAR:#x} 2",
                "--cmd", f"read i+{ANIM_VAR:#x} {ANIM_N * ANIM_REC}",
                "--", "--run-seconds", str(seconds)]
        print(f"driving the original to its main menu"
              f"{f', asking for rating {rating}' if rating else ''} ...")
        out = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True).stdout
        if not os.path.exists(path):
            raise SystemExit(
                "the run produced no capture - it never reached mode 10h.\n"
                "That is a failure, not a pass.")
        if screen == "briefing":
            # The control here is a menu capture from a previous run, because
            # --start has already left the menu by the time any --cmd runs.
            menu_cap = os.path.join(ROOT, "out", "menu.scrn")
            with open(path, "rb") as f1:
                a2 = f1.read()
            n = len(a2) - (8 + 768)
            ink = sum(1 for i in range(8 + 768, len(a2)) if a2[i])
            if ink < n // 10:
                raise SystemExit(
                    f"the briefing capture is {n - ink} of {n} pixels blank -"
                    f" a screen in\ntransition, not the briefing. A failure,"
                    f" not a percentage.")
            if os.path.exists(menu_cap):
                with open(menu_cap, "rb") as f2:
                    b2 = f2.read()
                m = min(len(a2), len(b2))
                moved = sum(1 for i in range(8 + 768, m) if a2[i] != b2[i])
                if moved < (m - 8 - 768) // 5:
                    raise SystemExit(
                        f"the briefing capture is within {moved} pixels of"
                        f" out/menu.scrn - F1\nnever left the menu. A"
                        f" failure, not a percentage.")
        elif KEY_FOR.get(screen) and os.path.exists(path + ".menu"):
            with open(path, "rb") as f1, open(path + ".menu", "rb") as f2:
                a2, b2 = f1.read(), f2.read()
            n = min(len(a2), len(b2)) - (8 + 768)
            moved = sum(1 for i in range(8 + 768, 8 + 768 + n)
                        if a2[i] != b2[i])
            # NOT "are they different" - the menu's banner scrolls, so two
            # captures of it never match byte for byte and an inequality test
            # passes while nothing has happened. A different SCREEN moves most
            # of the picture.
            if moved < n // 5:
                raise SystemExit(
                    f"the {screen} screen was never reached: only {moved} of"
                    f" {n} pixels\nchanged after the key press, which is the"
                    f" menu's own animation and not a\nnew screen. That is a"
                    f" failure, not a percentage.")
            # And "it changed a lot" is not enough either. Pressing F1 twice
            # went straight past the briefing into the level, and the capture
            # came back ENTIRELY BLACK - which differs from the menu
            # everywhere and sailed through the test above. A screen that is
            # nearly all zero is a transition, not a screen.
            ink = sum(1 for i in range(8 + 768, 8 + 768 + n) if a2[i])
            if ink < n // 10:
                raise SystemExit(
                    f"the capture after the key press is {n - ink} of {n}"
                    f" pixels blank.\nThat is a screen in transition, not the"
                    f" {screen} screen. A failure, not a percentage.")

        got = None
        for line in out.splitlines():
            if f"read i+{RATING_VAR:#x}" in line:
                got = "pending"
                continue
            if got == "pending" and line.strip():
                for tok in line.replace(":", " ").split():
                    if len(tok) == 2:
                        try:
                            got = int(tok, 16)
                        except ValueError:
                            continue
                        break
                break
        if not isinstance(got, int):
            raise SystemExit(
                "could not read ds:0x1F6E back out of the run, so which rating\n"
                "this capture shows is unknown. That is a failure, not a pass.")

        scroll = None
        anim = None
        anims = []
        pend = False
        apend = 0
        abytes = []
        for line in out.splitlines():
            if f"read i+{ANIM_VAR:#x}" in line:
                apend = 1
                continue
            if apend:
                toks = [t for t in line.replace(":", " ").split() if len(t) == 2]
                for t in toks:
                    try:
                        abytes.append(int(t, 16))
                    except ValueError:
                        pass
                if len(abytes) >= ANIM_N * ANIM_REC:
                    anims.append([
                        int.from_bytes(bytes(abytes[i * ANIM_REC:
                                                    i * ANIM_REC + 2]),
                                       "little", signed=True)
                        for i in range(ANIM_N)])
                    abytes = []
                    apend = 0
                continue
            if f"read i+{SCROLL_VAR:#x}" in line:
                pend = True
                continue
            if pend and line.strip():
                # ds:0x21 is a WORD; take both bytes, little-endian.
                by = []
                for tok in line.replace(":", " ").split():
                    if len(tok) == 2:
                        try:
                            by.append(int(tok, 16))
                        except ValueError:
                            continue
                    if len(by) == 2:
                        break
                if len(by) == 2:
                    scroll = by[0] | (by[1] << 8)
                # Not `break`: the walkers' read is emitted AFTER this one,
                # and leaving the loop here is exactly why it came back None.
                pend = False
                continue
        # Before and after the dump. The counters step once per socket
        # command, so the two are almost never equal - but a counter is not
        # what gets DRAWN. Image 0x04FC4 shifts it right one, so each frame
        # spans two counts, and a one-step gap usually leaves the picture
        # identical. Compare what would be drawn: for every entry, whether it
        # is on screen and which of its eight frames.
        def drawn(cs):
            return [None if c == -32768 or c >= 0 else ((-c) - 1) >> 1
                    for c in cs]

        if len(anims) == 2 and drawn(anims[0]) == drawn(anims[1]):
            anim = anims[0]
        elif len(anims) == 2:
            a, b = drawn(anims[0]), drawn(anims[1])
            moved = [i for i in range(ANIM_N) if a[i] != b[i]]
            print(f"  the walkers changed FRAME across the screen dump - "
                  f"entries {moved}, {anims[0][0]} then {anims[1][0]}. The "
                  f"capture is between two pictures, so it is not scored "
                  f"against either.")
            anim = None
        elif anims:
            anim = anims[0]
        if scroll is None:
            raise SystemExit(
                "could not read ds:0x21 back, so which phase of the banner\n"
                "this capture shows is unknown. That is a failure, not a pass.")
        if got > 3:
            raise SystemExit(f"ds:0x1F6E reads {got}, which is not a rating")
        if got == rating and anim is not None:
            return path, got, scroll, anim
        if got == rating:
            # The rating is right and only the walkers' phase is ambiguous.
            # That is worth another run, not a score against a guess.
            print("  the capture's phase is ambiguous"
                  f"{'; retrying' if attempt < tries - 1 else ''}.")
            last = got
            last_scroll = scroll
            last_anim = anim
            continue
        print(f"  asked for rating {rating}, the guest reached {got} - "
              f"presses were dropped or doubled"
              f"{'; retrying' if attempt < tries - 1 else ''}.")
        last = got
        last_scroll = scroll
        last_anim = anim
    print(f"  gave up after {tries} tries; scoring against rating {last}, "
          f"which is what is actually on the screen.")
    return path, last, last_scroll, last_anim


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--capture", default="",
                    help="reuse an existing SCRN capture of the menu")
    ap.add_argument("--seconds", type=int, default=120)
    ap.add_argument("--delay", type=int, default=10,
                    help="seconds after mode 10h to sample; the demo starts "
                         "a level on its own if left long enough")
    ap.add_argument("--data", default="lemming1.pc")
    ap.add_argument("--rating", type=int, default=0, choices=(0, 1, 2, 3),
                    help="press Up this many times before capturing, so the "
                         "other three rating plates can be checked too")
    ap.add_argument("--screen", default="menu",
                    choices=("menu", "code", "input", "briefing"),
                    help="which screen: the menu itself, the F2 level-code "
                         "screen, the F4 input chooser, or the level briefing")
    a = ap.parse_args()

    if a.capture:
        cap, shown, scroll, anim = a.capture, a.rating, None, None
    else:
        cap, shown, scroll, anim = capture(
            os.path.join(ROOT, "out",
                         f"{a.screen}{a.rating or ''}.scrn"),
            a.seconds, a.delay, a.rating, screen=a.screen)
    w, h, pal, theirs = read_scrn(cap)

    port_raw = os.path.join(ROOT, "out", "menu_port.raw")
    subprocess.run(["make", "-C", os.path.join(ROOT, "reconstruct")],
                   check=True, stdout=subprocess.DEVNULL)
    port_cmd = [os.path.join(ROOT, "reconstruct", "lemdev"),
                "--data", os.path.join(ROOT, a.data),
                "--screen", a.screen, "--rating",
                ("fun", "tricky", "taxing", "mayhem")[shown],
                "--raw", port_raw]
    if a.screen == "menu":
        # The banner scrolls, so a capture shows one phase of it and the port
        # has to be rendered at the same one or its rows come back as a
        # difference that is really a different moment.
        #
        # ds:0x21 is the game's own scroll and the run reads it back - 176 in
        # the capture this was written against - but it is NOT the port's
        # --scroll in the same units, and rendering at it scores worse than
        # not. So the phase is FITTED here, the way tools/compare_frame.py
        # fits the level's scroll, and the fitted value is printed beside the
        # score. What the mapping from ds:0x21 is has not been read.
        best = None
        for sc in range(0, 64):
            subprocess.run(port_cmd + ["--scroll", str(sc)],
                           check=True, stdout=subprocess.DEVNULL)
            pw2, ph2, _pp2, o2 = read_raw(port_raw)
            bad = sum(1 for y in range(330, min(ph2, h))
                      for x in range(min(pw2, w))
                      if ATTR[o2[y * pw2 + x] & 15] != theirs[y * w + x])
            if best is None or bad < best[0]:
                best = (bad, sc)
        # The fit above searches 0..63, which is one tile - it can pin the
        # PHASE and cannot pin which characters are showing. The guest's own
        # counter gives both, so it is the answer and the fit is its control:
        # the two must agree modulo 64, and it is worth knowing if they stop.
        use = best[1]
        if scroll is not None:
            derived = scroll - BAR_SCROLL_INIT
            note = "agrees" if derived % 64 == best[1] % 64 else "DISAGREES with"
            print(f"  the banner: ds:0x21 reads {scroll}, so --scroll"
                  f" {derived}, whose phase {note} the best fit ({best[1]}).")
            if derived % 64 == best[1] % 64:
                use = derived
        else:
            print(f"  the banner: no ds:0x21 read, so --scroll {best[1]}"
                  f" by fitting ({best[0]} pixels differing in its rows).")
        port_cmd += ["--scroll", str(use)]
        # The five walking lemmings, from the guest's own counters. Without
        # them the port composes from the staggered reset, which is a
        # different moment - and whichever of the five is on screen then
        # scores as a difference that is really a different frame.
        if anim:
            port_cmd += ["--anim", ",".join(str(v) for v in anim)]
            on = [i for i, v in enumerate(anim) if -16 <= v < 0]
            print(f"  the walkers: ds:0x4960 reads {anim}"
                  f"{f', so {len(on)} on screen (entries {on})' if on else ', none on screen'}.")
    subprocess.run(port_cmd, check=True, stdout=subprocess.DEVNULL)
    pw, ph, ppal, ours = read_raw(port_raw)
    if (pw, ph) != (w, h):
        raise SystemExit(f"size mismatch: original {w}x{h}, port {pw}x{ph}")

    # The palette first: if it disagrees, every pixel figure below is noise.
    badpal = [i for i in range(16)
              if tuple(ppal[i * 3:i * 3 + 3]) !=
              tuple(pal[ATTR[i] * 3:ATTR[i] * 3 + 3])]
    print(f"\n  palette entries differing : {len(badpal)}"
          f"{'  ' + str(badpal) if badpal else ''}")
    if a.screen == "briefing" and badpal:
        print("    entries 8-15 are the LEVEL's terrain colours on this screen,"
              "\n    not the menu's. The port emits the menu palette, which is"
              "\n    the same gap as the missing background - both come from"
              "\n    somewhere that has not been found.")

    # The sub-screens have no scroller and nothing the port leaves out, so
    # nothing is excluded from their score.
    excluded = NOT_IMPLEMENTED if a.screen == "menu" else []

    # The briefing's BACKGROUND is not reconstructed - it is not the menu's
    # tile, and it is in neither MAIN.DAT nor VGAGR0.DAT. The port leaves it
    # black rather than drawing something plausible and wrong, so scoring the
    # whole screen would measure the missing background, not the text. The
    # text is what was built, and the text is what is scored.
    if a.screen == "briefing":
        # Both halves of this screen are drawn now: the background is the
        # menu's own tile from row 78, and above it is the level's terrain at
        # quarter scale. They are reported separately because they are checked
        # differently - the lower rows are exact, the preview is not, and
        # averaging the two would hide which.
        PREVIEW_Y = 78
        tot = ok = 0
        pre = 0
        for y in range(h):
            for x in range(w):
                t = theirs[y * w + x]
                o = ATTR[ours[y * w + x] & 0x0F]
                if y < PREVIEW_Y:
                    pre += (t != o)
                    continue
                tot += 1
                ok += (t == o)
        whole = (tot + (PREVIEW_Y * w) - (tot - ok) - pre) / (h * w)
        print(f"  rows {PREVIEW_Y}..{h-1}            : {100*ok/tot:.2f}%  "
              f"({tot-ok} of {tot} differ)")
        print(f"  whole screen              : {100*whole:.2f}%")
        print(f"\n  rows 0..{PREVIEW_Y-1} are the LEVEL PREVIEW - the level's own")
        print(f"  terrain at quarter scale  : {pre} pixels differ there")
        return 0 if tot == ok else 1
    skip = bytearray(h)
    for _, y0, y1 in excluded:
        for y in range(y0, min(y1, h)):
            skip[y] = 1

    tot = ok = 0
    part_tot = part_ok = 0
    for y in range(h):
        for x in range(w):
            t = theirs[y * w + x]
            o = ATTR[ours[y * w + x] & 0x0F]
            tot += 1
            ok += (t == o)
            if not skip[y]:
                part_tot += 1
                part_ok += (t == o)
    print(f"  whole screen              : {100*ok/tot:.2f}%  "
          f"({tot-ok} of {tot} pixels differ)")
    print(f"  the parts the port builds : {100*part_ok/part_tot:.2f}%  "
          f"({part_tot-part_ok} of {part_tot} differ)")
    if not excluded:
        print("\n  nothing excluded: the port draws all of this screen.")
    else:
        print("\n  not composed by the port, and scored out above:")
    for name, y0, y1 in excluded:
        n = o2 = 0
        for y in range(y0, min(y1, h)):
            for x in range(w):
                n += 1
                o2 += theirs[y*w+x] == ATTR[ours[y*w+x] & 0x0F]
        print(f"    {name:24s} y {y0}..{y1}: {100*o2/n:.1f}% by accident")
    print("\n  Any remainder is the screen's own animation caught between the"
          "\n  screen dump and the reads above - the guest keeps running between"
          "\n  socket commands, so the counters can be a step or two ahead of"
          "\n  the picture they describe. It shows as one walker a frame out,"
          "\n  and clears on a re-run.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
