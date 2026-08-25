"""What works the rating plate?

Nothing found so far does: not Up, not Down, not any function key, and not a
click (the menu ignores where you click). The four ratings are in the
executable at image 0x0E691 - "Fun   TrickyTaxingMayhem", four six-character
slots - so something must.

This sweeps the keys that have not been tried, in ONE run, watching only the
rating plate's own box at (452,120)-(571,180). That is safe because none of
these keys is known to leave the menu, and it is checked: if the whole screen
changes, the sweep stops and says so rather than reporting nonsense from the
next screen along.

  uv run python tools/probe_rating.py
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

PLATE = (452, 120, 572, 181)          # x0, y0, x1, y1 of the rating plate

# Keys not yet tried. F1/F2/F4/Esc are left out: they leave the menu.
KEYS = ["left", "right", "pageup", "pagedown", "kp_minus", "kp_plus",
        "space", "return", "tab", "1", "2", "3", "4", "f5", "f6",
        "plus", "minus", "n", "r"]


def load(path):
    with open(path, "rb") as f:
        b = f.read()
    w, h = struct.unpack_from("<HH", b, 4)
    return w, h, b[8 + 768:]


def region_diff(a, b, w, box):
    x0, y0, x1, y1 = box
    return sum(1 for y in range(y0, y1) for x in range(x0, x1)
               if a[y * w + x] != b[y * w + x])


def main():
    sock = os.path.join(ROOT, "out", "rating.sock")
    if os.path.exists(sock):
        os.unlink(sock)
    argv = [sys.executable, "-c",
            "import sys; sys.path.insert(0, %r); import lemmings; "
            "lemmings.run(sys.argv[1:])" % HERE,
            "--control-socket", sock, "--run-seconds", "600"]
    proc = subprocess.Popen(argv, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    try:
        wait_for(sock, lambda st: "mode=0x03" in st, "the adapter menu")
        time.sleep(0.6)
        ask(sock, "key 1")
        time.sleep(1.5)
        ask(sock, "key 2")
        wait_for(sock, lambda st: "mode=0x10" in st, "the menu", timeout=200)
        time.sleep(12)

        base = os.path.join(ROOT, "out", "rating_base.scrn")
        if not capture_lit(sock, base):
            print("  never came up out of its fade - measured nothing")
            return 2
        w, h, prev = load(base)
        print(f"  watching the rating plate at {PLATE}\n")

        for k in KEYS:
            for _ in range(2):
                ask(sock, f"key {k} 4")
                time.sleep(1.2)
            time.sleep(1.5)
            shot = os.path.join(ROOT, "out", f"rating_{k}.scrn")
            if not capture_lit(sock, shot, tries=6, wait=0.8):
                print(f"  {k:10s} screen went dark - it left the menu. STOP.")
                return 1
            _, _, now = load(shot)
            plate = region_diff(prev, now, w, PLATE)
            whole = sum(1 for x, y in zip(prev, now) if x != y)
            if whole > 20000:
                print(f"  {k:10s} the WHOLE screen changed ({whole}) - it left "
                      f"the menu. STOP.")
                return 1
            mark = "  <-- CHANGED" if plate > 0 else ""
            print(f"  {k:10s} plate {plate:5d} px   whole {whole:6d} px{mark}")
            prev = now
        print("\n  Nothing moved the plate. The rating is not on the keyboard,"
              "\n  or not on any key tried here.")
    finally:
        try:
            ask(sock, "quit")
        except Exception:
            pass
        proc.terminate()
    return 0


if __name__ == "__main__":
    sys.exit(main())
