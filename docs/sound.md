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
