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

# Image offset where the game's own code segment begins. Unknown until the
# unpacked image has been mapped; see docs/.
CODE_BASE = 0


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
