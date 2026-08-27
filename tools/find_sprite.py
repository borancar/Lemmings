"""Find a sprite in MAIN.DAT by SCORING it against the original's own pixels.

The port has three times now needed the file offset of a sprite the image
does not name - the builder's, and now the bomber's cloud - and each time it
was found by hand with a throwaway script. This is that search, written down.

The method, and every part of it is load-bearing:

  * LIFT THE ORIGINAL'S OWN PIXELS. A recorded frame plus the lemming record
    that produced it says exactly where a sprite is on screen: the record
    carries x, y and the hot spot, and the manifest carries the scroll. So
    the rectangle is known and does not have to be guessed at.

  * SCORE, DO NOT DEMAND. A whole round went into "no offset matched", which
    was a fact about the search and not about the file: the input script
    parks the pointer on the lemming it clicked, so the cross-hair sits over
    the sprite and thirty-odd pixels can never agree. Demanding a perfect
    match rejects the right answer.

  * CHECK THE SCORER ON ONE YOU ALREADY KNOW. --check digger runs the same
    search for the digger, whose offset is 720, and says whether it comes
    back. A search that cannot find a sprite it has been told the answer to
    is not evidence about one it has not.

  * EVERY FRAME OF EVERY SPRITE IN THIS FILE LOOKS LIKE A LEMMING. One good
    score is not an answer; --frames asks for the same offset to win on
    several different animation frames.

    uv run python tools/find_sprite.py --ref out/ref_nuke --frame 504 \
        --lemming 0 --w 32 --h 30 --planes 4
"""
import argparse
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from dat import split, decompress            # noqa: E402

LEMMING_DX = 16          # the record-to-canvas offset prove.sh prints
LEMMING_DY = 16
SPRITE_SECTION = 0       # game.h's LEM_SPRITE_SECTION


def load_scrn(path):
    with open(path, "rb") as f:
        if f.read(4) != b"SCRN":
            raise SystemExit(f"{path}: not a SCRN capture")
        w, h = struct.unpack("<HH", f.read(4))
        dac = [tuple(f.read(3)) for _ in range(256)]
        return w, h, dac, f.read(w * h)


def decode(sec, base, w, h, planes):
    """One frame, as a list of h rows of w palette indices."""
    row = (w + 7) // 8
    plane = row * h
    need = plane * planes
    if base + need > len(sec):
        return None
    out = []
    for r in range(h):
        line = []
        for c in range(w):
            v = 0
            for p in range(planes):
                byte = sec[base + p * plane + r * row + (c >> 3)]
                v |= ((byte >> (7 - (c & 7))) & 1) << p
            line.append(v)
        out.append(line)
    return out


def dac(v):
    """The attribute a sprite carries, as the INDEX the capture holds.

    The level's screen is mode 0x0D read back through v < 8 ? v : v + 8, so a
    three-plane sprite (values 0..7) compares as itself and a FOUR-plane one
    does not. Without this the search cannot find any sprite that uses the
    upper eight colours - the bomber's cloud is red, yellow and white - and
    it fails silently, because the check on the digger is three planes and
    passes either way.
    """
    return v if v < 8 else v + 8


def score_at(px, sx, sy, screen, sw, sh):
    """Matching non-zero pixels minus disagreeing ones."""
    good = bad = ink = 0
    for r, line in enumerate(px):
        y = sy + r
        if y < 0 or y >= sh:
            continue
        for c, v in enumerate(line):
            if not v:
                continue
            ink += 1
            x = sx + c
            if x < 0 or x >= sw:
                continue
            if screen[y * sw + x] == dac(v):
                good += 1
            else:
                bad += 1
    return good, bad, ink


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", required=True, help="a record_level.py output dir")
    ap.add_argument("--frame", type=int, required=True)
    ap.add_argument("--frames", type=int, nargs="*", default=[],
                    help="further frames the winner must also win on")
    ap.add_argument("--lemming", type=int, default=0)
    ap.add_argument("--w", type=int, required=True)
    ap.add_argument("--h", type=int, required=True)
    ap.add_argument("--planes", type=int, required=True)
    ap.add_argument("--data", default="lemming1.pc")
    ap.add_argument("--section", type=int, default=SPRITE_SECTION,
                    help="which MAIN.DAT section to search. The walker and its relatives are section 0; a sprite whose record takes its segment from a different ds: word lives elsewhere - the bomber's cloud is one.")
    ap.add_argument("--slack", type=int, default=0,
                    help="how far to slide the sprite around its predicted spot.\n"
                         "DEFAULT ZERO: the record says exactly where the\n"
                         "sprite is, and letting a candidate slide lets a\n"
                         "wrong offset find a flattering spot - it is how\n"
                         "the digger check first came back with 11982.")
    ap.add_argument("--top", type=int, default=5)
    ap.add_argument("--check", action="store_true",
                    help="run the digger first, whose offset is 720")
    a = ap.parse_args()

    root = os.path.dirname(HERE)
    main_dat = os.path.join(root, a.data, "main.dat")
    if not os.path.exists(main_dat):
        main_dat = os.path.join(root, a.data, "MAIN.DAT")
    sec = decompress(split(open(main_dat, "rb").read())[a.section])
    print(f"  section {a.section}: {len(sec)} bytes")

    man = {r["frame"]: r
           for r in json.load(open(os.path.join(root, a.ref, "manifest.json")))}

    def search(frame, w, h, planes):
        row = man.get(frame)
        if row is None or not row.get("lem"):
            print(f"  frame {frame}: no record - nothing to score against")
            return []
        rec = row["lem"][a.lemming]
        scroll = row["scroll"]
        path = os.path.join(root, a.ref, f"f{frame:05d}.scrn")
        sw, sh, _dac, screen = load_scrn(path)
        # The port draws at x + hot_x - LEMMING_DX in the CANVAS; the screen
        # is the canvas shifted by the scroll, itself in record space.
        px0 = rec["x"] + rec["hot_x"] - scroll
        py0 = rec["y"] + rec["hot_y"] - LEMMING_DY
        print(f"  frame {frame}: lemming {a.lemming} at ({rec['x']},{rec['y']})"
              f" hot ({rec['hot_x']},{rec['hot_y']}) scroll {scroll}"
              f" -> screen ({px0},{py0})")
        fsz = ((w + 7) // 8) * h * planes
        hits = []
        for base in range(0, len(sec) - fsz + 1, 2):
            pxl = decode(sec, base, w, h, planes)
            if pxl is None:
                continue
            best = None
            for dy in range(-a.slack, a.slack + 1):
                for dx in range(-a.slack, a.slack + 1):
                    good, bad, ink = score_at(pxl, px0 + dx, py0 + dy,
                                              screen, sw, sh)
                    if ink < 20:
                        continue
                    v = good - bad
                    if best is None or v > best[0]:
                        best = (v, good, bad, ink, dx, dy)
            if best:
                hits.append((best[0], base) + best[1:])
        hits.sort(reverse=True)
        return hits

    if a.check:
        print("\n  CHECKING THE SCORER on the digger, whose offset is 720:")
        h = search(a.frame, 16, 14, 3)
        for v, base, good, bad, ink, dx, dy in h[:3]:
            print(f"    offset {base:6d}  score {v:5d}  {good} of {ink} agree,"
                  f" {bad} differ, slid ({dx},{dy})")
        print("    (720 means the search works; anything else means it does"
              " not, whatever it says about a sprite you do not know.)\n")

    hits = search(a.frame, a.w, a.h, a.planes)
    print(f"\n  best {a.top} for {a.w}x{a.h}x{a.planes}:")
    for v, base, good, bad, ink, dx, dy in hits[:a.top]:
        print(f"    offset {base:6d}  score {v:5d}  {good} of {ink} agree,"
              f" {bad} differ, slid ({dx},{dy})")

    for f2 in a.frames:
        h2 = {b: (v, g, bd, ik, dx, dy)
              for v, b, g, bd, ik, dx, dy in search(f2, a.w, a.h, a.planes)}
        print(f"  the same offsets on frame {f2}:")
        for v, base, *_ in hits[:a.top]:
            r = h2.get(base)
            if r is None:
                print(f"    offset {base:6d}  not scored")
            else:
                print(f"    offset {base:6d}  score {r[0]:5d}"
                      f"  {r[1]} of {r[3]} agree, {r[2]} differ")


if __name__ == "__main__":
    main()
