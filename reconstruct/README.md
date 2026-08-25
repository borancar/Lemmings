# PC Lemmings, reconstructed

A reimplementation in C of **PC Lemmings (VGA)** — DMA Design / Psygnosis,
1991 — reconstructed by reverse engineering the original executable and its
data files.

![Level 1, "Just dig!"](docs/level1.png)

```sh
make
./lemmings --data /path/to/lemmings --show   # a window on level 1
./lemmings --data /path/to/lemmings          # or just write level.bmp
./lemmings --data /path/to/lemmings --file level000.dat --level 3
```

**The game's level order is not its file order.** What the original calls
"Level 1" is section 1 of `level009.dat`, not the first level of the first
file - established by running the original under an emulator and reading its
own briefing screen ("Just dig!", 10 lemmings, 10% to be saved, release rate
50, 5 minutes, diggers only), then confirming this parser gives exactly those
numbers. That pair is the default. What maps a level number onto a file and a
section has not been traced yet.

Arrow keys scroll (hold Shift to go faster), Home and End jump to the ends,
Esc quits.

![The level in a window](docs/window.png)

## What it needs

A C11 compiler and nothing else. **X11 is optional**: with it, `--show` opens
a window; without it the port still renders to a file. No image codecs, no
game engine, no other libraries. It reads
**your own copy of the original data files**: `level000.dat`, `ground0o.dat`
and `vgagr0.dat` for the first level. Nothing of the game is included here.

`--data` is the directory holding them. The output is a 24-bit BMP, because
that needs no dependencies; convert it with anything.

## What it does so far

Loads a level and draws its terrain:

- **DAT decompression** — the container format all of Lemmings' data uses
- **Level parsing** — globals, terrain, objects, steel areas, the name
- **GROUNDxO.DAT** — the per-set index of bitmaps and the 16-colour palette
- **VGAGRx.DAT** — 4-bit planar bitmaps
- **Terrain composition** — drawing order with the no-overwrite, erase and
  upside-down flags
- **Interactive objects** — the entrance and the exit, with their own
  transparency masks and animation frames

There is no game yet: no lemmings, no skills, no animation, no sound.

## How it is known to be right

**Its data loading is proven byte for byte.** The development repository has a
check that takes the bytes the *original* has in RAM after loading a file and
compares them against what this code produces from the same file on disk:
**179 618 bytes across seven sections, byte-identical, none differing** — the
level, both `VGAGR0.DAT` sections including the terrain bitmaps, three sections
of `MAIN.DAT`, and the ground file. Two programs, one input, an exact answer.

**Its terrain rendering is proven byte for byte.** The comparison is not
against the original's *screen* — that carries sprites, a cursor, a palette
fade and a viewport, none of which is the thing being checked. It is against
the level the original **composes in memory**, captured at the exact
instruction its terrain loop finishes:

> **100.00%. 49 741 terrain pixels, zero differing**, across the whole
> 1584x160 level.

That one number covers the DAT decompressor, the level parse, the ground
table, the planar bitmap decode, piece placement, the flags, drawing order and
overlap resolution.


Not yet as thoroughly as it should be, and this section will say so honestly
until that changes.

The **file formats** here were written from published descriptions and then
checked against the real files rather than trusted — the published notes are
wrong or ambiguous in three places, all recorded in the source comments:

- the DAT header's words are **big**-endian, not little
- DAT bits come out of a byte LSB-first while multi-bit values assemble
  MSB-first; no other combination decodes even one level
- the terrain entry's bit packing was settled by decoding all 168 pieces of
  level 1 both plausible ways and keeping the reading where every piece lands
  in range and names a bitmap the set defines (168/168 against 82/168)

**One real check has happened.** The original was run under an emulator as far
as its level briefing screen, which states the level's name and parameters in
the game's own words. This parser, given the same section, reports the same
six values - name, lemming count, percentage to save, release rate, time limit
and the single skill. That is independent confirmation of the level globals,
not a reading of the same document twice.

What has **not** happened is the check that matters most: comparing this
renderer's output against the original's video memory, pixel for pixel, and
its routines against the original's, one at a time. Until then this is a
plausible reimplementation, not a proven one. The development repository holds
the emulator, the verification harness and the disassembly notes.

## Where it deviates from the original, deliberately

- **VGA only.** The original shipped separate CGA, Tandy and EGA/VGA
  executables; only the VGA one is reconstructed.
- **Machine type "High Performance PCs".** The original's start-up menu offers
  four; that is the one that enables no compatibility work-arounds, and the
  other three are not implemented.
- **No copy protection.** The original decrypts itself under the trap flag and
  checks for an original floppy. None of that is reproduced.
- **The window is a viewer, not the game's display loop.** The original draws
  a 320x160 viewport with a control panel below it and paces on the vertical
  retrace; this just shows the composed level and scrolls it.

## Licence

The **reconstruction** — everything in this directory — is GPL-2.0-only.

**Lemmings itself is not included and is not covered by that.** It remains the
property of its rights holders; this code reads data files you must already
have. No game asset, level or executable is redistributed here.
