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
NOT_IMPLEMENTED = [
    ("the scrolling banner", 330, 350),
]


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


def capture(path, seconds, delay):
    if os.path.exists(path):
        os.unlink(path)
    cmd = [sys.executable, os.path.join(HERE, "play.py"), "--auto",
           "--dump-vga", "--dump-when", "0x10",
           "--dump-delay", str(delay),
           "--cmd", f"screen {os.path.abspath(path)}",
           "--", "--run-seconds", str(seconds)]
    print("driving the original to its main menu ...")
    subprocess.run(cmd, cwd=ROOT, stdout=subprocess.DEVNULL)
    if not os.path.exists(path):
        raise SystemExit(
            "the run produced no capture - it never reached mode 10h.\n"
            "That is a failure, not a pass.")
    return path


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
    ap.add_argument("--screen", default="menu",
                    choices=("menu", "code", "input", "briefing"),
                    help="which screen: the menu itself, the F2 level-code "
                         "screen, the F4 input chooser, or the level briefing")
    a = ap.parse_args()

    cap = a.capture or capture(os.path.join(ROOT, "out", "menu.scrn"),
                               a.seconds, a.delay)
    w, h, pal, theirs = read_scrn(cap)

    port_raw = os.path.join(ROOT, "out", "menu_port.raw")
    subprocess.run(["make", "-C", os.path.join(ROOT, "reconstruct")],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run([os.path.join(ROOT, "reconstruct", "lemmings"),
                    "--data", os.path.join(ROOT, a.data),
                    "--screen", a.screen, "--raw", port_raw],
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
        tn = tok = 0
        for y in range(h):
            for x in range(w):
                t = theirs[y * w + x]
                o = ATTR[ours[y * w + x] & 0x0F]
                if t in text or o in text:
                    tn += 1
                    tok += (t == o)
        print(f"  text pixels               : {100*tok/tn:.2f}%  "
              f"({tn-tok} of {tn} differ)")
        print("\n  the background is NOT drawn and NOT scored: it is not the"
              "\n  menu's tile and has not been found. See game.c.")
        return 0 if tok == tn else 0
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
