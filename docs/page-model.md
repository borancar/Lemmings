# The page model, as read (owed work)

The original never recomposes the play area. It keeps a PAGE and refreshes
only what something marked.

  master   ds:0x4A40, 0xD0-byte rows, far wider than the screen. The truth.
  page     ds:0x1F76 + 0x2C2 (and + 0x27E2), 0x2C = 44-byte rows, mode 0x0D.
  flags    ss:[ds:0x26] + 1, 20 rows x 40 cols of 8x8 tiles = 0x320 bytes.
           Byte 0 (at ds:0x26 itself) is REDRAW EVERYTHING.

Per frame, image 0x018F:

  0x001A7  image 0x080FD.  If ss:[ds:0x26] is nonzero, the whole-page path at
           image 0x0819D. Otherwise 20 outer passes, each `repne scasb` for a
           1 over 40 bytes, then `repe scasb` for the run's length; each run
           copies those tiles master -> page. bp += 0x160 (8 page rows of 44)
           and si += 0x680 (8 master rows of 0xD0) per outer pass. Ends at
           image 0x08153 clearing all 0x320 flags.
  0x001B3  the lemmings step
  0x001BC  the lemmings draw, onto the PAGE, each preceded by image 0x07178's
           save of a 24 x 16 block (3 bytes x 16 rows, di += 0x29, stride
           0x2C) and followed next frame by its restore. UNMASKED, so it
           carries three columns of history either side of the sprite.
  0x001C2  the panel

Who marks:

  image 0x09A15  takes a rectangle, y in ax biased by 0x10, sets 1s in
                 ss:[si]. ONE caller per the code map: image 0x0885B, inside
                 the blitter, gated on ds:0x2A != 0xFF.
  image 0x0413B  writes 0xFF to ss:[ds:0x26] - redraw everything. It sits
                 inside the scroll at image 0x04085, which is what a scroll
                 must do to a page that is only 320 wide.

What does NOT mark: anything writing the master directly. The digger's clear
at image 0x05EBA is a bare stosb run with no call in it, so the page keeps
its old tile until some blit dirties it. THAT IS THE ORIGINAL'S BEHAVIOUR and
it is the whole of the port's remaining play-area residue - one pixel, canvas
(799, 81), measured by tools/probe_page_history.py.

## What the port does instead

level_frame_tick does `memcpy(pix, a->from->pix, w * h)` - the whole master
into a working canvas the full canvas width - draws the lemmings into THAT,
and windows 320 of it to the screen. No page, no flags, no save/restore, and
no history to be stale.

## The change

  1. a persistent 44-byte-row page, not a working copy of the master
  2. the 20x40 flag grid and the redraw-everything byte
  3. image 0x0413B: the scroll sets redraw-everything
  4. image 0x09A15 called from the port's blitter, gated on its ds:0x2A
  5. image 0x080FD's tile copy in place of the memcpy
  6. image 0x07178's save/restore around each lemming draw, on the page

Measure after each: tools/compare_play.py (play 568/671 now, panel 671/671)
and the recorded playthrough (534/534 both halves). Anything that falls is a
step to revert, not to tune.
