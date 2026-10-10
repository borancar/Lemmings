# The page model (transcribed)

The original never recomposes the play area. It keeps two pages in video
memory, draws into one while the CRTC shows the other, and refreshes only
what something marked. The port does the same; the code is the page-pair
section of `reconstruct/src/game.c` (`level_page_refresh` and its neighbours).

    master   0x4A40, 0xD0-byte rows, far wider than the screen. The truth.
    pages    ds:0x1F76 (drawn) and ds:0x1F78 (shown), 0 and 0x2520 at
             image 0x081CC, swapped every frame at image 0x00105.
    flags    one array PER PAGE, in the stack segment at ss:0x42B2 and
             ss:0x45D3, 0x321 bytes each: byte 0 is REDRAW EVERYTHING, then
             20 x 40 tiles of 8 x 8. ds:0x26 always points at the drawn
             page's array; image 0x0010F swaps it with ds:0x28.

Per frame, image 0x0018F:

    0x001A7  image 0x080FD - copy the flagged tiles master -> drawn page,
             or the whole window if byte 0 is set, then clear the tiles.
             A run that ends on an unflagged tile copies that tile too.
    0x001B3  the lemmings step (the terrain skills write the MASTER)
    0x001B9  the objects drawn on the page; the blitter marks each one's
             rectangle at image 0x0885B -> image 0x09A15 while ds:0x2A is 0xFF
    0x001BC  the lemmings drawn on the page; image 0x03654 calls image
             0x05726 first, which marks 3 tiles wide (5 for the cloud) from
             8 rows above the hot spot (16 with a fuse)
             - the bomber's debris marks one tile a pixel, image 0x06F01

Who sets redraw-everything: image 0x0413B, inside image 0x0410F, after every
scroll - the edge (image 0x04100, image 0x0416A) and the minimap (image
0x03FDE) - on both arrays. And image 0x081CC for a new level.

What does NOT mark: anything writing the master directly. A digger, basher,
miner or bomb reaches a page only where some sprite marked the tile.

## Two things this file used to say that were wrong

- **Image 0x07178 is the CURSOR's save and restore**, not each lemming's.
  Image 0x05827 calls it right after the flip; it restores exactly what it
  saved, so the cursor leaves nothing on a page and the port drawing it last
  is the same picture.
- **The one-pixel residue at screen (159, 81) was not a stale tile.** With
  the page transcribed, the port's page held 0 there just as the original's
  did, and the 2 stayed. It is the cursor's second pass, image 0x074AF, ORing
  plane 1 unmasked over the box's corner pixels. See `level_cursor_draw`.

What the page model DID change: the nuke went from 541 to 558 of 558 frames
exact - its craters are the master changing under tiles nothing marked.
