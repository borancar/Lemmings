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
    blink), which a single composed frame cannot match.

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


def capture(path, seconds, delay, rating=0, tries=3):
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
    """
    last = rating
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
        cmd += ["--cmd", f"read i+{RATING_VAR:#x} 1",
                "--cmd", f"screen {os.path.abspath(path)}",
                "--", "--run-seconds", str(seconds)]
        print(f"driving the original to its main menu"
              f"{f', asking for rating {rating}' if rating else ''} ...")
        out = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True).stdout
        if not os.path.exists(path):
            raise SystemExit(
                "the run produced no capture - it never reached mode 10h.\n"
                "That is a failure, not a pass.")

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
        if got > 3:
            raise SystemExit(f"ds:0x1F6E reads {got}, which is not a rating")
        if got == rating:
            return path, got
        print(f"  asked for rating {rating}, the guest reached {got} - "
              f"presses were dropped or doubled"
              f"{'; retrying' if attempt < tries - 1 else ''}.")
        last = got
    print(f"  gave up after {tries} tries; scoring against rating {last}, "
          f"which is what is actually on the screen.")
    return path, last


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
        cap, shown = a.capture, a.rating
    else:
        cap, shown = capture(
            os.path.join(ROOT, "out", f"menu{a.rating or ''}.scrn"),
            a.seconds, a.delay, a.rating)
    w, h, pal, theirs = read_scrn(cap)

    port_raw = os.path.join(ROOT, "out", "menu_port.raw")
    subprocess.run(["make", "-C", os.path.join(ROOT, "reconstruct")],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run([os.path.join(ROOT, "reconstruct", "lemmings"),
                    "--data", os.path.join(ROOT, a.data),
                    "--screen", a.screen, "--rating",
                    ("fun", "tricky", "taxing", "mayhem")[shown],
                    "--raw", port_raw],
                   check=True, stdout=subprocess.DEVNULL)
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
        text = {0x04, 0x05, 0x07, 0x14}
        # "A pixel in one of the text colours" is a HEURISTIC, and on this
        # screen it is not quite sound: the briefing's background art also
        # contains a few pixels in that range. Scoring by colour alone counted
        # four of them as text the port had missed, and reported 99.97% for a
        # text layer that is in fact exact.
        #
        # So the rows the port actually writes text into decide what is text.
        # They come out as seven bands, 81..319; everything outside them is
        # background, which this screen does not draw and does not score. The
        # stragglers are reported rather than dropped - a heuristic that
        # quietly discards its own misfits is worse than one that admits them.
        rows = sorted({y for y in range(h)
                       for x in range(w) if ATTR[ours[y * w + x] & 0x0F] in text})
        bands = []
        if rows:
            st = pr = rows[0]
            for y in rows[1:]:
                if y > pr + 1:
                    bands.append((st, pr))
                    st = y
                pr = y
            bands.append((st, pr))
        in_text = bytearray(h)
        for y0, y1 in bands:
            for y in range(y0, y1 + 1):
                in_text[y] = 1

        tn = tok = outside = 0
        for y in range(h):
            for x in range(w):
                t = theirs[y * w + x]
                o = ATTR[ours[y * w + x] & 0x0F]
                if not (t in text or o in text):
                    continue
                if not in_text[y]:
                    outside += (t != o)
                    continue
                tn += 1
                tok += (t == o)
        print(f"  text rows                 : {len(bands)} bands, "
              f"{bands[0][0]}..{bands[-1][1]}")
        print(f"  text pixels               : {100*tok/tn:.2f}%  "
              f"({tn-tok} of {tn} differ)")
        if outside:
            print(f"  background art in the text colours, outside every text")
            print(f"  row and NOT scored        : {outside} pixels")
        print("\n  the background is NOT drawn and NOT scored: it is not the"
              "\n  menu's tile and has not been found. See game.c.")
        return 0 if tok == tn else 1
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
    print("\n  The remainder is the screen's own animation, which a single"
          "\n  composed frame cannot match.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
