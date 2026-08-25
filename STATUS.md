# STATUS — PC Lemmings (VGA) reconstruction

Last updated: **2026-08-25**

**Which world this is in: not yet determined.** Whether the original was
compiled (making a byte-exact *matching* reconstruction possible) or written in
assembly (leaving behavioural equivalence as the standard) is unanswered — the
image has only just been unpacked and has not been examined for prologues or a
compiler banner. Everything below is scaffolding, not port progress.

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
- **The original reaches gameplay under our emulator** —
  `docs/reference/original-in-level.png`: lemmings falling out of the trapdoor,
  the clock counting down, the skill panel and the minimap drawn. Its **terrain
  is not drawn**, for a reason now diagnosed and recorded upstream (planar
  reads return flat memory, so a video-to-video blit reads garbage).

## Retracted

- **"The protected code does not re-encrypt behind itself" (claimed
  2026-08-25, withdrawn the same day).** It does. The INT 01h handler
  re-encrypts the previous instruction (`xchg word ptr [si], ax` at image
  `0x0E14`) before decrypting the next, so exactly one instruction is plaintext
  at a time. The measurement behind the original claim compared two regions at
  instruction counts where neither was under single-step protection, so it
  showed only that idle memory is idle. See
  [docs/protection.md](docs/protection.md).

## Open

- **NOTHING IS VERIFIED PIXEL FOR PIXEL YET, and it is blocked.** The
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
- **No unpacked image has been written to disk**, so no code map, no
  disassembly, and no coverage table. The coverage figures below are absent
  rather than zero.
- **Toolchain unidentified** — see the note at the top.

### Coverage, as last measured

| | |
| --- | --- |
| routines transcribed | 0 |
| routines proven (ran, did work, agreed) | 0 |
| routines shallow (agreed, but every call was an early return) | 0 |
| routines differing | 0 |
| routines never reached | not yet enumerated |

No sweep has been run. These are not measurements; they are placeholders, and
the first sweep should write this table rather than have it retyped by hand.

## Next

1. **Reach the level itself in the original** — the briefing wants a mouse
   click, and the control socket has no verb for one. Then capture its video
   memory and compare the port's render against it, pixel for pixel. This is
   the check everything else is waiting on.
2. Capture the unpacked image to a file at the moment the stub hands control to
   the program, and **prove** it — round-trip against the stub's own output.
3. Identify the toolchain: count `push bp / mov bp,sp` prologues across the code
   segment, and look for a compiler banner. Record the finding and the evidence
   in `docs/`, and say which world this is in at the top of this file.
4. Find the handler tables and map the code by recursive descent from the entry
   point, seeded with them.
5. Only then start transcribing.

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
