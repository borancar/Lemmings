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

## What this port targets

**High Performance PCs (`BL=1`).** It is the only option that sets no flags at
all, so it is the plain path: the other three each switch on a work-around for
a specific machine's quirks. Targeting it means the compatibility flags stay
zero and the code they gate is never entered, which is both the behaviour worth
having and the smallest surface to prove.

The other three machine types are therefore **deliberate non-goals**. When the
paths behind `ds:0x200A`/`0x200B`/`0x200C`/`0x200E` are eventually identified,
they belong in the port as no-ops with a comment saying why, and out of the
verifier's dispatch — comparing a no-op against the original reports a decision
as a difference.

`tools/lemmings.py` holds this as `STARTUP_KEYS = ("1", "2")`: `'1'` for VGA,
`'2'` for High Performance. No RETURN is needed — the menu confirms on a digit
(image `0x1159`: `mov bl,1` then a jump straight to the dispatch); only the
arrow keys merely move the highlight.

Only option 1 (PC compatible) has actually been *executed* so far, from before
this decision. Option 2 is now what every tool drives, but a run under it has
not yet been recorded here.

## What two of them actually change

The table above says which option sets which flag. This is the start of the
other half - what the flag *does* - read out of the image rather than guessed.

### `ds:0x200C` blanks the display while the palette is written

Image `0x0548D`, in the palette-writing routine:

```
0548d  cmp byte ptr [0x200c], 0xff
05492  je   0x54a1                  ; 0xFF: skip the blanking entirely
05494  mov  dx, 0x3c4               ; Sequencer index
05497  mov  al, 1                   ; register 1, Clocking Mode
05499  out  dx, al
0549a  inc  dx                      ; 0x3c5, its data
0549b  in   al, dx
0549c  or   al, 0x20                ; bit 5 - SCREEN OFF
0549e  out  dx, al
```

and image `0x054B6` is the other end of it, `and al, 0xdf` putting bit 5 back
after the palette has gone in. Image `0x054C6` is the same pair around a
second routine.

`ds:0x200C` is set by **PC compatible only**, and the test is `je` - so a PC
compatible is the one machine that does **not** blank, and the other three
turn the display off across the palette load. That is the opposite way round
from the guess anyone would make from the option's name, which is the reason
to read it rather than assume it.

### One of `ds:0x200E`'s twelve references is inert

Image `0x053C2`, immediately after a `rep movsw`:

```
053c2  cmp byte ptr [0x200e], 0xff
053c7  jne  0x53c9
053c9  ret
```

The `jne` displacement is **zero**, so it jumps to the instruction after
itself and both arms reach the same `ret`. The test's result is discarded.
Confirmed as a real instruction boundary by disassembling from three
different earlier addresses and watching the stream converge on it.

This project has one of these already - image `0x02CB9` is a `jns` to the
instruction after itself, in the miner's trigger handling - and both matter
for the same reason: a site that appears in an xref list looks like a use, and
this one gates nothing. Twelve direct references to `ds:0x200E` are eleven.
