# The machine-type menu

`VGALEMMI.EXE` asks two questions before it touches the display. The second is
the machine type, and it is **not cosmetic** — each answer sets flag bytes that
the rest of the program reads, and one of them patches code.

Addresses are image offsets into the unpacked image. **`DGROUP` starts at image
offset `0x09A90`**, so a `ds:XXXX` reference is image offset `0x09A90 + XXXX`.
The code segment and the image base coincide, so a `cs:` offset *is* an image
offset.

## The menu loop — image `0x1110`–`0x11B4`

The selection lives in `BL`, 0-3. Arrow keys `0x48`/`0x50` move it, `'1'`-`'4'`
set it directly, `RETURN` (`0x0D`) or `0x0A` confirms, `ESC` (`0x1B`) exits to
DOS via `INT 21h AX=4C00h` at `0x1213`.

Confirmed by reading the strings each index redraws:

| `BL` | menu entry | string at |
| --- | --- | --- |
| 0 | `1 - For PC compatibles` | `ds:0x239F` |
| 1 | `2 - For High Performance PCs` | `ds:0x23D2` |
| 2 | `3 - For an IBM PS2 Machine` | `ds:0x240B` |
| 3 | `4 - For an AMSTRAD PC1640 or PC1512 Machine` | `ds:0x2442` |

## What each choice actually does

On confirm, `0x116B` dispatches on `BL`:

**`BL=0` — PC compatibles** (image `0x1200`)
```
1200  c6060e20ff    mov byte ptr [0x200e], 0xff
1205  c6060c20ff    mov byte ptr [0x200c], 0xff
```

**`BL=1` — High Performance PCs** — falls straight through to the redraw at
`0x1194` and **sets nothing at all**. So "high performance" is the *default*
state, and the other three options each switch something on. That is a useful
orientation: the flags are all work-arounds, not features.

**`BL=2` — IBM PS/2** (image `0x11E2`)
```
11e2  c6060a20ff    mov byte ptr [0x200a], 0xff
11e7  c606c61f74    mov byte ptr [0x1fc6], 0x74     ; writes an opcode - JZ rel8
11ec  c706c91f6834  mov word ptr [0x1fc9], 0x3468
```

**`BL=3` — Amstrad PC1640 / PC1512** (image `0x11F4`)
```
11f4  c6060b20ff    mov byte ptr [0x200b], 0xff
11f9  c6060e20ff    mov byte ptr [0x200e], 0xff
```

## The flags

| flag | set by |
| --- | --- |
| `ds:0x200A` | PS/2 only |
| `ds:0x200B` | Amstrad only |
| `ds:0x200C` | PC compatible only |
| `ds:0x200E` | PC compatible **and** Amstrad |

`ds:0x200E` being shared by the two "slow/quirky" options, while High
Performance sets nothing, suggests it gates a compatibility slow-path. That is
a reading of the pattern, **not** a finding — nothing has been traced yet.

## Open — the work still to do

1. **Find every read of `ds:0x200A`, `0x200B`, `0x200C`, `0x200E`** and
   transcribe the paths they gate. This is the substance of the question "what
   is the difference between the machine types" and it is not yet answered.
2. **`ds:0x1FC6` and `ds:0x1FC9` look like a code patch**, not data: `0x74` is
   the `JZ rel8` opcode. They sit just before `ds:0x1FDA`, which holds the CRTC
   base port used by the retrace wait at image `0x1550`. The PS/2 option
   plausibly patches the retrace or video-timing path — the PS/2's VGA differs
   from a clone's — but this is a **hypothesis**; disassemble around
   `0x09A90 + 0x1FC6` and confirm what is really there before believing it.
3. Establish whether the choice is persisted (`RUSSELL.DAT` is the obvious
   candidate, and `LEMMI.BAT` resets it) or re-asked every run.

Only option 1 (PC compatible) has been exercised in a run so far. The other
three have been read, not executed.
