#!/usr/bin/env python3
"""Decode DOSBox's raw OPL capture (.dro) into the format probe_opl.py writes.

This is the reference every audio comparison in this project wanted and could
not have. Three recordings were spoiled by their instrument - a float format
read as s16, a master compressor flattening the envelope, a disk driver
draining the mixer faster than the guest filled it - and even once those were
fixed, comparing two different OPL implementations by ear or by correlation
could not separate a tempo difference from a timbre one.

A register stream has none of that. It is what the game asked the chip to do,
in milliseconds, and it compares against ours write for write.

    uv run python tools/dro.py rawopl0001.dro --out out/dosbox_stream.txt
"""
import argparse
import struct
import sys


def decode(path):
    """-> (list of (ms, reg, val), total_ms). Raises on anything unexpected."""
    b = open(path, "rb").read()
    if b[:8] != b"DBRAWOPL":
        raise ValueError("%s: not a DRO capture" % path)
    maj, minor = struct.unpack_from("<HH", b, 8)
    if (maj, minor) != (2, 0):
        raise ValueError("%s: DRO version %d.%d, only 2.0 is decoded here"
                         % (path, maj, minor))
    pairs, total_ms = struct.unpack_from("<II", b, 12)
    hw, fmt, comp, short_code, long_code, cmlen = struct.unpack_from(
        "<BBBBBB", b, 20)
    if comp:
        raise ValueError("%s: compressed DRO, not decoded here" % path)
    if fmt:
        raise ValueError("%s: interleaved format %d, not decoded here"
                         % (path, fmt))
    codemap = b[26:26 + cmlen]
    pos = 26 + cmlen

    out, t = [], 0
    for _ in range(pairs):
        if pos + 1 >= len(b):
            break
        code, val = b[pos], b[pos + 1]
        pos += 2
        if code == short_code:
            t += val + 1                      # milliseconds
            continue
        if code == long_code:
            t += (val + 1) << 8
            continue
        # The high bit selects the second chip on a dual-OPL2; this game is
        # an OPL2 and never sets it, and guessing what to do with it would be
        # inventing behaviour.
        if code & 0x80:
            raise ValueError("%s: second-chip write in an OPL2 capture" % path)
        if code >= len(codemap):
            raise ValueError("%s: code %d past the %d-entry codemap"
                             % (path, code, len(codemap)))
        out.append((t, codemap[code], val))
    return out, total_ms, hw


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dro")
    ap.add_argument("--out", help="write it in probe_opl.py's format")
    ap.add_argument("--limit", type=int, default=16)
    args = ap.parse_args()

    writes, total_ms, hw = decode(args.dro)
    print("  %s: %d register writes over %.1f s  (%s)"
          % (args.dro, len(writes), total_ms / 1000.0,
             {0: "OPL2", 1: "OPL3", 2: "dual OPL2"}.get(hw, "hw %d" % hw)))
    if not writes:
        print("  NOTHING IN IT. That is a fact about the capture.")
        return 1
    for t, r, v in writes[:args.limit]:
        print("    %6d ms  %02X = %02X" % (t, r, v))
    if len(writes) > args.limit:
        print("    ... %d more" % (len(writes) - args.limit))

    if args.out:
        # probe_opl.py's columns are `tick divisor reg val`, and a tick there
        # is a PIT count. DOSBox gives milliseconds, so the divisor written
        # here is the one that makes a tick a millisecond - 1193182/1000 -
        # and the two streams are then comparable on the same clock.
        with open(args.out, "w") as f:
            f.write("# tick divisor reg val   (a tick is 1 ms here)\n")
            for t, r, v in writes:
                f.write("%d 1193 %02X %02X\n" % (t, r, v))
        print("  wrote %s" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
