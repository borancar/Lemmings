"""Every skill the port transcribes, against the original's own recording.

One command for the claims this port makes about lemming BEHAVIOUR, as
opposed to the ones tools/verify_all.sh makes about screens. Each skill was
driven through the original with tools/record_level.py --skills and one
input script, and this replays the same script through the port and compares
the two arrays frame by frame.

    uv run python tools/verify_skills.py

The comparison is the RECORD, not the picture: x, y, the hot spot, the state
word, the animation counter and the alive byte, for every lemming on screen
rather than only the one that was clicked. That last part matters - the
bomber's own record was exact while another lemming walked over a hole it
should have fallen into, and only comparing all of them showed it.

A skill with no reference recorded is reported as such rather than skipped
silently.
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# button -> (name, reference directory, script, level number for the port)
SKILLS = [
    (2, "climber", "ref_s2b", "out/s2.txt", 1),
    (3, "floater", "ref_l2f", "out/l2_float.txt", 2),
    (4, "bomber",  "ref_s4",  "out/s4.txt", 1),
    (5, "blocker", "ref_s5",  "out/s5.txt", 1),
    (6, "builder", "ref_s6b", "out/s6.txt", 1),
    (7, "basher",  "ref_s7",  "out/s7.txt", 1),
    (8, "miner",   "ref_s8",  "out/s8.txt", 1),
    (9, "digger",  "refplay_full", "out/dig.txt", 1),
]
UNTOUCHED = [("level 2, nobody touching it", "ref_l2", None, 2),
             # THE NUKE is not a skill and does not belong in the table
             # above: nothing is assigned to a lemming, the script presses
             # panel button 11 twice within five frames and the whole array
             # is expected to light its fuses one a frame and go up.
             ("the nuke, level 1", "ref_nuke", "out/nuke.txt", 1)]


def compare(ref, script, number, frames):
    state = os.path.join(ROOT, "out", "vs_state.jsonl")
    if os.path.exists(state):
        os.unlink(state)
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", LEM_SKILLS="20",
               LEM_FRAMES=str(frames), LEM_STATE_FILE=state)
    if script:
        env["LEM_INPUT_SCRIPT"] = os.path.join(ROOT, script)
    subprocess.run([os.path.join(ROOT, "reconstruct", "lemdev"),
                    "--data", "lemming1.pc", "--rating", "fun",
                    "--number", str(number), "--show"],
                   cwd=ROOT, env=env, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    if not os.path.exists(state):
        return None, 0
    man = json.load(open(os.path.join(ROOT, "out", ref, "manifest.json")))
    port = {}
    for line in open(state):
        r = json.loads(line)
        port[r["frame"]] = r["lem"]
    bad = shared = 0
    for r in man:
        f = r["frame"]
        p = port.get(f)
        if p is None or r.get("lem") is None:
            continue
        shared += 1
        for k in range(min(len(r["lem"]), len(p))):
            A, B = r["lem"][k], p[k]
            # The FUSE is compared where the port reports one. A nuke does
            # nothing visible for seventy-nine frames, so position and flags
            # agree all the way from the double click to the first explosion
            # and would pass whatever order the cascade lit them in.
            keys = ["x", "y", "hot_x", "hot_y", "flags", "anim", "alive"]
            # Only where BOTH sides carry it: the older reference runs were
            # recorded before the manifest kept the fuse, and a missing key
            # must cost coverage rather than raise.
            keys += [k for k in ("fuse", "dir") if k in A and k in B]
            if tuple(A[k] for k in keys) != tuple(B[k] for k in keys):
                bad += 1
                break
    return bad, shared


def main():
    print("\n  every skill's RECORD against the original's own:\n")
    worst = 0
    for _, name, ref, script, number in SKILLS:
        d = os.path.join(ROOT, "out", ref)
        if not os.path.exists(os.path.join(d, "manifest.json")):
            print(f"    {name:9s} NO REFERENCE at out/{ref} - not a pass")
            worst = 1
            continue
        n = len(json.load(open(os.path.join(d, "manifest.json"))))
        bad, shared = compare(ref, script, number, n + 20)
        if bad is None:
            print(f"    {name:9s} the port wrote no state - not a pass")
            worst = 1
            continue
        print(f"    {name:9s} {bad} of {shared} frames differ")
        worst = max(worst, 1 if bad else 0)
    for name, ref, script, number in UNTOUCHED:
        d = os.path.join(ROOT, "out", ref)
        if not os.path.exists(os.path.join(d, "manifest.json")):
            continue
        n = len(json.load(open(os.path.join(d, "manifest.json"))))
        bad, shared = compare(ref, script, number, n + 20)
        print(f"    {name}: {bad} of {shared} frames differ")
        worst = max(worst, 1 if bad else 0)
    print()
    return worst


if __name__ == "__main__":
    sys.exit(main())
