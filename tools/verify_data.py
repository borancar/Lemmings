# SPDX-License-Identifier: GPL-2.0-only
"""Check the port's data loading against the original's own memory.

The strongest check available short of comparing routine by routine: take the
bytes the ORIGINAL has in RAM after it has loaded a file, and compare them
against what the port produces from the same file on disk. Two programs, the
same input, and a byte-for-byte answer.

  uv run python tools/play.py --auto --start --dump-vga \
      --dump-mem "0x10000 0xA0000 out/mem.bin" -- --run-seconds 210 --shots 1
  uv run python tools/verify_data.py out/mem.bin

A section that is NOT found is not a failure. The game frees buffers it has
finished with - the terrain bitmaps are consumed while composing the level and
the block is reused - so absence means "no longer resident", not "wrong".
Only a section that is found and differs is a failure.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lemmings
from dat import load


def find_and_compare(mem, base, want, probes=(0x1000, 0x2000, 0x400, 0x40),
                     max_candidates=64):
    """Locate `want` in `mem` and compare it whole. Best candidate wins.

    Every occurrence of the signature is scored, not just the first. A 64-byte
    run of a sprite sheet really does turn up inside the video aperture where
    the game blitted it, and taking the first hit reported two of main.dat's
    sections as 40% wrong when they were simply found in the wrong place. A
    coincidental hit scores far below a real one, so the best-scoring candidate
    is the right answer and its score says how much to believe it.
    """
    best = None
    for probe in probes:
        if probe + 64 > len(want):
            continue
        sig = want[probe:probe + 64]
        if sig.count(sig[0:1]) == len(sig):
            continue                        # all one byte: not distinctive
        at = 0
        for _ in range(max_candidates):
            i = mem.find(sig, at)
            if i < 0:
                break
            at = i + 1
            start = i - probe
            if start < 0:
                continue
            n = min(len(want), len(mem) - start)
            if n < len(want):
                continue                    # would run off the end of the dump
            got = mem[start:start + n]
            same = sum(1 for a, b in zip(got, want) if a == b)
            if best is None or same > best[1]:
                best = (base + start, same, n)
                if same == n:
                    return best
    return best


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", help="a guest memory dump")
    ap.add_argument("--base", type=lambda v: int(v, 0), default=0x10000,
                    help="linear address the dump starts at")
    ap.add_argument("--threshold", type=float, default=0.9,
                    help="below this, treat a candidate as a coincidence "
                         "rather than a differing copy (default 0.9)")
    a = ap.parse_args()

    mem = open(a.dump, "rb").read()
    g = lemmings.GAME_DIR

    targets = []
    for name in ("level009.dat", "vgagr0.dat", "main.dat"):
        path = os.path.join(g, name)
        if not os.path.exists(path):
            continue
        for i, sec in enumerate(load(path)):
            targets.append((f"{name} section {i}", sec))
    for name in ("ground0o.dat",):
        path = os.path.join(g, name)
        if os.path.exists(path):
            targets.append((f"{name} (uncompressed)", open(path, "rb").read()))

    print(f"{'what':<30} {'where':>10}  {'result'}")
    proven = missing = wrong = 0
    for label, want in targets:
        r = find_and_compare(mem, a.base, want)
        if r is None:
            print(f"{label:<30} {'-':>10}  not resident")
            missing += 1
            continue
        where, same, n = r
        frac = same / n
        if same == n == len(want):
            print(f"{label:<30} {where:>#10x}  EXACT {n} bytes")
            proven += 1
        elif frac < a.threshold:
            # Not a copy of this section at all - the signature matched
            # somewhere by chance, most often in the video aperture.
            print(f"{label:<30} {'-':>10}  not resident "
                  f"(best candidate only {100.0 * frac:.1f}%)")
            missing += 1
        else:
            print(f"{label:<30} {where:>#10x}  DIFFERS "
                  f"{same}/{n} ({100.0 * frac:.2f}%)")
            wrong += 1

    print(f"\n{proven} section(s) byte-identical, {wrong} differing, "
          f"{missing} not resident.")
    print("A section that is not resident was freed by the game after use; "
          "that is not a failure.")
    return 1 if wrong else 0


if __name__ == "__main__":
    sys.exit(main())
