"""Watch the original's lemming array, frame by frame.

The array is at ds:0x88 - image 0x03634 walks it, ds:0x43 entries of 0x2D
bytes each - and it is the last subsystem the port does not have. Nothing here
transcribes anything; this records what the original's own records do so a
transcription has something to be checked against, the way tools/verify_dat.py
records a routine's inputs and outputs rather than guessing at them.

Sampled at image 0x01A3, the play loop's own frame counter, so a row is a
frame and not a moment.

  uv run python tools/probe_lemmings.py --frames 400
  uv run python tools/probe_lemmings.py --frames 400 --lemming 0   # one, in full
"""
import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

DGROUP = 0x9A90
ARRAY  = 0x88        # ds: the lemming records
REC    = 0x2D        # image 0x0367F: add di, 0x2d
COUNT  = 0x43        # ds: how many have been released
MAX    = 16

# What each offset is, as far as it has been read. Anything not named here is
# still unknown and is reported as a raw byte when it moves.
FIELDS = [
    ("x",        0x00, 2), ("y",        0x02, 2),
    ("hot_x",    0x04, 2), ("hot_y",    0x06, 2),
    ("ink",      0x0A, 2), ("mask",     0x0C, 2),
    ("frame_sz", 0x10, 2), ("mask_d",   0x12, 2),
    ("f14",      0x14, 2),
    ("f23",      0x23, 1), ("alive",    0x26, 1),
    ("dir",      0x27, 1), ("action",   0x28, 1), ("f29",     0x29, 1),
    ("f2a",      0x2A, 1),
]


def install(m):
    import atexit
    from unicorn import UC_HOOK_CODE

    base = m.load_seg * 16
    out_path = os.environ["LEM_LEM_OUT"]
    want = int(os.environ["LEM_LEM_FRAMES"])
    st = {"f": 0, "rows": []}

    def on_frame(uc, address, size, user):
        st["f"] += 1
        if st["f"] > want:
            return
        n = uc.mem_read(base + DGROUP + COUNT, 1)[0]
        n = min(n, MAX)
        recs = []
        for k in range(n):
            b = bytes(uc.mem_read(base + DGROUP + ARRAY + k * REC, REC))
            recs.append(list(b))
        st["rows"].append({"f": st["f"], "n": n, "recs": recs})

    m.uc.hook_add(UC_HOOK_CODE, on_frame, None, base + 0x01A3, base + 0x01A3)
    atexit.register(lambda: open(out_path, "w").write(json.dumps(st)))


def word(b, o):
    v = b[o] | (b[o + 1] << 8)
    return v - 0x10000 if v >= 0x8000 else v


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--frames", type=int, default=400)
    ap.add_argument("--lemming", type=int, default=-1,
                    help="print this one's whole record when it changes")
    ap.add_argument("--out", default=os.path.join(ROOT, "out", "lemmings.json"))
    a = ap.parse_args()

    cap = os.path.abspath(a.out)
    if os.path.exists(cap):
        os.unlink(cap)
    env = dict(os.environ, LEM_HOOK=os.path.abspath(__file__),
               LEM_LEM_OUT=cap, LEM_LEM_FRAMES=str(a.frames))
    subprocess.run([sys.executable, os.path.join(HERE, "play.py"),
                    "--auto", "--start", "--start-delay", "5",
                    "--start-tries", "3", "--click-tries", "4",
                    "--", "--run-seconds", str(int(60 + a.frames / 5.0))],
                   cwd=ROOT, env=env, stdout=subprocess.DEVNULL)
    if not os.path.exists(cap):
        print("\n  NOTHING WAS RECORDED. That is a failure, not a pass.")
        return 2
    with open(cap) as f:
        st = json.load(f)
    rows = [r for r in st["rows"] if r["n"]]
    if not rows:
        print("\n  no lemming was ever released in that many frames.")
        return 1
    print(f"\n  {len(st['rows'])} frames, first lemming at frame {rows[0]['f']}")

    # which offsets ever move at all
    moved = set()
    prev = {}
    for r in rows:
        for k, rec in enumerate(r["recs"]):
            if k in prev:
                for o in range(REC):
                    if rec[o] != prev[k][o]:
                        moved.add(o)
            prev[k] = rec
    named = {o for _n, o, w in FIELDS for o in range(o, o + w)}
    print(f"  offsets that ever change : "
          f"{' '.join(hex(o) for o in sorted(moved))}")
    print(f"  ...and are not named yet : "
          f"{' '.join(hex(o) for o in sorted(moved - named)) or 'none'}")

    if a.lemming >= 0:
        k = a.lemming
        print(f"\n  lemming {k}, when anything about it changes:")
        last = None
        shown = 0
        for r in rows:
            if k >= len(r["recs"]):
                continue
            rec = r["recs"][k]
            cur = tuple(rec)
            if cur == last:
                continue
            last = cur
            shown += 1
            if shown > 40:
                print("    ... (truncated)")
                break
            bits = []
            for name, o, w in FIELDS:
                v = word(rec, o) if w == 2 else rec[o]
                bits.append(f"{name}={v}")
            print(f"    f{r['f']:<5} " + " ".join(bits))
    return 0


if __name__ == "__main__":
    sys.exit(main())
