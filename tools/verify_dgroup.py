"""THE PORT'S DATA SEGMENT AGAINST THE ORIGINAL'S OWN, field by field.

Every other check here compares a PICTURE. This compares the bytes the game
decides with, at the game's own end-of-frame, which is the only way to see
state that never reaches a pixel: a latch, a divider, a dirty flag, a phase
byte. Two bugs a player had to find would have shown up here on frame one -
ds:0x200E losing its setter (the level came up black) and two `static g_tick`
definitions merging into one byte.

IT COMPARES FIELDS, NEVER sizeof(dgroup). The struct is 19,965 bytes of which
51 are known; the port never writes the padding, so a memcmp would report
nineteen thousand differences and not one of them would mean anything. The
field list is parsed out of dgroup.h's own ENSURE_DG_AT assertions, so a field added
there is compared here with no change to this file.

    uv run python tools/verify_dgroup.py --level 8 --frames 40

`--level` is the GAME's index, ds:0x1F6B; `--rating`/`--number` put the PORT on
the same level, and the two are a permutation apart - see CLAUDE.md's table.
Level 8 is Fun 9, which is `--rating fun --number 9`.

Both sides capture at image 0x018F. The port's half is LEM_DG_FILE, written by
harness.c beside the frame sink; the guest's is tools/dg_hook.py.
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

DG_H = os.path.join(ROOT, "reconstruct", "src", "dgroup.h")
DG_END = 0x4DFD


def fields():
    """The 51 offsets, out of dgroup.h's own assertions, with their sizes.

    Parsed rather than repeated: a second list of offsets in this file would be
    a second declaration of the layout, and this project has been bitten twice
    by exactly that - the ctypes Lem and the two static g_ticks.
    """
    src = open(DG_H).read()
    off = {n: int(v, 16)
           for n, v in re.findall(r'ENSURE_DG_AT\((\w+),\s*(0x[0-9A-Fa-f]+)\)', src)}
    size = {}
    # the closing brace, however the struct is attributed - this read
    # `src.index("} dgroup;")` and broke the moment the packing moved from a
    # pragma to __attribute__((packed)). A parser that assumes the exact
    # spelling of what it parses is a second declaration of it.
    end = re.search(r'^\}[^;\n]*\bdgroup\s*;', src, re.M)
    if not end:
        sys.exit("dgroup.h: cannot find the end of the struct")
    body = src[src.index("typedef struct {"):end.start()]
    for typ, name, arr in re.findall(
            r'(u?int(?:8|16|32)_t|char)\s+(\w+)(\[[^\]]*\])?\s*;', body):
        if name not in off:
            continue
        w = {"uint8_t": 1, "int8_t": 1, "char": 1,
             "uint16_t": 2, "int16_t": 2, "uint32_t": 4, "int32_t": 4}[typ]
        n = 1
        if arr:
            n = int(eval(arr[1:-1], {"__builtins__": {}}, {}))
        size[name] = w * n
    missing = sorted(set(off) - set(size))
    if missing:
        sys.exit("dgroup.h asserts %s but the struct has no such field"
                 % ", ".join(missing))
    return sorted((off[n], size[n], n) for n in off)


# WHAT THIS RUN CANNOT PUT THE TWO SIDES IN AGREEMENT ABOUT, and why.
#
# The guest is driven from the title through the menu and the briefing into a
# level; the port is handed the level directly. So anything the MENU left
# behind differs, and so does the pointer, because the guest's auto-driver
# clicked its way in and the port's never moved.
#
# These are STATE, not behaviour. They are reported on their own line rather
# than counted as disagreements, and never hidden - verify_menu.py scored 728
# pixels for exactly this shape of thing (two sides in different states, one
# percentage) until the state was passed in. When one of these can be put in
# agreement it should be, and the entry deleted.
EXPECTED = {
    "rating_drawn": "the MENU's plate guard - the guest ran the menu, the "
                    "port was handed the level",
    "fx_drawn":     "the same, image 0x0441B",
    "anim_ctr":     "the menu's animation counters, image 0x0E3F0",
    "tune":         "no sound driver in this harness, so music_choose returns "
                    "at image 0x04834 without picking one",
    # These three are only expected to differ WITHOUT --script. Driven from
    # one recording the two pointers are in the same place, and if they still
    # differ that is the finding rather than the set-up.
    "mouse_x":      "no --script: the guest's auto-driver clicked its way in "
                    "and the port's pointer never moved from image 0x00F41's "
                    "(150, 100)",
    "mouse_y":      "as mouse_x",
    "buttons":      "as mouse_x - ds:0x14 while the driver was clicking",
    "level_phase":  "the FADE. Image 0x00325 and image 0x00346 branch on "
                    "ds:0x200E, and a capture harness answers 0 to that on "
                    "purpose - a fade inside frame 0 would spend a second of "
                    "wall clock producing nothing",
}


def run(cmd, env=None, why=""):
    e = dict(os.environ)
    if env:
        e.update(env)
    r = subprocess.run(cmd, cwd=ROOT, env=e,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = r.stdout.decode("utf-8", "replace")
    for line in out.splitlines():
        if "[dg]" in line or "Traceback" in line or "Error" in line:
            print("   " + line)
    if r.returncode != 0:
        sys.exit("%s failed (rc=%d)\n%s" % (why, r.returncode, out[-2000:]))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--level", type=int, default=8,
                    help="the GAME's level index, ds:0x1F6B - a permutation "
                         "away from a file and section, see CLAUDE.md")
    ap.add_argument("--frames", type=int, default=40)
    ap.add_argument("--rating", default="fun")
    ap.add_argument("--script", default=None,
                    help="an input recording - lemdev --record-input - fed to "
                         "BOTH sides, so the comparison covers the part of the "
                         "game that needs a player")
    ap.add_argument("--seconds", type=int, default=240,
                    help="how long the guest may run to reach the level")
    ap.add_argument("--number", type=int, default=1)
    a = ap.parse_args()

    out = os.path.join(ROOT, "out")
    os.makedirs(out, exist_ok=True)
    os.makedirs(os.path.join(out, "dg_shots"), exist_ok=True)
    # NAMED AFTER THE INPUT, because a fixed output path is a shared global -
    # this project has already taken three measurements of one level's port
    # against another level's original that way.
    orig = os.path.join(out, "dg_orig_%d.bin" % a.level)
    port = os.path.join(out, "dg_port_%d.bin" % a.level)
    for p in (orig, port):
        if os.path.exists(p):
            os.remove(p)          # a stale capture reads as a successful run

    print("== the original's data segment ==")
    # record_level.py is the only driver that gets through the title, the
    # briefing and INTO a level - see CLAUDE.md. LEM_EXTRA_HOOK is its way in.
    run([sys.executable, os.path.join(HERE, "record_level.py"),
         "--level", str(a.level), "--frames", str(a.frames),
         "--seconds", str(a.seconds),
         "--out", os.path.join(out, "dg_shots")],
        dict({"LEM_EXTRA_HOOK": os.path.join(HERE, "dg_hook.py"),
              "LEM_DG_OUT": orig, "LEM_DG_FRAMES": str(a.frames)},
             **({"LEM_REC_SCRIPT": os.path.abspath(a.script)}
                if a.script else {})),
        "driving the original")

    print("== the port's ==")
    run([os.path.join(ROOT, "reconstruct", "lemdev"),
         "--data", "lemming1.pc", "--rating", a.rating,
         "--number", str(a.number), "--raw", "/dev/null", "--show"],
        dict({"SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy",
              "LEM_DG_FILE": port, "LEM_FRAMES": str(a.frames)},
             **({"LEM_INPUT_SCRIPT": os.path.abspath(a.script)}
                if a.script else {})),
        "rendering with the port")

    if not os.path.exists(orig) or os.path.getsize(orig) == 0:
        sys.exit("the original captured nothing - see the [dg] line above")
    o = open(orig, "rb").read()
    p = open(port, "rb").read()
    n = min(len(o) // DG_END, len(p) // DG_END)
    if n == 0:
        sys.exit("no frame was captured on both sides - 0 of 0 is not a result")

    print("== comparing %d frames, field by field ==" % n)
    if n < a.frames:
        print("   NOTE: %d frames, not the %d asked for - the ORIGINAL "
              "captured %d. A short run is not a failed one, but say so."
              % (n, a.frames, len(o) // DG_END))
    bad = {}
    for f in range(n):
        O = o[f * DG_END:(f + 1) * DG_END]
        P = p[f * DG_END:(f + 1) * DG_END]
        for off, size, name in fields():
            if O[off:off + size] != P[off:off + size]:
                bad.setdefault(name, []).append(
                    (f, O[off:off + size], P[off:off + size]))

    tot = len(fields())
    at = {n_: o_ for o_, s_, n_ in fields()}
    pointer = {"mouse_x", "mouse_y", "buttons"}
    exp = dict(EXPECTED)
    if a.script:
        # Both sides read the same recording, so a pointer that still differs
        # is not the set-up any more.
        for k in pointer:
            exp.pop(k, None)
    known = {k: v for k, v in bad.items() if k in exp}
    real = {k: v for k, v in bad.items() if k not in exp}

    def show(name, rows):
        f, ov, pv = rows[0]
        return ("   ds:0x%04X %-14s %d/%d frames; first at %d: "
                "original %s port %s"
                % (at[name], name, len(rows), n, f, ov.hex(), pv.hex()))

    print("   %d of %d fields agree on every frame" % (tot - len(bad), tot))

    if known:
        print("\n   %d differ for a reason this run cannot remove:" % len(known))
        for name, rows in sorted(known.items(), key=lambda kv: -len(kv[1])):
            print(show(name, rows))
            print("       %s" % exp[name])

    if real:
        print("\n   %d DIFFER AND ARE NOT EXPLAINED:" % len(real))
        for name, rows in sorted(real.items(), key=lambda kv: -len(kv[1])):
            print(show(name, rows))
    else:
        print("\n   nothing differs that is not accounted for.")
    return 1 if real else 0


if __name__ == "__main__":
    sys.exit(main())
