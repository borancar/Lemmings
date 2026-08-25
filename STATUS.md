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
- **The port is a level renderer, not a game.** No lemmings, no skills, no
  animation, no sound, no control panel.
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
| routines transcribed | 0 |
| routines proven (ran, did work, agreed) | 0 |
| routines shallow (agreed, but every call was an early return) | 0 |
| routines differing | 0 |
| routines never reached | 211 call targets enumerated, none transcribed |

**Nothing is transcribed yet.** The port renders the first level exactly, but
it does that from the *file formats*, written from published descriptions and
checked against the data — not by transcribing the original's routines. Those
are different claims and the second has not been started.

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
3. **Start transcribing.** 211 call targets are enumerated and 48.19% of the
   code region is proven to be code. Nothing is transcribed. This is the work.
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
