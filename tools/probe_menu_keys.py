# SPDX-License-Identifier: GPL-2.0-only
"""What does each plate on the main menu actually DO?

The port can draw the menu; making it functional means knowing what F2, F3, the
rating arrows and F4 change. Guessing from the labels is how a reconstruction
acquires behaviour nobody checked, so this asks the original instead: reach the
menu, capture the screen, press one key, capture again, and report what moved.

  uv run python tools/probe_menu_keys.py
  uv run python tools/probe_menu_keys.py --keys f3 up down

A key that changes nothing at all in this build is a finding, not a failure -
this is a ten-level demo and some of the full game's menu is not wired up.
Because the menu ANIMATES (the lemmings blink), a baseline is taken first with
no key pressed, and anything that moves in the baseline is discounted.
"""
import argparse
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lemmings
from play import ask, wait_for

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def read_scrn(path):
    with open(path, "rb") as f:
        b = f.read()
    if b[:4] != b"SCRN":
        return None, None, None
    w, h = struct.unpack_from("<HH", b, 4)
    return w, h, b[8 + 768:]


def diff(a, b, w):
    """(pixels differing, bounding box) between two index buffers."""
    if a is None or b is None or len(a) != len(b):
        return None, None
    n = 0
    x0 = y0 = 1 << 30
    x1 = y1 = -1
    for i, (p, q) in enumerate(zip(a, b)):
        if p != q:
            n += 1
            x, y = i % w, i // w
            x0, y0 = min(x0, x), min(y0, y)
            x1, y1 = max(x1, x), max(y1, y)
    return n, (x0, y0, x1, y1) if x1 >= 0 else None


def palette_of(path):
    with open(path, "rb") as f:
        b = f.read()
    return b[8:8 + 768]


def lit(path):
    """Is this capture taken with the palette actually up?

    A screen sampled mid-fade has an all-black DAC and every index still in
    place, so it compares as a huge difference for a reason that has nothing to
    do with what was pressed. The first version of this probe reported that F3
    changed 77,407 pixels; it had caught a fade.
    """
    pal = palette_of(path)
    return any(pal)


def one_key(key, settle, after, samples, hold):
    """Drive a FRESH guest to the menu, press one key, and watch.

    A fresh guest per key, because state carries: pressing F2 first left the
    screen mid-fade, and the next key's "before" shot was black."""
    sock = os.path.join(ROOT, "out", f"mk_{key}.sock")
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
        wait_for(sock, lambda st: "mode=0x10" in st, "the main menu",
                 timeout=200)

        # Settle for a fixed time AND require a lit palette. Breaking out of
        # the wait the moment the palette is merely non-black is not enough:
        # mid-fade every entry is non-black and nearly black, and the menu
        # drops keys pressed while it is still fading in.
        time.sleep(settle)
        before = os.path.join(ROOT, "out", f"mk_{key}_before.scrn")
        ask(sock, f"screen {before}")
        if not lit(before):
            return key, "still black after settling", None, None
        w, h, p0 = read_scrn(before)

        # HELD, not instantaneous. A zero-duration press is invisible to a
        # guest that polls the key state between two of its own polls, which
        # is how the first run of this probe reported that F1 does nothing -
        # F1 is the one key we already know works. tools/play.py holds it for
        # the same reason.
        # Pressed more than once and spaced out, as tools/play.py does: there
        # is no cue over the socket for "the menu is ready for input", and one
        # press at the wrong moment is indistinguishable from a key that does
        # nothing. Three tries turns "no response" into a claim worth making.
        for _ in range(3):
            ask(sock, f"key {key} {hold}")
            time.sleep(1.5)
        rows = []
        for i in range(samples):
            time.sleep(after)
            shot = os.path.join(ROOT, "out", f"mk_{key}_{i}.scrn")
            ask(sock, f"screen {shot}")
            st = ask(sock, "status").strip()
            _, _, p1 = read_scrn(shot)
            n, bx = diff(p0, p1, w)
            rows.append((i, n, bx, lit(shot),
                         st.split("mode=")[1].split()[0]))
        return key, None, rows, w
    finally:
        try:
            ask(sock, "quit")
        except Exception:
            pass
        proc.terminate()


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--keys", nargs="*",
                    default=["none", "f2", "f3", "f4", "up", "down"])
    ap.add_argument("--settle", type=float, default=12.0)
    ap.add_argument("--after", type=float, default=2.5)
    ap.add_argument("--samples", type=int, default=3)
    ap.add_argument("--hold", type=int, default=4,
                    help="frames to hold the key down; 0 is invisible to a "
                         "guest that polls the key state")
    a = ap.parse_args()

    print("One fresh run per key. 'none' is the baseline: whatever moves there"
          "\nis the menu's own animation, not a response.\n")
    for key in a.keys:
        k, err, rows, w = one_key(key if key != "none" else "0",
                                  a.settle, a.after, a.samples, a.hold)
        label = "none (baseline)" if key == "none" else key
        if err:
            print(f"  {label:16s} {err}")
            continue
        for i, n, bx, is_lit, mode in rows:
            print(f"  {label:16s} +{(i+1)*a.after:4.1f}s  {n:6d} px  "
                  f"mode {mode}  {'lit' if is_lit else 'MID-FADE'}"
                  f"{'' if not bx else f'  box {bx}'}")
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
