# SPDX-License-Identifier: GPL-2.0-only
"""The one place this project names its own game.

Every tool here imports the emulator *through* this module rather than
importing `dos_emulator` directly, so when the shared code moves underneath
there is one file to fix instead of nine.
"""
import os
import sys

from dos_emulator import VgaDos, set_game_dir, main as _emu_main

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


def make_machine(args):
    """Build the machine from parsed CLI args, for `main(make_machine=...)`."""
    return LemmingsVga(args.program, blaster=args.blaster, max_insns=1 << 62,
                       cmdline=args.cmdline,
                       psp_seg=args.psp_seg if args.psp_seg is not None
                       else PSP_SEG)


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
