"""The original and the port, played side by side in lockstep - for LOOKING.

Reads a reference from tools/record_level.py (.scrn frames) and the port's
own screens from LEM_SCREEN_DIR (.raw frames), pairs them the way
tools/compare_screen.py does - the reference's frame N against the port's
N-1, because the capture at image 0x018F holds the previous pass's page - and
shows them in one window: the original on the left, the port on the right,
and - press d - a third pane with the pixels that differ in magenta.

    uv run python tools/side_by_side.py --ref out/ref_bug2 --dir out/bug2_screen

To make the port's half for a recorded input script:

    SDL_VIDEODRIVER=dummy LEM_FRAMES=340 LEM_SCREEN_DIR=out/bug2_screen \\
        LEM_INPUT_SCRIPT=out/bug.txt ./reconstruct/lemdev \\
        --data lemming1.pc --rating fun --number 1 --show

Keys: space pause, d the difference pane, Left/Right one frame (pauses), Up/Down speed, Home start,
Esc or q quit. It plays at the level's own 17 frames a second by default.

BOTH HALVES ARE DRAWN THROUGH THE CAPTURE'S ONE PALETTE. A capture holds the
DAC as it stood at one moment, and the level swaps the upper eight for the
panel part-way down the screen, so the panel's colours here are the terrain's
- on both sides alike. The indices are what is compared; the colours are only
for the eye.
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from compare_screen import MAP, read_raw, read_scrn   # noqa: E402

W, H = 320, 200


def read_pal(path):
    with open(path, "rb") as f:
        b = f.read(8 + 768)
    # Already widened to eight bits by the emulator (record_level.py writes
    # m.palette as it stands), so the bytes go to the screen as they are.
    return [tuple(b[8 + 3 * i:11 + 3 * i]) for i in range(256)]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ref", required=True, help="directory of .scrn frames")
    ap.add_argument("--dir", required=True, help="directory of the port's .raw frames")
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("--fps", type=float, default=17.0)
    ap.add_argument("--start", type=int, default=0, help="reference frame to open on")
    ap.add_argument("--frame-offset", type=int, default=1,
                    help="the reference's frame N is the port's N-OFFSET (see compare_screen.py)")
    a = ap.parse_args()

    refs = sorted(f for f in os.listdir(a.ref) if f.endswith(".scrn"))
    pairs = []
    for i, name in enumerate(refs):
        k = i - a.frame_offset
        g = os.path.join(a.dir, f"f{k:05d}.raw")
        if k >= 0 and os.path.isfile(g):
            pairs.append((i, os.path.join(a.ref, name), g))
    if not pairs:
        raise SystemExit("no frames pair up - check --ref and --dir")

    os.environ.pop("SDL_VIDEODRIVER", None)      # this tool is for looking at
    import pygame

    pygame.init()
    s = a.scale
    show_diff = False

    def window():
        n = 3 if show_diff else 2
        return pygame.display.set_mode((n * W * s + (n - 1) * 4, H * s + 24))

    screen = window()
    font = pygame.font.Font(None, 22)
    clock = pygame.time.Clock()

    def surface(pix, pal):
        surf = pygame.image.frombuffer(bytes(pix[:W * H]), (W, H), "P")
        surf.set_palette(pal)
        return pygame.transform.scale(surf, (W * s, H * s))

    cache = {}

    def frame(j):
        if j not in cache:
            i, rf, gf = pairs[j]
            rw, rh, rp = read_scrn(rf)
            gw, gh, gp = read_raw(gf)
            orig = b"".join(rp[y * rw:y * rw + W] for y in range(H))
            port = b"".join(gp.translate(MAP)[y * gw:y * gw + W] for y in range(H))
            pal = read_pal(rf)
            diff = bytes(255 if o != p else 0 for o, p in zip(orig, port))
            ndiff = diff.count(255)
            dpal = [(0, 0, 0)] * 255 + [(255, 0, 255)]
            dimmed = [tuple(c // 3 for c in rgb) for rgb in pal]
            dsurf = pygame.image.frombuffer(
                bytes(255 if d else p for d, p in zip(diff, port)), (W, H), "P")
            dsurf.set_palette(dimmed[:255] + dpal[255:])
            cache[j] = (surface(orig, pal), surface(port, pal),
                        pygame.transform.scale(dsurf, (W * s, H * s)), ndiff, i)
        return cache[j]

    pygame.display.set_caption("original | port")
    j = next((n for n, p in enumerate(pairs) if p[0] >= a.start), 0)
    playing = True
    fps = a.fps
    while True:
        for ev in pygame.event.get():
            if ev.type == pygame.QUIT:
                return
            if ev.type == pygame.KEYDOWN:
                if ev.key in (pygame.K_ESCAPE, pygame.K_q):
                    return
                if ev.key == pygame.K_SPACE:
                    playing = not playing
                elif ev.key == pygame.K_RIGHT:
                    playing = False
                    j = min(j + 1, len(pairs) - 1)
                elif ev.key == pygame.K_LEFT:
                    playing = False
                    j = max(j - 1, 0)
                elif ev.key == pygame.K_UP:
                    fps = min(fps * 2, 120)
                elif ev.key == pygame.K_DOWN:
                    fps = max(fps / 2, 1)
                elif ev.key == pygame.K_d:
                    show_diff = not show_diff
                    screen = window()
                elif ev.key == pygame.K_HOME:
                    j = 0
        o, p, d, ndiff, i = frame(j)
        screen.fill((32, 32, 32))
        screen.blit(o, (0, 24))
        screen.blit(p, (W * s + 4, 24))
        if show_diff:
            screen.blit(d, (2 * (W * s + 4), 24))
        label = (f"original frame {i}   |   port frame {i - a.frame_offset}   |   "
                 f"{ndiff} pixels differ   |   {fps:g} fps"
                 f"{'' if playing else '   PAUSED'}")
        screen.blit(font.render(label, True, (230, 230, 230)), (6, 4))
        pygame.display.flip()
        if playing:
            j += 1
            if j >= len(pairs):
                j = len(pairs) - 1
                playing = False
        clock.tick(fps if playing else 30)


if __name__ == "__main__":
    main()
