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
/* One section, decompressed. Exposed so tools/verify_dat.py can run exactly
 * this code against the original's own body on the same call - see the
 * PROVENANCE note in dat.c. Transcribed from image 0x01BAF. */
int dat_decompress(const uint8_t *comp, uint32_t comp_len,
                   uint8_t bits_in_first, uint8_t checksum,
                   uint8_t *out, uint32_t out_len, const char **err);

int dat_load(const char *path, dat_file *out, const char **err);
void dat_free(dat_file *d);

#endif /* DAT_H */
