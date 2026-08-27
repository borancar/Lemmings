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
