/* SPDX-License-Identifier: GPL-2.0-only
 *
 * PC Lemmings, reconstructed. Shared types.
 *
 * stdint types only, everywhere - see CLAUDE.md in the development
 * repository. This is 16-bit code originally, where every value has a width
 * the original depended on, and `int16_t` says "this truncation is the
 * original's" in a way `short` does not.
 */
#ifndef LEM_H
#define LEM_H

#include <stdint.h>
#include <stddef.h>

/* A level's terrain canvas. The original addresses x from -16 to 1583. */
#define LEVEL_W 1584
#define LEVEL_H 160

/* One 16-colour palette entry, already widened from the file's 6 bits. */
typedef struct {
    uint8_t r, g, b;
} lem_rgb;

#endif /* LEM_H */
