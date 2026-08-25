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
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lemmings


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
    proc = subprocess.Popen(argv)
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
            if a.dump_vga:
                # Gameplay is mode 0Dh again, after the 10h title.
                wait_for(sock, lambda st: "mode=0x0d" in st,
                         "the level", timeout=180)
                time.sleep(a.start_delay)
                print("[play] --- video state in the level ---")
                print(ask(sock, "vga"))
                print(ask(sock, "status"))
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
