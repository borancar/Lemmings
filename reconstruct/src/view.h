/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Showing the game on screen, through SDL3.
 *
 * SDL3 is required, not optional - see the note at the top of view.c. There is
 * deliberately no second, platform-specific path and no headless fallback:
 * either would drift from this one and never be looked at.
 */
#ifndef LEM_VIEW_H_
#define LEM_VIEW_H_

#include "lem.h"
#include "render.h"

/* Open a window on an 8-bit indexed buffer and let the viewer scroll it.
 * `view_w`/`view_h` is the viewport onto a `src_w`x`src_h` picture; pass the
 * full size for a screen that is not scrolled. Returns 0 when it is closed. */
int view_show(const uint8_t *pix, uint16_t src_w, uint16_t src_h,
              uint16_t view_w, uint16_t view_h,
              const lem_rgb *palette, const char *title,
              uint16_t start_x, uint8_t scale);

/* The level, in the original's own 320x160 viewport. */
int view_run(const canvas *c, const lem_rgb *palette, const char *title,
             uint16_t start_x, uint8_t scale);

#endif /* LEM_VIEW_H_ */
