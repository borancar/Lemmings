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

## What this does and does not explain

It confirms the port's rules: drawing order, the meaning of "do not
overwrite", and that pixels are replaced rather than combined. It does **not**
explain the ~19% of terrain pixels that still differ (see `STATUS.md`), and
nothing here suggests where that comes from. The next thing to read is what
sets up `[0xa948]` and the source pointer for each piece — the caller of this
routine, which is where a placement or piece-selection difference would live.
