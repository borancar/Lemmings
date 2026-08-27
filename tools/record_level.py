"""Record the original's level, one file per frame, on the game's own cue.

Frame-by-frame comparison needs a reference sequence, and the only honest
place to take it is the moment the game itself calls a frame finished. That
is image 0x018F: the routine at 0x0105 swaps the two pages, waits the five
timer ticks a frame costs, and then resets the tick counter and bumps its own
frame counter at ds:0x1FD3. By the time 0x018F runs, the page now being
displayed holds a complete frame.

A wall-clock capture cannot do this. The emulator's speed varies with what the
guest is doing, so a frame sampled every N milliseconds lands in the middle of
a redraw as often as not - and a torn frame does not look like a mistake in a
report, it looks like a difference.

Each frame is written in the same SCRN form the control socket's `screen`
command uses, so tools/compare_frame.py and tools/diff_png.py read them
already: b"SCRN", width and height as little-endian u16, 768 palette bytes,
then one index per pixel.

  uv run python tools/record_level.py --frames 300
  uv run python tools/record_level.py --frames 4200 --out out/ref   # 5 minutes

  # the same input the port gets, at the same frames
  uv run python tools/record_level.py --frames 3000 --script out/dig.txt

The level is the SECOND time the guest enters mode 0x0D - the first is the
start-up screen, which is why a driver that waits for "mode 0x0D" alone
captures the loading message and reports it as a level.
"""
import argparse
import json
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

DGROUP = 0x9A90            # image offset of DS
FRAME_DONE = 0x018F        # the game's own end-of-frame
FRAME_COUNTER = 0x1FD3     # ds: the counter it bumps there
SCROLL = 0x00              # ds: the view's left edge, in level pixels
MOUSE_X, MOUSE_Y = 0x10, 0x12   # ds: the pointer, image 0x0427E and 0x04293
CUR_OFF, CUR_BASE, CUR_BIAS = 0x0C, 0x1F78, 0x1FE0   # image 0x071B1
ARRAY, COUNT, REC = 0x88, 0x43, 0x2D   # the lemming array
DIGGERS = 0x7F             # ds: how many diggers are left to give out
# THE EIGHT SKILL COUNTS, ds:0x6A and every THREE bytes - each handler opens
# by testing its own: image 0x0329B reads ds:0x6A, image 0x03557 ds:0x6D,
# then 0x70, 0x73, 0x76, 0x79, 0x7C, and image 0x0320D ds:0x7F.
#
# "Just dig!" stocks nothing but diggers, so seven of the eight can never be
# assigned by playing it - and a routine no run reaches is a routine the port
# can have wrong for ever. LEM_REC_SKILLS stocks them all, which is a POKE
# and says so: it changes what the level offers, not how any skill behaves,
# and both sides get the same poke so the comparison stays a comparison.
SKILLS = [0x6A, 0x6D, 0x70, 0x73, 0x76, 0x79, 0x7C, 0x7F]
# WHICH LEVEL. ds:0x1F6B is the index across the whole game - image 0x0472E
# advances it when one is won and image 0x04566 turns it into the briefing's
# number. play.py reaches Fun 1 and nothing else, so a run that wants another
# level pokes this before the level loads.
#
# The second is worth having for its own sake: lemmings FALL there, and far
# enough to die. Nothing on "Just dig!" falls far enough for image 0x021BA to
# even ask whether a lemming is a floater, so the floater's behaviour and the
# splat cannot be reached on the first level at all.
LEVEL_INDEX = 0x1F6B
# WHERE THE LEVEL RECORD IS LOADED. Image 0x036B9 sets di to this fixed
# address and reads the release rate, the counts and all eight skills from it,
# so it is the record the game believes it is playing. Carried in the manifest
# because the port and the original have been seen to disagree about a
# level's skills while agreeing about its terrain, and only these bytes can
# say which record each side actually had.
LEVEL_RECORD = 0x0FCB
# The end condition, image 0x001E7: released == to-release AND active == 0.
RELEASED, ACTIVE, TO_RELEASE = 0x43, 0x45, 0x47
SAVED, OVER = 0x46, 0x4F   # ds:0x4F is what image 0x001C8 tests first


def install(m):
    import atexit
    from unicorn import UC_HOOK_CODE

    base = m.load_seg * 16
    out_dir = os.environ["LEM_REC_DIR"]
    want = int(os.environ["LEM_REC_FRAMES"])
    os.makedirs(out_dir, exist_ok=True)
    st = {"n": 0, "manifest": []}

    # THE PLAYER'S INPUT, from the same file the port reads.
    #
    # A level nobody touches compares only the parts of the game that need no
    # player - every skill, the panel, and the level's own ending are behind a
    # click. Both sides take the script from one file so the comparison stays
    # a comparison and not two different games.
    #
    # An action for frame N is applied at the END of frame N-1, because that
    # is when this hook runs: by the time image 0x018F is reached the frame
    # has already been composed with whatever the mouse said earlier. The port
    # applies the same action at the top of the loop pass that composes frame
    # N. Same frame, both sides.
    script = []
    sp = os.environ.get("LEM_REC_SCRIPT")
    if sp:
        for line in open(sp):
            f = line.split("#")[0].split()
            if len(f) >= 2:
                script.append((int(f[0]), f[1],
                               [int(v) for v in f[2:]]))
        script.sort(key=lambda a: a[0])
        print(f"  [rec] {len(script)} scripted actions from {sp}")
    all_lem = os.environ.get("LEM_REC_ARRAY") == "1"
    want_level = os.environ.get("LEM_REC_LEVEL")
    stock = int(os.environ.get("LEM_REC_SKILLS", "0"))
    st["script_at"] = 0
    st["down"] = None

    def pick_level(uc, address, size, user):
        """Poke the level index before the briefing reads it."""
        if want_level is None or st.get("picked"):
            return
        st["picked"] = True
        uc.mem_write(base + DGROUP + LEVEL_INDEX, bytes([int(want_level)]))
        print(f"  [rec] level index poked to {want_level}")

    def on_frame(uc, address, size, user):
        # The emulator keeps every mode the guest has set, in order, so the
        # cue is simply how many times 0x0D has been entered: the first is
        # the "PC Lemmings Loading" screen, the second is the level.
        modes = getattr(m, "video_modes", [])
        if not modes or modes[-1] != 0x0D or st["n"] >= want:
            return
        if sum(1 for v in modes if v == 0x0D) < 2:
            return
        try:
            fb = m.framebuffer()
        except Exception:
            return
        i = st["n"]
        with open(os.path.join(out_dir, f"f{i:05d}.scrn"), "wb") as f:
            f.write(b"SCRN")
            f.write(struct.pack("<HH", m.width, m.height))
            for c in m.palette:
                f.write(bytes(c))
            f.write(fb)
        lit = sum(1 for c in m.palette if any(c))
        # Every skill stocked, if asked. Written each frame rather than once:
        # the level's own set-up runs after the first frames are captured, so
        # a single poke at the start is undone before anything can use it.
        if stock:
            for a in SKILLS:
                if uc.mem_read(base + DGROUP + a, 1)[0] == 0:
                    uc.mem_write(base + DGROUP + a, bytes([stock]))

        # Lemming 0 and the scroll, in THIS run's frame numbering.
        #
        # An input script has to name a screen position, and working one out
        # from a different tool's frame count is how a click lands on empty
        # sky: tools/probe_lemmings.py hooks image 0x01A3 from the start of
        # the level and this hooks image 0x018F from the second mode 0x0D, so
        # their frame numbers are not the same number. Carrying the position
        # HERE means the script and the comparison are counting the same
        # frames by construction.
        def w(o):
            return int.from_bytes(uc.mem_read(base + DGROUP + o, 2),
                                  "little", signed=True)
        n_lem = uc.mem_read(base + DGROUP + COUNT, 1)[0]
        lem0 = None
        if n_lem:
            lem0 = {"x": w(ARRAY), "y": w(ARRAY + 2),
                    "hot_x": w(ARRAY + 4), "hot_y": w(ARRAY + 6)}
        # The WHOLE array, when asked for. A screen says two lemmings look
        # different; the records say which field differs, which is the
        # difference between a finding and a puzzle.
        lem_all = None
        if all_lem:
            lem_all = []
            for k in range(min(n_lem, 16)):
                o = ARRAY + k * REC
                def b1(k):
                    return uc.mem_read(base + DGROUP + o + k, 1)[0]
                # +0x27 the direction, +0x29 the state, +0x24 the byte image
                # 0x027DE sets bit 7 of when a builder runs out and image
                # 0x03DFD reads to go on calling it a BUILDER, +0x21 the
                # bricks image 0x032F2 hands out. A record comparison that
                # stops at position cannot see any of them - and the builder's
                # last six frames differ in something none of x, y, the hot
                # spot, the flags or the animation counter carries.
                lem_all.append({"x": w(o), "y": w(o + 2),
                                "hot_x": w(o + 4), "hot_y": w(o + 6),
                                "flags": w(o + 8) & 0xFFFF,
                                "anim": b1(0x28), "alive": b1(0x26),
                                "dir": b1(0x27) - (256 if b1(0x27) > 127 else 0),
                                "state": b1(0x29), "extra": b1(0x24),
                                "floater": b1(0x25), "fuse": b1(0x20),
                                "bricks": b1(0x21)})
        st["manifest"].append({
            "frame": i,
            "scroll": w(SCROLL),
            "mx": w(MOUSE_X), "my": w(MOUSE_Y),
            "set_pos_calls": int(getattr(m, "mouse_calls", {}).get(4, 0)),
            "emu_pos": list(getattr(m, "mouse_pos", (0, 0))),
            "cur_off": w(CUR_OFF), "cur_base": w(CUR_BASE),
            "cur_bias": uc.mem_read(base + DGROUP + CUR_BIAS, 1)[0],
            "lemmings": n_lem,
            "lem": lem_all,
            "skills": [uc.mem_read(base + DGROUP + a, 1)[0] for a in SKILLS],
            "record_head": bytes(uc.mem_read(base + DGROUP + LEVEL_RECORD,
                                             0x20)).hex(),
            "diggers": uc.mem_read(base + DGROUP + DIGGERS, 1)[0],
            "released": uc.mem_read(base + DGROUP + RELEASED, 1)[0],
            "active": uc.mem_read(base + DGROUP + ACTIVE, 1)[0],
            "to_release": uc.mem_read(base + DGROUP + TO_RELEASE, 1)[0],
            "saved": uc.mem_read(base + DGROUP + SAVED, 1)[0],
            "over": uc.mem_read(base + DGROUP + OVER, 1)[0],
            "mode": (getattr(m, "video_modes", []) or [0])[-1],
            "lem0": lem0,
            "game_frame": uc.mem_read(base + DGROUP + FRAME_COUNTER, 1)[0],
            "lit_dac": lit,
            "crtc_start": getattr(m, "crtc", {}).get(0x0C, 0) * 256
                          + getattr(m, "crtc", {}).get(0x0D, 0),
        })
        st["n"] = i + 1

        # Then the input for the frame about to be composed.
        while (st["script_at"] < len(script)
               and script[st["script_at"]][0] <= st["n"]):
            _, verb, arg = script[st["script_at"]]
            st["script_at"] += 1
            if verb == "move" and len(arg) == 2:
                st["down"] = None
                # THE SCRIPT IS IN SCREEN PIXELS, AND THE GUEST IS NOT.
                #
                # Image 0x0426D stores what INT 33h hands back, clamped to
                # 8..0x148 across and 8..0xC8 down - it adds nothing. The
                # game set that range itself, and it is the screen shifted by
                # eight, which is why image 0x05827 draws the cross-hair with
                # the same eight taken back off. So a script coordinate has
                # to have the eight PUT ON here, exactly as the port's window
                # path does in level_run.
                #
                # Without it the two sides read one file and mean different
                # places by it, which is not a comparison at all - it is two
                # different games scored against each other. The first run
                # here aimed eight pixels apart and the original assigned a
                # digger the port had no way to.
                at = (arg[0] + 8, arg[1] + 8)
                m.click_mouse(0, at[0], at[1], False)
                st["at"] = at
            elif verb == "down" and st.get("at"):
                st["down"] = st["at"]
                m.click_mouse(0, st["at"][0], st["at"][1], True)
            elif verb == "up" and st.get("at"):
                st["down"] = None
                m.click_mouse(0, st["at"][0], st["at"][1], False)
        # RE-ASSERT THE POSITION EVERY FRAME, not just when the script moves.
        #
        # Under the dummy video driver pygame still delivers a MOUSEMOTION,
        # and the emulator's window loop takes it: the scripted pointer was
        # quietly replaced by whatever that event said, five frames after the
        # script last touched it. It reads as the GAME moving the pointer,
        # and cost a round chasing an INT 33h set-position call the guest had
        # never made - the emulator's own AX=4 counter stayed at 2 throughout,
        # which is what settled it.
        #
        # Holding it also matches what a player does. A press in particular
        # has to be HELD across frames because the game samples the button
        # through its own handler rather than on an edge - see
        # tools/probe_digger.py - and a tapped one is missed as often as not.
        if st.get("at"):
            m.click_mouse(0, st["at"][0], st["at"][1],
                          bool(st["down"]))

    m.uc.hook_add(UC_HOOK_CODE, on_frame,
                  None, base + FRAME_DONE, base + FRAME_DONE)
    # Image 0x04401 is the menu, by which point the image is unpacked and DS
    # is up - the same place tools/probe_code.py drives from.
    if want_level is not None:
        m.uc.hook_add(UC_HOOK_CODE, pick_level, None,
                      base + 0x04401, base + 0x04401)

    def save():
        with open(os.path.join(out_dir, "manifest.json"), "w") as f:
            json.dump(st["manifest"], f)
        print(f"  [rec] {st['n']} frames -> {out_dir}")

    atexit.register(save)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--out", default=os.path.join(ROOT, "out", "ref"))
    ap.add_argument("--level", type=int, default=None,
                    help="poke ds:0x1F6B, the level index, before the "
                         "briefing reads it - play.py only reaches Fun 1. "
                         "Level 2 is where lemmings fall far enough to die. "
                         "THIS IS THE ORIGINAL'S OWN INDEX AND IT IS "
                         "ZERO-BASED, so --level N is the port's --number "
                         "N+1: lemdev --number 5 is this tool's --level 4. "
                         "Using the same integer for both records one level "
                         "and replays another, and the comparison then reads "
                         "as the port getting everything wrong from the "
                         "first frame a lemming exists")
    ap.add_argument("--skills", type=int, default=0,
                    help="stock every skill this many times - a POKE, so that "
                         "the seven \"Just dig!\" never offers can be driven "
                         "at all. The port gets the same one")
    ap.add_argument("--lemmings", action="store_true",
                    help="carry the whole lemming array in the manifest, so "
                         "a difference on the screen can be traced to a field")
    ap.add_argument("--script", default=None,
                    help="an input script, the same file the port reads "
                         "through LEM_INPUT_SCRIPT - see level_run in "
                         "reconstruct/src/sdl_io.c for the format")
    ap.add_argument("--seconds", type=int, default=0,
                    help="how long to let the guest run; 0 picks a length "
                         "from --frames at the rate this actually manages")
    a = ap.parse_args()

    out_dir = os.path.abspath(a.out)
    # A stale directory reads as a successful run.
    if os.path.isdir(out_dir):
        for n in os.listdir(out_dir):
            if n.endswith(".scrn") or n == "manifest.json":
                os.unlink(os.path.join(out_dir, n))
    # A RUN THAT IS CUT OFF MUST NOT LOOK LIKE A SHORT ONE.
    #
    # The guest manages somewhere between 1.5 and 6 frames a second depending
    # on what else the machine is doing, so a budget worked out from an
    # average is a coin toss at 5,100 frames - and the failure is silent: the
    # run is killed at its ceiling, the frames it managed are all valid, and
    # the comparison scores them and reports a figure. That is the same trap
    # as tools/probe_exit.py's "0 lemmings reached it", which read as a fact
    # about the game three times running.
    #
    # So the budget is deliberately generous, --seconds overrides it, and the
    # check at the end says plainly when fewer frames arrived than were asked
    # for.
    seconds = a.seconds or int(90 + a.frames / 1.4)

    env = dict(os.environ, LEM_HOOK=os.path.abspath(__file__),
               LEM_REC_DIR=out_dir, LEM_REC_FRAMES=str(a.frames))
    if a.script:
        env["LEM_REC_SCRIPT"] = os.path.abspath(a.script)
    if a.lemmings:
        env["LEM_REC_ARRAY"] = "1"
    if a.skills:
        env["LEM_REC_SKILLS"] = str(a.skills)
    if a.level is not None:
        env["LEM_REC_LEVEL"] = str(a.level)
    subprocess.run([sys.executable, os.path.join(HERE, "play.py"),
                    "--auto", "--start", "--start-delay", "5",
                    "--start-tries", "3", "--click-tries", "4",
                    "--", "--run-seconds", str(seconds)],
                   cwd=ROOT, env=env, stdout=subprocess.DEVNULL)

    man_path = os.path.join(out_dir, "manifest.json")
    if not os.path.exists(man_path):
        print("\n  NOTHING WAS RECORDED. That is a failure, not a pass.")
        return 2
    with open(man_path) as f:
        man = json.load(f)
    if not man:
        print("\n  The run never reached the level. Nothing to compare.")
        return 1
    # A scripted run that never assigned anything has not tested what the
    # script was for, and must say so rather than report a happy 100% of a
    # level where nothing happened.
    if a.script:
        dig = [m["diggers"] for m in man]
        if dig and dig[0] == dig[-1]:
            print(f"\n  THE SCRIPT NEVER ASSIGNED A DIGGER - the count stayed"
                  f" at {dig[0]} for all {len(man)} frames. Whatever this"
                  f" sequence compares, it is not a level being played.")
        elif dig:
            first = next(i for i, v in enumerate(dig) if v != dig[0])
            print(f"\n  diggers {dig[0]} -> {dig[-1]}, first spent at frame"
                  f" {first}")

    if len(man) < a.frames:
        # A SHORT RECORDING HAS TWO CAUSES AND THEY LOOK IDENTICAL in a frame
        # count: the run was cut off at its ceiling, or the LEVEL ENDED. The
        # second is not a failure - play "Just dig!" properly and every
        # lemming is out at about 708 frames, four fifths short of the 5,100
        # of the expiration timer - and reporting it as truncation would send
        # someone off to raise --seconds for a run that was already complete.
        #
        # The fade tells them apart. The game fades out on its way to the
        # results screen, so a dead palette on the last frames is the level
        # finishing; a live one is the recorder being stopped mid-play.
        tail = [m["lit_dac"] for m in man[-3:]]
        if tail and not any(tail):
            print(f"\n  {len(man)} frames, and the last of them fade out -"
                  f" THE LEVEL ENDED. That is the whole level, not a short"
                  f" recording, and the {a.frames} asked for was simply more"
                  f" than it lasts.")
        else:
            print(f"\n  ONLY {len(man)} OF THE {a.frames} FRAMES ASKED FOR"
                  f" were recorded, and the last one is still lit - so the"
                  f" run was cut off at its {seconds}s ceiling rather than"
                  f" the level ending. The frames that did arrive are good,"
                  f" but this is NOT a recording of {a.frames} frames and"
                  f" must not be scored as one. Give it --seconds.")

    lit = [m["lit_dac"] for m in man]
    print(f"\n  {len(man)} frames in {out_dir}")
    print(f"    game frame counter : {man[0]['game_frame']} .. "
          f"{man[-1]['game_frame']}")
    print(f"    lit DAC entries    : {min(lit)} .. {max(lit)}"
          f"   (0 means a frame captured mid-fade)")
    dark = sum(1 for v in lit if v == 0)
    if dark:
        print(f"    {dark} frame(s) with a dead palette - the fade-in. Real,"
              f" not an error, but do not score colour against them.")
    starts = {m["crtc_start"] for m in man}
    print(f"    CRTC start values  : {sorted(hex(v) for v in starts)}"
          f"   ({len(starts)} - two means the page is flipping)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
