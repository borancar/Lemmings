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

Its ink uses only values 8–15, never 1–7, so bit 3 is in effect its own mask,
and **index 0 is transparent** — the captured bit mask the blitter programs is
exactly the sprite's ink, 100.00% agreement, so keying is precisely what the
original does.

Knocking the ink out of the original's screen leaves a black halo hugging the
outline — [reference/behind-logo.png](reference/behind-logo.png). That is **not**
a compositing rule: it is the **drop shadow** described below, a second sprite.

Worth recording because it cost three sessions: seven models were fitted to
those pixels first, and every one of them plateaued — keyed 93.64%, opaque per
byte-column 93.93%, opaque per ink span 90.27%, fully opaque 82.14%,
keyed-then-opaque at the best split 96.42%, a 1-pixel dilation 94.08%, section 2
as a 1-bit mask 92.29%. None was the answer because the answer was another
sprite, not a rule.

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

**The bar is not a sprite, and it is not static either.** Logging the blits
that land in those rows finds the source at image `0x191BC`, in BSS, and
dumping *that* shows rows of `0xFF`, `0xF0` and `0x00`. That is the strip the
game **scrolls**, not artwork. The artwork is a single 16×16 column it tiles.

The whole mechanism is readable, at image `0x07CBC` (set-up) and `0x07D0D`
(one step):

| | |
| --- | --- |
| the strip | 35 columns × 16 px, four planes, 70 bytes to a row — image `0x07D54` shifts exactly that: `cx`=4 planes, `cx`=0x10 rows, `cx`=0x46 bytes |
| one step | shift left **4 pixels**, add 4 to `ds:0x21` |
| a new character | when that crosses a multiple of 16: image `0x07D8D` stamps the bar template into the rightmost column, image `0x07DB7` masks a glyph over it |
| `0xFF` in the text | not a glyph — sets `ds:0x23` = 100, a hundred-step **pause**, and the character *after* it is drawn |
| `0x0D` | rewind to the start of the string |

**The tile** is at the original's `ds:0x4C10`, which is **section 4 offset
23040**. Decoded it is colour 13 everywhere except rows 2 and 13, which run
`0000dddd0000dddd` — exactly the two dashed rules at screen y 336 and 347.

**So the dashed rules scroll.** They are not a function of x at all. Four
captures give phases 0, 0, 4 and 4 mod 8, which is what a 4-pixel step
produces, and any fixed rule fitted to one capture is wrong on the next — that
happened twice here, once as `(x & 7) >= 4` and once as `(x & 7) < 4`.

**The text** is at image `0x0E45E` (`ds:0x49CE`), 266 bytes ending in `0x0D`:

```
         Lemmings By DMA Design      ÿ       Programming By Russell Kay    ÿ
       Animation By Gary Timmons     ÿ       Graphics By Scott Johnston    ÿ
 Music By Brian Johnston & Tim Wright       PC Music By Tony Williams     ÿ
      Copyright 1991 Psygnosis Ltd.  ÿ
```

**The font is the same one the rest of the menu uses.** The original computes
the glyph as `0x5BC0 + (ch - 0x21) * 96`, and `0x5BC0 + 3568 - 0x21 * 96` is
**23888**, which is `FONT_BASE`. This document previously called it "a
different font"; it is not. (3568 is the offset between section 4 and the
segment at `[0x1FEE]+0x1000`, and the bar tile lands on its exact artwork at
the same offset, which is what confirms it.)

One thing that matters and is easy to get backwards: **text enters at the
right**. At start-up all 35 columns are bare template, so composing at scroll
0 with the ribbon already in place puts "Lemmings By DMA Design" on a bar the
original still has empty. Getting the direction wrong cost 3,076 differing
pixels against 62.

The port draws all of this — `draw_banner` in `game.c`, `--scroll N` on the
command line, and `menu_run` animates it. Scored against the original at the
scroll each capture is actually at:

| rating | scroll | whole screen |
| --- | --- | --- |
| Fun | 0 | **99.70%** |
| Tricky | 4 | **99.70%** |
| Taxing | 4 | **99.70%** |
| Mayhem | 0 | **99.71%** |

and the model *predicts* those phases rather than being told them. What is
left inside the bar is 62–72 pixels near x 588–604, which is the same
unlocated thing that puts colour 12 in the margins and a stripe of colour 6 at
x=592: the two end lemmings.

The **step rate** is not measured. The 4-pixel step and the 100-step pause
are; how often the original takes a step is inferred from its menu loop
waiting on the display once per pass, so the port takes one step per displayed
frame and says so.

Three shapes draw in those rows, and separating them needs the blit *starts* —
`cx` counts down inside the loop, so counting hook fires measures bytes and
reports 132,588 draws instead of 4,760:

| shape | count | source | what |
| --- | --- | --- | --- |
| `cx=40, bp=2` | 20 | `0x58AB0`, step 40 | the background tile, rows 330–333 |
| `cx=70, bp=4` | 1,584 | `0x292BC`, step 70 | the bar |
| `cx=6, bp=4` | 2,368 | `0x6A000`–`0x6D000` | the scrolling text, a different font |

**The two lemmings holding the ends are at x 0–47 and 592–639**, found by
hooking the shared blitter (`tools/probe_banner_ends.py`) rather than by
looking for a sprite. Two of the calls to image `0x8822` landing in these rows
are not the strip:

    x=0    y=334  rows=16  planes=4  width=48
    x=592  y=334  rows=16  planes=4  width=48

with the source in the segment at `ds:0x1FE4`, which image `0x04F96` sets to
`[0x1FEE]+0x800` — section 4. They decode as a lemming with **the bar's end cap
built into the same 48 pixels**, dashed rows included, which is why the painted
bar runs 36–602 while the strip is only 48–607: *the caps are not the bar.*

16 frames of 384 bytes; left at section 4 offset **10752**, right at
**16896** (= left + 6144). Every capture matches a frame exactly — zero pixels
differing, both sides — and the two are always on the same frame index: Fun 14,
Tricky 13, Taxing 15, Mayhem 2. The frame does **not** follow the scroll:
fitting `frame = scroll/4 + c` gives a consistent `c` for three captures and
none for the fourth.

With them drawn, the banner's rows differ by **zero** at all four ratings and
the whole screen scores **99.85%**.

## What each plate does

Read off the original by pressing one key on a **fresh** guest and diffing the
screen against a no-key baseline (`tools/probe_menu_keys.py`):

| plate | key | what the original does |
| --- | --- | --- |
| 1 PLAYER | F1 | fades out into the level, via the briefing |
| NEW LEVEL | F2 | an **"Enter Code"** screen |
| FX | F3 | **nothing** — only the scroller moves |
| FUN (rating) | Up / ` and Down / - | raises and lowers the rating, capped at Mayhem and floored at Fun |
| EXIT TO DOS | Esc | quits |
| ? | F4 | a **"Choose Input Type"** screen |

## How a sprite is actually drawn — image 0x0890E

Not "keyed". The blitter branches on **bit 6 of `bp`** at image `0x08905`; the
menu passes `0x03` for a sprite and `0xF43` for its shadow. Both paths take a
mask pointer, which the menu supplies as `ink + 3 * plane_size` — the plates at
`si+2745` with planes of 915, the logo at `si+22278` with planes of 7426. The
mask is the sprite's own fourth plane.

**The sprite path is four passes over the planes, and they differ:**

| pass | what it does |
| --- | --- |
| 0 | Enable Set/Reset = `0x0E`: planes 1–3 take Set/Reset = 0, plane 0 takes the CPU byte. The mask row goes into **GC 8, the bit mask** (image `0x089B5`), with a latch load before each write. Mask bit set → write plane 0 from the ink and **clear** planes 1–3. Mask bit clear → leave the byte alone. |
| 1–3 | image `0x089ED`: Enable Set/Reset off, function = **OR** (GC 3 = `0x10`), bit mask back to **`0xFF`**, one plane at a time through the Map Mask. `0xFF` means **unmasked** — every byte in the rectangle is OR-ed. |

So, per pixel:

```
mask set    dst  = ink
mask clear  dst |= ink & 0x0E
```

**The shadow path**, image `0x0909C`: Set/Reset = 0 and Enable Set/Reset =
`(bp >> 8) & 0xF` = `0xF`, driving all four planes to zero — solid black —
with the mask as its silhouette.

This is what took the menu to **100.00%, zero pixels differing at all four
ratings**. It had sat at 326 for a long time and came down to 257 by fitting
four plausible keying rules against captures; the best-scoring of those was
still wrong, and no further fitting would have reached zero from it. Reading
the routine took less time than the fitting had.

The screen had been saying so all along: sprite value 3 over background 0
gives `0 | (3 & 0xE)` = 2, and the original shows 2 exactly where keying on
"value != 0" drew 3 — 210 times.

**The plates are keyboard-only.** The menu calls INT 33h AX=0002h once — hide
cursor — and draws no pointer: moving the position to (600,40) changes nothing
near it. It polls AX=0003h 1,342 times, so it reads the *button*, and the
position reaches it and is ignored. Clicking "EXIT TO DOS" does not exit; it
starts the level, exactly as clicking bare background does. **A click anywhere
acts as F1.**

### The rating plate: Up and Down, from the dispatcher

It carries two arrow tabs rather than a function key, and it took a wrong turn
to read them. The keys came in the end from the binary, not from sweeping.

The menu reads a key at image `0x04D1A`. It scans an 0x53-byte key-state array
at `ds:0x0F78` indexed by **scancode**, finds one with bit 7 set, and
translates it through a table at **`ds:0x202E`** (image `0x0BABE`) with
`xlatb`. The dispatcher at image `0x0447B` then compares the translated code:

| scancode | key | code | goes to |
| --- | --- | --- | --- |
| `0x3B` | F1 | `0x80` | image `0x0454C` |
| `0x3C` | F2 | `0x81` | image `0x04967` |
| `0x3D` | F3 | `0x82` | the FX toggle at image `0x044B3` |
| `0x3E` | F4 | `0x83` | image `0x04A01` |
| `0x01` | Esc | `0x1B` | image `0x004D0` |
| `0x48` | **Up** | `0x8A` | **image `0x044EC` — rating up** |
| `0x29` | **`** | `0x60` | **image `0x044EC` — rating up** |
| `0x50` | **Down** | `0x8B` | **image `0x0451C` — rating down** |
| `0x0C` | **-** | `0x2D` | **image `0x0451C` — rating down** |
| `0x4A` | **keypad -** | `0x2D` | **image `0x0451C` — rating down** |

Both handlers are gated on `cmp byte [0x1f6f],4 / jne`, and `ds:0x1F6F` reads
0 on the menu, so the gate is open. Up bails when the rating is already 3
(image `0x044F9`), Down when it is already 0 (image `0x04529`) — it **caps and
floors, it does not wrap**.

**`ds:0x1F6E` is the rating** — image `0x045D5` reads it, multiplies by 6 and
copies six characters out of `"Fun   TrickyTaxingMayhem"` at `ds:0x4C01`
(image `0x0E691`) into the plate's text buffer at `ds:0x4B98`.

**`ds:0x1F6B` is the absolute level number**, and both handlers recompute it
from the new rating with four shift-and-adds:

```
0x04505  d0e0    shl al, 1      ; 2r
0x04507  8ae0    mov ah, al     ; ah = 2r
0x04509  d0e0    shl al, 1      ; 4r
0x0450b  02e0    add ah, al     ; ah = 6r
0x0450d  d0e0    shl al, 1      ; 8r
0x0450f  02e0    add ah, al     ; ah = 14r
0x04511  d0e0    shl al, 1      ; 16r
0x04513  02e0    add ah, al     ; ah = 30r
0x04515  88266b1f  mov [0x1f6b], ah
```

`ds:0x1F6B = rating * 30`. That is the first level of the new rating, and it
is the binary's own confirmation that the level order table is four blocks of
thirty rather than any other division of 120.

`tools/probe_rating.py` drives all five keys against the original and watches
`ds:0x1F6E` and `ds:0x1F6B`:

```
  gate ds:0x1F6F = 0x00 (open)
  start        rating 0 Fun     level   0
  up         0->1->2->3->3->3->3   Mayhem  ok
  down       3->2->1->0->0->0->0   Fun     ok
  `          0->0->1->2->3->3->3   Mayhem  ok
  [-]        3->2->1->0->0->0->0   Fun     ok
  `          0->1->2->3->3->3->3   Mayhem  ok
  -          3->2->1->0->0->0->0   Fun     ok
```

#### How this was wrong before, which is the useful part

This section previously read *"the rating plate answers to nothing found —
twenty-five keys have been tried"*. Three separate faults produced that, and
each is a general one:

1. **The sweep's key list did not contain Up or Down.** The prose named them as
   ruled out; the list read `left, right, pageup, pagedown, kp_minus, kp_plus,
   space, return, tab, 1, 2, 3, 4, f5, f6, plus, minus, n, r`. A key that is
   not in the list is not a key that was tried.
2. **Four of the keys that *were* listed were never delivered.** The control
   socket resolves a name with `pygame.key.key_code`, which names punctuation
   by the character — `` ` ``, `-`, `[-]`, `[+]` — so `"minus"`, `"plus"`,
   `"kp_minus"` and `"kp_plus"` came back as `error: unknown key name`. The
   sweep printed the reply as though the key had done nothing. **"Not
   delivered" and "delivered, no effect" are different findings**, and a probe
   that cannot tell them apart is not measuring.
3. **It watched pixels, from the floor.** The baseline rating is Fun = 0, where
   *down is supposed to do nothing*. Even a correct down key looks inert from
   there. Watching `ds:0x1F6E` instead of the plate's box removes the question
   entirely: a byte says what a redraw cannot.

The emulator's `KEYMAP` was also missing `` ` ``, keypad `-`, keypad `+`, `'`,
`\`, Insert and Delete — real keys with plain XT scancodes. They were added
upstream, additively.

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

### Its background is the menu's own tile, from row 78

This section previously read *"its background is unsolved — it is not the
menu's tile"*. It is the menu's tile. What was wrong was the vertical phase,
and the search that concluded otherwise was looking for a stored bitmap.

`tools/probe_blits.py --driven` settles it by logging what the original blits
rather than searching for pixels. Three pairs of blits, from `srcseg 0x587E`
offset 0 — `MAIN.DAT` section 3's start, the menu's background — 320×104, two
planes, exactly as the menu tiles it:

| x | y |
| --- | --- |
| 0, 320 | **78** |
| 0, 320 | **182** |
| 0, 320 | **286** |

Measured against a capture of the original's own briefing, over rows 78–349,
`x < 632`, background-coloured pixels: **100.00%, 0 of 156,720 differ.** Tiled
from row 0 instead — which is what the menu does — the same comparison gives
**44%**. That single 78-row offset is the whole of it.

Rows **0–77** are not the tile: they are black with the level's own preview art
over them, about 3,000 pixels in colours 8–15. That preview is not
reconstructed, and it is the only thing left on this screen.

**Palette entries 8–15** were the other half of the same "gap" and are also
settled: they are `GROUNDxO.DAT`'s **preview** palette at offset `0x408` — not
the custom palette at `0x3D8` that the level itself is drawn in. For graphics
set 0 the two differ in one byte, entry 8's red, `0x38` against `0x34`.

With both, the briefing scores **100.00% over rows 78–349** — background, text
and palette — and 98.64% over the whole screen, the remainder being the
preview.

The lesson is the same one the plates taught: a search for a *stored bitmap*
cannot find a bitmap that is stored but drawn somewhere unexpected. Logging the
blits finds it immediately, because the original says where it puts things.

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
buffer and shifted right one bit at a time**, then blitted byte-aligned.

The blit then spans **one byte more than the sprite is wide**: the plates are
120 pixels — 15 bytes — and sit at x values that are 4 mod 8, and their blits
cover 16 columns. Shifting right by four pushes the sprite's rightmost bits
into an extra byte, which has to be written too.

Reproducing the shift in the port is **not** worth it: simulating it gains 21
pixels across the three plates that have nothing drawn over them, and costs a
second blit path. The port blits per pixel from the unshifted artwork, which is
identical in the middle of a sprite and near enough at its edges. That
is why the blitter's own destinations are always byte-aligned even though
sprites plainly are not — the FX label sits at x=351, which is 7 mod 8.

It also explains the rating name's one-pixel offset: the plate's bitmap carries
a `FUN` at one position and the original overdraws the current rating from a
pre-shifted copy at another. The scratch buffer is what the blit reads, so
following the blit's source address finds the buffer and not the artwork.

### The four rating sprites, and which way round they go

They were found by searching for them instead. With the rating now settable —
see *The rating plate* above — the original can be driven to each of the four
in turn, its plate cropped out of the capture at (486,145), and section 4
searched for the 72×27 four-plane block that decodes to it. Every match is
exact:

| rating | offset in section 4 | pixels differing |
| --- | --- | --- |
| Mayhem | `23168` (`0x5A80`) | **0** |
| Taxing | `24140` (`0x5E4C`) | **0** |
| Tricky | `25112` (`0x6218`) | **0** |
| Fun | `26084` (`0x65E4`) | **0** |

A uniform 972 apart — `RATING_PLANE * 4` — and **descending**: the four are
stored in reverse, Mayhem first. Cross-checked against every capture, the
matrix is a clean diagonal, each capture 0 against its own sprite and 616–736
against the other three.

The rating-dependent content is reproduced **exactly**. Inside the plate's box
the original changes **713** pixels between Fun and Mayhem; the port changes
the same **713**.

Whole-screen scores run 99.41% (Fun, Tricky, Taxing) to 99.04% (Mayhem), and
that spread is **the animated banner alone** — successive captures of the same
rating vary by as much. The plate's box holds a **70-pixel residual identical
at every rating**, which belongs to the unexplained plate pixels recorded
above, not to the rating.

The direction is the part worth stating plainly, because it hid for a long
time behind a coincidence: **at rating 0 the sign of the step does not
matter**, and rating 0 was the only rating that could be reached. The port had
`RATING_BASE + rating * RATING_STEP` with `RATING_BASE` set to Fun, which is
correct for exactly one of the four values it takes. Nothing was available to
contradict it until the plate could be moved.

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
| the briefing | **100.00%** — 0 of 224,000: background, text, palette and the level preview |
| the menu | 99.41% — 1,321 of 224,000, of which 995 are the animated banner |

So two of the four match the original pixel for pixel, and the menu matches
everything on it that holds still to within about 320 pixels.
