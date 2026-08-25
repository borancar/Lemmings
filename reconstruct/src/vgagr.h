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
/* Which bit of the pixel value each STORED plane contributes. The published
 * description says plane i adds 1<<i; this is selectable so that claim can be
 * tested against the original rather than believed. Default {0,1,2,3}. */
extern uint8_t g_plane_bits[4];

/* Which end of a byte the leftmost pixel is. 1 = the high bit, which is what
 * every published description says; selectable because inside a solid area it
 * makes no difference at all, so believing it is cheap and checking it is
 * cheaper than the bug it hides. */
extern uint8_t g_msb_first;

int vgagr_decode(const uint8_t *sec, uint32_t len, uint32_t loc,
                 uint8_t w, uint8_t h, uint8_t *out);

/* Decode a single monochrome plane - an object's transparency mask, which is
 * stored separately because objects use all sixteen colours and so cannot
 * borrow the fourth bitplane as terrain does. 1 means "draw this pixel". */
int vgagr_decode_mask(const uint8_t *sec, uint32_t len, uint32_t loc,
                      uint8_t w, uint8_t h, uint8_t *out);

#endif /* VGAGR_H */
