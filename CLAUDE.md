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

**And a wrong rule can still improve the number.** The terrain erase bug on
level006/3 sat at 814 differing pixels. The first hypothesis - that erase only
applies to a byte-aligned piece - took it to **459**, which by the only
measure available is progress and was a different wrong answer. The real rule
came from reading image 0x07C67's flag unpack: the erase bit sets bp bit 6
*and* the 0x0F00 nibble, and image 0x088F3 dispatches no-overwrite before
image 0x08905 ever tests bit 6, so a piece with both flags is drawn rather
than erased. That went to zero. A score that improves is evidence the change
moved something in the right direction, and nothing at all about whether the
reason is the real one.

So: **when a comparison plateaus a little short, stop fitting and go read the
routine.** The clue that this is the situation is a residue that is
*structured* — here, 210 of the 326 were the single relation "sprite value 3
where the original shows 2". A structured residue is a rule you have not
found, not noise to be whittled down.

## A round trip only checks the round trip

The access code has two routines in the original: image 0x05042 makes one and
image 0x051A2 takes one back. That looks like a free oracle - generate a code
with the port, feed it to the validator, and if it is accepted the generator
is right.

It is not. The validator checks a **checksum the generator computed**, so it
accepts whatever the generator produced. Three deliberate faults were put into
the port's generator - a bit moved to the wrong character, the rotation run
backwards, the level's two nibbles swapped - and all three produced codes the
validator was perfectly happy with. Every one of them would have shipped.

What settled it was driving the original's own routine and comparing its ten
characters with the port's: `tools/verify_code.py`. The round trip is still
worth having, but as the weak claim it is, which is why the tool prints it on
its own line and says beside it that a wrong table passes too.

The general form: **a check whose reference is derived from the thing under
test is not a check.** Two routines in the original agreeing is evidence; one
routine agreeing with itself through another is not.

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

## A frame diff of a level does not measure terrain

Two rounds went into "the bomber's crater clears 29 pixels where the original
clears 343". The 343 was never terrain. It was the explosion's CLOUD - a
sprite of 0x280 bytes a frame, far bigger than a lemming - appearing over the
ground between the two frames being differenced. The crater underneath it was
correct the whole time.

What settled it was comparing the two TERRAINS once the cloud had gone, at
frame 270 rather than 207: every row from 62 to 90 identical, and the record
comparison at 0 of 400.

So: **when measuring what a routine did to the LEVEL, difference two frames
in which nothing is drawn over it.** A frame is terrain, sprites, cursor and
cloud, and a diff cannot tell them apart. This file already says a percentage
cannot tell you what is wrong; this is the same rule one level down, about
what a pixel even IS.

Two other things went wrong on the way and both are worth keeping:

- **`make ... >/dev/null 2>&1` hides a compile error**, and every
  "measurement" after it reads the previous binary. A `sed` had mangled
  `#define CRATER_UP 0x1E` into `30x1E`; four offsets were swept and all four
  reported the same number, from a build that had never happened. Check the
  build succeeded before believing what the binary says.

- **When a parameter has no effect, stop tuning it.** A routine landing in
  the wrong place still clears a DIFFERENT wrong set when you move it. No
  response to being moved means it is not doing what you think - and a
  deliberate 40x40 block, too big to miss, answers that in one run.

## Finding a sprite: score it, and check the scorer on one you know

The builder's frames had to be found in MAIN.DAT the way the digger's and the
exit's were, and it took three attempts.

**A shape that looks right is not evidence.** The sprites sit consecutively -
the walker is 18 frames of 40 from 0, so the digger starts at 720 and its 16
frames of 84 end at 2064 - and 2064 decodes as a perfectly plausible lemming.
Wiring it in changed the comparison by nothing. Every frame of every sprite
in this file looks like a lemming.

**Lift the original's own pixels.** The record says where a lemming is, so its
sprite's rectangle on a captured frame is known exactly: frame 130, record
(796, 94), hot spot (-8, -13), which puts its 16x13 at screen (148, 65). Then
try every offset in the section against those pixels.

**But do not demand a perfect match, and this is where a whole round went.**
Every offset failed, which read as "the frames are not in this section". It
was the CURSOR - the input script parks the pointer on the lemming it
clicked, so the cross-hair's box sits over the sprite and about thirty pixels
can never agree. Scoring instead of demanding gives the answer at once, and
it holds across five different animation frames with 0, 1, 2, 2 and 0 pixels
disagreeing, every one of them under the cursor.

**Check the scorer on a sprite whose offset is already known** before
believing what it says about one that is not. Run it against the digger and
it comes back 720 with two pixels disagreeing - the same cursor. That is what
turns "no offset matched" from a fact about the file into a fact about the
search.

## The dispatch is not the only thing that dispatches

The builder's last eight frames could not be explained from its record. The
flag word read zero, so image `0x01F5E`'s chain says "walk", and the original
was plainly not walking - the position did not move while the animation
counter climbed.

Because the flag word is not asked first. Image `0x01F54` reads `[si+0x24]`,
tests bit 7 and jumps to image `0x02DB5` - the shrug - **before** image
`0x01F5E` ever looks at the flags. There is a second test above it too, at
image `0x01F45`, on `[si+0x20]`.

So the loop head is part of the state machine, not scaffolding around it, and
a port that transcribes the chain and calls the loop its own has left a
branch out. What made it findable was recording the fields nothing had
needed until then - the direction at +0x27, the state at +0x29, the byte at
+0x24 - because **a record comparison that stops at position cannot see a
state that does not move anything.** x, y, the hot spot, the flags and the
animation counter all agreed; the difference was in a byte none of them
carried.

## The same constant can be a guard and a keep-mask, and only one is the bug

Image `0x0320D` refuses a digger with `test word [si+8], 0xf57f` and then,
four instructions later, uses **the same 0xF57F as a KEEP mask**:
`and word [si+8], 0xf57f / or word [si+8], 0x10`. That reads as a
contradiction and is not - by the time the AND runs, the test has already
established that none of those bits is set, so it does nothing.

The port had taken the second reading for the first and tested the
COMPLEMENT, so a lemming already digging (0x10 is in the mask) sailed
through, was assigned again, and spent a second digger on itself. **A player
found it. Nothing here could**: every check in this repository compares
against a recorded run in which the click was made once.

And what the mask actually holds is worth asserting rather than summarising.
0xF57F is bits 0..6, 8, 10 and 12..15 - so a BLOCKER is refused (0x100 is bit
8) and a BUILDER is not (0x80 is bit 7, and it is clear). A builder can be
turned into a digger mid-brick. "Refuse anything busy" would have been a
tidier rule and a wrong one.

## A decision made in main.c is a decision no test can reach

Five of the faults fixed in one session came from the user PLAYING the game,
not from any check in this repository, and three of those lived in the same
place: `main.c`, deciding something the game decides for itself.

- it passed a constant 0 for "did the clock run out", so a level that timed
  out was told "All lemmings accounted for."
- it worked out the requirement percentage inline, correctly, with no address
  against it - a guess that happens to be right is still a guess
- it took any click on the results screen as "next level", because the menu's
  hotspot matcher reports WHERE a click landed and not WHICH button made it

None of these is visible to a frame comparison. `verify_results.py` sets the
clock itself, so it was confirming the screen renders a timed-out result
correctly - which it did - while the game never asked for one. **A harness
that feeds both sides the same answer cannot tell you the question was
wrong.**

The rule the port already has covers it and had drifted: **`game.c` holds the
game.** A thing the original decides is a thing to transcribe with its
address, not to recompute in the caller - and the practical test is whether a
unit test can reach it. `level_out_of_time` (image 0x04710) and
`level_needed_percent` (image 0x04591) are both three lines long, and both
were in `main.c` where nothing could ask them anything.

So when a routine's INPUTS are assembled by the caller, check the caller
against the original too. The screen being pixel-exact says only that it
draws what it was told.

## A verifier that never reached the positive case has verified nothing

`verify_hit.py` compared 326 frames against the original's own `ds:0x1A` and
agreed on all 326. It looked like the strongest kind of check this project
has - driven by the original, frame by frame, with the guest's own answer as
the reference. What it actually established was that both sides say "nothing"
when the pointer is in open sky, because **in none of those 326 frames was
the pointer over a lemming**. The one case the sixteen-pixel bug lived in was
never reached, and the tool did not say so.

Two things fixed it, and both generalise:

- **Report the directions separately.** A single agreed/differed count hides
  which way the frames went. It now prints `over a lemming : n/m` beside
  `over nothing : n/m` and says outright when the first is zero.
- **Drive the input rather than ride along.** The hook now sweeps the pointer
  over a 21x21 block around a lemming - bigger than the hit box, so it
  straddles the edges - aimed with nothing but the lemming's raw x and y. A
  sweep aimed by the arithmetic under test would only confirm it.

The result went from 326 frames of nothing to 600 frames with 256 real hits,
still agreeing.

Getting there caught an off-by-one in the INSTRUMENT, which is worth its own
line: the array was being sampled at the end of the frame while the guest had
scanned it mid-frame, and a walker moves a pixel in between. One frame in 545
disagreed, at a box edge, and it read exactly like a port bug. Capturing at
image `0x03DAF` - the instruction that clears `ds:0x1A`, immediately before
the scan - is the one moment the inputs and the answer belong together.

**And note what a driven check still cannot see.** Both sides here are handed
the guest's `ds:0x10` and `ds:0x12`, so this scores the relation from those
two words onward and nothing before it. The trip from a SCREEN position to
`ds:0x12` is the port's own, and it can be wrong while every frame agrees.
That is `tools/compare_play.py`'s claim, not this one.

## Drive the ending, do not play to it

Reaching the results screen by playing costs fifteen minutes of emulator
time: enter the level, assign a digger from a script, walk every lemming out
of the exit. Three runs went that way and two produced nothing - one captured
on a retrace the screen never waits for, one with a frame counter that had
started before the level did. Neither said so; they just ran to their ceiling.

None of it was necessary. Image `0x046EB` is a plain routine and the screen
is a pure function of six bytes - the rescued percentage, the requirement,
the level index, the two clock cells and the input kind. `tools/verify_results.py`
sets them and CALLS it, stopping at image `0x047A9`, the screen's own wait
loop, which is the moment everything is drawn and nothing has happened yet.
Ninety seconds, and repeatable while you edit the port.

That is `tools/verify_digits.py`'s trick applied to a screen instead of a
routine, and the rule generalises: **if the state is a pure function of a few
bytes, set them and call it.** Playing to a state is for things that are not.

Stepping the calls is worth the same again. `LEM_RES_STEP=1` stops after each
of image `0x046EB`'s seven set-up calls and prints the framebuffer's
histogram, which answered "which of these draws the background" in one run
where reading them one at a time had already taken three.

**And check whether the background is the game's at all.** `LEM_RES_WIPE=1`
zeroes video memory before the call. Driving a routine from the menu leaves
the menu's pixels behind, and anything the routine does not clear shows
through looking exactly like something it drew.

## A run that reaches the exit takes longer than a command may run

Three attempts at `tools/probe_exit.py` reported **"0 lemmings reached it"**,
and each time that read as a fact about the game: the digger would not assign,
the click was mistimed, the probe was flaky. It was none of those. The runs
were being killed at a ten-minute ceiling on how long a single command may
take, and the file they left behind said only what they had managed before
being cut off.

Reaching the exit on "Just dig!" needs a lemming to be given a digger, the
shaft to break through, the lemming to fall and then walk the lower ledge to
the far side. That is several hundred game frames, and the emulator runs at a
few frames a second.

So: **a probe that has to play a level for minutes must run in the
background**, not inside a command with a timeout. And a probe that finds
nothing must be able to say whether it finished - "0 lemmings reached it"
after being killed is not a measurement, and it cost three rounds here of
treating an instrument's limit as the subject's behaviour. The same rule this
file already states about stale captures applies to truncated runs.

## A test written from the same reading as the code proves nothing

`level_lemming_at` was sixteen pixels wrong and its unit test passed. Both
came from one reading of image 0x03D0A - that the pointer's record x is
`scroll - 16 + mouse_x` - and both used the GUEST's scroll where the port's
`view_x` is sixteen less. The test agreed with the code because they were the
same mistake written twice.

The port has three coordinate systems - record, canvas and screen - and the
sixteen between them is the offset `tools/prove.sh` prints and
`tools/compare_run.py` fits. Any test that hard-codes a number from the
disassembly rather than from the port's own units can only confirm the
author's arithmetic.

So: **for anything with a coordinate in it, check against the ORIGINAL, not
against your reading of the original.** `tools/verify_hit.py` is the pattern -
the guest clears ds:0x1A at image 0x03DAF and sets it at image 0x03DE8 when
its own scan finds a lemming, so that byte is the answer, and the port has to
agree with it frame by frame on the guest's own inputs. It compares 326
frames and would have failed on the first one.

## The level runs at seventeen frames a second, not seventy

The clock fell down its own display in the playable game while every frame
matched, and the two facts are not in tension: the frame-by-frame comparison
scores frame N against frame N, and says nothing about how long a frame is
meant to LAST.

`LEVEL_FPS` is the game's own number - image 0x0418 and image 0x041D start
`ds:0x85` and `ds:0x86` at 0x11, and the clock steps a second every time that
counter runs out, which is exactly what makes 5,100 frames the five minutes of
the expiration timer. The display loop was sleeping 16 milliseconds, so the
level ran at sixty and the game was three and a half times too fast.

Two things follow, and the second is the one that bites:

- **The play loop's rate is the GAME's, not the display's.** 70 Hz is what the
  emulator's guest measures off the CRTC, and the port has no business
  borrowing it: the original waits five ticks a frame and lands on seventeen.
- **A frame-for-frame match cannot see this.** Nothing in
  `tools/compare_screen.py` or `compare_run.py` would ever have caught it -
  they compare pictures, not durations. It took someone watching the clock.
  If the port ever grows a check for pace, it has to be a wall-clock one:
  N frames should take N/LEVEL_FPS seconds, and that is measurable
  (`LEM_FRAMES=170` takes 10.0 s).

**The menu is a different mechanism, not a different number.** Image 0x01550
is a vertical retrace wait - it reads the status register at `ds:0x1FDA + 6`
and spins for bit 3 to go high and then low - and the menu loop calls it once
a pass (image 0x0445B). So the menu advances one step per retrace and its
speed is the *adapter's*: 70 Hz for the modes this port uses, which is both
what `tools/lemmings.py` hands the emulator and what the game's own
measurement at image 0x1602 arrives at. The level waits several retraces a
frame and lands on seventeen.

Measuring this from the outside does **not** work: counting menu passes
against `m._elapsed()` in a guest run gives 3.4, 5.0, 4.9, 3.7, 8.3 and 3.4
per second in consecutive windows, because what that measures is the
emulator's throughput, not the game's intent. The rate has to be read out of
what the loop waits ON.

Pace on an **absolute deadline**, not a sleep per frame. A sleep is at the
mercy of the host scheduler and of however long the frame took to compose,
which makes the game's speed a property of the machine rather than of the
game; and falling behind must reset the deadline rather than build a debt the
loop then sprints to repay.

## Five thousand one hundred frames is the level NOT being played

Every long comparison here has been 5,100 frames, because that is the level's
expiration timer - 5 minutes at 17 a second - and with nobody touching the
game that is exactly how long it lasts.

Play it and it does not. Give one lemming a digger on "Just dig!" and the
other nine follow it down the shaft and out of the exit; the level is over at
about **708 frames**, forty-one seconds, and the game fades to its results
screen. The timer is what happens when you FAIL.

So a scripted run must be budgeted by what the level does, not by the timer,
and a recorder waiting for 5,100 frames of a level that ended at 708 sits
there producing nothing while looking exactly like a slow run. What settled
it was the last captured frame: `lit DAC entries: 0`, the fade-out, which
`record_level.py` already prints for every capture and which this file
already warns about in another context.

Two things follow:

- **Score the level, not the timer.** The whole-level comparison is ~760
  frames and finishes in minutes; 5,100 was measuring a level nobody plays
  for four fifths of its length.
- **A stalled recorder and a slow one look the same.** Check the frame count
  against the clock, and look at the last frame before assuming the machine
  is just busy.

## A capture holds one palette, and the screen has two

The level is EGA mode 0x0D in sixteen colours and the game **swaps the upper
eight part-way down the screen**: the play area keeps the terrain's browns,
the panel below gets greens and greys. `tools/record_level.py` dumps the DAC
once a frame, so a capture holds *one* of those palettes - and the port
rendered the whole 320x200 from it, putting the terrain's oranges through the
status line while **every index matched the original**.

Both halves of that were true at once, and the index comparison could never
have caught it. So:

- **Matching indices is not matching colours.** `tools/compare_frame.py`
  compares indices, and should - it is what makes the check independent of a
  palette sampled at one moment. But a screen that scores 100.00% can still be
  the wrong colour, and nothing in the pipeline says so.
- **Measure the palette over TIME, not at a point.** `tools/probe_dac.py`
  samples the emulator's palette every 512 instructions between two of the
  game's own end-of-frame points at image 0x018F. Every settled frame holds
  exactly two, DAC 16..23 alternating and ending on the terrain's.
- The panel's eight are image `0x0C080`, and `--pc high|compat` selects
  whether the swap happens at all - image 0x055B7 branches on `ds:0x1F7C`,
  which the adapter letter sets.

**The original DOES take a command line.** This file and `main.c` both said it
does not. `LEMMINGS.BAT` never runs `VGALEMMI.EXE` directly - it runs
`lemvga.com /v` for VGA and `/e` for EGA - and the parser at image `0x012E0`
reads that letter.

## A scripted pointer has to be HELD, or the dummy driver takes it back

`compare_play.py` drives both sides from one input script, and the guest's
pointer kept wandering back to (160, 100) a few frames after the script put
it somewhere. It looked exactly like the GAME repositioning it - Lemmings
plausibly confines its cross-hair to the play area - and a reading of image
`0x0715F` was already under way to find the clamp.

There is no clamp. Under `SDL_VIDEODRIVER=dummy` pygame still delivers a
`MOUSEMOTION`, and the emulator's window loop takes it and overwrites
`mouse_pos`; (160, 100) is what that event maps to. The guest had not called
INT 33h AX=4 once - the emulator's own `mouse_calls[4]` stayed at **2**, both
of them from start-up, and that counter is what settled it in one run after
two rounds of reading disassembly.

So:

- **Re-assert a scripted pointer every frame**, not only when it moves. That
  is also what a player does: a hand resting on a mouse holds a position, it
  does not set one and let go.
- **A button press must be held across frames** for a different reason - the
  game samples it through its own handler rather than on an edge.
- And when the guest appears to do something surprising, **count the calls
  before reading the code**. "Which interrupt did it use, and how often" is a
  cheaper question than "where is the clamp", and here the answer was that it
  had not used one at all.

This is the same trap as the edge-scrolling note above: under the dummy
driver the pointer reads as present, focused and somewhere definite, and
none of those is true.

## Wrapping a method the emulator already registered records nothing

`probe_dac.py`'s first version replaced `m._on_out` to watch port writes. It
reported "no port writes" for every frame of a level, which read as a finding.
Unicorn was handed the **bound method** when the machine was built, so
reassigning the attribute leaves the original registered and the wrapper is
never called.

Sample state from a `UC_HOOK_CODE` callback instead, the way
`tools/verify_level.py` installs its hooks. And treat a probe that finds
*nothing at all* as a probe to check before it is a result to report.

## Run the game's own path, not a second arrangement of it

Every frame-by-frame number here came from `lemdev`, which composed the canvas
and the panel separately and re-simulated from zero for each frame. The window
path was a different arrangement of the same calls, and `lemdev --show` was a
**third** - a 320x160 window of a static canvas with no panel, no lemmings and
no stepping. All 5,100 frames matched while `./lemmings` showed an empty cave.

The per-frame order now lives in `game.c` (`level_play_step`,
`level_play_draw`, `level_panel_build`) and both callers use it; `--show`
calls `level_show`, the same entry `main.c` uses; and `LEM_SCREEN_DIR` makes
the game write the 320x200 screen it actually displays so *that* can be
scored. Being able to run it turned up three faults in one afternoon: the
viewport centred on `start_x` instead of using it as the left edge, edge
scrolling that could not tell "pointer at the left edge" from "no pointer"
and walked off the level, and a first frame drawn after a step that put the
whole game one frame ahead.

**A sequence written out twice is a sequence that will differ.**

## RUSSELL.DAT must start from RUSSELL.VAR

`RUSSELL.DAT` is a working file the game rewrites, and its state decides what
happens at start-up: drifted, the game takes its floppy path at image `0x0062B`
and quits with "Lemmings Disk 1 Not found" before drawing anything. The
packaging knows it - `LEMMI.BAT` is three lines and two of them copy
`RUSSELL.VAR` over it.

`LemmingsVga` now seeds the overlay from `RUSSELL.VAR`, so every run starts
shipped-clean without touching the host directory. If runs suddenly report
"never reached mode 10h", something ran the game **outside** this emulator -
which does not intercept the write - and `git checkout` will not necessarily
fix it, because the committed copy is itself a used one.

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

## A blit's colour need not be the sprite's

The one-way arrows came out blue where the original draws them red, and the
sprite was not the reason. Both one-way tiles in `ground2o.dat` decode to
attribute 1 and nothing else - 200 pixels, one value, on every one of their
eight frames - so no reading of the sprite could ever have produced the 5 the
original's video memory holds.

The colour is the **blitter's**. Image 0x096BA sets the graphics controller's
Set/Reset to 4 and image 0x096C2 its Enable Set/Reset to 0x0E, so planes 1, 2
and 3 come from that constant and only plane 0 comes from the ink. The sprite
supplies a *shape*; the routine supplies the colour, and it clears plane 3
while it is at it, so an arrow is not terrain afterwards.

So when a shape is right and its colour is not, **the sprite is the wrong
place to keep looking.** Two cheap things settle it:

- **Ask whether the sprite could produce that value at all.** A one-colour
  sprite and a two-colour result is a contradiction, and it is one line of
  Python. That is now a unit test, because it is the premise the fix rests on
  and it is a fact about the FILE - unlike an assertion about
  `object_pixel_value`, which would only repeat the reading the code was
  written from.
- **Search the image for every Set/Reset load rather than following flags.**
  There are eighteen in the whole program. Three rounds went into chasing
  `bp` bit 14 to image 0x0969C when the blitter's own entry at image 0x088EA
  sends it to image 0x08D66 instead - a routine that forces planes 1..3 to
  zero and can only ever write 0 or 1. An exhaustive search over a small set
  beats a chain of inferences over a big one.

**And do not read a static render's animation phase as a bug.** The port's
arrows sat six pixels left of the original's, which looked like a placement
error stacked on the colour one. The tiles are animated - eight frames that
march sideways - so a `--raw` render at frame 0 against a capture at frame 600
is simply a different phase. `--obj-frame` swept it to a match in seconds, and
the frame-by-frame comparison, which advances the animation itself, was exact
once the colour was right. A one-frame comparison is a fine instrument, but
every clock in the picture has to be set to the same time.

## Read the segment register; do not assume which segment a thing is in

The trigger map - the only thing that answers "what is under this lemming" -
is addressed `ss:[bx]`. Image 0x023A6 reads it that way and image 0x03886
writes it that way. `tools/verify_triggers.py` dumped it from DGROUP instead,
because a small-model DOS program usually has `SS == DS`, and got a page of
unrelated stack variables.

That dump scored the port's map as **47% wrong**, and it was a fact about the
address. What killed it in one look was checking the SHAPE of both sides
before quoting the percentage: the "original's" map held all sixteen effect
values in roughly equal numbers, and no level's trigger map ever does. The
port's held three - the one-way walls, the steel and the exit - which is what
a real one looks like.

So: **when a routine names a segment, read that segment register out of the
guest rather than working out which one it ought to be.** The tool now prints
`SS` beside its result, so a wrong segment arrives as something visible
rather than as a score.

## A fixed output path is a shared global, and a slow writer makes it race

`tools/verify_triggers.py` wrote the original's dump to `out/orig_trig.bin`
whatever level it was given. Running it on one level in the background while
measuring another in the foreground meant the background run **silently
overwrote the dump**, and three measurements were then taken of one level's
port map against a different level's original.

They did not look like nonsense. They looked like a regression: 16,040 of
16,040 cells agreeing, then 14,666, right after a change - so the change was
reverted as refuted, and a comment was written into `game.c` saying the
reading had been *measured and was worse*. That comment would have been a
confident lie in the repository, resting on nothing.

What caught it was reverting the revert and measuring AGAIN: still 14,666.
A number that does not move when the code moves is a number about the
instrument.

So:

- **name outputs after their input** - `orig_trig_77.bin`, not `orig_trig.bin`
- **do not start a long background run that writes where a foreground
  measurement reads.** This project runs the emulator for minutes at a time,
  so the window for this is enormous.
- and when a change makes a score worse, **check the change is the only thing
  that moved** before writing the refutation down. Reverting and re-measuring
  costs one run and is the difference between a fact and a plausible story.

## A comparison has to be bounded by what the program can read

The same tool then reported **3,180 differing cells** on a map that agrees
completely. The port's array is 64 rows because that is convenient; the
original's map lives in its stack segment with other variables after it, and
everything past the live extent compared as a difference.

The bounds are the program's own and both have addresses: image 0x02EC5
refuses any lemming at `y >= 0xB0` and the row is `(y - 0x10) >> 2`, so row 39
is the last one anything can ask about; the canvas is 1,600 wide and the
column is `x >> 2`, so column 400 is the last. Inside that, 16,040 of 16,040
cells agree.

**Dump generously and compare narrowly.** A dump sized to the port's array is
fine; a comparison sized to it is a comparison of the port against memory the
program will never look at.

## Conventions

- **SDL3 for the window, input and sound.** Never X11, Win32, Cocoa or SDL2;
  never behind an `#ifdef`; never as "the optional viewer" beside a file
  writer. The port's claim is that it is the same game somewhere the original
  cannot run, and a platform-specific display layer takes that back. Two
  rendering paths also drift, and only the one a given build takes is ever
  looked at. SDL's job stops at putting the port's own composed 8-bit buffer
  in a window and handing back input - it must not scale, filter or blend
  anything the original decided for itself.
- **One way out, and it is `sdl_io.c`'s.** Closing the window and Ctrl+C both
  end the process immediately, through `view_quit_now` - called from every
  `SDL_EVENT_QUIT` site and from a `SIGINT`/`SIGTERM` handler - and it
  `_exit`s so `view_shutdown` does not run over its own freed handles. No
  transcribed loop is taught about windows: they leave on the keys the game
  gives them, which is the transcription being right. See the section below.
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
  program its authors made. The port is `game.c` + `game.h`; `sdl_io.c`,
  `main.c`, `adlib.c` and `data.c` are boundaries **the port chose**, and
  their headers say so. `adlib.c` is the one with a claim to being a real
  translation unit: it is `adlib.dat`, which WAS a separate program.
  `data.c` holds the transcribed tables and nothing else - a thousand lines
  of bytes between two routines makes the routines harder to find and says
  nothing while you scroll past them. Every table there carries its address
  exactly as a routine does, and a table that had to be COMPUTED rather than
  read does not belong in it.
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

## Half the plates are not the level their file says

`ODDTABLE.DAT` exists and the port reads it. Image `0x019C9` tests bit 0 of
`n - 1`, and when it is set the game copies twenty-four bytes of entry
`(n - 1) >> 1` over the level record - the release rate, the three counts and
all eight skills - and thirty-two more over its NAME. So `level001.dat`
section 2 is "I have a cunning plan" with 2, 0, 5, 10, 20, 2, 2, 2 when
reached one way and "As long as you try your best" with twenty of everything
when reached the other, and both are the same terrain.

**A plate knows its own entry; a file and a section does not.** `./lemmings`
and `lemdev --rating R --number N` work it out from the plate. `lemdev --file
X --level S` cannot - it has no plate - so a comparison against a recorded run
of an odd plate has to be told with `--odd E`.

The way this shows up is the PANEL: the skill digits differ and everything
else matches, because the two sides disagree about what the numbers ARE rather
than about how to draw them. If a panel scores near zero while the play area
is exact, check the plate before anything else.

## The level index is a PERMUTATION, not an offset

`tools/record_level.py --level N` pokes **ds:0x1F6B**, the index into the
game's own running order - `rating * 30 + (number - 1)`, the plate on the
menu. `lemdev --file X --level S` names a **file and a section** directly.
Neither is the other plus anything.

`LEVEL_ORDER` in `game.c` maps the first to the second: `n = LEVEL_ORDER[i]`,
file `(n - 1) >> 4`, section `((n - 1) >> 1) & 7`. It is a genuine
permutation, so no arithmetic relates them and the table has to be read:

**And `lemdev --level` is ONE-based where `level_locate`'s section is
zero-based**, so `--file level001.dat --level 3` is section **2**. Getting
that wrong on top of the permutation lands on a level that is wrong twice
over, and the check is free: both sides print the level's NAME.

| lemdev | record_level |
| --- | --- |
| `level001.dat --level 3` "I have a cunning plan" | `--level 8` (Fun 9) |
| `level002.dat --level 4` "Watch out, there\`s traps about" | `--level 61` (Taxing 2) |
| `level002.dat --level 6` "Lend a helping hand...." | `--level 63` (Taxing 4) |
| `level000.dat --level 1` "This should be a doddle!" | `--level 30` (Tricky 1) |

Getting it wrong does not look like a mix-up. Both sides run the same
SIMULATION - the same lemmings in the same places, the panel's OUT, IN and
TIME agreeing to the character - while the terrain is a different graphics
set entirely. It scored **0 of 535 frames exact** and read as the port being
comprehensively broken; two pictures side by side settled it in a minute.

**When a comparison is total but the moving parts agree, check that both
sides are on the same level before reading anything else.**

## Two `static` definitions of one name in a file are one variable

`g_tick` already meant `ds:0x85`, the clock's frames-per-second counter, when
the nuke's own frame counter at `ds:0x1FD3` was written into the same file
under the same name. C's tentative definitions **merge**: the compiler said
nothing, and the two became a single byte the timer reloads to 17 every
second. So the double click measured its gap against something that never
went above 19, and the nuke fired or refused depending on where in the second
the press landed - which reads exactly like a fussy double-click threshold.

What found it was **counting the calls and printing the value beside them**:
400 calls to a routine whose only statement was `g_tick++`, and `g_tick`
reading 18. That pair of numbers cannot both be true of one variable, and it
took one run to get them where an hour of reading the arithmetic got nowhere.

So: when a value provably does not follow from the code that writes it,
**stop reading the code and instrument the variable**. And when transcribing a
new byte of the data segment, grep the file for the name first - this port
names things after what they do, and two different bytes doing similar jobs
will collide.


## A check must say what it exercised

Five faults in one session were the CHECK, not the port, and every one of them
produced a clean plausible number:

- `out/park.txt` parked the port's pointer at one measured spot, so 42 frames
  of level001/3 carried two cross-hairs - 56 pixels, where `compare_screen.py`
  excuses 28. Written off as "the opening's cursor" for several rounds.
- `verify_skills.py` handed `LEM_SKILLS=20` to a reference recorded without
  it, and the digger's PANEL scored **0 of 676** for as long as the case had
  existed. Its play area was 658 of 676, and only the play area was printed.
- `verify_all.sh` passed `${timed:+LEM_RES_TIMEUP=$timed}`. A shell decides
  what is an assignment BEFORE it expands anything, so all three results cases
  died on `command not found`.
- `verify_objects.py` rendered the port with plain `--raw`, which draws
  objects the DISPLAY-TIME way, and compared that against the original's
  COMPOSITION of them. Two different routines, scored against each other, and
  the difference read as the port's for two passes.
- a sweep of every level reported zero for all eighty, from a binary that did
  not have the counter compiled in. The zero was written up as a fact about
  the code path.

Four rules come out of that, and they are cheap:

- **Score the thing under test, not the picture it sits in.** `verify_objects`
  compares 243,936 pixels of which 815 are an object: `render_objects` could
  break outright and the total would fall 0.3%. Scoring the objects separately
  turns that into 56%. `compare_frame.py` already does this for the play area
  and the panel and says why - an average hides which one is missing.
- **Check the scorer against a fault it should catch.** Shift every object one
  pixel and confirm the number moves. This is the same rule `find_sprite.py`
  states for a sprite whose offset is already known, and it applies to every
  check here, not just that one.
- **A zero is a question about the instrument before it is an answer.** Count
  what the routine SAW alongside what disagreed: `0 of 0` is not `0 of
  303,085`, and only the second is a result. A check that scored nothing at
  all should say so and FAIL rather than print a percentage.
- **A comparison is only as good as the claim that the two sides are the same
  thing.** `prove.sh` gets this right and says so in its own comment: the
  original's composed buffer against the port's composed buffer. Say which two
  things are being compared, in the tool, where the next reader will see it.

A fifth of the same kind, and it is the subtlest: **the two sides have to be
in the same state, and the state includes things the screen does not show.**
`verify_menu.py` scored the menu at 728 pixels differing - one tight block,
rows 146..171 - which is exactly the FX label. Image 0x04E5A picks that label
from `ds:0x25A7` and `ds:0x2010`, the emulator answers the AdLib detect so the
guest has a driver, and `lemdev --screen menu` loads none. Neither side was
wrong. The tool read the guest's rating and its banner scroll and told the
port both; it did not read those two bytes, and the difference arrived as a
percentage. It now passes them as `--snd`.

The tell was the shape, not the size: 728 pixels in one 26x53 block is a
SPRITE, and a sprite that is wholly different rather than shifted is the wrong
sprite being chosen - which is a state question, not a drawing one.

**And the same rule caught a worse one in the same hour.** `menu_diff.py`'s
table said the BRIEFING is scored against `menu.scrn` - the main menu. So it
rendered one screen and compared it with a picture of another, and reported
**30.23%**. The two share a background tile and a palette, which is exactly
why it came out as a plausible percentage rather than as obvious nonsense;
`{(3, 0): 28313, (0, 3): 23601}` in its own value-pair histogram is two
different pictures, not a port that is 70% wrong. Pointed at
`briefing.scrn` it is 0 of 224,000.

Both were one line: the state to put the port in, and the file to compare it
with. **Say which two things are being compared, and check the table that
decides it** - a percentage from the wrong pairing looks exactly like a
percentage from the right one.

And the one that ties them together: **the port has been exact on every screen
this repository scores throughout all five.** A number that moves is not
evidence the reconstruction moved. Ask what the harness fed each side before
reading a residue as the port's.

## A ctypes struct is a SECOND declaration of the record

`tools/verify_hit.py` reported 553 of 600 frames agreeing - 209 of 256 with
the pointer over a lemming - and it read exactly like a fault in
`level_lemming_at`, which this file already records as having shipped sixteen
pixels wrong with a unit test that agreed with it. It was the tool's `Lem`.

The declaration stopped at `state`: sixteen bytes where the port's record is
thirty-four. ctypes laid the array out at its own stride, so the C read every
lemming after the first out of the middle of its neighbour. Corrected, the
same trace scores **600 of 600** and 256 of 256 over a lemming.

What settled it in one run was the rule already here about a score getting
worse: **build the port at an older commit and score the same trace.** A
commit from before any of the work the number appeared to indict gave the
identical 553/47, and a number that does not move when the code moves is a
number about the instrument.

So the struct is declared **once**, in `tools/lemmings.py`, and asserted
against `sizeof(lemming)` through `lemming_record_size()`. A field added to
`game.h` and not to there now fails on the assert rather than producing a
plausible measurement. This is the same shape as the two `static` definitions
of `g_tick`, one file further out: two spellings of one object, and the
compiler cannot see across the boundary to complain.

## A tool that will not link is a tool nobody has re-run

Eight verifiers each hand-wrote a `cc` line naming `game.c` and `sdl_io.c`.
Every source file the port has gained since broke all eight at once, and none
of them said so in terms anyone would connect to the cause: `verify_dig` died
on `undefined symbol: CRATER` from the `data.c` split, and `adlib.c` had
already broken it before that - so it had not run since sound landed.

The list of sources now lives once, in the Makefile, as `libgame.so`, and
`lemmings.game_lib()` asks make for it and checks the build succeeded.

That silence is worth naming on its own. `make test` passed throughout,
`prove.sh` passed, every frame-by-frame comparison passed - because none of
them goes through ctypes. **A check that has stopped running looks exactly
like a check that has nothing to report**, and the only thing that
distinguishes them is running it. When a source file is added to the port,
run the verifiers, not just the tests.

And when one does run again after a gap, **its number is about the whole gap,
not about your change**. `verify_dig`'s second trace reported 286 of 614
frames differing; the port is re-simulated on terrain alone, so it has no
exit to walk into, and frame 432 is where the original's digger reached one.
The comparison now stops there and says why - 328 of 328 agree up to it.

## The port compensating for something is a routine not yet read

`ds:0x4E` starts at 1 and the port set it to 4, with a comment saying so:
image 0x0031E's third branch is the timed cues, something later must set bit
2, and starting at 1 would have lost them. That comment was honest and it
sat there for weeks.

Bit 2 is set by **image 0x003C1's first instruction**, `or al, 4` - the fade
in arms the cues, on every pass of it, and clears bit 0 when the palette
stops moving. It explains the probe's own unexplained observation exactly:
`ds:0x4E` at 5 and then 4 is the fade running with the cues already armed,
and the frame after it finished.

The port reached the right state and skipped the fade to get there, and no
comparison here could see it: the fade is a palette, every check compares
indices, and the harness has no display so it never ran.

**A constant chosen to make the observed behaviour come out right is the same
thing as a fitted compositing rule** - it narrows, it improves the number, and
it is not the routine. The tell is the comment admitting it. When the port
holds a value "because otherwise X does not happen", X has a routine and
nobody has read it yet.

## Inserting anything above a function detaches its provenance

`provenance.py` reads the comment block **immediately** above a definition -
deliberately, because a window would inherit the address of whatever came
before. The cost is that adding a forward declaration, a helper, or a `#define`
between a function and its comment silently orphans it, and `make test` starts
failing for a reason that has nothing to do with the change.

It happened three times in one session and twice went unnoticed long enough to
be committed over - the second time `make test` was already at rc=2 while a
commit message claimed it passed. Grepping the tail of `make test` for FAIL
does not catch it either: the failure is a count printed by a Python script,
not a line saying "FAIL".

**Check `make test`'s exit status, not its output**, and after moving anything
in `game.c` re-run `provenance.py` before believing the build is clean.

## A comment that was true when written is not a fact about the code now

`lem_dig_start` carried a careful paragraph explaining that the digger's
store could be one of two geometries, that picking one would be fitting, and
that "so it is not drawn... a digging lemming is invisible". Every word of
that was true the day it was written. `DIG_SPR_BASE` was worked out later, by
rendering both candidates and looking, and the paragraph was never updated.

A pass through this file then read it, believed it, added a second digger
draw beside the working one, and wrote "the digger is drawn now, it was not
drawn at all before" into a commit message. The two paths measure identically
- 659 of 709 frames exact, worst 40 - so the duplicate changed nothing and
the claim was false.

**The reconstruction is the code; the comments are a record of how it got
there, and they go stale silently.** Before believing a comment that says
something is missing, grep for the thing. `DIG_SPR_BASE` was forty lines away
in the same file.

This is the same failure this file already records for `STATUS.md`'s numbers,
one level down: a figure or a claim that is cheap to re-derive should be
re-derived rather than carried forward. It applies to prose about what the
port does not do, and those are harder to notice because nothing re-runs them.

## The original's arithmetic over the port's layout is a different picture

Image 0x070A2 computes a lemming's sprite frame as its animation counter, and
image 0x070AB **adds the state** when the direction byte is not 1 - so a
lemming facing the other way draws from further along. That is a correct
reading of the original and it is wrong here, and the difference is not
subtle: applied to `lemming_draw`, three of the seven levels that are exact on
all 1,100 frames collapsed to 88, 103 and 91, with residues of 209, 275 and
305 pixels.

The original indexes its **pre-shifted store**, where the two directions sit
one after the other and `+ state` reaches the second set. This port indexes
**MAIN.DAT**, whose layout is the file's. `lemming_draw`'s own header already
said it "splits by state because it indexes MAIN.DAT rather than the
pre-shifted store" - the warning was there and the reading walked past it.

**And the port already had the rule, expressed differently.** Group 3 of the
sprite store is the climber: `CLIMB_SPR_RIGHT` is 2064 and `CLIMB_SPR_LEFT`
is 2448, which is 2064 + 8 * 48 - half way through that group's sixteen
frames. Eight frames facing one way, eight the other. The original reaches the
second half by adding the state; the port reaches it by having two bases. So
applying image 0x070AB's offset ON TOP of the port's own choice corrected the
direction twice, which is precisely what the collapse measured.

**A transcription is of a routine's MEANING, not of its arithmetic.** When the
original addresses a structure this port does not build - a pre-shifted store,
a planar page, a prepared parallel array - its index arithmetic is about that
structure and does not carry over. The tell is a formula that reads cleanly
and moves the score a long way in the wrong direction.

Keep the reading as a comment when it is disproved. It cost a measurement to
refute and the next person to find image 0x070AB should not spend it again.

## A control that never fires proves nothing, including about your own change

The same rule, put in the wrong place first: `lemming_draw` computes the frame
once at the top, and it went into a later block that only handles
burn/drown/float/splat/blast/fall/dig. The seven-level sweep then reported all
seven still exact and that looked like confirmation.

It was not. **The branch fired zero times in that run** - counted, not
assumed. This file already says a negative result is only evidence if the
positive control fired in the same run; that applies to a change you have just
made as much as to a probe. Count the executions of a new branch before
reading a sweep that did not regress as a pass.

## A byte-scan for callers finds callers that are not there

Grepping the image for `E8 rel16` whose target matches an address is a quick
way to ask "who calls this", and it is wrong often enough to have put a false
claim in a commit message here. It scans BYTES, not decoded instructions, so
a two-byte displacement that happens to compute to the target is reported
even when those bytes sit in the middle of some other instruction.

It said image 0x08FF7 was "called directly from image 0x068FA". Image 0x068FB
is a `cli`; there is no call there. What settled it was `image/codemap.json`,
which descends properly: none of the blitter's five modes is a routine head
at all, because they are **jump** targets inside it and nothing calls them.

The quick scan is still worth having - it finds jump targets a call-graph
misses, which is how the seven `jmp 0x031E5` sites that decide whether the
click sounds were found. But **cross-check anything it reports against the
code map before writing it down**, and prefer "reached from" to "called by"
unless the instruction boundary is confirmed.

It produced a second false caller in the same session, after the rule above
was written: image 0x05473 was recorded as having callers at image 0x017DD
**and** image 0x0524C, and the second is `and al, 2` inside image 0x051A2's
bit decode. What caught it was comparing every caller COUNT in a block
against `image/codemap.json` - seven of the eight matched and the eighth did
not. **The count is the cheap check**: the map carries one for every routine
head, and a disagreement points straight at the claim to re-read.

This is the same rule this file already states about instruments generally,
one level down: when a scan disagrees with the map, suspect the scan.

## A cited address is not a transcribed routine

`provenance.py` asks whether every function in the port says where it came
from. It cannot ask the other question, and the other question is the one that
shipped a bug a player found: **how much of that routine actually came
across.**

`menu_code_screen` carried `image 0x04967` and passed the check, having
transcribed the SET-UP half of that routine and none of the loop under it - so
typing a level code did nothing at all. `menu_caret_step` was worse: six lines
carrying `image 0x04C7E`, which is a 156-byte entry loop with ten cells, a
key read, backspace, Enter and a return. **A partial transcription reads
exactly like a complete one.** Both carry an address, both are counted, and
nothing looked at the size of what was claimed.

`tools/transcribed_whole.py` measures that: for every call target the code map
knows, the span up to the next one against the number of lines in the function
whose comment block claims it. Twelve bytes of original per line of C or worse
is the smell.

Two things about the instrument, and the second is the important one:

- **It is a smell, not a verdict.** A transcribed table is three lines for
  eighty bytes and perfectly complete, and a routine honestly split across
  several port functions needs the TOTAL column rather than the largest
  claimant. Read the list; do not score it.
- **The first version of it did not work, and only a control showed that.**
  It counted how many distinct addresses inside a routine the port cites, and
  by that measure `0x04C7E` scored *five* - because the port's comments
  DESCRIBED the loop in detail while implementing the blink alone. Citation
  density measures documentation. Rebuilt around function size it flags the
  case at 26 bytes per line, and stops flagging it once the loop is there.
  **Run a new check against the commit where the bug was live before believing
  it works** - this repository has that rule for scorers already, and it
  applies to a coverage tool exactly as it does to a sprite search.

The general form: **presence and depth are different measurements, and the
convention only demands the first.** When a routine has an address in the port
and behaviour is still missing, ask how many bytes that address stands for
before looking anywhere else.

## A transcribed loop has no way out, because the original owns the machine

Image 0x04C7E leaves on Enter and nothing else. Image 0x0445B leaves on one of
seven keys. That is correct - the original owns the machine, its screens end
when the game says so, and there is no window to close and no shell to
interrupt. Transcribe such a loop faithfully and the port has **no escape at
all**, which is not a bug in the transcription and is unusable anyway.

It shipped: pressing F2 entered image 0x04967's screen, the only exit was
Enter on a valid code, and SDL delivers Ctrl+C as an `SDL_EVENT_QUIT` that the
entry loop dropped along with every other key outside A-Z. The process had to
be killed.

**The fix belongs in the IO layer and nowhere else.** Threading a "has it
closed" flag through every transcribed loop puts a host concern inside
routines that are supposed to read as the game, and it has to be remembered
at every new loop - which is how it was missed here. `view_quit_now` ends the
process where it stands, every `SDL_EVENT_QUIT` site calls it, and a
`SIGINT`/`SIGTERM` handler does the same. No loop above needs to know it can
happen.

**And it must `_exit`, not `exit`.** The first version called `SDL_Quit()` and
then `exit(0)`, which runs the `atexit` handler that destroys the same
texture, renderer and window and calls `SDL_Quit()` again - one teardown over
another's freed handles. Closing the window **segfaulted**, and only closing
it: Ctrl+C went through the signal path, which was already `_exit`, so the
bug was invisible from the terminal. Two ways out of a program are two
teardowns that will differ, and the compositor reclaims a window from a
process that is gone anyway.

**And a window shows one buffer at a time, which a page flip has to be told
about.** The same F2 report said "nothing changes on screen when I type": the
flip still pointed at the menu's composed buffer while the code screen was
being typed into. The original's image 0x04C20 points the CRTC at whatever it
has just composed and cannot get this wrong; a port holding a pointer can, and
silently. `video_screen_set` is called by every screen that runs its own loop,
and `menu_free` clears it so the flip can never be left aiming at memory that
has gone.

## A pipeline's exit status is the last command's, and that is usually `sed`

`tools/verify_all.sh` printed a Python traceback and exited **0**. Four of its
checks were written

```sh
if uv run python tools/verify_triggers.py --level 77 2>&1 | sed 's/^/  /'
then :; else fail=1; fi
```

and the `if` tests **sed**, which succeeds whatever the program before it did.
`tile_sprites`, `verify_rate`, `verify_objects` and `verify_triggers` could
each crash outright and the sweep would still pass. Read `PIPESTATUS[0]`, and
prove it with the negative case - a check that exits 3 must move the count.

What it was hiding is the point. `verify_triggers.py` is the **only** thing
that scores the trigger map, which no picture and no frame comparison can
reach, and it had been dead since the tables split: it reads `LEVEL_ORDER` out
of `game.c` to say which file and section an index is, the table moved to
`data.c`, and it died on `NoneType has no attribute 'group'` every run.

So this is the same lesson as the eight verifiers that stopped linking when
`data.c` appeared, one level out. **A tool that reaches into a source file BY
NAME breaks silently when that file stops being the one** - search the sources
for what you need, and refuse outright when it is in none of them rather than
guessing. And a check that has stopped running looks exactly like a check with
nothing to report, so when one starts working again, **its number is about the
whole gap and not about your change.**

## The page is not the canvas, and a probe sees every screen

The miner's last pixel took six wrong answers, and the two that cost most were
both about WHERE a probe was looking rather than what it found.

**A byte offset means a different pixel on every screen.** `probe_writes.py`
was pointed at "canvas row 81, column 799" - which is really base `0x4A40`
plus row times `0xD0` plus a byte - and it named twelve writing instructions,
four of them inside the shared blitter. Ten were the title, the menu and the
briefing drawing themselves: `record_level.py` records the whole run, those
screens are mode `0x10` at 80 bytes a row, and the same linear offset lands
somewhere else entirely on each of them. Gated to the level, exactly two
instructions write that byte. The first answer was written up as a finding and
committed before the gate existed. **Count the game's own end-of-frame at
image `0x018F` - it runs only inside a level - and record nothing before it.**

**And the level has three buffers, not one.** The master terrain sits at
`0x4A40` with `0xD0`-byte rows and is far wider than the screen; the display
pages are at `0x02C2` and `0x27E2`, 9,504 bytes apart, which is 44 x 216 -
mode `0x0D`'s 44-byte rows. The game composes into the master and windows it
onto whichever page is hidden, then draws the lemmings **on the page**. So
nothing that draws a lemming ever touches `0x4A40`, and a probe watching the
master can only ever find the composition and the terrain-cutting skills. Ask
which buffer a routine writes before choosing a base.

**A lemming is drawn 24 columns wide, not 16**, for the same reason the
miner's masks have eight shifts: sixteen pixels at an arbitrary bit offset
span three bytes. The blit at image `0x07440` masks out the sixteen that are
really the sprite, but the background save and restore at image `0x07178` is
an unmasked 24 x 16 block - so the original's page carries three columns of
history either side of every lemming, and a pixel there can be neither terrain
nor any current sprite. That is what the residue was, and the port, which
composes from the master each frame, was right the whole time.

**Reading one plane cannot tell you a colour.** Plane 3 is the solidity bit,
so "plane 3 is clear" is true of cleared ground AND of anything drawn in 1..7.
The first solidity read here said "cleared" on frames where the screen plainly
showed attribute 2, and both were true; only reading all four planes
distinguished "the master holds nothing" from "the master holds a 2".

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
| **`tools/verify_all.sh`** | **every claim this port makes, in one command** - now including the THREE FURTHER LEVELS, which were checked only by hand until a comparison of them needed three things it could not work out for itself: the reference's ds:0x1F6B is a permutation away from the file and section lemdev takes, half the plates play under ODDTABLE's numbers rather than the file's, and the port's cursor has to be aimed at the guest's own or 56 pixels of cross-hair drown everything else. All three are in the script, so the next reader does not rediscover them: the port's own tests, `prove.sh`, all four screens on the way in, and the level frame by frame. `--full` records and compares all 5,100 frames - the level's whole expiration timer - which takes about twenty minutes and 1.5 GB. A claim nobody can re-run is a claim that goes stale |
| **`tools/compare_run.py`** | **the port against a whole recorded run**, frame by frame, play area and panel scored separately. It fits the scroll once and holds it, so a drift shows up as a score falling rather than being fitted away. `--raw` scores one static render against every frame, which is what to use while the port has no play loop - the shape of the curve is then exactly the cost of everything that moves |
| **`tools/probe_lemmings.py`** | **the original's lemming array, frame by frame** - ds:0x88, ds:0x43 records of 0x2D bytes, sampled at the play loop's own counter so a row is a frame and not a moment. It transcribes nothing; it is what a transcription of image 0x1F19 gets checked against. `--lemming N` prints one record whenever it changes, which is how the states and the animation counter were told apart |
| **`tools/record_level.py`** | **the reference sequence**: the original's level, one SCRN file per frame, captured at image 0x018F - the game's own end-of-frame, after the page swap and the five-tick wait. A wall-clock capture cannot do this and lands mid-redraw; a torn frame does not read as a mistake, it reads as a difference. Note the level is the **second** time the guest enters mode 0x0D - the first is the loading screen |
| **`tools/probe_pace.py`** | **what clock the game is running on**: the divisor it programs at `ds:0x1FC9`, the ticks it actually gets, and the page pair the ISR feeds the CRTC. The game *measures* its frame rate off the display rather than assuming one, so a wrong answer from the emulator comes back as a wrong frame rate and looks like a freeze |
| `tools/probe_isr.py` | how often a routine is entered and how many instructions run between entries, plus where the guest's time goes. Use it before blaming an interrupt for starving a loop - here it cleared the retrace ISR and sent the search somewhere else |
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
| **`tools/transcribed_whole.py`** | **is a cited routine transcribed WHOLE, or only where it starts?** `provenance.py` asks whether a function says where it came from; this asks how much of that routine came across, by weighing the span up to the next call target against the lines of the function claiming it. A partial transcription reads exactly like a complete one - `menu_caret_step` carried image 0x04C7E's 156-byte entry loop in six lines, and typing a level code did nothing until a player said so |
| **`tools/probe_codein.py`** | the access-code VALIDATOR at image 0x051A2, driven - the verdict, the level index it sets, the rating and the percentage. The codes it is driven over come from the original's own generator in the same run, because the first version built them in Python from the measured bit table and the original refused 108 of 123 |
| **`tools/verify_code.py`** | the access-code generator at image 0x05042, driven rather than read. Its ten mask-and-shift blocks are the sort of thing this port has twice mis-read by eye, so the bit table in `game.c` was **measured** - one input bit at a time - and this checks that measurement against 400 further many-bit inputs covering every rotation count, because a single-bit sweep determines a permutation only if the contributions never interact. It also prints how many codes pass the validator's checksum **and says that a wrong table passes it too**: all three deliberate faults tried here round-tripped happily |
| **`tools/compare_play.py`** | **the port and the original PLAYING the same level**, frame by frame, from one input script both sides read. Every other frame-by-frame number here came from a level nobody touches, which exercises only the part of the game that needs no player - the skills, the panel's behaviour, the cursor's box and the level's ending are all behind a click and none of them was ever scored. It also makes the CURSOR part of the comparison rather than an excused 28 pixels, because with both pointers driven from one script they are in the same place. Refuses to report a happy figure for a run where the digger was never assigned |
| **`tools/probe_cursor.py`** | the cursor's two shapes, **read out of the original's sprite store** rather than measured off a frame. Image 0x05827 puts them at 144 bytes a shift in segment `ds:0x1FEA`, and image 0x05854 adds 0x480 for the box it wears over a lemming |
| **`tools/verify_results.py`** | **the results screen, driven rather than played to**: it sets the six bytes image 0x046EB reads and calls it, stopping at the screen's own wait loop. Ninety seconds against the fifteen minutes a playthrough costs. `--step` reports the framebuffer after each set-up call, `--wipe` clears video memory first so a background that was only left over cannot pass for one the game drew |
| **`tools/verify_skills.py`** | **every skill's RECORD against the original's own** - and THE NUKE, which is not a skill: `out/nuke.txt` presses panel button 11 twice and the whole array is expected to light its fuses one a frame. It compares the FUSE where both sides carry one, because a nuke does nothing visible for seventy-nine frames and a comparison of position and flags alone would pass whatever order the cascade lit them in. In one command - the lemming array frame by frame rather than any picture. It compares EVERY lemming on screen, not only the one that was clicked: the bomber's own record was exact while another lemming walked over a hole it should have fallen into, and nothing but comparing all of them would have shown it. A skill with no reference says so rather than being skipped |
| **`tools/record_skills.sh`** | a reference run for each of the eight, one at a time. Sequential on purpose: two emulators on one machine halve each other's frame rate, and a run that falls behind its ceiling looks exactly like a level that ended early |
| `tools/snapshot.py` | capture memory at a chosen point in a run |
| `tools/probe_protected.py` | record the protected instruction stream as it runs |
| `tools/probe_desync.py` | find a single-step desynchronisation, if the decryptor ever loses step |
| **`tools/probe_rating.py`** | the rating plate, watching `ds:0x1F6E` rather than the plate's pixels - and refusing a key the control socket would not deliver |
| **`tools/verify_dig.py`** | **the digger against the original's own**, state AND terrain. The state half is nearly useless on its own and says so - reintroduce any of the three bugs found here and y and the animation still agree, because none of them changes anything visible while the digger is inside solid ground. The SHAFT is what discriminates: 1,056 pixels, and the three faults differ by 34, 14 and **2**. Reads the solidity map at image 0x05DEA's 0x4A40, which is not the start of the segment |
| **`tools/verify_hit.py`** | **the under-pointer test against the original's own answer**, frame by frame. The guest clears ds:0x1A at image 0x03DAF and sets it at image 0x03DE8 when its scan finds a lemming, so that byte settles it. Exists because the unit test for this passed while the code was sixteen pixels wrong - both were written from the same reading, and a test that shares the code's mistake agrees with it |
| **`tools/verify_pace.py`** | **how long the port's loops take**, against the rates the original sets - the level at 17 a second (image 0x0418) and the menu at 70 (image 0x01550 waits a retrace). The ONLY check here that measures duration: every other one compares frame N against frame N and cannot see a port running at four times speed, which is exactly how the level shipped at sixty. Times the port's own loops on the host, never the guest's - the emulator's throughput answers between 3 and 8 a second and that is the instrument, not the game |
| **`tools/compare_screen.py`** | **the game's own 320x200 screen** against the original's, frame by frame - the window path, not the harness. A different claim from `compare_run.py`, which scores a canvas and a panel `lemdev` composed separately: every frame of that matched while `./lemmings` showed an empty cave. Play area and panel scored SEPARATELY, and the cursor's 28 pixels named rather than averaged away |
| **`tools/verify_triggers.py`** | **the trigger map, the port's against the original's own** - the map at ds:0x12 that answers "what is under this lemming", and the only thing that does. Two passes build it, image 0x03839's STEEL and image 0x037C2's objects, and the port had the second and not the first: steel areas were parsed and never stamped, so every test against them was dead code and a bomb went through a steel floor. No picture can show this map and no frame comparison can reach it |
| **`tools/probe_attrpal.py`** | **the EGA attribute registers, per level** - the step between the attribute a routine writes and the DAC entry a capture holds. `compare_screen.py`'s map is one measurement taken on one level, and a per-level palette would make it right there and wrong everywhere else. It reports how many frames it sampled and says outright when it read nothing, because a run that never reached a level would otherwise report the menu's |
| **`tools/probe_dac.py`** | **is the palette changed part-way down the screen?** Samples the emulator's palette every 512 instructions between the game's own end-of-frame points at image 0x018F. It is a sampler and not a port hook on purpose: wrapping `m._on_out` records nothing, because Unicorn holds the bound method from construction - and "no port writes" reads as a finding |
| **`tools/compare_frame.py`** | one level frame, the original beside the port. Mode 0x0D is two things stacked - the play area at rows 0..159 and the panel at 176..199 - and they are scored SEPARATELY, because an average hides which one is missing. Fits the scroll if you do not give one, and says which it used |
| **`tools/probe_writes.py`** | who writes to video memory, and from where - every write into the EGA window attributed to the instruction that made it, `--rows` filtering by destination row and `--stride` telling it the mode's width. Use it when a screen is drawn by something OTHER than the shared blitter: the level's panel is mode 0x0D and touches 0x8822 not once, so probe_blits.py reported nothing and this found it in one run. Three env settings make it answer a question about a LEVEL rather than about a linear offset: `LEM_WRITES_BASE` (the level's canvas is image 0x05DEA's `0x4A40`, and its rows are `0xD0` not 40), `LEM_WRITES_COLS` (the byte holding a column), and **`LEM_WRITES_AFTER_FRAME`**, which records nothing until the game's own end-of-frame at image 0x018F has run N times. Without that last one it scores the title, the menu and the briefing too - ten of twelve "writers" of a level pixel were the briefing, and that got written up as a finding |
| **`tools/probe_mine.py`** | the miner's masks **and the terrain under them**, read out of the original. It checks its own premise - that keeping shift 0 of the eight pre-shifted variants is sound - and prints `0 of 364` when they are all pure shifts, so the assumption in `data.c` fails loudly if it ever stops holding. `LEM_MINE_FRAME` takes a LIST of frames: it reads the solidity map (plane 3) and the full four-plane attribute at each, because one frame lands on an agreement two times in three and says nothing while looking like an answer, and because plane 3 alone cannot tell attribute 0 from attribute 2 |
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
