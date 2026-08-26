# STATUS — PC Lemmings (VGA) reconstruction

Last updated: **2026-08-25**

**Which world this is in: hand-written assembly** — settled, not assumed. The
whole 131,072-byte image contains **zero** `push bp / mov bp,sp` prologues and
**zero** cdecl caller-side cleanups, and carries no compiler banner or runtime
library fingerprint. A byte-exact *matching* reconstruction is therefore not
meaningfully available; **behavioural equivalence through differential
verification is the standard**, and it is the one the terrain result below
meets. Evidence and how to re-run it: [docs/toolchain.md](docs/toolchain.md).

**Which configuration.** VGA, and machine type **"High Performance PCs"** —
the one start-up option that sets no compatibility flags, so it is the plain
path rather than a work-around for some machine's quirks. PC compatible, PS/2
and Amstrad are deliberate non-goals; see
[docs/machine-types.md](docs/machine-types.md).

**Which binary.** `vgalemmi.exe` — the VGA build. `cgalemmi.exe` and
`tgalemmi.exe` are **deliberate non-goals**: the game logic is the same in all
three and only presentation differs, so a second variant would re-derive the
level format and the main loop for no gain. `vgalemmi.exe` also serves EGA
(menu option 2); only its VGA path is in scope.

## Where it stands, at a glance

Everything below is stated as what was *checked*. These are the headline
numbers, re-measured on the current build:

| | |
| --- | --- |
| **the game's own screen**, all 5,100 frames of the expiration timer | play area **exactly 28 differing — the mouse cursor — on 5,100 of 5,100**; panel exact on 5,099 (frame 0 is the fade-in). This is the WINDOW path, what a player sees, not the harness (`tools/compare_screen.py`) |
| the original's composed **level terrain** vs the port | **100.00%** — 49,741 pixels, 0 differing (`tools/prove.sh`) |
| the port's **data loading** vs the original's RAM | **100.00%** — 179,618 bytes across 7 sections |
| the **level-code** screen | **100.00%** — 0 of 224,000 |
| the **input chooser** | **100.00%** — 0 of 224,000 |
| the **briefing** | **100.00%** — 0 of 224,000: background, text, palette, and the level preview |
| the **main menu** | **100.00%** — zero pixels differing, at all four ratings, **with one of the walking lemmings on screen** in three of the four. Background, logo, all six plates, both drop shadows, the rating name, the FX label, the credits, the whole scrolling banner, its two end lemmings, and the animation |
| the DAT decompressor vs the original's own body | 7 calls, 150,807 bytes, byte-identical |
| the LVL header parse | 15 fields, all agreeing |
| the sprite pre-shift vs the original's own body | 200 calls, byte-identical |
| the level sprite pre-builder vs the original's own body | 106 calls, byte-identical |
| tests | `make test` 82 checks across two binaries, `make sanitize` clean under ASan + UBSan |

**Transcription is a different claim from any of the above, and it is 33 of
211 call targets** — counted by `reconstruct/tests/provenance.py`, which reads
the comment immediately above every function and fails while any of them says
neither an address nor an explicit "not transcribed". It runs in `make test`. The port renders these screens from the file formats and from
geometry measured against the original — that is real evidence, and it is not
the same as having transcribed the routines. See *Coverage* below.

Reference material lives in `docs/`: [menu.md](docs/menu.md) for the menu
screens, [protection.md](docs/protection.md), [toolchain.md](docs/toolchain.md),
[codemap.md](docs/codemap.md), [machine-types.md](docs/machine-types.md).

## Done

Each of these is stated as what was *checked*, not what was written.

- **The original runs under the emulator, and reaches its start-up menus.**
  Verified by capture: `mode 0x03` text, the "PC Lemmings Machine Type
  Selection Screen", and after a `1` keypress the four-way machine-type menu.
- **The PKLITE stub's failure was diagnosed to an instruction, not guessed.**
  `sub bh,0x10` at `19a9:0029` (stub-relative) borrowed below zero at the
  emulator's default load segment, giving `DS = 0xF934` and a `DS:SI` past 1 MB.
  Confirmed by instruction trace with registers, and by a write-hook that
  caught the relocation walker looping on `add word ptr es:[di], bx` at
  `0x0A1060`.
- **The emulator loads the image byte-exactly.** Memory at the load address
  compared against the file image (32 669 bytes past the 80-byte header) — an
  exact match, which is what ruled the loader out before the CPU was suspected.
- **Loading at a realistic segment fixes decompression.** At `psp_seg=0x1000`
  the unpacked image contains plausible 16-bit code (register save/restore
  runs) where at 0x100 it contained no readable code at all, and the program
  goes on to run, set a video mode and poll for input — where before it issued
  **zero** interrupts.

- **The start-up path runs**: both machine-type menus cleared, BIOS mode 0Dh
  set, and `main.dat` and `adlib.dat` opened. Verified by run log, not by eye.
- **The derailment inside the protection is fixed, and diagnosed to a cause
  rather than patched around.** Two BIOS gaps: `INT 1Eh` was zero, so the
  protection's write to the diskette parameter table's bytes-per-sector field
  landed on `0000:0003` and corrupted the `0000:0000` pointer its own decryptor
  uses; and `INT 13h` was unhandled, leaving carry as the caller set it, so 24
  failed sector reads all looked like successes. Both fixed upstream. The
  protection now runs to completion and restores its own vectors.
- **The protection is characterised, and measured rather than assumed** — see
  [docs/protection.md](docs/protection.md). `IVT[01h]` = image 0x00DFF;
  `EFLAGS = 0x0146` (TF set) at handler entry with no `int 1` in the stream;
  108 single-steps in a whole start-up run.
- **The original reaches its main menu under DOSBox** — captured in
  `docs/reference/dosbox-main-menu.png`. So `vgalemmi.exe` runs standalone, its
  protection is satisfiable, and any failure here is ours.
- **The original now runs under our own emulator too**, through both start-up
  menus, past the copy protection, to the title screen and on to the level
  briefing — `docs/reference/emulator-title-screen.png` and
  `docs/reference/original-level1-briefing.png`. Six emulator defects were in
  the way and each was diagnosed to an instruction rather than guessed: the
  load segment, the BIOS CRTC-base variable, an interrupt gate that left TF
  set, the missing INT 1Eh diskette parameter table, an unhandled INT 13h that
  reported failure as success, and an unhandled INT 21h AH=1Bh. All are fixed
  upstream and additive.
- **The copy protection is understood end to end and defeated by being
  emulated correctly, not patched.** It runs, checks, and passes: the disk
  check only fires when a run counter in `RUSSELL.DAT` wraps to zero, one run
  in ten, and the file ships holding 8.
- **The program image is extracted and permanently decrypted**, with a sha256
  and its load segment recorded beside it — `tools/decrypt_image.py`. Nothing
  has to re-run the PKLITE stub or the protection to read the code.
- **A C port draws the game's Level 1**, and its reading of that level agrees
  with the original's own briefing screen on all six stated values.
- **THE PORT'S DATA LOADING IS PROVEN, BYTE FOR BYTE.**
  `tools/verify_data.py` takes the bytes the **original** has in RAM after
  loading a file and compares them against what the port produces from the same
  file on disk. Two programs, one input, an exact answer. Against a memory dump
  taken while the game was loading level 1:

  | section | bytes | result |
  | --- | --- | --- |
  | `level009.dat` section 1 (the level) | 2 048 | **exact** |
  | `vgagr0.dat` section 0 (terrain bitmaps) | 30 008 | **exact** |
  | `vgagr0.dat` section 1 (object bitmaps) | 47 700 | **exact** |
  | `main.dat` section 3 | 61 968 | **exact** |
  | `main.dat` section 4 | 36 080 | **exact** |
  | `main.dat` section 5 | 758 | **exact** |
  | `ground0o.dat` (uncompressed) | 1 056 | **exact** |

  **179 618 bytes, 0 differing.** That settles the DAT decompressor - written
  from a published description and corrected against the bytes in three places
  - and the ground-file reader, and it means every *input* to the terrain
  renderer is the same as the original's. A section reported "not resident" is
  one the game had already freed; only a section found *and* differing would be
  a failure, and there are none.

  The check is careful about one thing worth naming: it scores **every**
  candidate location, not the first. A 64-byte run of a sprite sheet really
  does occur inside the video aperture where the game blitted it, and taking
  the first hit reported two of `main.dat`'s sections as 40% wrong when they
  had merely been found in the wrong place.
- **THE PORT'S TERRAIN IS PROVEN BYTE-FOR-BYTE.**
  `tools/compare_composed.py` compares the port's canvas against the level the
  **original itself composes**, captured by breaking at the instruction after
  its terrain loop — the exact moment composition is complete. No sprites, no
  cursor, no palette, no viewport, no fade: two programs' terrain, as 4-bit
  indices, over the whole 1584x160 level.

  **100.00%. 49 741 terrain pixels, zero differing.**

  That covers the DAT decompressor, the level parse, the ground table, the
  planar bitmap decode, piece placement, the flags, drawing order and overlap
  resolution — every one of them exact, against the original's own output.

- ~~**THE PIXEL COMPARISON RUNS**, and the port is 81% right, with the rest
  unexplained after eleven rejected hypotheses.~~ **Retracted 2026-08-25.**
  The port was right all along. The difference was in **our emulator**, which
  did not implement VGA read modes: Lemmings' blitter reads video memory in
  read mode 1 and turns the answer into the Graphics Controller's bit mask —
  that read *is* its "do not overwrite" test. Handed flat memory instead, the
  game composed a level with exactly the right shape and the wrong colours in
  14 883 pixels, and every one of those was attributed to the port. Fixing the
  emulator changed exactly those 14 883 pixels and nothing else.

  This gap had been *written down and argued away*, on the grounds that the
  blitter discarded the value it read. It did not. That mistake cost several
  rounds of testing hypotheses about the port while the reference was the
  thing that was wrong.

- **The protected code is recovered and committed, and rebuilds byte-identically
  from records without running the game.** `image/vgalemmi.decrypted.img` is the
  unpacked image with 651 bytes of plaintext written back over nine spans — the
  whole of what the protection was hiding. Checked, not assumed: the recovered
  handler at image `0x0DFF` disassembles as the rolling-XOR decryptor
  (`not`/`rol`/`xor`), the re-encrypting `xchg word ptr [si], ax` at `0x0E14`,
  and the `cmp al, 0xcc` anti-debugger test — matching, byte for byte, what
  `docs/protection.md` had transcribed by hand from live single-stepping.
  Rebuilt twice from `image/rec` to the same SHA-256.
- **The protection runs only on the floppy path, shown by a differential run.**
  Identical in every respect but the media descriptor byte INT 21h AH=1Bh
  reports: `0xF8` (fixed disk) gives **0** `INT 01h` events and `IVT[01h]`
  stays null all run; `0xF9` (floppy) gives **2945**. This also explains, after
  the fact, why the game used to quit before the menu — it was taking the
  floppy branch by accident because the emulator did not answer AH=1Bh.

- **The main menu is composed, and checked against the original's own screen.**
  The menu runs in EGA mode 10h — 640×350, four planes — a different mode from
  the game itself. `tools/verify_menu.py` drives the original to it, dumps that
  screen and compares: **95.04% identical over all 224,000 pixels, with nothing
  excluded from the count**, and all 16 palette entries byte for byte. Verified piece by piece: the background
  is a 320×104 **two-plane** tile (four colours) repeating from (0,0) — proven
  by measurement, since assembling it from four planes scores 39% and forcing
  planes 2 and 3 to zero scores 95.6%; the logo is 632×94 at (8,10), keyed on
  index 0; and each of the six signs matched its own slot at 86.7–95.4% against
  39–78% for every other slot, a clean diagonal.
- **The reconstruction shows a screen by default and uses SDL3.** `./lemmings`
  with no arguments opens the main menu; `--out` or `--raw` asks for a file.
  The X11 viewer is gone — see the convention in CLAUDE.md for why a
  platform-specific display layer is not acceptable here.

- **The port is one translation unit, and every function says where it came
  from.** It was eight files split by modern instinct — `dat.c`, `level.c`,
  `ground.c`, `vgagr.c`, `render.c`, `menu.c` — a division nothing in the
  binary supports. Compilers of the period emitted one object per source file
  and the linker laid each module into the segment whole and in link order, so
  a contiguous run of addresses **is** one original source file. The port is
  now `game.c` + `game.h`, with `sdl_io.c` and `main.c` as boundaries the port
  chose and whose headers say so. Every function carries the image offset it
  was transcribed from, or an explicit note that it was not transcribed and
  where it lives is unknown.
- **No licence is claimed on reconstructed code.** The original's licence is
  unknown, and a file transcribed out of someone else's binary is not ours to
  licence. The `SPDX-License-Identifier: GPL-2.0-only` headers that were on
  every reconstructed file asserted exactly that, machine-readably, and are
  gone; a provenance header naming the binary is there instead. **The tools in
  `tools/` still carry that SPDX line** — they are our own work so a licence
  there is legitimate, but GPL-2.0 was picked by us without being asked for,
  and the sibling reconstructions use MIT. Worth settling.
- **The menu answers to its keys and to the mouse.** F1 starts the level and
  coming out of it returns to the menu; Esc and the window close exit.
  `reconstruct/tests/menu_input.c` checks all six plates by key and by click,
  and five points that must *not* be buttons — 18 checks, run by `make test`.

- **`tools/prove.sh` now checks that it captured what it thinks it did.** It
  arms a breakpoint at image `0x0376A` and then sleeps; if the guest has not
  actually stopped there when the dump is taken, the dump is of whatever was in
  video memory. That happened during this work and reported **83.65%** — which
  reads as a port that lost a third of its terrain, not as a failed capture.
  The port's output was byte-identical to the previous commit's, which is what
  identified it. The harness now asserts on `where`, retries once, and fails
  loudly rather than comparing against a bad capture.

- **The level briefing is drawn, and its text is verified.** The original
  shows it between the menu and the level and the port now does too:
  menu → briefing → level → menu. The strings were not guessed — they were read
  back off the original's own briefing by matching each 16×16 cell against the
  font's glyph masks, which gives `'Level 1        Just dig!'`,
  `'Number of Lemmings 10'`, `'10%  To Be Saved'` and the rest character for
  character. The templates they are built from are transcribed with their
  addresses (`0x0E59D`, `0x0E5CD`, `0x0E5E7`, `0x0E5FB`, `0x0E60E`, `0x0E620`,
  `0x0E633`) and the numbers written into the runs of spaces the original
  leaves for exactly that. **97.99% of text pixels.**

- **The screens are clean under AddressSanitizer and UBSan, leak detection
  on.** `make sanitize` builds every screen three times over — menu, briefing,
  level-code, input chooser, level canvas — and frees each one, because the
  menu loop does exactly that for as long as someone plays and *nothing else
  here ran it more than once*. Each verifier builds one screen and exits, which
  is the shape of run that never sees a leak or a double free. It also frees
  twice deliberately, to pin that `menu_free` and `dat_free` are idempotent.
  All clean. `make test` runs it alongside the input test.

- **Correction: this is not a demo at all — it is the full game.** The order
  table has **120 entries**, four ratings of thirty, and every one resolves to
  a real level. The earlier correction below got the count right (eighty
  *files*' worth of unique levels) and the conclusion wrong: forty of those are
  used by two ratings each, which is how 80 stored levels present as 120.
- **Correction: this is not a ten-level demo.** The project described it that
  way from early on, in `CLAUDE.md` and in the port's own header. It is a
  misreading of "ten level *files*": `level000.dat` .. `level009.dat` each hold
  **eight** 2048-byte levels — **eighty** — with names from across the full
  game's ratings (`Tightrope City`, `Mary Poppins\` land`, `Every Lemming for
  himself!!!`). Counted with `tools/dat.py`. It surfaced only because a
  *different* claim leaned on it — "the rating plate does nothing because there
  is only one rating here" — and that reasoning had to be checked before it
  could be written down. The plate turned out to work anyway; see the rating
  entry below.

- **The FX plate's label is drawn, and section 3 is now fully accounted for.**
  Its lettering is not in the plate's bitmap — the plate is blank because the
  label swaps. Section 3's last **1,984 bytes** are two 64×31 four-plane
  sprites, 992 bytes each; sprite 1 is "FX" and sprite 0 is its alternate. At
  **(351,146)** it matches **100.00%** — 1,654 ink pixels, none differing.

  The pixel that mattered: sweeping positions in steps of 8 gives a
  convincing-looking 92.74%, and it is wrong. The label is drawn one pixel off
  byte alignment. A sweep whose step size matches the data's natural alignment
  will confidently find the wrong answer.

- **The background is 632 pixels wide, not 640 — and finding that made a whole
  screen exact.** The original leaves the last byte of each row black: no
  non-black pixel anywhere beyond x=635, while the left edge draws normally.
  The port was tiling the full 640.

  | screen | before | after |
  | --- | --- | --- |
  | the **input chooser** | 99.37% | **100.00%** |
  | the level-code screen | 98.82% | **99.45%** |
  | the menu | 95.21% | **95.83%** |

  It was found by *mapping* the residual rather than assuming it: every 16-row
  band of the menu's differences had a few pixels at x 632–639, which is a
  systematic error wearing the costume of noise. About 1,400 pixels spread
  thinly down one edge look like animation until they are counted by band.

- **SOLVED: the black halo was a DROP SHADOW — another sprite, drawn first.**
  Every model fitted to the pixels was wrong because the compositing was never
  wrong. Capturing the EGA bit mask the blitter actually uses shows it is
  **exactly the sprite's own ink, 100.00% agreement** — keying on index 0 is
  precisely what the original does.

  Attributing every write into the logo's rows to the instruction that made it
  found a *second* masked blitter, image `0x097F4`, writing **7,426 bytes** —
  the logo's exact size — into rows 20..113 while the logo occupies 10..103.
  Ten rows down and eight pixels left, in black. The plates do the same, from
  image `0x09128`: rows 128..188 against their 120..180, and one byte left.

  | | before | after |
  | --- | --- | --- |
  | the menu, whole screen | 95.83% | **99.27%**, and 99.41% once the rating name was found |

  A second finding came out of the same pass: **the FX plate's label is drawn
  opaquely**, not keyed. Its own black is real — keying it out leaves the
  plate's yellow where the original has black around the letters. That follows
  from what the label is *for*: it swaps between FX and its alternate, so
  drawing one has to erase the other, and a keyed blit cannot. 94.04% keyed
  against 98.79% opaque over its neighbourhood.

  What is left, **as last measured**, is 1,321 pixels of 224,000:

  | where | pixels | what |
  | --- | --- | --- |
  | the banner's rows | 995 | its scrolling text and end lemmings — animated, so a composed frame cannot match them |
  | inside the plates | 317 | unexplained; static, spread 16–113 a plate |
  | elsewhere | 9 | |

  The rating name is drawn *over* the plate, not baked into it — the plate
  carries a "FUN" that the original then overdraws one pixel to the left, which
  is why the two nearly coincide and differ along every edge. It has to be
  drawn separately, because the rating changes; the four names are a string in
  the executable at image `0x0E691`.

  It is **not** the menu's 16×16 font: overlaying `"Fun"`, `"FUN"` and `"Fun   "`
  from that font at every position over the plate is *worse* than leaving it
  alone (76.56% against 83.45%).

  **FOUND, and drawn.** `MAIN.DAT` section 4, offset **26084**: 72×27, four
  planes of 243 bytes, at screen **(486,145)**, drawn **opaquely** — the same
  reason the FX label is, since the rating swaps and drawing one has to erase
  the last. Fitted at pixel granularity: **100.00% over 1,760 ink pixels, none
  differing.** The four names follow at a stride of 972; only rating 0 is
  verified against a capture, and `game.c` says so.

  It sits *below* the font's nominal base, which is why looking "in the font"
  never found it: the font is indexed from 23888 so ASCII lands correctly, but
  no printable character reaches below `23888 + 32×96`, and the ratings live in
  that unused space.

  | | before | after |
  | --- | --- | --- |
  | the menu, whole screen | 99.27% | **99.41%** |
  | of which outside the animated banner | 630 px | **326 px** |

- **The level-code screen is now exact too, at 100.00%.** All 1,230 of its
  differing pixels were one thing: the dots are an **input field**, and the
  original blacks its whole strip out rather than letting the background show
  between them. The title above it is keyed normally, so the screen needed both
  behaviours and had only one.

  **How it was found, since three earlier attempts failed:** The writer is image `0x09387`,
  and the routine around it at `0x09360` **pre-shifts** its sprite: it copies
  the artwork into a scratch buffer and shifts the whole buffer right one bit
  at a time (`shr` then `rcr` down the bytes, repeated for each bit) before
  blitting it byte-aligned. So every blit's source address points at the
  *scratch buffer*, not at the artwork — which is why following blit sources
  keeps landing on anonymous memory. Finding the rating's sprite means finding
  what is copied *into* that buffer, one step further back.

  It is also the general answer to how this game draws anything at a
  non-byte-aligned x — the FX label sits at x=351, which is 7 mod 8.

  **Following the fill one step back locates the artwork's file.** The
  instruction that fills the scratch buffer is image `0x09352`, and its sources
  land in pages `0x67600`–`0x67900` and `0x6DF00`–`0x6E300`. Probing those
  against every data file puts both inside **`MAIN.DAT` section 4**, loaded at
  linear `0x679F0` — the two probes agree on that base independently, which is
  the check.

  Section 4 is 36,080 bytes and only its tail is understood: the 16×16 font
  occupies `23888`–`36080`. **Its first 23,888 bytes have never been looked
  at**, and the rating name is in there. Rendering that head as a 4-plane image
  at 64 and 96 pixels wide gives noise, so it is not one big bitmap; it is
  probably several sprites with their own dimensions, the way section 3 turned
  out to be.

  That is the next thread, worth about 630 pixels, and it is now a question
  about one known region of one known file.

  Two things were tried and rejected on measurement rather than taste: a drop
  shadow for the label (worse — 99.16% to 99.06%), and a one-pixel black
  outline (95.55% against opaque's 98.79%).

  The shadows use the sprite's own silhouette as a stand-in; the original reads
  a separate buffer (page `0x5F000`), so this is a **fit, not a
  transcription**, and it is marked as one in `game.c`.

  The lesson is the expensive one: **three rounds of fitting models to pixels,
  then one register trace and one write-attribution.** When a port and the
  original disagree about compositing, ask what the original's blitter did —
  not what rule would explain the picture.

- **The plates are keyed, and that is now measured rather than assumed.**
  Drawn as they are they differ by **317 pixels across all six**; drawn opaquely
  they would differ by 7,532. That is essentially the whole of the menu's
  non-banner residual, spread 16–113 pixels per plate, and it is static — those
  rows do not move between captures.

  Nothing is drawn *over* a plate: attributing every write into the "?" plate's
  box finds only the screen clear, the background, the plate, and its shadow.
  So the 317 is not a missing overlay.

  A hypothesis was raised and then **refuted by measuring it**, which is the
  outcome worth recording. The plates do go through the pre-shift buffer —
  they are read from `0x20200`, they sit at x values that are 4 mod 8, and
  their blits span 16 columns for a 15-byte sprite. So the port's per-pixel
  blit from unshifted artwork might differ at the edges.

  Simulating the shift — `shr`/`rcr` down a 16-byte buffer, exactly as image
  `0x09360` does — does **not** explain the residual. Over the three plates
  with nothing drawn on top of them it is better by 21 pixels in total
  (75 against 96); over all six it is much worse, though that count is
  confounded by the FX and rating labels. Twenty-one pixels is not a mechanism,
  and it does not justify a second blit path.

  So the 317 remains **unexplained**: static, spread thinly, inside plates
  whose ink is otherwise exact.
- **A stale `--raw` file has now cost three measurements**, most recently a
  plate comparison that read 5,528 differing pixels where the truth was 40.
  Each time the file looked valid and was simply from an earlier build.
  `tools/menu_diff.py` regenerates before it reads and offers no way to skip
  that; ad-hoc analysis scripts should not be written for this any more.

## Open

- ~~**The pixel comparison now runs, and the port is 79.99% right.**~~
  **Superseded 2026-08-25** — the terrain comparison is now **100.00%**, 49,741
  pixels with zero differing, reproducible with `tools/prove.sh`. The 79.99%
  was measured against the original's *screen*; comparing against what it
  *composes* is what closed the gap. Kept because the intermediate number was
  reported and should not vanish.
- ~~**NOTHING IS VERIFIED PIXEL FOR PIXEL YET, and it is blocked.**~~
  **Retracted 2026-08-25** - the block was the emulator's own terrain, which
  now renders. Original text follows for the record: The
  comparison harness exists — `tools/capture_level.py` captures the original's
  screen, `tools/compare_level.py` slides the port's canvas across it and
  reports the best alignment and the differing pixel count, and the port emits
  raw indices with `--raw` for exactly this. But the original's own terrain
  does not render yet (planar reads, above), so there is nothing to compare
  against. **Fixing that gap upstream is the single thing standing between
  this project and a real proof**, and it wants doing in bulk — syncing a
  plane into flat memory when the read map select changes — because per-access
  correction was measured and is far too slow.
- **`tools/capture_level.py` forces a timer tick per chunk**, because an
  in-process driver runs the guest far faster than the wall clock the timer is
  paced on. That changes how fast the game thinks time passes. Fine for
  capturing a picture; **not** fine for anything measuring timing.
- **The port is a menu and a level renderer, not a game.** No lemmings, no
  skills, no animation, no sound, no control panel.
- **What each menu plate does was settled by asking the original, and four of
  the six now work.** `tools/probe_menu_keys.py` drives a *fresh* guest to the
  menu per key, presses one key, and diffs the screen against a no-key
  baseline:

  | plate | what the original does | the port |
  | --- | --- | --- |
  | F1 "1 PLAYER" | fades out into the level | starts the level, returns here after |
  | F2 "NEW LEVEL" | an **"Enter Code"** screen | drawn, **98.82%** |
  | F3 "FX" | **nothing** — measured, only the scroller moves | a faithful no-op |
  | F4 "?" | a **"Choose Input Type"** screen | drawn, **99.37%** |
  | Esc "EXIT TO DOS" | quits | quits |
  | the rating plate | Up / `` ` `` raise the rating, Down / `-` / keypad `-` lower it; caps at Mayhem, floors at Fun, and sets the level to rating×30 | all four keys, capped and floored the same way |

  What the two screens *do* with the answer is not reconstructed: the port
  draws them and comes back. Neither excludes anything from its score.

  Getting that table took three wrong runs, and each error is one this project
  had already written down. A capture taken mid-fade made F3 look like it
  redrew 77,433 pixels. A zero-duration key press is invisible to a guest that
  polls the key state, so the first clean run said **F1** does nothing — F1
  being the one key already known to work is what caught it. And the menu drops
  keys pressed while it is still fading in, so the press has to be repeated.
  The probe now does all three: fresh guest, held press, repeated, and it
  refuses to compare a capture whose palette is black.
- **Every object in every level was drawn 16 pixels too far right**, and the
  briefing's preview is what caught it.

  Boran pointed out that the preview shows the entrance and the exit, not just
  terrain. The commit before this one had claimed "terrain only" on the
  strength of an **aggregate score** — drawing the objects made the preview
  worse, 187 differing against 167 — without ever asking *where* the
  differences were. They fell in two clusters, at level (708..747, 36..47) and
  (860..887, 108..131); the level's three objects are at (720,36), (864,116)
  and (864,108). They were in the preview all along.

  Looking at the pixels rather than the total: the port's entrance was
  **exactly right and exactly four preview cells — sixteen level pixels — to
  the right.** Terrain x in the LVL is stored `+16`, and `level_parse` subtracts
  it. Object x is stored the same way and it did not.

  | object x | preview differing |
  | --- | --- |
  | as-is | 187 |
  | −12 | 163 |
  | **−16** | **114** |
  | −20 | 163 |

  A clear minimum, not a fit. It had survived because **nothing ever compared
  objects against anything**: `tools/prove.sh` passes `--no-objects`
  deliberately, since the original never bakes them into its composition. The
  preview is the first thing that put them beside the original.

  Two lessons, and the first is the one that cost the round: **an aggregate
  score is a poor way to decide whether a feature belongs on a screen** — it
  said "objects make it worse", which was true, and "so there are no objects",
  which was false. And **a routine no comparison covers will hold a plain bug
  indefinitely**; `render_objects` places every object in the game and had
  never once been checked.

  The briefing is now **99.95%** whole-screen, 114 of 224,000, all of it in the
  preview.

- **Rows 0–77 are the level's own terrain and objects at QUARTER SCALE — the
  level preview.** The palette said so and it took saying out loud to hear it:
  the array taken from `GROUNDxO.DAT` offset `0x408` is the file's **`preview`**
  palette. What it is the preview *of* was never asked.

  | | |
  | --- | --- |
  | the preview's ink box | 127 × 39 |
  | the composed terrain's | 510 × 160 |
  | ratio | **4.016** and **4.103** |

  Sampling pixel (2,2) of each 4×4 block, drawn at **(120, 19)**, 396×40. That
  sample point is not a guess: over the sixteen positions in the block it
  scores 167 differing against 1,739–2,097 for every other one.

  This had been hunted as a stored bitmap, and then as a 1:1 region of the
  level — which scored **71.4%** and read as "not a match". It was the level
  all along, at a scale nobody had tried. **A negative result from a search is
  only as good as the transforms the search covered**, and "same pixels,
  different size" was not in mine.

  **The preview is exact — 0 differing.** Getting the last 114 took one look
  at a picture. They had been described here, twice, as "almost all at piece
  edges, scattered"; `tools/diff_png.py` drew them as **a single horizontal
  magenta line**. Every one was in row 19, and every one was "original black,
  port has terrain": the preview starts at row **20**, not 19, and shows level
  rows 4..159 rather than 0..159. One constant.

  A percentage cannot tell you what is wrong. It said 114 and it said 99.95%,
  both true, and the shape of those 114 - which is the whole of the answer -
  was invisible in the number and obvious in the image.

- **The briefing's background is the menu's own tile, tiled from row 78.**
  It had been recorded as *"not the menu's tile, and in neither MAIN.DAT nor
  VGAGR0.DAT"* — searched for as a stored bitmap, at all eight bit alignments,
  and not found.

  `tools/probe_blits.py --driven` settles it by logging what the original
  blits rather than searching for pixels: three pairs at y = **78**, **182**
  and **286**, from `srcseg 0x587E` offset 0 — `MAIN.DAT` section 3's start,
  the menu's own background — 320×104, two planes, exactly as the menu tiles
  it.

  Over rows 78–349, `x < 632`, background-coloured pixels: **100.00%, 0 of
  156,720.** Tiled from row 0, as the menu does, the same comparison gives
  **44%**. A 78-row offset was the whole of it.

  With the preview palette as well, the briefing is now **100.00% over rows
  78–349** — background, text and palette together, 0 of 174,080 — and 98.64%
  over the whole screen. Rows 0–77 are black with the level's preview art over
  them, about 3,000 pixels, and that preview is the only thing left here.

  Same lesson as the plates: **a search for a stored bitmap cannot find a
  bitmap that is stored but drawn somewhere unexpected.** Logging the blits
  finds it at once, because the original says where it puts things. That is
  now twice in one session that `probe_blits.py` has closed something a pixel
  search had given up on.

- **The briefing's palette is the level's PREVIEW palette, and it is now
  exact — all 16 entries.** This had been open as "entries 8-15 are the
  level's terrain colours, not the menu's, and come from somewhere that has
  not been found".

  They come from `GROUNDxO.DAT`. The file carries **three** upper-eight
  palettes at `0x3D8`, `0x3F0` and `0x408` — custom, standard and preview —
  and the briefing uses the **preview** one, which is exactly what its name
  says for a screen that previews a level.

  Using `palette[8..15]`, which is the *custom* set the level itself is drawn
  in, got **seven of the eight** and left one entry wrong: entry 8's red,
  `0x38` against `0x34`, 227 against 211. One byte. That is the kind of
  near-miss that reads as a rounding error in the 6-bit widening rather than
  as the wrong array, and it is worth recording as such — the fix was not to
  adjust anything, it was to notice that a different array existed.

  Verified against a fresh capture of the original's own briefing: **palette
  entries differing: 0**, text 100.00%.

  What is still open on this screen is only the **background**, which is in
  neither `MAIN.DAT` nor `VGAGR0.DAT`, and which the port leaves black rather
  than drawing something plausible and wrong.

- **The main menu is EXACT. 100.00%, zero pixels differing, at all four
  ratings.**

  The residue had sat at 326 for many rounds, came down to 257 by fitting, and
  went to zero the moment the blitter was actually read. The sprite path at
  image `0x0890E` is **four passes over the planes, and they do not do the
  same thing**:

  | pass | what it does |
  | --- | --- |
  | 0 | Enable Set/Reset = `0x0E`, so planes 1..3 take Set/Reset = 0 and plane 0 takes the CPU byte. The **mask row goes into GC 8, the bit mask** (image `0x089B5`), with a latch load before each write. Where the mask bit is set: write plane 0 from the ink and **clear** planes 1..3. Where clear: leave the byte alone. |
  | 1..3 | image `0x089ED` turns Enable Set/Reset off, sets the function to **OR** (GC 3 = `0x10`), sets the bit mask back to **`0xFF`**, and selects one plane at a time through the Map Mask. Bit mask `0xFF` means these are **not masked** — every byte in the rectangle is OR-ed. |

  Net effect per pixel, which is the whole of what the port needed:

      mask set    dst  = ink
      mask clear  dst |= ink & 0x0E

  And that is exactly what the screen had been saying all along. Sprite value
  3 over background 0 gives `0 | (3 & 0xE)` = **2** — the original shows 2,
  210 times, where keying on "value != 0" drew 3. Value 2 gives 2. Value 1
  gives 0, 22 times. Values 4..7 never occur.

  The drop shadows are the other branch, bit 6 of `bp` at image `0x08905`:
  the menu passes `0x03` for a sprite and `0xF43` for its shadow. The shadow
  path at image `0x0909C` sets Set/Reset = 0 and Enable Set/Reset =
  `(bp >> 8) & 0xF` = `0xF`, driving all four planes to zero — solid black —
  with the mask as its silhouette.

  **Five models, measured, whole screen, all four ratings:**

  | ink rule | shadow rule | differing |
  | --- | --- | --- |
  | `!= 0` | `!= 0` | 326 |
  | plane 3 | `!= 0` | 460 |
  | plane 3 | plane 3 | 377 |
  | `!= 0` | plane 3 | 257 |
  | **the two-phase rule above** | **mask** | **0** |

  The lesson is not that measurement was useless — it narrowed four models to
  one and found the shadow rule. It is that **the best-scoring model was still
  wrong**, and no amount of further fitting was going to reach zero from 257.
  Reading the routine took less time than the fitting did.

- **Three of the four menu screens were being compared against the main
  menu.** `tools/verify_menu.py` drove to the title and captured, with the
  *same commands for every screen* - so `--screen code`, `--screen input` and
  `--screen briefing` scored the port's screen against a picture of the MENU
  and reported 65%, 64% and 30% as measurements. They never moved because
  nothing about the port was in them.

  The tool's own docstring records this exact failure for the RATING - "the
  port was then scored against the wrong screen, which read as the port
  drawing Taxing and Mayhem badly when it was drawing them correctly against a
  screen that was not showing them" - and the same bug was sitting one level
  up, in which screen was captured at all.

  With the key presses added, and F1 driven by the machinery `play.py`
  already had for it:

  | screen | reported before | actually |
  | --- | --- | --- |
  | menu | 99.3-100% | **100.00%** |
  | code | 65.43% | **100.00%**, or 99.97% when the caret is up |
  | input | 64.44% | **100.00%** |
  | briefing | 29.90% | **100.00%**, and its level preview **0 pixels** |

  The menu needed the banner's phase. It scrolls, so a capture shows one
  moment of it, and the port has to be rendered at the same one. `ds:0x21` is
  the game's own scroll and the run reads it back.

  **The 140 this used to subtract was not a real quantity**, and the way it
  survived is worth keeping. The socket was asked for **one byte** of a
  variable the game reads and writes as a **word** (`mov word ptr [0x21], 4`
  at image `0x07CC6`, `add ax, 4` at image `0x07D23`), so any counter past 255
  came back truncated. The derived phase was then wrapped to `0..63` and
  checked against a fit over the same 64 values - and a fit that only searches
  one tile cannot see a message position at all. The port's `--scroll` is a
  position along the whole **261-character** message: `draw_banner` indexes
  `ribbon[]` by `scroll/16 - 35`. So the check agreed, repeatedly, about four
  characters of it.

  Reading **both** bytes and using `c - 4` unwrapped - image `0x07CC6` starts
  the counter at four - puts the ribbon at **exactly zero** differing pixels on
  every capture. Sweeping the port's scroll across the whole message is what
  showed it: the best position was `c - 4` each time, and for two of the four
  captures that was 272 and 256 against a byte read of 20 and 4.

  The fit over `0..63` is kept, but as what it actually is - a check on the
  **phase**, not on the position.

  Three controls had to be added before that number could be trusted, and each
  one caught a different way of reaching nothing:

  - *"the capture changed"* is not enough - the menu's banner scrolls, so two
    captures of it never match and an inequality test passes while nothing has
    happened. It must change **most** of the picture.
  - *"it changed a lot"* is not enough either - pressing F1 twice went past
    the briefing into the level and the capture came back **entirely black**,
    which differs from the menu everywhere. A mostly-blank capture is a
    transition, not a screen.
  - and a single held F1 does not leave the menu at all, because the title
    fades its palette in and drops what is pressed during it.

- **The briefing has its own lower seven DAC entries**, at image 0x0C04B
  (`ds:0x25BB`) - blue, green, white, yellow, red, grey, orange, the set its
  text is drawn in. It does *not* keep the menu's, which are the background's
  browns.

  This went unnoticed for as long as it did because **every pixel of the
  screen already matched by index**. A palette-only difference is invisible to
  a comparison of indices and changes every colour on the screen; only
  `verify_menu`'s separate palette check saw it, and its message about which
  half was wrong had gone stale in the meantime.

  Six of the seven are byte-for-byte what a capture shows. The seventh, entry
  4, reads (44,44,0) in the file and (60,60,0) on screen - brighter. Something
  modifies it at run time, that has not been read, and it is left as the file
  has it rather than patched to match one capture.

- **The code screen's last 57 pixels are the entry caret** - the original
  draws a bar over the first code slot where the port draws the plain button.
  An animated element, like the banner, and not yet drawn: the screen scores
  **100.00%** on a capture taken while it is down and 99.97% while it is up,
  which is the blink and not a difference in the drawing.

  This is now the only animated thing on these four screens the port cannot be
  told the phase of. The banner's scroll comes from `ds:0x21` and the five
  walkers from `ds:0x4960`; the caret's own variable has not been found.

## Into a level

- **BLOCKER CLEARED: the original plays.** It was never frozen. It was
  running on a clock made of noise, and then starving itself in a spin the
  emulator was executing rather than waiting out. Two faults, both in the
  emulator, both measured before and after.

  **The game measures its own frame rate.** It does not assume one. At image
  `0x1602` it loads PIT channel 0 with `0xFFFF`, counts `bx` transitions of
  the display-enable bit on port `0x3DA`, latches the counter and keeps
  `0xFFFF - count`: the number of PIT counts in `bx` scanlines. Image
  `0x15AD` stores that in `ds:0x1FC9`, and the IRQ 0 handler at `0x17C7`
  reloads the divisor from it on **every tick**. So whatever the emulator
  says about the display comes straight back as the game's clock.

  `bx` is 320 or 160, chosen at image `0x15DF` from `ds:0x2007`, which the
  adapter probes at image `0x13DC` fill in. A VGA doubles the lines of a
  200-line mode, so 320 of its lines and 160 of a CGA's are the same 10.17 ms
  — the game is compensating for the adapter, and getting the adapter wrong
  costs exactly a factor of two.

  Four faults, all in the emulator, all now fixed (`dos_emulator` 347c1d4):

  | fault | what the game saw |
  | --- | --- |
  | PIT counter read back from elapsed time, ignoring the loaded value | divisor `0x3853`, `0x7698`, `0xC59C`, `0xDD25` across four runs of the same program |
  | a latched count recomputed per byte | low byte of one instant, high byte of another |
  | `INT 10h AH=12h BL=10h` unanswered → BL returns unchanged → "no EGA" | detected a **CGA**, timed against 160 lines, twice too fast |
  | timer looked at once a slice (40/s), and every divisor write threw the next deadline away | 13 Hz delivered against 97 Hz asked |

  The display-enable bit is the awkward one: it changes every 32 µs and a
  slice of guest instructions is a millisecond, so no wall clock this
  emulator can offer resolves it — which is why it toggled per read, and why
  a guest timing against it measured how fast the emulator was running. The
  guest says which it is doing, though: to *time* the register it needs a
  counter it can read back, so it puts channel 0 in **mode 0** first. While
  that holds, each read advances a virtual clock an eighth of a scanline.
  Otherwise it toggles as before and costs nothing.

  Measured after, and stable run to run where it used to be noise:

  | | |
  | --- | --- |
  | lines counted | **320** (was 160 — detected CGA) |
  | `ds:0x1FC9` | **12,279** counts (hardware: 12,135 — within **1.2%**) |
  | timer tick | **97.2 Hz** |

  **This did not regress anything, and the figures this file used to carry
  were stale.** Baselined by stashing the change and running the same check:

  | screen | before | after |
  | --- | --- | --- |
  | menu | 99.45% (1242 px) | **99.92% (186 px)** |
  | code | 65.43% | 65.43% |
  | input | 64.44% | 64.45% |
  | briefing | 29.90% | 29.91% |
  | `tools/prove.sh` | 100.00% | **100.00%** |

  The menu *improved*: a correct frame rate puts the animation on the phase
  the capture expects. The 100% recorded here for **code**, **input** and
  **briefing** was not measured at this tooling and is withdrawn — those
  three are ~65%, ~64% and ~30% and were so before this change too.

  **Then the level still would not run, for a second reason.** With the clock
  right, 29% of every instruction the game executed was the six-instruction
  wait for the vertical retrace inside its own IRQ 0 handler at image
  `0x17FE` — 919 passes an entry, each pass a port read and so a callback out
  of the emulator. The handler could not finish often enough for the main
  loop to get a frame, and the five-ticks-to-a-frame loop at `0x0164` was not
  running at all.

  That spin is idle by definition: the guest waits for a bit to change and
  does nothing until it does, so the time passes whether or not the spin is
  executed. `dos_emulator` 9cbca82 now skips to the edge after 64 unbroken
  reads of the status register. Measured over 106 s, before and after:

  | | before | after |
  | --- | --- | --- |
  | ISR entries at `0x17C7` | 20.2/s | **29.8/s** |
  | ...reaching its tail at `0x189E` | 71% of them | **every one** |
  | frame loop at `0x018F` | **never ran** | **5.9/s** |
  | ticks per frame | — | **5.08** (the game asks 5) |

  **The level plays.** The entrance opens, the lemmings drop, the panel and
  minimap draw, and the timer counts down from 5-00. Two captures 36 s apart
  show 5-00 with 0 out and 4-45 with 8 out.

  One trap this sprang, worth keeping: time the guest has waited out belongs
  in its clock but **not** in the harness's. With both on one number a run
  told to last 60 s stopped after 20, and `tools/prove.sh` went from 100% to
  a failed capture — which it reported honestly as a failed capture rather
  than as a comparison, which is the only reason it was caught at once. The
  emulator now separates `_elapsed` (what the guest sees) from `_wall` (run
  length, screenshots, driver scripts).

  Re-derived after both changes, with `tools/prove.sh` back at **100.00%**:

  | screen | before | after |
  | --- | --- | --- |
  | menu | 99.45% (1242 px) | 99.34% (1468 px) |
  | code | 65.43% | 65.45% |
  | input | 64.44% | 64.44% |
  | briefing | 29.90% | 29.92% |

  The menu figure **wobbles between 186 and 1468 pixels from run to run**,
  and `tools/menu_diff.py --map` says why in one look: every differing pixel
  is in rows 336..349, the scrolling banner. It is the phase the capture
  lands on, not the port. The 100% this file used to claim for **code**,
  **input** and **briefing** was not measured at this tooling and is
  withdrawn — they were at ~65%, ~64% and ~30% before any of this too.

- **Where the port stands inside a level, measured against a real frame.**
  With `tools/record_level.py` giving a reference sequence,
  `tools/compare_frame.py` against frame 60:

  | | |
  | --- | --- |
  | play area, rows 0..159, at the fitted scroll of 624 | **98.60%** (716 of 51,200 differ) |
  | panel, rows 176..199 | 4,717 pixels of ink in the original, **the port draws none** |

  The 716 are the lemmings, the cursor and the animated objects — everything
  that moves. The terrain underneath is right.

- **The panel's artwork is transcribed** - image 0x057D1, set up at image
  0x05914, which fixes every dimension the fitting could not: `si = 0` so
  main.dat section 2 from its start, `cx = 0x28` rows, `bx = 0x428` for forty
  bytes a row and four planes, `ax = 0x2C` for a 44-byte destination stride,
  `di = 0xF91F` which stashes it near the top of the 64K plane. The loop pops
  `di` per plane but lets `si` run on, so the source is plane-major - 40 x 40
  x 4 = 6,400 bytes, exactly where the status font begins.

  Screen row 160 and the attribute map are measured, not read: values 0..7 go
  to themselves and 8..15 to 16..23, which is inference from the five high
  values the artwork uses, since 7, 8, 9 and 13 never occur in it.

  **Panel 86.41%.** The picture shows the rest is the status counts, the
  minimap contents and the selected button. The status line is deliberately
  not drawn: `panel_status` blanks the cells it is given and nothing has read
  what fills its template, so the bare template scores **65.65%** against
  86.41% for the artwork alone. A placeholder there is worse than a gap.

- **The objects are drawn the way the original draws them now.** `--raw` was
  using `render_objects`, which bakes them into the terrain and whose own
  comment says that is *not* what the original does; the transcribed
  `level_anim_draw` (image 0x07BB8) was only ever reached by the window. That
  is why every frame comparison carried the same residue round the entrance.

  Switching it costs a little on a static render and pays once anything
  animates, which is the point:

  | | baked, frame 0 | animated, best frame |
  | --- | --- | --- |
  | reference frame 30 | 134 differ | 157 |
  | reference frame 42 | 559 | **329** (frame 7) |
  | reference frame 54 | 688 | **335** (frame 13) |
  | reference frame 90 | 742 | **389** (frame 13) |

  Stepping the port's animation once per game frame over the whole run takes
  the play area's mean from 98.96% to **99.11%** - and shows up a real
  difference rather than hiding it: the port's entrance starts opening at once
  while the original's stays shut for about **32 frames**, so the early frames
  get worse (98.61% at frame 12) as the late ones improve (99.23%). That the
  late frames agree says the animation ends in the right state; it does *not*
  establish the rate in between, and the delay has not been read - only
  measured.

- **There is ONE attribute map for the screen, and believing there were two
  hid a real difference.** The checker compared terrain with an unconditional
  +8. That is right by accident - terrain values are all 8..15 - and wrong for
  everything else. The objects proved it: where the port and the original
  disagreed around the exit, the port's raw 1, 3, 4, 5, 6 and 7 came back from
  the original as **1, 3, 4, 5, 6 and 7**, unmapped, while the checker was
  showing them as 9, 11, 12, 13, 14 and 15.

  The rule is `v < 8 ? v : v + 8`, the same one measured independently on the
  panel's artwork. Applying it took the play area from 98.61% to **98.76%**
  without touching a line of the port - the port's canvas was right and the
  instrument was wrong, which is the failure mode this file keeps recording
  from the other direction.

- **The entrance sprite itself is still wrong, in both renderers.** 91 of the
  134 pixels that differ before any lemming drops are the entrance: the
  original draws two lit torches and a blue-and-green interior, the port draws
  the grey arch alone. The original's blit is 48 wide, 25 rows, 4 planes from
  segment 0x2900, with frames stepping **0x2EE = 750** bytes - and 48x25 at
  four planes is 600, so there is a fifth 150-byte plane, the mask. Whatever
  the port is decoding, it is not all of the sprite. That is the next thing to
  read, and it is worth more than the panel's remaining text.

- **The level's opening sequence is transcribed** - image 0x0361, called from
  the play loop's first line at image 0x019D:

  | play-loop frame (`ds:0x52`) | what happens |
  | --- | --- |
  | 15 (`0x0F`) | a sound |
  | **35 (`0x23`)** | **image 0x394 sets `ds:0x55` to 0xFF - the trapdoor opens** |
  | 55 (`0x37`) | image 0x3AD, the music |
  | past 55 | bit 2 of `ds:0x4E` clears; the opening is over |

  That one byte holds back both the trapdoor (image 0x07C1E tests it before
  animating object id 1) and the lemmings (image 0x30B0 tests it before
  releasing one). Nothing in the image *calls* 0x394 - no near call, no far
  call, and the literal 0x0394 appears nowhere - it is fallen into from this
  chain, which is why the return address on the stack when it runs is 0x1A0.
  Watching the running game confirmed the frame before it was transcribed:
  the 0xFF arrives from image 0x394 at frame **35**.

  `tools/probe_blits.py` also gave up the play loop itself, at image
  0x019C..0x01C5 - fourteen calls with `inc byte [0x52]` as its frame
  counter. That is the spine of everything still to be done.

- **The status line, the level clock, and where the panel really sits.**
  Watching every write into the line settled all of it:

  | | |
  | --- | --- |
  | the line's base | **`ds:0x207F`**, not `0x2080` - the comment on `PANEL_TEMPLATE` had it one byte out |
  | its width | 44 cells of 8 = **352**, the mode's logical line |
  | the artwork inside it | cells 4..43, so the line's cell 0 is **32 px off the left edge** |
  | the glyph rows | 16 rows from screen row 160; ink lands in 164..174 |
  | OUT count | cols 22..24 (image 0x04B68..0x04BBA) |
  | IN percentage | cols 29..31 (image 0x03B1C) |
  | TIME | minutes col 40, `:` col 41, seconds cols 42..43 (image 0x0243) |
  | the clock | **17 frames to a second** - `ds:0x85`/`ds:0x86`, both 0x11 at image 0x0418 |

  Every character lands where that predicts: the template's `O` of OUT at col
  18 draws at screen cell 14, its `:` at col 41 at cell 37. The port's
  `PANEL_TEMPLATE` turned out to be character-for-character right; only the
  values and the origin were missing.

  So **five minutes is 5,100 frames** - 300 seconds at 17 apiece - and a
  play-loop pass is a display frame one for one (`0x01A3` and `0x018F` were
  counted together: 151 and 151, 676 and 676).

- **The panel font writes planes 0, 1 and 3 - not 0, 1, 2.** Image 0x077A8
  makes three passes and each names its plane through the Map Mask: 0x0F with
  Enable Set/Reset 0x0E for plane 0, then **0x02**, then **0x08**. So a glyph
  takes values 0..3 and 8..11 and never 4..7.

  The port had the third plane in bit 2. That is wrong in a way that still
  draws perfectly legible text, which is how it survived: against a recorded
  frame it was writing 5 where the original had video 9 - **177 pixels**, and
  every character looked correct. The residue was one clean relation, which
  is the signal this file already records for the menu's plates: a structured
  residue is a rule not yet found, not noise.

- **The lemming array, and the release schedule transcribed.** Image 0x03634
  walks it: `di = 0x88`, `cl = ds:0x43` entries of **0x2D = 45 bytes**, and it
  ends by putting the count of live ones - those whose `+0x26` is not 0xFF -
  into the line at `ds:0x2095`, which is the OUT field at col 22. Fields read
  so far, from records dumped out of the running game: `+0x00` x, `+0x02` y,
  `+0x04`/`+0x06` the sprite's hotspot offset (-8, -10), `+0x26` alive,
  `+0x28` the action.

  The release itself is image 0x30A6, gated on the same `ds:0x55` the
  trapdoor is, with the interval from image 0x39D6:

      al = 99 - rate / shr / neg / add 0x35 / neg / add 0x39

  which is `((99 - rate) >> 1) + 4` once the two negations cancel - **28
  frames** at the rate of 50 that "Just dig!" carries - and the countdown
  starts at **0x14 = 20** (image 0x39D1). So the entrance opens on pass 36 and
  the ten come out on 55, 83, 111 ... 307.

  **The OUT field differs in 0 pixels** at frames 60, 200, 400 and 690, and
  across both release boundaries (52..58 and 82..85). The probe's readings
  are one pass later than the releases because it read `ds:0x43` at image
  0x01A3 and image 0x30A6 runs at 0x01B0, later in the same pass; the pixels
  settle it independently.

  What OUT really counts is the LIVE lemmings, not the released ones. Nothing
  walks the array yet, so the port feeds it the released count, and
  `level_status_out` says so where it does it: the two agree only while
  nothing is lost.

- **`status_put_num`, image 0x04B68** - three cells, left-aligned with leading
  zeros suppressed, `si` advancing only when a digit is printed. Not the
  briefing's `put_num`, which is still unread and right-aligns, and not
  `digits3` at 0x04BC2 either; the game has three.

- **The skill buttons' numbers - a font nobody had found.** Image 0x0765F
  draws them from ten digits of 16 bytes at **offset 0** of the loaded panel
  font, which is `main.dat` section 2 offsets **6400..6559** - the 160 bytes
  immediately before `PANEL_HEAD`, and the reason 0xA0 is where the big font
  starts.

  Each digit is **four pixels wide** and stored twice, pre-shifted: bytes
  0..7 hold it in the low nibble and 8..15 in the high, so a two-digit number
  is the tens through Bit Mask 0x0F into one byte and the units through 0xF0
  into the next. That lands the pair at pixels 4..11 of the button's sixteen
  - exactly where a recorded frame has them.

  Two things about it would not have been guessed:

  - a count of **zero is SOLID, not blank**. Image 0x076DB writes 0xFF for
    both nibbles, which is why a level offering only diggers still has ink in
    all ten button positions.
  - the colours: a set font bit is index **3** and a clear one index **0**,
    measured by counting - "50" has 40 set bits between its glyphs and the
    original shows 40 pixels of index 3 and 24 of index 0 there, and a zero
    count shows 64 of index 3.

  Finding it needed the page base resolved: watching video writes and
  filtering to the exact rows and byte columns named image 0x076A7 and
  0x076D2 straight away, where a rank by volume had buried them under the
  background repaints.

- **The selected button's box, image 0x0780D.** Sixteen by twenty-four in
  colour 3 with only its edges drawn: top and bottom rows solid, and
  twenty-two middle rows carrying just the leftmost and rightmost pixel,
  which image 0x07869 does with Bit Mask 0x80 then 0x01. `Enable Set/Reset`
  0x0C is what makes a `0xFF` come out as colour 3 - planes 2 and 3 cleared,
  0 and 1 from the CPU - and that agrees with the recorded frame.

  Image 0x2102 starts the buttons **two bytes into the line**, so button *b*
  is at 16 + 16*b*, and `ds:0x83` names the boxed one - image 0x0427 starts
  it at **3**, the third skill button, whether or not the level offers that
  skill. `ds:0x82` is the previously boxed one so image 0x3B29 can rub one out
  and draw the other; with nothing clicked they stay 2 and 3.

- **The IN percentage, image 0x03AF8**, and the trick in it: with no tens
  digit it loads `0xF0` and adds `'0'`, which wraps a byte to a **space**. So
  0 reads `" 0"` and not `"00"`. A hundred is special-cased across all three
  cells before any of that.

- **The minimap, transcribed.** Image 0x07900 copies it into place from a
  bitmap the game stashes off-screen at `0xA000:0xD320`, 13 bytes by 20 rows,
  and images 0x0796F, 0x07A25 and 0x07A39 build that bitmap one byte-column a
  frame - `ds:0x56` cycling by 16 and wrapping at 0xD0, so thirteen frames to
  refresh the lot. The port builds all thirteen at once; the picture is the
  same.

  How a pixel is decided had to be read. Image 0x079C9 puts the graphics
  controller in **read mode 1** with Color Compare and Color Don't Care both
  0x08, so a read returns "is plane 3 set" - and plane 3 is where terrain
  lives, which is why GROUNDxO.DAT's fourth bitplane doubles as its mask.
  Image 0x07A39 then reads two of those bytes, counts the sixteen bits, and

      cmp dh, 8 / jg  ->  or byte cs:[bx], dl

  sets the pixel only when **more than eight of the sixteen** are terrain. A
  majority, not "any" - which is why a level's stalactites do not show on the
  map. Vertically there is no reduction at all: image 0x079FB steps the source
  by 0x680, eight rows of the 208-byte canvas, so it samples every eighth row
  and ignores the seven between.

  The colour is **7**, and the EGA state says it must be: Set/Reset 0x06 with
  Enable Set/Reset 0x08 leaves plane 3 clear and gives planes 0, 1 and 2 the
  CPU's bits. A recorded frame agrees - 308 of the 389 pixels the rule lights
  are index 7 there, the rest being the viewport box and the lemming dots over
  it.

  Where it sits is measured, like `PANEL_TOP`: screen x **209**, y **177**, a
  sharp minimum over a 7 x 7 search - 137 differing at that origin against
  216 and 217 either side.

  **One loose end, written down rather than papered over.** The copy is
  byte-aligned, so the map must start on a byte boundary of the 352-pixel
  line; screen x 209 is line x 241, and 241 is not a multiple of eight.
  Something in the page-offset-to-screen-column mapping is still a pixel out,
  and the proof is that clearing the full 104-pixel span - which the
  original's copy does - rubs out part of the artwork's frame: 232 differing
  against 137 for ink alone. Until that is read, only the ink is drawn.

- **The minimap's viewport box, image 0x07A5A.** `di = [0] >> 7` puts it at
  the scroll in minimap bytes, `Enable Set/Reset 0x0C` makes a written bit
  colour 3, and `cx:bx = 0xFFFF:0xFF00` is a **24-bit** run shifted right by
  `([0] >> 4) & 7`. Twenty-four minimap pixels - not the twenty that 320
  screen pixels come to at this scale - and there is no arithmetic behind the
  24 to get wrong; it is the mask's own width.

  With the scroll at 624, `624 >> 4` is 39, so the box starts at minimap pixel
  39, which is screen x **248** - and 248..271 is exactly where two separate
  recorded frames put it, solid top and bottom with a single pixel down each
  side.

- **The lemming record, read from the running game** (`tools/probe_lemmings.py`).
  Two of the names in this file were wrong and are corrected here:

  | offset | what it is |
  | --- | --- |
  | `+0x00`, `+0x02` | x and y - and **canvas position is the record minus 16 on both axes**, confirmed against two lemmings' pixels in one frame |
  | `+0x04`, `+0x06` | the sprite's hotspot, -8 and -10 |
  | `+0x23` | the fall counter: +3 a frame, capped at 21 |
  | `+0x26` | alive; 0xFF is dead |
  | `+0x27` | direction, 1 for right |
  | **`+0x28`** | the **animation frame**, not the action |
  | **`+0x29`** | the **state**: 4 falling, 9 walking |

  What they do, over 1,127 recorded walking steps:

  - **falling** - x fixed, y **+3** a frame, animation cycling 0..3, the fall
    counter climbing 3 at a time to 21
  - **walking** - x **+1** a frame in the direction, animation cycling 0..7,
    and y becoming the topmost terrain row in the new column

  The walker is **transcribed now, and exact**: image 0x2047, replayed over
  every recorded step, is **1,127 right and 0 wrong**
  (`probe_lemmings.py --check`).

      anim = (anim + 1) & 7                         image 0x2047
      x += direction                                image 0x2051
      if x < 0x10 or x >= 0x680: turn round         images 0x2057, 0x2142
      solid at y?  yes: the first empty of y-1..y-3 settles at y-(k-1)
                        the first empty of y-4..y-7 settles at y-2 and sets
                        bit 3 of [si+8], a different handler from then on
                        none of the seven: turn round, unless it can climb
                   no:  the first solid of y+1..y+3 settles there
                        none: it becomes a faller, y += 4

  Getting there needed three things right, and each one alone made an exact
  rule look approximate:

  - **the canvas.** Image 0x5DC6 takes the row from `y - 0x10` and the byte
    from `x >> 3`, and the port's canvas sits 16 pixels along from the
    original's - which is not a fudge, it is the `offset 16` that
    `tools/prove.sh` has been printing all along.
  - **terrain only.** The buffer the original probes has no objects in it;
    they are drawn at display time. Probing a canvas with the entrance in it
    scored 322 of 1,134.
  - **what counts as a step.** `[si+8]` dispatches to fifteen other handlers,
    and seven of the steps in the recording belong to the one that image
    0x2169 turns the lemming into. Counting those made 1,127-out-of-1,127
    read as 1,127 out of 1,134, which is exactly the shape of a rule that is
    nearly right - and it was not nearly right, it was right.

- **The simulation is image 0x1F19**, reached from the play loop at image
  0x01B3. Image 0x03634 only *draws*; image 0x05726 erases. `[si+8]` is a
  **state bitmask** and image 0x1F5E dispatches on it to sixteen handlers -
  and with no player input it is **zero on every lemming on every frame**, so
  the default at image 0x2047 is the only path the whole reference run takes.
  That is why the walker was worth transcribing first.

  Still to read: the faller at image 0x21B0 (its shape is clear - probe three
  rows, land on the first solid one, else `y += 3` with the counter `+= 3`,
  and over 0x3C it splats), where a released lemming starts, and the sprites.
  The dots on the minimap at image 0x06FE2 need only x and y, so they come
  free with the simulation.

- **The simulation runs in the port.** The walker (image 0x2047), the faller
  (image 0x21B0) and the release (image 0x030F1) are transcribed, and both
  motions are checked against the original's own array rather than a
  screenshot - `probe_lemmings.py --check` replays them over every recorded
  step: **1,127 walker steps and 54 faller steps, none wrong**.

  A released lemming comes from the entrance table at `ds:0x0E98`, four bytes
  an entrance, cycling `count & 3`. For "Just dig!" it holds **744, 66**
  against an entrance object at 704, 36 - so the entrance plus (24, 14) - and
  it starts falling with the counter at 3.

  The faller: probe y, y+1, y+2; land on the first solid one; else `y += 3`
  and the counter `+= 3`. Landing under `0x3C` walks, over it splats.

- **Two bugs the dots found, which the terrain alone could not.** Image
  0x07992 begins the minimap sampler with `si += 0xD00`, which at the canvas's
  208-byte stride is **sixteen rows** - and I had dropped it. The terrain
  looked right without it because a fitted origin absorbed the error. What
  gave it away is that the dots would not sit on the ground: image 0x06FE2's
  row is `(y - 0x20) >> 3`, and the two only agree when the sampler starts at
  16. Putting it back and sweeping the origin gives 201, 98, **12**, 93, 194
  differing pixels at 17..21 - a peak so sharp it is not a fit.

  The dot's colour is **2**, measured; the map's own ink is 7.

- **The Makefile did not depend on the headers.** `src/%.o: src/%.c` alone
  means a change to `game.h` leaves every object file alone and the binary
  keeps the old constants - so the first origin sweep returned the *same*
  number five times and I nearly read that as "the origin does not matter".
  It is the stale-artefact trap this file records for captures, applied to
  object files. Fixed.

- **The lemmings are drawn.** The addressing was right all along; the probe
  was a frame behind it.

  Capturing the pointers the draw itself computes - at image 0x070E7, where
  `bp` is the paragraph offset and `bx` the mask's byte - the walker's eight
  animation frames come out at mask offsets **60, 870, 1680, 2490, 3300,
  4110, 4920, 5730**. Steps of exactly 810, which is 9 x 90, because `anim`
  and `x & 7` both advance by one every frame. That is
  `(anim * 8 + shift) * 90 + 60`, precisely as image 0x070BF reads.

  Why every search had failed: `probe_lemmings.py` samples at image 0x01A3,
  and the simulation at 0x01B3 and the draw at 0x01BC both run after it, so
  the record was one step behind the pixels. **Measuring the game's own
  registers settled in one run what four rounds of searching could not.**

  With the offsets known, the store's bytes lead back to the source: it is
  `main.dat` **section 0 from offset 0**, 40 bytes a frame - plane 0's ten
  rows of two bytes, then plane 1's. The port has no reason to pre-shift, so
  it reads that directly. Two planes give values 1..3 and 0 is transparent,
  because the builder's second pass ORs the planes to make the mask. The
  frame is `anim` facing right and `anim + [si+0x29]` facing left - image
  0x070AB - which is why the loader builds eighteen.

- **And one more handler.** Every lemming froze at x=851 the first time this
  ran, all at the same spot: they had hit the four-row climb at image 0x2169,
  which sets bit 3 of the flags, and the port had no handler for it. Image
  0x23BF is that handler - climb one row a frame while there is terrain above,
  clear the bit the frame it finds air - and with it they turn at the wall and
  walk back, as the original's do.

  That the port could freeze ten lemmings in one place and still score 99.34%
  is worth remembering. The score moved 0.5% when it was fixed; the picture
  moved from wrong to right.

- **FIVE MINUTES, FRAME BY FRAME.** The level's expiration timer is 300
  seconds at seventeen frames a second, so the run is **5,100 frames**. All
  5,100 are recorded (`tools/record_level.py`, 319 MB) and 102 of them -
  every fiftieth - are scored against the port:

  Every one of the 5,100, not a sample:

  | over 5,100 frames | exact |
  | --- | --- |
  | play area, once the mouse cursor's 28 pixels are subtracted | **5,100 / 5,100** |
  | panel | **5,099 / 5,100** |

  The single exception is frame 0, caught mid-fade with the palette still
  dark. Everything else is pixel for pixel.

  So the port draws what the original draws, pixel for pixel, for the whole
  of the level's expiration timer.

- **The last sixty frames were the faller's own sprites.** Comparing every
  frame rather than every fiftieth found them: ten groups of six consecutive
  frames, starting at 55, 83, 111 ... 307 - exactly the release frames, and
  exactly the six frames each lemming spends falling. A 2% sample had missed
  all sixty.

  A released lemming gets `[si+0x0c] = 0xA42` from image 0x030F1, which is the
  paragraph its group starts at in the pre-shift store - the walker's is 0 -
  and image 0x05A48 builds a group of eight there. Reading those frames back
  out of the store and looking for them in the file puts them at source frames
  **358..361**, one match each and no others, and they are unmistakably
  falling lemmings with their arms out.

  The important thing is not the mean but that it is **flat**. Frame 50 and
  frame 5,000 score the same to two decimal places. Nothing drifts, nothing
  accumulates, and the lemmings are still where the original's are after five
  minutes of simulation.

  **The clock is exact at every one of them** - the TIME field differs in 0
  of 512 pixels at frames 0, 850, 867, 1700, 2550, 3400, 4250, 5000, 5050 and
  5099.

  Getting that took one more correction. With the countdown reloading at 17
  and *starting* at 17, the port and the original disagreed on exactly one
  frame in every seventeen - 850, 867, 884 - and agreed on both neighbours.
  The original's timer is image 0x0213, which sits later in the play loop than
  image 0x018F where a frame is finished and captured, so **a frame shows the
  time from before its own pass's tick**. The port defers its first tick by
  one frame to match. A whole-frame comparison would have called that 56
  pixels of noise; sampling the one field it belongs to is what made it a
  question with an answer.

- **The exit's flames were an animation phase, not a position.** Two objects
  sit at the same x=848 and only one of them was offset, which is not what a
  misplaced sprite looks like. Comparing the port's frame 399, 400 and 401
  against the original's 400 settles it: the exit region scores 51, 47 and
  **23**, so the looping flame is exactly one animation step ahead of the
  port's. One extra `level_anim_step` before the first frame matches it - the
  entrance is unaffected because it is gated shut until frame 35.

  The residue is now **identical at every frame - 51 pixels, of which 28 are
  the cursor**. That it does not vary at all is the useful part: what is left
  is static, so nothing about the simulation is still drifting.

- **The minimap's dots sit one pixel left of its terrain.** Measured: the
  panel is 12 pixels out with the dots at the map's own origin and exactly 0
  out at one less. That is also the loose end already recorded against the map
  itself - the copy that places it is byte-aligned so it must start on a
  multiple of eight, and 240 is one while the fitted 241 is not. The dots are
  at the aligned origin; the map's 241 is absorbing a one-pixel shift
  somewhere in its own decode. Both are exact against the original; **why the
  two differ is not read yet**, and the constant says so.

- **The last 23 pixels of the play area were the object modifier.** The port was drawing the
  exit's bottom rows from a mask that really is dense there - so the data was
  not the answer, and the rule had to be somewhere else. It is the LVL's own
  `e[6] & 0x80`, which `level_parse` had been reading into `no_overwrite`
  since the level format was done, which `render_objects` had always honoured,
  and which the transcribed `level_anim_draw` that replaced it did not: **an
  object is not drawn where there is terrain**. It reaches the blitter as bit
  15 of `bp`, which is the `0x800E` the blit probe logs against every object.

  The measurement is what makes it certain rather than plausible. Of the
  pixels where the port drew the exit and the original did not, **23 of 23**
  had terrain under them; of the pixels where both drew it, **49 of 49** did
  not. Terrain is the only thing with bit 3 set, so the test is exact rather
  than "something is already there".

  A capability the port already had, lost in the move to the transcribed
  routine. Worth looking for whenever a rewrite makes something slightly
  worse.

- **Open lead: the four-plane writer at image 0x07F1D.** `probe_writes.py`,
  run over the rows the missing lemming occupies with the correct 44-byte
  stride, ranks four writers at 601 bytes apiece — images `0x07F7B`,
  `0x07FF1`, `0x08067`, `0x080DD`. Four, one per plane.

  It is a **chunky-to-planar transpose**: it reads four consecutive source
  bytes into `ch/cl/bh/bl` and builds one output byte per plane out of them
  with `rol`/`ror` and a mask per bit, setting the Sequencer's map mask between
  passes. Four source bytes become four planar bytes — eight pixels.

  From the masks, the source looked like **4bpp packed two pixels to a byte**:
  output bit 7 comes from `ch` bit 5 and bit 6 from `ch` bit 1, which is the
  same plane of two adjacent nibbles. **That inference does not hold.**
  Rendering `main.dat` section 0 as 16×10 at 4bpp gives noise, not a lemming,
  so the source is something else — quite possibly a composed buffer rather
  than stored artwork, since the routine also latch-reads its destination.

  Recorded as a lead, not a finding. What would settle it is dumping the
  source bytes at the moment of the call, the way `probe_blits.py --dump`
  does for the blitter, rather than inferring the format from the rotates.

- **The level's logical line is 44 bytes, not 40 — and that is how it
  scrolls.** Image `0x07710` puts the panel at `0x1E40`, which is 7744, which
  is **176 × 44**. So the mode shows 320 pixels of a 352-pixel line and has 32
  pixels of margin to slide within. It is also why the panel's glyph drawer
  steps a row by `0x2B` at image `0x077BB` — 43, plus the 1 `stosb` already
  added.

  The scroll lives at `ds:0x0000` in pixels with `ds:0x0002` the same divided
  by eight (images `0x0416A` and `0x04175`), and `ds:0x0000` is what the object
  draw subtracts at image `0x07BCF`.

- **The panel's status line is transcribed** — image `0x0417B`: `lodsb`, a NUL
  ends the line, the cell index rides in `AH` and is incremented per character,
  and image `0x07710` adds that index straight to the base, so cells are eight
  pixels apart. What still is not read is what writes the counts *into* the
  template before the line is walked.

- **The play area matches the original at 99.95%, and the frame comparison
  exists.** `tools/compare_frame.py` puts one level frame of the original
  beside the port and scores the play area and the panel **separately**,
  because they are reconstructed to different degrees and an average would
  hide which.

  | | |
  | --- | --- |
  | play area, rows 0..159 | **99.95%** — 28 of 51,200 differ |
  | the 28 | **all of them the mouse cursor** — see below |
  | panel, rows 176..199 | 4,361 pixels of ink in the original; the port draws **none** of it |

  Two things fell out of getting there.

  **The mode's colour map is +8.** The port's canvas carries indices 0..15 and
  the capture carries DAC indices; the histograms line up 19/11, 18/10, 17/9,
  16/8, and 15/23 and 14/22 agree to the exact pixel count. Measured, not
  assumed.

  **The level opens scrolled to `start_x`.** The fit lands on 624 and the
  level's own header says `start x 624`.

  **Correction: the 28 are the mouse cursor, and the play area is otherwise
  exact.** They were written up here as "a lemming, which the port does not
  draw" - a guess from a bounding box. Drawn as a shape they are a perfect
  14x14 dithered cross-hair:

      ......##......        an arm every other row,
      ......##......        two solid rows across the middle,
      #.#.#.##.#.#.#        the horizontal arm dithered
      #.#.#.##.#.#.#        every other pixel
      ......##......

  which is the game's own pointer. It lands wherever the mouse is - the
  emulator is driven by clicks - so it says nothing about the reconstruction.
  `compare_frame.py` matches the shape and reports it as the cursor rather
  than as a difference.

  Drawing the objects makes it **worse** — 585 to 843 against 28 — so at this
  moment the original is not showing them where the port bakes them in. That is
  consistent with the level having only just started: no lemmings have been
  released, and the entrance has not been drawn either.

- **The DAC reads all black in every mode 0x0D capture taken so far.** Two
  captures sixteen seconds apart are both entirely black, 0 of 256 entries, so
  the usual "sampled mid-fade" explanation is **not** established - this mode
  may set its colours somewhere the capture path does not read. The index
  comparison is unaffected and sound. `compare_frame.py` says so loudly and
  draws its images in greys by index, so a dead palette can never again look
  like a broken port.

- **The panel's font is found and transcribed** — `main.dat` section 2's tail,
  and a **three**-plane font, not four like everything else on this screen.

  It could not be found by hooking the blitter, because the panel does not use
  it. `tools/probe_writes.py` is new and finds a drawing path the only way that
  cannot miss: it attributes every write into the EGA window to the
  instruction that made it. Three `stosb` sites — images `0x077B9`, `0x077DD`,
  `0x077F5` — and a fourth at `0x07804` that writes zeroes.

  Three sites because there are three planes, and the glyph stride says the
  same: image `0x07782` shifts the index left four times for ×16, then
  `0x0778C` shifts once more and adds again for **×48**, which is three planes
  of sixteen rows.

  The dispatch at image `0x07756`:

  | character | table |
  | --- | --- |
  | `' '` | image `0x077FE` — sixteen rows of zero, not a skip |
  | `'%'` | 0 |
  | `':'` | `0x210` |
  | `'0'..'9'` | `0x30` |
  | `'A'..'Z'` | `0x240` |
  | anything else | nothing |

  Every glyph is `base + 0xA0 + table + index * 48`, and **that arithmetic
  closes on the font's size exactly**: `0xA0 + 0x240 + 26*48 = 1984`, which is
  what image `0x0593C` copies (`cx = 0x3E0` **words**) from offset `0x1900`.
  6400 + 1984 is section 2's length to the byte.

  Rendered, the glyphs read `0 1 2 3` and `A B C` unmistakably.

  The status line's template is image `0x0BB10`: **44 characters**, which is
  the panel's own stride — image `0x077BB` steps by `0x2B` and the `stosb`
  before it has already added the forty-fourth.

- **THE LEVEL IS MODE 0x0D — 320x200 — not the menu's 640x350.** That is the
  single most important thing about this screen and nothing here had recorded
  it. The mode sequence through a whole run is `03 → 05 → 0D → 10 → 0D`: the
  menu is `0x10`, and the game drops back to `0x0D` to play.

  It explains why a blit probe over "rows 160..349" found nothing but menu
  leftovers: in a 200-row screen those rows mostly do not exist.

  Captured from the original (`play.py --dump-when 0x0d` with a long delay):

  | rows | what is there |
  | --- | --- |
  | 0..159 | the play area - terrain, in colours 16..19 |
  | 160..175 | blank |
  | **176..199** | **the control panel**, 24 rows |

  The panel is **not drawn through image 0x8822**, the blitter every menu
  screen uses - a driven probe with `--play` logs no call into those rows. So
  mode `0x0D` has a drawing path of its own that has not been found, and that
  is the next thing to look for.

  Searching the data files for the panel as one 320x24 four-plane bitmap gets
  **85.8%** at `main.dat` section 2 offset 960 - close enough to say section 2
  is involved and far enough to say it is not stored whole. Looking at it, that
  is unsurprising: twelve skill buttons with counts above them and a status bar
  is a composition, not a picture.

  The port draws none of it. Its level view is the play area alone.


- **The level's objects are animated at display time, and both halves are
  transcribed** — image `0x07BB8` draws them, image `0x07C0A` steps them, over
  a 28-byte state per object at `ds:0x17CB`.

  The call graph found them: driving `tools/probe_blits.py` past the briefing
  with `--play` added exactly **one** call site to the menu's twelve —
  `0x07BF9` — and its seventeen blits are the entrance and the exit.

  Both routines walk the same two arrays in step: the level's own 32-entry
  object list at `ds:0x0FEB`, eight bytes each, and the state array at `0x1C`
  bytes each. An x of zero ends an object's turn.

  The stepper is a small state machine and it is exact:

      bx = [si]                 flags: bit 1 animating, bit 0 one-shot
      test bx,2 / je            not animating - leave it
      al = [si+2] / ah = [si+3] current frame, end frame
      inc al / cmp al,ah / jne
      xor al,al                 wrapped
      test bx,1 / and bx,~2     one-shot: stop animating
      mul word [si+6] / add [si+0x15] / mov [si+0x17],ax

  **Verified against what the original actually blits**, not a screenshot. The
  probe logs the entrance as 48×25 at screen (96,36) with ten distinct sources
  750 apart — `0, 750, 1500 … 6750` — and `si=0` seen **179 times** against one
  apiece for the others. That is a one-shot running its frames once and then
  sitting on frame 0, and the port reproduces the sequence exactly.
  `reconstruct/tests/screens.c` asserts it.

  It also settled the sixteen. Image `0x07BCF` reads the level list's x
  **unbiased** and does `sub ax,[0]` then `add ax,0x10` — so the +16 is the
  original's own, applied against the scroll at display time, and
  `level_parse` taking it off at parse time is the same constant seen from the
  canvas's end. That had been derived by fitting the briefing's preview; it is
  now read.

- **No function in `game.c` claims to be the port's own any more.** That
  phrase inside the game's file was a contradiction, and it was hiding two
  different things behind one label.

  Moved to `sdl_io.c` — genuinely not the game: `menu_run` (the original has
  no loop; image `0x0447B` reads a key, jumps, and never returns a value),
  `menu_build`, the four scroll/frame accessors, `be16`, `le16`, `ground_load`
  and `GROUND_SIZE`. What runs *inside* the loop stayed: `menu_frame_step` is
  image `0x07D0D` and is the game's half of it.

  Kept in `game.c` but reclassified as **owed**, because the original does have
  a routine and nobody has read it: `screen_start` (the sub-screens both open
  with `0x04DEF`, `0x04BF5`, `0x0531E`, `0x05017`), `put_num`,
  `menu_briefing_screen` (reached from `0x0454C`), `ribbon_build`
  (`0x07D33` walks the stream and never builds it), and `menu_build_rating`
  (every piece it draws is transcribed; the routine that calls them in order is
  not).

  Two that looked like candidates to move stayed put: `sign_x`/`sign_y` and
  `menu_anim_visible` read the game's own transcribed tables, and moving them
  would have dragged `SIGN_ROW` and `anim_ctr` into the IO file — worse than
  the label they had. They now cite the data they read instead.

  **The count fell as a result, and that is the honest direction.** Three
  separate faults in `provenance.py` had been inflating it: it could not see a
  marker on a line that *opens* a comment block, it let an address mentioned in
  prose outrank an explicit declaration, and two comments said both
  "the port's own" and "Transcribed from…". Fixed, and the figures are
  **30 transcribed, 35 the port's own, 9 owed, 0 unannotated**.

- **The menu is composed by the original's own routines now, not by fitted
  geometry** — images `0x04EEF`, `0x04F31` and `0x04EC2`, all three found
  through the call graph rather than by looking at pixels.

  | the port had | the original has |
  | --- | --- |
  | `SIGN_POS[6]`, six literal coordinates | **two loops** — four from (68,120), two from (196,197), each `add ax, 0x80` |
  | `SIGN_BASE + i * SIGN_STEP` | a **table at `CS:0x4F8A`**, read `mov si, cs:[di]` |
  | `RATING_BASE + (3 - rating) * STEP` | a **table at `CS:0x4EE7`**, indexed `shl bx,1` |
  | `LOGO_X/Y` plus `LOGO_SHADOW_DX/DY` | two blits at **literal** (0,20) and (8,10) |
  | `SIGN_SHADOW_DX/DY` | `sub ax,8` / `add bx,8` inside the loop |

  `CS:0x4F8A` holds exactly `38024, 41684, 45344, 49004, 52664, 56324` and
  `CS:0x4EE7` holds the four rating offsets less the 3,568 segment delta —
  both byte-for-byte what the fitted arithmetic produced. **That is the point.**
  The arithmetic was right about the numbers and wrong about the program, and
  the rating's had already been got backwards once for exactly that reason: a
  table cannot be reversed by accident.

  Still **0 differing at all four ratings**, and five dead constants are gone.
  `SIGN_POS` survives only because `menu_hotspots` needs boxes after the fact,
  and it is now generated from the loop's own parameters so there is one
  source of truth rather than two.

- **Correction: the transcription counts reported over the last several
  commits were too high.** `provenance.py` anchored its markers to a line
  start but did not allow the line that OPENS a comment block, so
  `/* NOT TRANSCRIBED ...` was invisible to it. Four functions that say so
  explicitly — `dat_load`, `ground_load`, `render_terrain`, `render_objects` —
  were being counted as transcribed on the strength of an address mentioned in
  their prose. The file was honest; the checker was under-reading it. Fixed,
  and the honest figures are **31 transcribed, 38 the port's own, 5 owed**.

- **Drawing a screen from a spec loses the call graph — and hides an
  overfit.** Boran's point, and it is the sharpest correction of the session.

  This port composed the menu from *measured geometry*: every position, size
  and offset fitted against captures until the screen matched. It reached
  100.00% and left **184 of 211 call targets never reached**, because nothing
  in that process ever followed the original's control flow. A transcription
  hands you its callees; a spec hands you a picture.

  So `tools/probe_blits.py` now records the **far return address on the stack**
  at the blitter's entry — the `lcall` pushes `CS:IP`, and reading it turns a
  list of rectangles into a call graph. **Twelve call sites**, immediately, and
  one of them was `draw_end_lemming`'s, which had been marked *"address
  unknown"* after both a pixel search and a geometry fit had failed to find it.

  Reading image `0x07E2F` then overturned something this file had asserted:

      mov si, 0x1c10 / mov ax,[0x21] / shr ax,1 / shr ax,1
      and ax, 0x0f / xchg al,ah / add si,ax / shr ax,1 / add si,ax

  **`frame = (scroll >> 2) & 15`.** `STATUS.md` had said the opposite — *"the
  frame does NOT follow the scroll… the lemmings run on a counter of their
  own"* — on the strength of a fit that never had the information: a capture
  pins the scroll only modulo 16, and this needs it modulo 64. Underdetermined
  was read as independent.

  **And that is how the overfit surfaced.** The port had been scoring
  **0 differing at every rating** using an *independent* frame — two free
  parameters where the original has one. With the true single parameter it
  scored 66–120, and the residue was entirely inside the two lemmings. Solving
  all four captures at once (the dash has period **8**, so only `scroll mod 8`
  is observable) gave one consistent constant: `frame = ((scroll + 4) >> 2) &
  15`. **Four is `ds:0x21`'s initial value**, image `0x07CC6`, which this port
  counts from zero.

  With it: **0 differing at all four ratings, on one parameter**, and the
  frames come out 14, 13, 15 and 2 — exactly the ones measured a session ago.

  The lesson is not that the fit was sloppy. It matched four captures to the
  pixel. It is that **a spare parameter buys a perfect score and hides a wrong
  model**, and only the code says how many parameters there are supposed to be.

- **The transcription debt: six down to two, and four of them were not debts
  at all.** Working the list found that in four cases *there is nothing to
  transcribe*, which is a better answer than a transcription:

  - **`ground_load`** — the original **never parses GROUNDxO.DAT.** It loads
    all 4,096 bytes raw to `ds:0x1B4B` (image `0x01A0B` names the file,
    `0x01A18` calls the loader at `0x01916`) and indexes the bytes in place:
    image `0x037D1` does `mul 0x1C` on the object id, `lea si,[0x1b4b]`, and
    reads the trigger box straight out of `[si+0x0E]` and `[si+0x10]`. The
    port's struct is a convenience; the *layout* is the transcription and that
    lives in the field comments.
  - **`vgagr_decode` / `vgagr_decode_mask`** — the original never unpacks these
    bitmaps either. It hands the planar bytes to the EGA and the card
    assembles the pixels; every blit `probe_blits.py` logs points `si` into the
    loaded section itself. Same class as `planar`, and moved to `sdl_io.c`
    with it.
  - **`draw_text`** — **transcribed, image `0x04D4D`, and it corrected a
    fitted rule.** That routine reads a three-byte header off the front of
    every string before drawing: `cl` = x in 16-pixel cells (with one added),
    `ch` = y in cells, then a flag. The credit lines carry theirs in the
    string table — `09 11` before *"(C)MCMXCI Psygnosis Ltd"* is x=128, y=272;
    `0C 12` before *"A DMA Design Game"* is x=176, y=288.

    Those are exactly the numbers this port used to reach by **centring and
    then subtracting half a character** — a rule invented to explain two
    coordinates that were never computed at all, and which this file recorded
    as a finding: *"both lines agree on that, which is why it is written as a
    rule rather than as two measured constants."* Two data points, a rule
    fitted to them, and it got them right. That is the whole trouble with it.

  What is left is genuinely owed: `draw_preview` and `draw_end_lemming`, both
  found by measuring the original's screen and its blits rather than reading
  its code.

- **The provenance convention had decayed, and is now a test.** `CLAUDE.md`
  says every function carries the address it came from, and calls it "the
  convention that decays first". Audited: **13 functions carried an address, 12
  said they were the port's own, and 45 said nothing at all** — most of the 45
  written by sessions that had the rule in front of them.

  Chasing them down found real addresses for fourteen more:

  | function | image | what pinned it |
  | --- | --- | --- |
  | `draw_char` | `0x04D78` | `cmp al,0x20 / je` — the space-skip the port already documented, and `(ch-0x21)*96` from `0x5BC0` |
  | `draw_at` | `0x04D4D` | walks a string calling `0x04D78`, stops on `0x0D` |
  | `menu_code_screen` | `0x04967` | stores `0x2E` ten times into `ds:0x4DF2` — the input field's dots |
  | `menu_input_screen` | `0x04A01` | second line conditional on `ds:0x1F73`, as the port's is |
  | `level_locate` | `0x01983` | reads the order table at `ds:0x54EE`, shifts out container and section, calls `digits3` at `0x019A0` |
  | `sign9` | `0x07C90` | the `sar bx,cl` that sign-extends the 9-bit y |
  | `menu_anim_reset` | `0x04FF8` | counter *i* starts at `0x28 * (i+1)` |
  | `scroll_char` | `0x07D33` | `0xFF` sets the pause, `0x0D` rewinds |
  | `draw_banner` | `0x07CE6`, `0x07D54` | the strip blit and the four-bit shift |

  Two are now marked **NOT TRANSCRIBED, address unknown** rather than left
  silent — `draw_preview` and `draw_end_lemming`, both found by measuring the
  original's screen and its blits rather than by reading its code. That is a
  different kind of knowledge and it now says so on the function.

  The checker takes a **file-level** declaration (`sdl_io.c` and `main.c` each
  say "THIS FILE IS THE PORT'S OWN" once) and lets a run of one-line accessors
  share one comment. Sixteen identical comments would be ritual, and ritual is
  what a convention decays into just before it decays out.

- **The five walk-on lemmings are transcribed, and no capture can check
  them.** Image `0x04F96` walks a ten-entry table at `ds:0x4960` (image
  `0x0E3F0`) — counter, reload, x, y, sprite base, eleven bytes each. Five
  entries are live and five are switched off with `0x8000`. The routine is
  small and completely determined:

  | counter | what happens |
  | --- | --- |
  | `0x8000` | switched off |
  | `>= 0` | still waiting — draw nothing |
  | `-1 .. -16` | `frame = (-counter - 1) >> 1`: eight frames, two passes each |
  | `-17` | reload |

  then a 32×12 four-plane blit at (x, y) from `base + frame * 192`. The
  multiply does not look like one — image `0x04FC4` shifts the index into AH
  and adds `frame<<7` and `frame<<6`.

  So each is on screen **32 passes out of 400–560**, about seven per cent of
  the time. This section used to say **that is why no capture of the original
  shows one** — and to treat the port agreeing to the pixel inside all five
  boxes as evidence, while being careful to say it only meant the port was
  right to draw *nothing* there.

  It was still a claim about a routine nothing had ever run. Driving the
  original to each of the four ratings puts entry 0 on screen at frames 4, 5
  and 6, and the compositing was **wrong**: these are drawn **opaquely**
  through four planes, their own black included, exactly as the banner's end
  lemmings next door are, and the port keyed on `v != 0`. Against those three
  captures the four candidate rules score 14, 22, 22 and **zero**.

  The counters are the original's own array at `ds:0x4960` (image `0x0E3F0`),
  eleven bytes an entry with the signed counter at `+0`. `verify_menu.py` reads
  them back and hands them to the port with `--anim`, the same way it hands it
  the banner's phase. They come back as **11, 51, 91, 131, 171** — spacing
  `0x28`, which is image `0x04FF8`'s stagger arriving as independent
  confirmation — and the five dead entries as `INT16_MIN`, which is the port's
  `ANIM_IDLE`.

  The lesson is the one this file already records about the rating plate:
  *a check at one value of a parameter says nothing about the parameter*, and
  "no capture shows this" is a reason to go and **reach** the state, not a
  reason to record the routine as settled.

  Since pixels cannot settle it, the counter logic is tested directly:
  `menu_anim_visible()` is exposed for that. Zero is a **valid** counter value
  meaning "walk on next pass", so a static array left at its default would put
  all five on screen at once on the first frame; that is the specific thing the
  first assertion catches.

  Sprite offsets are `bp - 29200` — the table's `bp` is an offset in the
  segment at `[0x1FEE]+0x800`, and 29200 is where section 4 begins in it,
  which is entry 0's `bp` exactly.

- **The banner is finished: bar, text, and both lemmings holding it.
  Zero pixels differing in its rows, on all four captures.**

  The two end lemmings were the last thing on this screen that had never been
  located. They were found by hooking the **shared blitter** rather than by
  looking for a sprite: `tools/probe_banner_ends.py` logs every call to image
  `0x8822` landing in the banner's rows, and two of them are not the strip —

      x=0    y=334  rows=16  planes=4  width=48
      x=592  y=334  rows=16  planes=4  width=48

  with the source in the segment at `ds:0x1FE4`, which image `0x04F96` sets to
  `[0x1FEE]+0x800`. That resolves into section 4, and the sprites decode as a
  lemming with **the bar's end cap built into the same 48 pixels**, dashed
  rows and all.

  That answers a question that had been open all along: why the painted bar
  runs 36..602 when the strip is only 48..607. **The caps are not the bar.**
  They are part of these two sprites, which is also what puts colour 12 in the
  margins and the stripe of colour 6 at x=592.

  16 frames, 384 bytes each; left at 10752, right at 10752 + 6144. Every
  capture matches a frame **exactly, zero differing, on both sides**, and in
  each the two are on the same frame: Fun 14, Tricky 13, Taxing 15, Mayhem 2.

  The frame does **not** follow the scroll. Fitting `frame = scroll/4 + c`
  gives a consistent `c` for three captures and none for the fourth, so the
  lemmings run on a counter of their own; its rate is not established and the
  port's choice of one frame per displayed frame is marked as a choice.

  | rating | whole screen | the banner's rows |
  | --- | --- | --- |
  | Fun | **99.85%** | **0** |
  | Tricky | **99.85%** | **0** |
  | Taxing | **99.85%** | **0** |
  | Mayhem | **99.85%** | **0** |

  What is left is **326 pixels, identical at every rating**, none of them in
  the banner — the long-standing residue around the plates.

- **The scrolling banner is drawn, text and all, from the game's own
  artwork.** It was the menu's largest gap and it is now the smallest.

  The mechanism is readable at image `0x07CBC` and `0x07D0D`: a strip of 35
  columns of 16 pixels, four planes, 70 bytes to a row; **4 pixels a step**; a
  new character each time that crosses 16; `0xFF` in the text is a **100-step
  pause**, not a glyph; `0x0D` rewinds.

  Three things fell out of reading it that no amount of pixel-fitting would
  have found:

  - **The bar's artwork is a 16×16 tile at section 4 offset 23040** — the
    original's `ds:0x4C10`. Colour 13 except rows 2 and 13, which run
    `0000dddd0000dddd`: the two dashed rules.
  - **So the dashed rules scroll.** They are not a function of x. Four
    captures give phases 0, 0, 4, 4 mod 8. Any fixed rule fitted to one
    capture is wrong on the next — which happened here twice, once as
    `(x & 7) >= 4` and once, earlier this session, as `(x & 7) < 4`. Both
    scored well on a count and were measuring nothing.
  - **The scroller's font is the menu's own font.** `0x5BC0 + 3568 - 0x21*96`
    is 23888, which is `FONT_BASE`. It had been recorded as "a different
    font".

  The text is at image `0x0E45E`, 266 bytes: *"Lemmings By DMA Design …
  Programming By Russell Kay … Copyright 1991 Psygnosis Ltd."*

  **Text enters at the right.** At start-up all 35 columns are bare template,
  so composing at scroll 0 with the ribbon already in place puts the first
  credit on a bar the original still has empty. That direction cost 3,076
  differing pixels against 62 — and it is why every capture taken so far shows
  an empty bar.

  | rating | scroll it was actually at | whole screen |
  | --- | --- | --- |
  | Fun | 0 | **99.70%** |
  | Tricky | 4 | **99.70%** |
  | Taxing | 4 | **99.70%** |
  | Mayhem | 0 | **99.71%** |

  The model **predicts** those phases rather than being told them, which is
  the check that it is a model and not a fit. What is left inside the bar is
  62–72 pixels near x 588–604 — the two end lemmings, still unlocated, which
  are also what puts colour 12 in the margins and a stripe of colour 6 at
  x=592.

  `menu_run` animates it. The **step rate is inferred, not measured**: the
  4-pixel step and the 100-step pause are read off the code, but how often the
  original steps is taken from its menu loop waiting on the display once a
  pass, so the port steps once per displayed frame and says so.

- **All four rating plates are verified against the original, and the sprites
  ran the wrong way.** Only Fun could be checked before, because only Fun could
  be reached. With the plate working, the original can be driven to each rating
  in turn, its plate cropped out of the capture at (486,145), and section 4
  searched for the 72×27 four-plane block that decodes to it:

  | rating | offset in section 4 | pixels differing |
  | --- | --- | --- |
  | Mayhem | `23168` | **0** |
  | Taxing | `24140` | **0** |
  | Tricky | `25112` | **0** |
  | Fun | `26084` | **0** |

  A uniform 972 apart and **descending** — the four are stored in reverse. The
  cross-check is a clean diagonal: each capture scores 0 against its own sprite
  and 616–736 against the other three.

  The rating-dependent content is reproduced **exactly**: inside the plate's
  box the original changes **713** pixels between Fun and Mayhem, and the port
  changes the same **713**.

  Whole-screen scores run 99.41% (Fun, Tricky, Taxing) to 99.04% (Mayhem), and
  that spread is **the animated banner alone** — a single composed frame cannot
  match it, and successive captures of the *same* rating vary by as much. The
  plate's own box holds a **70-pixel residual that is identical at every
  rating**, so it belongs to the long-standing unexplained plate pixels rather
  than to the rating work. An earlier note here attributed those 70 to Mayhem;
  that was wrong — Fun has exactly the same 70.

  The port had `RATING_BASE + rating * RATING_STEP` with the base set to Fun,
  which is right for exactly one of the four values it takes. **At rating 0 the
  sign of the step does not matter**, and rating 0 was the only rating
  reachable, so nothing could contradict it. This is the general hazard in a
  reconstruction verified against one sample of a parameter: the check passes
  and says nothing about the parameter.

- **The menu capture now reports which rating it is showing.** `verify_menu.py`
  takes `--rating` and reads `ds:0x1F6E` back out of the guest before scoring.
  This was not caution — it caught a real fault the same hour it was written.
  An earlier version pressed Up n times and believed it; its rating-2 and
  rating-3 captures **both showed Tricky**, one press each, and the port was
  scored against the wrong screen. That read as the port drawing Taxing and
  Mayhem badly, 99.10% and 98.71%, when it was drawing them correctly against a
  screen that was not showing them. It now holds the key across 24 frames
  rather than 4, retries up to three times, and scores against whatever the
  guest actually reached rather than what was asked for.

- **Correction: the rating plate works, on Up and Down.** This entry used to
  read *"answers to nothing found, and 25 keys have now been tried"*. That was
  wrong, and it stayed wrong because every instrument used to test it was
  blind in the same direction.

  The keys came from the binary instead. The menu translates a scancode
  through a table at `ds:0x202E` (image `0x0BABE`) and dispatches on the result
  at image `0x0447B`: **Up (`0x48`) and `` ` `` (`0x29`)** reach the rating-up
  handler at image `0x044EC`, **Down (`0x50`), `-` (`0x0C`) and keypad `-`
  (`0x4A`)** reach rating-down at image `0x0451C`. Up caps at 3, Down floors at
  0 — it does not wrap — and both then write **`ds:0x1F6B = rating * 30`**,
  computed with four shift-and-adds. `ds:0x1F6E` is the rating itself: image
  `0x045D5` multiplies it by 6 and copies six characters out of
  `"Fun   TrickyTaxingMayhem"` into the plate's text.

  `tools/probe_rating.py` now drives all five keys against the original and
  watches the variable rather than the plate's pixels:

      up    0->1->2->3->3->3->3   Mayhem  ok
      down  3->2->1->0->0->0->0   Fun     ok

  with `ds:0x1F6B == rating * 30` at every sample. That is also the binary's
  own confirmation that the level order table is **four blocks of thirty**.

  Three faults produced the old claim, and they are the general ones:

  1. **The sweep's key list contained neither `up` nor `down`** — the two keys
     the prose named as ruled out.
  2. **Four listed keys were never delivered.** The control socket resolves
     names through `pygame.key.key_code`, which names punctuation by the
     character (`` ` ``, `-`, `[-]`, `[+]`), so `"minus"`, `"plus"`,
     `"kp_minus"` and `"kp_plus"` returned `error: unknown key name` — printed
     as though the key had done nothing. **"Not delivered" and "delivered, no
     effect" are different findings.**
  3. **It watched pixels from the floor.** The rating starts at Fun = 0, where
     down is *supposed* to do nothing, so a correct key looked inert.

  The port maps all four keys now (`MENU_RATING_UP` / `MENU_RATING_DOWN`), caps
  and floors as the original does, and resets to level 1 of the new rating.
  `reconstruct/tests/menu_input.c` checks the mapping headless.
- **The menu's plates are keyboard-only, and a click anywhere acts as F1.**
  Measured three ways: the menu calls INT 33h AX=0002h once — hide cursor — and
  draws no pointer, so moving the position to (600,40) changes nothing near it;
  it polls AX=0003h 1342 times, so the position reaches it and is ignored; and
  clicking "EXIT TO DOS" starts the level rather than exiting, exactly as
  clicking bare background does. The port had per-plate click boxes, which was
  behaviour the original does not have — a worse kind of wrong than leaving it
  out — and they are gone.
- **The briefing's background is not byte-identical to anything in the files,
  and is drawn through the ordinary blitter from a run-time buffer.** What is
  established, with a control: two lit captures from separate runs are
  identical pixel for pixel, so it is deterministic; and a search of every data
  file *and* the executable at all eight bit alignments finds **0** matches for
  it, while the same search finds **298** for the menu's background, which is
  stored. `tools/probe_briefing_bg.py` runs that argument including the
  control.

  **Correction (same session).** That was written up as "generated from a fixed
  seed", which overstates it. Watching writes into the EGA window across the
  transition shows the screen is cleared by image `0x04E34` (a `rep stosb` with
  all four planes enabled) and then drawn by the blitter at
  `0x089C4`–`0x098D4` from a **buffer in memory**. So it is data at the moment
  it is drawn. Whether that buffer is filled from file data by some transform —
  which a byte search would not catch — or generated, is **not settled**. The
  buffer has since been narrowed. Logging every blit and **resetting the log on
  each full-screen clear** — so what survives is the last screen drawn, rather
  than the biggest blit, which was the trap that caught the logo — puts the
  briefing's background at source pages **`0x58000`–`0x59FFF`**, 21,488 blits
  through entry `0x0988A` with **`cx=40, bp=2`**: a 320-pixel-wide, two-plane
  tile, the same shape as the menu's. The font glyphs on the same screen come
  from `0x6E000`–`0x70FFF` with `bp=3`, which matches the font's three planes
  exactly and is a good check that the reading is right.

  Grabbing the source *at* the first wide blit after a clear — the timing the
  two earlier dumps got wrong — shows the buffer at `0x587E0` begins
  `f1 80 07 fc 00 00 05 f7`, which is **MAIN.DAT section 3 byte for byte**: the
  menu's own graphics, loaded there. But the briefing's background is **not**
  the menu's tile: `tile|8` scores 1.65% at its best vertical phase, and plain
  `tile` scores 48% which is only the black agreeing.

  **So the dig is parked here**, with the honest state written down rather than
  a guess: the source buffer holds section 3, the background is somewhere in or
  derived from it, and it is not the tile at offset 0. Section 3 is accounted
  for as background `0..8319`, logo `8320..38023`, signs `38024..59983` — with
  **1,984 bytes unexplained at the end**, too few for another 320×104 tile.
  This is a cosmetic element on a screen whose text is already 97.99%, and it
  has now taken more of this project's time than the whole menu did.
- **The briefing and the two sub-screens dismiss on a click**, which is what
  the briefing's own text asks for — "Press mouse button to continue". Esc does
  too; that is the port's addition, and marked as such.
- **The port leaves that background black**, deliberately. Drawing the menu's
  tile there would score 66% and look about right, which is the failure worth
  avoiding: it invites the screen to be called finished. Palette entries 8–15
  are the same gap — on that screen they are the level's terrain colours.
- **The banner's BAR is drawn; its scrolling text and end lemmings are not.**
  The search for a bar sprite was the wrong search: logging the blits that land
  in those rows finds the source at image `0x191BC`, in BSS, and dumping it
  shows rows of `0xFF`, `0xF0` and `0x00` — **solid fills**. The bar is a strip
  the game fills and then renders scrolling text into, so there was never a
  bitmap in a file to find. It is drawn instead, from geometry measured off the
  original: x 36–604, y 334–349, colour 13, with dashed rules at y 336 and 347
  where the colour appears only where `(x & 7) >= 4` — 499/500 and 495/500 over
  x 100–600, the misses being the scrolling text passing through.

  With that, and with the FX plate's label (below), **the whole menu is
  composed: 95.83% over all 224,000 pixels with nothing excluded**, up from
  91.28% and from a 95.26% that had the banner scored out. A better number over
  a bigger area.

  What the three shapes in those rows are, which is how the fill was found:

  | shape | count | source | rows | what it is |
  | --- | --- | --- | --- | --- |
  | `cx=40, bp=2` | 20 | `0x58AB0`, step 40 | 330–333 | the **menu tile**, above the bar — and it checks out: `0x58AB0` is section 3 offset 720 = tile row 18, and 330 mod 104 = 18 |
  | `cx=70, bp=4` | 1,584 | `0x292BC`, step 70 | 344–349… | the **bar** — 560 pixels wide, four planes, from image `0x191BC`, which is inside the BSS the program fills at load time |
  | `cx=6, bp=4` | 3,136 | `0x6A000`–`0x6D000` | — | the **scrolling text**, from a different font than the menu's 16×16 one |

  The bar's row in BSS turned out to be solid fill, not a picture — which is
  why it is now drawn rather than hunted. The scrolling **text** still is not:
  it uses a different font from the menu's 16×16 one, and it scrolls, so a
  composed frame cannot match it anyway. The two lemmings holding the ends
  (x 12–35 and 605–628) are sprites that have not been located.

  Correction worth recording: the menu's animation is **not** only the
  scroller, as this file once said — a no-key baseline moves 1,381–1,807 pixels
  in a box from y=49 down, so the logo and the plates animate too.
- **The menu font is found and used.** 16×16 glyphs, three 32-byte planes,
  ASCII-indexed, at `MAIN.DAT` section 4 offset 23888 — which ends at
  `23888 + 127*96 = 36080`, exactly the section's length. Plane 2 doubles as
  the opacity mask. Reading the mask as the *first* plane scores 51%; it is the
  last, and then it is 100.0% over both credit lines. One behaviour came with
  it: the original draws **nothing** for a space rather than relying on an
  empty glyph, which is the whole difference between 94.3% and 99.3%.
- ~~**What maps a level number onto a file and a section is not known.**~~
  **Solved.** The order table is at image `0x0EF7E`, 120 bytes,
  zero-terminated — four ratings of thirty — and the loader at `0x01983`
  decodes each entry as `file = (n-1) >> 4`, `section = ((n-1) >> 1) & 7`.
  Three independent checks agree: the 120 entries cover all 80 `(file,
  section)` slots with 40 referenced twice; `ODDTABLE.DAT` holds 80 records of
  which exactly 40 are marked "non-used duplicate" — the same 40; and entry 0
  is `level009` section 1, which is the level the original's briefing calls
  "Level 1". The first level of each rating comes out as the real game's:
  `Just dig!`, `This should be a doddle!`, `` If at first you don`t succeed.. ``,
  `Steel Works`. See [docs/menu.md](docs/menu.md).

  The port now takes `--rating fun|tricky|taxing|mayhem` and `--number 1..30`
  and resolves them through that table.
- **The C port's file-format code is written from published descriptions**, not
  transcribed from the disassembly. It is checked against the files and against
  the briefing screen, which is real evidence but not the standard the method
  asks for. Provenance is marked in every such file.
- **The unpack is not yet *proven*.** The skill's standard is a round-trip
  against the stub's own output, and that has not been done. "It produces
  plausible code and the program runs" is evidence, not proof. Nothing has been
  transcribed on the strength of it.
- ~~**The second start-up menu does not respond to RETURN.**~~ **Retracted
  2026-08-25.** Not an input problem at all: the menu was waiting on a vertical
  retrace, reading the CRTC base port from BIOS variable 0040:0063, which the
  emulator left at zero — so it polled *port 6* twenty-six million times for a
  bit that could never arrive. Fixed upstream by filling in the BDA variable.
  Recorded here because the first diagnosis was wrong and a status file that
  only ever gains claims is worth nothing.
- ~~**No unpacked image has been written to disk**~~ **Done 2026-08-25** —
  `image/vgalemmi.img` is the raw unpacked image and
  `image/vgalemmi.decrypted.img` is the same with the protected stretches
  recovered, both committed. A **code map and a coverage table are still
  absent**: the figures below are not yet measurements.
- ~~**Toolchain unidentified**~~ **Answered 2026-08-25** — hand-written
  assembly, see [docs/toolchain.md](docs/toolchain.md) and the note at the top.
- **The protection is only reachable on the floppy path**, so an ordinary run
  recovers none of it. Understood and handled — `tools/decrypt_image.py`
  records a deliberate `media=0xF9` run — but it means **anything measured on a
  run that never touched the protection says nothing about it**, which is how
  the earlier "108 INT 01h events per start-up" claim came to be wrong. See the
  correction in [docs/protection.md](docs/protection.md).

### Coverage, as last measured

**Code map** — real measurements now, written by `tools/codemap.py`
([docs/codemap.md](docs/codemap.md)):

| | |
| --- | --- |
| code region (0 .. DGROUP `0x9A90`) | 39,568 bytes |
| decoded by recursive descent | 33,875 bytes (**85.61%**) |
| of that, **proven executed by a run** | 19,067 bytes (**48.19%**) |
| call targets found | 211 |
| call targets no run ever executed | 44 |
| offsets that would not decode | 1 |

Only the proven figure is evidence; the rest is a hypothesis that those bytes
are code. Descent from the entry point *alone* reaches 66.47%, so the recorded
executions are worth about 19 points.

**Transcription** — separately, and this is the number that matters for the
port:

| | |
| --- | --- |
| routines transcribed | 33 (`make test` enforces the count) |
| routines proven (ran, did work, agreed) | 7 |
| routines shallow (agreed, but every call was an early return) | 0 |
| routines differing | 0 |
| routines never reached | 184 of the 211 call targets |

| routine | what it is | how it is checked |
| --- | --- | --- |
| `0x01BAF` | DAT decode loop | `tools/verify_dat.py` — 7 calls, 150,807 bytes, byte-identical |
| `0x01CA3` | the bit reader | ” |
| `0x01C82` | emit a literal | ” |
| `0x01C87` | copy a back-reference | ” |
| `0x036B6` | LVL header parse | `tools/verify_level.py` — 15 fields, all agree |
| `0x09347` | the sprite **pre-shift**: copy a row, append a zero byte, shift right N bits | `tools/verify_shift.py` — 200 calls, byte-identical, over shifts of 2, 6 and 7 |
| `0x05B11` | the level's **sprite pre-builder**: all eight alignments at once, each with a per-column OR | `tools/verify_preshift.py` — 106 calls, byte-identical |
| `0x04BC2` | **three-digit decimal**, used to build the `levelNNN.dat` filename | `tools/verify_digits.py` — the original's own code driven over **all 256 byte values**, all agree |
| `0x0447B` | the menu's **key dispatcher**, with the scancode table at `ds:0x202E` its reader at `0x04D1A` translates through | every key it maps, driven against the original (`tools/probe_rating.py`) |
| `0x044EC`, `0x0451C` | **rating up and down** — cap at 3, floor at 0, and `ds:0x1F6B = rating * 30` | the same, watching `ds:0x1F6E` |
| `0x04F96` | the **walk-on animation driver**: ten entries, counter down past zero, eight frames held two passes each | the counter logic directly — no capture shows one, see above |
| `0x07CBC`, `0x07D0D` | the **banner**: set-up and one scroll step, with the 4-pixel shift, the 16-pixel character cell, the `0xFF` pause and the `0x0D` rewind | the banner's rows, **0 differing** at all four ratings |
| `0x0890E`, `0x0909C` | the shared blitter's **compositing rules** — the sprite's four passes and the shadow's set/reset fill. Not the whole routine; the rule it implements | the whole menu, **0 differing** at all four ratings |

These verifiers work the same way, and it is the method's way rather than a
screenshot: stop the original, capture the inputs, let **its own body** run,
compare its output against the port's C on that same call. None of them needs
determinism to mean anything.

`0x04BC2` adds a variant worth reusing. The game calls it **once per level
load**, so riding along with `play.py` yields exactly one sample — value 9,
agreed, and thin. But the routine is **pure**: it reads `AL` and writes three
bytes at `DS:SI`, touching nothing else. So the original's own code can be
*driven* with inputs of our choosing — point `CS:IP` at `0x04BC2`, set `AL`,
run to the `ret` at `0x04BE6`, read the three bytes back — which turns one
sample into all 256. That is still the original's arithmetic answering, not a
model of it.

The check that this is not vacuous is in the tool's own history: swapping the
tens and units digits in the port makes it report `DIFFER (1, b'001', b'011')`
— the original's bytes on the left. A harness that had quietly failed to run
the guest would have compared zeros against zeros and passed.

One caveat on `0x05B11`, stated because the number looks stronger than it is:
all 106 calls pass the **same shape**, 10 columns by 2 rows. That is not a
limitation of the test — driving into a level with `play.py` finds no other
shape either — it is the only shape the game gives that routine. So the
transcription is proven on what the game actually does with it, and untested
outside that.

Stated precisely, because the distinction matters: the four DAT routines are
proven *as a unit*. `0x01C82` and `0x01C87` are folded into the loop body in C
and are not separately compared, so what is checked is their composite
behaviour.

Everything else in the port is still written from the file formats rather than
transcribed — the object table, the steel table, the level name, the graphics
sets, the rendering. That is the great majority of it. The port rendering the
first level exactly and the port being a transcription are different claims,
and only the first is met for the rest.

Also settled while mapping: **there are no dispatch tables in this binary** —
zero indirect calls or jumps through memory, six register-indirect sites in
total, no jump tables. The usual "seed descent with the handler tables" step
has nothing to seed with here.

## Next

1. **Draw the interactive objects as sprites, not as terrain.** The terrain is
   exact. The objects are the remaining visual difference, and the earlier plan
   for them was wrong: breaking at image `0x03789`, the final `ret` after all
   five composition passes, shows the original **never composes objects into
   the level bitmap at all** — it draws the entrance and the exit as animated
   sprites at display time. `tools/prove.sh` passes `--no-objects` for exactly
   this reason, and the port's 807-pixel difference with objects enabled is
   precisely those three objects painted into a bitmap they do not belong in.
2. ~~**Write a code map and a real coverage table.**~~ **Done 2026-08-25** —
   `tools/codemap.py`, `image/codemap.json`,
   [docs/codemap.md](docs/codemap.md). The plan said "seeded with the handler
   tables"; there are none, which the map establishes rather than assumes.
   What remains is reading the six register-indirect sites, the only dispatch
   static descent cannot follow.
3. **Keep transcribing.** 211 call targets are enumerated, 13 are transcribed
   and verified. `tools/hotspots.py` ranks the rest by weight, which is how the
   decompressor was found: inside the windows following an INT 21h AH=3Fh read
   it is 83% of all instructions executed. `LEM_HOOK` lets a verifier ride
   along with `tools/play.py`, which is what it takes to check anything that
   only happens inside a level.

   By weight the next is `0x05B11`, a horizontal sprite pre-shifter — it reads
   two bytes, shifts them right by 0..7 with the carry into a third, and writes
   three. Worth having, but it is off the level-drawing path. On that path the
   next are the four composition passes `0x0378A`, `0x03839`, `0x037C2` and
   `0x0389F`, which is what `render.c` should be checked against.
4. **Prove the unpack by round-tripping** against the PKLITE stub's own output.
   Still the standard, still not done.

- **The machine-type menu is mapped but not explained.** Which flags each of
  the four options sets is established and written down in
  [docs/machine-types.md](docs/machine-types.md); *what those flags change* is
  not. Reading every use of `ds:0x200A/0x200B/0x200C/0x200E`, and confirming
  whether the PS/2 option really patches the retrace path at `ds:0x1FC6`, is
  outstanding work we intend to do — noted here so it is not mistaken for
  finished.

## Deferred

- **CGA and Tandy builds** — non-goals, as above.
- **The PC compatible, PS/2 and Amstrad machine types** — non-goals. What their
  flags gate is still worth *identifying* (it is in Open), so the port can carry
  honest no-ops rather than silent gaps, but the behaviour is not being ported.
- **The EGA path** through `vgalemmi.exe`.
- **`flr.exe`** (the crack's trainer), `install.exe`, and the `lem*.com`
  loaders — not part of the game.
- **Sound.** Not looked at; `adlib.dat` and `tandysnd.dat` are unexamined.

## Provenance notes

- `lemming1.pc/` is a cracked repackaging (`lemmthg!.doc`, `crue.inf`,
  `LEMMINGS THG` in the `.com` loaders). The `.com` loaders and `flr.exe` are
  the cracker's work, **not** Psygnosis's, and nothing from them should end up
  in the reconstruction.
- `lempass.txt` is a Lemmings **2** password table, dropped in by the
  repackager. It is not about this game.
- The shipped documentation (`lemming.doc`, `lem1-4.doc`) is a **primary
  source** — the authors' own word on the keys, the skills, and the command-line
  switches. `docs/` should say which claims were confirmed against the binary
  and which are still inherited unverified.
