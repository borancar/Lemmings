# PC Lemmings — working notes

How to work on this reconstruction. [STATUS.md](STATUS.md) is where the port has
got to and what is *proven*; `docs/` is what the program *is*.

## What this is

A reverse engineering of **PC Lemmings (VGA)** — `lemming1.pc/vgalemmi.exe`,
dated 1996-12-24 in a cracked repackaging — checked
against the original running under
[`dos_emulator`](https://github.com/borancar/dos_emulator).

**It is not a ten-level demo.** This file said so for a long time and it was a
misreading of "ten level *files*": `level000.dat` .. `level009.dat` hold **eight
2048-byte levels each — eighty in all**, with names from across the full game
(`Tightrope City`, `Mary Poppins\` land`, `Every Lemming for himself!!!`). Count
them with `tools/dat.py` before repeating any claim about how much game is here.

**VGA only.** The game shipped one executable per adapter (`cgalemmi.exe`,
`tgalemmi.exe`, and `vgalemmi.exe` for both EGA and VGA), dispatched by
`LEMMINGS.BAT` through `AUTODET.EXE`. Only the VGA one is being reconstructed;
the others are deliberate non-goals, not gaps. Mode 13h is chunky, so a blit
transcribes as array writes and a frame check is a plain byte compare.

## Running it

```sh
uv sync
uv run python tools/play.py --window -- --scale 3  # a window to play in
uv run python tools/play.py --auto                 # answer the start-up menus
```

`--scale` is the **emulator's** flag, not `play.py`'s, so it goes after `--`.
Without the `--` this is a usage error, which is what the line here used to
say to do.

The reconstruction runs on its own, and opens on the main menu:

```sh
make -C reconstruct
./reconstruct/lemmings                    # the game; a data dir may follow
./reconstruct/lemdev --screen menu --raw out/menu.raw   # the harness
```

Its keys are the original's, read off the plates it draws: F1 play (briefing,
level, back), F2 Enter Code, F3 FX, F4 input, Esc quit, Up / `` ` `` and
Down / `-` for the rating. A click anywhere acts as F1, which is what the
original does.

`tools/lemmings.py` is the **one place this project names the game** — its
directory, its executable, its load segment. Every tool imports the emulator
*through* it, so when the shared code moves there is one file to fix.

## Everything here is headless unless you ask for a window

`tools/lemmings.py` sets `SDL_VIDEODRIVER=dummy` before `dos_emulator` imports
pygame, so no tool opens a window on anyone's desktop. That is the right
default: the proofs, verifiers, probes and the image recorder all drive the
guest automatically and capture through the **control socket**, and a window
nobody reads is just something popping up while a sweep runs.

`tools/play.py --window` opts back in - it is the one tool that is for looking
at. `LEM_WINDOW=1` does the same for anything else, and play.py passes it down
to the machine it spawns.

The setting has to happen before the pygame import, which is why it is at the
top of `lemmings.py` and not inside a function.

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

## An in-process run never reaches a level

It answers the two start-up menus and stops. Anything that only happens inside
a level - the header parse, the composition passes, the play loop - has to be
checked under `tools/play.py`, which is the only driver that gets through the
title, the briefing and into the game.

Set `LEM_HOOK=<file>` and the machine calls that file's `install(m)` once
before anything runs, so a verifier can add its breakpoints and ride along. The
guest side should only *capture*; do the comparison in the parent, where the C
can be built and called without a compiler in the hot path.
`tools/verify_level.py` is the worked example.

Delete the capture file before the run. A stale one reads as a successful run,
and a run that never reached the breakpoint should report **that**, not a pass.

**Regenerate the port's output before analysing it — always.** A stale `--raw`
file has produced three misleading measurements here: plates that appeared to
match 18-25% when they matched 93%, a "regression" that was really a capture
taken before the fix, and a plate comparison off by a factor of twenty. Each
time the file on disk was from an earlier build and looked perfectly valid.
`tools/menu_diff.py` regenerates before it reads and has no flag to skip it;
use it rather than an ad-hoc script.

**Never compare a capture without checking its palette is up.** A screen
sampled mid-fade has an all-black DAC and every index still in place, so it
compares as an enormous difference for a reason that has nothing to do with
what is being tested - and it does not look like a mistake in a report, it
looks like a finding. It produced two here: F3 appeared to redraw 77,433 pixels
of the menu, and two captures of the same briefing appeared to disagree on a
fifth of their background. `play.capture_lit()` takes a capture and refuses a
faded one; use it rather than `screen` directly.

## Re-run the numbers before you trust the ones written down

`STATUS.md` accumulates figures over many sessions and they go stale silently -
a screen improves, one place gets updated and another does not, and the file
then holds two different answers to the same question. It happened here: the
menu was quoted at 95.26% in one paragraph and 95.04% in another, the first
being a figure measured back when the banner was excluded from the count.

Every number in there is cheap to re-derive, so re-derive them rather than
copying them forward:

```sh
for s in menu code input briefing; do
    uv run python tools/verify_menu.py --screen $s --capture out/<the capture>
done
make -C reconstruct test
bash tools/prove.sh
```

A figure that has drifted is worse than no figure, because it is quoted with
the same confidence as a measured one.

## A percentage cannot tell you what is wrong

`tools/diff_png.py` writes three images - the original, the port, and the port
with every differing pixel in magenta. Run it the moment a comparison is not
zero, before writing a word about what the residue "looks like".

Twice here a perfectly accurate number supported a wrong conclusion:

- A residue was described in `STATUS.md`, twice, as "114 pixels, almost all at
  piece edges, scattered". The image showed **one horizontal line** - the top
  row of the level preview, which the port drew a row too high. One constant,
  and it went to zero.
- The briefing's preview was recorded as "terrain only" because drawing the
  objects made the score *worse*, 187 against 167. The differing pixels were
  two tight clusters exactly where the level's objects are. They were there all
  along and every object in the game was being drawn sixteen pixels too far
  right.

Both cost a round. **An aggregate score is a poor way to decide what a screen
contains** - it is a good way to decide whether you are finished.

## Fitting narrows; only reading settles

The menu's last residue is the cleanest example this project has, and it is
worth keeping because the fitting was *good* work that still could not finish
the job.

326 pixels differed, in the plates, for many rounds. Four compositing rules
were measured against four captures — ink keyed on `!= 0` or on plane 3, cross
with the same two for the drop shadows — and the spread was wide and clear:
460, 377, 326, **257**. That is a real result: it narrowed the space and it
established the shadow rule.

It was also still wrong. Reading the blitter at image `0x0890E` gave the
actual rule in twenty minutes — the sprite is drawn in **four passes**, the
first masked through GC 8 and clearing planes 1..3, the other three OR-ing
their plane in **unmasked** — and the port went to **zero differing**.

The trap is that the fitted winner *looked* like convergence. It was the best
of the candidates, it improved on the baseline, and every further idea would
have been another candidate in the same family. No amount of it reaches zero,
because the true rule (`mask ? ink : dst | (ink & 0x0E)`) was not in the
family at all.

So: **when a comparison plateaus a little short, stop fitting and go read the
routine.** The clue that this is the situation is a residue that is
*structured* — here, 210 of the 326 were the single relation "sprite value 3
where the original shows 2". A structured residue is a rule you have not
found, not noise to be whittled down.

## A check at one value of a parameter says nothing about the parameter

The rating plate cost a day twice over, and both halves are general.

**Verifying at one value proves the value, not the formula.** The port drew the
rating name from `RATING_BASE + rating * RATING_STEP`. The four sprites are
actually stored *backwards*, so that expression is right for exactly one of the
four values it takes — and rating 0 was the only rating reachable, because the
plate appeared inert. At `rating == 0` the sign of the step does not matter.
The check passed, repeatedly, and was silent about the only thing it could have
caught. **If a routine takes a parameter, reach more than one value of it
before calling it verified**, and if you cannot reach the others, say so where
the score is reported rather than beside the code.

**"Not delivered" is not "delivered, no effect".** A sweep concluded the rating
plate answered to nothing, having tried 25 keys. Four of them came back from
the control socket as `error: unknown key name` — the socket resolves names
through `pygame.key.key_code`, which names punctuation by the character, so
`"minus"` is not a key and `-` is. The sweep printed the error in the same
column as a real result. Two of the keys that *do* work were not in the list
at all, though the prose named them as ruled out.

So: **a probe must fail loudly on a key it could not send**, and a negative
result is only evidence if the positive control fired in the same run. The
sweep also watched the plate's *pixels* from Fun, where Down is supposed to do
nothing — a correct key looks inert from the floor. Watching `ds:0x1F6E`
instead settles it in one byte.

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

Timing that capture matters and **used to fail silently**: a dump taken at the
wrong moment looks exactly like a port that is 83% right. `tools/prove.sh` now
asserts on `where` - the guest must actually be stopped at `0x0376A` - retries
once, and fails loudly rather than comparing against a bad capture.

When a comparison suddenly regresses, **check whether the port changed at all**
before believing it: rendering with the previous commit's binary and diffing
the two raw outputs takes a minute and settles it. Byte-identical output means
the reference moved, not the port.

## Conventions

- **SDL3 for the window, input and sound.** Never X11, Win32, Cocoa or SDL2;
  never behind an `#ifdef`; never as "the optional viewer" beside a file
  writer. The port's claim is that it is the same game somewhere the original
  cannot run, and a platform-specific display layer takes that back. Two
  rendering paths also drift, and only the one a given build takes is ever
  looked at. SDL's job stops at putting the port's own composed 8-bit buffer
  in a window and handing back input - it must not scale, filter or blend
  anything the original decided for itself.
- **The port shows a screen by default.** `./lemmings` with no arguments opens
  the game; `--out` or `--raw` is what asks for a file instead. A
  reconstruction whose default output is a bitmap is a converter.
- **`game.c` holds the game and nothing else.** A function marked "the port's
  own" inside it is a smell: either it is **IO** - it replaces something a
  card, the BIOS or DOS did, and belongs in `sdl_io.c` - or it is **game logic
  written instead of transcribed**, which is the thing this method exists to
  avoid. `sdl_io.c` therefore holds more than SDL: the DAC's 6-to-8-bit
  widening, the assembly of a pixel out of four bit-planes, the frees DOS used
  to do, the window loop, and the flat header view `verify_level.py` needs.
  See "WHAT THE HARDWARE USED TO DO" there.
- **Prefer transcribing to writing.** If a routine does something the original
  does, go and read the original's. A behavioural match is only as good as the
  states you happened to compare, and this port has shipped several that
  agreed with every capture and were still wrong.
  `reconstruct/tests/provenance.py` counts what is transcribed, what is ours,
  and what is **owed** - game logic the original has a routine for that nobody
  has read - and `make test` fails while any function says none of the three.
- **`main.c` is the game's entry point and nothing else; `devmain.c` is the
  harness.** The original has no command line - it starts, shows its menu, and
  plays - so `main.c` mirrors that, and every developer flag lives in
  `devmain.c` and links into a second binary, `lemdev`. `tools/` calls
  `lemdev`. This drifts back on its own, because a new flag is easiest to add
  where the parsing already is; it had produced a four-hundred-line `main.c`
  whose actual start-up was a dozen lines lost among the options.
- **No file writer in the game path.** There was a `--out` that wrote a BMP and
  nothing wanted one: a BMP throws away the palette *index* a pixel had, which
  is what a comparison against the original's video memory needs. `--raw`
  carries indices; pictures are `tools/diff_png.py`'s job. `prove.sh` had been
  passing `--out /dev/null` for a very long time.
- **The port's `.c` files mirror the original's translation units**, not our
  own idea of modules. Compilers of the period emitted one object per source
  file and the linker laid each module into the segment whole and in link
  order, so a contiguous run of addresses is one original source file and
  address order is source order inside it. That is the only division of this
  program its authors made. The port is `game.c` + `game.h`; `sdl_io.c` and
  `main.c` are boundaries **the port chose**, and their headers say so.
- **Every function carries where it came from** — the image offset if it is
  transcribed, and an explicit "not transcribed, address unknown" if it is
  not. Transcribed *data* (a palette, a string table) takes the same comment.
  This is the convention that decays first: each omission is trivial and the
  loss only shows up later, when a verifier disagrees and there is no way to
  find the routine it disagreed about.
- **No licence header on reconstructed code.** The original's licence is
  unknown and a file transcribed out of someone else's binary is not ours to
  licence; an SPDX line asserts exactly that, machine-readably. Reconstructed
  sources carry a provenance header naming the binary instead. Our own
  tooling is a different matter.
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
| `docs/menu.md` | what the menu screen *is*: the mode, the palette, where every piece lives in MAIN.DAT, the font, what each plate does, and what animates |
| `tools/lemmings.py` | the adapter: game dir, executable, load segment, start-up keys, `LemmingsVga` |
| `tools/extract_image.py` | write the unpacked image once, with a sha256 and the load segment beside it |
| **`tools/verify_menu.py`** | the port's main menu against the one the original composes, in EGA mode 10h - and it scores the parts the port does not build separately rather than hiding them. `--rating 0..3` drives the original's own rating plate first, and reads `ds:0x1F6E` back so a dropped key press costs coverage instead of producing a confident wrong number |
| `tools/hotspots.py` | ranks routines by how much work they do, not by whether they are code. `--after-read` counts only inside the windows following an INT 21h AH=3Fh, which is how the decompressor was found |
| **`tools/diff_png.py`** | **three pictures of a difference**: the original, the port, and the port with every differing pixel in magenta. `--rows`/`--cols` to crop, `--scale` to enlarge. Emit these the moment a comparison is not zero - a percentage says how much is wrong and never what, and this has twice here supported a confidently wrong conclusion that one look would have killed |
| **`tools/menu_diff.py`** | where the port's menu differs from the original's, regenerating the port's output first so a stale file cannot mislead. `--map` for a picture, `--rows`/`--cols` to zoom |
| **`tools/probe_briefing_bg.py`** | is a screen's background stored or generated? Two lit captures from separate runs, then a search of every file **with a control** - a search that finds nothing proves nothing unless the same search finds something it should |
| **`tools/probe_menu_keys.py`** | what a menu key actually does, asked of the original: fresh guest per key, held and repeated press, screen diffed against a no-key baseline, and it refuses to compare a capture taken mid-fade |
| `tools/profile_menu.py` | which routines run while the menu is up, through `LEM_HOOK` |
| **`tools/verify_preshift.py`** | the level's sprite pre-builder at image 0x05B11, checked against the original's own output; `--driven` rides along with play.py for shapes a start-up run never reaches |
| **`tools/verify_shift.py`** | the sprite pre-shift at image 0x09347, checked call by call against the original's own buffer |
| **`tools/verify_digits.py`** | three-digit decimal at image 0x04BC2. Also the pattern for a **pure** routine: rather than wait for the game to call it, point CS:IP at it and drive the original's own code over every input |
| **`tools/verify_level.py`** | the same for the LVL header parse, and the example of a verifier that rides along with `play.py` via `LEM_HOOK` - needed for anything that only happens inside a level |
| **`tools/verify_dat.py`** | the differential test, and the pattern to copy for the next routine: stop the original, capture the inputs, let **its own body** run to its `ret`, compare its output with the port's C on the same call |
| `tools/codemap.py` | the code map: recursive descent seeded with the entry point *and* every recorded execution, so it separates proven-code from believed-code. `--routine` for one routine, `--tables` for dispatch tables (there are none) |
| `tools/decrypt_image.py` | builds `image/vgalemmi.decrypted.img` - records each instruction's plaintext as it executes and writes it back. Merges per-run records, so coverage only grows |
| `tools/disasm.py` | disassemble the decrypted image at an image offset |
| `tools/dat.py` | Lemmings DAT decompression, the Python oracle the C port was checked against |
| `tools/snapshot.py` | capture memory at a chosen point in a run |
| `tools/probe_protected.py` | record the protected instruction stream as it runs |
| `tools/probe_desync.py` | find a single-step desynchronisation, if the decryptor ever loses step |
| **`tools/probe_rating.py`** | the rating plate, watching `ds:0x1F6E` rather than the plate's pixels - and refusing a key the control socket would not deliver |
| **`tools/probe_blits.py`** | every blit into a chosen band of rows, **and who called it** - it reads the far return address off the stack at the blitter's entry, which is what turns a list of rectangles into a call graph. `--driven` rides along with play.py, `--play` goes on past the briefing into the level, `--seconds` sets how long that run lasts |
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
