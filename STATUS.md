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

- **Correction: this is not a ten-level demo.** The project described it that
  way from early on, in `CLAUDE.md` and in the port's own header. It is a
  misreading of "ten level *files*": `level000.dat` .. `level009.dat` each hold
  **eight** 2048-byte levels — **eighty** — with names from across the full
  game's ratings (`Tightrope City`, `Mary Poppins\` land`, `Every Lemming for
  himself!!!`). Counted with `tools/dat.py`. It surfaced only because a
  *different* claim leaned on it — "the rating plate does nothing because there
  is only one rating here" — and that reasoning had to be checked before it
  could be written down.

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
  | the rating plate | no response to Up **or Down** — Down's screen diff is identical to the no-key baseline | no key mapped, deliberately |

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
- **The rating plate answers to nothing found, and 25 keys have now been
  tried** — F1–F6, Esc, Up, Down, Left, Right, PgUp, PgDn, keypad ±, ±, Space,
  Return, Tab, 1–4, `n`, `r` — plus a click, whose position the menu ignores
  anyway. Every one leaves the plate's box at (452,120)–(571,180) untouched at
  **zero pixels**, while the scroller keeps moving in the same samples, which
  is what shows the guest was alive and taking input. `tools/probe_rating.py`
  does the sweep in one guest and stops if the whole screen changes.

  Why it is inert is **not established**, and the obvious explanation is ruled
  out: this is not a one-rating set. The data ships **ten level files of eight
  levels each — eighty levels** — with names spanning "This should be a
  doddle!" and "MENACING !!". So "nothing to select" does not hold. The port
  maps no key to the plate rather than inventing one.
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

  With that, **the whole menu is composed: 95.04% over all 224,000 pixels with
  nothing excluded**, up from 91.28% and from a 95.26% that had the banner
  scored out. A better number over a bigger area.

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
- **What maps a level number onto a file and a section is not known.** The
  game's Level 1 is section 1 of `level009.dat`, found by reading the
  original's briefing screen — not derived. `oddtable.dat` is an unexamined
  candidate.
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
| routines transcribed | 5 |
| routines proven (ran, did work, agreed) | 5 |
| routines shallow (agreed, but every call was an early return) | 0 |
| routines differing | 0 |
| routines never reached | 206 of the 211 call targets |

| routine | what it is | how it is checked |
| --- | --- | --- |
| `0x01BAF` | DAT decode loop | `tools/verify_dat.py` — 7 calls, 150,807 bytes, byte-identical |
| `0x01CA3` | the bit reader | ” |
| `0x01C82` | emit a literal | ” |
| `0x01C87` | copy a back-reference | ” |
| `0x036B6` | LVL header parse | `tools/verify_level.py` — 15 fields, all agree |

Both verifiers work the same way, and it is the method's way rather than a
screenshot: stop the original, capture the inputs, let **its own body** run,
compare its output against the port's C on that same call. Neither needs
determinism to mean anything.

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
3. **Keep transcribing.** 211 call targets are enumerated, 5 are transcribed
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
