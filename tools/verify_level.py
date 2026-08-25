# SPDX-License-Identifier: GPL-2.0-only
"""Check the port's LVL header parse against the original's, on the same level.

The original parses a level's 2048-byte header at image 0x036B6 into fixed
DGROUP globals - it has no struct, so the globals *are* the parsed level. This
stops it once that is done (image 0x03742, the first instruction after the
last field is stored), reads the raw level bytes the original was working from,
reads the globals it produced, then runs the port's own parse on those same
bytes and compares field by field.

  uv run python tools/verify_level.py

Where each field lands, read out of 0x036BC..0x0373F:

  [0x0064] [0x0067]  release rate, stored twice
  [0x0047]           number of lemmings
  [0x0044]           number to rescue
  [0x20A7]           time limit, ALREADY +0x30, i.e. an ASCII digit
  [0x006A]..[0x007F] the eight skills, stride 3
  [0x0000]           start x, plus 16
  [0x0002]           that over 8

Every one of those is a byte: the original assembles the word high byte first
and keeps only AL. The port truncates to match, which is why this test can
compare them directly.

The level bytes themselves are at DS:0x0FCB - the buffer image 0x036B9 points
DI at.
"""
import argparse
import ctypes
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

AFTER_PARSE = 0x03742    # `mov al, [0xfe8]`, the first thing after the parse
LEVEL_BUF = 0x0FCB       # DS-relative, from `mov di, 0xfcb` at 0x036B9
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

SKILLS = ("climber", "floater", "bomber", "blocker",
          "builder", "basher", "miner", "digger")


def build_lib(path):
    src = os.path.join(ROOT, "reconstruct", "src")
    subprocess.run(["cc", "-shared", "-fPIC", "-O2", "-Wall", "-Wextra",
                    "-Wconversion", "-I", src, "-o", path,
                    os.path.join(src, "level.c")], check=True)
    lib = ctypes.CDLL(path)
    lib.level_header_fields.restype = ctypes.c_int
    lib.level_header_fields.argtypes = [
        ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_uint16), ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_char_p)]
    return lib


def install(m):
    """Called inside the guest process by tools/play.py, via LEM_HOOK."""
    import json
    from unicorn import UC_HOOK_CODE
    from unicorn.x86_const import UC_X86_REG_DS

    base = m.load_seg * 16
    out_path = os.environ["LEM_VERIFY_OUT"]
    done = {"hit": False}

    def at(uc, ad, size, _):
        if ad - base != AFTER_PARSE or done["hit"]:
            return
        done["hit"] = True
        ds = uc.reg_read(UC_X86_REG_DS) * 16

        def b(off):
            return uc.mem_read(ds + off, 1)[0]

        def w(off):
            lo, hi = uc.mem_read(ds + off, 2)
            return lo | (hi << 8)

        cap = {
            "raw": bytes(uc.mem_read(ds + LEVEL_BUF, 2048)).hex(),
            "release_rate": b(0x64),
            "release_rate_2": b(0x67),
            "lemmings": b(0x47),
            "to_rescue": b(0x44),
            "time_ascii": b(0x20A7),
            "skills": [b(0x6A + 3 * i) for i in range(8)],
            "start_x_plus_16": w(0x0000),
            "start_x_over_8": w(0x0002),
        }
        with open(out_path, "w") as f:
            json.dump(cap, f)
        print(f"  [hook] captured the parsed level -> {out_path}")
        m.quit_requested = True
        uc.emu_stop()

    m.uc.hook_add(UC_HOOK_CODE, at)


def main():
    import json
    import tempfile
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seconds", type=int, default=120)
    a, extra = ap.parse_known_args()

    lib = build_lib(os.path.join(ROOT, "out", "liblevel.so"))

    cap_path = os.path.join(tempfile.gettempdir(), "lem_verify_level.json")
    if os.path.exists(cap_path):
        os.unlink(cap_path)          # a stale file reads as a successful run
    env = dict(os.environ,
               LEM_HOOK=os.path.abspath(__file__),
               LEM_VERIFY_OUT=cap_path)
    cmd = [sys.executable, os.path.join(HERE, "play.py"),
           "--auto", "--start", "--start-delay", "4", "--start-tries", "3",
           "--click-tries", "4", "--",
           "--run-seconds", str(a.seconds)] + list(extra)
    print("driving the original into a level ...")
    subprocess.run(cmd, cwd=ROOT, env=env)

    if not os.path.exists(cap_path):
        print("\n  NOTHING WAS COMPARED. That is a failure, not a pass -\n"
              f"  the run never reached image {AFTER_PARSE:#07x}.")
        return 2
    with open(cap_path) as f:
        cap = json.load(f)

    raw = bytes.fromhex(cap["raw"])
    buf = (ctypes.c_uint8 * 2048).from_buffer_copy(raw)
    out = (ctypes.c_uint16 * 15)()
    err = ctypes.c_char_p()
    if lib.level_header_fields(buf, 2048, out, 15, ctypes.byref(err)) != 0:
        print("  the port refused the level:",
              err.value.decode() if err.value else "?")
        return 1

    rows = [
        ("release rate",   cap["release_rate"],   out[0], "[0x0064]"),
        ("release rate 2", cap["release_rate_2"], out[0], "[0x0067]"),
        ("lemmings",       cap["lemmings"],       out[1], "[0x0047]"),
        ("to rescue",      cap["to_rescue"],      out[2], "[0x0044]"),
        ("time (ASCII)",   cap["time_ascii"], (out[3] + 0x30) & 0xFF,
         "[0x20A7]"),
    ]
    for i, name in enumerate(SKILLS):
        rows.append((name, cap["skills"][i], out[4 + i], f"[{0x6A+3*i:#06x}]"))
    rows.append(("start x + 16", cap["start_x_plus_16"],
                 (out[12] + 0x10) & 0xFFFF, "[0x0000]"))
    rows.append(("(start x+16)/8", cap["start_x_over_8"],
                 ((out[12] + 0x10) >> 3) & 0xFFFF, "[0x0002]"))

    print(f"\n  {'field':<16} {'where':<10} {'original':>9} {'port':>9}")
    bad = 0
    for name, theirs, ours, where in rows:
        ok = theirs == ours
        bad += not ok
        print(f"  {name:<16} {where:<10} {theirs:>9} {ours:>9}   "
              f"{'agree' if ok else '<-- DIFFER'}")
    print(f"\n  fields compared : {len(rows)}")
    print(f"  differing       : {bad}")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
