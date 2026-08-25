/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Lemmings DAT decompression.
 *
 * PROVENANCE: written from the published format description at
 * camanis.net/lemmings/files/docs/lemmings_dat_file_format.txt, not
 * transcribed from the original's disassembly, and therefore NOT yet checked
 * against the original the way a transcribed routine must be. See STATUS.md.
 *
 * Two things that description gets wrong or leaves ambiguous, both settled
 * against real files:
 *
 *   - The header's words are BIG-endian. level000.dat begins
 *     03 d8 00 00 08 00 ... and the decompressed size reads as 0x0800 = 2048,
 *     one level, big-endian - against a nonsensical 8 little-endian.
 *   - Bits come out of each byte LSB first, while multi-bit values are
 *     assembled MSB first. Both halves matter: any other combination decodes
 *     a few opcodes and then references data past the end of the output.
 *
 * The stream is consumed from its last byte backwards and the output is
 * rebuilt from its end backwards, which is why a back-reference reads from
 * *higher* addresses - those are the bytes already written.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dat.h"

typedef struct {
    const uint8_t *data;
    int32_t pos;         /* byte index, walking backwards */
    uint8_t avail;       /* bits still usable in data[pos] */
    uint8_t idx;         /* bits taken from data[pos] so far */
    int overrun;
} bitreader;

static uint8_t get_bit(bitreader *b)
{
    if (b->idx >= b->avail) {
        b->pos--;
        b->idx = 0;
        b->avail = 8;
        if (b->pos < 0) {
            b->overrun = 1;
            return 0;
        }
    }
    uint8_t v = (uint8_t)((b->data[b->pos] >> b->idx) & 1u);
    b->idx++;
    return v;
}

static uint32_t get_bits(bitreader *b, uint8_t n)
{
    uint32_t v = 0;
    while (n--)
        v = (v << 1) | get_bit(b);
    return v;
}

static int decompress(const uint8_t *comp, uint32_t comp_len,
                      uint8_t bits_in_first, uint8_t checksum,
                      uint8_t *out, uint32_t out_len, const char **err)
{
    uint8_t sum = 0;
    for (uint32_t i = 0; i < comp_len; i++)
        sum ^= comp[i];
    if (sum != checksum) {
        if (err) *err = "section checksum mismatch";
        return -1;
    }

    bitreader b = { comp, (int32_t)comp_len - 1, bits_in_first, 0, 0 };
    uint32_t o = out_len;

    while (o > 0 && !b.overrun) {
        uint32_t n, off;
        if (get_bit(&b)) {
            switch (get_bits(&b, 2)) {
            case 0:  n = 3; off = get_bits(&b, 9)  + 1; break;   /* 100 */
            case 1:  n = 4; off = get_bits(&b, 10) + 1; break;   /* 101 */
            case 2:  n = get_bits(&b, 8) + 1;                    /* 110 */
                     off = get_bits(&b, 12) + 1; break;
            default: n = get_bits(&b, 8) + 9; off = 0; break;    /* 111 */
            }
        } else {
            if (get_bit(&b)) { n = 2; off = get_bits(&b, 8) + 1; }   /* 01 */
            else             { n = get_bits(&b, 3) + 1; off = 0; }   /* 00 */
        }

        if (n > o) {
            if (err) *err = "block runs past the start of the output";
            return -1;
        }
        if (off == 0) {                       /* literal bytes */
            while (n--) {
                o--;
                out[o] = (uint8_t)get_bits(&b, 8);
            }
        } else {                              /* back-reference */
            while (n--) {
                o--;
                if ((uint32_t)o + off >= out_len) {
                    if (err) *err = "back-reference past the end of the output";
                    return -1;
                }
                out[o] = out[o + off];
            }
        }
    }
    if (b.overrun) {
        if (err) *err = "ran off the front of the compressed data";
        return -1;
    }
    if (o != 0) {
        if (err) *err = "output not filled";
        return -1;
    }
    return 0;
}

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

int dat_load(const char *path, dat_file *out, const char **err)
{
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "rb");
    if (!f) { if (err) *err = "cannot open file"; return -1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); if (err) *err = "empty file"; return -1; }

    uint8_t *blob = malloc((size_t)len);
    if (!blob) { fclose(f); if (err) *err = "out of memory"; return -1; }
    if (fread(blob, 1, (size_t)len, f) != (size_t)len) {
        free(blob); fclose(f);
        if (err) *err = "short read";
        return -1;
    }
    fclose(f);

    uint32_t cap = 8;
    out->sections = calloc(cap, sizeof(*out->sections));
    if (!out->sections) { free(blob); if (err) *err = "out of memory"; return -1; }

    uint32_t pos = 0;
    while (pos + 10 <= (uint32_t)len) {
        uint8_t bits_in_first = blob[pos];
        uint8_t checksum      = blob[pos + 1];
        uint16_t dec_size     = be16(blob + pos + 4);
        uint16_t comp_size    = be16(blob + pos + 8);

        if (comp_size < 10 || pos + comp_size > (uint32_t)len)
            break;                  /* not a section header: stop cleanly */

        if (out->count == cap) {
            cap *= 2;
            dat_section *bigger = realloc(out->sections, cap * sizeof(*bigger));
            if (!bigger) {
                free(blob);
                dat_free(out);
                if (err) *err = "out of memory";
                return -1;
            }
            out->sections = bigger;
        }

        uint8_t *dec = malloc(dec_size ? dec_size : 1);
        if (!dec) {
            free(blob);
            dat_free(out);
            if (err) *err = "out of memory";
            return -1;
        }
        if (decompress(blob + pos + 10, (uint32_t)comp_size - 10u,
                       bits_in_first, checksum, dec, dec_size, err) != 0) {
            free(dec); free(blob); dat_free(out);
            return -1;
        }
        out->sections[out->count].data = dec;
        out->sections[out->count].size = dec_size;
        out->count++;
        pos += comp_size;
    }

    free(blob);
    if (out->count == 0) {
        if (err) *err = "no sections found";
        return -1;
    }
    return 0;
}

void dat_free(dat_file *d)
{
    if (!d) return;
    for (uint32_t i = 0; i < d->count; i++)
        free(d->sections[i].data);
    free(d->sections);
    d->sections = NULL;
    d->count = 0;
}
