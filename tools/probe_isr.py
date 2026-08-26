"""How much of the guest's time goes into one routine.

The level freezes: the picture never changes, the page never flips, and every
sample of CS:IP lands inside the vertical-retrace handler at image 0x17C7. The
question that settles why is not "where is it" but "how often is it entered,
and how many instructions run between entries" - because a handler entered
every few hundred instructions starves whatever it interrupts, and the main
loop is what flips the page.

  uv run python tools/probe_isr.py --play --at 0x17c7 --seconds 120
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def install(m):
    import atexit
    import json
    from unicorn import UC_HOOK_CODE

    base = m.load_seg * 16
    at = int(os.environ["LEM_ISR_AT"], 0)
    out_path = os.environ["LEM_ISR_OUT"]
    st = {"insns": 0, "entries": 0, "since": 0, "gaps": []}
    hist = {}

    def on_code(uc, address, size, _):
        st["insns"] += 1
        st["since"] += 1
        off = address - base
        hist[off] = hist.get(off, 0) + 1
        if off == at:
            st["entries"] += 1
            if len(st["gaps"]) < 400:
                st["gaps"].append(st["since"])
            st["since"] = 0

    m.uc.hook_add(UC_HOOK_CODE, on_code)
    def save():
        top = sorted(hist.items(), key=lambda kv: -kv[1])[:40]
        st["hist"] = [[hex(k), v] for k, v in top]
        with open(out_path, "w") as f:
            json.dump(st, f)
        print(f"  [hook] {st['entries']} entries, {st['insns']} instructions")

    atexit.register(save)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--at", default="0x17c7", help="the routine's image offset")
    ap.add_argument("--seconds", type=int, default=120)
    ap.add_argument("--play", action="store_true")
    a = ap.parse_args()

    cap = os.path.join(ROOT, "out", "isr.json")
    if os.path.exists(cap):
        os.unlink(cap)
    env = dict(os.environ, LEM_HOOK=os.path.abspath(__file__),
               LEM_ISR_OUT=cap, LEM_ISR_AT=a.at)
    subprocess.run([sys.executable, os.path.join(HERE, "play.py"),
                    "--auto", "--start", "--start-delay", "5",
                    "--start-tries", "3",
                    "--click-tries", "4" if a.play else "0",
                    "--", "--run-seconds", str(a.seconds)],
                   cwd=ROOT, env=env, stdout=subprocess.DEVNULL)
    if not os.path.exists(cap):
        print("\n  NOTHING WAS MEASURED. That is a failure, not a pass.")
        return 2
    import json
    with open(cap) as f:
        st = json.load(f)
    if not st["entries"]:
        print(f"\n  {a.at} was never entered in {st['insns']} instructions.")
        return 1
    gaps = st["gaps"][1:] or st["gaps"]
    gaps_sorted = sorted(gaps)
    print(f"\n  instructions executed : {st['insns']:,}")
    print(f"  entries to {a.at}   : {st['entries']:,}")
    print(f"  instructions between  : "
          f"min {min(gaps)}, median {gaps_sorted[len(gaps_sorted)//2]}, "
          f"max {max(gaps)}")
    print("\n  where the time goes (image offset, share of all instructions):")
    for off, n in st.get("hist", [])[:20]:
        print(f"    {off:>10}  {n:>12,}  {100.0*n/st['insns']:6.2f}%")
    print(f"\n  A handler entered every few hundred instructions leaves the\n"
          f"  interrupted code no room to run. That is the thing to compare\n"
          f"  against: on real hardware this fires once a frame.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
