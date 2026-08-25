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

## What this does and does not explain

It **confirms** the port's rules from the original's own code rather than from
a published description: drawing order, that "do not overwrite" means "only
where nothing is yet", that it applies per piece rather than universally, and
that for this data OR and replace cannot be told apart.

It does **not** explain the ~19% of terrain pixels that still differ (see
`STATUS.md`). What it does do is remove the last plausible guesses: there is
no clever combining rule to discover, because the hardware's combining rule is
a no-op on terrain data.

The next thing to read is the **caller** — what selects between the blitters,
what it puts in `[0xa948]` (the sub-byte shift) and `[0x6702]`/`[0x6704]` (the
source and destination row strides), and how it clips a piece to the window.
A placement or selection difference would live there, and it is now the only
place left for one to hide.
