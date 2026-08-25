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
