/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bmp.h"

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

int bmp_write(const char *path, const uint8_t *indices,
              uint16_t w, uint16_t h, const lem_rgb *palette,
              const char **err)
{
    uint32_t row = ((uint32_t)w * 3u + 3u) & ~3u;   /* rows pad to 4 bytes */
    uint32_t pix_bytes = row * h;

    uint8_t hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    put32(hdr + 2, 54u + pix_bytes);
    put32(hdr + 10, 54u);
    put32(hdr + 14, 40u);
    put32(hdr + 18, w);
    put32(hdr + 22, h);
    put16(hdr + 26, 1);
    put16(hdr + 28, 24);
    put32(hdr + 34, pix_bytes);

    FILE *f = fopen(path, "wb");
    if (!f) { if (err) *err = "cannot open output file"; return -1; }
    fwrite(hdr, 1, sizeof(hdr), f);

    uint8_t *line = calloc(row, 1);
    if (!line) { fclose(f); if (err) *err = "out of memory"; return -1; }

    /* BMP rows run bottom to top. */
    for (int32_t y = (int32_t)h - 1; y >= 0; y--) {
        memset(line, 0, row);
        for (uint16_t x = 0; x < w; x++) {
            lem_rgb c = palette[indices[(uint32_t)y * w + x] & 0x0F];
            line[x * 3 + 0] = c.b;
            line[x * 3 + 1] = c.g;
            line[x * 3 + 2] = c.r;
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    return 0;
}
