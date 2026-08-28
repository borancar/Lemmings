"""The port's access-code generator against the original's, call by call.

Image 0x05042 turns three bytes - the level index at ds:0x1F6B, ds:0x491D, and
the rescued percentage at ds:0x40 - into the ten characters the results screen
offers and the code screen at image 0x051A2 takes back.

The port does not READ that routine's ten mask-and-shift blocks. Reading them
off by eye is how this port has twice put a bit in the wrong column, and an
automatic walk over the disassembly produced a plausible table whose last four
rows were stale register state. The table in game.c was MEASURED instead, by
driving the original's own routine one input bit at a time - and this is what
turns that measurement into a check:

  * a single-bit sweep determines the permutation only if the contributions
    never interact. So the run also drives 400 many-bit inputs, over every
    value of level & 7 so that every rotation count is exercised, and this
    compares the port against all of them.

  * the round trip through level_code_ok() is NOT this check. A wrong table
    still round-trips, because the checksum is computed from whatever the
    generator produced - all three deliberate faults in the port's test pass
    it. Only the original's own output settles the shuffle.

    uv run python tools/verify_code.py             # reuse out/code.json
    uv run python tools/verify_code.py --drive     # record it again first

The recording needs a guest that has unpacked the image and established DS, so
it rides along with play.py through tools/probe_code.py rather than building a
machine in process.
"""
import argparse
import ctypes
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import lemmings   # noqa: E402  (after sys.path)
CASES = os.path.join(ROOT, "out", "code.json")
CASES_IN = os.path.join(ROOT, "out", "codein.json")


def build():
    src = os.path.join(ROOT, "reconstruct", "src")
    so = os.path.join(ROOT, "out", "libcode.so")
    lib = lemmings.game_lib()
    lib.level_code_generate.restype = None
    lib.level_code_generate.argtypes = [ctypes.c_uint8, ctypes.c_uint8,
                                        ctypes.c_uint8, ctypes.c_char_p]
    lib.level_code_ok.restype = ctypes.c_int
    lib.level_code_ok.argtypes = [ctypes.c_char_p]
    return lib


def record():
    """Drive the original. Delete the capture first - a stale one reads as a
    successful run, which has misled this project three times."""
    if os.path.exists(CASES):
        os.unlink(CASES)
    env = dict(os.environ, LEM_HOOK=os.path.join(HERE, "probe_code.py"),
               LEM_CODE_OUT=CASES)
    subprocess.run([sys.executable, os.path.join(HERE, "play.py"), "--auto",
                    "--", "--run-seconds", "60"], env=env, check=False)
    if not os.path.exists(CASES):
        sys.exit("  the run produced no capture - it never reached image "
                 "0x04401, so this proves nothing")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--drive", action="store_true",
                    help="record from the original again before comparing")
    ap.add_argument("--max", type=int, default=8,
                    help="how many differences to print")
    a = ap.parse_args()

    if a.drive or not os.path.exists(CASES):
        record()
    cases = json.load(open(CASES))
    lib = build()

    buf = ctypes.create_string_buffer(11)
    bad, single, multi, n_single, n_multi = [], 0, 0, 0, 0
    for c in cases:
        lv, ot, pc = c["level"], c["other"], c["percent"]
        want = bytes.fromhex(c["code"])
        lib.level_code_generate(lv, ot, pc, buf)
        got = buf.raw[:10]
        one = bin(lv).count("1") + bin(ot).count("1") + bin(pc).count("1") <= 1
        if one:
            n_single += 1
        else:
            n_multi += 1
        if got != want:
            bad.append((lv, ot, pc, want, got))
            if one:
                single += 1
            else:
                multi += 1

    print(f"\n  image 0x05042, driven {len(cases)} times")
    print(f"    one bit at a time : {n_single - single}/{n_single} agree")
    print(f"    many bits set     : {n_multi - multi}/{n_multi} agree")
    for lv, ot, pc, want, got in bad[:a.max]:
        print(f"    level {lv:3d} other {ot:3d} percent {pc:3d}: "
              f"original {want.decode('latin1')} port {got.decode('latin1')}")
    if len(bad) > a.max:
        print(f"    ... and {len(bad) - a.max} more")

    # A weaker claim, stated as the weaker claim it is.
    round_trip = sum(lib.level_code_ok(
        (lambda: (lib.level_code_generate(c["level"], c["other"],
                                          c["percent"], buf), buf.raw[:10])
         )()[1]) != 0 for c in cases)
    print(f"    and {round_trip}/{len(cases)} pass image 0x051D4's checksum "
          f"- which a WRONG table also does")

    # ---------------------------------------------------------------- and
    # THE OTHER DIRECTION, image 0x051A2, which is a real check rather than
    # the round trip above: tools/probe_codein.py drives the ORIGINAL's own
    # validator and records what it decided, and this compares the port's
    # against it - the verdict, the level index it set, the rating and the
    # percentage.
    #
    # The codes it is driven over come from the original's GENERATOR, in the
    # same run, so neither side of this comes from the port. The first
    # version of that probe built them in Python from the measured bit table
    # and the original refused 108 of 123: it had applied the DECODER's
    # rotation count while encoding. Reimplementing is what this project
    # keeps writing down not to do, and it did it again.
    bad_in = []
    if os.path.exists(CASES_IN):
        cin = json.load(open(CASES_IN))
        lib.level_code_apply.restype = ctypes.c_int
        lib.level_code_apply.argtypes = [ctypes.c_char_p]
        for c in cin:
            lib.level_index_set(0)
            ok = 1 if lib.level_code_apply(c["code"].encode("latin1")) else 0
            if ok == 0 and c["ok"] == 0:
                continue        # both refuse, and the guest keeps its state
            got = (ok, lib.level_index(), lib.level_rating(),
                   lib.level_code_pct())
            want = (c["ok"], c["level"], c["rating"], c["pct"])
            if got != want:
                bad_in.append((c["code"], want, got))
        acc = sum(x["ok"] for x in cin)
        print(f"\n  image 0x051A2, driven {len(cin)} times")
        print(f"    the original accepted {acc}, refused {len(cin) - acc}")
        print(f"    the port agrees on {len(cin) - len(bad_in)}/{len(cin)} - "
              "the verdict, the level, the rating and the percentage")
        for code, want, got in bad_in[:a.max]:
            print(f"    {code}: original {want} port {got}")
    else:
        print(f"\n  image 0x051A2 is NOT checked - no {CASES_IN}.")
        print("    Run tools/probe_codein.py through play.py; without it the "
              "only\n    claim about taking a code BACK is the round trip "
              "above, and that\n    accepts a wrong table.")

    if bad or bad_in:
        print(f"\n  {len(bad)} of {len(cases)} generated and {len(bad_in)} "
              "validated differ\n")
        return 1
    print("\n  no differences\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
