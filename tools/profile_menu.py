# SPDX-License-Identifier: GPL-2.0-only
"""Which routines run while the MAIN MENU is on screen.

tools/hotspots.py profiles a start-up run in-process, which never reaches the
menu. This rides along with tools/play.py through LEM_HOOK instead, counts
instructions only while the guest is in mode 10h, and attributes them to the
nearest call target from the code map.

  uv run python tools/profile_menu.py

The menu spends nearly all its time in one planar blitter, so the interesting
part of the report is what is left after that: the loop that polls the mouse
and the keyboard, which is what a port has to reproduce to make the plates do
anything.
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "out", "menu_profile.json")


def install(m):
    """Runs inside the guest, via LEM_HOOK."""
    import atexit
    from unicorn import UC_HOOK_CODE

    base = m.load_seg * 16
    top = base + 0x9A90
    count = {}

    def ran(uc, ad, size, _):
        if getattr(m, "mode", 0) == 0x10 and base <= ad < top:
            off = ad - base
            count[off] = count.get(off, 0) + 1

    m.uc.hook_add(UC_HOOK_CODE, ran)

    def dump():
        with open(os.environ["LEM_PROFILE_OUT"], "w") as f:
            json.dump({hex(k): v for k, v in count.items()}, f)
        print(f"  [hook] {len(count)} distinct addresses ran in mode 10h")

    atexit.register(dump)


def main():
    if os.path.exists(OUT):
        os.unlink(OUT)
    env = dict(os.environ, LEM_HOOK=os.path.abspath(__file__),
               LEM_PROFILE_OUT=OUT)
    cmd = [sys.executable, os.path.join(HERE, "play.py"), "--auto", "--",
           "--run-seconds", "60"]
    print("running the original to its menu and profiling mode 10h ...")
    subprocess.run(cmd, cwd=ROOT, env=env, stdout=subprocess.DEVNULL)
    if not os.path.exists(OUT):
        raise SystemExit("no profile written - the run never reached mode 10h")

    with open(OUT) as f:
        count = {int(k, 16): v for k, v in json.load(f).items()}
    cm = json.load(open(os.path.join(ROOT, "image", "codemap.json")))
    targets = sorted(int(r["at"], 16) for r in cm["routines"])

    import bisect
    weight = {}
    for off, n in count.items():
        i = bisect.bisect_right(targets, off) - 1
        if i >= 0:
            weight[targets[i]] = weight.get(targets[i], 0) + n
    total = sum(count.values())
    print(f"\n  instructions in mode 10h : {total}")
    print(f"\n  {'routine':>9}  {'executions':>12}  {'share':>7}")
    for at, n in sorted(weight.items(), key=lambda kv: -kv[1])[:20]:
        i = targets.index(at)
        end = targets[i + 1] if i + 1 < len(targets) else 0x9A90
        print(f"  {at:#09x}  {n:12d}  {100.0*n/total:6.2f}%   "
              f"{at:#07x}..{end:#07x}")


if __name__ == "__main__":
    main()
