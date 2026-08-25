/* SPDX-License-Identifier: GPL-2.0-only
 *
 * The main menu screen.
 *
 * PROVENANCE: the geometry below was measured against the original, not taken
 * from a format description - there is no published one for this. The method
 * was the project's usual: drive the original under the emulator to its menu,
 * dump the composed 640x350 screen, then find each piece in MAIN.DAT by
 * packing runs of screen pixels back into plane bytes and searching for them.
 * Consecutive matching rows give the row stride, hence the width; the distance
 * between the same row's planes gives the plane stride, hence the height.
 *
 * The menu runs in EGA mode 10h - 640x350, four planes, 16 colours - which is
 * a different mode from the game itself (13h, 320x200 chunky). That is why
 * everything here is planar and nothing is shared with render.c.
 *
 * What is verified, against a captured screen:
 *   background   the tile, its size, and that it repeats from (0,0)
 *   logo         size, position, plane layout, and value 0 meaning transparent
 *   signs        all six, each matched to its slot; a clean diagonal of
 *                86.7%-95.4% against every other slot's 39%-78%
 *   palette      all 16 entries, byte for byte
 *
 * What is NOT here, and would be dishonest to imply otherwise:
 *   - the two credit lines ("(C)MCMXCI Psygnosis Ltd" / "A DMA Design Game")
 *     and the scrolling banner at the foot. Both are drawn glyph by glyph from
 *     a font that has not been located yet; they are not stored bitmaps, which
 *     is established - searching every section for their pixel runs at all
 *     eight bit alignments finds nothing consistent.
 *   - the lemmings' eye-blink animation, and the sign highlight under the
 *     mouse. The menu is composed here, not driven.
 *
 * With those excluded the composition is 94.67% identical to the original's;
 * over the whole screen, including the parts not implemented, 88.77%.
 * tools/verify_menu.py is the check.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dat.h"
#include "menu.h"

/* MAIN.DAT section 3 holds the whole menu. */
#define MENU_SECTION 3

/* The background tile. Two planes only - it uses four colours - so values are
 * 0..3 and planes 2 and 3 are implicitly zero. Confirmed by measurement:
 * assembling it from four consecutive 4160-byte blocks scores 39%, and
 * forcing planes 2 and 3 to zero scores 95.6%. */
#define BG_W      320
#define BG_H      104
#define BG_STRIDE 40
#define BG_PLANE0 0
#define BG_PLANE1 4160

/* The logo, at screen (8,10). 632 wide, so it ends exactly at x=639. */
#define LOGO_BASE   8320
#define LOGO_W      632
#define LOGO_H      94
#define LOGO_STRIDE 79
#define LOGO_PLANE  7426
#define LOGO_X      8
#define LOGO_Y      10

/* The six signs, one atlas entry each, 4 x 915 = 3660 bytes apart. The atlas
 * begins at 38024, which is exactly where the logo's fourth plane ends. */
#define SIGN_BASE   38024
#define SIGN_W      120
#define SIGN_H      61
#define SIGN_STRIDE 15
#define SIGN_PLANE  915
#define SIGN_STEP   3660

static const struct { uint16_t x, y; } SIGN_POS[6] = {
    {  68, 120 },   /* F1  1 PLAYER   */
    { 196, 120 },   /* F2  NEW LEVEL  */
    { 324, 120 },   /* F3  FX         */
    { 452, 120 },   /* F4  FUN        */
    { 196, 197 },   /* ESC EXIT TO DOS*/
    { 324, 197 },   /* F4  ?          */
};

/* The menu palette, transcribed from image 0x0E3C0 - sixteen 6-bit RGB
 * triples, in attribute-register order. It is a table in the executable, not
 * in any data file, so it is transcribed here the way a routine would be.
 *
 * These go to DAC entries 0-5, 0x14, 7, 0x38-0x3F, which is the BIOS default
 * attribute palette for mode 10h - the game does not reprogram it. That
 * mapping matters only when comparing against the emulator's own screen dump,
 * which reports DAC indices; the port keeps the 4-bit values. */
static const uint8_t MENU_PAL6[48] = {
    0x00,0x00,0x00,  0x20,0x10,0x08,  0x18,0x0c,0x08,  0x0c,0x00,0x04,
    0x08,0x02,0x1f,  0x10,0x0b,0x24,  0x1a,0x16,0x29,  0x26,0x23,0x2f,
    0x00,0x14,0x00,  0x00,0x18,0x04,  0x00,0x1c,0x08,  0x00,0x20,0x10,
    0x34,0x34,0x34,  0x2c,0x2c,0x00,  0x10,0x14,0x2c,  0x38,0x20,0x24,
};

/* 6 bits to 8 by REPLICATING the top bits, not by scaling to 255. 0x3F must
 * come out 255 and 0x00 must come out 0, and v*255/63 gets both right and
 * everything between subtly wrong. */
static uint8_t dac8(uint8_t v)
{
    v &= 0x3F;
    return (uint8_t)((v << 2) | (v >> 4));
}

/* One pixel out of a planar bitmap. `planes` is how far apart consecutive
 * planes are; pass 0 for the two-plane background and only bits 0 and 1 are
 * read. The leftmost pixel of a byte is its most significant bit. */
static uint8_t planar(const uint8_t *sec, uint32_t base, uint32_t plane_step,
                      uint16_t stride, uint8_t nplanes,
                      uint16_t x, uint16_t y)
{
    uint8_t v = 0;
    for (uint8_t p = 0; p < nplanes; p++) {
        uint32_t o = base + p * plane_step + (uint32_t)y * stride + (x >> 3);
        v = (uint8_t)(v | (((sec[o] >> (7 - (x & 7))) & 1u) << p));
    }
    return v;
}

/* Blit a 4-plane sprite, treating value 0 as transparent. That the logo and
 * the signs are keyed on 0 rather than drawn opaquely is measured: drawn
 * opaquely the logo's own bounding box scores 35%, keyed it scores 95%+, and
 * the background texture is plainly visible between the letters. */
static void blit(uint8_t *dst, const uint8_t *sec, uint32_t base,
                 uint16_t w, uint16_t h, uint16_t stride, uint32_t plane,
                 uint16_t at_x, uint16_t at_y)
{
    for (uint16_t sy = 0; sy < h; sy++) {
        uint16_t y = at_y + sy;
        if (y >= MENU_H) break;
        for (uint16_t sx = 0; sx < w; sx++) {
            uint16_t x = at_x + sx;
            if (x >= MENU_W) break;
            uint8_t v = planar(sec, base, plane, stride, 4, sx, sy);
            if (v) dst[(size_t)y * MENU_W + x] = v;
        }
    }
}

int menu_build(const char *dir, menu_screen *m, const char **err)
{
    memset(m, 0, sizeof(*m));

    char path[1024];
    snprintf(path, sizeof(path), "%s/main.dat", dir);
    dat_file d;
    if (dat_load(path, &d, err) != 0)
        return -1;
    if (d.count <= MENU_SECTION) {
        dat_free(&d);
        if (err) *err = "main.dat has no menu section";
        return -1;
    }
    const uint8_t *sec = d.sections[MENU_SECTION].data;
    uint32_t seclen = d.sections[MENU_SECTION].size;
    if (seclen < SIGN_BASE + 6u * SIGN_STEP) {
        dat_free(&d);
        if (err) *err = "menu section is shorter than the menu needs";
        return -1;
    }

    m->pix = calloc((size_t)MENU_W * MENU_H, 1);
    if (!m->pix) {
        dat_free(&d);
        if (err) *err = "out of memory";
        return -1;
    }
    for (uint8_t i = 0; i < 16; i++) {
        m->palette[i].r = dac8(MENU_PAL6[i * 3 + 0]);
        m->palette[i].g = dac8(MENU_PAL6[i * 3 + 1]);
        m->palette[i].b = dac8(MENU_PAL6[i * 3 + 2]);
    }

    /* The background, tiled from (0,0). 640 is exactly two tiles across; the
     * vertical repeat at 104 does not divide 350, so the bottom tile is cut
     * off mid-way, which is what the original shows too. */
    for (uint16_t y = 0; y < MENU_H; y++)
        for (uint16_t x = 0; x < MENU_W; x++)
            m->pix[(size_t)y * MENU_W + x] =
                planar(sec, BG_PLANE0, BG_PLANE1 - BG_PLANE0, BG_STRIDE, 2,
                       x % BG_W, y % BG_H);

    blit(m->pix, sec, LOGO_BASE, LOGO_W, LOGO_H, LOGO_STRIDE, LOGO_PLANE,
         LOGO_X, LOGO_Y);
    for (uint8_t i = 0; i < 6; i++)
        blit(m->pix, sec, SIGN_BASE + (uint32_t)i * SIGN_STEP,
             SIGN_W, SIGN_H, SIGN_STRIDE, SIGN_PLANE,
             SIGN_POS[i].x, SIGN_POS[i].y);

    dat_free(&d);
    return 0;
}

void menu_free(menu_screen *m)
{
    if (!m) return;
    free(m->pix);
    m->pix = NULL;
}
