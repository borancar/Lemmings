# SPDX-License-Identifier: GPL-2.0-only
"""The one place this project names its own game.

Every tool here imports the emulator *through* this module rather than
importing `dos_emulator` directly, so when the shared code moves underneath
there is one file to fix instead of nine.
"""
import os
import sys
import time

# HEADLESS BY DEFAULT. Every tool here drives the guest automatically - proofs,
# verifiers, probes, the image recorder - and each one used to open a real
# window on whoever's desktop happened to be running it. Nothing reads those
# windows; the tools capture through the control socket.
#
# This must be set BEFORE dos_emulator imports pygame, which is why it sits
# above the import rather than in a function. Ask for a window with
# LEM_WINDOW=1, or `tools/play.py --window`, which is the one tool that is for
# looking at.
if not os.environ.get("LEM_WINDOW"):
    os.environ.setdefault("SDL_VIDEODRIVER", "dummy")
    os.environ.setdefault("SDL_AUDIODRIVER", "dummy")

from dos_emulator import VgaDos, set_game_dir, main as _emu_main, KEYMAP

# ---------------------------------------------------------------- the game
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME_DIR = os.path.join(REPO, "lemming1.pc")

# Lemmings shipped one executable per display adapter, dispatched by
# LEMMINGS.BAT through AUTODET.EXE. This port is of the VGA one; see
# STATUS.md for why the others are deliberate non-goals.
EXE = os.path.join(GAME_DIR, "vgalemmi.exe")
EXE_CGA = os.path.join(GAME_DIR, "cgalemmi.exe")
EXE_TANDY = os.path.join(GAME_DIR, "tgalemmi.exe")

# Where the program is loaded. NOT the emulator's 0x100 default: VGALEMMI.EXE
# is packed with PKLITE, and its stub computes a source segment as
# `psp + 0x834` and then subtracts 0x1000 from it (`sub bh,0x10`). At 0x100
# that borrows below zero, DS becomes 0xF934, and DS:SI addresses past the 1 MB
# mark instead of the compressed data - the stub then decompresses zeros and
# its relocation walker runs off the end of memory. 0x1000 is what a real DOS
# with a couple of drivers loaded would give, and the subtraction is ordinary.
PSP_SEG = 0x1000

# Image offset where the game's own code segment begins. The code segment and
# the image base coincide, so a cs: offset is an image offset.
CODE_BASE = 0

# Image offset of DGROUP. A `ds:XXXX` reference in the disassembly is image
# offset DGROUP + XXXX; getting this wrong turns strings into garbage.
DGROUP = 0x09A90

# The start-up answers this port is of. VGALEMMI.EXE asks two questions before
# it touches the display: the adapter, then the machine type.
#
# '1' = VGA. '2' = "High Performance PCs", which is the machine type this port
# targets - and it is the one that sets *no* compatibility flags at all (see
# docs/machine-types.md). PC compatible, PS/2 and Amstrad each switch on a
# work-around; High Performance is the plain path, so it is both the best
# target and the smallest surface to prove.
#
# Both are digits, and the menu code confirms on a digit rather than waiting
# for RETURN (image 0x1159: `mov bl,1` then `jmp` straight to the dispatch),
# so no confirm key is needed.
STARTUP_KEYS = ("1", "2")


class LemmingsVga(VgaDos):
    """PC Lemmings (VGA) on the shared machine.

    Nothing overridden yet. Game-specific behaviour - routine names on the
    control socket, sync points, the read cache - lands here as it is found,
    so that `dos_emulator` stays free of anything that mentions Lemmings.
    """

    fs_note = ("host filesystem READ-ONLY; writes intercepted in memory "
               "(RUSSELL.DAT is never touched)")


REC_MAGIC = b"LEMREC01"


def write_records(path, seen):
    """Write `image offset -> instruction bytes` as a mergeable sidecar.

    A run only recovers the code it actually executed, so no single run is
    ever complete. Recording the raw (offset, bytes) pairs rather than a
    patched image means several runs - a menu run, a level run, a run that
    reaches the results screen - can be merged into one image without either
    guessing which of two memory dumps is authoritative for a data byte.

    Format: b"LEMREC01", u32 count, then count records of
    u32 image offset, u8 length, `length` bytes.
    """
    import struct
    with open(path, "wb") as f:
        f.write(REC_MAGIC)
        f.write(struct.pack("<I", len(seen)))
        for off in sorted(seen):
            b = seen[off]
            f.write(struct.pack("<IB", off, len(b)))
            f.write(b)


def read_records(path):
    """Read a sidecar written by write_records(); returns {offset: bytes}."""
    import struct
    with open(path, "rb") as f:
        blob = f.read()
    if blob[:8] != REC_MAGIC:
        raise ValueError(f"{path}: not a {REC_MAGIC.decode()} record file")
    (count,) = struct.unpack_from("<I", blob, 8)
    seen, pos = {}, 12
    for _ in range(count):
        off, n = struct.unpack_from("<IB", blob, pos)
        pos += 5
        seen[off] = blob[pos:pos + n]
        pos += n
    return seen


class RecordingLemmingsVga(LemmingsVga):
    """Records the bytes of every instruction as it executes.

    The protection keeps one instruction in plaintext at a time and
    re-encrypts behind it, so the only way to read that code is to catch each
    instruction at the moment it runs. This is that, wired into the ordinary
    run so it can be driven by tools/play.py - which is the only driver that
    reliably gets past the menus, the protection and the briefing screen.
    Recording in-process with a hand-rolled driver reached the title screen
    and stopped, which recovers a fraction of what a real run does.
    """

    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self.seen = {}
        base = self.load_seg * 16
        top = base + 0x20000
        read = self.uc.mem_read
        seen = self.seen

        def code(uc, addr, size, _):
            if base <= addr < top:
                off = addr - base
                if off not in seen:
                    try:
                        seen[off] = bytes(read(addr, size))
                    except Exception:
                        pass

        from unicorn import UC_HOOK_CODE
        self.uc.hook_add(UC_HOOK_CODE, code)


def make_machine(args):
    """Build the machine from parsed CLI args, for `main(make_machine=...)`.

    LEM_DECRYPT_OUT in the environment turns on instruction recording and
    writes a permanently decrypted image there when the run ends.
    """
    cls = LemmingsVga
    out = os.environ.get("LEM_DECRYPT_OUT")
    if out:
        cls = RecordingLemmingsVga
    m = cls(args.program, blaster=args.blaster, max_insns=1 << 62,
            cmdline=args.cmdline,
            psp_seg=args.psp_seg if args.psp_seg is not None else PSP_SEG)
    hook = os.environ.get("LEM_HOOK")
    if hook:
        # A verifier needs to ride along with tools/play.py, because that is
        # the only driver that reaches a level - an in-process run answers the
        # start-up menus and stops. LEM_HOOK names a file with an install(m)
        # function; it is called once, with the machine, before anything runs.
        import importlib.util
        spec = importlib.util.spec_from_file_location("lem_hook", hook)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        mod.install(m)
        print(f"  [hook] installed {hook}")

    if out:
        import atexit

        def dump():
            os.makedirs(os.path.dirname(os.path.abspath(out)) or ".",
                        exist_ok=True)
            write_records(out, m.seen)
            print(f"  [decrypt] recorded {len(m.seen)} distinct instructions "
                  f"-> {out}")

        atexit.register(dump)
    return m


def run(argv=None):
    """The emulator's CLI, defaulted to this game."""
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0].startswith("-"):
        argv.insert(0, EXE)
    if not any(a == "--game-dir" or a.startswith("--game-dir=") for a in argv):
        argv += ["--game-dir", GAME_DIR]
    set_game_dir(GAME_DIR)
    return _emu_main(argv, make_machine=make_machine)


if __name__ == "__main__":
    run()


# ---------------------------------------------------------------- driving
# Reaching the level takes the whole start-up chain, and every probe needs it,
# so it lives here rather than being written out again in each one.
#
# PACING MATTERS. The emulator paces its timer interrupt on the wall clock so
# an interactive run goes at roughly the speed the game was written for. An
# in-process driver that runs flat out makes wall clock and guest time come
# apart, the timer barely fires, and the front end - which is driven entirely
# by that interrupt - never advances. Probes that skipped this sat on the
# title screen forever and looked like the game was broken.
CHUNK = 120_000
IPS_8086 = 8_000_000


def drive_to_level(m, on_chunk=None, chunks=6000, settle=400, verbose=True):
    """Run `m` from reset into the first level. Returns the chunk it started.

    `on_chunk(m, r, in_level)` is called after each chunk, for a probe that
    wants to record only once gameplay has begun.
    """
    import pygame
    keys = [getattr(pygame, f"K_{k}") for k in STARTUP_KEYS]
    addr, sent = m.start, 0
    saw_title = f1 = clicks = 0
    down_at = None
    playing = None
    budget = time.perf_counter()

    for r in range(chunks):
        m.blocked_on_input = False
        try:
            m.uc.emu_start(addr, 0, count=CHUNK)
        except Exception as e:
            if verbose:
                print("cpu:", e)
            return playing
        addr = m._reg(_CS) * 16 + m._reg(_IP)
        m.service_keyboard()
        m.service_timer()
        if m.finished:
            if verbose:
                print("exited:", m.finished)
            return playing

        # Hold to roughly 8086 speed, so the wall-clock timer fires at about
        # the rate the guest expects. A slice that stopped early waiting for
        # input ran almost nothing and is not charged.
        if not m.blocked_on_input:
            budget += CHUNK / IPS_8086
            spare = budget - time.perf_counter()
            if spare > 0.0005:
                time.sleep(spare)
            elif spare < -0.25:
                budget = time.perf_counter()

        if m.blocked_on_input and sent < len(keys):
            sc, asc = KEYMAP[keys[sent]]
            m.press_key(sc, asc, True)
            m.press_key(sc, asc, False)
            sent += 1
            continue

        if down_at is not None and r >= down_at + 8:
            m.click_mouse(0, None, None, down=False)
            down_at = None

        if m.mode == 0x10:
            saw_title += 1
            if playing is None:
                if saw_title > 900 and f1 < 4 and saw_title % 500 == 0:
                    sc, asc = KEYMAP[getattr(pygame, "K_F1")]
                    m.press_key(sc, asc, True)
                    m.press_key(sc, asc, False)
                    f1 += 1
                    if verbose:
                        print(f"  chunk {r}: F1 ({f1})")
                elif f1 >= 1 and clicks < 6 and saw_title % 600 == 0 \
                        and down_at is None:
                    m.click_mouse(0, 160, 100, down=True)
                    down_at = r
                    clicks += 1
                    if verbose:
                        print(f"  chunk {r}: click ({clicks})")
        elif m.mode == 0x0D and saw_title > 500 and playing is None:
            playing = r
            if verbose:
                print(f"  chunk {r}: in the level")

        if on_chunk:
            on_chunk(m, r, playing is not None)
        if playing is not None and r - playing > settle:
            return playing
    return playing


from unicorn.x86_const import UC_X86_REG_CS as _CS, UC_X86_REG_IP as _IP
