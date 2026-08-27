"""What the frame-by-frame checks actually EXERCISE, against the whole game.

Every level this repository compares scores "exact on every frame", and that
is true and narrower than it sounds. Five of the six real port bugs found in
one session were in code paths no reference here touches:

  the one-way arrows were drawn over the black beside their wall, because
  level_anim_draw never applied only_on_terrain. NONE of the three levels the
  loop measures has a single object carrying that flag. level004/3 has twelve.

  the release rate buttons did nothing. No recorded run presses one.

  the builder faced one way. Its own reference builds to the RIGHT.

A comparison is worth what it covers, and until now nothing here said what
that was. This does: it reads every level in the game, counts the features
each one uses, and marks which are reached by the levels the suite measures.

    uv run python tools/coverage.py

A feature nobody measures is not a bug, and this does not claim it is. It is
the list of places a bug could sit unseen - which is the thing to know before
reading "exact on every frame" as "correct".
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import dat

GAME = os.path.join(ROOT, "lemming1.pc")

# (file, section) of every level a frame-by-frame check here compares. The
# skills and the nuke and pause cases all use level 1 and level 2, which are
# level009.dat sections 1 and 2 by the game's own order.
MEASURED = {("level009.dat", 1), ("level009.dat", 2),
            ("level001.dat", 2), ("level002.dat", 3), ("level002.dat", 5)}

FLAGS = [(0x80, "no_overwrite"), (0x40, "only_on_terrain")]


def levels():
    for n in range(10):
        name = "level%03d.dat" % n
        path = os.path.join(GAME, name)
        if not os.path.exists(path):
            continue
        for i, sec in enumerate(dat.load(path)):
            if len(sec) < 0x800:
                continue
            yield name, i, sec


def objects(sec):
    for i in range(32):
        e = sec[0x20 + i * 8:0x28 + i * 8]
        x = int.from_bytes(e[0:2], "big")
        y = int.from_bytes(e[2:4], "big")
        o = int.from_bytes(e[4:6], "big")
        if x == 0 and y == 0 and o == 0:
            continue
        yield o, e[6]


def main():
    seen_all, seen_measured = {}, {}
    flag_all, flag_measured = {}, {}
    nlev = nmeas = 0
    for name, i, sec in levels():
        nlev += 1
        meas = (name, i) in MEASURED
        nmeas += meas
        for oid, mod in objects(sec):
            seen_all[oid] = seen_all.get(oid, 0) + 1
            if meas:
                seen_measured[oid] = seen_measured.get(oid, 0) + 1
            for bit, fname in FLAGS:
                if mod & bit:
                    flag_all[fname] = flag_all.get(fname, 0) + 1
                    if meas:
                        flag_measured[fname] = flag_measured.get(fname, 0) + 1

    print(f"  {nlev} levels in the game; {nmeas} of them are compared frame "
          f"by frame here.")
    print("\n  OBJECT IDS         in the game   in a measured level")
    for oid in sorted(seen_all):
        n, m = seen_all[oid], seen_measured.get(oid, 0)
        print(f"    id {oid:<3d}          {n:6d}      {m:6d}"
              f"{'' if m else '   <-- never measured'}")
    print("\n  OBJECT FLAGS       in the game   in a measured level")
    for _, fname in FLAGS:
        n, m = flag_all.get(fname, 0), flag_measured.get(fname, 0)
        print(f"    {fname:<16s} {n:6d}      {m:6d}"
              f"{'' if m else '   <-- never measured'}")
    unseen = [o for o in seen_all if not seen_measured.get(o)]
    unseen_f = [f for _, f in FLAGS if not flag_measured.get(f)]
    print(f"\n  {len(unseen)} of {len(seen_all)} object ids and "
          f"{len(unseen_f)} of {len(FLAGS)} flags are never exercised by a "
          f"frame-by-frame comparison here.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
