# SPDX-License-Identifier: GPL-2.0-only
"""The one place this project names its own game.

Every tool here imports the emulator *through* this module rather than
importing `dos_emulator` directly, so when the shared code moves underneath
there is one file to fix instead of nine.
"""
import os
import re
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


def game_lib():
    """The port, as a shared library, for a verifier to call routine by routine.

    THROUGH THE MAKEFILE, not a cc line written out here. Eight tools each had
    their own, naming game.c and sdl_io.c, so every source file the port has
    gained since broke all eight at once - data.c did exactly that, and the
    failure arrived as `undefined symbol: CRATER` from ctypes rather than as
    anything about the port. The list of sources belongs in one place and the
    Makefile already is that place; this asks it for `libgame.so`.

    It also CHECKS THE BUILD SUCCEEDED, which is CLAUDE.md's rule and the
    reason a tool must not compile the port itself and carry on: a `make` whose
    output went to /dev/null once produced four identical measurements from a
    binary that had never been built.
    """
    import ctypes
    import subprocess
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    rec = os.path.join(root, "reconstruct")
    subprocess.run(["make", "-C", rec, "libgame.so"], check=True,
                   stdout=subprocess.DEVNULL)
    return ctypes.CDLL(os.path.join(rec, "libgame.so"))


def lem_struct(lib):
    """`lemming` from game.h, declared ONCE and checked against the C.

    Two tools each wrote their own and one of them stopped at `state` -
    sixteen bytes where the record is thirty-four. ctypes then laid the array
    out at the wrong stride, the C read every lemming after the first out of
    the middle of its neighbour, and verify_hit reported 47 of 256 hits
    differing. That number was about the declaration; the port scored the
    identical 553/47 at a commit from before any of the work it appeared to
    indict, which is what showed it.

    The assert is the point. A field added to game.h and not to here now
    fails loudly rather than producing a plausible measurement.
    """
    import ctypes

    # DERIVED FROM dgroup.h, NEVER RESTATED HERE.
    #
    # This used to be a hand-written _fields_ list, and CLAUDE.md already
    # records what that cost once: a ctypes struct is a SECOND declaration of
    # the record, ctypes lays out its own idea of it, and every lemming after
    # the first is then read out of the middle of its neighbour. That produced
    # 553 of 600 frames "agreeing" and read as a fault in level_lemming_at.
    #
    # It happened AGAIN the moment the record moved into dgroup.h at the
    # original's own 0x2D layout: the list here still described the port's old
    # 34-byte one, in a different field ORDER, and tools/verify_skills.py came
    # back with 345 of 400 frames differing for six different skills - the
    # same number three times over, which is an instrument constant and not a
    # measurement. The size guard below caught it, and the guard is the only
    # reason it was not written up as eight broken skills.
    #
    # So the layout is read out of the header's own ENSURE_LEM_AT asserts,
    # which is where the offsets are already proved at compile time. A field
    # added to game.h and not here can no longer produce a plausible number.
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    hdr = os.path.join(root, "reconstruct", "src", "dgroup.h")
    with open(hdr) as f:
        src = f.read()
    offs = {m.group(1): int(m.group(2), 16)
            for m in re.finditer(r"ENSURE_LEM_AT\((\w+),\s*0x([0-9A-Fa-f]+)\)", src)}
    if not offs:
        raise SystemExit(f"{hdr}: no ENSURE_LEM_AT - the record's offsets are "
                         "not where this expects them, and guessing them is "
                         "how the last two of these went wrong")
    # The widths, from the header's own declarations.
    body = src[src.index("typedef struct {"):src.index("} __attribute__((packed)) lemming;")]
    TYPES = {"int16_t": ctypes.c_int16, "uint16_t": ctypes.c_uint16,
             "int8_t": ctypes.c_int8, "uint8_t": ctypes.c_uint8}
    width = {}
    for m in re.finditer(r"^\s*(u?int\d+_t)\s+(\w+)\s*(\[(\d+)\])?\s*;",
                         body, re.M):
        t, name, _, n = m.groups()
        width[name] = (TYPES[t], int(n) if n else 0)

    fields, at = [], 0
    for name in sorted(offs, key=lambda k: offs[k]):
        off = offs[name]
        if off < at:
            raise SystemExit(f"lemming field {name} at {off:#x} overlaps "
                             f"what came before it, ending {at:#x}")
        if off > at:
            fields.append((f"_gap_{at:02x}", ctypes.c_uint8 * (off - at)))
        ct, n = width[name]
        fields.append((name, ct * n if n else ct))
        at = off + ctypes.sizeof(ct) * (n or 1)

    want = lib.lemming_record_size()
    if at < want:
        fields.append((f"_gap_{at:02x}", ctypes.c_uint8 * (want - at)))

    class Lem(ctypes.Structure):
        _pack_ = 1
        _fields_ = fields

    got = ctypes.sizeof(Lem)
    if got != want:
        raise SystemExit(
            f"the lemming record is {want} bytes in the port and {got} here.\n"
            "  Nothing measured through this struct means anything until the\n"
            "  two agree - see lem_struct in tools/lemmings.py.")
    return Lem

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
# LEM_MACHINE overrides the second answer, so any tool can record or probe a
# machine type other than High Performance without its own flag. play.py has
# had --machine for a while; record_level.py had no way to ask at all, which
# is why every reference capture in this repository is machine 2 and the
# compat path went unmeasured.
STARTUP_KEYS = ("1", os.environ.get("LEM_MACHINE", "2"))


class LemmingsVga(VgaDos):
    """PC Lemmings (VGA) on the shared machine.

    Game-specific behaviour - routine names on the control socket, sync
    points, the read cache - lands here as it is found, so that
    `dos_emulator` stays free of anything that mentions Lemmings.
    """

    fs_note = ("host filesystem READ-ONLY; writes intercepted in memory "
               "(RUSSELL.DAT is never touched)")

    def __init__(self, *a, **kw):
        # The game does not take the display's timing on trust - it measures
        # it, at image 0x1602, and divides the answer into its whole clock.
        # So these two numbers *are* the frame rate, and the emulator's
        # defaults are a CGA's.
        #
        # A VGA refreshes its 200-line modes at 70 Hz, and scans 449 lines to
        # do it, which is 31.469 kHz across the line. The game counts 320 of
        # those line transitions on port 0x3DA bit 0 at image 0x1622 - 320 and
        # not 200, because a VGA doubles the lines of a 200-line mode, and the
        # adapter probe at image 0x1462 asks the BIOS which it is on. 320
        # lines at 31.469 kHz is 10.17 ms, which is 12,135 PIT counts and a
        # 98.3 Hz timer tick. Measured here: 12,279, within 1.2%, and steady
        # from run to run where it used to be noise.
        kw.setdefault("vsync_hz", 70.0)
        kw.setdefault("hsync_hz", 31469.0)
        super().__init__(*a, **kw)
        self._seed_config()
        self._opl_init()

    # WHILE A SCRIPT DRIVES, THE POINTER IS THE SCRIPT'S AND NOBODY ELSE'S.
    #
    # The start-up driver in run() clicks at (160, 100) to get through the
    # title and the briefing, and those clicks go through click_mouse like any
    # other. One of them lands after the level's capture has begun: the
    # DGROUP comparison came back with ds:0x10 and ds:0x14 differing on
    # exactly ONE frame of forty, at (160, 100) with the button down, and that
    # reads as the port mishandling a click rather than as the harness still
    # pressing the one it used to open the level.
    #
    # tools/record_level.py re-asserting the position every frame is not
    # enough, because the capture and the re-assert are different hooks at
    # different addresses: the stray write lands between them and survives for
    # exactly the one frame that gets dumped.
    #
    # So pin it. While pinned, only script_mouse() moves the pointer - the
    # start-up clicker, the control socket and any host event write nothing.
    # That includes INT 33h AX=4, which the game never calls (the emulator's
    # own mouse_calls[4] stays at 2, both from start-up, which is what settled
    # an earlier round of this); if it ever does, this will hold the pointer
    # still and that is a difference the comparison will show rather than hide.
    #
    # tools/play.py never pins - there the pointer is a person's.
    _mouse_pos = (160, 100)
    _mouse_btn = 0
    _mouse_pinned = False
    _mouse_from_script = False

    def pin_mouse(self, on=True):
        self._mouse_pinned = on

    def script_mouse(self, pos, down, right=False):
        """Move the pointer AS THE SCRIPT, past the pin. INT 33h numbers the
        buttons bit 0 left and bit 1 right."""
        self._mouse_from_script = True
        try:
            self.mouse_pos = tuple(pos)
            self.mouse_btn = (1 if down else 0) | (2 if right else 0)
        finally:
            self._mouse_from_script = False

    @property
    def mouse_pos(self):
        return self._mouse_pos

    @mouse_pos.setter
    def mouse_pos(self, v):
        if self._mouse_pinned and not self._mouse_from_script:
            return
        self._mouse_pos = v

    @property
    def mouse_btn(self):
        return self._mouse_btn

    @mouse_btn.setter
    def mouse_btn(self, v):
        if self._mouse_pinned and not self._mouse_from_script:
            return
        self._mouse_btn = v

    # THE TIMER CAN BE HELD, so the original runs a fast machine's frame.
    #
    # Image 0x03E6F - the keys, the panel, the release rate - runs on every
    # timer tick, and the emulator delivers ticks by the HOST's wall clock.
    # So how many of a frame's five ticks land in the middle of its work, and
    # how many in the wait after it, is decided by how fast the host is going:
    # a one-frame click on the release rate's plus made +2, +3, +4 or +5 in
    # one run here depending on how busy the frame was.
    #
    # tools/record_level.py and tools/lockstep.py set `ticks_held` from the
    # frame's start (image 0x018F) to its end (image 0x00105), and the ticks
    # then all fall in the wait - which is what the port's level_wait_ticks
    # runs. `ticks_after_keys` holds them too while a key is still queued, so
    # a key pressed at the frame's end is in ds:0x1FD0 before the first tick.
    #
    # Nothing is lost: the emulator never catches up on missed ticks, so a
    # held tick is simply the next one, delivered as soon as it is let go.
    ticks_held = False
    ticks_after_keys = False

    def service_timer(self):
        if self.ticks_held or (self.ticks_after_keys and self.scan_queue):
            return False
        return super().service_timer()

    # The game's config file starts every run from the SHIPPED copy.
    #
    # RUSSELL.DAT is a working file the game rewrites, and its state decides
    # what happens at start-up. The packaging knows this: LEMMI.BAT is three
    # lines, and two of them are `attrib -h russell.dat` and `copy
    # russell.var russell.dat`. RUSSELL.VAR is the pristine one.
    #
    # It matters because a drifted RUSSELL.DAT makes the game take its floppy
    # path and quit with "Lemmings Disk 1 Not found, Insert into drive A or B
    # and retry" - the branch at image 0x0062B - before it ever reaches a
    # screen. That is exactly what happened here: something ran the game
    # outside this emulator, which does NOT intercept the write, and every
    # run afterwards failed at the title with no capture. Restoring the file
    # by hand fixed it, which is not a fix anyone will remember next time.
    #
    # Seeding the overlay does what LEMMI.BAT does without touching the host
    # directory: the guest's first open of RUSSELL.DAT is served from memory
    # with RUSSELL.VAR's bytes, and its writes stay in the overlay as before.
    # So the read-only guarantee is not weakened - it is what makes this the
    # right place for it.
    def _seed_config(self):
        var = os.path.join(GAME_DIR, "russell.var")
        try:
            with open(var, "rb") as f:
                self.overlay["RUSSELL.DAT"] = bytearray(f.read())
        except OSError:
            pass          # no pristine copy: leave the host file to be read

    # ---------------------------------------------------------- the AdLib
    #
    # An OPL2 (YM3812) is TWO ports and 244 write-only registers: write the
    # register number to 0x388, the value to 0x389.  Reading 0x388 gives the
    # status byte, and that read is the whole reason this exists.
    #
    # `adlib.dat`'s detect at driver +0x00A3 is the AdLib manual's own:
    #
    #     04=0x60   mask both timers
    #     04=0x80   reset the IRQ flags
    #     read      -> must mask to 0x00
    #     02=0xFF   timer 1 preset
    #     04=0x21   start timer 1 (bit 0), timer 2 masked (bit 5)
    #     loop 0x12C
    #     read      -> must mask to 0xC0
    #
    # The shared emulator answers 0x00 to both reads, so the second test fails
    # and image 0x0059F concludes NO CARD - which is why every run of this
    # game has been silent, and why nothing about the sound path has ever
    # executed.  This makes it answer.
    #
    # Timer 1 expires IMMEDIATELY here rather than after its real 80 us.  The
    # driver's own delay is 300 `loop` iterations, about a millisecond on a
    # 4.77 MHz 8088, so it has always elapsed by the time the status is read -
    # modelling the duration would change nothing and hide the assumption.
    #
    # Nothing here makes a sound.  It records the register stream, which is
    # the reference the port's transcribed driver has to reproduce - the
    # sound equivalent of prove.sh's composed buffer.
    OPL_ADDR, OPL_DATA = 0x388, 0x389

    def _opl_init(self):
        self.opl_regs = bytearray(256)
        self.opl_index = 0
        self.opl_status = 0x00
        self.opl_log = []            # (n, reg, val) in the order written
        self.opl_detected = False
        self.opl_on_write = None

    def _opl_write_reg(self, reg, val):
        # Stamped with the guest's own PIT tick, not with a sequence number.
        #
        # A register write only means something at a MOMENT: the same stream
        # replayed with the writes evenly spaced is not the tune, it is the
        # notes in the right order at the wrong times. The game drives the
        # driver from IRQ 0 and programs the divisor itself, so `timer_ticks`
        # counts the driver's own calls and `pit0_div` says what one is worth
        # in seconds - PIT_HZ / div, which probe_pace.py reports as 82.7 Hz
        # during start-up and 39.3 Hz inside a level.
        #
        # Wall clock would be the emulator's throughput and not the game's,
        # which CLAUDE.md already records as the wrong instrument for pace.
        self.opl_regs[reg] = val
        self.opl_log.append((self.timer_ticks, self.pit0_div or 0x10000,
                             reg, val))
        # A probe that wants to act once the driver is running hooks HERE
        # rather than on every instruction. A UC_HOOK_CODE callback costs the
        # emulator so much throughput that the same run reached 40 register
        # writes instead of 10,109 - the instrument stopped the guest getting
        # anywhere near the thing being measured, and reported that as the
        # tune never starting.
        if self.opl_on_write is not None:
            self.opl_on_write(self)
        if reg == 0x04:                       # timer control
            if val & 0x80:                    # IRQ RESET clears both flags
                self.opl_status = 0x00
            elif val & 0x01 and not val & 0x40:
                # timer 1 started and not masked: bit 7 (IRQ) + bit 6 (T1)
                self.opl_status = 0xC0
                self.opl_detected = True

    def _on_out(self, uc, port, size, value, user):
        if port == self.OPL_ADDR:
            self.opl_index = value & 0xFF
            return
        if port == self.OPL_DATA:
            self._opl_write_reg(self.opl_index, value & 0xFF)
            return
        return super()._on_out(uc, port, size, value, user)

    def _on_in(self, uc, port, size, user):
        if port == self.OPL_ADDR:
            return self.opl_status
        if port == self.OPL_DATA:
            return 0x00
        return super()._on_in(uc, port, size, user)


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


# The game's own INT 08h handler at image 0x17C7 reprograms the PIT on every
# tick and the main loop at image 0x0164 counts five ticks to a frame, so the
# frame rate *is* the tick rate over five. A tick can only be delivered when
# the emulator looks at the timer, which it does once per slice; at the default
# chunk of 20,000 instructions and 800,000 ips that is 40 looks a second, and
# the 82.7 Hz the game asks for during start-up cannot fit into it. Eight
# slices give 320 looks a second, comfortably above anything the game programs.
TIMER_SLICES = 8


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
    if not any(a == "--timer-slices" or a.startswith("--timer-slices=")
               for a in argv):
        # The game drives everything from IRQ 0 and programs the PIT for
        # itself - 39.3 Hz inside a level, 82.7 Hz during start-up - so the
        # timer has to be looked at far more often than once a chunk or it
        # simply ticks at the chunk rate and the game runs slow. See
        # TIMER_SLICES.
        argv += ["--timer-slices", str(TIMER_SLICES)]
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
