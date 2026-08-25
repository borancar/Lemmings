# SPDX-License-Identifier: GPL-2.0-only
"""Run PC Lemmings (VGA), driving its two start-up menus from the guest's cue.

VGALEMMI.EXE opens in text mode and asks two questions before it ever touches
the VGA: which adapter, then which machine class. LEMVGA.COM answered them from
its command line; nothing here does, so this presses the keys - but it waits
for the guest to *be* on each screen rather than pressing at a wall-clock time,
because emulator speed varies with what the guest is doing and a script tuned
on one run misses on the next.

  uv run python tools/play.py                  # a window to play in
  uv run python tools/play.py --auto           # answer the menus, then play
  uv run python tools/play.py --auto --shots 6 --shot-dir out   # headless
"""
import argparse
import os
import shutil
import socket
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lemmings


def _palette_error(path, ground):
    """How far a capture's palette is from the level's real one.

    Brightness alone is the wrong cue. A fade does not simply dim the palette
    - it passes through intermediate tables whose peak component is high while
    the hues are badly wrong, and "keep the brightest" happily picked one of
    those: a level drawn in red, green and magenta instead of orange and brown.

    So score against what the palette SHOULD be. The 16 colours come from the
    graphics set's own file, and the mode 0Dh attribute map sends pixel 8-15 to
    DAC 0x10-0x17. Returns a mean absolute error per channel, or None.
    """
    try:
        import numpy as np
        with open(path, "rb") as f:
            if f.read(4) != b"SCRN":
                return None
            w, h = struct.unpack("<HH", f.read(4))
            pal = np.frombuffer(f.read(768), dtype=np.uint8).reshape(256, 3)
        buf = open(ground, "rb").read()
        custom, standard = buf[0x3D8:0x3F0], buf[0x3F0:0x408]
        want = np.zeros((16, 3), dtype=np.int16)
        for i in range(8):
            for c in range(3):
                v = standard[i * 3 + c] & 0x3F
                want[i][c] = (v << 2) | (v >> 4)
                v = custom[i * 3 + c] & 0x3F
                want[8 + i][c] = (v << 2) | (v >> 4)
        attr = list(range(8)) + [0x10 + i for i in range(8)]
        got = pal[attr].astype(np.int16)
        # Entries 4 and 7 are the game's own, not the file's; skip them.
        keep = [i for i in range(16) if i not in (4, 7)]
        return float(np.abs(got[keep] - want[keep]).mean())
    except Exception:
        return None


def _bright(path, value=False, threshold=220):
    """Is the captured screen actually lit, or still fading in?

    Judged on the rendered colours. A fade leaves the indices perfectly varied
    and every palette entry nearly black, so anything that looks at indices -
    or even at "is any entry non-black" - passes a screen that is a dark
    smear, and a comparison against it is meaningless.
    """
    try:
        import numpy as np
        with open(path, "rb") as f:
            if f.read(4) != b"SCRN":
                return 0 if value else False
            w, h = struct.unpack("<HH", f.read(4))
            pal = np.frombuffer(f.read(768), dtype=np.uint8).reshape(256, 3)
            idx = np.frombuffer(f.read(w * h), dtype=np.uint8).reshape(h, w)
        peak = int(pal[idx[:160]].max())
    except Exception:
        return 0 if value else False
    return peak if value else peak >= threshold


def screen_lit(path):
    """Was this capture taken with the palette actually up?

    A screen sampled during a fade has an all-black DAC and every index still
    in place. It compares as a huge difference for a reason that has nothing to
    do with what is being tested, and it does not look obviously wrong in a
    report - it looks like a finding. It has produced two of them here: F3
    appeared to redraw 77,433 pixels of the menu, and two captures of the same
    briefing appeared to disagree on a fifth of their background.
    """
    try:
        with open(path, "rb") as f:
            blob = f.read()
    except OSError:
        return False
    return blob[:4] == b"SCRN" and any(blob[8:8 + 768])


def capture_lit(sock_path, path, tries=20, wait=1.0):
    """Take a screen capture, refusing one taken mid-fade.

    Returns True if a lit capture was written. Callers that get False must
    report THAT rather than comparing whatever landed on disk - a run that
    never came up out of its fade has measured nothing.
    """
    if os.path.exists(path):
        os.unlink(path)          # a stale file reads as a successful capture
    for _ in range(tries):
        ask(sock_path, f"screen {path}")
        if screen_lit(path):
            return True
        time.sleep(wait)
    return False


def ask(sock_path, line, timeout=5.0):
    """One command down the control socket, one answer back."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(sock_path)
        s.sendall(line.encode() + b"\n")
        out = b""
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            out += chunk
        return out.decode(errors="replace")
    finally:
        s.close()


def wait_for(sock_path, predicate, what, timeout=120.0, poll=0.25):
    """Poll `status` until the machine is in the state we want to act on."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            st = ask(sock_path, "status")
        except (OSError, socket.timeout):
            time.sleep(poll)
            continue
        if predicate(st):
            return st
        time.sleep(poll)
    raise SystemExit(f"timed out after {timeout}s waiting for {what}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--window", action="store_true",
                    help="open a real window. Everything here is headless by "
                         "default: these tools drive the guest and capture "
                         "through the socket, and a window nobody reads is "
                         "just something popping up on a desktop")
    ap.add_argument("--auto", action="store_true",
                    help="answer the adapter and machine-type menus")
    ap.add_argument("--machine", default=lemmings.STARTUP_KEYS[1],
                    choices=list("1234"),
                    help="1 PC compatible, 2 high performance, 3 PS/2, "
                         "4 Amstrad PC1640/1512 (default 2 - the machine type "
                         "this port targets, and the only one that sets no "
                         "compatibility flags)")
    ap.add_argument("--start", action="store_true",
                    help="once at the title, press F1 to start a game")
    ap.add_argument("--start-tries", type=int, default=4,
                    help="how many times to press F1 (default 4)")
    ap.add_argument("--cmd", action="append", default=[], metavar="CMD",
                    help="with --dump-vga, send this control-socket command "
                         "once in the level (repeatable)")
    ap.add_argument("--dump-mem", action="append", default=[],
                    metavar="ADDR LEN PATH",
                    help="with --dump-vga, dump guest memory (repeatable)")
    ap.add_argument("--dump-screen", default="",
                    help="with --dump-vga, write the decoded screen + palette")
    ap.add_argument("--dump-planes", default="",
                    help="with --dump-vga, also write all four planes here")
    ap.add_argument("--ground",
                    default=os.path.join(lemmings.GAME_DIR, "ground0o.dat"),
                    help="the graphics set, for judging when the palette is up")
    ap.add_argument("--dump-when", default="0x0d",
                    help="video mode to wait for before dumping: 0x0d is the "
                         "level, 0x10 the title and briefing (default 0x0d)")
    ap.add_argument("--dump-delay", type=float, default=45.0,
                    help="seconds to wait for the palette to come up in the "
                         "level before --dump-vga")
    ap.add_argument("--dump-vga", action="store_true",
                    help="once in the level, print the video state")
    ap.add_argument("--click-tries", type=int, default=3,
                    help="how many times to click past the briefing screen")
    ap.add_argument("--start-delay", type=float, default=6.0,
                    help="seconds between F1 presses (default 6)")
    ap.add_argument("--socket", default="",
                    help="control socket path (default: a temporary one)")
    ap.add_argument("rest", nargs=argparse.REMAINDER,
                    help="passed through to the emulator, after --")
    a = ap.parse_args()

    rest = [x for x in a.rest if x != "--"]
    sock = a.socket or os.path.join(
        os.environ.get("XDG_RUNTIME_DIR", "/tmp"), f"lemmings-{os.getpid()}.sock")
    if os.path.exists(sock):
        os.unlink(sock)

    if not a.auto:
        # Nothing to drive: hand the window straight to the player.
        return lemmings.run(rest)

    argv = [sys.executable, "-c",
            "import sys, os; sys.path.insert(0, %r); import lemmings; "
            "lemmings.run(sys.argv[1:])" % os.path.dirname(os.path.abspath(__file__)),
            "--control-socket", sock] + rest
    # The child inherits headlessness unless a window was asked for.
    env = dict(os.environ)
    if a.window:
        env["LEM_WINDOW"] = "1"
        env.pop("SDL_VIDEODRIVER", None)
        env.pop("SDL_AUDIODRIVER", None)
    proc = subprocess.Popen(argv, env=env)
    try:
        # The menus are text mode; the game proper is not. That is the cue.
        wait_for(sock, lambda st: "mode=0x03" in st, "the adapter menu")
        time.sleep(0.6)
        print("[play] adapter menu -> 1 (VGA)")
        ask(sock, "key 1")
        time.sleep(1.5)
        # A digit confirms outright - the menu code jumps straight to the
        # dispatch on '1'-'4', and only the arrow keys merely move the
        # highlight. So no RETURN.
        print(f"[play] machine menu -> {a.machine}")
        ask(sock, f"key {a.machine}")
        st = wait_for(sock, lambda st: "mode=0x03" not in st,
                      "the game to leave text mode", timeout=180)
        print(f"[play] in graphics: {st.strip()}")
        if a.start:
            # The title screen is mode 10h; the menus before it are 0Dh. Wait
            # for the mode the title actually uses rather than a wall clock.
            st = wait_for(sock, lambda st: "mode=0x10" in st,
                          "the title screen", timeout=180)
            # The title fades its palette in before it takes input, and a key
            # pressed during the fade is dropped. There is no cue for "the
            # fade has finished" over the socket, so press more than once and
            # spaced out rather than betting on one moment.
            for attempt in range(a.start_tries):
                time.sleep(a.start_delay)
                print(f"[play] title -> F1 (attempt {attempt + 1})")
                ask(sock, "key f1 4")
            # The briefing screen says "Press mouse button to continue" and
            # means a real click; no key gets past it.
            for attempt in range(a.click_tries):
                time.sleep(a.start_delay)
                print(f"[play] briefing -> click (attempt {attempt + 1})")
                # Held, not instantaneous: a guest that polls the button state
                # rather than a press count would never see a click that goes
                # down and up between two of its polls.
                ask(sock, "click down 0 160 100")
                time.sleep(0.5)
                ask(sock, "click up 0")
        # Capturing does NOT require --start. It used to sit inside that
        # branch, so `--dump-vga` without it waited, dumped nothing, and
        # exited reporting success - which is exactly the failure mode
        # this file warns about elsewhere. The main menu is a screen you
        # reach by NOT pressing F1, so it was unreachable.
        if a.dump_vga:
            # Gameplay is mode 0Dh again, after the 10h title; the
            # briefing is still 10h. Which one to sample matters: a game
            # frees buffers as it finishes with them, so data that is
            # resident during loading is gone by the time the level runs.
            want_mode = a.dump_when
            if want_mode == "0x0d" and not a.start:
                # 0Dh is BOTH the loading screen before the title and the
                # level after it, so waiting for it alone matches
                # immediately and samples the wrong screen. Require the
                # title's 10h first - unless --start already went through
                # it, in which case waiting for 10h again just times out.
                wait_for(sock, lambda st: "mode=0x10" in st,
                         "the title", timeout=180)
            wait_for(sock, lambda st: f"mode={want_mode}" in st,
                     f"mode {want_mode}", timeout=180)
            # Wait for the guest's own cue rather than a wall clock. The
            # level fades its palette in, and "some entries are non-black"
            # is not enough of a cue - mid-fade every entry is non-black
            # and every one is nearly black, so the capture looks varied
            # by index and renders as a dark smear. Poll the actual
            # picture until it is bright.
            if a.dump_screen:
                # Keep the BRIGHTEST sample, not the last. The palette
                # fades in and out around a level, so a poll that stops on
                # a timeout keeps whatever happened to be on screen at the
                # end - which was black, and scored as a perfect match
                # against the level's empty left edge.
                tmp = a.dump_screen + ".sample"
                best = None
                deadline = time.time() + a.dump_delay
                while time.time() < deadline:
                    ask(sock, f"screen {tmp}")
                    err = _palette_error(tmp, a.ground)
                    if err is not None and (best is None or err < best):
                        best = err
                        shutil.copyfile(tmp, a.dump_screen)
                    if err is not None and err < 1.0:
                        break
                    time.sleep(1.5)
                try:
                    os.unlink(tmp)
                except OSError:
                    pass
                print(f"[play] kept the screen whose palette is closest to "
                      f"the level's (mean error {best})")
            else:
                time.sleep(a.dump_delay)
            print("[play] --- video state in the level ---")
            print(ask(sock, "vga"))
            print(ask(sock, "status"))
            if a.dump_planes:
                print(ask(sock, f"planes {a.dump_planes}"))
            # NOT another `screen` here: the loop above already chose
            # the best sample, and dumping again overwrote it with
            # whatever happened to be on screen a moment later. That is
            # how a carefully chosen capture became a mid-fade one with
            # the BIOS default EGA palette still in DAC 0x10-0x17.
            for spec in a.dump_mem:
                print(ask(sock, f"dump {spec}"))
            for c in a.cmd:
                if c.startswith("sleep "):
                    # Local, not a socket verb: a breakpoint needs time to
                    # be reached before `regs` is worth asking for.
                    time.sleep(float(c.split()[1]))
                    continue
                print(f"[play] > {c}")
                print(ask(sock, c))
        proc.wait()
    except KeyboardInterrupt:
        pass
    finally:
        if proc.poll() is None:
            try:
                ask(sock, "quit")
                proc.wait(timeout=10)
            except Exception:
                proc.kill()
        if os.path.exists(sock):
            os.unlink(sock)
    return proc.returncode


if __name__ == "__main__":
    sys.exit(main() or 0)
