/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Lemmings DAT decompression.
 *
 * PROVENANCE: transcribed from the original's disassembly. The decompressor
 * lives at image 0x01BAF and was found by weight, not by guessing - see
 * tools/hotspots.py: inside the windows that follow an INT 21h AH=3Fh read it
 * accounts for 83% of all instructions executed, and its innermost routine
 * 0x01CA3 alone runs 3.7 million times. Every routine below carries the image
 * offset it was read from.
 *
 * It was written from the published description at
 * camanis.net/lemmings/files/docs/lemmings_dat_file_format.txt first, and that
 * description turned out to agree with the binary on every case. Two things it
 * gets wrong or leaves ambiguous, both settled against real files and both
 * confirmed by the disassembly:
 *
 *   - The header's words are BIG-endian. level000.dat begins
 *     03 d8 00 00 08 00 ... and the decompressed size reads as 0x0800 = 2048,
 *     one level, big-endian - against a nonsensical 8 little-endian. The
 *     original reads them a byte at a time, high byte first (0x01BD9).
 *   - Bits come out of each byte LSB first, while multi-bit values are
 *     assembled MSB first. The original is unambiguous: `shr dl, 1` takes the
 *     low bit and `rcl ax, 1` shifts it in at the bottom, so the first bit
 *     read ends up the most significant (0x01CB0).
 *
 * The stream is consumed from its last byte backwards and the output is
 * rebuilt from its end backwards, which is why a back-reference reads from
 * *higher* addresses - those are the bytes already written. The original says
 * this with a single `std` at 0x01BB0 and a matching `cld` at 0x01C78.
 *
 * Checked against the original, not merely against the files:
 * tools/verify_dat.py breaks at 0x01BAF, captures the inputs, lets the
 * original's own body run to its `ret` at 0x01C79, and compares its output
 * with this code's byte for byte on the same call.
 *
 * Deliberate difference: the original has no bounds checks at all - a corrupt
 * stream walks off its buffers. The checks here are additions, marked where
 * they appear. They can only reject inputs the original would have accepted
 * and mis-decoded, never change the result of a well-formed one.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dat.h"

/* The original keeps the whole decoder in registers, and the mapping is worth
 * stating once rather than repeating at every line:
 *
 *   DS:SI  the compressed stream, read downwards      -> pos
 *   ES:DI  the output, written downwards              -> o
 *   BP     where the output stops (its start)         -> the o > 0 test
 *   CH     bits still usable in the byte held in DL   -> avail - idx
 *   DL     the byte bits are being shifted out of
 *   DH     the running checksum                       -> sum
 *   CL     how many bits the next get_bits should take
 *   BX     a block length, always one less than the count
 */
typedef struct {
    const uint8_t *data;
    int32_t pos;         /* byte index, walking backwards  (SI) */
    uint8_t avail;       /* bits still usable in data[pos] (CH) */
    uint8_t idx;         /* bits taken from data[pos] so far    */
    uint8_t sum;         /* running checksum               (DH) */
    int overrun;
} bitreader;

/* image 0x01CA3 - take one bit, refilling from the preceding byte.
 *
 *   0x01ca5  dec ch                 ; a bit left in the byte we hold?
 *   0x01ca7  jne 0x1cb0
 *   0x01ca9  dec si                 ; no - step BACK a byte
 *   0x01caa  mov dl, byte ptr [si]
 *   0x01cac  xor dh, dl             ; and fold it into the checksum
 *   0x01cae  mov ch, 8
 *   0x01cb0  shr dl, 1              ; low bit first out of the byte
 *
 * The checksum is folded in as bytes are *consumed*, not computed over the
 * section up front; see dat_decompress for why that distinction matters.
 */
static uint8_t get_bit(bitreader *b)
{
    if (b->idx >= b->avail) {
        b->pos--;
        b->idx = 0;
        b->avail = 8;
        if (b->pos < 0) {          /* addition: the original just walks off */
            b->overrun = 1;
            return 0;
        }
        b->sum ^= b->data[b->pos];
    }
    uint8_t v = (uint8_t)((b->data[b->pos] >> b->idx) & 1u);
    b->idx++;
    return v;
}

/* image 0x01CB2 - `rcl ax, 1` per bit, so the first bit read is the most
 * significant of the value. */
static uint32_t get_bits(bitreader *b, uint8_t n)
{
    uint32_t v = 0;
    while (n--)
        v = (v << 1) | get_bit(b);
    return v;
}

/* image 0x01BAF - decompress one section.
 *
 * The dispatch is a bit tree, and the original spends 0x01BF7..0x01C6D on it:
 *
 *   0x01bf9  1 bit  == 1  -> 0x01c03  2 bits
 *   0x01c09     0 or 1    -> length 3 or 4, offset in 9 or 10 bits  (0x01c0b)
 *   0x01c1b     == 3      -> 8 bits: a run of n+9 literal bytes     (0x01c20)
 *   0x01c38     == 2      -> 8 bits length, offset in 12 bits       (0x01c38)
 *   0x01c47  1 bit  == 1  -> length 2, offset in 8 bits             (0x01c51)
 *            else         -> 3 bits: a run of n+1 literal bytes     (0x01c5c)
 *
 * The length register BX always holds one less than the count, because the
 * copy routine at 0x01C87 opens with `inc bx`; that is folded into the
 * constants here rather than reproduced, since carrying an off-by-one through
 * C for the sake of resembling the assembly would be a worse transcription.
 *
 * The loop ends at 0x01C6F (`cmp di, bp / jbe`), i.e. when the output pointer
 * has reached its start - never on a marker in the stream.
 */
int dat_decompress(const uint8_t *comp, uint32_t comp_len,
                   uint8_t bits_in_first, uint8_t checksum,
                   uint8_t *out, uint32_t out_len, const char **err)
{
    /* 0x01BF2: the original loads the last byte and folds it into the
     * checksum before the loop starts, with CH = bits_in_first + 1 so that
     * the first `dec ch` leaves exactly bits_in_first usable. */
    bitreader b = { comp, (int32_t)comp_len - 1, bits_in_first, 0, checksum, 0 };
    if (comp_len == 0) {
        if (err) *err = "empty section";
        return -1;
    }
    b.sum ^= comp[comp_len - 1];
    uint32_t o = out_len;

    while (o > 0 && !b.overrun) {
        uint32_t n, off;
        if (get_bit(&b)) {                                    /* 0x01BF9 */
            switch (get_bits(&b, 2)) {                        /* 0x01C03 */
            case 0:  n = 3; off = get_bits(&b, 9)  + 1; break;   /* 0x01C0B */
            case 1:  n = 4; off = get_bits(&b, 10) + 1; break;   /* 0x01C0B */
            case 2:  n = get_bits(&b, 8) + 1;                    /* 0x01C38 */
                     off = get_bits(&b, 12) + 1; break;
            default: n = get_bits(&b, 8) + 9; off = 0; break;    /* 0x01C20 */
            }
        } else {                                              /* 0x01C47 */
            if (get_bit(&b)) { n = 2; off = get_bits(&b, 8) + 1; }  /* 0x01C51 */
            else             { n = get_bits(&b, 3) + 1; off = 0; }  /* 0x01C5C */
        }

        if (n > o) {               /* addition */
            if (err) *err = "block runs past the start of the output";
            return -1;
        }
        if (off == 0) {                       /* 0x01C82: emit a literal */
            while (n--) {
                o--;
                out[o] = (uint8_t)get_bits(&b, 8);
            }
        } else {                              /* 0x01C87: copy a reference */
            while (n--) {
                o--;
                if ((uint32_t)o + off >= out_len) {   /* addition */
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
    /* 0x01C75 `and dh, dh` - the original checks the running checksum only
     * once the output is full, so bytes past the last one it needed are never
     * folded in. Computing the XOR over the whole section up front, as this
     * code used to, is the same thing only when the decoder consumes every
     * byte - which is why this now mirrors the original instead. */
    if (b.sum != 0) {
        if (err) *err = "section checksum mismatch";
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
        if (dat_decompress(blob + pos + 10, (uint32_t)comp_size - 10u,
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
