"""Is the level briefing's background stored data, or generated per run?

It is not the menu's tile - the original draws it in DAC 56..59, which on that
screen are the LEVEL's terrain colours - and it is in neither MAIN.DAT nor
VGAGR0.DAT at any bit alignment. Before searching further it is worth asking
whether there is anything to find: a texture generated at run time would never
be in a file.

Two lit captures of the same screen from two separate runs settle it. Identical
means stored; different means generated, and the search should stop.

  uv run python tools/probe_briefing_bg.py

Both captures are taken through play.capture_lit, which refuses a screen
sampled mid-fade. An earlier attempt at this comparison caught one and reported
that the two runs disagreed on a fifth of their background; they did not, one
of them was simply black.
"""
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lemmings                                            # noqa: F401
from play import ask, wait_for, capture_lit

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def one_run(tag):
    sock = os.path.join(ROOT, "out", f"bbg_{tag}.sock")
    if os.path.exists(sock):
        os.unlink(sock)
    argv = [sys.executable, "-c",
            "import sys; sys.path.insert(0, %r); import lemmings; "
            "lemmings.run(sys.argv[1:])" % HERE,
            "--control-socket", sock, "--run-seconds", "300"]
    proc = subprocess.Popen(argv, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    try:
        wait_for(sock, lambda st: "mode=0x03" in st, "the adapter menu")
        time.sleep(0.6)
        ask(sock, "key 1")
        time.sleep(1.5)
        ask(sock, "key 2")
        wait_for(sock, lambda st: "mode=0x10" in st, "the menu", timeout=200)
        time.sleep(10)
        # F1 into the briefing, held and repeated - the menu drops keys
        # pressed while it is still fading in.
        for _ in range(3):
            ask(sock, "key f1 4")
            time.sleep(2.0)
        time.sleep(6)
        out = os.path.join(ROOT, "out", f"bbg_{tag}.scrn")
        if not capture_lit(sock, out, tries=25, wait=1.0):
            return None
        return out
    finally:
        try:
            ask(sock, "quit")
        except Exception:
            pass
        proc.terminate()


def load(path):
    with open(path, "rb") as f:
        b = f.read()
    w, h = struct.unpack_from("<HH", b, 4)
    return w, h, b[8:8 + 768], b[8 + 768:]


def main():
    paths = []
    for tag in ("a", "b"):
        print(f"run {tag}: driving to the briefing ...")
        p = one_run(tag)
        if p is None:
            print("  never came up out of its fade - measured nothing")
            return 2
        paths.append(p)

    w, h, pa, a = load(paths[0])
    _, _, pb, b = load(paths[1])
    print(f"\n  palettes identical : {pa == pb}")
    same = sum(1 for x, y in zip(a, b) if x == y)
    print(f"  whole screen agrees: {100*same/len(a):.2f}%")
    n = ok = 0
    for y in list(range(0, 70)) + list(range(270, 300)):
        for x in range(w):
            n += 1
            ok += a[y * w + x] == b[y * w + x]
    pct = 100 * ok / n
    print(f"  background-only rows agree: {pct:.2f}%")
    print()
    if pct <= 99.9:
        print("  DIFFERENT between runs - generated with no fixed seed. Nothing"
              "\n  in the data files will match it.")
        return 0
    print("  IDENTICAL between runs - so it is deterministic. That alone does"
          "\n  NOT mean stored: a generator with a fixed seed looks the same.")

    # So search for it, WITH A CONTROL. Searching and finding nothing is only
    # evidence if the same search finds something it should.
    print("\n  searching every data file and the executable for it ...")
    import dat
    blobs = {"the executable": open(
        os.path.join(ROOT, "image", "vgalemmi.decrypted.img"), "rb").read()}
    for f in ("main.dat", "vgagr0.dat", "ground0o.dat", "vgaspec0.dat"):
        try:
            for i, sec in enumerate(dat.load(
                    os.path.join(ROOT, "lemming1.pc", f))):
                blobs[f"{f}[{i}]"] = sec
        except Exception:
            pass

    inv = {v: i for i, v in enumerate(
        [0, 1, 2, 3, 4, 5, 0x14, 7, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D,
         0x3E, 0x3F])}

    def prow(px, y, x0, npx, plane):
        bs = bytearray()
        for i in range(0, npx, 8):
            v = 0
            for k in range(8):
                v = (v << 1) | ((inv.get(px[y * w + x0 + i + k], 0)
                                 >> plane) & 1)
            bs.append(v)
        return bytes(bs)

    def search(px, rows):
        hits = 0
        for y in rows:
            for sh in range(8):
                for plane in range(4):
                    d = prow(px, y, sh, 64, plane)
                    if len(set(d)) <= 1:
                        continue
                    for blob in blobs.values():
                        if d in blob:
                            hits += 1
        return hits

    found = search(a, range(5, 60))
    print(f"    the briefing's background : {found} matches")

    # The control: the MENU's background is stored, in main.dat section 3 at
    # offset 0. The same search must find it, or the search proves nothing.
    menu_path = os.path.join(ROOT, "out", "menu10.scrn")
    if os.path.exists(menu_path):
        _, _, _, mpx = load(menu_path)
        ctrl = search(mpx, range(300, 334))
        print(f"    CONTROL, the menu's       : {ctrl} matches  "
              f"(known to be stored)")
        if ctrl == 0:
            print("\n  the control found nothing either - the SEARCH is broken,"
                  "\n  and says nothing about the briefing.")
            return 2
    if found == 0:
        print("\n  Not a stored bitmap, anywhere, while the same search finds"
              "\n  the menu's. So it is GENERATED at run time, from a fixed"
              "\n  seed. Searching the data files again will not help; finding"
              "\n  the generator would.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
