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

### It does not re-encrypt behind itself

This matters more than anything else here, because it decides the method. Two
byte-for-byte comparisons of image `0x00600..0x00A00` within one run —
instruction 2,000,000 vs 3,000,000, and 3,000,000 vs 4,000,000 — showed **0 of
1024 bytes changed**. The plaintext persists.

So a memory snapshot taken after start-up holds decrypted code, and ordinary
static disassembly is valid. Had it re-encrypted, no snapshot would ever have
held the program and the code map would have had to be *recorded* instruction by
instruction. `tools/probe_selfmod.py` is that check; re-run it before trusting
any snapshot of a region not yet covered by it.

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
