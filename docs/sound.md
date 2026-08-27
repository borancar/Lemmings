# Sound: two loadable drivers on two interrupts

The game plays no digitised samples. `adlib.dat` and `tandysnd.dat` decompress
to **21-22 KB of 8086 code each** - loadable drivers the game installs on an
interrupt vector and then calls. Both begin `and ah,ah / jnz / jmp`, a
dispatcher on AH, and neither contains a PCM block: entropy is a flat 5.6-6.8
across every 4 KB, and the only long constant runs are 32-byte instrument
tables around +0x2BEA.

So a port cannot "play the samples": there are none. What it can do is
implement the two INTERFACES, which are small and completely readable.

## The two vectors

    INT 60h    the AdLib driver, adlib.dat
    INT 61h    the other one, tandysnd.dat

`AH` is the function and `AL` the argument:

    AX = 0x0100 / 0x0200   start up (INT 60h uses 1, INT 61h uses 2)
    AX = 0x0000            shut down
    AH = 3, AL = n         INT 60h: play n
    AH = 4, AL = n         INT 61h: play n

## The two gates

Every call site tests the same two bytes:

    ds:0x25A7   the sound settings. Bit 0 alone gates the INT 60h calls;
                bits 0-1 together gate the INT 61h ones, so bit 1 is a second
                switch - music against effects is the obvious reading and is
                NOT yet established.
    ds:0x2010   which driver is loaded. 0xFF selects INT 61h: image 0x02F8E
                skips the INT 61h path when it is not 0xFF, and image 0x03F7C
                skips the INT 60h path when it is.

## What the game asks for

INT 60h, three sounds only:

    0x0C   image 0x02377   the bomber's cloud - image 0x02350's first frame
    0x13   image 0x026C0, image 0x031F6, image 0x03F8D
    0x02   image 0x02F8C

INT 61h, eighteen:

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
