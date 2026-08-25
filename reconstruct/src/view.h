/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Showing a level on screen. Optional: built only when X11 is present, and
 * the port still renders to a file without it.
 */
#ifndef LEM_VIEW_H_
#define LEM_VIEW_H_

#include "lem.h"
#include "render.h"

/* Open a window on the composed level and let the player scroll it.
 * Returns 0 when the window is closed, -1 if no display could be opened. */
int view_run(const canvas *c, const lem_rgb *palette, const char *title,
             uint16_t start_x, uint8_t scale);

#endif /* LEM_VIEW_H_ */
