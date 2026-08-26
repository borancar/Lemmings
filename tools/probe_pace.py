"""Does the game's clock advance, and at what rate?

The level looks frozen. The main loop at image 0x0164 waits for the timer ISR
to set ds:0x1FCF, and repeats until ds:0x1FCD passes 4 - so one game frame is
five timer ticks. The ISR reloads the PIT divisor from ds:0x1FC9 on every tick,
which is where the intended rate is written down.

This samples those, and the page-flip pair the ISR feeds the CRTC, on the
guest's own instruction count rather than on the wall clock - so it says the
same thing on a fast host and a slow one.

  uv run python tools/probe_pace.py --seconds 60
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

DGROUP = 0x9A90            # image offset of DS - see docs/

# name -> (ds offset, width in bytes)
WATCH = [
    ("0x1FC9 divisor", 0x1FC9, 2),
    ("0x1FCD frames ", 0x1FCD, 1),
    ("0x1FCF flag   ", 0x1FCF, 1),
    ("0x1FD3 counter", 0x1FD3, 1),
    ("0x1F76 page A ", 0x1F76, 2),
    ("0x1F78 page B ", 0x1F78, 2),
    ("0x1F7A offset ", 0x1F7A, 2),
    ("0x1FE3 tick&3 ", 0x1FE3, 1),
]


def install(m):
    import atexit
    import json
    from unicorn import UC_HOOK_CODE

    base = m.load_seg * 16
    out_path = os.environ["LEM_PACE_OUT"]
    every = 2_000_000
    st = {"insns": 0, "samples": [], "ticks": 0}

    def on_code(uc, address, size, _):
        st["insns"] += 1
        if address - base == 0x189E:      # the ISR sets the frame flag here
            st["ticks"] += 1
        if st["insns"] % every:
            return
        row = [st["insns"], st["ticks"]]
        for _n, off, width in WATCH:
            b = uc.mem_read(base + DGROUP + off, width)
            row.append(int.from_bytes(b, "little"))
        st["samples"].append(row)

    m.uc.hook_add(UC_HOOK_CODE, on_code)
    atexit.register(lambda: open(out_path, "w").write(json.dumps(st)))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--menu", action="store_true",
                    help="stay on the title instead of entering a level")
    a = ap.parse_args()

    cap = os.path.join(ROOT, "out", "pace.json")
    if os.path.exists(cap):
        os.unlink(cap)
    env = dict(os.environ, LEM_HOOK=os.path.abspath(__file__),
               LEM_PACE_OUT=cap)
    subprocess.run([sys.executable, os.path.join(HERE, "play.py"),
                    "--auto", "--start", "--start-delay", "5",
                    "--start-tries", "3",
                    "--click-tries", "0" if a.menu else "4",
                    "--", "--run-seconds", str(a.seconds)],
                   cwd=ROOT, env=env, stdout=subprocess.DEVNULL)
    if not os.path.exists(cap):
        print("\n  NOTHING WAS MEASURED. That is a failure, not a pass.")
        return 2
    import json
    with open(cap) as f:
        st = json.load(f)
    if not st["samples"]:
        print(f"\n  only {st['insns']:,} instructions ran - too few to sample.")
        return 1

    hdr = "  " + "insns".rjust(12) + "ticks".rjust(8)
    for n, _o, _w in WATCH:
        hdr += n.rjust(16)
    print("\n" + hdr)
    for row in st["samples"]:
        line = f"  {row[0]:>12,}{row[1]:>8}"
        for v in row[2:]:
            line += f"{v:>16}"
        print(line)

    div = st["samples"][-1][2] or 0x10000
    hz = 1193182.0 / div
    print(f"\n  the game programs divisor {div} = {hz:.1f} Hz;"
          f" five ticks a frame is {hz/5:.1f} fps.")
    print(f"  it actually got {st['ticks']} ticks in {a.seconds}s "
          f"= {st['ticks']/a.seconds:.1f} Hz "
          f"= {st['ticks']/a.seconds/5:.2f} fps.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
