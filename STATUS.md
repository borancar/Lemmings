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
| **the game's own screen**, all 5,100 frames of the expiration timer | play area **exact — ZERO differing — on 5,099 of 5,100, THE CURSOR INCLUDED**; panel exact on 5,099 (frame 0 is the fade-in). The one play frame is 40, and the cause written here was wrong: it is not a `MOUSEMOTION` from the dummy video driver. Traced at the setter, the writer is `click_mouse` over the control socket — `tools/lemmings.py`'s own start-up driver, which clicks at (160, 100) to get through the title and the briefing, and one of those clicks lands after the capture has begun. The recorder's per-frame re-assert cannot cover it because the capture and the re-assert are different hooks and the stray write falls between them. `LemmingsVga.pin_mouse()` now makes every writer but the script a no-op while one drives; a 535-frame recording taken with it has the guest at (160, 100) on **0 frames of 535**, where it was on one. This row's reference (`out/ref5100b`) predates the pin, so the 5,099 stands until it is re-recorded — the instrument, not the port. This is the WINDOW path, what a player sees, not the harness (`tools/compare_screen.py` against `out/ref5100b`) |
| the original's composed **level terrain** vs the port | **100.00%** — 49,741 pixels, 0 differing (`tools/prove.sh`) |
| the port's **data loading** vs the original's RAM | **7 sections byte-identical, 0 differing**, 11 freed by the game and so not resident — **re-run on this build** (`verify_all.sh --units`, which needs `out/mem.bin`; the recipe in that tool's header used to carry a `--shots 1` that ends the run after 1.6 seconds) |
| the **level-code** screen | **100.00%** — 0 of 224,000 |
| the **input chooser** | **100.00%** — 0 of 224,000 |
| the **briefing** | **100.00%** — 0 of 224,000: background, text, palette, and the level preview |
| the **main menu** | **100.00%** — zero pixels differing, at all four ratings, **with one of the walking lemmings on screen** in three of the four. Background, logo, all six plates, both drop shadows, the rating name, the FX label, the credits, the whole scrolling banner, its two end lemmings, and the animation. This figure needs the two sides to agree about the SOUND: image 0x04E5A picks the FX label from `ds:0x25A7` and `ds:0x2010`, the emulator answers the AdLib detect and `lemdev` loads no driver, so a run that does not carry those two bytes across scores 728 — one label, and nothing else on the screen. `verify_menu.py` reads them out of the guest and passes `--snd`; it did not, and the 728 read as the port's |
| **seven further levels**, frame by frame for over a minute each | every one **exact — ZERO differing, worst 0** — play area AND panel: `level001.dat --level 3` 530 of 530, and `level002.dat --level 4`, `level002.dat --level 6`, `level004.dat --level 3`, `level006.dat --level 3`, `level007.dat --level 1`, `level009.dat --level 1` all 1,100 of 1,100 (`tools/verify_all.sh`). This row read **488 of 530** and **1,071 of 1,100** for far longer than it was true, naming 42 frames of opening cursor and 28 frames of three pixels at x 319 as what was left. Both were the HARNESS and both are gone: verify_all.sh aims the port's cursor at the guest's own, since otherwise 56 pixels of cross-hair drown everything else, and it passes the odd-table entry. A headline table that says it is "re-measured on the current build" has to be, or it understates the port and nobody notices |
| **the nuke**, driven by a double click | record **0 of 557 frames**; screen exact on **541 of 557**, the residue being 16 frames of 12 pixels - the crater's top row, which the original's incremental display has not repainted yet |
| the DAT decompressor vs the original's own body | 7 calls, 150,807 bytes, byte-identical — **re-run on this build, 0 differing** (`verify_all.sh --units`) |
| the LVL header parse | 15 fields, all agreeing — **re-run on this build, 0 differing** (`verify_all.sh --units`) |
| the sprite pre-shift vs the original's own body | 200 calls, byte-identical (`verify_all.sh --units`) |
| the level sprite pre-builder vs the original's own body | 106 calls, byte-identical (`verify_all.sh --units`) |
| **the pointer's hit test** vs the original's own answer | **600 frames, 600 agreed — 256 of them with the pointer actually ON a lemming**. The guest sets `ds:0x1A` when its scan finds one, so that byte settles it (`tools/verify_hit.py`). The earlier figure here, 326 of 326, was a SHALLOW PASS: not one of those frames had the pointer over a lemming, so what it checked was that both sides say “nothing” when pointing at sky |
| **the digger** vs the original digging | state **138 frames agreed**; the shaft it cuts **1,056 pixels, 1,056 agreed** (`tools/verify_dig.py`). A second trace agrees on **328 of 328** frames and then STOPS: the port is re-simulated on terrain alone, so from the frame the original's lemming reaches the exit the two sides are not simulating the same thing, and the tool says so rather than scoring it |
| **pace** | the level 170 frames in 10.01s against 17/s; the menu 350 passes in 5.01s against 70 Hz (`tools/verify_pace.py`) |
| **the DATA SEGMENT**, field by field, over a recorded playthrough | **45 of 52 fields agree on every one of 40 frames, and nothing differs that is not accounted for** (`tools/verify_dgroup.py --script out/me.txt`). This is a different claim from every picture above: `dgroup.h` lays the port's globals out at the original's own `ds:` offsets, packed, with named padding runs for the bytes nobody has read, so the two segments can be compared as memory rather than as a screen. The seven that differ are the harness — the briefing's `needed_pct`, the tune with no sound driver, the menu's two plate guards and its animation counters, the timer's `ds:0x1FCD` count this port substitutes, and the fade — and each carries its reason in the tool rather than in prose here |
| **a recorded playthrough**, the whole level to its end | **534 of 534 frames exact — ZERO differing, play area AND panel, worst 0**, with the cursor scored rather than excused (`tools/compare_screen.py --ref out/me_ref535`). Both sides read one recording of real play: 80 lemmings released, all eighty dead, the level ending itself at 535. The scorer's control is `--frame-offset 0`, which scores **0** of 534 exact — so the 534 is a result and not an insensitive instrument. **What this run does NOT cover is a skill**: the recording's clicks move `selected` but never spend one, so the skill vector is `[20]*8` at the first frame and at the last. That case is `tools/compare_play.py`'s, which assigns a digger and refuses to report a figure for a run where it was not |
| **both sides PLAYING**, a digger assigned from one script | play area exact on **568 of 671**, and every one of the other 103 frames differs by **exactly one pixel, always canvas (799, 81)**; panel exact on **671 of 671, worst 0** (`tools/compare_play.py --frames 700`). The panel read 661 until the two counts were put where the original computes them: OUT is image 0x3684's, the tail of the walk image 0x01BC calls, and IN is image 0x03ACE's, inside the panel image 0x01C2 calls — both AFTER image 0x01B3 steps the lemmings, where the port had them in its pre-step routine. A lemming reaching the exit therefore showed in the percentage a frame late, which is ten frames of one character cell and nothing else. The positive control fired: the original's own digger count falls 10 → 9 at frame 113. **The one pixel is not the port's.** Probed in the guest, the original's MASTER terrain at image 0x05DEA's `0x4A40` goes 14 → 0 on frame 121 — the digger clears it, exactly as the port does — while its displayed PAGE reads **2** from frame 122 on, and the page not being displayed reads 0. So the master agrees and the page carries something the master does not; the reason is read rather than guessed: **image 0x080FD, which image 0x001A7 calls, is a DIRTY-RECTANGLE blitter.** It scans a 20x40 array of flags at `ss:[ds:0x26]` — `repne scasb` for a set one, `repe scasb` for the run's length — copies only those 8x8 tiles from the master at `0x4A40` to the page at `ds:0x1F76 + 0x2C2`, and clears all 0x320 flags at image 0x08153. So the original's page is maintained INCREMENTALLY and keeps whatever no flag asked it to refresh. (Image 0x001AA's `0x7900` is not that window: it copies 13 bytes by 19 rows, and this file used to name it as the terrain window.) This port recomposes the whole play area from the master every frame, which reaches the same picture everywhere a flag was set. **And which writer leaves it stale is read too**: image 0x09A15 marks the tiles a rectangle covers, and the code map gives it ONE caller — image 0x0885B, inside the blitter, gated on `ds:0x2A != 0xFF`. So a blit marks what it wrote; the digger's clear at image 0x05EBA is a bare `stosb` run into the master with no call in it, marks nothing, and the page keeps what it had. The stale 2 is the ORIGINAL's behaviour rather than a fault in it. Modelling it means the flag array, image 0x09A15 on every blit, image 0x080FD's tile copy, the page pair and image 0x07178's unmasked 24x16 background save — **owed**, and now specified rather than half-read |
| tests | `make test` 134 checks across two binaries, `make sanitize` clean under ASan + UBSan |

**Transcription is a different claim from any of the above, and it is 153
routines with an address, 56 marked the port's own and 7 still owed, with
NONE unaccounted for** — counted by `reconstruct/tests/provenance.py`, which reads
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

- **The code screen's caret is drawn, and it was never a bar.** This section
  described it as "a bar over the first code slot", which is what an
  underscore looks like when you have only ever seen it in a capture.

  Image `0x04C7E` is the entry loop and image `0x04CB8` is the whole of the
  blink: every `0x14` retraces - the counter at `ds:0x491E`, reloaded at image
  `0x04CB3` - it swaps the cell the cursor is on between `'.'` (0x2E) and
  `'_'` (0x5F). Image 0x04C92 keeps the pair in CX.

  So its phase is readable like every other animated thing here:
  `verify_menu.py` reads the ten cells at `ds:0x4DF2` **both sides of the
  screen dump** and hands the port `--caret`. Equal both sides means the
  blink did not turn over during the dump; different means the capture is
  between two pictures and is not scored against either, which happens often
  enough to matter - one run needed two retries. Both scored **100.00%**.

## A level can now be finished

Everything between a lemming reaching the exit and the game deciding what
that was worth:

- **the exit trigger**, image `0x02F3B`: not while falling, and the whole
  state word becomes `0x2000`. Its rectangle's units are FOUR PIXELS, which
  was measured rather than read - `tools/probe_exit.py` caught six lemmings
  entering, every one at record (888, 147), against an exit object at
  (848, 116)
- **leaving**, image `0x02326`: eight frames, then `ds:0x46` and gone. Its
  sprite is `main.dat` section 0 offset 16816, 16x13 in two planes
- **the end**, image `0x001C8`: everyone released with none active, or the
  clock's three digits all reading `'0'`
- **win or lose**, image `0x04720`: `saved >= needed`, and nothing subtler.
  The nine verdicts at image `0x048F6` are a ladder either side of it
- **what winning does**, image `0x0472E`: the level index advances, the
  rating is recomputed as index / 30, and image `0x0473E` caps at 120

Thirty a rating and a hundred and twenty in all is the same thirty the
briefing's level number subtracts at image `0x04566`, reached from the other
end - which is the kind of agreement worth more than either reading alone.

### The access code, driven rather than read

Image `0x05042` scatters the bits of three bytes - the level index at
`ds:0x1F6B`, `ds:0x491D` and the rescued percentage at `ds:0x40` - across ten
characters, each added to a base letter, and image `0x0516B` then rotates the
first seven. **The port generates it, and it is checked against the
original's own output on 428 calls** - `tools/verify_code.py`.

Three parts of it are *read*: the base letters `"AJHLDHBBCJ"` at `ds:0x4913`,
the level's two nibbles into characters 7 and 8 at image `0x05135`, and the
rotate-right of characters 0..6, `8 - (level & 7)` times, at image `0x0516B`.
The checksum in character 9 is read twice over - image `0x05147` builds it and
image `0x051D4` checks it, two routines written apart agreeing on one rule.

The **bit table for characters 0..6 is measured, not read**, and says so in
`game.c`. Ten near-identical mask-and-shift blocks are exactly what this port
has twice mis-read by eye, and an automatic walk over the disassembly produced
a plausible table whose last four rows were stale register state.
`tools/probe_code.py` drives the original's own routine instead, one input bit
at a time, which determines a bit permutation completely.

| | |
| --- | --- |
| one bit at a time | 25 / 25 agree |
| many bits set, every rotation count | 403 / 403 agree |

The second row is the one that earns the first. A single-bit sweep pins a
permutation only if the contributions never interact, and that was believed
rather than measured until those 400 inputs were driven. A control that
short-circuits the rotation at `level & 7 == 5` - a value no single-bit case
reaches - produces 51 differences, all of them in the many-bit set.

What the measurement says that reading would not have made obvious: **bits
4..7 of `ds:0x491D` and bit 7 of the percentage reach no character at all.**
The code carries four bits of the one and seven of the other.

And the round trip through the validator is **not** what settles this; see
"A round trip only checks the round trip" in `CLAUDE.md`. All three deliberate
faults tried against the port - a bit in the wrong character, the rotation
backwards, the nibbles swapped - produced codes image `0x051D4` accepted.

### The pointer has to be held for this number to mean anything

The earlier recording of these 5,100 frames was made with nobody touching the
mouse, and that is not the same as the pointer being where the game put it.
Image `0x00F41` sets it to (150, 100) at start-up and the port starts there
too, but under `SDL_VIDEODRIVER=dummy` pygame still delivers a `MOUSEMOTION`
and the emulator's window loop takes it, replacing the game's position with
the emulator's own default of (160, 100). Ten pixels, and the two cross-hairs
never overlap again.

Scored against that recording the play area is exact on **41** of 5,100
frames rather than 5,099, and every one of the other 5,059 differs by exactly
56 pixels — two cursors, drawn ten apart. It reads as a regression and is not
one: building the port at an earlier commit gives the identical 56.

Re-recorded with the pointer HELD where image `0x00F41` puts it - one line of
input script, re-asserted every frame - the two coincide and the cursor stops
being something a comparison has to excuse. **A cursor that is scored rather
than subtracted is worth having**; it is 28 pixels of the port that nothing
else checks.

### Three skills of eight, and the harness that reached them

"Just dig!" stocks nothing but diggers, so seven of the eight skills cannot be
assigned by playing it - and a routine no run reaches is one the port can have
wrong for ever. `tools/record_level.py --skills` stocks all eight in the guest
(`ds:0x6A`, and every THREE bytes) and `LEM_SKILLS` does the same in the port;
`tools/make_skill_script.py` writes the input for one of them. Both sides get
the same poke, so the comparison stays a comparison.

| 400 driven frames | records | play area |
| --- | --- | --- |
| the digger (button 9) | 0 differ | — the level's own |
| the builder (button 6) | **0 differ** | **399 of 400 exact** |
| the blocker (button 5) | **0 differ** | **398 of 400 exact** |
| the climber (button 2) | **0 differ** | 390 of 400 |
| the floater (button 3) | **0 differ** | 400 of 400 — but see below |

### What is left of the eight

ALL EIGHT are transcribed and every one of them is exact against the
original's own record. What is left of them is pictures.

| | state machine | sprites |
| --- | --- | --- |
| digger, builder, blocker, climber | **exact** | found |
| floater | **exact** (level 2, umbrella open) | not searched |
| bomber | fuse, blast and cloud **exact** | the CRATER is not written |
| basher | **exact** | not searched |
| miner | **exact** | not searched |

Three data locations are open and all three are the same kind of problem -
a table or a sprite whose bytes are somewhere in a `.dat` and whose address
is not in the image: the bomber's crater masks (22 rows of eight pre-shifted
three-byte spans, image `0x05DF8`), the basher's equivalent at image
`0x05FAC`, and the bomber's own sprites. The method that found the others -
score every offset against the original's own pixels, and check the scorer on
something already known - applies to all of them.

The record now carries the two trigger bytes the original keeps at `[si+0x2B]`
and `[si+0x2C]`, because the basher's guard at image `0x034DC` reads one of
them. A lookup that only returned its answer left the basher no way to ask.

### The basher

Transcribed and exact - **0 of 400 frames differ**. What follows is the
reading it came from.

**Assigning**, image `0x034CE`: refuse if `[si+8] & 0xF76F`; then take
`[si+0x2B] & 0x0F` - the trigger cell two rows up and two along the way it
faces, which image `0x02EFC` stored - and refuse if it equals 7 facing right
or 8 facing left, and divert to image `0x03287` if it is 9 (steel). Then
`flags = (flags & 0xF56F) | 0x200`, hot spot (-8, -10), state `0x20`, and
the sprite is `0x50` = 80 bytes a frame with the mask `0x3C` in.

**Doing it**, image `0x0287D`: a counter of THIRTY-TWO, `and [si+0x28], 0x1F`.
Its low nibble decides:

| | |
| --- | --- |
| 2..5 | image `0x028A1` calls the terrain removal with the slice index `al - 2` |
| 5 | image `0x028AE` then looks at what is left ahead |
| 0x0B..0x1F | image `0x0293A` walks it forward one and falls if nothing is under it |
| otherwise | nothing |

**The masks**, image `0x05FAC`: the slice index is multiplied by 240 (image
`0x05FB4`'s two shifts and a subtract), image `0x0600B` adds `0x3C0` when the
direction is `0xFF`, and the shift comes from `(x + hot_x) & 7`. Two
directions of four slices of eight shifts of ten rows of three bytes - 1,920
bytes at offset 0 of the same resource the crater's masks sit at 0xC60 of,
and like them built at run time rather than read from a `.dat`.

**And what stops it**, image `0x028AE` - without which a basher never stops.
On the frame the counter reads 5 it probes FOUR columns starting eight ahead
(image `0x028BC` multiplies the direction by eight), each at `y - 6`; solid
in any of them and it carries on, nothing in all four and image `0x028FE`
puts it back to walking. The original bashed for FOUR frames on "Just dig!"
where the port bashed for 288, which is what said the test was missing
rather than mis-read.

### The miner, the eighth and last

**0 of 400 frames differ.** Two readings short of it, and both were a tail
that had been left unread rather than a rule mis-transcribed.

**Assigning**, image `0x03598`: refuse on `[si+8] & 0xFD6F`; refuse if
`[si+0x2C] & 0x0F` - the cell UNDER it - is 9, which is steel, and no other
skill asks that; then the same ahead-test the basher has. Hot spot
**(-8, -13)**, state `0x18`. The same constant `0xFD6F` is the guard at image
`0x035A2` and the keep-mask at image `0x035D8`, which is the shape the
digger's assignment was got wrong on.

**And the assignment drops it a row** - image `0x03616`, `inc word [si+2]`,
the last instruction before the jump out. Without it every one of the miner's
288 frames sat exactly one row high while x, the flag word and the animation
counter all tracked perfectly. A residue with one constant in it and nothing
else is a rule that has not been read, not a rule read wrongly.

**Doing it**, image `0x02B35`: a counter of TWENTY-FOUR, image `0x02B3A`
wrapping at `0x18` rather than masking, and its VALUE dispatches rather than
its low nibble:

| | |
| --- | --- |
| 0 | image `0x02BE1` drops y by one, and dies past `0xB4` |
| 1, 2 | image `0x02B60` takes a slice away, index `al - 1` |
| 3 | image `0x02BF1` moves x by the direction TWICE, and down one |
| 0x0F | image `0x02B66` the same, without the row |
| otherwise | nothing |

Four columns and two rows a cycle: the 2:1 diagonal a miner cuts. Its counter
is 24 where the basher's is 32 because it moves twice a cycle.

Both move branches end by asking what is underneath - image `0x02B90` calls
the same solidity read at image `0x05DC6` everything else uses - and a miner
with nothing under it becomes a faller. **That conversion clears the miner
bit**, image `0x02BD9`'s `and [si+8], 0xF7FF`, one instruction past the
`and [si+0x24],1` every other conversion in the file ends on. Reading stopped
there the first time and the port's lemming fell at the right speed to the
right place with `0x804` in its flag word against the original's `4`, for ten
frames.

Standing on a trigger cell, image `0x02C76`: 9 turns it round, 7 turns it if
it faces right, and 8 turns it **whichever way it faces** - image `0x02CB9`
is a `jns` to the instruction after itself, so the test it appears to make is
not one. Transcribed as it is.

**The masks** are at offset `0x780` of the resource at `ds:0x1FE8`, which is
exactly where the basher's 1,920 bytes end, and they end at `0xC60` where the
crater's begin: two directions of two slices of eight shifts of thirteen rows
of three bytes. `tools/verify_results.py` dumps them; nothing was read by eye.

**And keeping only shift 0 of the eight is now measured, not assumed.** The
port shifts at pixel granularity rather than picking a pre-shifted variant,
which is sound only if the other seven are pure translations - a pre-shift
into three fixed bytes could drop whatever runs off the end. It does not:
`tools/probe_mine.py` reports **0 of 364** variants differing from
`shift0 >> s`, because no row of these masks carries a bit within seven
columns of the right-hand edge. The probe had dumped the bytes and left the
question in a comment for months; it now computes the answer and a non-zero
there is the assumption breaking.

#### One pixel of the miner is still unexplained

The record comparison is 0 of 400. The **screen** is not: on `--number 1`
driven by `out/s8.txt`, 134 of 400 frames differ by exactly one pixel at
screen **(159, 81)**, where the original holds attribute 2 and the port 0.
No frame differs by two, or three - the histogram is `[(1, 134), (28, 1)]`
and the 28 is the cursor - so this is one rule, not noise.

What it is **not**, each ruled out by measurement rather than by reading:

- **not the mask** - 364 of 364 variants are pure shifts, above
- **not the placement** - the original clears column `col + d` for each set
  bit `d` (byte offset `col - s` plus the variant shifted by `s`), the port
  clears `col + d - 16` on a canvas exactly sixteen to the left; the two
  clear sets are identical by construction
- **not the terrain** - `probe_mine.py` reads the guest's own solidity map
  (image `0x05DC6`'s stride, image `0x05DEA`'s `0x4A40`) and it is **clear**
  at that pixel on every frame sampled, including ones where the screen shows
  2. Both sides cut the same tunnel; the 2 is drawn over it
- **not an object** - the level's three sit at world x 720 and 864, and this
  is at 799
- **not the cursor's box** - `cur_bias` is 0 on all 215 frames of the run, so
  image `0x05854`'s `0x480` shape never appears

The pointer is parked, which is why a world that moves leaves the difference
at a fixed screen spot.

**Inside the level, only the miner's own clear writes that canvas byte** -
image `0x06117` and image `0x06126`, and nothing else at all.
`tools/probe_writes.py` gained `LEM_WRITES_BASE` and `LEM_WRITES_COLS` for
this - the level's canvas starts at image `0x05DEA`'s `0x4A40` with `0xD0`-byte
rows, so without a base every row buckets wrongly - and then a third,
`LEM_WRITES_AFTER_FRAME`, which is the one that mattered.

Without that gate the same probe reported TWELVE writing instructions: four
inside the shared blitter, four more of its paths, and image `0x04DEB`. All
ten of the extra ones were the title, menu and briefing drawing themselves.
`record_level.py` records the whole run, those screens are mode `0x10` at 80
bytes a row, and a filter expressed as "canvas row 81, column 799" is really a
LINEAR OFFSET that addresses a different pixel on every screen before the
level. It was written up here as "the pixel is drawn, not carved" and that was
wrong. The gate counts the game's own end-of-frame at image `0x018F`, which
runs only inside a level, and the hook now prints how many level frames it
saw so a run that reached none says so instead of scoring the briefing.

**Image `0x04DC2` is still a page copy nobody here had read**, though it is
not what draws this pixel - it was found by chasing it, and the reading below
is from the disassembly rather than from that mis-scoped run. It sets the
Sequencer's map mask to `0x0F` and the graphics controller to mode `0x105` -
write mode 1, the latched copy - and `rep movsb`s `0x6D60` (28,000) bytes from
`ds:[0x1F76]` to `ds:[0x1F78]` inside A000, moving all four planes at once.
So the original holds a composed buffer and copies a window of it to the
display page each frame, drawing the lemmings onto the PAGE; image `0x04DEF`
is the same loop clearing instead.

`0x6D60` is 28,000 bytes, which is 640 x 350 / 8 - a MENU page, not a level
one - so this routine belongs to the menu's mode `0x10` and the port already
reads its sibling image `0x04DEF`, the clear.

So the level's own picture is a third buffer, not this pair. The run's
`crtc_start` alternates `0x02C2` and `0x27E2`, 9,504 bytes apart, and 9,504 is
44 x 216 - mode `0x0D`'s 44-byte rows. The master terrain at `0x4A40` has
`0xD0`-byte rows and is far wider; the game composes there and puts a window
of it on whichever page is hidden. **The per-frame sprite drawing therefore
goes to a page at `0x02C2` or `0x27E2`, and nothing that draws a lemming
touches `0x4A40` at all** - which is why the canvas filter found only the
composition and the miner's clear, and why it can never answer this question.
Pointed there - stride 44, base `0x02C2`, row 81, the bytes around column
159, and still gated to the level - it names 56 instructions, and they fall
into three groups that are the level's whole drawing path:

| | |
| --- | --- |
| `0x07440`, `0x075B4` | the LEMMING blits: `lodsw`/`stosb` through the graphics controller's bit mask at index 8, **three bytes a row**, then `add di, 0x29` - 41, which is 44 minus 3 - to step the page |
| `0x07178` | the background SAVE/RESTORE: `movsb` three times, `cx = 0x10`, the same `add di, 0x29`, with `ds` and `es` both `0xA000`. A 24 x 16 block copied inside video memory, **unmasked** |
| `0x08188`, `0x0688D` | a rectangular `rep movsb`, and a further masked blit |

**A lemming is drawn 24 columns wide, not 16.** Sixteen pixels at an arbitrary
bit offset span three bytes, so the sprite is pre-shifted into them and the
bit mask picks out the sixteen that are really it. The port draws exactly 16
columns from an unshifted sprite, which lands the same pixels - but it means
the port has no counterpart to the 24-wide window, and none to the
save/restore either, because it composes every frame from nothing.

**And that is the answer.** The master canvas holds attribute **0** at that
pixel on every frame sampled - all four planes read, not just the solidity bit
- while the displayed page shows 2, with frame 200 agreeing at 0 on both sides
as the control. So the 2 is in neither the terrain nor any current sprite; it
exists only on the page.

The arithmetic closes it. At frame 190 the nearest lemming is at record
x 789, so its sprite starts at 781 - and the blit takes `di` from `x >> 3`, so
its window starts at `781 & ~7` = **776** and spans 24 columns to **799**,
which is screen 136..159. The disputed pixel is the LAST COLUMN of that
window: outside the sixteen the bit mask writes, inside the unmasked 24 x 16
block image `0x07178` saves and paints back. The restore returns a value that
was true when it was saved and is not true now.

**The port cannot reproduce this by drawing better.** It composes each frame
from the master plus the sprites, which is why it holds the honest 0; the
original's page carries three columns of history either side of every lemming.
Matching it pixel-for-pixel means transcribing the page model - the save at
`0x07178`, the two pages `0x02C2`/`0x27E2` and the window copy between them -
rather than adjusting any sprite or mask. That is a change to the port's frame
model and a scope decision, so it is recorded here rather than made: **the one
pixel is understood, and closing it is a choice about how much of the
original's video memory the port should own.**

Two instrument faults were found on the way and both produced confident
numbers: a scorer looking for `%05d.raw` where the port writes `f%05d.raw`
compared **zero** frames and reported no differences, and a coverage test
written `if lem["alive"]` skipped every living lemming, because `alive` is
`0xFF` for DEAD. Each read as a fact about the game.

### The nuke

**0 of 557 frames differ** in the RECORD, and the screen is exact on **541 of
557** - re-measured after the explosion sprite was corrected, not carried
forward from the 535 it scored before.

What is left is **16 frames, 501..516, of 6 or 12 pixels**, and it is one
thing: on frame 508, all twelve are on the single row `y = 80`, in two runs of
six at `x 45..50` and `x 56..61`. The port has 0 there and the original still
has terrain. Two runs of six is the two craters' top row, and the port has
cleared it while the original's visible page has not been repainted there yet.

That is the DISPLAY MODEL and it stays a deliberate non-goal - see the note
below. What is worth recording is that correcting the sprite narrowed it from
22 frames to 16 and from whole craters to one row of each: the previous
figure was measuring the wrong sprite as well as the display lag, and the two
were not separable while the first was wrong.

**0 of 557 frames differ**, against a recording of the original being nuked on
level 1 - the double click, the cascade, all ten explosions and the level's
end. `tools/verify_skills.py` runs it; `out/nuke.txt` is the script both sides
read.

It is **panel button 11** and it is a **DOUBLE CLICK**, image `0x03FEF`: image
`0x03FFB` reads the frame counter at `ds:0x1FD3`, image `0x04002` stores it as
the last press, and image `0x04007` gives up unless the two were within five
frames. `ds:0x1FD3` is bumped once a frame at image `0x00199` - the game's own
end-of-frame - so the window is five GAME frames, a third of a second at
seventeen a second.

Image `0x04005` subtracts in AL and image `0x04007` is a **signed** compare,
and image `0x00438` starts the last-press byte at zero. So a press at tick 128
or beyond reads its gap as negative and is accepted **on its own**: for half
of every fifteen seconds a single click nukes the level. The original does
this - it lit its first fuse on the first press of the reference run, at the
same frame and with the same 79 as the port - so it is transcribed rather than
corrected.

**AND ITS FOUR BYTES WERE NEVER RESET**, which is what a player actually hit:
after several levels in a row the nuke "became a single click". The quirk
above is the original's and is on its own schedule; carrying the state over
made the accept window turn up at random instead.

A level start clears them - image `0x00438` clears ds:0x1FD4 and image
`0x0043B` clears ds:0x1FD3 explicitly, and image `0x0044F` then wipes ds:0x12
for 0x42A0 bytes, through 0x42B1, which covers ds:0x4B and ds:0x4C. The port
reset none of the four.

`ds:0x4B` is the worse one: nuke a level and it stayed 0xFF, so **the next
level began its cascade the moment it started.**

`reconstruct/tests/screens.c` now tests the double click, which nothing did -
and it ASSERTS the signed-compare quirk rather than contradicting it, so that
nobody turns image `0x04007` into an unsigned compare later and breaks a match
that is already proven against the reference run.

Then image `0x04013` copies the released count over the level's total, so the
entrance stops and image `0x001EA`'s "everyone released and none alive" can be
reached at all.

**The cascade is the tail of the walk loop**, image `0x01FF8`, not a routine of
its own: image `0x01FF3`'s `dec [0x42] / je` falls into it once every lemming
has been stepped. One lemming a frame gets a fuse of `0x4F`, which is what
makes the nuke a run of little explosions rather than one bang - and a dead one
costs no frame, image `0x02026` bumping the cursor and going round again in the
same pass.

Nothing resets `ds:0x4B` or `ds:0x4C` at a level's start; they are written only
by the nuke and by image `0x0203C` when the cascade finishes. A second nuked
level picks the cursor up where the last one left it, and that is the
original's behaviour rather than an omission here.

### The panel is not thirteen skills

Image `0x03F27` refuses any button below 2 outright, and image
`0x03F3D..0x03F4D` sends 10, 11 and 12 away before image `0x03F66` stores the
rest in `ds:0x82`. The port had treated all thirteen as skills, so clicking
the nuke selected "skill 11" - which nothing dispatches, quietly leaving the
panel with no skill armed.

| | | |
| --- | --- | --- |
| 0, 1 | image `0x04032` | the release rate - **not transcribed** |
| 2..9 | image `0x03F66` | the eight skills |
| 10 | image `0x04018` | pause - **not transcribed** |
| 11 | image `0x03FEF` | the nuke |
| 12 | image `0x03FEE` | nothing at all, a bare `ret` |

Pause is not a button on its own but a flag the whole frame loop reads - image
`0x01F19` returns early on `ds:0x2B` and so does image `0x0020C` - so it is a
change to the loop rather than to that switch, and it is left with its address
against it rather than a guess in its place.

### Eighty lemmings, not sixteen

`LEMMING_MAX` was **16**, a number nothing in the original says. The array is
at `ds:0x88` and image `0x030EA` indexes it by the release ordinal times
`0x2D` with no bound but the level's own total at `ds:0x47` - and
`0x88 + 0x2D * 80` is `0xE98`, exactly where image `0x030E2` reads the
entrance table. The array is **eighty** records and ends where the next thing
begins. The largest of the eighty levels asks for eighty.

So every level wanting more than sixteen released lemmings into a slot
`lemmings_release` refused to fill: they counted as OUT and existed nowhere
else. Level 3 of Fun releases fifty, and from frame 692 the panel showed
none out, none saved and a level that would not end for another 735 frames -
which is what it was reported as. It ends correctly now, and level 1's 5,100
frames are unchanged at 5,099 exact.

### A lemming starts each level as a walker

Image `0x030F1` is a `loop` that writes a zero over every byte of the record
before image `0x030F9` fills any in. The port had only ever overwritten the
fields it went on to set, and the slot is reused from level to level - so a
level played after one with floaters started with lemmings that could not be
killed by a fall. The abilities at `[si+0x24]` and `[si+0x25]` are the
visible half; the brick count, the fuse and the blocker's saved cells were
carried across too.

### Four entrances, taken in turn

Image `0x030DB` is `and bx,3`: the release ordinal picks one of four slots in
the table at `ds:0x0E98`, and the port had always taken the first entrance
object it found. Eight of the eighty levels have more than one - four have
two, two have three, two have four - and on every one of them every lemming
came through the same door.

Image `0x038A3` builds the table by walking the 32 object slots and appending
each `id == 1`, then image `0x038D6` dispatches on how many it found and
copies the ones it has over the ones it has not: none gives four of
`(0x100, 0x10)`, one gives itself four times, two gives `1, 0` after them,
three repeats entry 1. All four entries are always valid, which is why
`and bx,3` needs no further check.

### The three deaths the trigger dispatch never reached

Reported from playing, all three within a few minutes of each other, and all
three are ONE gap:

| | |
| --- | --- |
| `level001.dat --level 3` | a death scene is missing |
| `level002.dat --level 4` | drowning in the water is missing |
| `level002.dat --level 6` | trap activation is missing |

Image `0x02F0F` dispatches **seven** trigger effects and the port had three.

| | | |
| --- | --- | --- |
| 0 | - | nothing |
| 1 | image `0x02F3B` | the exit |
| 2, 3 | image `0x02FA2`, `0x02FAC` | a blocker's two columns |
| **4** | image `0x02FB6` | **a trap** |
| **5** | image `0x02FF8` | **water** |
| **6** | image `0x0304F` | **the other death** |

Effects 5 and 6 set flags `0x1000` and `0x4000`, and those are exactly the two
entries of image `0x01F5E`'s chain that stood in `game.c` with an address and
nothing else. Nothing could set the bits, so nothing ever ran the handlers,
so there was nothing to check a transcription against - a closed loop that
looked like a decision.

**The trap**, image `0x02FB6`, kills OUTRIGHT: image `0x02FD1` writes `0xFF`
into `[si+0x26]` with no animation of the lemming's own. What animates is the
TRAP: image `0x02FCE` sets bit 1 of the object's record at `ds:0x17CB`, and
image `0x02FC5` refuses while that bit is still set, so one lemming at a time.

**And the trigger map carries which object.** Image `0x03808` builds the cell
as `ax = bp << 4 / or al, [si+0x14]` - the object's index in the high nibble,
the effect in the low - and the port stamped the effect alone. It could tell
that a trap had been touched and never WHICH, because image 0x02FB8 indexes
those records by that nibble. With every index zero the two bytes are the
same, which is why nothing here noticed.

**Drowning**, image `0x022F5`: sixteen frames and then dead, drifting one
pixel a frame in the direction it was facing while the water EIGHT ahead is
clear - image `0x02306` multiplies the direction by eight before the probe.
The flag word is ASSIGNED `0x1000` rather than OR-ed, so whatever it was
doing stops.

**The other death**, image `0x02340`: fourteen frames, no movement. Its hot
spot is `(-8, -14)`, four rows deeper than anything else here, and its frame
is `0x70` = 112 bytes against the drowner's `0x3C` = 60.

### A minute is not available on every level

`level001.dat --level 3` is scored over 530 frames and not the 1,020 that a
minute would be, and that is the LEVEL rather than the run falling short: its
own recording ends at frame 563 with all eighty lemmings dead, `over` set to
0xFF and the DAC fading to nothing by 564. Reached as Fun 9 it stocks twenty
of every skill and releases at 99, so with nobody playing the whole level is
over in thirty-three seconds.

The other two do give a minute and more - 1,100 frames each, and neither ends
inside it.

This is the same rule this file already states about the first level's 5,100:
score the LEVEL, not the timer. A recorder waiting for frames a finished level
will never produce looks exactly like a slow one.

### The fire death, and "I have a cunning plan" matched

`level001.dat --level 3`, frame by frame: **play area exact on 488 of 530**,
and the 42 that are not are the CURSOR during the level's opening and nothing
else. Before the fire death was drawn it was 42 of 530 with a worst of 139;
before the level indices were right it was 0 of 535 with a worst of 25,954.

**The sprite.** Effect 6's file frame is image `0x0306D`'s `[si+0x12]` = `0x70`
= 112, which for sixteen wide and fourteen rows is **four planes** - the only
sprite in this port that uses all four, and the colours in it run to `0x0D`.
Base **17232**, found against the original's own frames: 204 and 205 score 32
of 32 and 31 of 31 with nothing differing, and 209 and 212 pick indices 1 and
0 of the same base with 31 of 31 and 32 of 32 - a second lemming arriving at
the same fire and starting its own animation. Frame 0 is a lemming standing
and 4, 8 and 12 are scattered ash, so `lem_burn` is what it is.

**And park the cursor.** The two cross-hairs sat ten pixels apart on every
frame - 56 pixels of pure noise that hid everything else, and the score read
42 of 530 while the level was already exact. One line of input script aimed
from the two cursors' own pixels put it at 488. `tools/compare_screen.py`
excuses "exactly 28", which is one cursor; two in different places is 56 and
excused by nothing.

### The explosion's sprite, found; its animation, not

The cloud is **not in MAIN.DAT at all** - three searches of all seven sections
found nothing, and that is not a mystery: image `0x02D45` gives it a segment
from `ds:0x3C` where the walker's relatives take theirs from `ds:0x2E`, and
what the blitter reads is the PRE-SHIFTED STORE, not the file.

**The store, and how to read it.** Image `0x07112` takes `ds:0x1FE6` as the
segment and image `0x07116` adds `bp`, which image `0x070C6` has divided by
SIXTEEN - so `[si+0x0c]` and the frame index are PARAGRAPH counts, not byte
offsets. Reading them as bytes gives dense noise that looks exactly like a
sprite decoded with the wrong geometry, and cost three attempts.

**And `[si+0x12]` is the MASK'S OFFSET inside the store frame**, not a file
size. That is what makes the geometry fall out, and it checks on every sprite
whose base is already known:

| | `[si+0x10]` | `[si+0x12]` | so |
| --- | --- | --- | --- |
| basher | 80 | 60 | 2 bytes x 10 rows x 3 ink, mask 20 |
| miner | 104 | 78 | 2 x 13 x 3 ink, mask 26 |
| faller | 90 | 60 | **3** x 10 x 2 ink, mask 30 - pre-shifted |
| cloud | 640 | 480 | **5** x 32 x 3 ink, mask 160 - pre-shifted |

The three that are not pre-shifted have `[si+0x2a] = 0xFF`, image `0x070B0`'s
test; the two that are have 0.

**The proof it is right**: decoding the store at the FALLER's own paragraph
`0x0A42` with 3 x 10 x 2 gives a falling lemming, and decoding at the cloud's
`0x180B` with 5 x 32 x 3 gives a starburst in colours 3, 4 and 5 - the exact
colours the original's screen holds.

**The animation is ONE FRAME, and that is now proved rather than guessed.**
The store holds one picture in eight shifts at indices 0..7 and nothing at
8..31, which said the index cannot be image `0x070B6`'s `anim * 8 + shift`
when the counter runs to `0x34`. Scanning the original's own frames 492..528
for dense runs of colours 3, 4 and 5 settles it: there is exactly ONE burst on
screen at a time, at frames 496, 498, 500, 502 and 504, and the lemming
beside it is at **anim 0** every time - lemmings 1, 3, 5, 7 and 9 in turn.
Each burst is about 430 pixels and lasts a single frame.

**IT WAS DRAWN WRONG, and a player found it: the left of the explosion was
cut off.** The reading below was self-consistent and wrong in both halves,
and it is kept because the way it failed is the point.

It took store index **7**, dropped columns 32..39, and placed the result at a
fitted `(x - scroll - 7, y + hot_y - 18)` for **2,086 of 2,125 pixels**. The
argument for 7 was that the pre-shift ROTATES within the 40-bit row rather
than shifting into it, so index `i` carries the sprite at columns `i..i+31`
with the overflow coming back at the start - checked cell by cell, index `i`
agrees with index 0 on 1,011 of the 1,024 cells of that window for every one
of the eight. All of that is true. The conclusion drawn from it - that 7 is
the rotation bringing the sprite's columns into one run - was not.

**Index 0 is simply the sprite.** 507 lit pixels, a whole round cloud in
colours 3, 4 and 5, and every other index is that same picture rotated.

**And the placement is not fitted at all - it is the HOT SPOT**, `(x + hot_x,
y + hot_y)`, the same one every other sprite here is drawn at. That is what
image 0x02D3C's `-16` and image 0x02D37's `-25` are FOR: they are set as the
bomber turns into a cloud, to place this sprite.

Measured against the original's own **ten** bursts in `out/ref_nuke`, frames
495..504, one lemming reaching anim 0 on each:

    frame 0 at the hot spot     5,032 of 5,070, and FIVE of the ten
                                are 507 of 507, exactly
    the old frame 7 at its
    fitted offset                  77 of 507 on the first of them

The five that are not exact are the early ones, where another lemming's
sprite is still drawn over the cloud.

**A 98% that came from fitting two wrong things against each other.** The
sprite was a rotated window missing its left columns, and the offset was
chosen to make that window land as well as it could - so the score was high,
monotonic in the index, and pointing away from the answer. `CLAUDE.md` already
says a structured residue is a rule you have not found; here the residue was
the sprite's own left edge, and no amount of choosing between the eight
indices could have reached it.

The nuke's play area is now exact on **541 of 557** frames, worst 12
pixels - it was 535 and a worst of 672 under the old reading.

**And what remains is DEBRIS over the crater, not a slow crater.** An earlier
reading of this said the original "goes on removing terrain for twenty-five
frames". The measurement was right and the reading of it was wrong.

Take the crater's final footprint - the 615 pixels inside a box that the port
ends with empty - and ask how many the ORIGINAL still has ink on:

```
  frame     505 506 508 512 520 548
  still ink 182  92  86  77  48   0
```

The crater is cut at once on BOTH sides. What the original then has is ink ON
TOP of it, thinning away over forty frames. Frame to frame, the changing
pixels are the same 8x8 blocks every frame with a falling count - 138, 151,
130, ... 19 - an effect animating over the site, not terrain being revealed
block by block.

It is debris in the terrain's OWN COLOURS, which is exactly why counting
terrain-coloured pixels made it look like terrain not yet removed. The same
trap this file already records one level up: a frame diff cannot tell you what
a pixel IS.

**And that is measured, not asserted.** Comparing each still-inked pixel
inside the crater against the value it held BEFORE the blast:

```
  frame              505  508  512  520
  still inked        182   86   77   48
  same as before it   79    1    1    1
```

From 508 onward exactly one pixel of eighty-six is the terrain that was there.
The rest is new ink at those coordinates - so the crater IS cut and something
is drawn over it, rather than the crater being cut slowly. The obvious reading
of "terrain-coloured pixels still present" was the wrong one twice over, and
this is the comparison that separates them: not how MANY pixels, but whether
they are the SAME pixels.

Two mechanisms were ruled out by reading and need not be checked again. The
dirty-block repaint at image `0x080FD` drains EVERY marked run each frame -
image `0x08199`'s `jcxz`/`jmp` loops back rather than budgeting - so the
screen never lags its map. And `[si+0x22]`, set to fifteen by image `0x02D4F`
and the obvious candidate for a particle count, is written in four places and
read in none.

**And then it was found by WATCHING rather than reading.**
`tools/probe_debris.py` buckets every write into the EGA window by the
instruction that made it, over two windows of ONE run - frames 400..450 with
nothing exploding, and 505..545 with the debris thinning. One instruction
writes 23,431 times in the second and **not once** in the first:

| | |
| --- | --- |
| image `0x06F6D` | `mov es:[di], ah` - one pixel |
| image `0x06E83` | the routine: `bx = 0x2788 + (anim - 1) * 160` |
| image `0x06ECC` | `cl = ds:0x4D` - the count, set to **80** at image `0x0042C` |
| image `0x06F5B` | the colour: `ss:0x2778 + (cx & 0x0F)`, sixteen entries `04 02 06 07 08 09 0a 0b 08 09 0a 0b 0c 0d 0e 0f` |
| image `0x06ED2` | the loop: signed `(dx, dy)` pairs, `0x80` ends the row |
| image `0x06EF2` | clipped to `x < 0x150` and `y < 0xA0` |

So the debris is EIGHTY pixels a frame, positioned from a row of eighty signed
offsets and coloured by a countdown through sixteen. That is why it is in the
terrain's colours - half that table is.

**How much debris there is, frame by frame.** Counting pixels the original
has and the port does not, over the whole play area:

```
  frame  494 498 502 506 510 514 518 522 526 530 534 538 542 546 550 554
  pixels   0 249 466 589 577 508 380 302 325 309 230 122  84  44  24   8
```

It rises as the ten blasts accumulate - peaking near 589, which for ten
lemmings is about 59 of the eighty each, the rest clipped or overlapping -
and then falls away over fifty frames.

That decline is a **trajectory**, and the table holds it: fifty-two rows of
eighty (dx, dy) pairs, one row per frame of the cloud's own fifty-two-frame
counter, read at image `0x06E83` as `(anim - 1) * 160`. The pairs are absolute
offsets from the lemming - image `0x06EDE` pushes and pops `bp` and `dx` around
each one, so they do not accumulate - and a pair whose first byte is `0x80` is a
particle that has already gone, **skipped rather than ending the row**: image
`0x06EDB` jumps to the `dec cx` at image `0x06F74` and carries on. Counting the
live ones per row gives 80, 76, 75, 71, 69, 69, 68, 68, 67, 34, 20 and finally
16, which is the decay seen on screen.

**Getting this wrong twice is the lesson, and both times it was an
instrument.** A dump of the table taken from a running level is byte-identical
to the image, so nothing computes it at load or during play - it is static data
and always was. But a parser that treated `0x80` as a **terminator** read row 0
as eighty particles and every row after it as empty, which said the eighty were
drawn once and left to be erased. That was written up here as a finding and
committed. The sentinel skips; the disassembly says so plainly at image
`0x06EDB`, and the reading that contradicted it came from a five-line script
nobody checked against a case whose answer was known.

The count is `ds:0x4D`, and it is **80** - image `0x0042C` stores `0x50` over
the `0x80` the image ships with. Read from the static image alone it looks like
128, which is more pairs than a row holds.

So this project's rule about a check whose reference is derived from the thing
under test needs a companion: **a parser is an instrument too.** Run it once
against data whose answer is already known - here, any row but the first would
have done - before believing what it says about data that is not.

The table is **static data in the image**, not a buffer: dumped from a running
level it is byte-identical to `image/vgalemmi.decrypted.img` at `ds:0x2788`,
and nothing writes it in a quiet window or a blast one. There is nothing left
to find about where it comes from - it ships with the game, all 8,320 bytes of
it, and the port transcribes it the way it transcribes a palette.

### A harness setting reported as a reconstruction failure

The nuke run's PANEL scored **0 of 557 frames** and had for as long as the
case has existed. Nothing was wrong with the panel: `verify_skills.py` sets
`LEM_SKILLS=20` for every case it runs, because the eight skill references are
recorded with `record_level.py --skills 20` and both sides must get the same
poke - but the nuke and the pause references are recorded WITHOUT it, so the
port was drawing twenty of everything beside an original showing level 1's own
counts. Giving those two cases the level's own skills takes the nuke's panel
to **540 of 557**.

The play area was exact on 535 the whole time, which is what made this easy to
leave alone: one number looked finished and the other looked like a hard
problem, and they were the same run. A harness that feeds the two sides
different inputs reports the difference as the port's, and there is no way to
tell that from the score - only from asking what the harness set.

### All three levels are exact, panel included - and the last two faults were
### routines sitting in the wrong half of the loop

    level001.dat --level 3     530 of 530 play area,   530 of 530 panel
    level002.dat --level 4   1,100 of 1,100 play area, 1,100 of 1,100 panel
    level002.dat --level 6   1,100 of 1,100 play area, 1,100 of 1,100 panel

Zero differing pixels on every frame of all three, over a minute and a half of
play each. The last hundred pixels were both on **frame 0** - the level as it
stands before anything moves, which the port draws without stepping - and both
were a routine the port ran in the step where the original runs it elsewhere:

- **the OUT count belongs to the DRAW.** Image `0x03645`'s loop skips a
  lemming whose `[di+0x26]` is 0xFF and counts the rest into `ds:0x45` as it
  draws them; image `0x03684` hands that count to the three-digit routine with
  `si = ds:0x2095`. The port computed it at the tail of `level_play_step`,
  which agrees on every frame except the first.
- **the IN percentage is written before the loop starts.** Image `0x03AC8` has
  exactly two callers - image `0x001C2` inside the play loop, and image
  `0x0049E` in the level's set-up, after image `0x00495` draws and image
  `0x00498` puts the objects up. The port had only the first.

This is the third time on this project that a routine has turned out to live
in the draw rather than the step - the object animation at image `0x07C1E`
was the second, and it is what fixed level002/6. **When a difference appears
on the first frame and nowhere else, the question is not what the routine
computes but which half of the loop it is called from**, and finding the
answer means finding the original's callers rather than reading the routine
itself.

Note also what the frame-by-frame score could not see. Both of these agreed on
1,099 frames out of 1,100 and were still wrong, and the only frame that could
show it was the one the port draws before it has stepped - which is exactly
the frame a comparison is most tempted to excuse as the fade-in.

### The terrain flags were inherited; now they are read

`level_parse` decoded a terrain entry's three flags as word bits 15, 14 and 13
and said so with the comment "word bit 15" - which came from the published
description of the LVL format, not from the binary. This file already
distinguishes the two: a third-party description is a hypothesis, and which
claims were confirmed against the bytes and which were inherited unverified
are different kinds of knowledge.

They are read now, out of image `0x07C67` - the blitter the composition loop
at image `0x03752` hands each entry to. It is given the same two words and
unpacks them into its own flag word: image `0x07C6C` shifts AX right by seven
and keeps `0xC0`, putting AX bit 14 in bit 7 and AX bit 13 in bit 6; image
`0x07C75` tests that bit 6 and ORs in `0xF00`; image `0x07C7F` rotates AX left
so bit 15 falls into carry and ORs in `0x8000`. So **bit 15 is no-overwrite,
bit 14 upside down, bit 13 erase** - and bit 15 is the same bit
`level_anim_draw` already knew from the object side as bp bit 15, "not drawn
where there is terrain", which is two independent routines agreeing.

The coordinates agree to the instruction as well: image `0x07C95`'s
`and ax, 0x1fff` is the port's `(e[0] & 0x1F) << 8 | e[1]`, and image
`0x07C90`'s `sar bx, 7` on the signed word is exactly the port's `sign9` of
the top nine bits.

So `render_terrain`'s debt is narrower than it was, and the comment now says
where the line runs: the UNPACKING is transcribed, the COMPOSITION is still
the port's own - the original programs the card and blits a byte at a time
where this walks pixels. `tools/prove.sh` still reports 49,741 pixels, zero
differing.

### dat_load was not a debt, it was IO in the wrong file

`dat_load` carried a NOT TRANSCRIBED marker and the note that "the original
streams sections through a read routine (image 0x01D2E) into one buffer as it
goes". Nobody had read that routine, so the debt stood. Read now, it is **pure
DOS**: `INT 21h AH=23h` for the size through an FCB, the size shifted into
paragraphs and checked against what is left at `ds:0x1F90`, then `INT 21h
AH=3Fh` reading into a bump allocator at `ds:0x1F8A` which advances past a
13-byte section header each time (image `0x01DA6`'s `add dx, 0xd`).

So there is no container-parsing routine in the original to transcribe at all:
the reading and the walking are one loop, and the reading is DOS's. The port
reads the file whole, which is a real arrangement difference, but the arrangement
was never the debt - not having read the routine was.

The reading half now lives in `sdl_io.c` as `io_read_file`, under the note
that file already carries: *a routine marked as ours inside the game's file is
either IO that belongs here, or game logic nobody has transcribed*. What stays
in `game.c` is the part that is the game's - the section headers and the
decompressor, which `tools/verify_dat.py` checks against the original's own
body.

    owed routines: 6 -> 5

The general form, and it is the second time this pass: **a routine can look
like an untranscribed debt when the original's equivalent is not game logic at
all.** `level_panel_build` was the other - there the original had no such
routine, here it has one and it is DOS. Neither could be settled without going
and reading the address that was already written down beside the marker.

### Sweep the flag, do not wait for a case to expose it

Finding the pause bug by having a pause case is luck; finding the OTHER seven
by enumerating them is a method. `tools/xref.py` lists every instruction in the
image that names one DGROUP offset directly, and `ds:0x2B` has nine:

    image 0x0020C  the clock                    the port guards it
    image 0x01F19  the lemmings walk            the port guards it
    image 0x030A6  the release                  the port guards it
    image 0x03150  the SKILL ASSIGNMENT         the port guards it
    image 0x03944  the object animations        THE ONE THAT WAS MISSING
    image 0x03F52  selecting a skill            the port guards it
    image 0x03FEF  the nuke's double-click      the port guards it
    image 0x04028  the toggle itself            image 0x04018's `xor`
    image 0x04032  the release-rate buttons     moot: image 0x03F27 refuses
                                                every button below 2 and the
                                                rate is not transcribed

So pause is now COMPLETE rather than one-bug-fixed, and that is a different
claim. Image `0x03150` is worth naming while it is here: it is the skill
assignment, dispatching `ds:0x82` to the eight handlers - image `0x03598` in
it is the miner, the same address `lem_mine_start` carries - and it lives in
the play LOOP, not on the click.

**And the tool had to be checked on a known answer first.** Its first version
matched only the accumulator short forms and a hand-written list of
`op [x],imm`, and it reported `ds:0x4D` - the debris count - as having ONE
reference, the store at image `0x0042C`. The read at image `0x06ECC` is
`mov cl,[0x4d]`, a modrm form, and was invisible. A sweep that under-reports
reads exactly like a sweep that found everything. It matches any modrm opcode
with mod 00 and rm 110 now, and still cannot see an access made through a
register - so a count of zero means "no direct reference", not "never
touched".

### The briefing's preview is one blit from a prepared buffer

`draw_preview` and `render_objects` are two of the five routines still owed,
and both are about the briefing's little map. The search for the original's
version is now narrowed to one address rather than a screen.

`tools/probe_writes.py --rows 20 58 --stride 80` finds twelve instructions
writing into the preview's band, and the biggest - 9,360 bytes at image
`0x04E34` - is a red herring: it is the `rep stosb` of image `0x04DEF`, which
clears `cx = 0x6D60` = 28,000 bytes, the whole 640x350 page. The rest are the
shared blitter's own stores.

`tools/probe_blits.py --driven --rows 20 58` then names the caller, and there
is exactly one for that band:

    x=0  y=20  632 wide  94 rows  4 planes   from image 0x04F0A

Image `0x04EEF` sets it up - `bx = 0x14` for y, `dx = 0x278` for 632,
`cl = 0x5E` for 94 rows, `si = 0x2080`, `bp = 0xF42` - and image `0x04F0A` is
`lcall 0x1010:0x8822`, the shared blitter, reading from the segment in
`ds:0x1FEE`. So **the briefing's whole upper area, the preview inside it, is
composed into a buffer and put up in ONE blit.** Nothing draws the preview to
the screen piece by piece, which is why a probe watching the EGA window can
only ever see the clear and the blit.

So the routine that is owed writes to `ds:0x1FEE:0x2080`, not to `0xA000`.
`tools/probe_preview.py` watches there, and **found it**:

    image 0x04639 -> image 0x05017 -> image 0x07E76      the builder
    image 0x07EC5 .. image 0x080F8                       a 4:1 packer
    image 0x04434 -> image 0x04EEF -> image 0x04F0A      the blit

**And a correction: the preview is NOT part of that blit.** This was written
up twice as "the briefing's whole upper area, preview included, composed into
a buffer and put up in ONE blit". It is not. Pointing
`tools/probe_preview.py` at the other half of the segment - `LEM_PREVIEW_LO=0`
- finds the same two instructions writing there as at `0x2080`, and both are
the DECOMPRESSOR (image `0x01C83`, image `0x01C9A`). Segment `0x587E` is a
decompression buffer, so the 632 x 94 blit is the panel's STATIC artwork, and
the preview is drawn on top of it afterwards.

Where it is drawn is the thing the earlier note got backwards: image `0x07EFC`
sets **DS as well as ES to 0xA000**, so the packer reads the level bitmap out
of video memory and writes the visible page directly, with no buffer between
and no blit. Every number in it is one of `draw_preview`'s:

    image 0x07EFE   cx = 0x27     39 rows                        PV_H
    image 0x07F04   cx = 0x34     52 bytes a row
    image 0x080ED   di += 0x50    80, the 640-wide screen's stride
    image 0x080F0   si += 0x340   832 = 4 x 208, four LEVEL rows  PV_SCALE
    image 0x07F1D   four source bytes in, one byte a plane out    PV_SCALE
                                                                  PV_SAMPLE

so the vertical scale is read as well as the horizontal one. The builder
copies `ds:0x1FEE` into `ds:0x1FE4` and blits four times, then falls into the
packer: `cx = 0x34` at
image `0x07F04` is **52 destination bytes a row**, and it reads FOUR source
bytes at image `0x07F1D` and scatters their bits into one output byte per
plane - which is `draw_preview`'s `PV_SCALE` of 4, read out of the original
rather than fitted. Its four unrolled plane writes are image `0x07F7B`,
`0x07FF1`, `0x08067` and `0x080DD` at **2,028 bytes each**, and 52 x 39 is
2,028 - so 39 rows, which is `PV_H`. The blit then puts 632 x 94 up at y = 20,
`PV_Y` exactly.

And the SAMPLE is read too, which was the last of the fitted constants.
Decoding image `0x07F28`..`0x07F76`'s bit scatter - four source bytes in, one
output byte a plane out, bit 7 being the leftmost pixel:

    out bit 7   ch rol 2, and 0x80    source bit 5    pixel  2
    out bit 6   ch ror 3, and 0x40    source bit 1    pixel  6
    out bit 5   cl        and 0x20    source bit 5    pixel 10
    out bit 4   cl rol 3, and 0x10    source bit 1    pixel 14
    out bit 3   bh ror 2, and 0x08    source bit 5    pixel 18
    out bit 2   bh rol 1, and 0x04    source bit 1    pixel 22
    out bit 1   bl rol 4, and 0x02    source bit 5    pixel 26
    out bit 0   bl ror 1, and 0x01    source bit 1    pixel 30

Every fourth pixel starting at the third: `PV_SCALE` 4 and `PV_SAMPLE` 2, and
the preview still scores **0 pixels differing** against the original's own.

Every constant `draw_preview`'s comment said was "measured against the
original's screen, not read from its code" is now read from its code, and they
all agree. One difference worth noting rather than chasing: the original packs
the level bitmap's full 1,664 columns to 416 output pixels where the port
packs the level's 1,584 to 396. The 20 output pixels between them are the
bitmap's empty tail, which is why nothing shows.

Image `0x07EB2`, the packer's own entry, gives the last two: `di` is
`ds:0x1F76 + ds:0x1F7A + 0x64F`, and 0x64F is 20 rows of 80 plus byte 15 - **y
= 20 and x = 120**, `PV_Y` and `PV_X`. `si = 0x4F22` is `0x4A40 + 6 * 208 +
2`: the level bitmap's **row 6**, byte 2 being its sixteen-pixel left margin,
which is where the port's canvas starts. Row 6 is `PV_TOP + PV_SAMPLE`. All
six constants read, all six agreeing with what was measured off the screen.

**And one thing that does not add up, which is now the whole of what
`render_objects` owes.** The original's preview DOES show the objects -
removing them from the port takes the preview from 0 pixels differing to
**53**. But the packer's source is the level bitmap, and image `0x03789`
established that the original never composes objects into it; `prove.sh`
compares against that bitmap with `--no-objects` and gets 100.00%.

All five passes after the terrain loop are identified now, and none draws an
object: image `0x0378A` copies the records into the animation array, image
`0x03839` is the STEEL list at `ds:0x172B` (its `dl = dh & 0xf` and `dh >>= 3`
unpack the nibbles `level_parse` reads), image `0x037C2` is the TRIGGER MAP
(image `0x03808`), image `0x0389F` is the ENTRANCES (image `0x038A3`), and
image `0x036AD` is a clear.

And the contradiction is **settled by dumping the bitmap itself.**
`tools/probe_pvsrc.py` rides along and captures the level bitmap at image
`0x07EB2` - the packer's entry, hit exactly once, at the briefing - and
compares it against the port's canvas:

    the port WITHOUT objects    815 pixels differ
    the port WITH objects        19 pixels differ

and the 815 begin at (707, 36), which is where this level's objects are. So
the objects ARE in the bitmap when the preview reads it, and drawing them is
right.

**Image `0x03789`'s conclusion was a fact about one instant read as a fact
about the program.** What it establishes is that at `prove.sh`'s break - image
`0x0376A` - the objects are not there yet, which is why that comparison passes
`--no-objects` and gets 100.00%. The five passes at image `0x0376A`..`0x03776`
all run after that break, and the briefing comes after all of it. "The
original never composes objects into the level bitmap" was too strong, and it
had stood here long enough to make a correct routine look like a wrong one.

**And the routine is found: image `0x03981`.** Asking
`tools/probe_blits.py` for rows 36..37 - the level's first object is at
(720, 36) - lists three blits, two terrain pieces from image `0x07CB5` and one
from image `0x039BD` with a different source segment (`0x02900` against
`0x07272`), which is what tells an object from a terrain piece in one line.

The loop walks `ds:0x0FEB`, 32 entries; takes x from bytes 0..1 with zero
ending the list, y from 2..3, and the LVL's modifier bytes from 6..7 - image
`0x039A8`'s `bp = dx & 0xFFFE` makes them the blit's flags, bit 15 being
`e[6] & 0x80`, the no-overwrite `level_parse` reads, and the probe caught
`bp = 0x800E`. Width, rows and the graphic come from the object definition,
four planes, blitted at image `0x039BD`.

It sits in a **second composition path at image `0x0394F`**, which is exactly
why `prove.sh` has never seen it: that breaks at image `0x0376A`, inside the
first. Two composition paths, one break, and a claim about the program drawn
from it.

**And the nineteen pixels were the check's, not the port's.** With the check
corrected the composed level with its objects is **243,936 of 243,936, zero
differing**.

`verify_objects.py` rendered the port with plain `--raw`, which draws objects
the DISPLAY-TIME way - `level_anim_draw`, image `0x07BB8` - and compared that
against the original's COMPOSITION of them, image `0x03981`. Two different
routines with different rules, scored against each other, and the difference
read as the port's. `--baked` is the flag that calls `render_objects`, and
with it the same run is exact.

So a residue that three separate leads were chased through - the no-overwrite
rule, a pre-shifted byte blit, an edge offset - **never existed**. Two of those
reads changed the code and all three were worth having, but what they were
aimed at was a harness comparing the wrong pair. This is the fifth harness
fault this session and the first one that was mine, written the pass before.

The rule it does establish is the one this project keeps relearning in new
places: **a comparison is only as good as the claim that the two sides are the
same thing.** `prove.sh` gets this right and says so - it compares the
original's composed buffer against the port's composed buffer. The new check
did not, and nothing about its output looked wrong.

And the three transcribed rules are **unverifiable, measured across the whole
game**. The old rule and the new one differ only where an object pixel lands
on an attribute of 1..7 - non-zero with bit 3 clear - and a sweep of all
eighty sections finds that **zero times in 303,085 no-overwrite object
pixels**.

So the rule that was replaced was never wrong on anything that ships, and no
check built from these levels could ever have told the two apart. That is a
better answer than "unverified": it says where the test would have to come
from, and that it cannot come from here.

**And the first sweep's zeros were not a result, twice over.** They were
recorded here as proof that the `--file`/`--level` path "does not call
`render_objects` at all" and that `--baked` is silently ignored on it. That
is FALSE - instrumenting the branch shows both paths reach it with identical
state, and `--file level001.dat --level 3 --baked` counts 4,176 no-overwrite
object pixels. What actually produced the zeros was a binary that did not have
the counter compiled into it, which this file warns about in another place
entirely: `make ... >/dev/null` hides a build that did not happen, and every
measurement after it reads the previous binary.

The rule that did survive is the one that caught it: **count what the routine
SAW alongside what disagreed.** 0 of 0 is not 0 of 303,085, and only the
second is a result.

Getting there needed a hook rather than the control socket: `break i+0x7eb2`
halts the guest while the menus are still up and every F1 the driver sends is
dropped, so the run never reaches the briefing at all. The routines stay owed - found is a step short of read - but the
marker can say where now, which is the difference between nobody having read
it and nobody having found it.

**And one caveat about the instrument**, because it produced 31 rows of
plausible nonsense: `probe_blits.py` works the destination row out from the
offset at the stride it is given, and the other 31 blits it listed have x of
642, 867, 951 and so on - level coordinates. They are the LEVEL's terrain
composition, whose stride is 208 and not the 80 the briefing uses, landing in
"rows 20..58" by arithmetic rather than by being there. A row filter is only
as good as the stride it is handed, and this file has the same warning about
`--stride` one screen up.

### ds:0x84 is the panel's auto-repeat divider

Nothing here had a name for `ds:0x84`, and it gates the panel. Image `0x03EF9`
decides what a click landed on: below `ds:0x12 + 8 = 0xC0` it is not the panel
at all; at `ds:0x10 - 8 >= 0xD0` it is the minimap; and otherwise - the button
strip - image `0x03F15` refuses the click outright unless `ds:0x84` is zero.

That byte cycles. Image `0x03B53` increments it once a frame from the panel's
own drawing, and image `0x03B3F` catches it at 2, copies `ds:0x82` into
`ds:0x83` and puts it back to 0xFF so the next increment wraps it to zero. So
a HELD button fires one frame in three, which is what gives the release-rate
buttons their ramp - and the minimap, which is a drag rather than a press, is
deliberately outside the divider.

The port is not affected and it is worth saying why rather than just that: it
acts on the button-down EDGE, so a skill selection lands once whatever frame
it arrives on, and buttons 0 and 1 - the only ones the divider exists for -
are refused at image `0x03F27` and not transcribed. The two agree for every
button the port implements, by different means again.

Yield: five flags swept, one bug.

### Two more flags swept, and both came back clean

The sweep is only worth having if its negative answers are recorded too,
otherwise the next pass repeats it.

**`ds:0x55`, the entrance**, has four sites and the port has all four: image
`0x00394` opens it at frame 35 of the opening sequence, image `0x00447` clears
it at level start, image `0x030B0` stops the release until it is open, and
image `0x07C26` stops object id 1 animating until it is.

**`ds:0x61`, the assignment latch**, has three, and they are an edge detector
written as a poll. Image `0x0316E` tests `ds:0x14` bit 0 - the HELD button -
and clears the latch whenever it is up (image `0x031BE`); image `0x03175`
refuses a second assignment while it is set, and image `0x03177` sets it. So a
held button assigns once and then nothing, however long it is held and
whatever it is dragged over.

The port reaches the same behaviour by a different route: it acts on the
button-DOWN edge, in both the script path and the SDL one, so a held button
produces one assignment and no more. Equivalent, and worth writing down as
equivalent-by-a-different-mechanism rather than as a match - if the port ever
grows a polled input path, this is the thing that quietly breaks.

Yield so far: three flags swept, one bug (`ds:0x2B` at image `0x03944`).

### PAUSE stops the objects too, and the port's kept turning

A real fault in the port, and the pause case is the only thing in this
repository that could have seen it: its play area went from **215 of 500
frames exact to 499**, the one left being a single cursor.

Image `0x03944` is the only thing that reaches the object-animation advance at
image `0x07C0A`, and it is a guard:

    cmp byte [0x2b], 0xff
    je  (return)
    jmp 0x7c0a

`ds:0x2B` is the pause flag - image `0x04018` toggles it with
`xor byte [0x2b], 0xff` - and the clock (image `0x0020C`) and the release
(image `0x030A6`) already guarded on it in the port. The objects did not, so
from the moment the button went down the original's objects were frozen and
the port's kept turning, and **they never came back into step**: about twenty
pixels differing on every frame from the press to the end of the run, long
after the game was unpaused.

Two things worth keeping:

- **A permanent divergence that starts at an input is a state the input
  changed, not a drawing bug.** The residue was ~20 pixels in one 32x5 band,
  which looks like a sprite; what it actually was is a phase difference in an
  animation, and the give-away is that it began exactly at frame 200 and never
  healed.
- **It needed a run that pauses.** Every frame-by-frame number this port has
  came from levels nobody touches or from single skill assignments. Five
  thousand one hundred frames of "Just dig!" cannot see this, and neither can
  any of the eight skills.

And a correction to the panel-order note written earlier this session: image
`0x03944`, at image `0x001B6` in the play loop and image `0x00495` in the
set-up, is **not "the play area"**. It is this guard in front of the object
animations. The addresses were right and the label was a guess.

### What "exact on every frame" covers, measured

Five of the six real port bugs found in one session were in code paths no
reference here touches. That is not an argument against the frame-by-frame
numbers - they are exact and they are real - it is an argument for knowing
what they cover, which nothing here said until now.

`tools/coverage.py` reads all 80 levels and marks which features the five
levels this repository compares actually reach:

    object id 3          45 in the game,   0 measured
    object id 4          21 in the game,   0 measured
    only_on_terrain      66 in the game,   0 measured

Ids 3 and 4 are the ONE-WAY ARROWS, and `only_on_terrain` is the flag whose
missing test drew them over the black. **The tool would have predicted that
bug**: three rows of zero, in the feature a player reported a week of frames
later.

Two of ten object ids and one of two object flags are never exercised. That is
the honest shape of the claim - `exact on every frame` over five levels, out
of eighty, covering eight of ten object kinds.

It is reported by `verify_all.sh` and does not fail it. A feature nobody
measures is not a bug, and pretending otherwise would make the suite cry wolf.
It is the list of places a bug can sit unseen, which is the thing to have
before reading a run of 100.00% as "correct".

### A fourth level, chosen by the coverage gap, found a bug on its first run

`tools/coverage.py` said object ids 3 and 4 and the `only_on_terrain` flag
were measured by NOTHING. So level004.dat section 3 - "Tribute to M.C.Escher",
twelve one-way walls - was recorded and added to the suite. Its
`ds:0x1F6B` index is **77**; the file's own header at the recording confirms
it, `0032004b00410005` on both sides.

Its panel was 1,100 of 1,100 exact from the first run. Its play area was
**0**, and the whole difference was one thing:

    1,409 pixels, every one of them (original DAC 5, port attribute 1),
    and only where the one-way arrows are. The terrain around them agrees
    to the pixel.

DAC 1 is blue and DAC 5 is red, so the original's arrows were red and the
port's blue. It is the same objects a player photographed for the clipping,
one layer further in.

**It is fixed, and the level is now 1,100 of 1,100 exact in both halves.**

The answer is that **an only-on-terrain object is not drawn in its sprite's
colour at all.** Image 0x096BA sets the graphics controller's Set/Reset to 4
and image 0x096C2 its Enable Set/Reset to 0x0E, so planes 1, 2 and 3 are
written from that constant - 0, 1, 0 - and only plane 0 comes from the ink.
An arrow pixel lands as attribute **5** whatever the sprite holds, and its
plane 3 is CLEARED, so an arrow is not terrain afterwards. `game.c`'s
`object_pixel_value` is that rule, and both drawing paths go through it.

Four things were ruled out first, each measured rather than argued, and all
four are worth keeping because each was a plausible answer that was wrong:

- **the sprite.** At the port's ink base the arrow decodes uniformly to
  attribute 1, 200 pixels, on every one of its eight frames - and so does the
  other one-way tile. `reconstruct/tests/screens.c` now asserts that, because
  it is the premise the fix rests on: a sprite of one colour cannot produce
  two, so the second colour has to be the blitter's.
- **the offsets.** Image 0x07BEF sets `di = [si+8]` and image 0x07BF2 sets
  `si = [si+0x17]`, so SI is the ink at 24600 and DI the mask at 25112 - the
  port's `frames_base_loc` and `mask_offset` exactly.
- **the plane arrangement.** Four planes at 128 apart with the mask at +512 is
  the only reading that decodes to a coherent sprite; mask-first gives 8 and
  five planes give noise. No base within +-1280 gives 5.
- **the attribute-to-DAC path**, which was the leading hypothesis and had a
  whole paragraph here proposing `ground2o.dat`'s palette at offset 960 as the
  next thing to read. `tools/probe_attrpal.py` settled it in one run: the
  attribute registers are the identity map `00..07 10..17` on this level in
  **all 1,261 frames sampled**, so attribute 1 is DAC 1 and the port's palette
  is right. The port's sixteen entries match the original's DAC exactly -
  `palette[0..7]` against DAC 0..7 and `palette[8..15]` against DAC 16..23.

Two further things about how this went are worth recording.

**A residue of one relation is a rule, and this one said so twice.** Every
differing pixel was the same pair - port 1, original 5 - and 5 differs from 1
in plane 2 alone. That is the shape `CLAUDE.md` already describes as "a rule
you have not found", and it was pointing straight at a blit that forces a
plane on.

**The `bp` flag inference was wrong, and reading the entry corrected it.**
The arrows carry `bp = 0x400E`, and bit 14 was recorded here as reaching image
0x0969C. It does not from the blitter's own entry: image 0x088EA jumps to
image 0x08D66, which forces planes 1..3 to zero and can only produce attribute
0 or 1. Chasing that path was three rounds of reading a routine that draws
something else. What ended it was giving up on the flags and searching the
whole image for every Set/Reset load - eighteen of them - and finding the one
that writes 4.

**And the 6-pixel offset that looked like a second bug was not one.** The port
drew its arrows six pixels left of the original's, which read as a placement
error on top of the colour error. The tiles are **animated** - eight frames
that march the arrows sideways - so a static `--raw` render at frame 0 is
simply a different phase from a capture at frame 600. Sweeping `--obj-frame`
found the match at 5, and the frame-by-frame comparison, which advances the
animation itself, is exact.

### A third coverage axis: the trigger EFFECTS

`coverage.py` counted object ids, object flags and terrain flags. It did not
count the thing the loop's three original bugs were actually about - the
trigger EFFECT an object carries, which is not in the LVL at all. The level
names an id; the effect is a field of the GROUND file's object record,
`[si+0x14]`, so it needs the graphics set read alongside.

    TRIGGER EFFECTS             in the game   in a measured level
      0 none                     178          18
      1 exit (0x02F3B)            84           8
      4 TRAP (0x02FB6)            22           6
      5 water (0x02FF8)           63           1
      6 fire (0x0304F)            41           3
      7 one-way right             45          14
      8 one-way left              21           2

Every effect is exercised, but **water by ONE object** across six levels -
and drowning is one of the three faults this loop exists to chase. Effects 4
and 8 are barely better.

Effects 2, 3 and 0x0A are a BLOCKER's three columns, written into the map at
run time rather than from an object record, and 9 is steel from the LVL's own
list. They cannot appear in a table built from object records, and the table
says so, because otherwise their absence reads as a gap.

**The instrument was checked twice before any of this was reported**, and
needed it both times: the parse was verified against `game.c`'s own
`ground_load` on all sixteen ids of five graphics sets, and the first version
scored EVERY level against `ground0o`, because `graphics_set` is a big-endian
word at 0x1A and `sec[0x1A]` is the high half - always zero. That table
claimed effect 6 does not exist in the game. It does: `ground1o` has three ids
carrying it and 41 objects use it.

### A fifth level, chosen the same way, found a TERRAIN bug

`tools/coverage.py` had one object id left that nothing measured - id 4, the
mirrored one-way wall, 21 of them in the game. level006.dat section 3, "One
way or another", carries two of those and two of id 3, in graphics set 0
rather than set 2. Its `ds:0x1F6B` index is **95**; the level's own header
confirms the recording picked the right one, `0032004b004b0004...000a0000`,
which distinguishes it from level007 section 1 - the two agree for their first
eight bytes.

It found a difference on its first run, and not in objects at all:

    814 pixels, on every one of 1,100 frames, all of them terrain the
    ORIGINAL draws at the bottom of the play area and the port did not.

The port was **erasing terrain the original keeps**. A terrain entry's first
byte carries three flags, and 116 pieces in the game - across **9 of the 80
levels** - carry both `erase` (0x20) and `no-overwrite` (0x80). The port
tested erase first and blanked them.

Image 0x07C67 unpacks that byte into the blitter's flag word and the erase bit
sets **two** things there: `and dx, 0xc0` puts word bit 13 into bp bit 6, and
image 0x07C7B ORs in 0x0F00 beside it. Image 0x08905 tests bp bit 6, jumps to
image 0x0909C, and there image 0x090B8 sets Set/Reset to 0 while image 0x090BF
sets Enable Set/Reset to bp bits 11..8 - the 0x0F - so all four planes are
written as zero. That is the erase, and it is why the nibble exists.

But **image 0x088F3 tests bp bit 15 first**, and jumps to image 0x08B42, an
ordinary masked draw that never looks at the erase nibble. A piece carrying
both flags is drawn, not erased. The port's two branches are now in that
order, and level006/3 is **1,100 of 1,100 exact in both halves**.

**A partial improvement came from a rule that was wrong.** The first
hypothesis was that erase only works when the piece is byte-aligned, since the
paths that read the nibble are reached by other dispatch arms. Wiring it in
took 814 differing pixels to 459 - real progress by the only number on offer,
and a different wrong answer. What the residue showed was that the port had
stopped erasing two pieces that the original DOES erase, which is how the
distinction between those pieces and the third one surfaced. The rule was then
read out of image 0x07C67 rather than fitted, and went to zero.

`tools/prove.sh` is untouched by this: its level, level009 section 2 - "Just
dig!" - has no piece carrying both flags. That is checked rather than assumed.

Two levels chosen by a coverage gap, two bugs, and neither of them in the
thing the gap was about. That is the argument for choosing the next reference
by what nothing measures rather than by what looks interesting.

### A sixth level closes the last coverage gap, and was exact first time

`tools/coverage.py` grew terrain-flag counting after the erase bug, and named
two combinations nothing exercised: `upside_down + erase` (89 pieces) and all
three together (17). level007.dat section 1, "The Far Side", is the only level
carrying both - one of the first and six of the second - and its `ds:0x1F6B`
index is **101**. Its header confirms the recording,
`0032004b004b0004**0002**0001`, where level006 section 3 has `000a0000` in the
same place and the two agree for their first eight bytes.

**1,100 of 1,100 exact, play area and panel, on the first run.**

That is the other thing a coverage gap can turn into, and it is worth having
either way: it converts "nothing here has ever looked" into "this was looked
at and agreed". It also generalises the erase precedence fix, since these are
erase pieces carrying a second flag and the port draws them correctly.

Six levels are now compared frame by frame, and `coverage.py` reports **0 of
10 object ids, 0 of 2 object flags and 0 of 7 terrain flag combinations
unexercised**.

### The level did not fade out, and the original does

Reported from watching the emulator: leaving a level fades it rather than
cutting away. The port cut away.

**Measured rather than inferred.** `out/ref_i8`'s own manifest - level001
section 3, the recording that ends when all eighty are dead - walks its lit
DAC entries down at the end with `over` already 0xFF:

    frame  553..555   lit_dac 13
    frame  556..557   lit_dac 10
    frame  558..561   lit_dac  9
    frame  562..563   lit_dac  7
    frame  564..566   lit_dac  0

So the fade is the original's, on the level's own end, and `record_level.py`
has been printing it in every capture all along.

The port's fade machinery already existed for the menu; what the level needed
is that it carries TWO palettes - the play area's and the panel's, swapped
part-way down - so both walk. It reuses the loop's own `screen` buffer and
conversion rather than composing a second frame, because the last frame the
game composed is the one that should fade.

It runs only for a real window. Under `LEM_SCREEN_DIR` this is a capture
harness with a frame budget, and frames after that budget would be scored as
the game's - the six frame-by-frame levels still write exactly 530, 1100,
1100, 1100, 1100 and 1100 frames and are all still exact.

### An effect of ZERO is still stamped, and the skip cited the wrong address

The one cell of 16,040 that `verify_triggers.py` found - row 27, column 208,
original 0x20 against the port's 0x00 - was a real transcription fault, and it
was settled from the LEVEL'S OWN RECORDS rather than from a run:

    object 2 is id 6, effect 0, trigger 1x1 at (0, 0)
      -> stamps column 208 row 27 with (2 << 4) | 0 = 0x20

Exactly the cell, exactly the value. Object 0 also has effect 0 and stamps
0x00, which is indistinguishable from an empty cell, which is why only one
showed.

`level_triggers_build` had `if (!go->trigger_effect) continue;` citing image
0x037E3. **That address is not that test.** It is the `je` for image 0x037E1's
`and ax, ax`, and AX there is the object's X - image 0x037DC reads bytes 0 and
1 - so it is the empty-slot test `level_parse` already does. There is no
effect test anywhere: image 0x03812 ORs `[si+0x14]` into `index << 4` and
image 0x0381D stamps unconditionally.

It is not cosmetic. **The stamp overwrites**, so an effect-0 object lying over
a real trigger erases it in the original and did not here. On these levels the
effect-0 rectangles are 1x1 and nothing is under them; that is a fact about
these levels.

Also transcribed: image 0x037C6's `cx = 0x10`. The trigger pass walks the
first SIXTEEN object slots where image 0x07BB8 draws all thirty-two. No level
in this set has more than sixteen objects, so it changes nothing.

**How nearly this went the other way** is the part worth keeping. The first
attempt at this fix was measured against a dump a concurrent background run
had overwritten, scored worse, and was reverted as refuted - with a comment
written into `game.c` claiming it had been measured. See the note in
`CLAUDE.md` about fixed output paths. The reading survived only because it was
re-derived from the level's records afterwards, where no run could corrupt it.

### The rate buttons stepped TWICE per press

Reported from playing: the plus and minus move the rate by the wrong amount.

The step itself was never wrong - image 0x0404F is `inc byte [0x67]` and image
0x0407F is `dec`, one either way, and the port did the same. It FIRED TWICE.

The divider was tested in `level_rate_held`, outside the handler, so a press
arrived as an edge and stepped for free; the very next frame the held path
found the divider at rest and stepped again. One click, two steps.

The original tests ds:0x69 **inside** the handler - image 0x0403D for the plus
and image 0x0406C for the minus, BEFORE either bound is looked at - and image
0x03B8C turns it from the PANEL'S REDRAW rather than from the button. Image
0x03B86 compares ds:0x68, the value the panel is showing, against ds:0x67, and
skips the whole block when they agree, so the divider only turns while the
drawn number is stale. That is what makes the first press always take and the
next two frames refuse.

The port now has that shape: the test is in `level_rate_click`, and
`level_rate_tick` turns the divider from `level_panel_build`, with ds:0x68
modelled as a "is the drawn number stale" flag because only that question is
ever asked of it.

**Why the check that exists did not catch it.** `tools/verify_rate.py` agrees
with the original on 240 of 240 clicks - and pokes ds:0x69 to zero before every
one, on BOTH sides. It scores the step arithmetic and its two bounds and says
nothing whatever about the pacing. `reconstruct/tests/screens.c` now tests that
frame by frame, and `--rate-probe` clears the divider before each click the way
the guest side does, so the two halves of that comparison still make the same
claim.

### The level bitmap was eighty columns too narrow

Found by a SEVENTH level, level009 section 1 - "Rendezvous at the Mountain",
Mayhem 30, the last level of the game - recorded because the new trigger-effect
axis showed effect 5, water, resting on a single object across the whole suite.

Its play area was exact on all 1,100 frames. Its PANEL was 992, in twelve runs
of exactly nine frames spaced 43 apart - and 43 is that level's release
interval, `((99 - 20) >> 1) + 4`. Down to one character cell: the status line's
OUT digit, the original showing 3 where the port showed 2.

**Both sides agreed on `released` and disagreed on how many were ALIVE.** At
frame 141 the port had killed lemming 1 - `alive=255`, x=1600, y=180, falling -
and the original counted it for nine more frames. The rule is not the
difference: image 0x02210 is `cmp bp, 0xb4 / jl`, so the original kills at
`y >= 0xB4` exactly as the port does.

The port got there early because **its level bitmap was 1584 wide and the
original's is 1664**. Image 0x05DC6 addresses it as `0x4A40 + y * 0xD0 + x/8`,
and 0xD0 bytes is 1664 pixels - so terrain past column 1584 is real memory the
original composes into and walks on. 1584 is the width the game SHOWS. This
level places TWENTY-FIVE terrain pieces at x up to 1596, and a lemming out
there found no ground in the port's array and fell through the floor.

`LEVEL_W` is now **1648**, and the sixteen matters: 1664 is in RECORD
coordinates and the canvas is offset by sixteen, so it covers record 16..1663.
Setting it to 1664 made `compare_composed.py` refuse to broadcast, (160,1648)
against (160,1664) - which is how that was caught rather than shipped, and is
the third coordinate system this project keeps tripping over.

The DISPLAY is deliberately unchanged. Image 0x040E8 clamps the scroll at
0x500, which is this port's `view_x` 1264, so those columns are never shown -
only walked on. `max_x` had been derived from the canvas width, which was right
only while the canvas WAS the displayed width; it is now `LEVEL_SCROLL_MAX`
with that address against it.

**What made this invisible for so long**, and it is the general point: the
lemming was OFF-SCREEN the whole time. 1,100 frames of pixel-exact play area
sat on top of a wrong count, and the only symptom anywhere was one digit in the
panel. Scoring the panel separately from the play area is what made 44 pixels
in 64,000 visible at all.

    level009/1  play area 1,100 of 1,100   panel 992 of 1,100  before
                play area 1,100 of 1,100   panel 1,100 of 1,100 after

`tools/prove.sh` still reports offset 16, 100.00%, 0 differing - now over 1,648
columns of the original's own composed bitmap rather than 1,584.

### The trigger map is now checked against the original's own

`tools/verify_triggers.py` dumps the map the original builds and compares it
with the port's, cell by cell. It exists because the steel fix below was
justified by READING image 0x03839 and by the port's own render, and nothing
checked it against the original - and because no frame comparison or
screenshot can reach this map at all.

Two levels, chosen so that between them they exercise both halves of the
byte:

    level004/3 - twelve one-way walls, five steel areas, an exit
      effect nibble   16040 agree, 0 differ
      whole byte      16040 agree, 0 differ    (16039 before the effect-0 fix)
      STEEL cells     original 479, port 479

    level002/6 - "Lend a helping hand....", the TRAP level
      effect nibble   16040 agree, 0 differ
      whole byte      16040 agree, 0 differ
      STEEL cells     original 422, port 422

The second one matters for the INDEX nibble, which the first barely touches:
level004/3 has two cells that are not steel or one-way wall, where level002/6
is full of traps and the index is what decides WHICH trap fires. A port that
agreed on the effect and not the index would be right about steel and wrong
about that, which is why the tool scores them separately.

The one byte that used to differ on level004/3 - row 27, column 208 - was a
real fault and is fixed; see the entry above it. Reporting it rather than
rounding it away is what led to it.

**Two instrument faults on the way, and both were caught before anything was
reported.** They are in `CLAUDE.md` because both generalise:

- the map is in the STACK segment. Image 0x023A6 reads it `ss:[bx]` and image
  0x03886 writes it `ss:[bx]`. Dumping from DGROUP - because small-model DOS
  usually has SS == DS - gave a page of other variables that scored the port
  **47% wrong**. What killed it in one look was checking the SHAPE of both
  sides first: the "original's" map held all sixteen effect values in roughly
  equal numbers, and no level's ever does.
- the comparison has to be bounded by what the program can READ. The port's
  array is 64 rows because that is convenient; the original's map has other
  stack variables after it. Unbounded, the tool reported **3,180 differing
  cells** on a map that agrees completely. Image 0x02EC5 refuses `y >= 0xB0`
  and the row is `(y - 0x10) >> 2`, so row 39 is the last; the canvas is
  1,600 wide and the column is `x >> 2`, so column 400 is the last.

### A finished capture cannot stop its own run

Worth writing down because it was tried and reverted. A short capture pays its
whole `--seconds` budget - four frames cost five minutes - and the obvious fix
is for `record_level.py` to call `uc.emu_stop()` once it has them.

It does nothing. `dos_emulator`'s main is a `while running:` loop that calls
`emu_start` again every pass and only leaves on `--run-seconds`, so a hook
gives up one slice and the loop hands it straight back. The measurement said
so before the reading did: the frames and the dump landed at 15:57 and the
recorder was still going two minutes later.

Reverted, with the reason in the comment. Ending early would need an additive
hook UPSTREAM in dos_emulator. A comment claiming an optimisation that does
not happen is worse than no change, because the next reader believes the
comment instead of the clock.

### The steel areas were parsed and never used

Reported from playing and checked against DOSBox: a bomber's crater ate
through a STEEL floor on level004 section 3.

`level_parse` has read the LVL's 32 steel entries for a long time. Nothing
ever put them in the TRIGGER MAP, which is the only thing that answers "what
is under this lemming" - `trig_stamp` was called from one place, the object
pass. So a steel area read as cell 0 and **every test against it was dead
code**:

- the BOMBER's, image 0x023A6, which refuses effects 9 and 5 before image
  0x023B9 digs the crater. That is the one a player hit.
- the BASHER's `(a->tcell & 0x0F) == 9`, equally dead.

Image 0x03839 is the pass that was missing: it walks the entries at ds:0x172B
and stamps **9** over each, `(width + 1)` cells by `(height + 1)` - the two
nibbles image 0x03851 and image 0x03856 unpack - at image 0x03886. It runs
BEFORE the object trigger pass at image 0x037C2, so an object's effect lands
on top of steel, and the port's loop is in that order for that reason.

level004/3's five areas now stamp 479 cells of 9 across canvas x 560..879,
y 136..159 - its floor.

**Nothing here could have caught it**, and that is the part worth keeping.
Every frame-by-frame reference in this repository is a level nobody bombs, and
the eight skill runs are on "Just dig!", which has no steel at all. It is the
same shape as the digger that could be assigned twice: a rule with no
reachable state to check it, found by a player in a minute.

### The right mouse button did nothing, and it should pick the other lemming

Also from playing: with two lemmings under the cursor the wrong one takes the
skill.

The original files each lemming under the pointer into ONE OF TWO slots -
image 0x03D79 - and keeps the LAST that matched in each:

    test byte [si+0x24], 0x80   ; jne -> the BUSY slot
    test word [si+8], 0x8b90    ; je  -> the PLAIN slot

Then image 0x03DB7 tests bit 1 of ds:0x14 and picks the PLAIN one when it is
set, the busy one otherwise, falling back to plain when there is no busy one.
And ds:0x14 is the raw mouse button word - image 0x04269 stores INT 33h AX=3's
BX - so bit 0 is the left button and bit 1 the right. Image 0x03168 does
`and ax, 1` before it will assign anything.

So: **left assigns, right points past a worker at the walker beside it.**

Two things were wrong. `lem_buttons` was never written, so the right button
never switched the preference; and the port treated ANY button as a click, so
the right button also assigned, which the original will not do. Both fixed in
`sdl_io.c`, and the two-slot classification now asks record byte 0x24 as well
as the flag word - the port had no such byte when that scan was written and
has carried `extra` since the builder's shrug, so the comment saying it could
not was simply stale.

### The one-way arrows DO land on a builder's bricks, in both

Reported from playing: the arrows appear over bricks a builder has laid, and
the original was thought not to do that. **It does.** The player checked the
emulator and confirmed it, so the port matches and there was nothing to fix.

Kept because the reading behind it is worth having, and because it is a case
where the reading and the report disagreed and the reading was right:

- a brick is attribute 8 and nothing else - image 0x05F1E and image 0x05F26
  set Set/Reset and Enable Set/Reset to 8, so it is plane 3 alone - and the
  only-on-terrain rule asks exactly `dst & 8`. A fresh brick is therefore
  indistinguishable from terrain to the arrow blit.
- the two writes go to different buffers: image 0x05DC6 puts the brick at
  `0x4A40 + y * 0xD0 + x/8`, the level-wide bitmap, while the blitter's
  destination is the visible page.
- but image 0x080FD repaints the visible page from that bitmap every frame
  for every DIRTY BLOCK, all four planes, and image 0x09A15 marks the blocks
  a blit touches. So the brick reaches the page and the arrows see it.
- and the arrows animate, measured rather than assumed: ~1,366 pixels of the
  band change between consecutive frames of out/ref_i77, for all 1,100.

### An object can carry both flags too, and five of them do

Found by listing what a modifier byte actually holds across all eighty levels
rather than assuming the flags are exclusive. `e[6]` takes exactly four
values - 0x00 (77 objects), 0x40 (61), 0x80 (311) and **0xC0 (5)** - and
`e[7]` is 0x0F on all 454 of them.

The five with 0xC0 carry only-on-terrain AND no-overwrite. `object_pixel_allowed`
ANDed the two conditions, which for such an object is unsatisfiable - `dst & 8`
cannot be both zero and non-zero - so the port **drew nothing at all**. All
five are object id 4 in level003 section 7, "Upsidedown World".

Image 0x093D2 tests bp bit 14 and jumps to image 0x0969C; only if that is
clear does image 0x093DB test bit 15. So only-on-terrain wins and the
no-overwrite bit never gets a say. The gate is now that dispatch.

Rendering that level draws 553 more pixels than it did. **It is not verified
frame by frame** - level003/7 has no reference here, and it is the only level
in the game that would exercise it. The five other levels are unchanged.

This is the same shape as the terrain erase bug two entries up, found the same
way and on the same afternoon: two flags, a dispatch that picks one, and a
port that treated them as independent tests. Worth remembering that BOTH came
from enumerating the data rather than from any comparison.

### And the no-overwrite blit ORs, which nothing here can see yet

Read while confirming the arrows: image 0x0956B sets the graphics
controller's Data Rotate/Function register to 0x10. Bits 4:3 of that register
select the ALU function and 0b10 is **OR**, so a no-overwrite blit ORs the
sprite into what is already in video memory. Image 0x096CA and image 0x08D98
both set the same register to 0, so every other path replaces.

`object_pixel_combine` now does that. **No comparison in this repository can
tell the difference**, and the reason is worth stating rather than leaving as
a passing score: every no-overwrite object measured here has been drawn over
black, and `x | 0` and `0 | x` are both `x`. It diverges only where one object
lands on another's pixels. `verify_objects.py` still reports 815 of 815 object
pixels agreeing, before and after.

It is transcribed anyway, because "unreachable in the levels measured so far"
is a fact about the measurements. It also settles what becomes of the
sprite's TRANSPARENT pixels on that path - the no-overwrite blit consults no
mask, so the whole rectangle goes down - they OR in a zero and change nothing,
where assigning wrote black.

### Sound: the asking is transcribed, the timbre is not

`docs/sound.md` has the interface, read out of the binary. The short of it:

- **The game ships no digitised samples.** `adlib.dat` decompresses to 22,125
  bytes of 8086 CODE - a loadable driver - with a flat 5.6-6.8 entropy across
  every 4 KB and no PCM block in it. There is nothing to play back.
- The driver owns **INT 61h**, and the game asks it for numbered sounds. Image
  0x00589 loads adlib.dat and image 0x0059D does `ah = 1 / int 0x61` to ask
  whether the hardware is there; `tandysnd.dat` goes on the SAME vector. INT
  60h is a second interface of whichever driver is loaded, not a second card.
- **AH=3 and AH=4 do not play.** The driver's dispatcher stores AL in
  `cs:0xED` and `cs:0xF0` - two request slots its own timer ISR reads.
- Two gates at every site: `ds:0x25A7` (bit 0 for an AH=3 call, bits 0-1 for
  AH=4) and `ds:0x2010`.

The port now mirrors the ASK, which is the half that is the game's:
`sound_request(fn, id)` with the original's own AH and AL, and
`sound_enabled(bits)` for the gate. Four sites are wired with their addresses:
the bomber's bang (image 0x02374, AH=3 id 0x0C), the trapdoor releasing one
(image 0x030A0, AH=4 id 0x0D), a panel button (image 0x03F77, id 0x01) and a
skill assignment (image 0x03205, id 0x04). The rest are listed in the doc with
their sites.

**What is NOT reproduced, and says so in `sdl_io.c`:** the timbre. An id's
sound is FM register writes the driver performs, and getting those right means
an OPL2 synthesiser - which is emulating the card. The voices in the port are
its own, a pitch and an envelope each, chosen to be distinguishable and
measured off nothing. They make the game audible and the call sites checkable
and they are not the original's sound. Replacing them means reading the
driver's instrument tables - the 32-byte runs at +0x2BEA - and synthesising
two-operator FM.

Nothing visual moved: the three levels are still exact in both halves,
`prove.sh` 100.00%, `verify_rate` 240 of 240.

### The one-way arrows were drawn over the black beside their wall

Reported from playing, with screenshots of level004/3 and level005/8: arrows
floating outside the block they belong to.

`only_on_terrain` - the LVL's `e[6] & 0x40` - was tested in exactly ONE place,
`render_objects`, and **`level_anim_draw` never applied it**. That second
routine is the one the game actually displays, so a one-way wall's arrows went
down wherever the object's rectangle reached.

Image `0x09709` is the rule and it mirrors the no-overwrite one directly: a
blit with `bp` bit 14 reaches image `0x0969C`, which sets the graphics
controller to READ MODE 1 comparing plane 3 alone and then does
`and ah, [bx]` - the destination's terrain AND the object's own mask. The
arrows go down only where plane 3 is already set.

Measured: **432 pixels** on level004/3 and **101** on level005/8, every one of
them arrow-blue becoming black.

**This is the second time the same split has bitten.** Two routines draw
objects here - the composition and the per-frame draw - and a rule added to
one is not added to the other. The no-overwrite tests had drifted apart the
same way, and that one was invisible because the two rules happen to agree on
every shipped level. This one was not invisible; it was just never looked at.

And nothing here could have found it: **none of the three levels this loop
measures has a single `only_on_terrain` object.** level004/3 has twelve.
Frame-by-frame comparison of three levels says nothing about a flag none of
them carries, however many frames it runs for.

### Tiling the sprite file found a second builder-shaped bug, unreported

The builder's fix rested on its region abutting the basher's. That argument
generalises: lay every sprite region out from the port's own constants,
doubling the ones that face both ways, and see whether they tile MAIN.DAT.

Nine of twelve abut exactly. Two of the three gaps are sprites the port
already has and the tiling did not know about - the climber's 768 bytes
(8 frames x 2 directions x 12 rows x 2 planes) fill 2064..2832 exactly, which
is `CLIMB_SPR_RIGHT` and `CLIMB_SPR_LEFT`, and the exit's 416 fill
16816..17232.

The third was real. **The shrug faces both ways and the port drew one.** Its
base is 19440 and eight frames of 40 end at 19760, where the next thing the
port knows about is the blast at 20080 - exactly 320 bytes, which is eight
more frames. Image `0x070AB` adds the STATE for a lemming facing left, and
image `0x027CC` sets `[si+0x29]` to 8 as the builder gives up, so
`19440 + 8 * 40 = 19760` is where the left-facing eight are. Decoding frame 8
shows the mirrored shrug, 142 of 160 cells agreeing with a strict mirror of
frame 0.

**Nobody reported this one.** It is the same fault as the builder's, in the
same shape, and it was found by arithmetic on constants rather than by anyone
playing - which is the first time on this project that a bug of this class has
been caught before a player hit it.

Both abutments are asserted in `tests/screens.c` now, against the numbers
rather than against `game.c`'s defines.

**And the last gap was my table, not the port.** 18800..19440 is the BLOCKER,
which `game.c` has had all along at `BLOCK_SPR_BASE` - sixteen frames of 40,
exactly 640. With it in, the section tiles **completely**:

    walker        0 +  720 =    720
    digger      720 + 1344 =   2064
    climber    2064 +  768 =   2832      8 frames, both ways
    drown      2832 +  640 =   3472
    hoist      3472 +  768 =   4240      8 frames, both ways
    builder    4240 + 2496 =   6736     16 frames, both ways
    basher     6736 + 3840 =  10576     32 frames, both ways
    miner     10576 + 3744 =  14320     24 frames, both ways
    fall      14320 +  320 =  14640      4 frames, both ways
    float     14640 + 1536 =  16176      8 frames, both ways
    splat     16176 +  640 =  16816
    exit      16816 +  416 =  17232
    burn      17232 + 1568 =  18800
    blocker   18800 +  640 =  19440
    shrug     19440 +  640 =  20080      8 frames, both ways
    blast     20080 +  640 =  20720

Sixteen regions, **no gaps and no overlaps**. That is a complete check on every
base and every frame count this port believes, and it is what made the shrug's
missing direction visible - its eight frames left 320 spare and nothing else
could have used them.

It also confirms a NEGATIVE that would otherwise be a guess: the blocker has no
direction term in its draw, and the tiling says it cannot have one - sixteen
frames of 40 fill its region exactly, with no room for a second set. The same
holds for the drowner, the splatter, the burner and the blast.

A sweep that finds nothing is worth having when it can say what it covered.
This one covers 20,720 bytes and leaves none of them unexplained.

`tools/tile_sprites.py` does it on demand now, reading the constants out of
`game.c` so it checks what the PORT believes rather than a table here that
could drift from it. It reports 16 regions and 0 gaps, it fails loudly if a
constant it needs is missing rather than printing a partial answer, and it was
checked against a fault it should catch: `BUILD_SPR_FRAMES` at 15 instead of
16 comes back as one gap. Wired into `verify_all.sh`.

### And a held rate button ramps, the way the original's does

The rate buttons stepped once per click and stopped. The original's ramp when
one is held down, and `ds:0x69` is how: image `0x03B8C` runs it from the
panel's own per-frame update in exactly the shape `ds:0x84` has for the skill
strip - caught at 2 it is put to 0xFF and the `inc` at image `0x03B9C` wraps
it to zero - and image `0x0403D` refuses a click whenever it is not zero. So a
held button walks the rate **one frame in three**.

The port takes a press as an EDGE, which is right for the skills: image
`0x03170`'s latch gives them one assignment however long the button is down.
So the repeat is driven the other way round - `sdl_io.c` calls
`level_rate_held` once a frame while a rate button is held, and the divider
lives in `game.c` where it belongs. Same three-frame pace, reached from the
opposite direction, and the split is the original's own: buttons from 2 up go
through the latch at image `0x03F2E` and 0 and 1 never touch it.

`verify_rate.py` still reports **240 of 240**, `prove.sh` 100.00%, the three
levels exact, and every skill's record 0 of n differing.

### The rate now has a check, and it is checked against a fault

`tools/verify_rate.py` drives image `0x04032` itself - the rate is a pure
function of three bytes, so it sets `ds:0x2B`, `ds:0x64` and `ds:0x67`, points
CS:IP at the handler and reads back `ds:0x67` and `ds:0x49`. The walk is the
one a player makes, from the level's own rate up to the ceiling and back down
to the floor, because that is the only way the PORT's rate can be moved too:
it lives in a pair of globals rather than an argument, and `lemdev
--rate-probe` walks it the same way.

    240 of 240 clicks agree on both the rate and the interval

It needs no level, only a machine far enough in that DS holds DGROUP, so it is
ninety seconds rather than fifteen minutes - the same trick
`verify_results.py` uses.

**And it was checked against a fault it should catch.** Taking the floor out -
`g_rate == 0` in place of `g_rate <= g_rate_min` - it reports from click 169:

    click 169 down: port rate 49 step 29, original rate 50 step 28
    click 170 down: port rate 48 step 29, original rate 50 step 28

which is the minus walking past the level's own rate while the original holds.
A check written for a bug a player found is worth exactly what it can catch,
and now that is measured rather than assumed.

### Two more from playing: the release rate did nothing, the builder faced one way

Both reported by the user playing the game, and neither is visible to anything
in this repository - the same as every other fault a player has found here.

**The panel's minus and plus did nothing**, because the port read image
0x03F27 as refusing every button below 2. It does the opposite:
`cmp al,2 / jge 0x3f2e / jmp 0x4032` SENDS 0 and 1 to the rate handler, and it
is the buttons from 2 up that go through the latch at image `0x03F2E`. One
misread word - "refuses" for "routes" - and a whole control was inert.

What the handler does, read out of image `0x04032`:

    ds:0x67   the rate in force, which the buttons move (image 0x036C4)
    ds:0x64   the LEVEL'S OWN rate, and the floor the minus stops at
              (image 0x036C1) - a level can be sped up and never slowed
              below what its header asked for
    99        where the plus stops (image 0x04046)
    ds:0x49   the interval, ((99 - rate) >> 1) + 4 - image 0x04053 writes it
              as four negations and the port already had the formula

The panel's first two numbers are those two bytes, not one value twice: they
start equal, which is why a single `lv->release_rate` stood in for both until
the buttons worked.

Named rather than transcribed: `ds:0x69`, tested at image `0x0403D`. It is a
divider of exactly the shape `ds:0x84` has for the skill strip - image
`0x03B8C` catches it at 2 and the `inc` at image `0x03B9C` wraps it - so a
HELD button steps the rate one frame in three. The port acts on the button-down
edge, so one press is one step and holding does not ramp yet.

**The builder faced one way.** Its branch computed
`BUILD_SPR_BASE + anim * FRAME` with no direction term, so a builder laying
bricks leftwards was drawn with the right-facing sixteen. The basher and the
miner two branches down already had the rule and even explain it in their own
comment - image `0x070AB` adds the STATE to the frame index when the lemming
faces left - and the builder's state is 0x10 (image `0x0330A`).

Two independent things confirm the sixteen-and-sixteen. The region abuts:
**4240 + 32 x 78 = 6736, which is `BASH_SPR_BASE`** - thirty-two frames fit
exactly between the builder's start and the basher's, where sixteen would
leave 1,248 bytes of nothing in a file whose every other sprite region touches
its neighbour. And decoding frame 16 shows the left-facing builder: the arm
and the brick mirror across, 159 of 208 cells agreeing with a strict mirror of
frame 0, which is what a hand-drawn pair looks like rather than a flipped one.

**Neither is checked by anything here, and that is the point.** The builder's
own case still scores 399 of 400 because its reference builds to the RIGHT;
the rate buttons are never pressed in any recorded run. Both were found by
someone playing, both are read out of the original, and the repository would
have gone on reporting everything exact.

### What "owed" means for screen_start, now that the four calls are read

Reading them turned a vague debt into a specific decision. The port's
`screen_start` and the original's set-up sequence are not the same four
things:

    image 0x04DEF  clear the page       -> the port's calloc, which is not the
                                          same act - the original clears VIDEO
                                          MEMORY it already owns
    image 0x04BF5  declare the surface  -> NOTHING. The port has no blitter to
                                          bound, so ds:0x66F0/2/4 have no
                                          counterpart
    image 0x0531E  the palette          -> the MENU_PAL6 loop. The one thing
                                          that lines up
    image 0x05017  the level preview    -> draw_preview, a different routine

and the port does two things the original's sequence does not contain: loading
main.dat, which is DOS's work through image `0x01D2E`, and tiling the
background.

So the debt is not "write out four calls". It is a decision about where the
port's boundary goes, and transcribing it means restructuring around the
original's split. `level_panel_build` and `dat_load` raised the same question
and answered it - no such routine, and IO. This one is neither: the original's
sequence exists and the port's is a different four things. Worth doing
deliberately or not at all, and now it can be.

### All five owed routines have an address now

The provenance tool's list is unchanged at five, and that is right - none of
them is transcribed. But the heading it prints, *"game logic the original has
a routine for, not yet read"*, is no longer true of any of them. Every one has
been located:

    render_terrain          image 0x03752's loop, blitter at image 0x07C67;
                            the UNPACKING is transcribed already
    render_objects          image 0x03981, in the second composition path at
                            image 0x0394F
    screen_start            image 0x04DEF, 0x04BF5, 0x0531E, 0x05017
    draw_preview            image 0x04639 -> 0x05017 -> 0x07E76, packer at
                            image 0x07EB2
    menu_briefing_screen    image 0x04566, composition at image 0x045EF

`menu_briefing_screen` was the last, and its own comment had listed what was
missing: *"the clear at image 0x04DEF, the fade at image 0x05329, image
0x0394F, image 0x07EB2, the page registers, and the draw at image 0x0463C"*.
Four of those six were read this session for other questions entirely - the
clear as the red herring in the preview's write probe, `0x0394F` while
chasing where the objects come from, `0x07EB2` as the preview's packer. Only
the fade and the text drawer needed looking at, and `0x0463C` turned out to
pick its last line from `ds:0x25A6` - the input device - so the briefing says
mouse, joystick or keyboard.

**The distinction the tool draws is still the right one.** Found is not read
and read is not transcribed, and the count stays at five until the C carries
the addresses. But "nobody has looked" and "here is the address, nobody has
written it out" are different debts, and only the second can be worked on
directly.

### screen_start's four set-up calls, and what sets the blitter's bounds

`screen_start` said "image 0x04967 and 0x04A01 both open with 0x04DEF,
0x04BF5, 0x0531E and 0x05017 - and none of those four has been read". All four
are read now:

    image 0x04DEF   clear the page - cx = 0x6D60, 28,000 bytes, 640 x 350 / 8
    image 0x04BF5   declare the surface: page bases ds:0x1F76 = 0 and
                    ds:0x1F78 = 0x6D60, fine scroll 0, then image 0x8808 with
                    ax = 0x280 (640), bx = 0x15E (350)
    image 0x0531E   the palette - sixteen entries through image 0x055B7,
                    which branches on ds:0x1F7C and indexes ds:0x52D4
    image 0x05017   the level preview - `xor bx,bx / jmp 0x7E76`

**And image `0x8808` closes a loop from earlier this session.** It stores its
`ax` in `ds:0x66F2`, `bx` in `ds:0x66F0` and `ax >> 3` in `ds:0x66F4` - the
three words `tools/probe_clip.py` measured at 352, 160 and 44 inside a level.
So the blitter's clip limit, the thing two wrong guesses were built on before
it was measured, is nothing more than **the width of whatever surface was last
declared**: 640 on the menus, 352 in a level. The measurement said what it was;
this says why.

Two of the four were already read this pass for other reasons - `0x04DEF`
turned up as the red herring at the top of the preview's write probe, and
`0x05017` is the preview builder's wrapper. A routine read for one question
answers another.

### Score the thing under test, not the picture it sits in

Three passes running, the fault was my own instrument: a check comparing two
different routines, a sweep that exercised none, and a sweep against a stale
binary. Each produced a clean plausible number. The common shape is a check
that never established it reached what it names.

`verify_objects.py` had it too. It compared 243,936 pixels of which barely 800
are an object, so **`render_objects` could break outright and the headline
would fall 0.3%**. It now renders the level a second time with `--no-objects`
and differences the two, which gives the object footprint for nothing, and
scores that separately:

    whole bitmap    243936 of 243936 agree, 0 differ  (100.0000%)
    OBJECT pixels      815 of    815 agree, 0 differ  (100.0000%)

**And it was checked against a fault it should catch**, which is the step the
last three passes skipped. Shifting every object one pixel right:

    whole bitmap    243479 of 243936 agree, 457 differ  (99.8127%)
    OBJECT pixels      458 of    817 agree, 359 differ  ( 56.0588%)

99.81% reads as a rounding error; 56% does not. A run whose two renders come
out identical now reports that it scored **no object at all** and returns a
failure, because that is what every one of this session's instrument faults
looked like from the outside: a number, with nothing behind it.

### The digger's panel had scored 0 of 676 and nobody was shown the number

`verify_skills` prints one line per case and it was the PLAY AREA's. The
digger's play area is exact on 658 of 676 frames, which reads as a case in
good shape; its PANEL was exact on **0**, and had been for as long as the case
has existed.

Two faults, and they protected each other:

- **The poke was per harness, not per reference.** `LEM_SKILLS=20` matches
  what `tools/record_skills.sh` passes to `record_level.py --skills`, so the
  references it made need it. `refplay_full` is not one of them - it is the
  original full playthrough of "Just dig!", which stocks only diggers and
  needs no help - so the port was drawing twenty of everything beside a panel
  showing the level's own counts. Taking the poke off that row: **0 to 666**.
- **Only one of the two numbers was printed.** The half being reported was the
  half that was already exact.

The other three rows that `record_skills.sh` did not make were each tried both
ways rather than assumed, and none is ambiguous: `ref_s2b` 399 against 1,
`ref_s6b` 398 against 1, `ref_l2f` 479 against 1 - all three do need the poke.
The table now carries the answer per row with that evidence beside it.

**This is the fourth harness fault this session**, after `out/park.txt`'s
single parked pointer, `LEM_SKILLS=20` on the nuke, and `verify_all`'s
expanded env assignment. Every one of them scored the reconstruction as wrong,
and none of them was findable from the number - only from asking what the
harness fed each side.

And printing the panel immediately showed something else: nearly every skill
case has **168 pixels differing at frame 0** and nowhere else. That is the
poke landing after the level record is read, so the guest's first frame still
shows the level's own counts while the port already shows twenty. A harness
artefact, not the port - the three levels, which take no poke, are exact at
frame 0 in both halves.

### One pixel of the miner's tunnel, and four things it is not

`verify_skills` scores the miner's SCREEN at 262 of 400 frames, the worst of
the eight, and 137 of those 138 differing frames are a **single pixel** - the
same one, at screen (159, 81) on "Just dig!" driven by `out/s8.txt`. The
original holds it and the port clears it.

It is terrain colour, and it is **cosmetic only** - a claim made twice here
that the port had a one-pixel hole in its solidity map was wrong. The
original's pixel is attribute 2, `0010`, whose PLANE 3 bit is clear, and plane
3 is what solidity is: image `0x05DC6` tests it in the guest and `lem_solid`
tests `& 8` in the port. Non-solid on both sides. What differs is that the
original still shows a terrain colour where the port shows background.

That also explains the shape of it. The clear made the pixel non-solid - it
cleared planes 2 and 3 - and left plane 1's bit standing, so what is left is
empty ground that has not stopped being drawn. The port writes a plain zero
and has nothing to leave.

What it is not, each checked rather than argued:

- **not the mask.** `tools/probe_mine.py` reads the miner's masks back out of
  the original's own sprite store at `ds:0x1FE8:0x780` (image `0x06085`) from
  inside a running level. All four - two directions, two slices, thirteen rows
  each - are byte-identical to `MINE_MASK` in `game.c`.
- **not the pre-shifting.** The port keeps only shift 0 and applies it at
  pixel granularity; the original picks one of eight pre-shifted variants by
  `x & 7` (image `0x060E6`). That is only equivalent if nothing falls off the
  end of the three bytes, so it was checked: shift *k* equals shift 0 shifted
  right by *k* for all 91 combinations. Nothing is lost.
- **not the position.** A misplaced mask differs along a whole edge. This is
  one pixel on one row, with every other pixel of the tunnel identical.
- **not a stale pixel from the original's incremental display**, which was the
  first guess because the nuke's residue is exactly that. The pixel reads 22
  on both sides through frame 137 and then goes to **2** in the original and
  **0** in the port on frame 138, the frame the first slice is cut. A stale
  pixel would have stayed 22.

What the values say, and it is the lead worth following: DAC 22 is attribute
14 (`1110`) and DAC 2 is attribute 2 (`0010`), so the original cleared planes
2 and 3 at that pixel and **left plane 1 standing**, where the port writes a
plain zero. And screen x 159 is the LAST PIXEL OF A BYTE.

The obvious next thing was how image `0x060A8`'s clear drives the write across
the mask's three bytes, and that has now been read as well, at image
`0x06106`: thirteen rows of three bytes, every byte latched with a read of
`es:[di]`, the GC's BIT MASK (index 8) set to the mask byte, the byte stored,
and `di` advanced by 0xCD so each row steps the level bitmap's 0xD0. The
arithmetic in front of it matches the port line for line - including the
`inc di` and `add cx, ax` image `0x060AD` does only when the animation counter
is 2, and the hot spot added at image `0x060BA` and image `0x060C9`. The one
asymmetry is expected: the original subtracts 0x10 from Y only, its level
bitmap's X being the lemming's own where the port's canvas is sixteen left.

So the mask is right, the pre-shifts are right, the position is right and the
write loop is right, and the pixel still differs. It is written down at that
depth so the next attempt starts below all of it rather than repeating any of
it.

### Two cross-hairs in different places is not a residue, it is a harness

All three of the levels being chased now score their play area **exact on
every frame**: level001/3 at 530 of 530, level002/4 and /6 at 1,100 of 1,100.
The last thing between them and that was not the port at all.

`out/park.txt` was one hand-measured `move` that parked the port's pointer
where the original leaves its own. That is right for level002's two sections,
whose guest settles its pointer on frame 1, and wrong for level001 section 3,
whose guest does not move it until frame **42**. Those 42 frames differed by
exactly 56 pixels each - two cross-hairs, 28 apiece - and `compare_screen.py`
excuses 28, not 56, so they scored as differences and had been written down
here as "the opening's cursor, harness-side" for several rounds without anyone
generating a script that matched.

`tools/park_script.py` now writes the whole track out of the reference's own
manifest, one `move` per change. It also removed the fade-in difference at
frame 0 of all three, which had been recorded separately as a palette
artefact: the cursor was in it too.

**And the two harnesses disagreed about where the pointer is.** `park.txt`
used `(mx - 8, my - 8)`; `verify_skills.py`'s `park_script()` uses
`(mx + 2, my - 8)`. Ten pixels apart, for the same job, and both were excused
as "the cursor" wherever they showed. The generator settles on the measured
`-8` in both axes; `verify_skills.py` should be moved onto it.

### A lemming is on screen if a TABLE says so, not if its pixels are

level002/4's last residue was three pixels in the screen's final column, x 319
at y 37, 38 and 44, on 28 frames of 1,100. It is fixed, and the level now
scores **1,099 of 1,100** with only frame 0 - the fade-in - left. Getting
there took two wrong guesses and one reading, and the shape of that is the
point.

**The rule.** Image `0x07070` takes the lemming's `x + hot_x`, wraps a
negative one by the page's `0x160`, takes its BYTE column, and image
`0x07086` refuses to draw the sprite at all unless `ds:0x0EA8[column]` holds
1. Image `0x04113` clears that table and image `0x0412B` marks exactly 41
bytes of it (`cx = 0x29`) starting two byte columns left of the scroll, so the
window is `(scroll >> 3) - 2 .. + 38`.

Because it is keyed on the BYTE column, a sprite is refused the moment its
left edge crosses into the first unmarked byte - not when its pixels leave the
screen. At frame 135 the walker's `x + hot_x` is 871, byte column 108, the
last marked one, and the original draws it; at frame 136 it is 872, column
109, and the original draws none of it. The port drew the visible sliver both
times.

**The two guesses, and why they were guesses.** Both were about the blitter's
own right-edge trim at image `0x09997`, which is real - it abandons a blit
starting at or past `ds:0x66F2` and trims one that crosses it a byte at a time
(image `0x099AC`). Applying it with the limit assumed to be the visible 320
took the level from 1,071 exact to **875**; refusing the last column outright
took it to **1,015**. Then `ds:0x66F2` was READ from a running level -
`tools/probe_clip.py`, 2,109 frames - and it is **352**, the whole page, so
during a level there is no right-edge trim to apply at all and the limit was
innocent from the start.

**And the table was then read back to check the transcription.**
`tools/probe_clip.py` dumps it every frame from inside a level: on level 1,
whose scroll is 640, the marked run is byte columns **78..118, 41 of them** -
which is `(640 >> 3) - 2 = 78` through `+ 40 = 118`, exactly what image
`0x04125` and image `0x0412B` say it should be. The first attempt at that dump
read only 44 bytes, on the assumption that the table was indexed by the
*page's* byte column, and got 44 zeros - a table that looks like nothing fills
it. It is 208 bytes (`0x68` words, image `0x04116`) and indexed by the
*level's*.

Three things worth keeping from it:

- **A byte trim cannot produce a one-column difference.** The residue was one
  column, and both candidates were byte-wise. That mismatch was visible before
  either was tried and was not looked at.
- **Look at the picture.** The counts said "three pixels at the right edge",
  which invites an edge-case explanation. Printing columns 308..319 for rows
  33..49 on two consecutive frames showed the whole sprite present at 311 and
  wholly absent at 312, which is a completely different fact and points
  straight at a per-sprite decision rather than a per-pixel clip.
- **Read the caller.** The clipping was not in the blitter at all. The drawer
  refuses the sprite before the blitter is ever reached.

### A harness setting reported as a reconstruction failure

The nuke run's PANEL scored **0 of 557 frames** and had for as long as the
case has existed. Nothing was wrong with the panel: `verify_skills.py` sets
`LEM_SKILLS=20` for every case it runs, because the eight skill references are
recorded with `record_level.py --skills 20` and both sides must get the same
poke - but the nuke and the pause references are recorded WITHOUT it, so the
port was drawing twenty of everything beside an original showing level 1's own
counts. Giving those two cases the level's own skills takes the nuke's panel
to **540 of 557**.

The play area was exact on 535 the whole time, which is what made this easy to
leave alone: one number looked finished and the other looked like a hard
problem, and they were the same run. A harness that feeds the two sides
different inputs reports the difference as the port's, and there is no way to
tell that from the score - only from asking what the harness set.

### The blitter's right-edge trim is NOT what clips a lemming

level002/4's last residue is three pixels in the screen's final column, x 319
at y 37, 38 and 44, on 28 frames of 1,100 - the port draws them and the
original does not. Image 0x09997 looked like the answer and is not, and the
refutation is worth more than the guess was.

The blitter really does trim: a blit starting at or past `ds:0x66F2` is
abandoned, and one that merely crosses it loses a whole BYTE at a time -
image 0x099AC's `sub dx,8`, looping at image 0x099A1 until the right edge
fits, with a `jge` so a sprite ending exactly ON the edge is trimmed too.
That is where the briefing background's 1,413 unwritten pixels come from.

Assuming the limit was the visible 320 and applying it to every lemming
sprite took level002/4 from **1,071 exact to 875** - it removes far more than
those three pixels, and the frames it broke are ones the original draws. So
either `ds:0x66F2` is not 320 while a level is up, or lemmings do not reach
the blitter by that path. The change is reverted.

What this cost was a round of chasing the wrong thing, and the reason is
worth writing down: the residue was three pixels, which is small enough to
look like an edge case and so invited an edge-case explanation. The right
next step is to READ `ds:0x66F2` out of a running level rather than assume
it, exactly as this file says about fitting - and to find what actually draws
those three pixels, which is still unknown. It is not the objects (a trace of
`level_anim_draw` writes nothing in rows 36..45), not the cursor (suppressing
it changes nothing), and not the terrain (the canvas at 863,37 is zero on
every frame). It IS a lemming sprite: at frame 136 a walker sits at x 880,
which puts its sprite at screen 312..327 with only 312..319 on screen.

And the original draws that sprite - the port and the original AGREE on
columns 312 to 318. Only column 319 differs. **Two clipping rules have now
been tried and both made it worse**, which is the useful part:

- the blitter's byte-wise trim, image 0x09997, against a limit of 320:
  1,071 exact down to **875**
- never writing the last visible column at all: down to **1,015**

A byte trim cannot produce a one-column difference, and a blanket last-column
rule is refuted by every other lemming that reaches 319 and matches. So the
rule is conditional on something not yet identified, and guessing at it has
cost two rounds. So `ds:0x66F2` was READ out of a running level -
`tools/probe_clip.py`, 2,109 frames sampled at the game's own end-of-frame -
and it is **352**, the full page width, on every one of them. Beside it sit
`ds:0x66F0` = 160, the play area's height, and `ds:0x66F4` = 44, its row in
bytes. So during a level the blitter clips against the PAGE and not against
the visible 320 at all, and a sixteen-wide sprite at screen 312 (page 328)
ends at page 344, well inside it: there is no right-edge trim to apply. Both
attempts were wrong for one reason, and one word would have said so before
either of them was written.

That leaves the three pixels genuinely open, but far better fenced: whatever
stops the original drawing there, it is not the blitter's limit.

And looking at the PICTURE rather than the count sharpens it to one line.
The walker responsible is at screen x0 **311** on frame 135 and **312** on
frame 136, and:

- at 311 the original draws it, and the port matches it pixel for pixel -
  columns 318 and 319 across rows 37 to 44, nine columns of sprite visible
- at 312 the original draws **none of it** - columns 308 to 319 are empty in
  every row above the terrain - while the port still draws its three lit
  pixels

312 is exactly a byte boundary (39 x 8 on screen, 41 x 8 in the page), and 311
is not. So the original refuses the whole sprite the moment its left edge
becomes byte-ALIGNED at the last visible byte column, and draws it happily one
pixel earlier when it is not. That is a strange enough rule to be a real one
rather than a guess, and it is a fact about two frames rather than an
inference from a score. The routine to read is the one that turns a sprite's x
into the blit's byte count and shift - not the limit test, which has now been
measured and is innocent.

### ODDTABLE.DAT: half the game's plates play under another name

The port was missing a whole data file, and the panel is how it showed.

`level001.dat --level 3` scored its PANEL 0 of 530 frames. Reading the digits
off both screens at the columns the ink occupies - button k at 5 + 16k:

```
  original: 99 99 20 20 20 20 20 20 20 20
  port:     99 99 02 ?? 05 ?0 20 02 02 02
```

The port drew the level header's own counts - 2, 0, 5, 10, 20, 2, 2, 2, which
is what the bytes at 0x08 say - and the original drew **twenty, eight times**.
Not a drawing fault: the two sides disagreed about what the numbers WERE.

**Eighty levels, a hundred and twenty plates.** A level reached from two
ratings has to differ somehow, and image `0x019C9` is where it does: it tests
**bit 0 of `n - 1`**, and when it is set image `0x019DC` loads
`oddtable.dat` - 4,480 bytes, eighty entries of fifty-six - and image
`0x019F5` copies **twenty-four bytes** of entry `(n - 1) >> 1` straight over
the level record at `ds:0x0FCB`. Those twenty-four are exactly the release
rate, the three counts and all eight skills. Image `0x019FE` copies the
entry's other thirty-two - its NAME - to `ds:0x17AB`.

The guest's own memory settles it. `tools/record_level.py` now carries the
record at `ds:0x0FCB`, and for this plate it reads rate 99, 80 lemmings,
rescue **72** where the file says 80, and `0x14` in all eight skill words -
which is entry 10 of `oddtable.dat` to the byte, **name and all**: the plate
is not "I have a cunning plan" but **"As long as you try your best"**.

Confirmed on four levels rather than one: n = 22 and n = 42 have the bit and
were overridden; n = 39 and n = 43 do not and kept their own numbers.

    panel, level001.dat --level 3:   0 of 530  ->  529 of 530

`lemdev --odd N` names the entry, because `--file` names a file and a section
and cannot know which PLATE it is; the original decides from the level index,
which `--file` does not have.

### Open: a sprite at the very last screen column

`level002.dat --level 4` differs on 28 of its 1,100 frames by exactly THREE
pixels, always the same three: (319, 37), (319, 38) and (319, 44), where the
original holds 0 and the port holds 2, 2 and 3.

They are the only on-screen part of a lemming. At frame 136 lemming 1 is at
record x 880 with hot_x -8 and the view at 544, so its sprite starts at screen
312 and spans 312..327 - and only its column 7 carries ink in the visible
part. The port draws that column; the original draws **nothing at all** in
312..319 for those rows.

So the original drops a sprite that reaches the right edge where the port
draws the sliver of it that fits. What has NOT been established is the rule:
the original's blit works in a 352-wide page (image 0x07098 adds 0x160 to a
negative x), so nothing about 320 forces a clip there, and scoring its ink
column by column does not separate the sprite from the terrain underneath.

Left alone deliberately. Three pixels on 28 frames is a poor reason to guess
at a clip, and a wrong guess would cost the 5,099 the first level scores.

### The minimap's dots go UNDER the viewport's frame

`level002.dat --level 4` scored its panel 891 of 1,100, and the whole of the
difference was ONE PIXEL: at frame 186 the original holds 3 at (266, 183) -
the colour of the viewport rectangle's own left edge, which runs down that
column on every row - and the port held 2, the dot's colour, on every frame a
lemming passed that cell.

The port drew the frame and then the dots; the original draws the dots and
then the frame over them. Swapping two calls took the panel to **1,099 of
1,100**.

Worth keeping as a shape: a difference of one pixel on two hundred frames
looks like nothing and is a whole missing ordering. The way to see it was to
print the neighbourhood rather than the pixel - the column of 3s above and
below said at once that something was drawn THROUGH.

### Sprites: where a group lives, and how to find one

`tools/find_sprite.py` scores a candidate offset against the ORIGINAL's own
pixels on a recorded frame, at the position the record says the lemming is.
Three things in it are load-bearing and every one was learned by getting it
wrong:

- **It scores rather than demands.** The input script parks the pointer on the
  lemming it clicked, so the cross-hair sits over the sprite and about thirty
  pixels can never agree. Demanding a match rejects the right answer.
- **It slides nothing.** The record says exactly where the sprite is. Letting
  a candidate slide lets a wrong offset find a flattering spot: the digger
  check first came back with 11982 instead of 720 that way.
- **It maps the attribute through `v < 8 ? v : v + 8`** before comparing,
  because that is what the capture holds. Without it no sprite using the upper
  eight colours can ever be found - and it fails SILENTLY, since the digger it
  is checked against is three planes and passes either way.

`--check` runs the digger first, whose offset is known, so a search that
cannot find an answer it has been given says so.

**What is in the file, in order.** The basher's 64 frames of 60 bytes end
exactly where the miner's 48 of 78 begin, and those end exactly on the
faller's base, which was already known:

| | file | frame | frames |
| --- | --- | --- | --- |
| digger | 720 | 84 | 16 |
| builder | 4240 | 78 | 16 |
| basher | **6736** | 60 | 64 |
| miner | **10576** | 78 | 48 |
| faller | 14320 | 40 | |

64 and 48 rather than 32 and 24 because image 0x070AB adds the STATE to the
frame index when the lemming faces left, and the basher's state is `0x20`
with the miner's `0x18`. So each group is that many frames facing right
followed by the same number facing left, and the fact that the three regions
ABUT is what makes the frame counts evidence and not only the bases.

**`[si+0x0c]` is a PARAGRAPH offset into the pre-shifted store**, and the
store is laid out in the same order:

| | `[si+0x0c]` | store bytes | store frame `[si+0x10]` |
| --- | --- | --- | --- |
| digger | `0x032A` | 12,960 | 112 |
| basher | `0x07CA` | 31,904 | 80 |
| miner | `0x090A` | 37,024 | 104 |
| faller | `0x0A42` | 42,016 | 90 |

31,904 + 64 * 80 = 37,024 and 37,024 + 48 * 104 = 42,016, both exact. A store
frame is the file frame plus a quarter - three ink planes and a mask - so
across those three groups `file = 0.75 * store - 17192` holds to the byte.

**It does NOT hold globally**, and that is the trap: the same line puts the
digger at -7,472 when its file base is 720. The store has other things in it
between groups, so the relation is only good for a run of adjacent groups and
cannot be extrapolated to a group far away. The bomber's cloud at
`[si+0x0c] = 0x180B` is 98,480 store bytes in, and reading 56,668 off that
line is a guess, not a derivation.

### Level 2, where lemmings fall and die

`tools/record_level.py --level N` pokes `ds:0x1F6B` before the briefing reads
it, because `tools/play.py` reaches Fun 1 and nothing else. The second level
is worth having for its own sake: **nothing on "Just dig!" falls far enough
for image `0x021BA` to even ask whether a lemming is a floater**, so the
splat, the fatal fall and the floater's own behaviour cannot be reached from
the first level at all.

| level 2, 482 shared frames | |
| --- | --- |
| every lemming's record | **0 differ** |
| what it exercised | 1,610 lemming-frames of SPLAT, 360 of falling, 1,450 dead |

That is a real positive case rather than agreement in a state nothing
reached: image `0x02250`'s fall counter over `0x3C` sends a lemming to state
1, and image `0x022DF` counts sixteen frames before `[si+0x26]` ends it.

The floater's number on the first level is weaker than it looks: image `0x021BA` wants the fall
counter past `0x10` before image `0x021C0` even asks whether `ds:0x25` is set,
and nothing on this level falls that far. What is verified is that ASSIGNING
it changes nothing else. The climber's ten are its own sprite, which is not
found - see `HOIST_SPR_BASE` in game.c.

The frames that are not exact are all 28 pixels of CURSOR, where a stray
`MOUSEMOTION` from the dummy video driver moves the pointer for a single
frame - the instrument, not the port.

The one frame is 31, where a stray `MOUSEMOTION` from the dummy video driver
moves the pointer for a single frame - the instrument, not the port.

What it took, beyond the assignment at image `0x032BD` and the handler at
image `0x02693`:

- **the walker's guard.** The port ran image `0x2047` only when the state
  byte read 9; image `0x01F61` looks at the flag word and nothing else.
- **the shrug**, image `0x02DB5`, which is dispatched from the LOOP HEAD -
  image `0x01F54` tests bit 7 of `[si+0x24]` before the flag word is read at
  all. Eight frames animating in place with the flags reading zero.
- **the brick**, image `0x05F0F`: six pixels wide into plane 3 alone, so it
  adds solidity and leaves the other three planes as they were.
- **two sprites**, both measured against the original's own pixels - the
  builder's 16x13x3 at 4240 and the shrug's 16x10x2 at 19440.

### The results screen, read at last

Its LAYOUT was the long-standing gap and it is now readable end to end.

**It is mode 0x10, 640x350** - not the level's 0x0D, which is why a recorder
hooked to the level's own end-of-frame at image `0x018F` never sees it and
stops dead at the fade.

**Every string carries its own position.** Image `0x04D4D` walks a chain of
records: one byte of column plus one, one of row, one of style, then the text,
with `0x0D` ending a line and a zero column ending the chain. So the layout is
data, not code:

| | |
| --- | --- |
| `ds:0x4C19` | col 12, row 1 - "Your time is up!" |
| `ds:0x4C2E` | col 6, row 1 - "All lemmings accounted for." |
| `ds:0x4C4E` | col 12, rows 3 and 4 - "You rescued" and "You needed" |
| `ds:0x4D3F` | col 4 row 14 and col 12 row 15 - the access code's two lines |

Image `0x04703` picks between the first two by whether `ds:0x20A7` and
`ds:0x20A9` read "0:00" - the clock, not a flag.

**The font is MAIN.DAT section 4 at offset 0x69B0**, 94 glyphs of 96 bytes -
16x16 in three planes - and all 9,024 bytes match what the guest draws from.
Image `0x04D78` puts a character at `(col * 16, row * 16)`, so the screen is a
40 by 21 grid. It lives in a loaded resource rather than the image, which is
why it was dumped from a run (`tools/probe_results.py`) rather than read out
of DGROUP, where those bytes are still zero.

**What the buttons do**, image `0x047A9` - and the loop polls `ds:0x14` with
no retrace wait at all, which is its own trap for a probe:

| | |
| --- | --- |
| left | `jmp 0x4551` - the NEXT level's briefing, the index having already advanced at image `0x0472E` |
| right | `jmp 0x4401` - the main menu |

**The background is a TILE**, image `0x07E76`, reached through image
`0x05017` - the call at image `0x046FD`. Two across and four down, at
y = 0, 104, 208 and 312, each 320x104 in two planes from offset 0 of a loaded
resource; that resource is **MAIN.DAT section 3**, measured the way the font
was.

And the right-hand tile loses its last byte. Image `0x08846` takes the
clipping path because image `0x07E99` passes `bp = 0x0C`, and image `0x099A1`
compares with `jl` - so a blit whose right edge lands EXACTLY on the limit
still has a byte taken off it. The right tile starts at 320, is 320 wide, and
never writes columns 632..639. That is 1,413 pixels, and it is the last thing
between this screen and an exact match.

| the results screen, 640x350 | **0 differing of 224,000 — 100.00%** |

Everything on it: the tiled background, its clip, the sixteen colours, the
16x16 font, all six text records in their own columns and rows, the verdict
chosen from image `0x04955`'s table, the percentages with the per cent sign
image `0x04B63` moves with them, and the access code.

## The results screen, mapped but not written

Both end conditions at image `0x001C8` jump to image `0x02B3`, which sets
`ds:0x4F` and `ds:0x4E` bit 1 and returns to the frame loop; the teardown is
image `0x02CD`. Neither decides win or lose - that is the saved count against
the level's requirement, and it belongs to this screen.

Its text is all in one run, and reading it says a good deal about the screen's
shape:

| | |
| --- | --- |
| `ds:0x4C1C` | "Your time is up!" — the clock ran out |
| `ds:0x4C35` | "All lemmings accounted for." — everyone released, none active |
| `ds:0x4C55` | "You rescued     " |
| `ds:0x4C69` | "You needed      " |
| `ds:0x517D` | the verdicts, two lines each, up to a "Congratulations!" block for finishing Mayhem |
| `ds:0x4C88`.. | "Press right mouse button for menu", "Press button 2 for menu", "Press Return for menu" |

**Three variants of every prompt** — mouse, joystick, keyboard — which is what
the input chooser screen is choosing between, and a thing the port's own
input screen matches to the pixel without yet acting on.

There is also "Your Access Code for Level     is ABCDEFGHIJ", so the screen
generates the code the level-code screen accepts. `ds:0x552A` and `ds:0x5539`
hold what look like the alphabets it builds them from.

**What is not known**: which routine composes it - none of these strings has a
literal reference, so they are reached through a table - and how the verdict
is chosen from the rescued percentage against the level's requirement.
Finding the composer is the same caller-scan that found image 0x04401 for the
menu.

## The first skill: the digger

Nothing in a recorded run ever assigns one, so none of the fourteen handlers
the dispatch at image `0x01F5E` can reach had ever executed. `tools/probe_digger.py`
drives the original into using one - it holds the panel's digger button, then
clicks a lemming - so transcribing it was differential rather than a guess.

- **the assignment**, image `0x0320D`: spends one from `ds:0x7F`, clears
  `0xF57F` and sets bit `0x10`, parks the animation on `0x11`, drops the hot
  spot two rows
- **the behaviour**, image `0x023F3`: sixteen frames, digging on the two where
  the counter comes round to 0 and to 8
- **the first frame**, image `0x0250C`: cuts THREE rows - `y-2`, `y-1` and the
  one it stands on - before the ordinary path takes over
- **the span**, image `0x0242D`: nine pixels, `x-4` to `x+4`. `0x0FF8`'s value
  bits 3..11 are PIXELS 4..12, because the leftmost pixel is the most
  significant bit
- **the sprite**, `main.dat` section 0 offset 720: 16 wide, 14 rows, three
  colour planes, 84 bytes - immediately after the walker's eighteen frames of
  forty

**Three of its bugs were invisible to every picture comparison here**, and to
a comparison of the lemming's own state as well: the span one column left, the
first frame cutting one row instead of three, and the shaft starting a row
low. None of them changes `y` or the animation while the digger is inside
solid ground. What caught them was comparing the TERRAIN, where they differ by
34, 2 and 14 pixels - and two of the three were found by a person playing
before the check existed.

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

- **The level's phase machine is transcribed** - image 0x0031E, called from
  the play loop's first line at image 0x019D. `ds:0x4E` carries three bits
  and they are tested in this order:

  | bit | branch | what it is |
  | --- | --- | --- |
  | 0 | image 0x0032C | the **fade in**, which runs to completion inside one call - a retrace and a DAC upload to a pass |
  | 1 | image 0x0034D | the **fade out**, armed by image 0x002B3 when the level ends |
  | 2 | image 0x00361 | the timed **cues**, on `ds:0x52` |

  | play-loop frame (`ds:0x52`) | what happens |
  | --- | --- |
  | 15 (`0x0F`) | a sound |
  | **35 (`0x23`)** | **image 0x394 sets `ds:0x55` to 0xFF - the trapdoor opens** |
  | 55 (`0x37`) | image 0x3AD, the music |
  | past 55 | bit 2 of `ds:0x4E` clears; the opening is over |

  **What arms bit 2 is image 0x003C1's own first instruction, `or al, 4`** -
  the fade in, every pass of it. This was recorded here and in `game.c` as
  not found, and the port started `ds:0x4E` at 4 rather than image 0x00431's
  1 to compensate; it reached the right state and skipped the fade to get
  there. It also explains what the probe saw and could not account for,
  `ds:0x4E` at 5 and then 4.

  **Neither fade is a screen either side of the loop.** Image 0x002B3 ends a
  level by setting `ds:0x4F`, ORing bit 1 into `ds:0x4E` and jumping *back*
  to image 0x00105; image 0x001C8 sends every later pass to image 0x002C3,
  which goes round again while bit 1 is set. The loop keeps composing and
  drawing, which is what the fade is a fade *of*.

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

- **THE PLAYTHROUGH MATCHES, and every remaining pixel is attributed.** A
  driven level - "Just dig!", the digger assigned, 400 frames - against the
  original:

  | | |
  | --- | --- |
  | play area | exact on **295 of 399**; whole-run residue **131 pixels** |
  | panel | exact on **396 of 399** |
  | the data segment | **41 of 52 fields** on every frame |
  | the lemming array | **every named field of every record on every frame** |

  and the 131 pixels are two known things, neither of them a fault in the
  reconstruction:

  - **103 of them** are one pixel a frame, and **55 of 55 measured sit inside
    a lemming's 24x16 save block, none outside**. That is image 0x07178's
    save-and-restore, which is UNMASKED and 24 columns wide where the blit is
    16 - so the original's page carries three columns of history either side of
    every lemming, and a pixel there is neither terrain nor any current sprite.
    This file already reached that conclusion for a different residue and said
    the port, composing from the master each frame, "was right the whole time".
    It closes "one pixel of the miner is still unexplained" above: the pixel is
    the original's incremental display, not the miner.
  - **28 of them** are the cursor on frame 34 alone. The original draws it at
    game y 100 while ds:0x12 reads 198 - a stray MOUSEMOTION from the dummy
    video driver, the artefact this file already records at frame 40 of the
    5,100-frame run. The instrument, not the port.

  The segment's remainder is `skill_div`, `rate_div` and `selected` differing
  on the one or two frames before they settle from their initial values, and
  `mouse_x`/`buttons`, which VARY BETWEEN RUNS - 1 frame in one, 18 in the next
  - and are the guest's input delivery.

  So what is left is a start-up phase of one or two frames in three counters.
  Everything else a played level does, this port does identically.


- **A PLAYED level is pixel-exact apart from one known pixel and the cursor.**
  `compare_play.py` had never once been exact and reported a 294-pixel residue;
  it was comparing the wrong frames. `record_level.py` captures the screen at
  image `0x018F`, after image `0x00105`'s flip, so the picture saved on pass N
  was composed by pass N-1, while the port's sink writes the page it has just
  composed. `compare_run.py` carried that offset and `compare_screen.py`, which
  compare_play drives, did not.

  Corrected, over 399 frames of "Just dig!" with the digger assigned:

  | | |
  | --- | --- |
  | play area | exact on **295**, worst 28 |
  | panel | exact on **396**, worst 43 |

  and the whole run's residue is **131 pixels**, all accounted for:

  | | |
  | --- | --- |
  | the cursor, on one frame | 28 |
  | the miner's pixel at canvas (799, 81), one per frame on 103 frames | 103 |
  | everything else | **0** |

  The second is the open item this file already describes at length - image
  `0x06117` and image `0x06126`, six wrong answers deep. So it is the only
  thing between a played level and pixel-exactness, and it was already known.

  WHAT THIS COST: that misalignment is what made the lemmings look wrong, which
  sent a whole iteration after a frame-index rule in `lemming_draw` that does
  not exist - using a sweep that was ITSELF misaligned, so two wrong alignments
  stacked and each produced a clean, consistent, 100%-confident answer. Six
  alignment faults have now been found in this instrument. Before reading any
  cross-instrument number here, prove the two sides describe the same moment.


- **The original has ONE loop and every screen JUMPS into it - the port has
  four.** Image `0x000FB` is the loop; image `0x0454C` ends `jmp 0x000FB`
  rather than calling it, and image `0x0469F` leaves a briefing with `pop ax /
  jmp 0x4401` - discarding the return address to get to the menu. That is not
  an unusual way to write it for the period: a screen is a place the program
  GOES, and the machine has one stack and one owner.

  The port has `level_loop` (image `0x000FB`), `menu_screen_wait` (image
  `0x04660`), `menu_loop` (image `0x0445B`) and `level_results_wait` (image
  `0x047A9`) as four separate C functions that return to their callers. Each
  is a faithful transcription of the loop it names, so nothing here is WRONG -
  but the relation between them is the port's arrangement, not the original's,
  and this file already records what that costs: a sequence written out twice
  is a sequence that will differ, and the frame order lived in two places until
  `level_frame_compose` collected it.

  Worth measuring before doing anything: how much of the four is actually
  common. They now all poll the same two words - `ds:0x14` through
  `mouse_buttons` and `ds:0x1FC7` through `key_held` - and all present and pace
  the same way, so what differs may be only the tick and the exit test. If it
  is, one loop taking those two is closer to the original AND smaller.

  What would settle whether it is worth it is reading how image `0x000FB`'s
  head serves the menu, which nobody here has done: the loop's first lines
  swap pages and wait five ticks, and the menu waits ONE retrace a pass (image
  `0x0445B` calls image `0x01550`). Two rates out of one loop is either a
  branch this port has not found or evidence that the menu really is a
  different loop - and this file's own rule says to go and read the routine
  rather than fit an arrangement that looks tidy.

- **The screen is never blanked across a DAC write, and it shows.** Image
  `0x054C6` and image `0x054B6` set and clear Sequencer register 1 bit 5 -
  the screen-off bit - around the palette write, gated on `ds:0x200C`, which
  image `0x01200` sets for PC compatibles. The port writes the palette with
  the screen live.

  A player sees it as snow in the LEVEL BRIEFING and nowhere else, which fits:
  the menu programs the DAC once on entry, the level's writes are inside the
  per-frame swap, and the briefing is the screen that reprograms it mid-life.
  Cosmetic, but it is a thing the original does and this port does not.

- **FIXED: the compat panel is MAIN.DAT section 6.** A player reported orange
  status text and orange skill buttons under `--pc compat`. Image `0x05976`
  runs only when `ds:0x200E` is set and unpacks the panel from another
  main.dat section through the same image `0x057D1` the port's `panel_frame`
  is transcribed from. `main.dat` has seven sections; the game uses 0, 2, 3
  and 4, and section 6 is 8,224 bytes - which is image `0x059A6`'s 0x1900 plus
  image `0x059B4`'s 0x720 exactly.

  Measured both ways, against recordings of the original on Fun 1 frame 39:

  | attribute | high orig/port | compat orig/port |
  | --- | --- | --- |
  | 4, the gold | 116 / 116 | **1442 / 1442** |
  | 6, grey | 233 / 233 | 884 / 884 |
  | 8..15 | 2680 / 2680 | 0 / 0 |

  **AND THE STATUS TEXT ABOVE IT, which the first fix missed.** Image
  `0x059B4` copies 1,824 bytes to offset `0xA0` - and `0xA0 + 0x720 = 1984`,
  which is the FONT's whole size: image `0x0593C` copies `0x3E0` words of it
  from section 2's own `0x1900`. So section 6 supplies the artwork from offset
  0 AND the font's glyphs from `0x1900`, and the `0xA0` table in front of them
  stays section 2's. Section 6 is 6,400 + 1,824 where section 2 is 6,400 +
  1,984: the compat font carries no table because it borrows one.

  The port's font glyphs live at `PANEL_FONT_OFF + 0xA0` of the section, not
  at `0xA0` - putting them at `0xA0`, which is the original's offset in its own
  prepared buffer, matched nothing.

  **Verified: rows 160..199 - the whole panel, status text included - 0
  differing pixels of 12,800, in BOTH machine types, across 35 frames of Fun 1
  and 25 of Taxing 2 (a different graphics set).**

  **And the machine type affects ONLY the level's panel.** Driven in compat,
  the menu is 0 of 224,000 and the briefing 0 of 174,080 - they draw their
  text from FONT_SECTION and need nothing. `prove.sh` is 100.00% with 0
  differing, and the play area's 56 is the two cross-hairs this file already
  records.

- ~~**THE PANEL IS DRAWN WITH DIFFERENT ATTRIBUTES PER MACHINE**~~ - true, and
  this is why. Two recordings of the original on the same level, Fun 1, frame
  39 - `out/panel_ref` at machine 2 and `out/panel_ref_compat` at machine 1 -
  counted over the panel's rows 176..199:

  | DAC | high | compat |
  | --- | --- | --- |
  | 4 gold | 116 | **1442** |
  | 5 red | 268 | 452 |
  | 6 grey | 233 | 884 |
  | 0 | 2964 | 3438 |
  | 18, 19, 20, 22, 23 | 2680 total | **0** |

  In compat the panel uses ONLY DAC 0..7. In high it uses 16..23 as well -
  attributes 8..15 through the ATC - and those are the entries the mid-screen
  swap replaces. The 2,680 pixels that were 18/19/20/22/23 reappear as 4, 5, 6
  and 0.

  So the port's `if (lem_machine == LEM_PC_HIGH)` gate on the PALETTE is
  treating a difference that is in the DRAWING. Compat has one palette, as it
  must, and the panel is drawn to fit it.

  **Where the branch is has not been found.** `ds:0x200C` and `ds:0x200E` are
  not read anywhere between image `0x03AC8` and image `0x03CDE`, so it is not
  in the panel routine. Likeliest is a different source for the panel artwork
  - a second MAIN.DAT section, or a remap as it is blitted.

  `LEM_MACHINE=1` now makes any tool record or probe a machine type other than
  High Performance; `record_level.py` had no way to ask, which is why every
  reference capture here was machine 2 and this went unmeasured for so long.

- ~~**THE SKILL BUTTONS ARE DRAWN WITH THE WRONG ATTRIBUTE**~~ - no; the art
  decode is right, and the counts for attributes 1, 4, 5, 6 and 7 match the
  original's high capture exactly.
  The original's button background is attribute 4, `b2b200`, gold. The port's
  is attribute 3, `f3d3d3`, pale pink. Attributes 0..7 are the FIXED ones -
  image `0x0C048`, level-independent - so this is wrong in high as well and
  was only noticed in compat, where the eye is already on the panel.

  **The palette is not involved and three diagnoses said it was.** A player's
  DOSBox capture in machine type 1 samples to the port's own sixteen once the
  scaler's checkerboard is averaged out: `bfd69b`/`ff9bd6` average to
  attribute 3, `ac7d00`/`7dac00` to attribute 4, `557655`/`765576` to
  attribute 6, and the greens to attribute 2. Compat uses the same DAC the
  port has, which is exactly why capturing it three times kept answering "the
  same as the port already has".

  Next: compare the port's panel INDICES against the original's composed
  panel, not its colours. `tools/compare_frame.py` scores the panel
  separately already.

- ~~**COMPAT DRAWS THE PANEL WITH ATTRIBUTES 0..7**~~ - wrong; there is no
  machine branch in the panel drawing at all.
  NOT a missing palette - that was two wrong diagnoses ago. Measured, with a
  player's DOSBox capture in machine type 1 as the reference:

  | | |
  | --- | --- |
  | attributes 0..7, both modes | `000000 4141e3 00b200 f3d3d3 b2b200 f32020 828282 d38220` |
  | attributes 8..15, high | ALTERNATE: terrain, and the panel's `00aa00 55ff55 555555 aa0000 aa5500 00aaaa ff55ff aa00aa` |
  | attributes 8..15, compat | terrain only. No swap, settled, confirmed |

  The DOSBox panel's colours are green, gold, pale pink, red and grey - and
  every one of them is in the FIRST row: `00b200`, `b2b200`, `f3d3d3`,
  `f32020`, `828282`. They are not in either half of the second. So compat
  reaches them by drawing the panel with attributes 0..7, and the DAC is the
  same in both modes, which is exactly why three rounds of capturing the
  palette kept answering "the same as the port already has".

  **What is not found yet** is the branch that does the remapping. The
  attribute registers are identical in both modes -
  `00 01 02 03 04 05 06 07 10 11 12 13 14 15 16 17` - so it is not the ATC.
  Image `0x05399` returns early when `ds:0x200E` is set and image `0x053A1`
  assembles a palette into `ds:0x25B8`, which is the region to read next.

- ~~**THE COMPAT PALETTE IS NOT TRANSCRIBED**~~ - superseded by the above. A player reported the status text
  and skill buttons coming out orange under `--pc compat`.

  **The split is a machine capability.** A High Performance PC can reprogram
  the DAC part-way down the frame, so the play area keeps the terrain's browns
  and the panel gets greens and greys; a PC compatible cannot - image `0x01200`
  sets `ds:0x200C`, which turns the screen off across a palette write - so the
  whole frame lives on ONE palette, and that palette has to serve the terrain
  AND show the skills UI in green.

  Measured so far, with `tools/probe_dac.py --machine 1` against `--machine 2`
  as the control:

  | | |
  | --- | --- |
  | lower eight, both | `000000 4141e3 00b200 f3d3d3 b2b200 f32020 828282 d38220` - identical, and it already holds the green and the grey |
  | upper eight, high | alternates: terrain `d38220 c35110 922010 610010 414151 616171 719200 206120` and panel `00aa00 55ff55 555555 aa0000 aa5500 00aaaa ff55ff aa00aa` |
  | upper eight, compat | **not yet observed settled** |

  The compat run has not reached a settled level frame in three attempts at
  120s, 300s and 700s: its fade blocks one step per retrace and loops (image
  `0x01200`'s `ds:0x200E`), so it is far slower to get there. The control DID
  fire in the same budget, so this is the instrument's reach and not an
  absence. A 1800s run is going.

  **Two wrong readings so far, both recorded so they are not repeated.**
  CLAUDE.md said `--pc high|compat` selects whether the swap happens - right
  about the gate, silent about the palette. Then I read image `0x055B7`'s
  `ds:0x1F7C` as the decider and made the swap unconditional, which mistook
  the palette FORMAT (EGA attributes against VGA triples, set by the
  command-line adapter letter) for the thing that decides the split.

- **SETTLED: the F-keys work, Z and X do not, and it is WHERE THEY ARE CALLED
  FROM.** Both read `ds:0x1FD0`, which image `0x01707` sets to the scancode on
  a press and image `0x01705` clears on the release.

  | reader | called from | sees |
  | --- | --- | --- |
  | image `0x03E6F` the F-keys | image `0x01874`, INSIDE THE KEYBOARD ISR | the key just stored |
  | image `0x04085` Z and X | image `0x001A0`, the frame loop | zero - image `0x00194` cleared it three instructions before |

  So the port keeps its F-keys and leaves Z and X transcribed but unwired.
  Both were confirmed in DOSBox by a player.

- **`level_key_button` dispatches on an SDL keycode, not the scancode the
  original reads.** Image `0x03E6F` reads the byte the keyboard handler left
  at `ds:0x1FD0` - image `0x016D4` stores it and clears it on the key-up - and
  the table is known:

  | scan | | |
  | --- | --- | --- |
  | 0x3B | F1 | image `0x04068`, the release rate down |
  | 0x3C | F2 | image `0x0403D`, the release rate up |
  | 0x3D..0x44 | F3..F10 | image `0x03F5C`, buttons 2..9 |
  | 0x4E | keypad + | image `0x04018`, pause |
  | 0x57 | F11 | image `0x04018` |
  | 0x53 | Del | image `0x03FEF`, the nuke |

  The port switches on `SDLK_*` instead, so the mapping is written twice - once
  in `scancode_of` and once here - and the two can disagree without anything
  saying so. Keying it on the scancode needs `scancode_of` extended with
  F5..F12, Delete and keypad +, which it does not carry: it has F1..F4 and the
  menu's eleven, and derives the letters by inverting the game's own
  `MENU_XLAT`. That inversion is the pattern to follow, and a wrong entry
  shows up as a key that does nothing rather than one that means something
  else - so this is worth doing carefully rather than quickly.

- **THE LEVEL'S Z AND X KEYS SELECT NOTHING.** Reading image `0x000FB`, the
  main loop, turned up a per-frame call the port has no counterpart for:
  image `0x04085` reads the pending scan code at `ds:0x1FD0` and steps the
  selected skill at `ds:0x82` - image `0x040BC` down, wrapping 1 to 9, image
  `0x040D4` up, wrapping 10 to 2 - both gated on `ds:0x84`, the same repeat
  divider the release-rate buttons use, so a held key steps one frame in
  three.

  The port sets `ds:0x82` from `level_skill_click` (image `0x03F66`) alone, so
  the panel works and the keyboard does not. Transcribing it needs `ds:0x84`
  as well, which the port has only in comments - image `0x03B8C` catches it
  at 2 and image `0x03B9C` wraps it.

  The same routine also does the EDGE SCROLL: `ds:0x10` at 8 goes to image
  `0x0414F` and at 0x148 to image `0x040E5`, with `ds:0x12 + 8 < 0xC0`
  bounding it to the play area.

- **Image `0x03150` is called TWICE a frame** - image `0x001AD` and image
  `0x001C5`, the same routine either side of the draw. The port calls its
  equivalent once. Whether that second call can do anything the first did not
  is unread; it is recorded because a loop read once should not have to be
  read again to find it.

- **THE FOUR VGASPEC LEVELS DO NOT LOAD, and until now they loaded the WRONG
  FILE without saying so.** Transcribing image `0x0197D`'s tail
  (`level_graphics_names`, image `0x01A00..0x01A7A`) turned up a branch nobody
  had read: image `0x01A26` tests the level header's +0x1C, and when it is
  non-zero the graphics come from `vgaspecN.dat` rather than `vgagrN.dat` -
  with `add al, 0x2F` at image `0x01A5E`, because the extended set is 1-based.

  The port built `vgagr%u.dat` for every level, so all four loaded graphics
  set 0 and rendered the wrong pieces. Scanning all eighty sections for a
  non-zero +0x1C finds exactly four, which is the same four `music_choose`'s
  by-number table has entries for:

  | | | |
  | --- | --- | --- |
  | `level000.dat` §1 | ext 1 | `What an AWESOME level` |
  | `level000.dat` §2 | ext 2 | `MENACING !!` |
  | `level000.dat` §4 | ext 3 | `A BeastII of a level` |
  | `level001.dat` §3 | ext 4 | `A Beast of a level` |

  The NAME is right now and `level_show` refuses these four with
  `VGASPEC level - image 0x03742 not transcribed` rather than loading the
  wrong graphics quietly. **What is owed is image `0x03742`**, the terrain
  build that lays one whole picture down instead of composing pieces - image
  `0x01A5A` reads section 0 alone, where the ordinary path reads two.

  Refusing is deliberate and it is a REGRESSION IN WHAT RUNS, traded for a
  gain in what is true: this repository's own rule is that a plausible wrong
  answer costs more than a loud failure, and nothing here could see the fault
  because no check ever opened one of these four.

- **`verify_objects.py` says nothing about image `0x09709`, and there is a
  level that would fix it.** Its own output reports the gap honestly -
  `only-on-terrain NOT PRESENT - this run says nothing about image 0x09709` -
  because it always drives Fun 1, and "Just dig!" carries no object with the
  only-on-terrain bit.

  **66 objects across the game do carry it** (`e[6] & 0x40`), and
  `level000.dat` section 3 has six, all object id 3. `LEVEL_ORDER` reaches it
  at **`ds:0x1F6B` index 49, Tricky 20**, an even entry so no ODDTABLE
  substitution complicates the record.

  **Attempted, reverted, and then the attribution was WRONG - which is the
  useful part.** Adding a `--level` that pokes `ds:0x1F6B` at image `0x04401`
  (the menu, the moment `record_level.py` uses) came with three runs reporting
  `NEVER REACHED image 0x07EB2`, against three passes from the committed
  tools. Three-nil looked decisive and it was a coincidence.

  Settled by isolating the change instead of counting runs: a scratchpad copy
  of `probe_pvsrc.py` carrying the same guarded block, driven through
  `play.py` directly with `LEM_PVSRC_LEVEL` unset, **installs and captures its
  hit normally**. The guard is inert, as it reads.

  So `verify_objects.py` is FLAKY, and this file already says why - `play.py`
  presses F1 with `--start-tries 3` and a script tuned on one run misses on
  the next. Six runs is not enough to tell a flaky check from a broken one,
  and the honest reading of 3-and-3 was "no signal", not "the edit did it".

  Why it is not the option-add it appears to be: image `0x07EB2` is reached
  BEFORE the title screen (`probe_pvsrc.py`'s header says so, and says why
  breaking there with the control socket fails), so the packer captures
  whichever level `ds:0x1F6B` names on the way IN, and a hook installed for a
  later address is already in the wrong place.

  **Retried properly, and the poke does not take.** Re-applied with the
  flakiness understood, the run reported `only-on-terrain NOT PRESENT` - still
  Fun 1 - with no `[pvsrc] level index poked` line, while the whole bitmap
  fell to **194,498 of 253,792, 76.64%**. That pair is the diagnosis: the PORT
  side moved level and the original side did not, so the comparison was of two
  DIFFERENT LEVELS - the mix-up this file already records as scoring 0 of 535
  and reading as a comprehensively broken port.

  Image `0x04401` is the right moment for `record_level.py` and the wrong one
  here: the packer at image `0x07EB2` runs on the way IN, and by the time the
  menu is reached the record it packs is already chosen. Reverted - a verifier
  that can report 76% on a port passing at 100% must not sit in the tree.

  Still untested rather than refuted. The target: 66 objects across the game
  carry `e[6] & 0x40`, `level000.dat` section 3 has six, and `LEVEL_ORDER`
  reaches it at `ds:0x1F6B` index **49**, Tricky 20 - an even entry, so no
  ODDTABLE. What it needs is a poke landing before the packer, and a check
  that BOTH sides took it.


- **Eight verifiers exist that `tools/verify_all.sh` never runs**, and the
  headline table above quotes five numbers that come from them:
  `verify_data.py` (179,618 bytes across 7 sections), `verify_dat.py` (7
  calls, 150,807 bytes), `verify_level.py` (15 fields), `verify_shift.py`
  (200 calls), `verify_preshift.py` (106 calls), plus `verify_adlib.py`
  (10,333 register writes), `verify_code.py` and `verify_digits.py`.

  This is how a figure goes stale, and it did twice in one session.
  `verify_dig.py`'s note said its terrain half could not be trusted, months
  after the read was fixed; `verify_adlib.py`'s usage line still named
  `LEM_OPL_TUNE`, which nothing reads, so following it scores **0 of 54** and
  reads as a broken sound port against a real 10,333 of 10,333. Neither is in
  the sweep, so neither was contradicted by anything.

  **Done: `tools/verify_all.sh --units` runs all eight**, and `--full`
  implies it. They stay off the default because they cost real time - timed,
  `verify_digits` alone is **92 seconds** (and passes, 256 of 256 values
  driven exhaustively), so the eight together are about ten minutes on top of
  a sweep that already takes twenty-five. A flag was the right shape, and the
  number is why rather than an impression of why.

  Each goes through `check_pipe`, so none of them can crash and still pass -
  which is exactly how `verify_triggers.py` stayed dead for months.

  **Two of the eight take an input and are SKIPPED without it**, with the
  reason and the command that records one. `verify_data.py` wants a memory
  dump from `play.py --dump-mem`; run bare it exits 2, and adding it to the
  loop unguarded would have made `check_pipe` fail every sweep - a false
  alarm of my own making, found by running the block rather than reading it.
  `verify_adlib.py` wants a recorded reference pair, and a run scoring 0 of
  54 means the two sides are different tunes, not a broken driver.

  The block was checked in both directions before it went in, by stubbing
  `uv`: every tool succeeding gives `fail=0` with those two SKIPPED, and
  every tool exiting 3 gives `fail=1` with each one named.

  **And the stubs missed the fault the real run found.** They return the exit
  codes you choose; tools return the ones they have. `verify_code.py` returned
  1 for the one code in the shipped `lemmings.cod` that image `0x05042` does
  not reproduce - Mayhem 2's `KNOHGONMHS`, a 1992 typo the ORIGINAL refuses
  too, which that tool prints nine lines explaining - and inside a sweep only
  the status survives, so a check that had just said the discrepancy was not
  the port's reported FAILED. Its exit now follows the two halves that are
  about the PORT, the generated and validated codes, with any shipped-list
  difference reported and counted but not failing; controlled by forcing the
  branch.

  Run end to end with real tools the block gives **`fail=0`** and **all eight
  run**, none skipping:

  | | |
  | --- | --- |
  | `verify_dat` | 7 calls, 0 differed |
  | `verify_level` | 15 fields, 0 differing |
  | `verify_digits` | 256 of 256, driven exhaustively |
  | `verify_code` | 25/25 single-bit, 403/403 many-bit, 127/127 validated; the 1 shipped difference is the 1992 typo the ORIGINAL refuses too |
  | `verify_shift` | 200 calls, 0 differed |
  | `verify_preshift` | 40 calls, 0 differed (and 106 more `--driven`, inside a level) |
  | `verify_data` | 7 sections byte-identical, 11 freed |
  | `verify_adlib` | **10,505 of 10,505 registers and values, 100.00%**; gap 99.99%, worst 1 tick |

  What that cost is worth writing down - the stub test, the per-tool runs and
  the whole-sweep run were three layers, each necessary and none sufficient.
  The stub test proved `PIPESTATUS` survives; the per-tool runs proved every
  tool passes; only the whole-sweep run showed that `verify_code` returned 1
  for a discrepancy it had just explained was not the port's.



- **Image 0x08FF7 is the one blitter mode with no counterpart in the port.**
  It is image 0x0909C's exact complement: image 0x09013 sets the graphics
  controller's set/reset to 0x0F where the erase path sets it to 0, and both
  then set enable-set/reset to `bp` bits 11..8 - so one writes ONE to the
  selected planes and the other writes ZERO. The two routines are identical
  but for that constant, which is what makes the pairing certain rather than
  plausible. Nothing in the port's own composition fills planes with ones, so
  what it is FOR is the question - not how it works.
  It is reached ONLY by the dispatch at image 0x08902: the code map has none
  of the five modes as a routine head, because they are jump targets inside
  the blitter. An earlier note claimed a direct caller at image 0x068FA and
  that was a scratch byte-scan for `E8` matching bytes that are not an
  instruction boundary - image 0x068FB is a `cli`.

  **AND NOTHING ASKS FOR IT.** There are fifteen far calls to 1010:8822. One
  sets an immediate bp with bit 5 clear (image 0x04F0A, bp = 0xF42, an
  erase); the rest compute it, and the only one whose bp comes from data is
  image 0x03981's object pass. Across **all eighty levels, 454 objects**,
  `e[7]` is `0x0F` every time and `e[6]` is one of 0x00, 0x40, 0x80, 0xC0 -
  bit 5 never set. So image 0x08FF7 is unreached in this build, measured over
  the whole game rather than over the levels this repository compares.

  Two things fell out of that scan. An object with `e[6] = 0x80` blits with
  **bp = 0x800E**, which is exactly the value the blit probe caught - the
  arithmetic and the observation agree without either being fitted to the
  other. And the `0x0E` is bits 3 and 2, so **every object blit asks for both
  clips**.

  **And the TERRAIN path cannot express bit 5 at all** - a proof, where the
  object result is a survey. Image `0x07C67` builds a piece's `bp` with
  `shr dx,7 / and dx,0xC0 / or dx,0x0F`, then ORs `0x0F00` for erase and
  `0x8000` for no-overwrite. The `and dx,0xC0` masks bit 5 out before
  anything else runs and nothing later ORs `0x20`, so no terrain piece can
  reach image `0x08FF7` whatever its flags hold. Both data-driven callers are
  now excluded, one empirically and one structurally.

- **`level_object.upside_down` is dead in every shipped level.** The port
  reads it as `e[7] == 0x8F` and `e[7]` is `0x0F` in all 454 objects. Left in
  place - it came from the published format description, and "no level
  exercises it" is not "it is wrong" - but recorded so nobody spends time on
  a rule nothing can reach.

- **The 40-pixel residue on the digger's run was the FALLERS, and it is
  fixed.** It was attributed to the digger for several passes on proximity
  alone; the pixels were five fallers stacked at nearly one position, drawn
  with more ink in the lower rows - 3s where the original had 2s, drifting
  further apart towards the bottom of the sprite, which is neither a plane
  swap nor a shift.

  It was the DIRECTION. Image `0x059BB`'s group 9 is eight frames of 40, not
  four: four facing each way, the same split the climber has at
  `CLIMB_SPR_LEFT`. The port had one base and drew right-facing artwork for
  fallers going both ways. `FALL_SPR_LEFT` is `FALL_SPR_BASE + 4 * 40`
  (commit `ada79f7` - a hash, not an image offset), and the digger's
  recorded run went **exact 659 -> 674 of
  709, worst 40 -> 1**.

  The four states that looked like the same bug are NOT: the builder, the
  floater, the hoist and the shrug each use half their group's frames with no
  left-facing base, and the pattern predicted they were broken too. Measured
  first, they are not - the builder scores 399 of 401 and the floater 401,
  both with a worst frame of 28, which is the cursor. Changing them would have
  moved no number and there would have been nothing to say so. Only the
  climber and the faller need a second base.


  **The MASK is ruled out too, by reading image 0x05B11.** Its second half at
  image 0x05B63 walks down the sprite's planes at a stride of `rows * 3`, ORs
  them together and appends the result - so the mask is "any plane set",
  which is exactly the port's `v != 0`. The transparency rule is confirmed
  from the original, not assumed, and it is not where the extra ink comes
  from.

  **And the states are identical.** The reference's own manifest carries the
  ORIGINAL's records: at frame 298 it has nine lemmings with flags 0x0010 for
  the digger and 0x0004 for five fallers, anim 10 and 2, at screen x 146..148
  where the port has 162..164 - the known sixteen between the guest's scroll
  of 640 and the port's `view_x` of 624. Same states, same animation
  counters, same positions. So nothing about the simulation differs and the
  residue is entirely in how the sprites are COMPOSED.

  What is left is that the digger (14 rows from y = 91) and five fallers (10
  rows from y = 102) OVERLAP at rows 102..104, and the residue spans the
  fallers' whole extent. Everything checkable about each sprite alone has
  been checked; what has not is how two of them interact where they meet.
  **Cause undetermined** - recorded at this level
  of detail rather than guessed at, because the last four attributions of
  these forty pixels were wrong.

- **(superseded) The digger's residue is 40 pixels, and it is not new.** The port has
  drawn the digger since DIG_SPR_BASE was settled; a stale comment beside
  `lem_dig_start` still said "so it is not drawn", a later pass believed it
  and added a second draw path, and measured the two score identically - so
  the claim that this work made the digger visible was wrong and is
  withdrawn. What image 0x059BB's build table adds is EVIDENCE for the
  geometry (three planes of fourteen rows, 84 a frame from base 720, image
  0x05BB0 appending the mask to make 112) which the port had reached by
  rendering candidates and looking. Against its own recorded run the play
  area is exact on **659 frames of 709**, worst **40**: a
  ten-by-ten block in the digger's own colours, the right shape with wrong
  values, bottom rows differing more than the top. That is a different
  ANIMATION FRAME rather than a different geometry - the port uses `a->anim`
  as the index and image 0x023FF's counter runs 0..15, and the mapping
  between them is unread. NOT fitted: trying frame indices until the number
  falls is narrowing without settling.

- **(fixed) A level's sound ran on into the next one, and it was the PORT'S
  BUFFER.**
  Reported from playing: switching level leaves a moment of music and a couple
  of effects before it resets. Image 0x002FE stops the driver when a level
  ends and `level_end_finish` makes that call, so the OPL is keyed off at the
  right instant - but `sound_pump` keeps **200 ms** of already-generated audio
  queued in SDL (`SND_TARGET_MS`), and that is what goes on playing. The
  original has no such buffer: its OPL is real time, so a key-off is silent
  immediately.
  The fix is IO and one line - clear the queued stream on the AH=2 stop in
  `sound_request`, which is the moment the game says *silence* and the only
  moment the port's buffer contradicts it. **DONE**, and the magnitude is
  measured rather than guessed: `reconstruct/tests/screens.c` divides
  `sound_target_bytes()` by a level frame's 2,924 samples and gets **over
  three frames**, which is what "a couple of sounds" sounded like. The test
  fails when `SND_TARGET_MS` is cut to 20, so it is measuring the constant
  rather than passing by construction.
  Only AH=2 clears. That is the one request meaning silence; doing it on an
  effect or a tune change would cut whatever is still sounding, and the
  original does not.

- **Is the builder's warning the right LOUDNESS against the original?** Effect
  0x12, image 0x026D0, fires on animation frame 10 when `[si+0x21]` is three
  or fewer - transcribed, and a player confirmed hearing it. Rendered alone it
  is the loudest effect in the game (peak 23,614, RMS 3,094, against 15,332
  and 2,457 for the release), so nothing in the port attenuates it. What is
  NOT measured is the same moment against DOSBox, the way the music was: that
  needs a scripted run which assigns a builder and lets it down to three
  bricks. Note also that `start_effect` (driver +0x0520) searches channels 8,
  7, 6, 5 and DISCARDS the effect if all four are busy - the original's
  behaviour - so under load it would be intermittent rather than quiet, and
  the two are worth telling apart before changing anything.

  **And the DOSBox half is blocked for a reason worth naming.** DOSBox
  Staging 0.82.2 is installed and has `autotype`, so keystrokes CAN be
  scripted - but a skill is chosen by POINTER only: image `0x03F07` takes the
  button from `(mouse_x - 8) >> 4` and there is no keyboard path to it, and
  assigning then needs a second positioned click on a lemming. `autotype`
  sends keys, not mouse events. So the comparison needs mouse scripting that
  DOSBox does not offer, which is a different piece of work from the capture
  `tools/reference_dosbox_audio.sh` already does well.

  **Half of it is already settled: the discard is transcribed.**
  `adlib.c`'s `start_effect` walks backwards from channel 8 for four
  channels and returns at driver `+0x053C` when none is free, dropping the
  effect exactly as the original drops it - it does not queue, and neither
  does the port. So intermittency under load is MATCHING behaviour and not a
  fault to chase; what is open is the loudness alone, and only against
  DOSBox. Anyone hearing the warning come and go should count the busy
  channels before touching a volume.


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

- **(done) Five usage blocks specified something that cannot work**, each in
  the header of a check the port's claims rest on: `verify_dig.py`'s
  `LEM_DIG_SHAFT` (a flag that never existed), `verify_adlib.py`'s
  `LEM_OPL_TUNE` (nothing reads it, so the two sides record different tunes
  and score 0 of 54) and its missing `--start` (without which the run parks on
  the title and captures six card-detect writes), `verify_data.py`'s
  `--shots 1` (the emulator's whole shot budget, so the run ends after 1.6 s
  and the dump is never written), and **`play.py`'s own first line**, which
  omits the `--` separator that CLAUDE.md already warns about for `--scale`.

  All five were found by accident, one at a time. `tools/check_recipes.py`
  now finds the sixth on purpose - every `--flag` a recipe hands to the tool
  it RUNS, against that tool's own `add_argument` calls, across 99 sources
  including the markdown. It needs no emulator so it runs in the default
  sweep, and `--self-test` plants a bad recipe to prove it can still catch
  one. It checks a flag EXISTS and not that its value works: `--shots 1`
  parses, and so does `--run-seconds 90`.

- **(read) The machine-type menu is mapped AND explained.** Which flags each
  of the four options sets was already written down in
  [docs/machine-types.md](docs/machine-types.md); what those flags CHANGE is
  now read out of the image too, all four of them, and each one matches the
  machine that sets it - which is what says the readings are right rather than
  merely plausible. The paragraph below is the original note, kept because the
  answer corrects part of it. Reading every use of `ds:0x200A/0x200B/0x200C/0x200E`, and confirming
  whether the PS/2 option really patches the retrace path at `ds:0x1FC6`, is
  outstanding work we intend to do — noted here so it is not mistaken for
  finished.

  **`ds:0x1FC6` is not a retrace path: it is the mouse's IRQ VECTOR NUMBER**,
  and that guess is withdrawn. Image `0x0006A` does `mov ah, 0x35` - DOS Get
  Interrupt Vector - then `mov al, [0x1FC6]` and `int 0x21`, saving the old
  handler at `ds:0x1FC2`/`ds:0x1FC4`. Image `0x00040` sets it to **`0x0C`**,
  which is INT 0Ch, **IRQ 4** - the serial port, so a serial mouse. Images
  `0x011E7` and `0x0133F` set it to **`0x74`**, INT 74h, **IRQ 12** - the
  PS/2 auxiliary device - and both sit in the same block that sets
  `ds:0x200A`, the PS/2 flag. So the PS/2 option hooks a different mouse
  interrupt, which is the whole of what it does here.

  **All four are now read**, with `tools/xref.py`, which lists every
  instruction naming a DGROUP offset directly (6, 7, 7 and 12 references
  respectively):

  - **`ds:0x200A` disables the keyboard controller around the scan-code
    read.** Image `0x016DD` sends 8042 command `0xAD` before `in al, 0x60`
    and image `0x017A3` sends `0xAE` at the ISR's exit; image `0x17B4` is the
    command send, spinning on `in al, 0x64 / test al, 2` before `out 0x64`.
    **PS/2 only** - the machine where a second scan code arriving inside the
    handler is a problem.
  - **`ds:0x200B` takes the mouse BUTTONS off the keyboard.** Image `0x0170B`
    turns scan codes `0x7D` and `0x7E` into bits 1 and 0 of `ds:0x14`, image
    `0x0173B` clears them on release, and image `0x04262` then refuses to
    copy INT 33h's own button word over the top. **Amstrad only**, and the
    PC1512/1640 is the machine whose mouse hangs off the keyboard controller.
  - **`ds:0x200C` blanks the display across the palette write.** Image
    `0x0548D` sets Sequencer register 1 bit 5 - screen off - and image
    `0x054B6` clears it again once the palette is in; image `0x054C6` is the
    same pair around a second routine. The flag is **PC compatible only** and
    the test is `je`, so the PC compatible is the one machine that does NOT
    blank and the other three do. That is the opposite of what the option's
    name suggests, which is exactly why it was read rather than assumed.
  - **`ds:0x200E` makes the palette fade a BLOCKING loop.** Image `0x00346`
    guards the fade-out: when set, image `0x03D7`'s step is followed by image
    `0x01550`'s retrace wait and the whole thing loops until the fade is
    done; when clear, image `0x035E` takes one step and falls through. The
    fade-in at image `0x0032A` is the same shape. **PC compatible AND
    Amstrad** - the two slow options - so the stepped fade the port has is
    the High Performance behaviour it targets, not an omission.
  - **One of `ds:0x200E`'s twelve references gates nothing.** Image `0x053C2`
    tests it and image `0x053C7`'s `jne` has displacement **zero**, so both
    arms reach the same `ret`. Boundary confirmed by disassembling from three
    earlier addresses and watching the stream converge. This is the second
    such site here - image `0x02CB9` is a `jns` to the instruction after
    itself - and it matters because an xref entry looks like a use.

  See [docs/machine-types.md](docs/machine-types.md) for the disassembly.

## Deferred

- **CGA and Tandy builds** — non-goals, as above.
- **The PC compatible, PS/2 and Amstrad machine types** — non-goals. What their
  flags gate is still worth *identifying* (it is in Open), so the port can carry
  honest no-ops rather than silent gaps, but the behaviour is not being ported.
- **The EGA path** through `vgalemmi.exe`.
- **`flr.exe`** (the crack's trainer), `install.exe`, and the `lem*.com`
  loaders — not part of the game.
- **`tandysnd.dat`** — the Tandy driver, unexamined, and a non-goal for the
  same reason the CGA and Tandy builds are.

  **`adlib.dat` is NOT deferred and this line used to say it was.** It is
  transcribed as `reconstruct/src/adlib.c` - its own translation unit, because
  it was its own program - and checked the way a screen is, against what the
  original COMPOSES rather than what comes out of it: ask both sides for the
  same tune through the driver's own request slots and compare the OPL2
  register stream. **10,505 of 10,505 registers and values agree, 100.00%**,
  with the gap between writes at 99.99% and a worst difference of one tick
  that is the instrument. See `tools/verify_adlib.py`, whose header carries
  the six ways the recipe for that comparison can silently produce a number
  instead of an answer.

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
