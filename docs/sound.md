# Sound: two loadable drivers on two interrupts

The game plays no digitised samples. `adlib.dat` and `tandysnd.dat` decompress
to **21-22 KB of 8086 code each** - loadable drivers the game installs on an
interrupt vector and then calls. Both begin `and ah,ah / jnz / jmp`, a
dispatcher on AH, and neither contains a PCM block: entropy is a flat 5.6-6.8
across every 4 KB, and the only long constant runs are 32-byte instrument
tables around +0x2BEA.

So a port cannot "play the samples": there are none. What it can do is
implement the two INTERFACES, which are small and completely readable.

## One driver, two vectors - and NOT one vector per card

A first reading here said INT 60h was AdLib and INT 61h the other card. That
is wrong, and the load sequence settles it. Image `0x00589` puts
`ds:0x2573` - "adlib.dat" - in DX, image `0x0058D` loads it, and image
`0x0059D` then does `ah = 1 / int 0x61` to ask whether the hardware is there.
`tandysnd.dat` is loaded the same way at image `0x005CA` and called on **the
same vector** at image `0x005E1`. Whichever driver is loaded owns INT 61h.

So INT 60h is a SECOND INTERFACE of the same driver, not a second card.

The driver's own dispatcher, at the head of the decompressed blob, is a chain
on AH:

    AH = 0   -> +0x115    shut down
    AH = 1   -> +0x0A3    start up, and it ANSWERS - image 0x0059F tests AL,
                          and a zero sends the game on to the next device
    AH = 2   -> +0x03A
    AH = 3   -> +0x022    `mov cs:[0xED], al`
    AH = 4   -> +0x02A    `mov cs:[0xF0], al`
    AH = 5   -> +0x032
    else     -> iret

**AH=3 and AH=4 do not play anything.** Each stores its AL in a byte the
driver's own timer ISR reads: `cs:0xED` and `cs:0xF0`. They are two REQUEST
SLOTS, and the port's job is to notice a write to either.

## The two gates

Every call site tests the same two bytes:

    ds:0x25A7   the sound settings. Bit 0 alone gates the INT 60h calls;
                bits 0-1 together gate the INT 61h ones, so bit 1 is a second
                switch - music against effects is the obvious reading and is
                NOT yet established.
    ds:0x2010   0xFF once a driver answered. Image 0x005A4 sets it when the
                AdLib detect returns non-zero and image 0x005CE sets it for
                the other; image 0x02F8E skips the INT 61h path when it is not
                0xFF, and image 0x03F7C skips the INT 60h path when it is.

## What the game asks for

INT 60h - `cs:0xED` - three only:

    0x0C   image 0x02377   the bomber's cloud - image 0x02350's first frame
    0x13   image 0x026C0, image 0x031F6, image 0x03F8D
    0x02   image 0x02F8C

INT 61h - `cs:0xF0` - eighteen, and these are the gameplay events:

    0x01   image 0x03F7A   a panel button - image 0x03E6F's click handler
    0x04   image 0x03209   in image 0x03150, the SKILL ASSIGNMENT
    0x05   image 0x02E5C
    0x08   image 0x0229E   guarded by `and bh,bh` - a lemming reaching water
    0x0A   image 0x024CE, image 0x02A68, image 0x02D0B
    0x0C   image 0x02D68, image 0x02EB0
    0x0D   image 0x030A3   just before image 0x030A6, the RELEASE
    0x10   image 0x02F9F
    0x11   image 0x0304C
    0x12   image 0x026D3, image 0x03298
    0x02   image 0x003AA   and 0x03 at image 0x00391, in the start-up
    0x00   image 0x003BE, image 0x01872   shut down

and six more at image 0x0059D..0x005F3 whose AL is not an immediate - they
load it from a register, so the id is computed and those need reading before
they can be named.

## What this means for the port

The GAME side is transcribable exactly: every site above is a place a
transcribed routine already exists or will, and `sound_play(id)` belongs
beside the routine that asks for it.

The DRIVER side is not, and cannot be without either running the original
blob or synthesising. The honest options are a synthesised approximation -
which is the port's own, not the game's, and must say so - or reading the
tone tables out of `tandysnd.dat`, whose square waves are what a PC speaker
actually produced and need no card emulated.

## The driver is an OPL2 sequencer, and this is settled from its own bytes

Decompressed (`tools/dat.py`), `adlib.dat` is 22,125 bytes and names itself:

    +0x0660  " Ad-Lib Music/FX Driver - (c) 1991 Sound Images   Tel. 061 773 4541"
    +0x00F1  "AdLib Music by Sound Images (c) 1991"

so it is bought-in middleware, not Psygnosis' own.

**The register write is at +0x0559** and is the whole hardware interface:

    push dx / mov dx,[0x8a3] / out dx,al   <- AL is the REGISTER
    in al,dx  x12                          <- the address-write settle
    mov al,ah / inc dx / out dx,al         <- AH is the VALUE
    in al,dx  x8                           <- the data-write settle

The base port is a variable at +0x08A3, loaded with 0x388 at +0x0045 and
+0x00A6. `inc dx` reaches 0x389. Nothing writes 0x38A, so this is an **OPL2
(YM3812), nine channels** and not an OPL3.

The twenty dummy reads are not a mistake. The YM3812 needs about 3.3 us after
an address write and 23 us after a data write, and an ISA read takes roughly a
microsecond, so reading the port back is how a 1991 driver waits. A port has
no bus to wait on and should drop them - they carry no state.

**The reset table at +0x006D** is 27 (reg,val) words and confirms the chip:

    80=0F 8B=0F B0=00   81=0F 8C=0F B1=00   82=0F 8D=0F B2=00
    83=0F 90=0F B3=00   84=0F 91=0F B4=00   85=0F 92=0F B5=00
    88=0F 89=0F 8A=0F   93=0F 94=0F 95=0F   B6=00 B7=00 B8=00

Eighteen operators' Sustain/Release set to the fastest release, nine channels'
Key-On cleared. That is exactly an OPL2's complement.

**The detect at +0x00A3 is the AdLib manual's own**, instruction for
instruction: reset both timers (reg 04 = 0x60), reset the IRQ flag (04 = 0x80),
read the status port; then preset timer 1 (02 = 0xFF), start it (04 = 0x21),
spin `loop` 0x12C times, read the status again; reset twice more. The card is
present iff the first read masks to 0x00 and the second to 0xC0. It returns
AL=1 or AL=0, which is what image 0x0059F tests.

## The dispatcher, read rather than summarised

    AH = 0  -> +0x115   shut down
    AH = 1  -> +0x0A3   detect; ANSWERS in AL
    AH = 2  -> +0x03A   push ds,ax,di,si,cx,dx; call +0x3AA; then silence all
                        nine channels and clear nine 0x14-byte structures at
                        +0x05AC.  The working registers say sequencer; the
                        unconditional silence afterwards says reset.  NOT YET
                        SETTLED - read +0x3AA before naming it.
    AH = 3  -> +0x022   mov cs:[0xED], al
    AH = 4  -> +0x02A   mov cs:[0xF0], al
    AH = 5  -> +0x032   mov cs:[0xEE], al     <- a THIRD slot; this file
                                                 previously gave the address
                                                 without saying what it does
    else    -> iret

So there are **three** request bytes, 0xED, 0xEE and 0xF0, not two.

## The music is IN the driver, and it is named

The built-in test program's menu is at +0x06AA and lists what the blob holds:

    ~ Lemmings ~
    A Awesome   B BeastI   C BeastII  D CanCan    E Doggie
    F Lemming1  G Lemming2 H Lemming3 I Menace    J Mountain
    K Ten Lemmings         L..U Tim1..Tim10
    "Shifted A-Q for Sound Effects"
    "No AdLib Card Connected"

Twenty-one tunes and seventeen effects, as sequenced data from about +0x0C5F
to +0x55EA. **The port does not have to synthesise the music - it has it.**
What it lacks is the thing that turns that data into register writes, and the
chip those writes go to.

## Nothing in the driver hooks a timer

There is no `out 0x40`, no `out 0x43`, no INT 21h at all, and no write into
the interrupt vector table anywhere in the code region (+0x0000..+0x0660).
The driver therefore cannot advance a tune on its own: **something must call
it periodically**, and finding what is the next question. The game already
programs the PIT (`ds:0x1FC9`, see `tools/probe_pace.py`), so its own ISR is
the first place to look.

This matters more than it sounds: if the tick is the game's, the port already
paces it correctly - 17 Hz in a level, 70 Hz in the menu - and no timer needs
emulating at all.

## INT 60h and INT 61h are ALTERNATIVES, not two faces of one driver

An earlier paragraph above says INT 60h is a second interface of the same
driver. The gates say otherwise, and they are complementary. Image 0x00262:

    cmp byte [0x2010], 0xff / je  skip     <- INT 60h runs only when NOT 0xff
    test byte [0x25a7], 1   / je  skip
    mov ax, 0x0100 / int 0x60

against image 0x02F8E, which skips the INT 61h path when 0x2010 is *not* 0xFF.
0x2010 is set to 0xFF when a loaded driver answered its detect. So:

    0x2010 == 0xFF   a driver (AdLib or Tandy) answered  -> INT 61h
    0x2010 != 0xFF   none did                            -> INT 60h

which makes INT 60h the game's own fallback - the internal speaker is the
obvious reading and is not yet established. Thirty-eight call sites in all,
ten on INT 60h and twenty-eight on INT 61h, and every one is transcribable.

## The card had to ANSWER before any of this could be read

Every run of this game here has been silent, and not because the sound was
unported: the shared emulator returned 0x00 from port 0x388, so the detect
above could never see its 0xC0, image 0x0059F concluded "no card", and the
game took its INT 60h fallback. **No part of the AdLib path had ever
executed.** `LemmingsVga` now answers the detect and logs every register
write, which is what turned an unreadable question into a capture:

    10,109 register writes over 10,478 PIT ticks
    74 distinct registers, all nine channels, rhythm mode used
    divisor 12291 = 97.08 Hz for 10,103 of them

Timer 1 is modelled as expiring immediately rather than after its real 80 us.
The driver's own delay is 300 `loop` iterations - about a millisecond on a
4.77 MHz 8088 - so it has always elapsed by the time the status is read.

**Each write is stamped with the guest's PIT tick, not a sequence number.** A
register write only means something at a moment: the same stream replayed
evenly spaced is the right notes at the wrong times. One tick is
`divisor / 1193182` seconds, and 97.08 Hz is the rate the game itself
programmed. Wall clock would measure the emulator's throughput, which
CLAUDE.md already records as the wrong instrument for pace.

## What is checked, and what is not

The chip is `reconstruct/src/opl.h` over **ymfm** (BSD-3-Clause, vendored -
see `reconstruct/vendor/README.md`). An OPL2 is hardware, so there is no
routine in the image to transcribe and nothing for the port to differ from;
`tests/opl.c` checks it against the DATASHEET instead - that f-number 408 in
block 4 sounds at 309.5 Hz, that one more block doubles it, that key-off
releases, that total level attenuates. Those are facts about a YM3812, not
about our reading of the game.

The claim that IS the original's is the register stream, and it is exact:
`tools/probe_opl.py` captures what the original emits, and a transcribed
sequencer has to reproduce it write for write. That comparison needs no
listening and has no tolerance in it - it is to sound what `prove.sh`'s
composed buffer is to the screen.

    LEM_HOOK=tools/probe_opl.py LEM_OPL_OUT=out/opl_play.txt \
        uv run python tools/play.py --auto --start -- --run-seconds 240
    ./reconstruct/lemdev --opl-replay out/opl_play.txt --wav out/opl_play.wav

So the sequencer at +0x3AA is the next thing to read, and the three request
slots (0xED, 0xEE, 0xF0) are its inputs.

## The driver, transcribed - and checked write for write

`reconstruct/src/adlib.c` is `adlib.dat`'s sequencer in C. It is about 1,200
bytes of 8086 in six runs, and recursive descent from the dispatcher plus the
**word table at +0x03EA** reaches all of it. That table is the command set:

    0x80  +0x0415  next pattern; a zero entry means loop, via +0x03FA
    0x81  +0x01C0  rest - key off, cancel the slide
    0x82  +0x01B3  tie - hold, and suppress the note-off gate
    0x83  +0x03AA  end of tune: stop every channel
    0x84  +0x0430  transpose, from the next byte
    0x85  +0x043A  end of this part: key off and go idle
    0x86  +0x0447  slide up one semitone a tick
    0x87  +0x044F  slide down

and the ranges above it are 0xB0..0xBF volume, 0xC0..0xDF instrument,
0xE0..0xFF note length. Anything below 0x80 is a note.

Nine channel structures of 0x14 bytes sit at +0x05AC, **pre-initialised in the
file** with the OPL2's operator pairs - (0,3) (1,4) (2,5) (8,11) (9,12)
(10,13) (16,19) (17,20) (18,21). The port reads that mapping out of the driver
rather than restating the datasheet, so a mistake there is a mistake about the
file.

Effects take channels 8, 7, 6 and 5, searched in that order (+0x051C), and are
dropped rather than queued if all four are busy. They also advance EVERY tick
where music channels wait for the tempo divider (+0x0182), which is what keeps
an effect at one speed whatever the tune's tempo is.

### The negative f-numbers

+0x0349 turns a note into a pitch through two 96-entry tables - +0x0AA7 gives
the block, +0x0B07 the semitone - and the semitone indexes +0x0927 with a
stride of SIXTEEN WORDS, hence the multiply by 32.

Semitones 7..11 are stored **negative**: 0xFE05, 0xFE23, 0xFE44, 0xFE67,
0xFE8B. That is not a f-number. It is a flag saying the value has already been
halved and belongs one octave up, and +0x0371's `dec bl / jge / inc bl` is
what decodes it. 0xFE05 masks to 517, and 517 in the next block is the same
pitch as 1034 in this one - which is 690 x 2^(7/12), the seventh semitone.
Transcribed in signed 16-bit, as written.

### The score

    LEM_HOOK=tools/probe_opl.py LEM_OPL_OUT=out/orig_stream.txt \
        LEM_OPL_REQ=out/orig_reqs.txt \
        uv run python tools/play.py --auto --start -- --run-seconds 240
    grep -v ' slot5 ' out/orig_reqs.txt > out/orig_reqs_game.txt
    ./reconstruct/lemdev --data lemming1.pc --adlib-script out/orig_reqs_game.txt \
        --adlib-ticks 9200 --raw out/port_stream.txt
    uv run python tools/verify_adlib.py out/orig_stream.txt out/port_stream.txt

    register and value agree : 10333 of 10333  (100.00%)
    the GAP between writes   : 10331 of 10332  (99.99%)
    worst gap difference     : 1 tick, at write #106

**The requests have to be captured too.** The driver plays what it is asked
for, so a capture of the register stream alone gives something to compare
against and no way to reproduce it. `probe_opl.py` watches the three slots
with a three-byte memory hook and writes the asking beside the answer.

Three things about that measurement are worth keeping:

- **Drop the detect before comparing.** The original's first six writes are
  +0x00A3 finding the card, and the port has no card to find. Left in, they
  offset every later write by six and the comparison scored **5.04%** on a
  stream that agrees completely.
- **`slot5` in the request log is the DRIVER's own write**, not the game's:
  +0x047F stores the tune id in +0x00EE itself. Feeding it back makes
  `start_music` take the pending-request branch and refuse to start. It is
  filtered out, and the honest fix is to record the writer's CS.
- **A code hook is not free.** Watching for the right moment with
  `UC_HOOK_CODE` cost so much throughput that the same run reached 40
  register writes instead of 10,109 - the guest never got near the music, and
  the probe reported that as the music never starting. The trigger moved into
  the OPL write path, which costs nothing.

And the residue is the instrument, not the port. Requests are stamped with the
emulator's PIT tick at the moment the guest stores the byte, and whether the
driver's tick for that count has already run is not something a tick counter
can answer. Reordering the port's requests against its tick was tried and
changed **nothing** - every request shifts equally, so the relative timing
came out identical.

## The pump is a RATE, and nothing that compares writes can see it

With the driver transcribed and 10,333 of 10,333 register writes agreeing,
the game played **chopped noise**. Both facts were true at once, and the
second is not visible to the first.

The device drains at 49,716 samples a second whatever the game is doing.
`sound_pump` is called once a frame, so:

    a LEVEL frame at 17 fps owes 2,924 samples
    a MENU  frame at 70 fps owes   710

and the first pump produced a fixed **1,024**. A third of what a level frame
owes, every frame, for as long as the level lasted. The fix is to make the
target a DEPTH rather than a quantity - top the queue up to a fifth of a
second, however many chunks that takes.

This is the same shape as the level shipping at sixty frames a second while
every frame matched: a rate is invisible to anything that scores frame N
against frame N, or write N against write N.

**The first attempt to measure it measured nothing.** `--audio-probe` drove
the real pump under `SDL_AUDIODRIVER=dummy` and watched
`SDL_GetAudioStreamQueued`. The dummy device never drains, so the queue only
ever grows: a run at THREE frames a second, which cannot possibly keep up,
reported a full buffer and zero underruns. The control failed to fail, and
that is the only reason the instrument was caught rather than believed.

It is now a simulation with no device in it - `sound_fill_bytes` split out of
the pump so the arithmetic can be exercised directly, drained one frame's
worth per frame:

    ./reconstruct/lemdev --audio-probe 10 --fps 17   never below 141 ms, 0 dry
    ./reconstruct/lemdev --audio-probe 10 --fps 70   never below 186 ms, 0 dry
    ./reconstruct/lemdev --audio-probe 10 --fps 3    runs dry 30 times, exit 1

and `make test` runs all three, the last as the control. A check that cannot
fail is not a check.

## Nothing ever asked for a tune

Effects worked and music did not, and the reason was not in the driver, the
chip or the pump. Every `sound_request` site the port had transcribed is an
EFFECT - `sound_request(4, ...)` - and **no site ever asked for a tune**.

No comparison could have found that. A request that is never made writes no
registers, so the stream comparison that scores 10,333 of 10,333 is scoring a
stream driven by a script; the game's own path never produced one.

### The routines, and why a static search could not find them

    image 0x0031E  the title's cue sequencer, called once a frame from
                   image 0x0019D; image 0x001A3 is the `inc byte [0x52]`
                   that gives it a frame count
    image 0x00380  frame 0x0F -> effect 3
    image 0x00394  frame 0x23 -> effect 2, and ds:0x55 = 0xFF
    image 0x003AD  frame 0x37 -> ask for ds:0x54 (AH=3), then run ONE tick
                   immediately (AH=0) rather than waiting for the next
    image 0x0482F  choose the tune, called from image 0x00489
    image 0x04840  the next in a seventeen-deep rotation at image 0x04877
    image 0x04866  one named by ds:0x0FE8, from image 0x04888

**Not one of these has a near CALL anywhere in the image.** image 0x0031E
JUMPS into them, so a search for callers returns nothing and says nothing.
The question had to be put to the running original - a one-instruction
UC_HOOK_CODE range on each, reading the near return address off the stack the
way probe_blits.py reads the blitter's caller. `tools/probe_music.py`.

That probe's first run reported **"none of them ran"**, which was a fact about
the instrument: `begin`/`end` are LINEAR addresses and it had been given raw
image offsets, so it watched five bytes near the bottom of memory that this
program never executes. `base = m.load_seg * 16` is the conversion, and
verify_level.py has always had it.

### The tables

    image 0x04877  06 07 08 0A 0B 04 0C 0D 0E 0F 05 10 11 12 13 14 15
    image 0x04888  01 09 03 02

Index 0 of the rotation is tune **6**, which is exactly what the original was
caught asking for. `ds:0x1F6C` is the rotation counter and only advances when
`ds:0x1F6D` is set; `ds:0x0FE8` non-zero names one of the four instead.

### The gate is now established

This file used to say bit 1 of `ds:0x25A7` was "music against effects - the
obvious reading, and NOT yet established". It is established, by the two
routines using different masks:

    image 0x00387   test [0x25a7], 3    the EFFECTS want either switch
    image 0x003AD   test [0x25a7], 2    the MUSIC wants bit 1 alone

### Checked

`reconstruct/tests/screens.c` runs the cue sequencer and watches for a write
to register **0xBD** - only the driver's `start_music` writes it (driver
+0x0504), so it witnesses a TUNE beginning rather than merely a noise, which
an effect would also make:

    no tune before frame 55                   ok
    the title starts a tune at frame 55       ok
    with no driver, nothing is asked for      ok

## Adjacency is not provenance

The port asked for sound 0x0D once per lemming leaving the entrance. The
original never does: a player heard it and said so.

The site was read as belonging to the release because it sits four
instructions above it:

    image 0x03092   cmp byte [0x2010], 0xff
    image 0x03099   test byte [0x25a7], 3
    image 0x030A0   mov ax, 0x40d
    image 0x030A3   int 0x61
    image 0x030A5   RET              <-- the routine ENDS here
    image 0x030A6   cmp byte [0x2b], 0xff    <-- the release STARTS here

That `ret` is the whole answer. The sound is the last act of the block above,
which is reached by falling through image 0x0304F - **the fire death**, the
trigger dispatch's effect 6. Sound 0x0D is a lemming burning.

And the release is silent: there is no `int 60h` or `int 61h` anywhere
between image 0x030A6 and image 0x0314F, which is one grep.

The comment asserting otherwise read perfectly well and had an address
against it, which is what makes this worth keeping. **An address proves where
a line came from, not that the routine around it is the one you think.** The
cheap check is the one that settles it here - find the routine's boundaries
before attributing anything to it, and a `ret` between the site and the
routine is decisive.

`tests/screens.c` now asserts the release makes no sound. Nothing could have
caught this before: the sound sites had never been scored against anything at
all, only read.

## Every sound site, attributed by its routine's boundaries

After 0x0D was found attached to the wrong routine, all thirty-eight sites
were re-attributed mechanically - walk back to the previous `ret` rather than
look at what is nearby:

    site       int  AH AL   routine
    0x00391    61h  04 03   0x00380   the entrance opening
    0x003AA    61h  04 02   0x00394   the trapdoor, with ds:0x55 = 0xFF
    0x003B9    61h  03 --   0x003AD   the tune, from ds:0x54
    0x02F8C    60h  03 02   0x02F3B   THE EXIT, no-driver fallback
    0x02F9F    61h  04 10   0x02F3B   THE EXIT
    0x02FF5    61h  04 --   0x02FB6   A TRAP, id from the OBJECT
    0x0304C    61h  04 11   0x02FF8   DROWNING
    0x030A3    61h  04 0D   0x0304F   THE FIRE - not the release
    0x031F6    60h  03 13   0x031C4   a skill assigned, fallback
    0x03209    61h  04 04   0x031C4   a skill assigned
    0x03298    61h  04 12   0x0320D   the digger's own guard

The exit is the clearest illustration of the shape: image 0x02F7B tests
ds:0x2010 and takes INT 60h with id 0x02 when NO driver answered, then image
0x02F8E tests it the other way and takes INT 61h with id 0x10 when one did.
Two ids for one event, and which one is asked for depends on the machine.

## The trap's sound is the OBJECT's, not the effect's

Image 0x02FE9 reads `[di+0x1b]`, where `di` is `0x17CB + index * 0x1C` - byte
0x1B of the object's own record, which is byte 0x1B of its GROUNDxO.DAT
entry. Image 0x02FEE drops the sound when it is zero.

The file agrees, and this is a check rather than a reading:

    ground0o  obj6 eff=4 snd=15   obj8 eff=4 snd=7   obj10 eff=4 snd=14
    ground2o  obj8 eff=4 snd=7
    ground3o  obj8 eff=4 snd=9    obj9 eff=4 snd=7   obj10 eff=4 snd=7
    ground4o  obj7 eff=4 snd=9    obj9 eff=4 snd=6   obj10 eff=4 snd=6

**Every object with effect 4 has a sound and no other object has one.** Five
different noises across the sets, so two traps in one graphics set differ from
each other - which no table keyed on the effect could express. The port's
parser stopped at byte 0x14 and never read it; `tests/screens.c` now asserts
the relation, because it is the premise the code rests on.

## Three or four voices IS the music

The port was thought to be dropping voices. It is not: the tune headers say
so. Reading each tune's descriptor - word, instrument table, tempo, channel
count - across all twenty-one:

    tunes with 3 channels : 15
    tunes with 4 channels : 6

and the port's own streams key on exactly those channels, never fewer. For
tune 6 the port and the original address the identical set - 0, 1, 2, 3 for
the music and 8 for an effect - with identical key-on counts:

    key-ons per channel   {0: 600, 1: 483, 2: 540, 3: 75, 8: 38}   both sides

**But only tune 6 has ever been compared against the original.** That is one
value of a parameter that takes twenty-one, and this file already records what
that is worth.

### And a WAV diff cannot settle it

Rendering both streams and comparing samples reported **99.3% differing** on
streams that agree write for write. Two causes, both the instrument:

- the original's capture still carried its detect, taken at the 18.2 Hz
  divisor a PC boots with, so the replay rendered fifteen seconds of silence
  in front of the music
- the port's log hard-codes divisor 12291 where that run measured 12261, and
  a 0.24% difference in the tick length shifts every sample

Audio that sounds identical differs everywhere under a sample compare. **The
register stream is the instrument for this; a waveform is not.**

## Which tune a level gets

`music_choose` (image 0x0482F) reads two bytes, and the port had neither.

**ds:0x0FE8 - the level's EXTENDED graphics set**, its header's +0x1C. Image
0x01A26 and image 0x03742 read the same byte to load a different file and
build the terrain a different way, which is what identifies it: non-zero means
one of the four VGASPEC levels, and image 0x04866 gives them their own tunes
from image 0x04888 - `01 09 03 02`. That is why the by-number table has
exactly four entries.

**ds:0x1F6D - the advance flag**, and it is what makes each level's music
differ from the last. Image 0x04720 compares the rescued percentage against
the requirement:

    pass  image 0x04729  ds:0x1F6D = 0xFF, and image 0x0472E advances the
                         LEVEL INDEX in the same breath
    fail  image 0x04778  ds:0x1F6D = 0
    cold  image 0x0454C  ds:0x1F6D = 0, in the block that wipes 0..0xFCA

So passing a level steps the rotation and failing one replays the same tune
with the same level. The port had neither, chose index 0 every time, and
played tune 6 on every level of the game:

    level 1  rotation 0  tune  6  Lemming1
    level 2  rotation 1  tune  7  Lemming2
    level 3  rotation 2  tune  8  Lemming3
    level 4  rotation 3  tune 10  Mountain
    level 5  rotation 4  tune 11  Ten Lemmings
    level 6  rotation 5  tune  4  CanCan

The first level was right by accident - index 0 is where a cold start begins.

## The LIVE path, measured at last

Everything scored before this was either the driver alone or the pump's
arithmetic. The code BETWEEN them - `sound_build_chunk`, ticking the driver
where the tick falls and rendering around it - had never been run by anything
but the game. `lemdev --adlib-live` runs it with no audio device and traces
what reaches the chip:

    register and value agree : 10231 of 10231  (100.00%)
    the GAP between writes   : 10228 of 10230  (99.98%)

**Its first reading was 93.85% on the gaps, and that was the instrument.** The
log stamped each write with a tick DERIVED from the sample count, which
quantises every one of them to the 1024-sample chunk the audio happens to be
built in - about two driver ticks. Asking the driver for its own count
(`adlib_tick_count`) took it to 99.98%.

The residue is one gap of 17 ticks at write #106, where the music starts. The
original passes 199 PIT ticks between level frame 15 and frame 55 - 4.975 a
frame, which is the five this project already documents - and the port's
LEVEL_FPS of 17 against a 97.08 Hz PIT gives 5.71. **So the level's frame rate
and the PIT rate disagree by about 13% and one of the two numbers is wrong.**
That is not settled here and should not be guessed at: it needs
`tools/probe_pace.py` run against a level.

## Four voices, and every one of them sounds

"Instruments are missing" was measured rather than argued about, three ways,
and all three say the port plays what the original wrote.

**Which channels sound.** Take the port's live stream, suppress every
channel's key-on but one, render, and look at the peak:

    ch 0  8080    ch 3  2044    ch 6  0
    ch 1  7856    ch 4     0    ch 7  0
    ch 2  4088    ch 5     0    ch 8  5200

Five channels sound - four for the music, one for the effects - and the four
silent ones are silent because tune 6's header declares FOUR channels.

**Which voices are loaded.** The distinct feedback/connection bytes written
per channel, which is the patch actually in use:

    ch 0  original [1]                port [1]
    ch 1  original [4, 8, 9, 13, 14]  port [4, 8, 9, 13, 14]
    ch 2  original [3, 8, 9, 13, 14]  port [3, 8, 9, 13, 14]
    ch 3  original [8]                port [8]
    ch 8  original [0, 11]            port [0, 11]

Identical. Channels 1 and 2 change instrument 147 and 296 times across the
tune, so the arrangement is rich in TIME rather than in voices.

**And the descriptor's first word is dead.** Each tune begins with a word the
driver stores at +0x05A8 - 0x4000, 0x4200, 0x4312 or 0x4400 - and **nothing
in the driver ever reads it back**. It is not a channel mask or a voice count,
which is what it looks like.

So if the game still sounds thin, the music is not where it is thin. **Nine
effect sites are still unported**, and the code map places them exactly:

    0x01F19  the lemming walk      0x0229E=08  0x024CE=0A  0x026D3=12
                                   0x026C0=13 (INT 60h)
    0x0283B                        0x02A68=0A  0x02D0B=0A  0x02D68=0C
    0x02E04                        0x02E5C=05  0x02EB0=0C
    0x03150  a skill assigned      0x03298=12  0x031F6=13 (INT 60h)
    0x03E6F  the panel             0x03F8D=13 (INT 60h)

A level with no splat, no "oh no" and no trap-object noises is a quieter level
than the original, and none of that is the music.

## An independent chip: DOSBox

Every audio comparison here went through ymfm at both ends. Our emulator has
no synthesiser - it logs register writes - so "the emulator's WAV" and "the
port's WAV" are the same chip fed two streams. **That settles the DRIVER and
says nothing about the CHIP.**

`tools/reference_dosbox_audio.sh` records DOSBox playing the same level with
its own OPL emulation and its own copy of the game, so it agrees with neither
by construction. It reuses `reference_dosbox.sh`'s input script - xvfb plus
xdotool for the adapter menu, the machine menu, F1 and the briefing's held
mouse button - and captures with SDL2's `disk` audio driver, which writes
exactly what the program sends to the device and needs no capture key.

**The raw is 32-bit FLOAT, not signed 16-bit.** dosbox-staging's mixer works
in float and asks SDL for AUDIO_F32SYS. Reading it as s16 does not fail, which
is the trap: it produced a file whose every five-second RMS was 18,900 - flat
to four digits for four minutes - with one channel a slow waveform and the
other noise, which is what the two halves of a float look like. A flat RMS is
not music, and that is what gave it away. `tools/dosbox_audio_wav.py` converts
it properly and refuses a capture with less than five seconds of sound.

So there are now three recordings of the same level and they are not the same
claim:

    A  our emulator's register stream, through ymfm
    B  the port's own stream, through ymfm         A vs B tests the DRIVER
    C  DOSBox, its own OPL, its own game           C vs B tests the CHIP

## The missing output capacitor

Comparing the port against DOSBox by SPECTRUM rather than by ear found a real
defect, and it is not in the driver.

An OPL2's three added waveforms are RECTIFIED - half-sine, absolute sine,
quarter-pulse - so every voice using one carries a positive offset
proportional to its envelope. Nine channels' offsets swell and decay together
and the sum wanders at a few hertz.

**43.3% of the port's total audio power was in 0-4 Hz.** DOSBox had 0.0%.

A real AdLib never emits that: the YM3014B feeds an op-amp through a coupling
capacitor and the card's output is AC. ymfm models the CHIP and hands back the
raw sum, offset and all, which is correct of it. One pole at about 5 Hz in
`opl_render` takes 0-4 Hz from 43.3% to 2.2% and leaves everything above 20 Hz
alone to within a hundredth of a decibel.

Inaudible on its own. What it did was ride the music up and down on a slow
swell, eating the headroom - the port's RMS was a quarter of DOSBox's at a
similar peak.

## The DOSBox reference is not trustworthy YET

Three instrument failures in a row, each of which produced a plausible file:

- **The raw is 32-bit FLOAT.** dosbox-staging's mixer asks SDL for
  AUDIO_F32SYS. Read as s16 it gave a five-second RMS of 18,900, flat to four
  digits for four minutes, one channel a slow waveform and the other noise.
- **The master compressor is ON by default** and flattens exactly the loudness
  envelope every comparison here was correlating. `compressor=off`.
- **`SDL_DISKAUDIODELAY=0` drains the mixer faster than the guest fills it.**
  The first run produced 350 seconds of audio in about 180 of wall clock.

After all three, DOSBox's recording still has a nearly constant RMS -
1863..2097 over two minutes - which music does not, and it correlates 0.13
with the port at any lag. Meanwhile:

    A our emulator vs B port    0.674   the same music, aligned at zero
    A our emulator vs C dosbox  0.068
    B port         vs C dosbox  0.044

**So the port matches our emulator and both differ from DOSBox.** Which of the
two emulators is right is NOT established here, and the fourth instrument
failure is at least as likely as a fourth finding. What would settle it is
DOSBox's own register stream, which needs a build that can log port writes -
not another recording.

## DOSBox is right, so the fault is UPSTREAM of the port

Taking that as given, the chain is:

    the port  ==  our emulator's run     10,970 of 10,970 register writes
    our emulator  !=  DOSBox             0.068 envelope correlation

**So the port is faithfully reproducing a run that is itself wrong**, and no
amount of work on `adlib.c` can fix it. Every check this project has - the
stream comparison, the live-path trace, the tests - compares the port against
our emulator, and all of them pass.

### What has been ruled out

- **Not the level.** DOSBox's panel reads rate 50, ten diggers and nothing
  else, TIME 4-57: Fun 1, the same level.
- **Not DOSBox falling back to another device.** With `oplmode=none` DOSBox
  produces complete silence - RMS 0, peak 0 - so the OPL is what is making
  the sound there.
- **Not the tune selection.** The rotation and the special-set pick are
  transcribed now, and a cold start is index 0 on both sides.

### What it points at

The pitch content is close and the timing is not:

    pitch-class profiles, cosine similarity   0.857   (same key, same notes)
    loudness envelopes, any lag               0.044   (never aligns)
    best over tempo ratios 0.40..2.60         0.288 at ratio 1.91

Similar notes that never line up is what a TEMPO difference looks like, and
1.9 is close enough to two to be worth reading as one. It also agrees with a
discrepancy already recorded above: the original passes 4.975 PIT ticks per
level frame while LEVEL_FPS of 17 against a 97.08 Hz PIT gives 5.71.

`ADLIB_TICK_HZ` is a MEASUREMENT of our emulator (96.6-97.7 Hz across four
runs), not a fact about the game. If our emulator's display timing makes the
game program the wrong PIT divisor, then the music, the level and everything
else paced off IRQ 0 run at the wrong speed - and the port, which was given
that measurement as a constant, inherits it exactly.

### What would settle it

DOSBox's own register stream. `tools/reference_dosbox_opl.sh` is written and
drives the game to a level, but no `.dro` appears: dosbox-staging has the
`caprawopl` handler and "Rec. OPL" label, and neither Ctrl+F7 (its
PRIMARY_MOD default) nor three other candidates produced a file - one of them
started a video capture instead. The binding for this build is not yet known.

Failing that: `tools/probe_pace.py` against a LEVEL, which reports the
divisor the game programs and the ticks it actually gets. That is the number
ADLIB_TICK_HZ and LEVEL_FPS both rest on, and neither has been checked
against a level.

## The port's music runs 1.9x too fast

Settled against DOSBox, and the reference is now three independent runs:

    port      vs user's DOSBox   (8k cycles)   ratio 1.920
    port      vs ours            (8k cycles)   ratio 1.900
    port      vs ours           (25k cycles)   ratio 1.905
    user's    vs ours           (25k cycles)   ratio 0.990   <- they agree

**Tripling DOSBox's CPU changed nothing** - not the music (0.990) and not the
game clock, which read 0.82x real time at 8,000 cycles and 0.81x at 25,000.
So DOSBox is not CPU-starved, it is self-consistent, and the 1.9 is real.

    04-level.png  TIME 4-56        05-end.png  TIME 3-42
    74 game-seconds across ~91 real ones

### Where it comes from

`ADLIB_TICK_HZ` is **a measurement of our own emulator**, not a fact about the
game: `probe_pace.py` reports the game programming divisor 12268 = 97.3 Hz
there, and every capture in this file measured 96.6-97.7. The port was handed
that number as a constant.

The game derives the divisor itself, at image 0x1602: it waits for a vertical
retrace, programs channel 0 to 0xFFFF, counts `bx` display-enable cycles on
port 0x3DA bit 0 (image 0x1624 and image 0x1629 wait for each edge), then
latches the counter. So the divisor is a direct function of the HORIZONTAL
rate the adapter reports - and ours reports 31.469 kHz.

For DOSBox's game to end up ~1.9x slower it must be measuring roughly half
that. Which of the two is right is not established here.

### And there are TWO rate errors, not one

    the music   1.9x  fast against DOSBox
    the clock   1.22x fast against DOSBox   (17 fps against DOSBox's ~13.9)

They differ, and that is itself the finding: **the port paces the level and
the driver independently** - `LEVEL_FPS` for one, `ADLIB_TICK_HZ` for the
other - where the original derives both from IRQ 0 and cannot have them
disagree. Fitting either constant to its ratio would leave the structure
wrong and the two free to drift apart again.

The fix is to pace the driver off the level's own clock, as image 0x0019D
does, and to settle the timer's rate from image 0x1602's measurement rather
than from ours. Neither is a constant to tune.
