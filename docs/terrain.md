# How the original composes terrain

Read from the decrypted image, and located by asking the machine rather than
by guessing: `watch 0x72720 0x79e00 r` over the control socket, while the game
loaded level 1, reported four read sites with **identical** counts of 20 560 —
one per bitplane. That is the compositor.

Addresses are image offsets into the decrypted image
(`tools/decrypt_image.py`); `ds:` references are `DGROUP + offset` with DGROUP
at image `0x09A90`.

## It composes into VIDEO memory, not into RAM

```
08bab  b800a0      mov ax, 0xa000
08bae  8ec0        mov es, ax
```

This is why the composed level cannot be found in main RAM, and it is worth
saying plainly because a lot of time was spent looking there: `ds:0x1FE4` holds
the segment of a *source* buffer, and searching that buffer for the level
finds nothing. The destination is the video aperture.

Nor is a full 1584-pixel-wide level kept anywhere. Video memory holds two
pages at a **44-byte stride** — 352 pixels, the 320-pixel window plus a byte of
slack each side for smooth scrolling — and the game composes the visible window
into them. Dumping all four planes at seven points during loading and looking
for the port's level shape at a 198-byte stride (1584 pixels) never rises above
71%, at any offset, in any of them.

## "Do not overwrite" is done by the hardware

The flag is not tested per pixel in software. The Graphics Controller is set
up for **read mode 1** — colour compare — against plane 3 alone:

```
08b83  b80708      mov ax, 0x0807      ; GC 7, colour don't care = 0x08
08b8b  b80208      mov ax, 0x0802      ; GC 2, colour compare    = 0x08
```

so a read of video memory returns one bit per pixel: *is this pixel already
solid terrain?* The inner loop then turns that into the write mask:

```
08bd1  fa          cli
08bd2  268a25      mov ah, byte ptr es:[di]   ; 8 pixels: already solid?
08bd5  f6d4        not ah                     ; -> where there is NO terrain
08bd7  b008        mov al, 8                  ; GC 8 = bit mask
08bd9  36a30e00    mov word ptr ss:[0xe], ax
08bdd  ef          out dx, ax                 ; mask = "empty pixels only"
08bde  ac          lodsb                      ; the source byte
08bdf  aa          stosb                      ; written through the mask
08be0  e2f0        loop 0x8bd2
08be2  fb          sti
```

Eight pixels per iteration, and the hardware drops the writes that would land
on existing terrain. The port does the same thing per pixel, testing the
canvas rather than plane 3 — equivalent while terrain only ever uses colours
8-15, which it does.

## Sub-byte placement is a shift loop, not addressing

A piece can start at any x, so the source row is copied to a scratch buffer at
`ds:0x6706`, given a zero byte to spill into, and shifted right by `x & 7`:

```
08b9d  8d3e0667    lea di, [0x6706]
08ba1  f3a4        rep movsb                  ; the row into scratch
08ba3  32c0        xor al, al
08ba5  aa          stosb                      ; one spare byte to shift into
08bb4  a048a9      mov al, byte ptr [0xa948]  ; how many bits
08bbb  d02c        shr byte ptr [si], 1       ; a multi-byte right shift
08bc0  d01c        rcr byte ptr [si], 1       ; carried between bytes
08bc5  e2f9        loop 0x8bc0
08bcb  75ea        jne 0x8bb7                 ; once per bit
```

`shr` then `rcr` through successive bytes is a shift of the whole row by one
bit, repeated `[0xa948]` times. A per-pixel renderer gets this for free, which
is why the port does not need it.

## Four passes, one per plane

The routine is not one blit but four, each writing a single plane through the
sequencer's map mask, with the source advanced to the next plane between them:

```
08b59  b80201      mov ax, 0x0102     ; map mask = plane 0
08c07  b80202      mov ax, 0x0202     ; map mask = plane 1
                   ...                ; and so on for 2 and 3
08bff  260336f866  add si, es:[0x66f8]; source -> the next plane
```

which is why watching the terrain bitmaps reported **four** read sites with
identical counts.

## The OR turns out to be a no-op, and that matters

`GC 3 = 0x10` selects the OR function, so each write is `source OR latch`, and
the loop writes every byte of the row without ever testing whether the source
pixel is transparent. That looks like it should smear colour into the gaps -
and it is exactly what a per-pixel renderer would get wrong.

It does not, because of what terrain data contains. A terrain pixel is either
**0** - all four planes clear - or **8-15**, with plane 3 set. There is no
value with colour bits and no mask bit. So in a transparent area the source
byte is zero in every plane, and OR-ing zero changes nothing.

The consequence is worth stating: for this data, `source OR latch` under a
"not already solid" mask is **indistinguishable** from writing solid pixels
where the canvas is empty. The port's per-pixel rule is equivalent, and this
was checked rather than argued - implementing the hardware rule literally
(`--terrain hw`) scores 66.09% against DOSBox where the port's own rule scores
79.15%, because applying the mask to *every* piece is what that would mean,
and only some pieces carry the flag.

## There is more than one blitter

That last point is the resolution. Watching reads reported other sites besides
these four - `0x08A1D`, `0x08A8B`, `0x08AF6` at 6 134 each and `0x0895D`,
`0x0896F` at 6 092 - so the game has several terrain blitters and picks one per
piece. The routine documented here is the **masked** variant, used for pieces
with "do not overwrite"; the others do not set up read mode 1 and simply write.

Which is what the port already models with its flag test, and is why treating
every piece as masked measures worse.

## The caller: how a level entry becomes a blit

Found by arming a breakpoint inside the blitter and reading the stack — a far
return address `1010:7CBA` sits there, CS above offset. `finish` could not walk
the frame, because this code does not keep a BP chain.

The dispatcher at image `0x07C67` decodes one terrain entry and calls the
blitter. It confirms, from the original's own instructions, everything the port
had taken from a published description:

```
07c67  56          push si
07c68  8bd0        mov dx, ax        ; the entry's first word
07c6a  b107        mov cl, 7
07c6c  d3ea        shr dx, cl
07c6e  81e2c000    and dx, 0xc0      ; bits 13,14 of the word -> dx bits 6,7
07c72  83ca0f      or  dx, 0x000f
07c75  f7c24000    test dx, 0x40     ; bit 13 = 0x20 = ERASE
07c79  7404        je  0x7c7f
07c7b  81ca000f    or  dx, 0x0f00
07c7f  d1d0        rcl ax, 1         ; bit 15 = 0x80 = DO NOT OVERWRITE
07c81  7304        jae 0x7c87
07c83  81ca0080    or  dx, 0x8000
07c89  8bea        mov bp, dx        ; the mode word the blitter takes
07c8b  8bd3        mov dx, bx
07c8d  83e23f      and dx, 0x3f      ; TERRAIN ID: six bits
07c9a  b80800      mov ax, 8
07c9d  f7e2        mul dx            ; id * 8 -> the terrain table
07c9f  050b1d      add ax, 0x1d0b    ; its base in DGROUP
07ca2  8bd8        mov bx, ax
07ca4  8a17        mov dl, [bx]      ; width
07ca8  8a4f01      mov cl, [bx + 1]  ; height
07cab  8b7702      mov si, [bx + 2]  ; image_loc
07cae  8b7f04      mov di, [bx + 4]  ; mask_loc
07cb3  b504        mov ch, 4         ; four planes
07cb5  9a22881010  lcall 0x1010:0x8822
```

So, confirmed against the binary rather than against a document:

| the port assumes | the original does |
| --- | --- |
| terrain id is the low **6** bits | `and dx, 0x3f` |
| flag `0x80` is "do not overwrite" | bit 15 of the word, `or dx, 0x8000` |
| flag `0x40` is "upside down" | bit 14, into dx bit 7 |
| flag `0x20` is "erase" | bit 13, `or dx, 0x0f00` |
| the ground table is **8** bytes per entry | `mov ax, 8` / `mul dx` |
| in the order width, height, image_loc, mask_loc | `[bx]`, `[bx+1]`, `[bx+2]`, `[bx+4]` |
| a bitmap has **four** planes | `mov ch, 4` |

## The loop: how the terrain list is walked

Above the dispatcher, at image `0x0374F`, and it settles the byte order:

```
0374f  beeb10      mov si, 0x10eb    ; the terrain array in DGROUP
03752  8a24        mov ah, [si]      ; and the word is assembled
03754  8a4401      mov al, [si + 1]  ;   HIGH BYTE FIRST
03757  8a7c02      mov bh, [si + 2]
0375a  8a5c03      mov bl, [si + 3]
0375d  83c604      add si, 4         ; four bytes per entry
03760  3dffff      cmp ax, 0xffff    ; terminator: the FIRST WORD alone
03763  7405        je  0x376a
03765  e8ff44      call 0x7c67
03768  ebe8        jmp 0x3752
```

Assembling a word a byte at a time, high first, is not what an 8086 word load
does — so the **big-endian** layout is a deliberate choice in the game, an
Amiga inheritance carried into the PC data, and not an artefact of anything.

And just above it, the viewport's origin:

```
0372d  8a6518      mov ah, [di + 0x18]   ; start_x, big-endian again
03730  8a4519      mov al, [di + 0x19]
03733  051000      add ax, 0x10          ; +16
03736  a30000      mov [0], ax
03739  d1e8        shr ax, 1             ; and /8 for a byte offset
```

The `+16` is the same 16 that a terrain entry's x is stored with. In the
game's coordinates a piece sits at its raw x and the viewport opens at
`start_x + 16`; the port subtracts 16 from both, so its canvas offset for the
viewport is `start_x` — which is exactly the **624** the pixel comparison
independently finds as the best alignment. The two arithmetics agree.

### One real difference, found and fixed

The original stops on the **first word** being `0xFFFF`. The port required all
four bytes to be `0xFF`, which would read past the end of the list for a level
whose terminator carried anything else in its second word. No shipped level
here does, so it changes nothing measurable — but it was wrong, and matching
the original costs nothing.

## What this does and does not explain

It **confirms** the port's rules from the original's own code rather than from
a published description: drawing order, that "do not overwrite" means "only
where nothing is yet", that it applies per piece rather than universally, and
that for this data OR and replace cannot be told apart.

It does **not** explain the ~19% of terrain pixels that still differ (see
`STATUS.md`). What it does do is remove the last plausible guesses: there is
no clever combining rule to discover, because the hardware's combining rule is
a no-op on terrain data.

The caller has now been read too, and it confirms the entry decode rather than
contradicting it. Every part of the port's terrain handling that can be
reached this way has been checked against the original's own instructions and
agrees.

So the residual has no remaining explanation at this level of analysis, and it
is worth being plain about that rather than inventing one. What has **not**
been read: the routine that computes the destination address and the sub-byte
shift `[0xa948]` from the entry's x and y, and whatever clips a piece to the
window. That arithmetic is the last unexamined step between a level entry and
a pixel.
