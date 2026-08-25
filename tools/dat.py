# SPDX-License-Identifier: GPL-2.0-only
"""Lemmings DAT decompression, as a Python oracle for the C port.

Written from the published format description (camanis.net), which is a
*hypothesis* until it is checked against the bytes - so this exists to be
validated against real files before the same algorithm is written in C.

One correction to that description already: the header's words are
BIG-endian, not little. level000.dat begins 03 d8 00 00 08 00 ... and the
decompressed size reads as 0x0800 = 2048 (one level) big-endian, versus a
nonsensical 8 little-endian.
"""
import struct


class Section:
    def __init__(self, bits_in_first, checksum, dec_size, comp_size, data):
        self.bits_in_first = bits_in_first
        self.checksum = checksum
        self.dec_size = dec_size
        self.comp_size = comp_size
        self.data = data          # the compressed payload, header excluded


def split(blob):
    """Cut a DAT file into its sections. Each carries its own 10-byte header."""
    out, pos = [], 0
    while pos + 10 <= len(blob):
        bits_in_first, checksum = blob[pos], blob[pos + 1]
        dec_size = struct.unpack_from(">H", blob, pos + 4)[0]
        comp_size = struct.unpack_from(">H", blob, pos + 8)[0]
        if comp_size < 10 or pos + comp_size > len(blob):
            break
        out.append(Section(bits_in_first, checksum, dec_size, comp_size,
                           blob[pos + 10:pos + comp_size]))
        pos += comp_size
    return out


def decompress(sec):
    """Rebuild a section, back to front, from a bitstream read back to front."""
    data = sec.data
    # Checksum is an XOR of the compressed payload.
    got = 0
    for b in data:
        got ^= b
    if got != sec.checksum:
        raise ValueError(f"checksum {got:#04x} != {sec.checksum:#04x}")

    # Bits come out of each byte LSB first, and bytes are consumed from the
    # end backwards. Only bits_in_first of the last byte are meaningful.
    # Multi-bit values are then assembled MSB first. Both halves of that are
    # needed: any other combination decodes a few opcodes and then references
    # data past the end of the output.
    pos = len(data) - 1
    avail = sec.bits_in_first
    idx = 0

    def get_bit():
        nonlocal pos, avail, idx
        if idx >= avail:
            pos -= 1
            idx = 0
            avail = 8
            if pos < 0:
                raise ValueError("ran off the front of the compressed data")
        b = (data[pos] >> idx) & 1
        idx += 1
        return b

    def get_bits(n):
        v = 0
        for _ in range(n):
            v = (v << 1) | get_bit()
        return v

    out = bytearray(sec.dec_size)
    o = sec.dec_size

    def raw(n):
        nonlocal o
        for _ in range(n):
            o -= 1
            out[o] = get_bits(8)

    def ref(n, offset):
        nonlocal o
        for _ in range(n):
            o -= 1
            out[o] = out[o + offset]

    while o > 0:
        if get_bit():                        # 1..
            t = get_bits(2)
            if t == 0:                       # 100
                ref(3, get_bits(9) + 1)
            elif t == 1:                     # 101
                ref(4, get_bits(10) + 1)
            elif t == 2:                     # 110
                n = get_bits(8) + 1
                ref(n, get_bits(12) + 1)
            else:                            # 111
                raw(get_bits(8) + 9)
        else:                                # 0..
            if get_bit():                    # 01
                ref(2, get_bits(8) + 1)
            else:                            # 00
                raw(get_bits(3) + 1)
    return bytes(out)


def load(path):
    return [decompress(s) for s in split(open(path, "rb").read())]


if __name__ == "__main__":
    import sys
    for p in sys.argv[1:]:
        blob = open(p, "rb").read()
        secs = split(blob)
        total = sum(s.comp_size for s in secs)
        print(f"{p}: {len(blob)} bytes, {len(secs)} section(s), "
              f"{total} accounted for")
        for i, s in enumerate(secs):
            try:
                d = decompress(s)
                ok = f"OK {len(d)} bytes"
                extra = ""
                if len(d) == 2048:
                    extra = f"  name={d[0x7E0:0x800].decode('latin1')!r}"
                print(f"  [{i}] comp={s.comp_size:<6} dec={s.dec_size:<6} {ok}{extra}")
            except Exception as e:
                print(f"  [{i}] comp={s.comp_size:<6} dec={s.dec_size:<6} FAILED: {e}")
