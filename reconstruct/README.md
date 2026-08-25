# PC Lemmings, reconstructed

A reimplementation in C of **PC Lemmings (VGA)** — DMA Design / Psygnosis,
1991 — reconstructed by reverse engineering the original executable and its
data files.

![Level 1, "This should be a doddle!"](docs/level1.png)

```sh
make
./lemmings --data /path/to/lemmings          # level 1 -> level.bmp
./lemmings --data /path/to/lemmings --level 3
```

## What it needs

A C11 compiler and nothing else — no libraries, no image codecs. It reads
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

The level that comes out reads `This should be a doddle!`, with 80 lemmings,
save 40, release rate 50 and ten of each skill, which is the level as
published.

What has **not** happened yet is the check that matters most: comparing this
against the original binary running under an emulator, routine by routine.
Until then this is a plausible reimplementation, not a proven one. The
development repository holds the emulator, the verification harness and the
disassembly notes.

## Where it deviates from the original, deliberately

- **VGA only.** The original shipped separate CGA, Tandy and EGA/VGA
  executables; only the VGA one is reconstructed.
- **Machine type "High Performance PCs".** The original's start-up menu offers
  four; that is the one that enables no compatibility work-arounds, and the
  other three are not implemented.
- **No copy protection.** The original decrypts itself under the trap flag and
  checks for an original floppy. None of that is reproduced.
- **Output is a file, not a screen.** There is no display loop yet.

## Licence

The **reconstruction** — everything in this directory — is GPL-2.0-only.

**Lemmings itself is not included and is not covered by that.** It remains the
property of its rights holders; this code reads data files you must already
have. No game asset, level or executable is redistributed here.
