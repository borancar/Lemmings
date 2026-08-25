/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Composing a level's terrain into the picture the player sees.
 */
#ifndef RENDER_H
#define RENDER_H

#include "lem.h"
#include "ground.h"
#include "level.h"

typedef struct {
    uint8_t *pix;        /* LEVEL_W * LEVEL_H palette indices; 0 = empty */
    uint16_t w, h;
} canvas;

/* How a terrain pixel combines with what is already on the canvas. The
 * original is not simply "replace": which of these it is, is being settled
 * against its video memory rather than assumed, so they are selectable.
 * See STATUS.md in the development repository. */
typedef enum {
    TERRAIN_REPLACE = 0,   /* the newer pixel wins */
    TERRAIN_OR,            /* colour bits are OR'd together */
    TERRAIN_KEEP,          /* the older pixel wins wherever there is one */
    TERRAIN_NOV_IGNORE,    /* replace, ignoring the no-overwrite flag */
    TERRAIN_NOV_INVERT,    /* no-overwrite means "only where terrain is" */
    TERRAIN_OR_ALL         /* every piece ORs, no-overwrite included */
} terrain_mode;

extern terrain_mode g_terrain_mode;

/* Draw the terrain list back to front. The list order is the drawing order in
 * every description of the format; selectable so that can be checked rather
 * than assumed, since it interacts with the no-overwrite flag. */
extern uint8_t g_terrain_reverse;

int  canvas_init(canvas *c);
void canvas_free(canvas *c);

/* Draw every terrain piece of `lv`, in file order - the order is the
 * drawing order and the "do not overwrite" and "erase" flags depend on it. */
int render_terrain(canvas *c, const level *lv, const ground *g,
                   const uint8_t *terrain_sec, uint32_t terrain_len,
                   const char **err);

/* Draw the level's interactive objects - the entrance, the exit, traps.
 * Terrain must already be composed: an object's "no overwrite" flag is about
 * the terrain underneath it. `frame` selects an animation frame; 0 is the
 * resting pose the level opens on. */
int render_objects(canvas *c, const level *lv, const ground *g,
                   const uint8_t *object_sec, uint32_t object_len,
                   uint8_t frame, const char **err);

#endif /* RENDER_H */
