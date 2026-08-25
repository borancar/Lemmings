"""Three pictures of a difference: the original, the port, and what differs.

A percentage says how much is wrong. It never says WHAT, and this project has
repeatedly drawn the wrong conclusion from a number that was perfectly
accurate - "objects make the preview worse, so there are no objects" was true
in its arithmetic and false in its conclusion, and one look at where the
differing pixels were would have settled it in a minute.

So whenever a comparison differs, emit these three and look at them:

  <name>-original.png   what the emulator's screen actually holds
  <name>-port.png       what the port composed
  <name>-diff.png       the port, with every differing pixel in magenta and
                        everything that agrees dimmed to a quarter

Both sides are rendered through their OWN palette, so a palette error shows up
as a colour difference rather than silently cancelling: the capture's pixels
are DAC indices and carry the DAC with them, the port's are 0..15 with its own
sixteen triples.

  uv run python tools/diff_png.py --capture out/menu.scrn --raw out/fit.raw \\
      --name out/menu
  uv run python tools/diff_png.py ... --rows 0 77 --scale 2

Writes PNG with nothing but the standard library, so it adds no dependency to
a repository whose point is that its measurements are reproducible.
"""
import argparse
import os
import struct
import sys
import zlib

# The menu's attribute-to-DAC mapping: the port's index i is DAC entry ATTR[i].
ATTR = [0, 1, 2, 3, 4, 5, 0x14, 7] + [0x38 + i for i in range(8)]
INV = {v: i for i, v in enumerate(ATTR)}


def write_png(path, w, h, rows):
    """rows is a list of h bytearrays of 3*w bytes, RGB."""
    raw = bytearray()
    for r in rows:
        raw.append(0)                      # filter type 0
        raw += r

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(chunk(b"IEND", b""))


def read_capture(path):
    """An emulator .scrn: w,h at offset 4, then the 768-byte DAC, then the
    screen as DAC indices."""
    with open(path, "rb") as f:
        b = f.read()
    w, h = struct.unpack_from("<HH", b, 4)
    dac = b[8:8 + 768]
    pix = b[8 + 768:]
    idx = [INV.get(v, -1) for v in pix[:w * h]]
    rgb = [(dac[v * 3], dac[v * 3 + 1], dac[v * 3 + 2]) for v in range(256)]
    return w, h, idx, [rgb[v] for v in pix[:w * h]]


def read_raw(path):
    """The port's LEMRW2: w,h at offset 6, sixteen RGB triples, then indices."""
    with open(path, "rb") as f:
        b = f.read()
    if b[:6] != b"LEMRW2":
        raise SystemExit(f"{path}: not a LEMRW2 file")
    w, h = struct.unpack_from("<HH", b, 6)
    pal = [(b[10 + i * 3], b[11 + i * 3], b[12 + i * 3]) for i in range(16)]
    pix = b[10 + 48:]
    idx = list(pix[:w * h])
    return w, h, idx, [pal[v & 15] for v in idx]


def emit(path, w, h, colours, scale):
    rows = []
    for y in range(h):
        r = bytearray()
        for x in range(w):
            c = colours[y * w + x]
            r += bytes(c) * scale
        for _ in range(scale):
            rows.append(r)
    write_png(path, w * scale, h * scale, rows)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--capture", required=True, help="the emulator's .scrn")
    ap.add_argument("--raw", required=True, help="the port's LEMRW2 output")
    ap.add_argument("--name", required=True,
                    help="output prefix, e.g. out/menu")
    ap.add_argument("--rows", nargs=2, type=int, metavar=("Y0", "Y1"),
                    help="crop to these rows, inclusive")
    ap.add_argument("--cols", nargs=2, type=int, metavar=("X0", "X1"),
                    help="crop to these columns, inclusive")
    ap.add_argument("--scale", type=int, default=1)
    a = ap.parse_args()

    ow, oh, oidx, ocol = read_capture(a.capture)
    pw, ph, pidx, pcol = read_raw(a.raw)
    if (ow, oh) != (pw, ph):
        raise SystemExit(f"size mismatch: capture {ow}x{oh}, port {pw}x{ph}")

    x0, x1 = a.cols if a.cols else (0, ow - 1)
    y0, y1 = a.rows if a.rows else (0, oh - 1)
    cw, ch = x1 - x0 + 1, y1 - y0 + 1

    o_c, p_c, d_c = [], [], []
    differ = 0
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            i = y * ow + x
            o_c.append(ocol[i])
            p_c.append(pcol[i])
            if oidx[i] != pidx[i]:
                differ += 1
                d_c.append((255, 0, 255))
            else:
                r, g, b = pcol[i]
                d_c.append((r // 4, g // 4, b // 4))

    d = os.path.dirname(a.name)
    if d:
        os.makedirs(d, exist_ok=True)
    emit(f"{a.name}-original.png", cw, ch, o_c, a.scale)
    emit(f"{a.name}-port.png", cw, ch, p_c, a.scale)
    emit(f"{a.name}-diff.png", cw, ch, d_c, a.scale)

    tot = cw * ch
    print(f"  region {cw}x{ch} at ({x0},{y0})")
    print(f"  differing : {differ} of {tot}  ({100*(1-differ/tot):.2f}% agree)")
    for suffix in ("original", "port", "diff"):
        print(f"    {a.name}-{suffix}.png")
    if differ == 0:
        print("  nothing differs - the diff image is the port, dimmed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
