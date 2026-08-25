/* SPDX-License-Identifier: GPL-2.0-only
 *
 * GROUNDxO.DAT - the per-graphics-set index: what terrain and object
 * bitmaps exist, how big they are, where they live in VGAGRx.DAT, and the
 * palette they are drawn in. Exactly 1056 bytes, and NOT compressed.
 */
#ifndef GROUND_H
#define GROUND_H

#include "lem.h"

#define GROUND_OBJECTS 16
#define GROUND_TERRAIN 64

typedef struct {
    uint8_t  width, height;
    uint16_t image_loc;      /* offset into VGAGRx.DAT section 0 */
    uint16_t mask_loc;
} ground_terrain;

typedef struct {
    uint16_t animation_flags;
    uint8_t  start_frame, end_frame;
    uint8_t  width, height;
    uint16_t frame_data_size;
    uint16_t mask_offset;
    uint16_t frames_base_loc;   /* offset into VGAGRx.DAT section 1 */
    uint8_t  preview_frame;
    uint8_t  trigger_effect;
} ground_object;

typedef struct {
    ground_terrain terrain[GROUND_TERRAIN];
    ground_object  objects[GROUND_OBJECTS];
    lem_rgb        palette[16];
} ground;

int ground_load(const char *path, ground *out, const char **err);

#endif /* GROUND_H */
