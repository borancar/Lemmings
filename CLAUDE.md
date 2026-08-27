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

## The two sides number the levels differently

`tools/record_level.py --level N` pokes **ds:0x1F6B**, which is the original's
own index and **zero-based**. `lemdev --number N` is the number on the menu
plate, **one-based** within a rating. So `--level 4` and `--number 4` are
different levels, and `--level N` is `--number N+1`.

Using the same integer for both records one level and replays another. It does
not look like a mix-up in the output: it reads as the port getting the very
first lemming wrong and never recovering - 365 of 420 frames differing, with
the entrance in a different place. A whole recording went that way here.

When a comparison is wrong from the FIRST frame that has a lemming in it,
check that both sides are on the same level before reading any of it.

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
| **`tools/verify_all.sh`** | **every claim this port makes, in one command**: the port's own tests, `prove.sh`, all four screens on the way in, and the level frame by frame. `--full` records and compares all 5,100 frames - the level's whole expiration timer - which takes about twenty minutes and 1.5 GB. A claim nobody can re-run is a claim that goes stale |
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
| **`tools/probe_dac.py`** | **is the palette changed part-way down the screen?** Samples the emulator's palette every 512 instructions between the game's own end-of-frame points at image 0x018F. It is a sampler and not a port hook on purpose: wrapping `m._on_out` records nothing, because Unicorn holds the bound method from construction - and "no port writes" reads as a finding |
| **`tools/compare_frame.py`** | one level frame, the original beside the port. Mode 0x0D is two things stacked - the play area at rows 0..159 and the panel at 176..199 - and they are scored SEPARATELY, because an average hides which one is missing. Fits the scroll if you do not give one, and says which it used |
| **`tools/probe_writes.py`** | who writes to video memory, and from where - every write into the EGA window attributed to the instruction that made it, `--rows` filtering by destination row and `--stride` telling it the mode's width. Use it when a screen is drawn by something OTHER than the shared blitter: the level's panel is mode 0x0D and touches 0x8822 not once, so probe_blits.py reported nothing and this found it in one run |
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
