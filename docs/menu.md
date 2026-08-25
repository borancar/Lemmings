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
forcing planes 2 and 3 to zero scores 95.6%. It tiles from (0,0), and the vertical repeat of 104 does not divide 350, so the
bottom tile is cut off, which is what the original shows.

**It is 632 pixels wide, not 640.** The original leaves the last byte of each
row black — it has no non-black pixel anywhere beyond x=635, while the left
edge is drawn normally — so the row is two tile blits of 40 and 39 bytes
covering x 0..631, and the rest stays as the screen clear left it. Drawing the
full 640 costs about 1,400 pixels spread thinly down the right edge of every
band, which reads as noise rather than as a bug. Clipping to 632 took the input
chooser from 99.37% to **100.00%**.

**The logo** is 632×94 at screen (8,10), stride 79, planes 7,426 apart. Its
**ink is exact** — a pixel-granularity sweep of its position gives 100.00% over
7,006 sampled ink pixels — so the size, the offsets and the placement are all
confirmed.

Its ink uses only values 8–15, never 1–7, so bit 3 is in effect its own mask.
Index 0 is treated as transparent, which is exact in the logo's upper rows and
**wrong somewhere below y≈55**: there the original shows black where the tile
would show through. Knocking the ink out of the original's screen shows a black
halo hugging the outline — [reference/behind-logo.png](reference/behind-logo.png).

Seven models have been tried and none reaches the ~100% that every other piece
of this screen reaches: keyed 93.64%, opaque per byte-column 93.93%, opaque per
ink span 90.27%, fully opaque 82.14%, keyed-then-opaque at the best split
96.42%, a 1-pixel dilation 94.08%, and section 2 as a 1-bit mask 92.29%. It is
not animation either — those rows are identical between captures. **Open**, and
STATUS.md carries the list so none of it need be retried.

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

The dots are an **input field**, and the original blacks its whole strip out —
every pixel of those ten 16×16 cells that is not a dot is 0, not background
showing through. The title above it is keyed normally. Getting that one
distinction right is the difference between 99.45% and **100.00%** on this
screen.

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

## The FX plate's label

The plate bitmaps carry their own lettering — "1 PLAYER", "NEW LEVEL", "FUN" —
except the FX one, which is blank in section 3 because its label swaps.

Section 3's last **1,984 bytes** are two 64×31 sprites of four planes each:
8 × 31 = 248 bytes a plane, 992 a sprite, plane-major. **Sprite 1 is the "FX"
lettering** the menu shows; sprite 0 is what it swaps to. That accounts for the
whole of section 3 — background, logo, six plates, two labels — with nothing
left over.

It is drawn at **(351, 146)**, which is *not* byte-aligned, and that alone is
the difference between 92.74% and **100.00%**: sweeping candidate positions in
steps of 8 finds a convincing-looking best fit, and sweeping single pixels
finds the real one — 1,654 ink pixels, none differing.

## How sprites are actually blitted

Recorded from the guest's own writes to the sequencer (`0x3C4`) and the
graphics controller (`0x3CE`) while the menu draws — 32,806 of them in one
pass. There are two blitters.

**Opaque**, at image `0x0985F`:

```
SEQ  map mask         = 0x0F      all four planes writable
GC   set/reset        = 0x00
GC   enable set/reset = 0x0E      planes 1-3 take set/reset, i.e. ZERO
GC   mode             = 0x00      write mode 0
GC   data rotate      = 0x00
GC   bit mask         = 0xFF
     ... mov al, es:[di] / movsb ...   (the read loads the latches)
GC   enable set/reset = 0x00      restored afterwards
```

Plane 0 takes the CPU byte and planes 1–3 are forced to zero, so every byte it
touches is replaced outright. Later passes with the map mask set to `0x02`,
`0x04` and `0x08` fill in the other planes.

**Masked**, at `0x089BE`, `0x09121`, `0x09442` and `0x097ED`: the same idea but
driving the graphics controller's **bit mask** per byte. Across one menu draw
it takes **184 distinct values**, and **7,193 of 29,179 writes are partial** —
neither `0x00` nor `0xFF`.

So sprites are clipped by a genuine per-pixel mask, and that mask is **not**
the sprite's own ink: where the mask is set and the sprite's pixel is 0, the
result is black. That is the black halo around the logo and the plates. Where
the mask data lives is **not yet found**.

## Drop shadows, and one sprite that is not keyed

Everything on this screen casts a **black drop shadow**, drawn as a separate
blit *before* the sprite:

| sprite | shadow blitter | offset |
| --- | --- | --- |
| the logo | image `0x097F4`, 7,426 bytes into rows 20..113 | 8 px left, 10 rows down |
| the plates | image `0x09128`, into rows 128..188 | 8 px left, 8 rows down |

Each shadow blit is the sprite's own size, so it is a second copy of the
silhouette rather than anything computed. The port uses each sprite's own ink
as a stand-in; the original reads a separate buffer (page `0x5F000`), so this
is a **fit, not a transcription**.

**The FX label is the exception: it is drawn opaquely**, black and all. That
follows from what it is for — it swaps between `FX` and its alternate, so
drawing one has to erase the other, which a keyed blit cannot do. Keyed 94.04%
against opaque 98.79% over its neighbourhood.

A drop shadow for the label, and a one-pixel black outline, were both tried and
are both worse.

## What the port still does not match

**1,625 pixels of 224,000**, and they are accounted for:

- **995 in the banner's rows** — the scrolling text and the two end lemmings.
  Animated, so no composed frame can match them.
- **about 630 inside the plates** — the **rating name**. The plate's own bitmap
  carries a `FUN`, and the original overdraws the current rating one pixel to
  its left, so the two nearly coincide and differ along every edge. It must be
  drawn separately because the rating changes; the four names are a string at
  image `0x0E691`.

  It is not drawn with the 16×16 menu font: overlaying `"Fun"`, `"FUN"` or
  `"Fun   "` from it at every position over the plate scores 76.56% against
  83.45% for leaving it alone. A second, smaller font or a sprite set is
  involved and has not been found.

## Unaligned sprites are pre-shifted, not drawn shifted

Image `0x09360` holds the mechanism, and it explains several things at once:

```
0x09365  mov al, [0xa948]      ; how many bits to shift by
0x09368  push si / push cx
0x0936c  shr byte ptr [si], 1  \  shift the whole buffer right one bit,
0x09371  rcr byte ptr [si], 1   >  carrying between bytes
0x09376  loop 0x9371           /
0x0937a  dec al / jne 0x9368   ; repeat for each bit
0x09383  mov al, es:[di]       ; then blit it, latches and all
0x09386  lodsb / stosb
```

So a sprite that lands at a non-byte-aligned x is **copied into a scratch
buffer and shifted right one bit at a time**, then blitted byte-aligned. That
is why the blitter's own destinations are always byte-aligned even though
sprites plainly are not — the FX label sits at x=351, which is 7 mod 8.

It also explains the rating name's one-pixel offset: the plate's bitmap carries
a `FUN` at one position and the original overdraws the current rating from a
pre-shifted copy at another. The scratch buffer is what the blit reads, so
following the blit's source address finds the buffer and not the artwork —
which is why the rating's own sprite has still not been located.

## MAIN.DAT section 4

Loaded at linear `0x679F0` in the runs measured here. Only its tail is
understood:

| offset | what |
| --- | --- |
| `0` – `23888` | **unexplored.** The rating names are in here; so is whatever else the menu draws that is not in section 3 |
| `23888` – `36080` | the 16×16 font, 96 bytes a glyph, ASCII-indexed |

The head is not one big planar bitmap — rendering it as four planes at 64 and
96 pixels wide gives noise — so it is likely several sprites with their own
dimensions, as section 3 turned out to be.

## Where the port stands against each screen

| screen | agreement |
| --- | --- |
| the level-code screen | **100.00%** — 0 of 224,000 |
| the input chooser | **100.00%** — 0 of 224,000 |
| the briefing's text | 99.97% — 4 of 15,188 |
| the menu | 99.41% — 1,321 of 224,000, of which 995 are the animated banner |

So two of the four match the original pixel for pixel, and the menu matches
everything on it that holds still to within about 320 pixels.
