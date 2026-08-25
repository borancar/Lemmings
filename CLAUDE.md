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

**The readable image is already built and committed.** Work from it; there is
no need to decrypt anything again:

```sh
uv run python tools/disasm.py image/vgalemmi.decrypted.img 0xdff -n 40
```

`image/vgalemmi.decrypted.img` is the unpacked image with the protected code
written back in the clear - 651 bytes across nine spans, which is the whole of
what the protection was hiding. See [image/README.md](image/README.md).

To rebuild it, or to grow it after reaching a new screen:

```sh
uv run python tools/decrypt_image.py                 # merge image/rec, ~1s
uv run python tools/decrypt_image.py --record        # re-record from the game
```

Three things about this are easy to get wrong, and each cost time here:

- **Only code that *ran* can be recovered.** Coverage is the union of the runs
  merged, which is why records are kept per run and merged rather than one run
  producing the image. Reach a new screen, record it, merge it in.
- **An ordinary run executes none of the protection.** The game installs the
  trap handler only when INT 21h AH=1Bh says *floppy*: media `0xF8` gives 0
  `INT 01h` events, `0xF9` gives 2945. `decrypt_image.py` records a deliberate
  `media=0xF9` run for this. A run that never touched the protection tells you
  nothing about it - and will not say so.
- **Bytes differing from the dump are not all protection.** Where the image
  held *zeros*, that is BSS the program filled in at run time - the AdLib
  driver at `0x18F00..0x1949F`, for one. The report keeps the two apart;
  conflating them makes the protection look several times larger than it is.

## Compare against what the original composes, not what it displays

The strongest check here is not a screenshot. A screen carries sprites, a
cursor, a palette fade, a scroll offset and a viewport, and every one of them
is a difference that is not the difference you are looking for - a comparison
against one sat at 81% for several rounds with nine rejected hypotheses about
why.

The game builds its level in video memory before showing it. Break at the
instruction *after* the loop that builds it - image `0x376A`, the exact moment
it is complete - dump the buffer, and compare that. Same port, same data:
**100.00%, 49 741 terrain pixels, zero differing.**

Timing that capture matters and fails silently: a dump taken a second early
catches composition mid-flight and looks exactly like a port drawing terrain
the original does not.

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
| `tools/decrypt_image.py` | builds `image/vgalemmi.decrypted.img` - records each instruction's plaintext as it executes and writes it back. Merges per-run records, so coverage only grows |
| `tools/disasm.py` | disassemble the decrypted image at an image offset |
| `tools/dat.py` | Lemmings DAT decompression, the Python oracle the C port was checked against |
| `tools/snapshot.py` | capture memory at a chosen point in a run |
| `tools/probe_protected.py` | record the protected instruction stream as it runs |
| `tools/probe_desync.py` | find a single-step desynchronisation, if the decryptor ever loses step |
| `tools/probe_int13.py`, `tools/probe_exit.py`, `tools/probe_derail.py` | what the disk check asks for, where the program quits, how it derails |
| `tools/reference_dosbox.sh` | capture DOSBox's screens as a reference (mounts a copy) |
| **`tools/prove.sh`** | **the proof, in one command**: drive the original to the end of its terrain composition, dump the level it built, render the same level with the port, compare |
| `tools/compare_composed.py` | port versus the original's own composed level, as indices |
| `tools/verify_data.py` | the port's file loading versus the original's memory |
| `tools/compare_level.py` | port versus the original's *screen* (weaker: sprites, cursor, fade) |
| `tools/play.py` | run the game; `--auto` answers the start-up menus from the guest's own cue |
| `tools/trace_stub.py` | instruction trace with registers, for reading a packer stub |
| `tools/find_write.py` | first guest write into a linear range, and the instruction that did it |
| `tools/stub_dump.py` | run to instruction N, then registers and a disassembly around PC |
| `tools/mem_dump.py` | run to instruction N, then dump a linear range to a file |
