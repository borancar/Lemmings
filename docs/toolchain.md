# What VGALEMMI.EXE was built with

**Hand-written assembly.** Not a compiled binary, and therefore a *matching*
reconstruction is not meaningfully available: behavioural equivalence through
differential verification is the standard this project can reach. Every
instruction is a decision someone made and is worth reading as one.

Measured against `image/vgalemmi.decrypted.img` — the unpacked image with the
protected stretches recovered, so the test covers the whole program and not
just the parts that happen to be in the clear.

## The evidence

| test | result | what it means |
| --- | --- | --- |
| `55 8b ec` — `push bp; mov bp,sp` | **0** in 131,072 bytes | no compiler emitted these functions |
| `e8 .. .. 83 c4` — near `call` then `add sp,imm` | **0** | no cdecl caller-side cleanup |
| `9a .. .. .. .. 83 c4` — far `call` then `add sp,imm` | **0** | likewise |
| `c8 ?? ?? 00` — the `enter n,0` shape | **3** | noise at this density, not framed functions |
| compiler banner strings | **none** | no Borland, Microsoft, Watcom, Zortech, Lattice or Aztec marker |
| runtime-library fingerprints | **none** | no `matherr`, no `__ctype` table, no FP emulator strings |

The only copyright string in the image is the game's own,
`Copyright 1991 Psygnosis Ltd.`, and the only other vendor text is
`1. Microsoft Mouse` — a menu item, not a toolchain marker.

The zero-prologue result is the decisive one. A compiled 16-bit C program has a
`push bp / mov bp,sp` at nearly every function; *zero* across a whole code
segment means no compiler was involved. Finding zero in the entire image, not
merely a low count, leaves no room for "compiled with frame pointers omitted" —
that setting was not common in 1991 toolchains and would not reach exactly zero.

## Corroboration from routines actually read

The terrain-composition routine near image `0x03720` passes its state through
**fixed absolute globals** rather than a stack frame:

```
03727  8a4517      mov al, byte ptr [di + 0x17]
0372a  a27f00      mov byte ptr [0x7f], al
0372d  8a6518      mov ah, byte ptr [di + 0x18]
03733  051000      add ax, 0x10
03736  a30000      mov word ptr [0], ax      ; a scratch global at DGROUP:0
03739  d1e8        shr ax, 1                 ; three shifts, not a divide
0373f  a30200      mov word ptr [2], ax
03742  a0e80f      mov al, byte ptr [0xfe8]
03745  22c0        and al, al
03747  7406        je 0x374f
```

`and al, al` to test a byte, three `shr` in a row instead of a division, values
threaded through `DGROUP:0` and `DGROUP:2` as scratch, arguments arriving in
`DI` — this is a person writing assembly, not a code generator.

## What follows for the port

- **The standard is behavioural, and the proof is a differential run.** There
  is no compiler to match, so "the bytes agree" is not on offer; "the original
  and the port composed the same 49,741 pixels" is.
- **Every idiom is intentional.** In compiled output, register shuffling is the
  compiler's noise and should not be preserved. Here it is not noise, and where
  a routine cannot be written honestly as structured C it should be written
  literally with a note saying why.
- **Globals are the calling convention.** Routines communicate through fixed
  DGROUP addresses, so the port's shared state mirroring those addresses is
  faithful rather than a shortcut.

## How to re-run this

```
uv run python - <<'PY'
img = open("image/vgalemmi.decrypted.img", "rb").read()
n = 0; i = img.find(b"\x55\x8b\xec")
while i >= 0: n += 1; i = img.find(b"\x55\x8b\xec", i + 1)
print("push bp; mov bp,sp :", n)
PY
```
