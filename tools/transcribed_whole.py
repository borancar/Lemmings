# SPDX-License-Identifier: GPL-2.0-only
"""Is each cited routine transcribed WHOLE, or only where it starts?

reconstruct/tests/provenance.py asks whether every function in the port says
where it came from. It cannot ask the other question, and the other question is
the one that let a bug through: `menu_code_screen` carried `image 0x04967` and
passed, while transcribing only the SET-UP half of that routine and none of the
loop underneath it - so typing a code did nothing, and a player found it.
`menu_caret_step` was worse: five lines carrying image 0x04C7E, which is a
150-byte entry loop with keys, a cursor and a return.

A partial transcription reads exactly like a complete one. Both carry an
address; both are counted; nothing looks at how much of the routine came
across.

So this measures DEPTH rather than presence. For every call target the code map
knows, it takes the span up to the next one and counts how many distinct
addresses inside that span the port cites. One citation for a long routine is
the shape that hides a missing half.

    uv run python tools/transcribed_whole.py            # the thin ones
    uv run python tools/transcribed_whole.py --all      # every cited routine

WHAT IT CANNOT TELL YOU. The span to the next call target is an upper bound on
a routine, not its extent: a routine reached only by `jmp` is invisible to the
map, so its bytes are charged to whoever precedes it. And a short routine
honestly needs one address. Read the list; do not score it.
"""
import argparse
import json
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
MAP = os.path.join(ROOT, "image", "codemap.json")
SRC = os.path.join(ROOT, "reconstruct", "src")
# adlib.c is left out on purpose: its addresses are offsets into adlib.dat's
# decompressed section, not image offsets, and mixing the two spaces would
# credit a routine here with a citation from there.
#
# NAMED AS AN EXCLUSION, not as an inclusion list. This was six filenames
# spelled out, which meant a source file added to the port would go unmeasured
# and the total would simply be smaller - silently, the way
# tools/verify_triggers.py silently died when LEVEL_ORDER moved out of the
# file it named. reconstruct/tests/provenance.py globs, and so does this now;
# what the report prints is which files it actually read.
#
# Globbing also picks up adlib.h and opl.h, which the hand-written list left
# out. CHECKED rather than assumed, because adlib.h is exactly where an
# adlib.dat offset would hide: its citations are image 0x01872 and image
# 0x017FE, both about INT 8 and the retrace, so they are offsets in the main
# image and belong here. The cited count is 59 either way.
SKIP = ("adlib.c",)


def functions(src_dir):
    """Every function in the port, its line count, and the addresses its own
    comment block cites.

    The block is the contiguous comment DIRECTLY above the definition, the
    same rule reconstruct/tests/provenance.py uses - a window would inherit
    the address of whatever came before.
    """
    defn = re.compile(r"^(?:static\s+)?[A-Za-z_][A-Za-z0-9_]*\s+\**"
                      r"([a-z_][a-z0-9_]*)\s*\(")
    out = []
    for f in sorted(os.path.basename(q) for q in
                    glob.glob(os.path.join(src_dir, "*.c"))
                    + glob.glob(os.path.join(src_dir, "*.h"))
                    if os.path.basename(q) not in SKIP):
        path = os.path.join(src_dir, f)
        if not os.path.exists(path) or not f.endswith(".c"):
            continue
        lines = open(path).read().split("\n")
        for i, ln in enumerate(lines):
            m = defn.match(ln)
            if not m or ln.rstrip().endswith(";"):
                continue
            # the body: to the closing brace in column 0
            end = i
            for j in range(i + 1, min(i + 900, len(lines))):
                if lines[j].startswith("}"):
                    end = j
                    break
            if end == i:
                continue
            # the comment block above
            block, j = [], i - 1
            while j >= 0 and lines[j].strip() == "":
                j -= 1
            while j >= 0:
                t = lines[j].strip()
                if t.endswith("*/") or t.startswith("*") or t.startswith("/*"):
                    block.append(lines[j])
                    if t.startswith("/*"):
                        break
                    j -= 1
                else:
                    break
            text = "\n".join(block)
            addrs = {int(x, 16) for x in
                     re.findall(r"image\s+0x0*([0-9A-Fa-f]{3,5})\b", text)}
            out.append({"file": f, "name": m.group(1), "line": i + 1,
                        "lines": end - i + 1, "addrs": addrs})
    return out


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--all", action="store_true",
                    help="every cited routine, not just the thin ones")
    ap.add_argument("--min-span", type=int, default=48,
                    help="ignore routines shorter than this many bytes")
    ap.add_argument("--ratio", type=float, default=12.0,
                    help="bytes of original per line of C to flag at")
    a = ap.parse_args()

    if not os.path.exists(MAP):
        sys.exit(f"{MAP} is missing - run tools/codemap.py first")
    cm = json.load(open(MAP))
    starts = sorted(int(r["at"], 16) for r in cm["routines"])
    executed = {int(r["at"], 16): r["executed"] for r in cm["routines"]}
    end_of_code = cm["code_region"][1]

    span = {}
    for i, at in enumerate(starts):
        span[at] = (starts[i + 1] if i + 1 < len(starts) else end_of_code) - at

    fns = functions(SRC)
    best, total = {}, {}
    for fn in fns:
        for at in fn["addrs"]:
            if at not in span:
                continue                 # cites inside a routine, not its head
            if at not in best or fn["lines"] > best[at][1]["lines"]:
                best[at] = (span[at], fn)
    # And the TOTAL, over every function citing anywhere inside the routine -
    # because a routine may honestly be transcribed across several. The two
    # numbers together are what discriminates: a big routine whose largest
    # claimant is tiny AND whose total is tiny is the one that stopped early.
    for at in best:
        end = at + span[at]
        total[at] = sum(f["lines"] for f in fns
                        if any(at <= x < end for x in f["addrs"]))

    # SAY WHICH FILES, so the number has a scope attached to it. A count of
    # cited routines is meaningless without knowing what was read, and this
    # tool named its six sources by hand until a seventh could have appeared
    # and gone unmeasured.
    read = sorted(os.path.basename(q) for q in
                  glob.glob(os.path.join(SRC, "*.c"))
                  + glob.glob(os.path.join(SRC, "*.h"))
                  if os.path.basename(q) not in SKIP)
    print("\n  read: " + ", ".join(read)
          + "  (skipped " + ", ".join(SKIP) + " - adlib.dat's own offsets)")
    print(f"  routines whose HEAD the port cites : {len(best)}")
    thin = [(at, sp, fn) for at, (sp, fn) in best.items()
            if sp >= a.min_span
            and sp / max(fn["lines"], 1) >= a.ratio
            and sp / max(total[at], 1) >= a.ratio / 3.0]
    print(f"  claimed by a function too small   : {len(thin)}   <- read these")
    print(f"\n  \"too small\" is {a.ratio} bytes of original per line of C or")
    print("  worse. It is a smell, not a verdict: a table lookup is three")
    print("  lines for eighty bytes and perfectly complete.\n")

    show = ([(at, sp, fn) for at, (sp, fn) in best.items()]
            if a.all else thin)
    show.sort(key=lambda r: -(r[1] / max(r[2]["lines"], 1)))
    for at, sp, fn in show:
        run = "ran" if executed.get(at, False) else "never ran"
        print(f"    image 0x{at:05X}  up to {sp:5d} bytes  ->  "
              f"{fn['file']}:{fn['line']:<5d} {fn['name']:<24s} "
              f"largest {fn['lines']:3d} lines, all {total[at]:3d}  "
              f"{sp / max(fn['lines'], 1):6.1f} b/l  ({run})")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
