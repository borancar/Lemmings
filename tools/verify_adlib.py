#!/usr/bin/env python3
"""The transcribed driver against the original's own register stream.

This is the check the sound port rests on, and it is the same shape as
`prove.sh`: not a picture, not a sound, but the thing both sides COMPOSE. Ask
the original's `adlib.dat` for a tune and ask the port's transcription of it
for the same tune, and the two must write the same OPL2 registers, in the
same order, at the same tick. There is no tolerance in that and nothing to
listen to.

It is worth being clear about what it does NOT check. The chip is ymfm and is
nobody's transcription (see reconstruct/src/opl.h); if this passes, the port
writes what the original wrote, and what ymfm then makes of it is ymfm's
business. `reconstruct/tests/opl.c` covers that separately, against the
datasheet.

    # the original, driven to a chosen tune rather than played to
    LEM_HOOK=tools/probe_opl.py LEM_OPL_OUT=out/orig_tune1.txt \
        LEM_OPL_TUNE=1 uv run python tools/play.py --auto -- --run-seconds 90

    # the port's own
    ./reconstruct/lemdev --data lemming1.pc --adlib-tune 1 \
        --adlib-ticks 600 --raw out/port_tune1.txt

    uv run python tools/verify_adlib.py out/orig_tune1.txt out/port_tune1.txt
"""
import argparse
import sys


def read(path):
    out = []
    for line in open(path):
        if line.startswith("#") or not line.strip():
            continue
        f = line.split()
        if len(f) < 4:
            continue
        out.append((int(f[0]), int(f[1]), int(f[2], 16), int(f[3], 16)))
    return out


def drop_detect(w):
    """Drop the card detection from the front of an original's capture.

    driver +0x00A3 writes registers 04 and 02 six times to find out whether
    there is an AdLib in the machine, and it does it during start-up - before
    the game has reprogrammed the PIT, so those writes carry the 18.2 Hz
    divisor a PC boots with and nothing else does.

    The port never does this: it has no card to find. Leaving the prologue in
    offsets every subsequent write by six and turns a stream that agrees into
    one that agrees with NOTHING - which is exactly what it scored first time,
    5.04%, and it read like a broken transcription.
    """
    i = 0
    while i < len(w) and w[i][1] == 0x10000 and w[i][2] in (0x02, 0x04):
        i += 1
    return w[i:], i


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("original")
    ap.add_argument("port")
    ap.add_argument("--limit", type=int, default=12,
                    help="how many differing writes to show")
    args = ap.parse_args()

    a, b = read(args.original), read(args.port)
    a, ndet = drop_detect(a)
    b, _ = drop_detect(b)
    if not a or not b:
        print("  one side is EMPTY: original %d writes, port %d writes"
              % (len(a), len(b)))
        print("  That is a fact about the capture, not a result. A comparison")
        print("  of nothing against something is not a score.")
        return 2

    # Tick numbering starts wherever each run happened to start, so both are
    # rebased. The DELTAS are the claim; the absolute numbers are not.
    t0a, t0b = a[0][0], b[0][0]
    a = [(t - t0a, r, v) for t, d, r, v in a]
    b = [(t - t0b, r, v) for t, d, r, v in b]

    n = min(len(a), len(b))
    if ndet:
        print("  dropped %d detect writes from the original's prologue" % ndet)
    print("  original : %d writes over %d ticks" % (len(a), a[-1][0]))
    print("  port     : %d writes over %d ticks" % (len(b), b[-1][0]))
    print("  comparing the first %d" % n)
    print()

    same_rv = same_all = 0
    first = None
    shown = 0
    for i in range(n):
        ta, ra, va = a[i]
        tb, rb, vb = b[i]
        if (ra, va) == (rb, vb):
            same_rv += 1
            if ta == tb:
                same_all += 1
            continue
        if first is None:
            first = i
        if shown < args.limit:
            print("    #%-6d original %02X=%02X @t%-5d   port %02X=%02X @t%-5d"
                  % (i, ra, va, ta, rb, vb, tb))
            shown += 1

    print()
    print("  register and value agree : %d of %d  (%.2f%%)"
          % (same_rv, n, 100.0 * same_rv / n))
    print("  and at the same tick     : %d of %d  (%.2f%%)"
          % (same_all, n, 100.0 * same_all / n))

    # The absolute tick is not the claim and cannot be. Each side starts
    # counting from wherever its own run began - the original from the
    # game's timer, the port from zero - so rebasing on the first write
    # leaves whatever happened between the request and that write. What has
    # to agree is the GAP between consecutive writes: that is the tempo, and
    # a transcription that got the tempo wrong would drift steadily even
    # while every register matched.
    dsame = dtot = 0
    worst = (0, -1)
    for i in range(1, n):
        da = a[i][0] - a[i - 1][0]
        db = b[i][0] - b[i - 1][0]
        dtot += 1
        if da == db:
            dsame += 1
        elif abs(da - db) > worst[0]:
            worst = (abs(da - db), i)
    print("  the GAP between writes   : %d of %d  (%.2f%%)"
          % (dsame, dtot, 100.0 * dsame / (dtot or 1)))
    if worst[1] >= 0:
        print("  worst gap difference     : %d ticks, at write #%d"
              % (worst[0], worst[1]))
    if first is not None:
        print("  first difference at write #%d" % first)
    if len(a) != len(b):
        print("  NOTE: the streams are different lengths (%d vs %d), so the"
              % (len(a), len(b)))
        print("  tail of the longer one was not compared at all.")
    return 0 if same_rv == n and len(a) == len(b) else 1


if __name__ == "__main__":
    sys.exit(main())
