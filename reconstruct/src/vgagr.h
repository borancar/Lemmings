/* SPDX-License-Identifier: GPL-2.0-only
 *
 * VGAGRx.DAT bitmaps: 4-bit planar, four monochrome planes one after
 * another, plane p contributing 1<<p to the pixel value.
 */
#ifndef VGAGR_H
#define VGAGR_H

#include "lem.h"

/* Decode one planar bitmap into 8-bit indices, one byte per pixel.
 * `out` must have room for w*h bytes. Pixel 0 means "not drawn": terrain
 * only ever uses colours 8-15, so plane 3 doubles as the shape's mask.
 * Returns 0 on success, -1 if the bitmap would read past `len`. */
int vgagr_decode(const uint8_t *sec, uint32_t len, uint32_t loc,
                 uint8_t w, uint8_t h, uint8_t *out);

#endif /* VGAGR_H */
