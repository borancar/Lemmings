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
- Only **108** `INT 01h` events occur in a whole start-up run, so the protected
  stretch is a short stub, not the whole program.

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

### A second-stage handler, and where our emulation currently fails

`IVT[01h]` starts at image `0x00DFF` and is later moved to image `0x00D0C` by
the game's own code — `les si, ptr [bp+0xf]` at image `0x0942` followed by
`mov word ptr es:[si+3], bx` at `0x0945`, which writes the new offset straight
into the vector.

Under `dos_emulator` the guest then executes **garbage** at `0x00D0C` and spins
forever in a ~14-instruction loop between `0x00D17` and `0x00D39`, taking no
interrupts at all. Under DOSBox the same binary reaches the Lemmings main menu,
so the second-stage handler is plaintext by then on a correct machine and our
run has failed to decrypt it.

**Root cause not yet found.** The scheme depends on receiving *exactly one*
`#DB` per instruction, so any extra or missing trap desynchronises the rolling
key and turns following instructions into noise. Candidates, none confirmed:

- the `mov ss` / `pop ss` **interrupt shadow** — a load of `SS` inhibits
  interrupts, single-step included, for the following instruction. If that is
  not modelled, one extra trap arrives and the key desynchronises.
- `REP`-prefixed string instructions, where the trap should arrive per
  iteration.
- our INT shim executing `INT 21h` inline rather than through a real gate,
  changing how many traps the sequence produces.

The instrument to build next is a desync detector: record the return address
the handler computes from `ss:[sp+6]`, and compare it against the address
actually executed after the `IRET`. The first mismatch is the bug.

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
