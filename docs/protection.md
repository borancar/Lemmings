# The protection in VGALEMMI.EXE

What is actually in the binary, argued from bytes. Addresses are **image
offsets** into the *unpacked* image (the PKLITE stub's output), unless written
`seg:off`.

## Layer 1 — PKLITE

`vgalemmi.exe` is packed with PKLITE (banner at file offset 0x1E, `03 31` at
0x1C). `cgalemmi.exe`, `tgalemmi.exe` and `install.exe` are too. The EXE header
is unremarkable except that `CS = 0xFFF0`, which is the usual PSP-relative
idiom: `load_seg + 0xFFF0` wraps to `load_seg - 0x10`, so `CS:IP` = PSP:0x0100
= the first byte of the image.

**Confirmed:** the stub relocates itself to near the top of the program's block
and decompresses backwards, then walks a relocation table grouped per 64 KB
segment (`add word ptr es:[di], bx` with `bx` = the load segment, `dx` advanced
by 0xFFF per group, `0xFFFF` as the end marker).

**Confirmed:** it will not run at an unrealistically low load segment. At
`psp_seg = 0x100` the stub computes `bx = psp + 0x3A + 0x7FA = 0x0934`, then
`sub bh, 0x10` borrows below zero to `0xF934`; with `SI = 0xFFFE` that reads
`0x10933E`, past 1 MB. It decompresses zeros. Real DOS never loads that low, so
this project loads at `psp_seg = 0x1000`. See `CLAUDE.md`.

## Layer 2 — trap-flag self-decryption

**Confirmed by measurement**, not inferred:

- `IVT[01h]` points at image **`0x00DFF`**.
- At the moment the handler is entered, `EFLAGS = 0x0146` — **TF set**, and the
  instruction stream contains no `int 1`. The cause is the trap flag.
- The protected stretch is a short stub, not the whole program: the recovered
  plaintext is 651 bytes across nine spans (see *Reading it* below).

### Correction (2026-08-25): the trap-flag layer is the FLOPPY path

This section used to read as though the protection runs on every start-up, on
the strength of counting 108 `INT 01h` events in one run. It does not, and the
difference is one byte.

The game asks DOS for the default drive's media descriptor (INT 21h AH=1Bh) and
branches on the answer. A **differential run** — identical in every other
respect, only the media byte changed:

| media byte reported | `INT 01h` events in a start-up run |
| --- | --- |
| `0xF8` — fixed disk | **0** |
| `0xF9` — floppy | **2945** |

On a hard disk the handler is never installed at all: `IVT[01h]` reads
`0000:0000` for the whole run. The 108-event measurement was taken back when
the emulator did not answer AH=1Bh, so the guest read whatever `DS:BX` happened
to hold and took the floppy branch by accident. Nothing about the handler's
*logic* below was wrong; the claim about *when it runs* was.

This matters twice over. It is why the game reaches the first level without any
of this being defeated — the protection is simply not on that path. And it is
why recovering the code needs a deliberate floppy run: `tools/decrypt_image.py`
records one with `media=0xF9` for exactly this reason, and an ordinary run
recovers none of the nine spans.

The floppy branch is corroborated by what the recovered code does. Image
`0x00913` reads `es:[0x78]` and `es:[0x7a]` — the **INT 1Eh vector**, i.e. the
Diskette Parameter Table — copies 11 bytes of it, and patches them. That is a
routine about floppy geometry, not about disks in general.

The handler decrypts the *next* instruction in place, keyed on the word before
it:

```
0e1a  8bf4        mov si, sp
0e1c  36c57406    lds si, ptr ss:[si + 6]   ; DS:SI = the return address in the
                                            ; interrupt frame = next instruction
0e20  8b44fe      mov ax, word ptr [si - 2] ; key = the preceding, decrypted word
0e23  f7d0        not ax
0e25  d1c0        rol ax, 1
0e27  3104        xor word ptr [si], ax     ; decrypt in place
```

and separately checks the INT 00h vector's target for an `0xCC` byte — an
anti-debugger test for a planted `INT 3`:

```
0e07  c5360000    lds si, ptr [0]           ; the INT 00h vector, as a pointer
0e0b  8b44fe      mov ax, word ptr [si - 2]
0e0e  f7d0        not ax
0e10  d1c0        rol ax, 1
0e12  3304        xor ax, word ptr [si]
0e14  8704        xchg word ptr [si], ax
0e16  3ccc        cmp al, 0xcc              ; was it an INT 3?
```

### It DOES re-encrypt behind itself — a one-instruction plaintext window

**Retraction (2026-08-25).** This section previously claimed the opposite, on
the strength of comparing image `0x00600..0x00A00` at instruction 2,000,000 vs
3,000,000 vs 4,000,000 and finding 0 of 1024 bytes changed. That measurement
was real but **proved nothing**: at those instruction counts the guest is still
in ordinary start-up and neither region was under single-step protection at the
time. An idle region not changing is not evidence that a protected region does
not change. The claim was wrong and anything resting on it should be re-checked.

The disassembly settles it. The full handler:

```
0e00  50          push ax
0e01  56          push si
0e02  1e          push ds
0e03  33c0        xor ax, ax
0e05  8ed8        mov ds, ax
0e07  c5360000    lds si, ptr [0]        ; the PREVIOUS instruction's address,
                                         ; stashed at 0000:0000 last time round
0e0b  8b44fe      mov ax, word ptr [si - 2]
0e0e  f7d0        not ax
0e10  d1c0        rol ax, 1
0e12  3304        xor ax, word ptr [si]
0e14  8704        xchg word ptr [si], ax ; <-- RE-ENCRYPTS it
0e16  3ccc        cmp al, 0xcc           ; unless it was a planted INT 3
0e18  741c        je 0xe36
0e1a  8bf4        mov si, sp
0e1c  36c57406    lds si, ptr ss:[si + 6]; the NEXT instruction, from the frame
0e20  8b44fe      mov ax, word ptr [si - 2]
0e23  f7d0        not ax
0e25  d1c0        rol ax, 1
0e27  3104        xor word ptr [si], ax  ; <-- decrypts it
0e29  1e          push ds
0e2a  33c0        xor ax, ax
0e2c  8ed8        mov ds, ax
0e2e  89360000    mov word ptr [0], si   ; remember it, to re-encrypt next time
0e32  8f060200    pop word ptr [2]
0e36  1f          pop ds
0e37  5e          pop si
0e38  58          pop ax
0e39  cf          iret
```

Exactly **one instruction is in plaintext at any moment**, and `0000:0000` holds
a far pointer to it. Consequences, and they shape the whole project:

- **Static disassembly of a snapshot is invalid for any region while it is
  under protection.** A snapshot catches one plaintext word and scrambled
  neighbours.
- The code map for protected stretches has to be **recorded as instructions
  execute**, not read out of memory.
- Regions the protection has finished with, or never covered, disassemble
  normally — which is why the start-up code and the menus read cleanly.

`tools/probe_selfmod.py` compares a range at two points in a run. It answers
"did this region change between these two moments", which is only evidence
about protection if the region was **actually being single-stepped in between** —
the mistake above. Check `int_counts[1]` climbed over the interval before
drawing any conclusion from it.

### The full chain, as now understood

This is the chain **on the floppy path** — the one a run with media byte
`0xF9` takes. On a fixed disk step 2 onwards never happens.

1. **PKLITE** unpacks the image.
2. An **INT 03h handler at image `0x0DB3`** bootstraps the decryptor. It stores
   its own return address into `0000:0004` (the `IVT[01h]` *offset*), adds
   `0x42` to reach image `0x0DFF`, puts `CS` in `0000:0006`, and then decrypts
   0x3B bytes with `neg byte ptr cs:[si]` — an involution, so the same loop
   re-encrypts. That is how the single-step handler is installed and made
   readable.
3. The **INT 01h handler** then runs the one-instruction window described above.
4. The protected stretch reads the **Diskette Parameter Table** through
   `INT 1Eh` (`0000:0078`), copies its eleven bytes, and writes back to offset
   3 — bytes-per-sector — before doing **`INT 13h`**: reset plus a one-sector
   read, on drives 0 and 1, cylinders 0-3, retried 24 times. A floppy check.
5. `IVT[01h]` is later moved from `0x0DFF` to a second-stage handler at
   `0x0D0C`.

### Two emulator defects this exposed, both now fixed upstream

- **`INT 1Eh` was zero.** The protection's write to "DPT+3" therefore landed on
  `0000:0003`, three bytes from the `0000:0000` pointer its own decryptor uses.
  It corrupted itself and spun forever in encrypted bytes at
  `0x0D17..0x0D39`. Fixed by putting a real table at F000:EFC7.
- **`INT 13h` was unhandled**, which is worse than absent: carry was left as
  the caller set it, so all 24 reads looked like successes returning an
  uninitialised buffer. Fixed by answering as a machine with drives fitted and
  no diskette in them.

With both, the guest stops derailing: the protection completes, restores its
vectors, and the program exits cleanly — which, on the floppy path with no
original disk present, is the correct outcome rather than a remaining bug. It
is also the run `tools/decrypt_image.py` records to recover the protected code.

### Resolved: why it used to exit instead of reaching the menu

This was open for a while. The symptom: under DOSBox the same binary reaches
the main menu (`docs/reference/dosbox-main-menu.png`), while here it ran the
protection and then took an orderly shutdown path at image `0x0EE2`-`0x0F08` —
restore interrupt vectors, restore the video mode via `INT 10h`, reprogram the
PIT through ports 0x43/0x40 — and called `INT 21h AH=4Ch` with exit code 0. A
deliberate quit, not a crash.

Ruled out along the way, and worth keeping because each was plausible:
reporting no diskette drive in the BIOS equipment word at `0040:0010` changed
nothing (same 13 `INT 13h` calls, same exit); timing and PIT programming were
never the cause; nor was a specific INT 13h failure code.

**The cause was the media descriptor byte.** The emulator did not answer INT
21h AH=1Bh, so the guest read whatever `DS:BX` pointed at, took it for a
floppy, and ran the floppy protection — which, finding no original disk, quit
exactly as designed. Answering AH=1Bh with `0xF8` puts the guest on the fixed
disk path, where it never installs the trap handler and goes straight to the
menu. See the correction above for the differential that shows it, and
`dos_emulator` `_dos()` AH=1Bh/1Ch for the fix.

The lesson is the general one: the game was not failing a check, it was
answering a *different question* than we thought it was asking.

## Reading it: the permanently decrypted image

The protection keeps exactly one instruction in plaintext at a time and
re-encrypts behind itself, so no memory dump ever holds the protected stretch
in the clear. But every instruction *is* plaintext for the moment it executes,
so recording each instruction's bytes as the CPU runs it and writing them back
into the dumped image produces one that is readable everywhere the program went.

```
uv run python tools/decrypt_image.py            # merge image/rec -> the image
uv run python tools/decrypt_image.py --record   # re-record it from the game
uv run python tools/disasm.py image/vgalemmi.decrypted.img 0xdff -n 32
```

Only code that *executed* can be recovered — that is inherent to the method,
not a shortcoming of it — so coverage is the union of the runs merged, and
`image/vgalemmi.decrypted.json` says which runs contributed what. Three are
recorded by default because **no single run reaches everything**:

| run | what it covers |
| --- | --- |
| `startup.rec` | the fixed-disk start-up path, in-process |
| `protection.rec` | the same, with the media byte forced to `0xF9` — the only run that executes the protection |
| `level-250s.rec` | driven by `tools/play.py` through the menus into the first level |

As last measured: **9,010 distinct instructions, 19,742 bytes recovered**, of
which **651 differed from the dumped image**. The first two numbers move by a
few either way between rebuilds, because the level run is driven for a fixed
number of seconds and does not execute exactly the same code each time. The
**651 does not move**, and neither do the nine spans - the protected stretch
is fully covered, so re-recording adds nothing to it. That 651 is the whole of what the
protection was hiding, in nine spans:

```
0x00000..0x0002c  0x0066d..0x006c0  0x00709..0x00748
0x0077e..0x007c8  0x00844..0x00877  0x008be..0x008d6
0x00913..0x0096f  0x00d9e..0x00da2  0x00dff..0x00e39
```

Two things the report deliberately keeps apart, because reporting them together
makes the protection look several times larger than it is:

- **encrypted spans** — the image held *different non-zero* bytes there. This
  is protection.
- **runtime-loaded spans** — the image held *zeros* there (BSS), and the bytes
  are code the program loaded at run time. `0x18F00..0x1949F` is the AdLib
  driver from `ADLIB.DAT`, reached through port `0x388`. Not protection at all.

Records merge rather than each run producing its own image, so coverage only
grows: reach a new screen, record it, merge it in. Where two runs disagreed
about an offset the report says so; as last measured, **0**.

## Layer 3 — a run counter, and magic pairs

Not yet fully traced, and named here as a lead rather than a finding.

Around image `0x00E66` a routine opens a filename held at `[0x258A]`, reads one
byte, increments it, wraps it at 10, writes it back and closes:

```
0e66  8d168a25    lea dx, [0x258a]
0e6a  b8003d      mov ax, 0x3d00            ; open for reading
0e6d  cd21        int 0x21
0e73  b43f        mov ah, 0x3f              ; read 1 byte
0e7e  a02a25      mov al, byte ptr [0x252a]
0e81  fec0        inc al
0e83  3c0a        cmp al, 0xa               ; wrap at 10
```

**Hypothesis, unverified:** the file is `RUSSELL.DAT` (256 bytes, and
`LEMMI.BAT` resets it with `copy russell.var russell.dat`), and the counter is
what makes a check fire every so many runs rather than every run. That would fit
the crack note's "it does another disk check" at level 15.

Immediately above it, image `0x00E3D..0x00E64` is a chain of paired constant
comparisons, each clearing carry on a match and the last setting it:

```
0e3d  cmp ax, 0x581c / cmp bx, 0xa9f2 / clc / ret
0e4a  cmp ax, 0xd950 / cmp bx, 0x270a / clc / ret
0e57  cmp ax, 0x9cf8 / cmp bx, 0x1a8c / clc / ret
0e64  stc / ret
```

A table of accepted `AX:BX` pairs. What produces the pair is not yet known.

**The emulator's read-only guarantee covers this**: the guest may rewrite
`RUSSELL.DAT` as much as it likes and nothing on disk changes, so runs stay
repeatable.

## Is this Psygnosis's protection or the cracker's?

**Certain:** the *package* is a cracked release. `lemmthg!.doc` is THG's note
("it does another disk check.. I patched that out"), `crue.inf` is Harry Lime's
trainer, `lemvga.com`/`lemcga.com`/`lemtga.com` are THG loaders carrying
`LEMMINGS THG` and `Bad copy.`, and `flr.exe` is the trainer itself. None of
that is Psygnosis's.

**Inferred, not proven:** that `vgalemmi.exe` retains the *original* protection
with the disk check patched out. The reasoning is that a cracker removes
protection rather than adding trap-flag self-decryption, and this decryptor is
load-bearing — the code is stored encrypted, so it cannot be stripped without
statically decrypting everything first. Patching a disk check is a far smaller
change and is what the note describes.

This has **not** been checked against an uncracked original, and no claim here
depends on the answer. If a clean copy ever turns up, comparing the two would
settle it.
