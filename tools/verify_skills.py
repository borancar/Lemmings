"""Every skill the port transcribes, against the original's own recording.

One command for the claims this port makes about lemming BEHAVIOUR, as
opposed to the ones tools/verify_all.sh makes about screens. Each skill was
driven through the original with tools/record_level.py --skills and one
input script, and this replays the same script through the port and compares
the two arrays frame by frame.

    uv run python tools/verify_skills.py

TWO comparisons, and both are needed.

The RECORD - x, y, the hot spot, the state word, the animation counter, the
alive byte and the fuse, for every lemming on screen rather than only the one
that was clicked. That last part matters: the bomber's own record was exact
while another lemming walked over a hole it should have fallen into, and only
comparing all of them showed it.

And the SCREEN, which this tool did not do for a long time and should have.
The record scores where a lemming IS and never what is drawn there, so three
faults lived behind a row of "0 of 400 frames differ": the basher and the
miner had no sprite at all and were drawn walking on the spot, and a nuked
lemming was drawn with the blast's pose at the cloud's hot spot and appeared
to leap into the air. Every one of them was reported by a player, because
nothing here was looking.

A skill with no reference recorded is reported as such rather than skipped
silently.
"""
import json
import os
import shutil
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
             ("the nuke, level 1", "ref_nuke", "out/nuke.txt", 1),
             # PAUSE is a flag the frame reads in eight places rather than a
             # button that does something, so what this scores is that
             # NOTHING moved between frames 200 and 300 and that everything
             # picked up where it left off.
             ("pause, level 1", "ref_pause", "out/pause.txt", 1)]


def screens(ref, shots):
    """The port's own screen against the original's, via compare_screen.py."""
    out = subprocess.run(
        [sys.executable, os.path.join(HERE, "compare_screen.py"),
         "--ref", os.path.join(ROOT, "out", ref), "--dir", shots],
        cwd=ROOT, capture_output=True, text=True)
    for line in out.stdout.splitlines():
        if "play area" in line:
            return line.split("play area")[1].strip()
    return None


def park_script(ref):
    """Aim the port's pointer at the ORIGINAL's own, frame by frame.

    A run with no input script leaves each side's cross-hair wherever it
    happened to be, and two cross-hairs in different places is 56 differing
    pixels on every frame - compare_screen.py excuses "exactly 28", which is
    ONE cursor, so the score reads as the port being wrong when the play area
    is already exact. It hid a matched level here for a whole pass.

    This USED to write a single move at `mx + 10 - 8`, with the ten explained
    as the emulator's pointer sitting ten short of the guest's own on a run
    recorded without a script. tools/park_script.py writes the whole TRACK at
    a plain -8 instead, and on "level 2, nobody touching it" that is 481 of
    482 frames exact against 443 - so the ten was standing in for the pointer
    MOVING, which a single move cannot follow, rather than for an offset. Two
    harnesses had two different answers for where the same pointer is; this is
    the one that measures.

    The single frame still differing is the frame the pointer moves on, and
    the two recordings disagree about that frame: ref_l2's screen still shows
    the old position on the frame its manifest reports the new one, and
    ref_i8's does not. One frame of 482, and it belongs to when the recording
    samples the pointer rather than to the port.
    """
    sys.path.insert(0, HERE)
    import park_script as ps
    path = os.path.join(ROOT, "out", "vs_park.txt")
    try:
        ps.write(os.path.join(ROOT, "out", ref), path)
    except (SystemExit, OSError, ValueError):
        return None
    return path


def compare(ref, script, number, frames, skills=20):
    """Run the port over one case and score it against the reference.

    `skills` is the poke record_level.py's --skills makes in the GUEST, and
    both sides have to get the same one. The eight skill references are
    recorded with --skills 20 (tools/record_skills.sh); the nuke and the pause
    are not, and handing the port 20 of everything against a reference showing
    the level's own counts scored their PANEL 0 of 557 frames - a harness
    setting reported as a reconstruction failure."""
    state = os.path.join(ROOT, "out", "vs_state.jsonl")
    if os.path.exists(state):
        os.unlink(state)
    shots = os.path.join(ROOT, "out", "vs_screen")
    shutil.rmtree(shots, ignore_errors=True)
    os.makedirs(shots, exist_ok=True)
    env = dict(os.environ, SDL_VIDEODRIVER="dummy",
               LEM_FRAMES=str(frames), LEM_STATE_FILE=state,
               LEM_SCREEN_DIR=shots)
    if skills:
        env["LEM_SKILLS"] = str(skills)
    if script:
        env["LEM_INPUT_SCRIPT"] = os.path.join(ROOT, script)
    else:
        p = park_script(ref)
        if p:
            env["LEM_INPUT_SCRIPT"] = p
    subprocess.run([os.path.join(ROOT, "reconstruct", "lemdev"),
                    "--data", "lemming1.pc", "--rating", "fun",
                    "--number", str(number), "--show"],
                   cwd=ROOT, env=env, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    if not os.path.exists(state):
        return None, 0, None
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
    return bad, shared, screens(ref, shots)


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
        bad, shared, pic = compare(ref, script, number, n + 20)
        if bad is None:
            print(f"    {name:9s} the port wrote no state - not a pass")
            worst = 1
            continue
        print(f"    {name:9s} record {bad} of {shared} frames differ")
        print(f"    {'':9s} screen {pic or 'NOT SCORED'}")
        worst = max(worst, 1 if bad else 0)
    for name, ref, script, number in UNTOUCHED:
        d = os.path.join(ROOT, "out", ref)
        if not os.path.exists(os.path.join(d, "manifest.json")):
            continue
        n = len(json.load(open(os.path.join(d, "manifest.json"))))
        bad, shared, pic = compare(ref, script, number, n + 20, skills=0)
        print(f"    {name}:")
        print(f"      record {bad} of {shared} frames differ")
        print(f"      screen {pic or 'NOT SCORED'}")
        worst = max(worst, 1 if bad else 0)
    print()
    return worst


if __name__ == "__main__":
    sys.exit(main())
