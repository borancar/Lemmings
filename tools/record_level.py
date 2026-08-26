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


def install(m):
    import atexit
    from unicorn import UC_HOOK_CODE

    base = m.load_seg * 16
    out_dir = os.environ["LEM_REC_DIR"]
    want = int(os.environ["LEM_REC_FRAMES"])
    os.makedirs(out_dir, exist_ok=True)
    st = {"n": 0, "manifest": []}

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
        st["manifest"].append({
            "frame": i,
            "game_frame": uc.mem_read(base + DGROUP + FRAME_COUNTER, 1)[0],
            "lit_dac": lit,
            "crtc_start": getattr(m, "crtc", {}).get(0x0C, 0) * 256
                          + getattr(m, "crtc", {}).get(0x0D, 0),
        })
        st["n"] = i + 1

    m.uc.hook_add(UC_HOOK_CODE, on_frame,
                  None, base + FRAME_DONE, base + FRAME_DONE)

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
    # Measured: ~5.9 frames a second once in the level, and about 55s of
    # start-up and briefing before the first one.
    seconds = a.seconds or int(60 + a.frames / 5.0)

    env = dict(os.environ, LEM_HOOK=os.path.abspath(__file__),
               LEM_REC_DIR=out_dir, LEM_REC_FRAMES=str(a.frames))
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
