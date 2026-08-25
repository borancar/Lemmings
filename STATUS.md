# STATUS — PC Lemmings (VGA) reconstruction

Last updated: **2026-08-25**

**Which world this is in: not yet determined.** Whether the original was
compiled (making a byte-exact *matching* reconstruction possible) or written in
assembly (leaving behavioural equivalence as the standard) is unanswered — the
image has only just been unpacked and has not been examined for prologues or a
compiler banner. Everything below is scaffolding, not port progress.

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

## Open

- **The unpack is not yet *proven*.** The skill's standard is a round-trip
  against the stub's own output, and that has not been done. "It produces
  plausible code and the program runs" is evidence, not proof. Nothing has been
  transcribed on the strength of it.
- **The second start-up menu does not respond to RETURN** delivered over the
  control socket, though `1` on the first menu works. So `tools/play.py --auto`
  reaches the machine-type menu and stops there. Unknown whether it reads scan
  codes off port 0x60, wants a longer press, or is waiting on something else
  entirely. **A person at the keyboard is not blocked by this** — only the
  unattended path is.
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

1. Capture the unpacked image to a file at the moment the stub hands control to
   the program, and **prove** it — round-trip against the stub's own output.
2. Identify the toolchain: count `push bp / mov bp,sp` prologues across the code
   segment, and look for a compiler banner. Record the finding and the evidence
   in `docs/`, and say which world this is in at the top of this file.
3. Find the handler tables and map the code by recursive descent from the entry
   point, seeded with them.
4. Only then start transcribing.

## Deferred

- **CGA and Tandy builds** — non-goals, as above.
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
