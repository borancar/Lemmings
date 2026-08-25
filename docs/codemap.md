# The code map

Built by `tools/codemap.py` from `image/vgalemmi.decrypted.img` — the unpacked
image with the protected stretches recovered, so the map covers the whole
program and not only the parts that happen to be in the clear.

```
uv run python tools/codemap.py                  # the map and the table below
uv run python tools/codemap.py --routine 0x1a7b # one routine, with which
                                                # instructions actually ran
uv run python tools/codemap.py --tables         # look for dispatch tables
```

## Why recursive descent, and why it is seeded twice

Linear disassembly desynchronises constantly in hand-written assembly — data
sits between routines and jumps land mid-instruction — so it produces
plausible-looking garbage with no way to tell which parts. Descent only decodes
bytes something reaches.

Two seeds:

- **image `0x0000`**, the entry point PKLITE restores.
- **every offset recorded as having executed**, from `image/rec/*.rec`. These
  are not inferred: a real run put the instruction pointer there.

The second seed is what makes the report honest, because it lets the map
separate *proven to be code* from *believed to be code*.

## Coverage, as measured

| | bytes | of the code region |
| --- | --- | --- |
| code region (0 .. DGROUP `0x9A90`) | 39,568 | 100% |
| decoded by descent | 33,875 | **85.61%** |
| of that, **proven executed by a run** | 19,067 | **48.19%** |

211 call targets found, 44 of which no recorded run ever executed. One offset
in the whole region would not decode.

Seeding from the entry point *alone* reaches 26,302 bytes (66.47%) and 198 call
targets, and leaves 2,727 recorded offsets that descent never meets. So the
execution seeds are worth about 19 points of coverage, and the gap between the
two is a direct measure of what static analysis cannot follow here.

**Only the proven figure is evidence.** The other 37 points are a hypothesis
that those bytes are code — a good one, since something branches to them, but
not the same kind of claim.

## There are no dispatch tables

This surprised me, and it was worth measuring rather than assuming, because the
usual advice for mapping a DOS game is to find the handler tables first and
seed descent with them.

Across the whole decoded code region:

| | count |
| --- | --- |
| `call` through memory (`call word ptr [...]`) | **0** |
| `jmp` through memory | **0** |
| `call` through a register | 5 |
| `jmp` through a register | 1 |
| runs of ≥4 consecutive `jmp near` (a jump table made of code) | **0** |

The six register-indirect sites are `0x00898` (`call di`), `0x01407`,
`0x042A5`, `0x0715B` (`call ax`), `0x04CC9` (`call bp`) and `0x04D18`
(`jmp bp`). Everything else is a direct call or a compare-and-branch chain.

Searching the image for tables of addresses finds none either, at a threshold
where chance would produce 0.017 false hits. That search took three attempts
and the two failures are worth recording, because both looked like results:

1. **"every entry is an instruction boundary"** — descent decodes 36% of the
   region, so a run of four hits constantly. It reported 156 tables, nearly all
   noise.
2. **"every entry is an address that executed"** — recorded offsets cover about
   5%, which ASCII text hits at a good rate. It reported a table at `0x0E6E4`
   whose four entries, read as bytes, spell `" rescued"`.
3. **"every entry is a routine start"** — a call target, or an address that
   executed *and* that a decoded instruction ends at with `ret`/`iret`/`jmp`.
   417 such addresses, 0.64% density. **Zero tables.**

This is consistent with the binary being hand-written assembly
([toolchain.md](toolchain.md)): a person dispatching on a small enumeration
writes a chain of compares, where a compiler would build a jump table.

## What the remaining gap is, then

Since there is nothing to dispatch through, the 2,727 recorded offsets static
descent misses are not behind handler tables. They are reached through the six
register-indirect sites, through **interrupt vectors the program installs**
(the protection's own INT 01h/INT 03h handlers among them), and through code
the program relocates. Reading those six sites is the next thing that would
raise static coverage; the execution seeds already cover them in practice.
