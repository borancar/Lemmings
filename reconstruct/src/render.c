/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Terrain composition.
 *
 * The level lists its terrain pieces in drawing order, and three flags on
 * each piece change what a pixel does when it lands on one already drawn:
 *
 *   no_overwrite - only write where the canvas is still empty
 *   erase        - clear the canvas where this shape is solid, drawing nothing
 *   upside_down  - read the source rows bottom to top
 *
 * A pixel is "solid" when its 4-bit value has bit 3 set. Terrain is drawn
 * only in colours 8-15, so the fourth bitplane is the shape's own mask and
 * there is no separate transparency to look up.
 */
#include <stdlib.h>
#include <string.h>

#include "render.h"
#include "vgagr.h"

int canvas_init(canvas *c)
{
    c->w = LEVEL_W;
    c->h = LEVEL_H;
    c->pix = calloc((size_t)c->w * c->h, 1);
    return c->pix ? 0 : -1;
}

void canvas_free(canvas *c)
{
    free(c->pix);
    c->pix = NULL;
}

int render_terrain(canvas *c, const level *lv, const ground *g,
                   const uint8_t *sec, uint32_t sec_len, const char **err)
{
    /* The largest terrain bitmap in any shipped set comfortably fits this. */
    uint8_t *tile = malloc(256u * 256u);
    if (!tile) { if (err) *err = "out of memory"; return -1; }

    for (uint16_t i = 0; i < lv->num_terrain; i++) {
        const level_terrain *t = &lv->terrain[i];
        const ground_terrain *gt = &g->terrain[t->id];

        if (!gt->width || !gt->height)
            continue;               /* a set need not define all 64 ids */

        if (vgagr_decode(sec, sec_len, gt->image_loc,
                         gt->width, gt->height, tile) != 0) {
            free(tile);
            if (err) *err = "terrain bitmap runs past the end of the section";
            return -1;
        }

        for (uint8_t sy = 0; sy < gt->height; sy++) {
            int32_t dy = t->y + sy;
            if (dy < 0 || dy >= c->h)
                continue;
            uint8_t src_y = t->upside_down
                          ? (uint8_t)(gt->height - 1u - sy) : sy;

            for (uint8_t sx = 0; sx < gt->width; sx++) {
                int32_t dx = t->x + sx;
                if (dx < 0 || dx >= c->w)
                    continue;

                uint8_t v = tile[(uint32_t)src_y * gt->width + sx];
                if (!(v & 0x08))
                    continue;       /* transparent */

                uint8_t *dst = &c->pix[(uint32_t)dy * c->w + dx];
                if (t->erase)
                    *dst = 0;
                else if (t->no_overwrite) {
                    if (*dst == 0)
                        *dst = v;
                } else
                    *dst = v;
            }
        }
    }
    free(tile);
    return 0;
}
