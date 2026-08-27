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
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import dat

GAME = os.path.join(ROOT, "lemming1.pc")

# (file, section, 0-based) of every level a frame-by-frame check here compares.
#
# The skills and the nuke and pause cases all use level 1 and level 2, which
# are level009.dat sections 1 and 2 by the game's own order, and those are
# named here because nothing else says so.
#
# The REST is read out of tools/verify_all.sh rather than repeated, because a
# hand-kept copy of that list is a copy that goes stale - and stale here does
# not look like an error, it looks like a coverage gap that has already been
# filled, or one that has not. This file exists to say what nothing measures;
# it should not be the thing that is wrong about it.
FIXED = {("level009.dat", 1), ("level009.dat", 2)}


def measured_from_verify_all():
    """The `for case in "<file> <section> <ref> ..."` list in verify_all.sh."""
    path = os.path.join(HERE, "verify_all.sh")
    out = set()
    try:
        with open(path) as f:
            text = f.read()
    except OSError:
        return out, "could not read tools/verify_all.sh"
    for m in re.finditer(r'"(level\d{3}\.dat) (\d+) (ref_\w+) ', text):
        out.add((m.group(1), int(m.group(2)) - 1))
    if not out:
        return out, ("found no levels in tools/verify_all.sh - the list is "
                     "there or its shape has changed, and either way this "
                     "run is understating what is measured")
    return out, None


MEASURED, _MEAS_WARN = measured_from_verify_all()
MEASURED |= FIXED

FLAGS = [(0x80, "no_overwrite"), (0x40, "only_on_terrain")]

# THE TERRAIN FLAGS TOO, and they earned their place. Every gap this file has
# reported so far was about OBJECTS, and the second level chosen to close one
# turned up a bug in terrain instead: a piece carrying both `erase` and
# `no_overwrite`, which image 0x088F3 draws and the port was erasing. 116 of
# them, across 9 of the 80 levels, and nothing here would have said so.
#
# The COMBINATION is listed as well as the two bits, because that is where the
# bug was - each flag on its own was exercised by levels that had been
# compared for months.
TFLAGS = [(0x80, "no_overwrite"), (0x40, "upside_down"), (0x20, "erase")]


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


def terrain(sec):
    """The 400 four-byte entries at 0x120; the first word 0xFFFF ends them."""
    for i in range(400):
        e = sec[0x120 + i * 4:0x124 + i * 4]
        if len(e) < 4 or (e[0] == 0xFF and e[1] == 0xFF):
            return
        yield e[0]


def objects(sec):
    for i in range(32):
        e = sec[0x20 + i * 8:0x28 + i * 8]
        x = int.from_bytes(e[0:2], "big")
        y = int.from_bytes(e[2:4], "big")
        o = int.from_bytes(e[4:6], "big")
        if x == 0 and y == 0 and o == 0:
            continue
        yield o, e[6]


# THE EFFECT an object has, which is not in the LVL at all - it is a field of
# the GROUND file's object record, image 0x037E3's [si+0x14], and the level
# only names an id. So it needs the graphics set read alongside.
#
# It earns its place the same way the terrain flags did. The three faults this
# repository was set up to chase - a missing death scene, no drowning, no trap
# - are effects 4, 5 and 6 of image 0x02F0F's dispatch, and until they were
# reported nothing here could say which effects had ever been compared.
# The names are the port's own transcription of image 0x02F0F's dispatch, so
# each has an address behind it rather than a guess. Effects 2, 3 and 0x0A are
# a BLOCKER's three columns, written into the map at run time rather than from
# an object record, which is why they never appear in this table; 9 is steel,
# stamped by image 0x03839 from the LVL's own list and likewise not an object.
EFFECT_NAME = {0: "none", 1: "exit (0x02F3B)", 2: "blocker left (0x02FA2)",
               3: "blocker right (0x02FAC)", 4: "TRAP (0x02FB6)",
               5: "water (0x02FF8)", 6: "fire (0x0304F)",
               7: "one-way right", 8: "one-way left", 9: "steel (0x03839)"}


def ground_effects(gset):
    """trigger_effect for each of a graphics set's sixteen object ids."""
    path = os.path.join(GAME, "ground%do.dat" % (gset & 0x0F))
    try:
        with open(path, "rb") as f:
            b = f.read()
    except OSError:
        return None
    # Sixteen object records of 28 bytes at 0, the effect at +0x14 - the same
    # layout game.c's ground_load reads.
    return [b[i * 28 + 0x14] if (i * 28 + 0x14) < len(b) else 0
            for i in range(16)]


def main():
    seen_all, seen_measured = {}, {}
    eff_all, eff_measured = {}, {}
    flag_all, flag_measured = {}, {}
    tf_all, tf_measured = {}, {}
    nlev = nmeas = 0
    for name, i, sec in levels():
        nlev += 1
        meas = (name, i) in MEASURED
        nmeas += meas
        # THE LOW BYTE. graphics_set is a big-endian WORD at 0x1A - game.c
        # reads it with be16 - so sec[0x1A] is the high half and is always
        # zero. Passing that scored every level against ground0o, whose ids
        # carry no effect 6, and the table then said effect 6 does not exist
        # in the game at all. It does: ground1o has three ids with it.
        effs = ground_effects(sec[0x1B])
        for oid, mod in objects(sec):
            seen_all[oid] = seen_all.get(oid, 0) + 1
            if meas:
                seen_measured[oid] = seen_measured.get(oid, 0) + 1
            if effs and oid < 16:
                e = effs[oid]
                eff_all[e] = eff_all.get(e, 0) + 1
                if meas:
                    eff_measured[e] = eff_measured.get(e, 0) + 1
            for bit, fname in FLAGS:
                if mod & bit:
                    flag_all[fname] = flag_all.get(fname, 0) + 1
                    if meas:
                        flag_measured[fname] = flag_measured.get(fname, 0) + 1
        for b0 in terrain(sec):
            names = [n for bit, n in TFLAGS if b0 & bit]
            if len(names) > 1:
                names.append(" + ".join(names))
            for fname in names:
                tf_all[fname] = tf_all.get(fname, 0) + 1
                if meas:
                    tf_measured[fname] = tf_measured.get(fname, 0) + 1

    print(f"  {nlev} levels in the game; {nmeas} of them are compared frame "
          f"by frame here.")
    print("\n  OBJECT IDS         in the game   in a measured level")
    for oid in sorted(seen_all):
        n, m = seen_all[oid], seen_measured.get(oid, 0)
        print(f"    id {oid:<3d}          {n:6d}      {m:6d}"
              f"{'' if m else '   <-- never measured'}")
    print("\n  TRIGGER EFFECTS             in the game   in a measured level")
    for e in sorted(eff_all):
        n, m = eff_all[e], eff_measured.get(e, 0)
        print(f"    {e} {EFFECT_NAME.get(e, '?'):<21s} {n:6d}      {m:6d}"
              f"{'' if m else '   <-- never measured'}")
    print("\n  TERRAIN FLAGS               in the game   in a measured level")
    for fname in sorted(tf_all, key=lambda k: (" + " in k, k)):
        n, m = tf_all[fname], tf_measured.get(fname, 0)
        print(f"    {fname:<25s} {n:6d}      {m:6d}"
              f"{'' if m else '   <-- never measured'}")
    print("\n  OBJECT FLAGS       in the game   in a measured level")
    for _, fname in FLAGS:
        n, m = flag_all.get(fname, 0), flag_measured.get(fname, 0)
        print(f"    {fname:<16s} {n:6d}      {m:6d}"
              f"{'' if m else '   <-- never measured'}")
    unseen = [o for o in seen_all if not seen_measured.get(o)]
    unseen_f = [f for _, f in FLAGS if not flag_measured.get(f)]
    unseen_t = [f for f in tf_all if not tf_measured.get(f)]
    unseen_e = [e for e in eff_all if not eff_measured.get(e)]
    print(f"\n  {len(unseen)} of {len(seen_all)} object ids and "
          f"{len(unseen_f)} of {len(FLAGS)} object flags are never exercised "
          f"by a frame-by-frame comparison here.")
    # The terrain combinations are named rather than counted, because "2 of 7"
    # does not tell anyone which level to record next and the whole use of
    # this file is to answer that.
    if unseen_e:
        print(f"  {len(unseen_e)} of {len(eff_all)} trigger effects are never "
              f"exercised:")
        for e in sorted(unseen_e):
            print(f"    effect {e} ({EFFECT_NAME.get(e, '?')}): "
                  f"{eff_all[e]} objects")
    else:
        print(f"  and every one of the {len(eff_all)} trigger effects is "
              f"exercised.")
    if unseen_t:
        print(f"  {len(unseen_t)} of {len(tf_all)} terrain flag combinations "
              f"are never exercised:")
        for f in sorted(unseen_t):
            print(f"    {f:<34s} {tf_all[f]:6d} pieces")
    else:
        print(f"  and every one of the {len(tf_all)} terrain flag "
              f"combinations is exercised.")
    if _MEAS_WARN:
        print("\n  WARNING: %s" % _MEAS_WARN)
    return 0


if __name__ == "__main__":
    sys.exit(main())
