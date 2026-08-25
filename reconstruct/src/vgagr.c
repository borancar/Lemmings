/* SPDX-License-Identifier: GPL-2.0-only
 *
 * PROVENANCE: written from the published description at camanis.net.
 * Not transcribed from the original - see STATUS.md.
 *
 * Row stride is (w+7)/8 bytes. The published note is self-contradictory here,
 * saying both "W x H bits" (no padding) and "byte-aligned rows" (padding).
 * It cannot be told apart from this game's data: every terrain bitmap in
 * ground0o.dat is 32, 48, 80 or 112 pixels wide, all multiples of 8, where
 * the two readings are identical - checked across all 50 defined entries by
 * comparing mask_loc - image_loc against both. (w+7)/8 is the safer of the
 * two and agrees everywhere it can be tested.
 */
#include <string.h>

#include "vgagr.h"

int vgagr_decode(const uint8_t *sec, uint32_t len, uint32_t loc,
                 uint8_t w, uint8_t h, uint8_t *out)
{
    uint32_t stride = ((uint32_t)w + 7u) / 8u;
    uint32_t plane  = stride * (uint32_t)h;

    if (loc + plane * 4u > len)
        return -1;

    memset(out, 0, (size_t)w * h);

    for (uint8_t p = 0; p < 4; p++) {
        const uint8_t *bits = sec + loc + (uint32_t)p * plane;
        for (uint8_t y = 0; y < h; y++) {
            const uint8_t *row = bits + (uint32_t)y * stride;
            for (uint8_t x = 0; x < w; x++) {
                /* The leftmost pixel of a row is the byte's high bit. */
                uint8_t bit = (uint8_t)((row[x >> 3] >> (7 - (x & 7))) & 1u);
                out[(uint32_t)y * w + x] |= (uint8_t)(bit << p);
            }
        }
    }
    return 0;
}
