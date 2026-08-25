# The main menu, and the screens it reaches

What the program *is*, for the menu. Progress and what is proven live in
[STATUS.md](../STATUS.md); this is reference, argued back to bytes.

Addresses are **image offsets** into the unpacked image unless written
otherwise. `MAIN.DAT` section numbers are 0-based, as `tools/dat.py` prints
them.

## The video mode

The menu, the briefing, the level-code screen and the input chooser all run in
**EGA mode 10h — 640×350, four bit planes, 16 colours**. The game itself runs
in mode 0Dh. Nothing in the level renderer applies here: everything on these
screens is planar.

The attribute palette is the BIOS default for mode 10h, unchanged by the game:
values 0–7 map to DAC `0x00`–`0x05`, `0x14`, `0x07`, and values 8–15 to DAC
`0x38`–`0x3F`. That mapping matters only when comparing against the emulator's
screen dump, which reports DAC indices; the port keeps the 4-bit values.

**The menu's 16-colour palette is transcribed from image `0x0E3C0`** — sixteen
6-bit RGB triples in attribute-register order. It is a table in the executable,
not in any data file. Six-bit values widen to eight by **replicating the top
bits**, `(v << 2) | (v >> 4)`, not by scaling to 255.

## Where the menu's pieces live

All in `MAIN.DAT` section 3, which is 61,968 bytes and fully accounted for:

| offset | size | what |
| --- | --- | --- |
| `0` | 8,320 | the background tile, **two planes** at `0` and `4160` |
| `8320` | 29,704 | the logo, four planes |
| `38024` | 21,960 | six sign plates, four planes, 3,660 apart |
| `59984` | 1,984 | unaccounted |

**The background** is 320×104 and uses only four colours, so it has two planes
and not four. That is measured, not assumed: assembling it from four
consecutive 4,160-byte blocks scores 39% against the original's screen, and
forcing planes 2 and 3 to zero scores 95.6%. It tiles from (0,0) — 640 is
exactly two across, and the vertical repeat of 104 does not divide 350, so the
bottom tile is cut off, which is what the original shows.

**The logo** is 632×94 at screen (8,10), stride 79, planes 7,426 apart. Index 0
is transparent: drawn opaquely its own bounding box scores 35%, keyed it scores
95%+, and the background texture is plainly visible between the letters.

**The plates** are 120×61, stride 15, planes 915 apart, at

    (68,120) (196,120) (324,120) (452,120)
             (196,197) (324,197)

Each matched its own slot at 86.7–95.4% against 39–78% for every other slot — a
clean diagonal, which is what makes the assignment evidence rather than a
guess.

## The font

**`MAIN.DAT` section 4, offset 23888.** 16×16 glyphs, **three 32-byte planes**
each, 96 bytes a glyph, indexed straight by ASCII code. The block runs from
character 0 to 126, ending at `23888 + 127×96 = 36080` — exactly the length of
section 4, which is the confirmation that the glyph size and the base are both
right.

**Plane 2 is last, and doubles as the opacity mask**, because every colour the
font uses is 4–7 and so has bit 2 set. Reading the mask as the *first* plane
scores 51%; as the last, with plane 0 then plane 1 before it, it is 100.0% over
both of the menu's credit lines — 5,606 masked pixels, none differing.

**A space draws nothing.** Its slot in the font is not blank; drawing it puts
ink where the original leaves background, and that alone is the difference
between 94.3% and 99.3% on the credit lines. The original's text routine skips
the character rather than relying on an empty glyph.

### Where text is placed

There is **no rule**, and looking for one wastes time. Centring gives one
answer, the original uses another, and the offset differs per string:

| line | characters | centred x | actual x |
| --- | --- | --- | --- |
| `(C)MCMXCI Psygnosis Ltd` | 23 | 136 | **128** (centre − 8) |
| `A DMA Design Game` | 17 | 184 | **176** (centre − 8) |
| `Enter Code` | 10 | 240 | **224** (centre − 16) |
| `Press mouse button to continue` | 30 | 80 | **80** (centred) |

So positions are per-screen constants, and this file records them as measured
rather than derived.

## The banner

The strip at the foot, rows 334–349.

**The bar is not a sprite.** Logging the blits that land in those rows finds
the source at image `0x191BC`, in BSS, and dumping *that* shows rows of `0xFF`,
`0xF0` and `0x00` — solid fills. The bar is a strip the game fills and then
renders scrolling text into, so there is no bitmap in any file to find. Its
geometry, measured off the original:

- x 36–604, y 334–349, colour 13 (DAC 61)
- dashed rules at y 336 and y 347, where the colour appears only where
  `(x & 7) >= 4`

Checked over x 100–600: 499/500 for the solid rows, 495/500 for the dashed
ones, the misses being the scrolling text passing through.

Three shapes draw in those rows, and separating them needs the blit *starts* —
`cx` counts down inside the loop, so counting hook fires measures bytes and
reports 132,588 draws instead of 4,760:

| shape | count | source | what |
| --- | --- | --- | --- |
| `cx=40, bp=2` | 20 | `0x58AB0`, step 40 | the background tile, rows 330–333 |
| `cx=70, bp=4` | 1,584 | `0x292BC`, step 70 | the bar |
| `cx=6, bp=4` | 2,368 | `0x6A000`–`0x6D000` | the scrolling text, a different font |

The two lemmings holding the ends (x 12–35 and 605–628) have not been located.

## What each plate does

Read off the original by pressing one key on a **fresh** guest and diffing the
screen against a no-key baseline (`tools/probe_menu_keys.py`):

| plate | key | what the original does |
| --- | --- | --- |
| 1 PLAYER | F1 | fades out into the level, via the briefing |
| NEW LEVEL | F2 | an **"Enter Code"** screen |
| FX | F3 | **nothing** — only the scroller moves |
| FUN (rating) | — | nothing found works it |
| EXIT TO DOS | Esc | quits |
| ? | F4 | a **"Choose Input Type"** screen |

**The plates are keyboard-only.** The menu calls INT 33h AX=0002h once — hide
cursor — and draws no pointer: moving the position to (600,40) changes nothing
near it. It polls AX=0003h 1,342 times, so it reads the *button*, and the
position reaches it and is ignored. Clicking "EXIT TO DOS" does not exit; it
starts the level, exactly as clicking bare background does. **A click anywhere
acts as F1.**

### The rating plate answers to nothing found

It carries two arrow tabs rather than a function key. **Twenty-five keys have
been tried** — F1–F6, Esc, Up, Down, Left, Right, PgUp, PgDn, keypad + and −,
plus and minus, Space, Return, Tab, the digits 1–4, `n`, `r` — and a click,
which the menu ignores the position of anyway. Every one leaves the plate's box
at (452,120)–(571,180) **untouched, zero pixels**, while the scroller keeps
moving in the same samples, which is what shows the guest was alive and taking
input.

The four ratings *are* in the executable, at image `0x0E691` —
`"Fun   TrickyTaxingMayhem"`, four six-character slots — so the game knows
about them.

Why the plate is inert here is **not established**, and one tempting
explanation is already ruled out: this is not a one-rating set. The data ships
**ten `levelNNN.dat` files of eight levels each, eighty levels**, with names
spanning `"Tightrope City"`, `"Mary Poppins` land"` and
`"Every Lemming for himself!!!"` — names from across the full game's ratings.
So "there is only one rating here, hence nothing to select" does not hold.

`tools/probe_rating.py` runs the sweep. It watches only the plate's own box, so
one guest can take every key in turn, and it stops if the whole screen changes
rather than reporting nonsense from the next screen along.

## The sub-screens

Both are the menu's background and font, with text at measured positions.

**Enter Code** (F2): `"Enter Code"` (image `0x0E874`) at (224,112) and
`".........."` (image `0x0E882`) at (224,144).

**Choose Input Type** (F4): the title (image `0x0E8CD`) at (176,80), then
`"1. Microsoft Mouse"` (`0x0E901`), `"2. Joystick"` (`0x0E8E2`) and
`"3. Keyboard"` (`0x0E8F1`) sharing x=112 at y 128, 160 and 192.

## The briefing

Shown between the menu and the level. Its text was **read back off the
original's own screen**, by building a mask-to-character table from the font's
plane 2 and matching each 16×16 cell against it:

```
y= 80  x=  0   'Level 1        Just dig!'
y=128  x=160   'Number of Lemmings 10'
y=160  x=160   '10%  To Be Saved'
y=192  x=160   'Release Rate 50'
y=224  x=160   'Time 5 Minutes'
y=256  x=160   'Rating  Fun'
y=304  x= 80   'Press mouse button to continue'
```

That settled three things guessing would have got wrong: the **percentage** is
printed, not the count; the level's own 32-character name field is drawn whole
at x=128, its padding being what positions the title; and the body lines are 32
apart.

The templates are in the executable and the numbers are written into the runs
of spaces each leaves for exactly that: `0x0E59D`, `0x0E5CD`, `0x0E5E7`,
`0x0E5FB`, `0x0E60E`, `0x0E620`, `0x0E633`.

**Its background is unsolved.** It is not the menu's tile — the original draws
it in DAC 56–59, which on this screen are the *level's* terrain colours, and
the tile with bit 3 set scores 1.65% at its best vertical phase. The blit
source buffer at `0x58000` begins `f1 80 07 fc 00 00 05 f7`, which is `MAIN.DAT`
section 3 byte for byte — so the data is that section, but not its tile at
offset 0. Palette entries 8–15 are the same gap.

## What animates

Two lit captures 2.5–7.5 seconds apart, with the palette identical, differ in
182–1,334 pixels — about 0.6% of the screen — in three places:

- **rows 336–351**, 240–660 px: the scrolling text and the end lemmings
- **rows 48–63, x 34–58**, 9–24 px: a detail on the logo's left-hand lemming
- **rows 328–335, x ~550–590**, 5 px: something small tracking leftwards over
  time (586 → 550 across two samples)

A single composed frame cannot match any of it, which is why the port's
comparison is against one frame and the residual is named rather than chased.

## Which level is "Level 1"? The order table

**Image `0x0EF7E` (DS:`0x54EE`), 120 bytes, zero-terminated.** One entry per
level: four ratings of thirty, in the order the game presents them. The decode
is the loader's own, at image `0x01983`:

```
0x01983  lea si, [0x54ee]     ; the table
0x01987  mov cl, [bx + si]    ; n = table[level index]
0x01989  dec cl
0x0198e  shr cx, 1            \ section = ((n - 1) >> 1) & 7
0x01998  and cx, 7            /
0x01990  mov ax, cx           \ file    = (n - 1) >> 4
0x01992  shr ax,1 x3          /
0x0199b  lea si, [0x2551]     ; the digits of "level000.dat" at DS:0x254C
0x019a4  lea dx, [0x254c]
```

The filename templates sit together at image `0x0BFC4`: `vgagr0.dat`,
`ground0o.dat`, `level000.dat`, `vgaspec0.dat`, `oddtable.dat`, `adlib.dat`,
`tandysnd.dat`, `russell.dat`. Only the digits are overwritten.

Three things check the reading:

- the 120 entries cover **all 80** `(file, section)` slots the data ships, with
  **40 referenced twice** — those levels appear in two ratings;
- `ODDTABLE.DAT` is 80 records of 56 bytes (the LVL header's first 24 bytes,
  then a 32-character name) of which **exactly 40** are marked
  `"This is a non-used duplicate"` — the same 40;
- entry 0 is `level009` section 1, and the original's briefing for it says
  **"Level 1"**.

The first level of each rating comes out as the real game's:

| rating | file | section | name |
| --- | --- | --- | --- |
| Fun | `level009.dat` | 1 | `Just dig!` |
| Tricky | `level000.dat` | 0 | `This should be a doddle!` |
| Taxing | `level002.dat` | 2 | `` If at first you don`t succeed.. `` |
| Mayhem | `level005.dat` | 5 | `Steel Works` |

The low bit of `n - 1` is discarded by the shift and is not decoded here; the
loader tests something else with it further on. Unread, and said so.

**So this is the full game — 120 levels across four ratings**, not a demo. The
file order is not the game's order, which is why the port takes `--rating` and
`--number` and resolves them through this table.
