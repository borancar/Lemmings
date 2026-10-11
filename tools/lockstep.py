"""Play the ORIGINAL, with the port playing the same game beside it.

    uv run python tools/lockstep.py                          # Fun 1
    uv run python tools/lockstep.py --rating tricky --number 4 --scale 3
    uv run python tools/lockstep.py --in-order               # the campaign

--port-only plays the PORT alone, in its own window at the level's own
seventeen frames a second, and keeps only the input; nothing of the
original runs until tools/verify_solutions.py records its run of each
solution. Combine it with --in-order for the campaign at full speed.

--in-order plays the levels one after another in the same window: the next
level after a win, the same one again after a loss, starting from the first
level that has no solution yet. A WON level keeps its input script as
solutions/<rating><NN>.txt and its two recordings - screens and data
segment, both sides - in out/solutions/<rating><NN>/, and is scored at once;
tools/verify_solutions.py replays every solution through the port later.
Won or lost is image 0x04720's own test, ds:0x40 against ds:0x41.

ONE window, opened when the level starts: the original on the left, the port
on the right, frame for frame. You play in that window - the pointer over
either half, both buttons, the keys - and what you do goes to the ORIGINAL.
The port is handed, once a frame, exactly what the original's frame loop
read, and it cannot run ahead: before each frame it waits until the original
has finished that frame (harness.c's LEM_INPUT_FOLLOW).

The original runs headless. Its start-up menus, title and briefing are driven
by tools/play.py out of sight, with ds:0x1F6B poked before the briefing reads
it as tools/record_level.py --level does, so the window opens on the level.
Close it, or let the level end, and the run is over.

What the original read is kept as an ordinary input script, and both games'
frames beside it, in out/lockstep/<level>/ - so anything worth a second look
replays through record_level.py --script and lemdev, and
tools/side_by_side.py plays the two halves back:

    uv run python tools/side_by_side.py --ref out/lockstep/fun01/orig \\
        --dir out/lockstep/fun01/port

HOW THE ORIGINAL'S INPUT IS PINNED TO A FRAME
---------------------------------------------
The original does not read the mouse once a frame. Image 0x017C7, the timer
ISR, calls image 0x04252 on every tick - INT 33h AX=3 at image 0x0425C, then
ds:0x14, ds:0x10 and ds:0x12 - so a frame acts on whatever the LAST tick
before it read, and a copy taken at a frame boundary would race the ticks
that follow. tools/record_level.py met the same race replaying a script.

THE TIMER IS HELD TOO, from image 0x018F to the frame's end, so all five of
a frame's ticks fall in the wait after its work - the fast machine the port
runs (game.c's level_wait_ticks). Without it the emulator's wall-clock timer
decides how many ticks land mid-frame, and a one-frame click on the release
rate moved it by anything from 2 to 5.

So inside a level the driver's answer is held still. At image 0x04261,
straight after the INT 33h, the hook takes in what the window has sent and
puts back the value frozen for this frame. At image 0x018F, the frame's end,
the newest becomes the next frame's: it is written to the script and stored
in ds:0x10/0x12/0x14 as one tick's poll would. Every tick of the next frame
reads that one value - which is what the port gets, at the top of the same
frame.

A CLICK SHORTER THAN A FRAME is latched: a button down at any moment counts
as down for the next frame. The emulator runs a few frames a second, so an
ordinary click would otherwise fall between two frames and vanish from both
games at once. Keys go the same way: delivered at the frame's end, with the
same line written for the port.

Nothing here is in the original. It is a harness, like record_level.py, and
it changes WHEN the original samples its mouse - once a frame instead of on
every tick - exactly as a replayed script already does.
"""
import json
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

DGROUP = 0x9A90            # image offset of DS
FRAME_DONE = 0x018F        # the game's own end-of-frame, inside a level only
AFTER_INT33 = 0x04261      # image 0x04252's poll, the instruction after INT 33h
MENU = 0x04401             # the menu: DS is up, the briefing not yet read
WORK_DONE = (0x00105, 0x002CD, 0x00314)   # the frame's work over - see below
PASS_END = 0x001C8         # where tools/dg_hook.py takes the data segment
DG_END = 0x4DFD            # reconstruct/src/dgroup.h
SAVED_PCT, NEEDED_PCT = 0x40, 0x41   # ds: image 0x04720 compares the two
LEVEL_INDEX = 0x1F6B       # ds: the index into the running order
NO_BUTTONS = 0x200B        # ds: 0xFF stops image 0x04262 storing ds:0x14
RATINGS = ("fun", "tricky", "taxing", "mayhem")   # lemdev --rating's names
SOLUTIONS = os.path.join(ROOT, "solutions")      # one input script a level
W, H = 320, 200


# ---------------------------------------------------------------------------
# THE HOOK - inside the emulator's process, through LEM_HOOK.
# ---------------------------------------------------------------------------
def install(m):
    import struct
    from unicorn import UC_HOOK_CODE
    from unicorn.x86_const import UC_X86_REG_BX, UC_X86_REG_CX, UC_X86_REG_DX

    base = m.load_seg * 16
    ds = base + DGROUP
    want = int(os.environ["LEM_LOCK_LEVEL"])
    rating, number = want // 30, want % 30 + 1
    sess = os.environ["LEM_LOCK_SESSION"]
    for d in ("orig", "port"):
        os.makedirs(os.path.join(sess, d), exist_ok=True)
        for f in os.listdir(os.path.join(sess, d)):
            os.unlink(os.path.join(sess, d, f))
    script = os.path.join(sess, "input.txt")

    # What the window sends: "m x y buttons", "k scancode down", "q".
    inbox = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    sock_path = os.environ["LEM_LOCK_SOCK"]
    if os.path.exists(sock_path):
        os.unlink(sock_path)
    inbox.bind(sock_path)
    inbox.setblocking(False)

    st = {
        "picked": False,
        "modes": -1,          # len(m.video_modes) when the level began
        "frozen": None,       # (cx, dx, bx) every tick of this frame reads
        "hand": None,         # (x, y, buttons) as the window last said
        "sticky": 0,          # every button down since the last frame
        "keys": [],           # (scancode, down) held for the frame end
        "i": 0, "f": None, "port": None, "last": None, "over": False,
        "dg": None, "result": {},
    }

    def status(what):
        tmp = os.path.join(sess, "status.json.tmp")
        with open(tmp, "w") as f:
            json.dump(dict(st["result"], state=what, frames=st["i"],
                           level=want), f)
        os.replace(tmp, os.path.join(sess, "status.json"))

    status("starting")

    def in_level():
        modes = getattr(m, "video_modes", [])
        return len(modes) == st["modes"] and modes and modes[-1] == 0x0D

    def finish(why):
        if st["over"]:
            return
        st["over"] = True
        # WON OR LOST, as image 0x04720 decides it: the rescued percentage
        # against the requirement. ds:0x40 is rewritten by every panel frame
        # (image 0x03AF4) and ds:0x41 by the briefing (image 0x0459E), so read
        # now, with the level over, they are what the results screen will
        # show. A window closed mid-level is no verdict at all.
        if why == "over":
            pct = m.uc.mem_read(ds + SAVED_PCT, 2)
            st["result"] = {"saved_pct": pct[0], "needed_pct": pct[1],
                            "won": pct[0] >= pct[1]}
        if st["dg"]:
            st["dg"].close()
            st["dg"] = None
        # The port first: `# end` releases it to run on without waiting, and
        # it would, writing frames the original never made.
        if st["port"] and st["port"].poll() is None:
            st["port"].terminate()
            try:
                st["port"].wait(timeout=5)
            except subprocess.TimeoutExpired:
                st["port"].kill()
        if st["f"]:
            st["f"].write("# end\n")
            st["f"].close()
            st["f"] = None
        status(why)
        m.stop_requested = True

    import atexit
    atexit.register(lambda: finish("stopped"))

    def drain():
        while True:
            try:
                msg = inbox.recv(64).decode().split()
            except BlockingIOError:
                return
            if not msg:
                continue
            if msg[0] == "m" and len(msg) == 4:
                x, y, b = (int(v) for v in msg[1:])
                st["hand"] = (x, y, b)
                st["sticky"] |= b & 3
            elif msg[0] == "k" and len(msg) == 3:
                st["keys"].append((int(msg[1]), msg[2] == "1"))
            elif msg[0] == "q":
                finish("closed")

    # The level, poked where tools/record_level.py pokes it.
    def pick_level(uc, address, size, user):
        if not st["picked"]:
            st["picked"] = True
            uc.mem_write(ds + LEVEL_INDEX, bytes([want]))

    # The start-up driver clicks at (160, 100) to get through the briefing,
    # and one of its clicks can land after the level is up - on a lemming,
    # with a skill selected. Once the level has begun it clicks nothing.
    real_click = m.click_mouse

    def click_mouse(button=0, x=None, y=None, down=True):
        if st["modes"] != -1:
            return m.mouse_pos
        return real_click(button, x, y, down)
    m.click_mouse = click_mouse

    def after_int33(uc, address, size, user):
        if st["frozen"] is None or st["over"]:
            return
        if not in_level():
            finish("over")
            return
        drain()
        fcx, fdx, fbx = st["frozen"]
        uc.reg_write(UC_X86_REG_CX, fcx)
        uc.reg_write(UC_X86_REG_DX, fdx)
        uc.reg_write(UC_X86_REG_BX, fbx)

    def begin_level():
        st["modes"] = len(getattr(m, "video_modes", []))
        st["f"] = open(script, "w")
        st["f"].write(
            "# recorded by tools/lockstep.py from the ORIGINAL's frame loop\n"
            f"# level {want}  rating {rating}   (ds:0x1F6B, ds:0x1F6E)\n"
            "# frames are the level's own count; move is in SCREEN\n"
            "# pixels, which is ds:0x10 less the eight image 0x0426D\n"
            "# clamps against; key is a SCANCODE.\n")
        st["f"].flush()
        st["dg"] = open(os.path.join(sess, "orig", "dg.bin"), "wb")
        env = dict(os.environ, LEM_INPUT_SCRIPT=script, LEM_INPUT_FOLLOW="1",
                   LEM_SCREEN_DIR=os.path.join(sess, "port"),
                   LEM_DG_FILE=os.path.join(sess, "port", "dg.bin"),
                   SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
        env.pop("LEM_HOOK", None)
        st["port"] = subprocess.Popen(
            [os.path.join(ROOT, "reconstruct", "lemdev"),
             "--data", "lemming1.pc", "--rating", RATINGS[rating],
             "--number", str(number), "--show"],
            cwd=ROOT, env=env, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)
        print(f"  [lockstep] {RATINGS[rating]} {number}: the level is up",
              flush=True)

    def on_frame(uc, address, size, user):
        if st["over"]:
            return
        modes = getattr(m, "video_modes", [])
        if not modes or modes[-1] != 0x0D or modes.count(0x0D) < 2:
            return
        if st["modes"] == -1:
            begin_level()
        elif len(modes) != st["modes"]:
            finish("over")
            return
        i = st["i"]
        # The original's frame, captured as tools/record_level.py does: at
        # image 0x018F the page just shown was composed by the pass before.
        tmp = os.path.join(sess, "orig", f"f{i:05d}.tmp")
        with open(tmp, "wb") as f:
            f.write(b"SCRN" + struct.pack("<HH", m.width, m.height))
            for c in m.palette:
                f.write(bytes(c))
            f.write(m.framebuffer())
        os.replace(tmp, os.path.join(sess, "orig", f"f{i:05d}.scrn"))

        drain()
        # The next frame's input: the window's newest, with every button
        # that went down since the last frame. Before the window has said
        # anything, the guest's own bytes stand.
        if st["hand"] is None:
            def w16(o):
                return int.from_bytes(uc.mem_read(ds + o, 2), "little")
            x, y, bx = w16(0x10), w16(0x12), w16(0x14) & 3
        else:
            x, y, bx = st["hand"]
            x, y = x + 8, y + 8                     # screen -> ds:0x10
        bx = (bx | st["sticky"]) & 3
        st["sticky"] = 0
        x = min(max(x, 8), 0x148)                   # image 0x0426D
        y = min(max(y, 8), 0xC8)                    # image 0x04282
        st["frozen"] = (x, y, bx)
        # As the tick's own poll would, so this frame sees it from its start.
        if uc.mem_read(ds + NO_BUTTONS, 1)[0] != 0xFF:     # image 0x04262
            uc.mem_write(ds + 0x14, bx.to_bytes(2, "little"))
        uc.mem_write(ds + 0x10, x.to_bytes(2, "little"))
        uc.mem_write(ds + 0x12, y.to_bytes(2, "little"))

        f = st["f"]
        last = st["last"] or (None, None, 0)
        if (x, y) != last[:2]:
            f.write(f"{i} move {x - 8} {y - 8}\n")
        if (bx ^ last[2]) & 1:
            f.write(f"{i} {'down' if bx & 1 else 'up'}\n")
        if (bx ^ last[2]) & 2:
            f.write(f"{i} {'rdown' if bx & 2 else 'rup'}\n")
        st["last"] = (x, y, bx)
        # The keys are pressed when the frame's work is done (work_done), as
        # tools/record_level.py presses a script's, and written now.
        st["due"] = st["keys"]
        st["keys"] = []
        for sc, down in st["due"]:
            f.write(f"{i} {'key' if down else 'keyup'} {sc}\n")
        f.write(f"# frame {i}\n")
        f.flush()
        st["i"] = i + 1
        if i % 17 == 0:
            status("playing")
        # No tick until the frame's work is done - the fast machine the port
        # runs. See LemmingsVga.ticks_held.
        m.ticks_held = True

    # THE DATA SEGMENT, a frame at a time, at the moment tools/dg_hook.py
    # takes it and for the reasons it gives - the port's LEM_DG_FILE is the
    # other half, and tools/verify_dgroup.py's compare() scores the two.
    def pass_end(uc, address, size, user):
        if st["dg"] and not st["over"] and in_level():
            st["dg"].write(bytes(uc.mem_read(ds, DG_END)))

    def work_done(uc, address, size, user):
        for sc, down in st.get("due", []):
            m.press_key(sc, 0, down)
        st["due"] = []
        m.ticks_held = False

    m.uc.hook_add(UC_HOOK_CODE, pick_level, None, base + MENU, base + MENU)
    m.ticks_after_keys = True
    for a in WORK_DONE:
        m.uc.hook_add(UC_HOOK_CODE, work_done, None, base + a, base + a)
    m.uc.hook_add(UC_HOOK_CODE, after_int33, None,
                  base + AFTER_INT33, base + AFTER_INT33)
    m.uc.hook_add(UC_HOOK_CODE, on_frame, None,
                  base + FRAME_DONE, base + FRAME_DONE)
    m.uc.hook_add(UC_HOOK_CODE, pass_end, None,
                  base + PASS_END, base + PASS_END)


# ---------------------------------------------------------------------------
# THE WINDOW - this process.
# ---------------------------------------------------------------------------
GAP, TOP = 4, 22


class Window:
    """One window for the whole sitting: levels come and go inside it.

    `hand`, if given, is a function called once a loop pass that returns a
    list of pygame-like events to inject - the headless self-test's player.
    """

    def __init__(self, scale, hand=None):
        if hand is None:
            os.environ.pop("SDL_VIDEODRIVER", None)  # this one is for looking at
        import pygame
        self.pg = pygame
        self.scale, self.hand = scale, hand
        pygame.init()
        self.screen = pygame.display.set_mode((2 * W * scale + GAP,
                                               H * scale + TOP))
        pygame.display.set_caption("original  |  port")
        self.font = pygame.font.Font(None, 20)
        self.big = pygame.font.Font(None, 16 * scale)
        self.clock = pygame.time.Clock()

    def events(self):
        return self.pg.event.get() + (self.hand() if self.hand else [])

    def message(self, *lines):
        self.screen.fill((24, 24, 24))
        y = self.screen.get_height() // 3
        for line in lines:
            t = self.big.render(line, True, (230, 230, 230))
            self.screen.blit(t, ((self.screen.get_width() - t.get_width()) // 2, y))
            y += t.get_height() + 8
        self.pg.display.flip()


def level_name(index):
    return f"{RATINGS[index // 30]}{index % 30 + 1:02d}"


def view(sess, sock_path, emu, win, title):
    """Show the two games and send the player's input to the original.

    Returns (worst frame's differing pixels, True if the player closed the
    window) - closing it ends the sitting, not just the level."""
    from compare_screen import MAP, read_raw, read_scrn
    pygame = win.pg
    scale = win.scale
    from dos_emulator.emulator import KEYMAP

    out = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)

    def send(msg):
        try:
            out.sendto(msg.encode(), sock_path)
        except OSError:
            pass

    orig_dir, port_dir = os.path.join(sess, "orig"), os.path.join(sess, "port")
    status_path = os.path.join(sess, "status.json")

    def state():
        try:
            with open(status_path) as f:
                return json.load(f)["state"]
        except (OSError, ValueError, KeyError):
            return "starting"

    # Nothing to show until the level is up: the original is still going
    # through its menus, out of sight.
    win.message(title, "the original is on its way to the level...")
    while not os.path.exists(os.path.join(orig_dir, "f00001.scrn")):
        for ev in win.events():
            if ev.type == pygame.QUIT:
                emu.terminate()
                return None, True
        if emu.poll() is not None or state() not in ("starting", "playing"):
            print("  [lockstep] the original stopped before the level")
            return None, False
        win.clock.tick(10)

    def surf(pix, pal):
        s = pygame.image.frombuffer(pix, (W, H), "P")
        s.set_palette(pal)
        return pygame.transform.scale(s, (W * scale, H * scale))

    shown, picture, btn = 0, None, 0
    worst = 0
    while True:
        for ev in win.events():
            if ev.type == pygame.QUIT:
                send("q")
                return worst, True
            if ev.type in (pygame.MOUSEMOTION, pygame.MOUSEBUTTONDOWN,
                           pygame.MOUSEBUTTONUP):
                if ev.type == pygame.MOUSEBUTTONDOWN:
                    btn |= {1: 1, 3: 2}.get(ev.button, 0)
                elif ev.type == pygame.MOUSEBUTTONUP:
                    btn &= ~{1: 1, 3: 2}.get(ev.button, 0)
                mx, my = ev.pos
                my -= TOP
                # Either half: the same game position under the pointer.
                if mx >= W * scale + GAP:
                    mx -= W * scale + GAP
                x = min(max(mx // scale, 0), W - 1)
                y = min(max(my // scale, 0), H - 1)
                send(f"m {x} {y} {btn}")
            elif ev.type in (pygame.KEYDOWN, pygame.KEYUP):
                k = KEYMAP.get(ev.key)
                if k:
                    send(f"k {k[0]} {1 if ev.type == pygame.KEYDOWN else 0}")

        # The newest frame BOTH have: the original's capture N holds the
        # pass before, which is the port's N-1 (compare_screen.py).
        n = shown
        while (os.path.exists(os.path.join(orig_dir, f"f{n + 1:05d}.scrn"))
               and os.path.exists(os.path.join(port_dir, f"f{n:05d}.raw"))):
            n += 1
        if n > shown:
            try:
                rf = os.path.join(orig_dir, f"f{n:05d}.scrn")
                rw, rh, rp = read_scrn(rf)
                gw, gh, gp = read_raw(os.path.join(port_dir, f"f{n - 1:05d}.raw"))
                with open(rf, "rb") as f:
                    pb = f.read(8 + 768)[8:]
                pal = [tuple(pb[3 * i:3 * i + 3]) for i in range(256)]
                orig = b"".join(rp[y * rw:y * rw + W] for y in range(H))
                port = gp.translate(MAP)
                port = b"".join(port[y * gw:y * gw + W] for y in range(H))
                diff = sum(1 for a, b in zip(orig, port) if a != b)
                worst = max(worst, diff)
                picture = (surf(orig, pal), surf(port, pal), diff, n)
                shown = n
            except (OSError, ValueError):
                pass        # a file mid-write; the next pass gets it

        win.screen.fill((24, 24, 24))
        what = state()
        if picture:
            o, p, diff, k = picture
            win.screen.blit(o, (0, TOP))
            win.screen.blit(p, (W * scale + GAP, TOP))
            tag = "identical" if diff == 0 else f"{diff} PIXELS DIFFER"
            text = (f"{title}  |  original frame {k}  |  port frame {k - 1}"
                    f"  |  {tag}"
                    + ("" if what == "playing" else f"  |  {what.upper()}"))
            win.screen.blit(win.font.render(text, True,
                            (230, 230, 230) if diff == 0 else (255, 80, 255)),
                            (6, 4))
        pygame.display.flip()
        if what not in ("starting", "playing"):
            return worst, False
        win.clock.tick(60)


def solved(index):
    return os.path.exists(os.path.join(SOLUTIONS, level_name(index) + ".txt"))


def main():
    import argparse
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rating", default=None, choices=RATINGS,
                    help="default fun, or with --in-order the first level "
                         "without a solution")
    ap.add_argument("--number", type=int, default=None,
                    help="the game's level number within the rating, 1-30")
    ap.add_argument("--in-order", action="store_true",
                    help="keep going: the next level after a win, the same "
                         "one again after a loss, until the window is closed")
    ap.add_argument("--port-only", action="store_true",
                    help="play the PORT alone, in its own window at full "
                         "speed, and keep only the input; "
                         "tools/verify_solutions.py records the original's "
                         "run of it later")
    ap.add_argument("--scale", type=int, default=None,
                    help="the window's magnification (default 2, or 3 "
                         "with --port-only)")
    ap.add_argument("--out", default=os.path.join(ROOT, "out", "lockstep"),
                    help="where each level's script and frames are kept")
    a = ap.parse_args()
    if a.number is not None and not 1 <= a.number <= 30:
        ap.error("--number is 1..30")
    if a.rating is None and a.number is None and a.in_order:
        index = next((i for i in range(120) if not solved(i)), 0)
    else:
        index = (RATINGS.index(a.rating or "fun") * 30
                 + (a.number or 1) - 1)
    scale = a.scale or (3 if a.port_only else 2)
    return run(RATINGS[index // 30], index % 30 + 1, scale, a.out,
               in_order=a.in_order, port_only=a.port_only)


def keep_solution(sess, index):
    """A won level: its script goes to solutions/, which is kept, and its two
    recordings to out/solutions/, so a later attempt cannot overwrite the
    reference tools/verify_solutions.py scores the port against."""
    import shutil
    name = level_name(index)
    os.makedirs(SOLUTIONS, exist_ok=True)
    shutil.copy(os.path.join(sess, "input.txt"),
                os.path.join(SOLUTIONS, name + ".txt"))
    ref = os.path.join(ROOT, "out", "solutions", name)
    shutil.rmtree(ref, ignore_errors=True)
    os.makedirs(os.path.dirname(ref), exist_ok=True)
    shutil.move(sess, ref)
    return ref


def run(rating, number, scale, out, hand=None, in_order=False,
        port_only=False):
    r = subprocess.run(["make", "-C", os.path.join(ROOT, "reconstruct")],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        return r.returncode

    win = None if port_only else Window(scale, hand)
    index = RATINGS.index(rating) * 30 + number - 1
    worst_all = 0
    while True:
        if port_only:
            worst, status, quit_ = play_port(index, out, scale)
        else:
            worst, status, quit_ = play_level(index, out, win)
        if worst is not None:
            worst_all = max(worst_all, worst)
        name = f"{RATINGS[index // 30]} {index % 30 + 1}"
        if status.get("won"):
            ref = keep_solution(os.path.join(os.path.abspath(out),
                                             level_name(index)), index)
            print(f"  [lockstep] {name}: WON, {status['saved_pct']}% of "
                  f"{status['needed_pct']}% - solution kept in "
                  f"solutions/{level_name(index)}.txt, recordings in "
                  f"{os.path.relpath(ref, ROOT)}")
            if port_only:
                print("  [lockstep] the original has not played it yet: "
                      "`uv run python tools/verify_solutions.py "
                      f"{level_name(index)}` records its run and compares")
            else:
                score(ref)
        elif "won" in status:
            print(f"  [lockstep] {name}: lost, {status['saved_pct']}% of "
                  f"{status['needed_pct']}%")
        if quit_ or not in_order or hand is not None:
            break
        if status.get("won"):
            if index == 119:
                break
            index += 1
        elif "won" not in status:
            break           # never reached the level, or no verdict
    if hand is not None:
        print(f"  [lockstep] worst frame: {worst_all} pixels differing")
        return 0 if worst_all == 0 else 1
    return 0


def score(ref):
    """The two recordings a won level left behind, scored the way
    tools/verify_solutions.py scores them."""
    subprocess.run([sys.executable, os.path.join(HERE, "verify_solutions.py"),
                    "--live", ref], cwd=ROOT)


def play_port(index, out, scale):
    """The port alone, in its own window at the level's own pace, recording
    what the game read (lemdev --record-input). The recording ends with the
    verdict harness_record_result writes; one without it is a window closed
    mid-level, which ends the sitting as closing lockstep's window does."""
    import shutil
    rating, number = index // 30, index % 30 + 1
    sess = os.path.join(os.path.abspath(out), level_name(index))
    shutil.rmtree(sess, ignore_errors=True)
    os.makedirs(sess)
    script = os.path.join(sess, "input.txt")
    env = dict(os.environ)
    for k in ("SDL_VIDEODRIVER", "LEM_INPUT_SCRIPT", "LEM_SCREEN_DIR",
              "LEM_DG_FILE", "LEM_STATE_FILE", "LEM_FRAMES"):
        env.pop(k, None)
    print(f"  [lockstep] {RATINGS[rating]} {number}: the port, on its own",
          flush=True)
    subprocess.run([os.path.join(ROOT, "reconstruct", "lemdev"),
                    "--data", "lemming1.pc", "--rating", RATINGS[rating],
                    "--number", str(number), "--scale", str(scale), "--show",
                    "--record-input", script],
                   cwd=ROOT, env=env, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    status = {}
    try:
        for line in open(script):
            f = line.split()
            if f[:2] == ["#", "result"] and len(f) == 8:
                over, saved, needed = int(f[3]), int(f[5]), int(f[7])
                status = {"saved_pct": saved, "needed_pct": needed,
                          "won": bool(over) and saved >= needed}
    except OSError:
        pass
    print(f"  [lockstep] input kept in {os.path.relpath(sess, ROOT)}")
    return None, status, not status


def play_level(index, out, win):
    rating, number = index // 30, index % 30 + 1
    sess = os.path.join(os.path.abspath(out), level_name(index))
    # EMPTIED HERE, before the emulator starts, and not only by the hook: the
    # window takes f00001.scrn as "the level is up", and a lost attempt's
    # frames would say so before the new run had cleared them.
    import shutil
    shutil.rmtree(sess, ignore_errors=True)
    os.makedirs(sess)
    sock_path = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"),
                             f"lemmings-lockstep-{os.getpid()}.sock")
    env = dict(os.environ, LEM_HOOK=os.path.abspath(__file__),
               LEM_LOCK_SESSION=sess, LEM_LOCK_LEVEL=str(index),
               LEM_LOCK_SOCK=sock_path, SDL_VIDEODRIVER="dummy")
    env.pop("LEM_WINDOW", None)
    # Headless, and no run-time ceiling: the level lasts as long as you play.
    emu = subprocess.Popen(
        [sys.executable, os.path.join(HERE, "play.py"), "--auto", "--start",
         "--start-delay", "5", "--start-tries", "3", "--click-tries", "4"],
        cwd=ROOT, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    worst, quit_ = None, False
    title = f"{RATINGS[rating].capitalize()} {number}"
    try:
        worst, quit_ = view(sess, sock_path, emu, win, title)
    finally:
        if emu.poll() is None:
            try:
                emu.wait(timeout=10)
            except subprocess.TimeoutExpired:
                emu.terminate()
                try:
                    emu.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    emu.kill()
        try:
            os.unlink(sock_path)
        except OSError:
            pass
    try:
        with open(os.path.join(sess, "status.json")) as f:
            status = json.load(f)
    except (OSError, ValueError):
        status = {}
    print(f"  [lockstep] input and frames kept in {os.path.relpath(sess, ROOT)}")
    return worst, status, quit_


if __name__ == "__main__":
    sys.exit(main())
