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
- **The protection is characterised, and measured rather than assumed** — see
  [docs/protection.md](docs/protection.md). `IVT[01h]` = image 0x00DFF;
  `EFLAGS = 0x0146` (TF set) at handler entry with no `int 1` in the stream;
  108 single-steps in a whole start-up run.
- **The original reaches its main menu under DOSBox** — captured in
  `docs/reference/dosbox-main-menu.png`. So `vgalemmi.exe` runs standalone, its
  protection is satisfiable, and any failure here is ours.

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

- **THE GUEST DERAILS INSIDE THE PROTECTION, and this now outranks the
  renderer.** After reaching mode 0Dh the game moves `IVT[01h]` from image
  `0x00DFF` to `0x00D0C`, and under our machine the bytes there are still
  encrypted: it executes garbage and spins in a ~14-instruction loop at
  `0x00D17..0x00D39`, taking zero interrupts for 40M+ instructions. DOSBox runs
  the same binary to the main menu. The decryption depends on receiving exactly
  one `#DB` per instruction, so a single extra or missing trap desynchronises
  the rolling key — the `mov ss`/`pop ss` interrupt shadow is the leading
  suspect, unconfirmed. Building a renderer before this is fixed would be
  rendering a crashed guest.
- **THE PORT DRAWS NOTHING, AND NEITHER DOES THE EMULATOR.** PC Lemmings' VGA
  build runs in **BIOS mode 0Dh** — 320x200, 16 colours, four bit-planes, eight
  pixels to the byte. It is *not* mode 13h. `dos_emulator`'s `framebuffer()`
  handles CGA 04h/05h/06h, chain-4 mode 13h and Mode X, and has no path that
  gathers a pixel's four bits across planes, so the game reaches its first
  graphics screen and renders black. Implementing EGA planar upstream is the
  next piece of work and everything visual is blocked behind it.
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

1. **Implement EGA 16-colour planar mode 0Dh in `dos_emulator`** — the
   framebuffer decode (four planes, eight pixels per byte, a pixel's value
   gathered across planes), the Graphics Controller write path (set/reset,
   enable set/reset, bit mask, the write modes and the latches) and the
   attribute controller's 16-entry palette. Nothing visual can be checked until
   this exists.
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
