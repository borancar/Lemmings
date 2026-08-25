/* SPDX-License-Identifier: GPL-2.0-only
 *
 * PROVENANCE: written from the published description at camanis.net, checked
 * against ground0o.dat. Not transcribed from the original - see STATUS.md.
 *
 * Note the endianness changes here: the *level* files are big-endian Amiga
 * leftovers, but the ground files' words are little-endian. That is not a
 * mistake in the reading - image offsets read little-endian line up exactly
 * with the differences between consecutive bitmaps, and big-endian ones do
 * not.
 */
#include <stdio.h>
#include <string.h>

#include "ground.h"

#define GROUND_SIZE 1056

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* The file stores 6-bit DAC values, as the VGA hardware took them. */
static uint8_t widen6(uint8_t v)
{
    v = (uint8_t)(v & 0x3F);
    return (uint8_t)((v * 255u) / 63u);
}

int ground_load(const char *path, ground *g, const char **err)
{
    uint8_t buf[GROUND_SIZE];

    FILE *f = fopen(path, "rb");
    if (!f) { if (err) *err = "cannot open ground file"; return -1; }
    size_t got = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (got != sizeof(buf)) {
        if (err) *err = "ground file is not 1056 bytes";
        return -1;
    }
    memset(g, 0, sizeof(*g));

    /* 16 object entries of 28 bytes, from 0x000. */
    for (uint8_t i = 0; i < GROUND_OBJECTS; i++) {
        const uint8_t *e = buf + i * 28;
        ground_object *o = &g->objects[i];
        o->animation_flags = le16(e + 0x00);
        o->start_frame     = e[0x02];
        o->end_frame       = e[0x03];
        o->width           = e[0x04];
        o->height          = e[0x05];
        o->frame_data_size = le16(e + 0x06);
        o->mask_offset     = le16(e + 0x08);
        o->trigger_effect  = e[0x14];
        o->frames_base_loc = le16(e + 0x15);
        o->preview_frame   = e[0x17];
    }

    /* 64 terrain entries of 8 bytes, from 0x1C0. */
    for (uint8_t i = 0; i < GROUND_TERRAIN; i++) {
        const uint8_t *e = buf + 0x1C0 + i * 8;
        ground_terrain *t = &g->terrain[i];
        t->width     = e[0x00];
        t->height    = e[0x01];
        t->image_loc = le16(e + 0x02);
        t->mask_loc  = le16(e + 0x04);
    }

    /* Palettes from 0x3C0: three EGA sets of 8 bytes, then three VGA sets of
     * 8 entries x 3 bytes. The 16-colour physical palette is the standard
     * eight followed by this set's custom eight; terrain only ever uses the
     * upper eight, which is why its fourth bitplane doubles as the mask. */
    const uint8_t *vga_custom   = buf + 0x3D8;
    const uint8_t *vga_standard = buf + 0x3F0;
    for (uint8_t i = 0; i < 8; i++) {
        g->palette[i].r = widen6(vga_standard[i * 3 + 0]);
        g->palette[i].g = widen6(vga_standard[i * 3 + 1]);
        g->palette[i].b = widen6(vga_standard[i * 3 + 2]);
        g->palette[8 + i].r = widen6(vga_custom[i * 3 + 0]);
        g->palette[8 + i].g = widen6(vga_custom[i * 3 + 1]);
        g->palette[8 + i].b = widen6(vga_custom[i * 3 + 2]);
    }
    return 0;
}
