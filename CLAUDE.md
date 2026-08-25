# PC Lemmings — working notes

How to work on this reconstruction. [STATUS.md](STATUS.md) is where the port has
got to and what is *proven*; `docs/` is what the program *is*.

## What this is

A reverse engineering of **PC Lemmings (VGA)** — `lemming1.pc/vgalemmi.exe`,
the 10-level demo build dated 1996-12-24 in a cracked repackaging — checked
against the original running under
[`dos_emulator`](https://github.com/borancar/dos_emulator).

**VGA only.** The game shipped one executable per adapter (`cgalemmi.exe`,
`tgalemmi.exe`, and `vgalemmi.exe` for both EGA and VGA), dispatched by
`LEMMINGS.BAT` through `AUTODET.EXE`. Only the VGA one is being reconstructed;
the others are deliberate non-goals, not gaps. Mode 13h is chunky, so a blit
transcribes as array writes and a frame check is a plain byte compare.

## Running it

```sh
uv sync
uv run python tools/play.py --scale 3      # a window to play in
uv run python tools/play.py --auto         # answer the start-up menus
```

`tools/lemmings.py` is the **one place this project names the game** — its
directory, its executable, its load segment. Every tool imports the emulator
*through* it, so when the shared code moves there is one file to fix.

## The load segment is not the default — this matters

`VGALEMMI.EXE` is packed with PKLITE, and its stub computes a source segment as
`psp + 0x834`, then subtracts `0x1000` from it with `sub bh,0x10`. The emulator
loads at `PSP_SEG = 0x100` by default, which is far lower than any real DOS
(DOS, its buffers and drivers sit below the first free block, so a real PSP is
0x800–0x2000). At 0x100 that subtraction **borrows below zero**: DS becomes
0xF934, `DS:SI` addresses past the 1 MB mark instead of the compressed data,
the stub decompresses zeros, and its relocation walker runs off the end of
memory writing into the video window. It looks like a hundred different bugs.

So this project loads at `PSP_SEG = 0x1000` (`lemmings.PSP_SEG`, or
`--psp-seg 0x1000` on the emulator's own CLI). **Anything that builds a machine
by hand must pass it**, or it will be measuring garbage.

## Do not re-run the decryption

The executable is PKLITE-packed *and* the protection decrypts itself one
instruction at a time under the trap flag, re-encrypting behind itself. Neither
the file on disk nor a plain memory dump is the program.

Build the readable image **once** and work from it:

```sh
uv run python tools/decrypt_image.py     # -> out/vgalemmi.decrypted.img
uv run python tools/disasm.py 0xdff -n 40
```

`decrypt_image.py` records the bytes of every instruction at the moment it
executes and writes them back into the dump, so the protected stretch is
plaintext too. Only code that *ran* can be recovered that way - that is
inherent. The sidecar `.json` says how many bytes differed from the raw dump
and in which spans, which is exactly what the protection was hiding; today
that is 499 bytes across `0x0066d..0x0096f` and `0x00dff..0x00e39`.

The count rises as the game gets further: it currently exits during start-up
(see STATUS.md), so most of the game has never executed and is only as
readable as the raw dump makes it.

## Conventions

- **`stdint` types only** in the reconstruction — `uint8_t`, `uint16_t`,
  `int16_t`, `uint32_t`. Never `unsigned`, `unsigned char`, `short`, `long` or a
  bare `int`. `char` stays `char` for real strings (paths, `printf`).
  This is not style. It is 16-bit assembly, where every value has a width the
  original depended on: `int16_t` says *this truncation is the `imul`'s* in a
  way `short` does not, and `unsigned` hides whether a thing is a byte, a word,
  or a register holding one. The widths are usually identical on a modern ABI,
  so getting it wrong compiles, runs, and silently loses the one fact the type
  was carrying.
- **Addresses are image offsets** unless written `seg:off`. Image offset 0 is
  the first byte after the EXE header — of the *unpacked* image once we have
  one, and `docs/` says which.
- **Every transcribed routine carries the address it came from**, as a comment,
  so any line can be argued back to a byte in the binary.
- **Where a name or a type is a guess, say so.** A confident wrong name costs
  more than an honest `sub_1558`.
- The port is **structured C that reads as a game**, checked against the
  emulator rather than against the screen.
- Deliberate non-goals live in the port as **no-ops with a comment saying why**,
  and stay out of the verifier's dispatch — comparing a no-op against the
  original reports a decision as a difference.

## The emulator is shared, not ours

`dos_emulator` is used by other games' reconstructions, and their verification
sweeps are the only thing that would catch a regression there. So:

- **Subclass, don't fork.** `LemmingsVga` in `tools/lemmings.py` is where
  game-specific behaviour goes. If that means copying an upstream file to change
  three lines, upstream is missing an extension point — add the hook *there*.
- Changes upstream must be **additive**: new optional parameters whose defaults
  preserve today's behaviour exactly. `--psp-seg` was added that way.
- `pyproject.toml` currently points at the local checkout for development.
  **Pin it to a commit before publishing any measurement** — a coverage figure
  or a frame rate is only reproducible if you can name the instrument.

## Tools

| | |
| --- | --- |
| `tools/lemmings.py` | the adapter: game dir, executable, load segment, start-up keys, `LemmingsVga` |
| `tools/extract_image.py` | write the unpacked image once, with a sha256 and the load segment beside it |
| `tools/decrypt_image.py` | **the one to use** - records each instruction's plaintext as it executes and writes it back, so the protected code is readable too |
| `tools/disasm.py` | disassemble the decrypted image at an image offset |
| `tools/dat.py` | Lemmings DAT decompression, the Python oracle the C port was checked against |
| `tools/snapshot.py` | capture memory at a chosen point in a run |
| `tools/probe_protected.py` | record the protected instruction stream as it runs |
| `tools/probe_desync.py` | find a single-step desynchronisation, if the decryptor ever loses step |
| `tools/probe_int13.py`, `tools/probe_exit.py`, `tools/probe_derail.py` | what the disk check asks for, where the program quits, how it derails |
| `tools/reference_dosbox.sh` | capture DOSBox's screens as a reference (mounts a copy) |
| `tools/play.py` | run the game; `--auto` answers the start-up menus from the guest's own cue |
| `tools/trace_stub.py` | instruction trace with registers, for reading a packer stub |
| `tools/find_write.py` | first guest write into a linear range, and the instruction that did it |
| `tools/stub_dump.py` | run to instruction N, then registers and a disassembly around PC |
| `tools/mem_dump.py` | run to instruction N, then dump a linear range to a file |
