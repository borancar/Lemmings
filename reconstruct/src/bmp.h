/* SPDX-License-Identifier: GPL-2.0-only
 *
 * A 24-bit BMP writer, so the port can show its work without pulling in an
 * image library. BMP because it needs no compression and no dependencies.
 */
#ifndef BMP_H
#define BMP_H

#include "lem.h"

int bmp_write(const char *path, const uint8_t *indices,
              uint16_t w, uint16_t h, const lem_rgb *palette,
              const char **err);

#endif /* BMP_H */
