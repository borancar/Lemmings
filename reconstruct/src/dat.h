/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Lemmings DAT containers: several independently compressed sections, one
 * after another, each with its own 10-byte header.
 */
#ifndef DAT_H
#define DAT_H

#include "lem.h"

typedef struct {
    uint8_t *data;      /* decompressed bytes, owned */
    uint32_t size;
} dat_section;

typedef struct {
    dat_section *sections;
    uint32_t count;
} dat_file;

/* Read and decompress every section of `path`. Returns 0 on success.
 * On failure `err` (if not NULL) gets a static reason string. */
int dat_load(const char *path, dat_file *out, const char **err);
void dat_free(dat_file *d);

#endif /* DAT_H */
