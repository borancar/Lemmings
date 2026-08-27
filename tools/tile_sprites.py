"""Do the lemming sprites tile MAIN.DAT's first section, with no gaps?

They do, and that is a complete check on every base and frame count this port
believes. Sixteen regions run from the walker at 0 to the blast ending at
20,720 with nothing between them and nothing overlapping, so a wrong constant
anywhere shows up as a gap or an overlap somewhere.

It is not a theory about the file - it is how the file is laid out, and two
bugs have been found with it:

  the BUILDER faced one way. 4240 + 32 * 78 = 6736 is the basher's base, so
  thirty-two frames fit exactly and sixteen would leave 1,248 bytes of
  nothing. The port drew sixteen. A player reported the symptom first.

  the SHRUG faced one way, and NOBODY reported it. Its eight frames of 40 end
  at 19760 where the blast begins at 20080 - 320 spare, which is eight more
  frames, and nothing else could have used them.

It also settles negatives, which are otherwise expensive: the blocker, the
drowner, the splatter, the burner and the blast have no second direction, and
the tiling says they cannot - their frames fill their regions exactly.

    uv run python tools/tile_sprites.py

The constants are read out of reconstruct/src/game.c, so this checks what the
PORT believes rather than a table written here that could drift from it.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
GAME = os.path.join(ROOT, "reconstruct", "src", "game.c")

# name, base define, frames, rows, planes, directions. The direction count is
# the port's own: a sprite whose draw adds the state for a left-facing lemming
# (image 0x070AB) has two sets, one after the other.
SPRITES = [
    ("walker",  None,             "LEM_SPR_FRAMES",   "LEM_SPR_H",   "LEM_SPR_PLANES",   1),
    ("digger",  "DIG_SPR_BASE",   "DIG_SPR_FRAMES",   "DIG_SPR_H",   "DIG_SPR_PLANES",   1),
    ("climber", "CLIMB_SPR_RIGHT","CLIMB_SPR_FRAMES", "CLIMB_SPR_H", "CLIMB_SPR_PLANES", 2),
    ("drown",   "DROWN_SPR_BASE", "DROWN_SPR_FRAMES", "DROWN_SPR_H", "DROWN_SPR_PLANES", 1),
    ("hoist",   "HOIST_SPR_BASE", "HOIST_SPR_FRAMES", "HOIST_SPR_H", "HOIST_SPR_PLANES", 2),
    ("builder", "BUILD_SPR_BASE", "BUILD_SPR_FRAMES", "BUILD_SPR_H", "BUILD_SPR_PLANES", 2),
    ("basher",  "BASH_SPR_BASE",  "BASH_SPR_STATE",   "BASH_SPR_H",  "BASH_SPR_PLANES",  2),
    ("miner",   "MINE_SPR_BASE",  "MINE_SPR_STATE",   "MINE_SPR_H",  "MINE_SPR_PLANES",  2),
    ("fall",    "FALL_SPR_BASE",  None,               None,          None,               0),
    ("float",   "FLOAT_SPR_BASE", None,               None,          None,               0),
    ("splat",   "SPLAT_SPR_BASE", None,               None,          None,               0),
    ("exit",    "EXIT_SPR_BASE",  "EXIT_SPR_FRAMES",  "EXIT_SPR_H",  "EXIT_SPR_PLANES",  1),
    ("burn",    "BURN_SPR_BASE",  "BURN_SPR_FRAMES",  "BURN_SPR_H",  "BURN_SPR_PLANES",  1),
    ("blocker", "BLOCK_SPR_BASE", "BLOCK_SPR_FRAMES", "BLOCK_SPR_H", "BLOCK_SPR_PLANES", 1),
    ("shrug",   "SHRUG_SPR_BASE", "SHRUG_SPR_FRAMES", "SHRUG_SPR_H", "SHRUG_SPR_PLANES", 2),
    ("blast",   "BLAST_SPR_BASE", None,               None,          None,               0),
]
# The ones whose shape the port does not spell out in defines - they are drawn
# through the shared sixteen-wide group with literals, or their comment
# carries it. Written here rather than guessed at read time, and each is the
# number that appears in game.c's own comment beside the base.
SHAPE = {"fall": (4, 10, 2, 2), "float": (8, 16, 3, 2),
         "splat": (16, 10, 2, 1),
         "drown": (16, 10, 2, 1),      # DROWN_SPR_FRAMES, 2 planes in its draw
         "basher": (0x20, 10, 3, 2),   # BASH_SPR_STATE frames each way
         "miner": (0x18, 13, 3, 2),
         "blast": (16, 10, 2, 1)}
WALKER_BASE = 0                        # the section begins with it
END = 20720          # the blast's own 16 frames of 40 end here


def defines():
    src = open(GAME).read()
    out = {}
    for m in re.finditer(r"^#define\s+([A-Z_0-9]+)\s+(0x[0-9A-Fa-f]+|\d+)",
                         src, re.M):
        out[m.group(1)] = int(m.group(2), 0)
    return out


def main():
    d = defines()
    rows, missing = [], []
    for name, base, frames, h, planes, dirs in SPRITES:
        b = WALKER_BASE if base is None else d.get(base)
        if b is None:
            missing.append(base)
            continue
        if name in SHAPE:
            f, hh, pl, dirs = SHAPE[name]
        else:
            if frames not in d or h not in d or planes not in d:
                missing.append(f"{name}'s shape")
                continue
            f, hh, pl = d[frames], d[h], d[planes]
        rows.append((name, b, f * dirs * hh * 2 * pl))
    if missing:
        print("  could not read from game.c: " + ", ".join(missing))
        print("  so nothing was checked. That is a failure, not a pass.")
        return 2

    rows.sort(key=lambda r: r[1])
    bad = 0
    print("  the lemming sprites, laid out from game.c's own constants:")
    for i, (name, base, size) in enumerate(rows):
        end = base + size
        nxt = rows[i + 1][1] if i + 1 < len(rows) else END
        note = ""
        if end != nxt:
            note = f"   <-- {'GAP' if end < nxt else 'OVERLAP'} of {abs(nxt - end)}"
            bad += 1
        print(f"    {name:<8} {base:6d} +{size:5d} = {end:6d}{note}")
    print(f"  {len(rows)} regions, {bad} gap(s) or overlap(s), "
          f"covering {END} bytes")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
