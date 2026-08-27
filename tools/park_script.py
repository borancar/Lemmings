"""Aim the port's pointer at the original's own, frame by frame.

Two cross-hairs in different places is 56 differing pixels on every frame, and
`compare_screen.py` excuses only the 28 of a single one - so a harness that
parks the port's pointer at a constant is buying itself a difference on every
frame where the guest's pointer moved.

out/park.txt was such a constant: one `move` measured off the two cross-hairs.
It is right for level002's two sections, whose guest settles on frame 1, and
wrong for level001 section 3, whose guest does not move its pointer until
frame **42** - 42 frames of exactly 56 differing pixels, every one of them the
cursor and none of them the reconstruction.

This writes the whole track instead: one `move` per change, read out of the
reference's own manifest. The offset is (-8, -8), which is where the port's
cross-hair sits relative to the position the guest reports - measured, and the
same eight in both axes.

Note `tools/verify_skills.py` had a DIFFERENT offset for the same job, mx + 2
rather than mx - 8. Two harnesses disagreeing by ten pixels about where the
same pointer is, is exactly the sort of thing that gets excused as "the
cursor" and never looked at.

    uv run python tools/park_script.py out/ref_i8 out/park_i8.txt
"""
import json
import os
import sys

DX, DY = -8, -8


def write(ref, out):
    man = json.load(open(os.path.join(ref, "manifest.json")))
    lines = [f"# the original's own pointer track, from {ref}/manifest.json",
             f"# offset ({DX}, {DY}) - where the port's cross-hair sits "
             f"relative to the position the guest reports"]
    prev = None
    for e in man:
        p = (e.get("mx"), e.get("my"))
        if p == prev or p[0] is None:
            continue
        lines.append(f"{e['frame']} move {p[0] + DX} {p[1] + DY}")
        prev = p
    if prev is None:
        raise SystemExit(f"{ref}: no pointer in the manifest - a script that "
                         f"aims at nothing is not a script")
    with open(out, "w") as f:
        f.write("\n".join(lines) + "\n")
    return len(lines) - 2


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    n = write(sys.argv[1], sys.argv[2])
    print(f"{sys.argv[2]}: {n} move(s)")
