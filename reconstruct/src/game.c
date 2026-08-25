/* game.c - PC Lemmings (VGA), reconstructed.
 *
 * Reconstructed from vgalemmi.exe (DMA Design / Psygnosis, 1991).
 * THE LICENCE OF THE ORIGINAL IS UNKNOWN and nothing here claims
 * one - a file transcribed out of someone else's binary is not ours to
 * licence, and an SPDX line would assert exactly that. The provenance note is
 * what belongs at the top of such a file, and it is the more useful thing
 * anyway. The project's own tooling is a different matter.
 *
 * WHY THIS IS ONE FILE
 *
 * Compilers of the period emitted one object per source file and the linker
 * laid each module's contribution into the segment whole and in link order, so
 * a contiguous run of addresses is one original source file and address order
 * is source order inside it. That is the only division of this program its
 * authors actually made. This port used to be eight files split by modern
 * instinct - dat.c, level.c, ground.c, vgagr.c, render.c, menu.c - a division
 * nothing in the binary supports, which also meant "which file is image
 * 0x03F5C in?" had no answer. So: one file, and the boundaries inside it are
 * marked with the addresses they came from.
 *
 * The one boundary that IS the port's own is sdl_io.c, the window and input
 * backend. That is not a reconstruction of anything and says so.
 *
 * WHAT IS TRANSCRIBED AND WHAT IS NOT
 *
 * Only some of this is transcribed from the disassembly. Those routines carry
 * the image offset they were read from, and are checked against the original
 * by tools/verify_dat.py and tools/verify_level.py.
 *
 * The rest - the object and steel tables, GROUNDxO.DAT, VGAGRx.DAT, terrain
 * composition, the menu's compositing - is written from published format
 * descriptions and checked against the data files and against what the
 * original composes in video memory. That is real evidence but it is NOT
 * transcription, and where those routines sit in the original is not yet
 * established. They are marked. See STATUS.md, which keeps the two counts
 * apart.
 *
 * The original is HAND-WRITTEN ASSEMBLY (docs/toolchain.md: zero
 * push bp / mov bp,sp in the whole 131,072-byte image), so there is no
 * compiler to match and behavioural equivalence is the standard.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"

/* ======================================================================
 * TRANSCRIBED - image 0x01BAF: DAT decompression.
 *
 * Found by weight, not by guessing: inside the windows following an
 * INT 21h AH=3Fh read this and its helpers are 83% of every instruction
 * executed (tools/hotspots.py). Checked by tools/verify_dat.py against the
 * original's own body: 7 calls, 150,807 bytes, byte-identical.
 * ====================================================================== */

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

/* NOT TRANSCRIBED. The original streams sections through a read routine
 * (image 0x01D2E) into one buffer as it goes; this reads the whole file and
 * decompresses every section up front, which is the port's own arrangement. */
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

/* The port's own: the original frees through its DOS allocator. */
void dat_free(dat_file *d)
{
    if (!d) return;
    for (uint32_t i = 0; i < d->count; i++)
        free(d->sections[i].data);
    free(d->sections);
    d->sections = NULL;
    d->count = 0;
}

/* ======================================================================
 * TRANSCRIBED - image 0x036B6: the LVL header parse; image 0x07C90:
 * the terrain entry decode. The rest of this section (objects, steel,
 * the name) is written from the published format, not transcribed.
 * Checked by tools/verify_level.py: 15 fields, all agreeing.
 * ====================================================================== */

/* be16 is defined once, above: both the DAT header and the LVL header
 * are big-endian, which is the Amiga heritage showing through. */

/* A 9-bit two's-complement field, as the terrain y position uses. */
static int16_t sign9(uint16_t v)
{
    return (int16_t)(v >= 256 ? (int32_t)v - 512 : (int32_t)v);
}

/* image 0x036B6 for the header, image 0x07C90 for each terrain entry. The
 * object table, the steel table and the name are NOT transcribed - they are
 * written from the published format, and where they live is not established. */
int level_parse(const uint8_t *buf, uint32_t len, level *lv, const char **err)
{
    if (len < 2048) {
        if (err) *err = "level section shorter than 2048 bytes";
        return -1;
    }
    memset(lv, 0, sizeof(*lv));

    /* image 0x036B6 - the header parse, transcribed.
     *
     * The original assembles each field high byte first and then keeps only
     * AL, so every one of these is effectively a BYTE despite occupying a
     * word in the file:
     *
     *   036bc  mov ah,[di]   / mov al,[di+1]
     *   036c1  mov [0x64],al                    release rate
     *   036c4  mov [0x67],al                    ... stored twice
     *   036cd  mov [0x47],al                    number of lemmings
     *   036d6  mov [0x44],al                    number to rescue
     *   036df  add ax,0x30 / mov [0x20a7],al    time limit, as an ASCII DIGIT
     *   036eb  mov [0x6a],al   climbers         ... and on at stride 3 for
     *   036f4  mov [0x6d],al   floaters             the eight skills, to
     *   ...                                         [0x7f] for diggers
     *   03733  add ax,0x10 / mov [0],ax         start x, plus 16
     *   03739  shr ax,1 x3   / mov [2],ax       and that over 8
     *
     * `add ax, 0x30` is the giveaway on the time limit: the original converts
     * it straight to a character, so its display only ever holds ONE digit.
     *
     * Keeping the low byte is transcribed rather than tidied, because the two
     * readings differ for any count above 255 and only the original's is
     * evidence. No level in this set has one, so it changes nothing here - it
     * is right rather than useful. */
    lv->release_rate  = (uint8_t)be16(buf + 0x00);
    lv->num_lemmings  = (uint8_t)be16(buf + 0x02);
    lv->num_to_rescue = (uint8_t)be16(buf + 0x04);
    lv->time_limit    = (uint8_t)be16(buf + 0x06);
    for (uint8_t i = 0; i < 8; i++)
        lv->skills[i] = (uint8_t)be16(buf + 0x08 + i * 2);
    /* Not truncated: the original keeps start x as a word (0x03736 stores AX,
     * not AL), and the graphics set numbers are read elsewhere. */
    lv->start_x               = be16(buf + 0x18);
    lv->graphics_set          = be16(buf + 0x1A);
    lv->extended_graphics_set = be16(buf + 0x1C);

    /* Objects: 32 entries of 8 bytes at 0x20. An all-zero entry is absent. */
    for (uint16_t i = 0; i < LEVEL_MAX_OBJECTS; i++) {
        const uint8_t *e = buf + 0x20 + i * 8;
        uint16_t x = be16(e), y = be16(e + 2);
        if (!x && !y && !e[4] && !e[5] && !e[6] && !e[7])
            continue;
        level_object *o = &lv->objects[lv->num_objects++];
        o->x = (int16_t)x;
        o->y = (int16_t)y;
        o->id = e[5];
        o->no_overwrite    = (uint8_t)((e[6] & 0x80) != 0);
        o->only_on_terrain = (uint8_t)((e[6] & 0x40) != 0);
        o->upside_down     = (uint8_t)(e[7] == 0x8F);
    }

    /* Terrain: 400 entries of 4 bytes at 0x120, FF FF FF FF terminates.
     *
     *   byte 0: flags in bits 7..5, x bits 12..8 in bits 4..0
     *   byte 1: x bits 7..0            - x is stored +16, so 0x0010 is 0
     *   byte 2: y bits 8..1
     *   byte 3: bit 7 is y bit 0; bits 5..0 are the terrain id
     *
     * x is THIRTEEN bits, not twelve: image 0x07C95 masks the word with
     * 0x1FFF, so bit 12 (byte 0's 0x10) belongs to x and only bits 13-15 are
     * flags. Ten of the 11,637 terrain entries in this demo set that bit, all
     * in level004, and all of them land far outside the canvas either way -
     * so it changes nothing visible here and is still what the original does.
     *
     * The y decode is the original's, line for line: image 0x07C90 does
     * `sar bx, cl` with cl = 7, an ARITHMETIC shift that sign-extends the
     * 9-bit field, and 0x07C92 does `sub bx, 4`.
     *
     * y is a 9-bit two's-complement value and then 4 less than it reads.
     *
     * Both words are BIG-endian, confirmed at image 0x03752 where the
     * original assembles them a byte at a time high-first:
     *   mov ah,[si] / mov al,[si+1]  and  mov bh,[si+2] / mov bl,[si+3]
     * which is not what an 8086 word load would do, so the byte order is a
     * deliberate choice in the game and not an artefact of the machine. */
    for (uint16_t i = 0; i < LEVEL_MAX_TERRAIN; i++) {
        const uint8_t *e = buf + 0x120 + i * 4;
        /* The original stops on the FIRST WORD being 0xFFFF, not on all four
         * bytes - image 0x03760, `cmp ax, 0xffff` where AX was assembled from
         * e[0] and e[1] alone. Requiring all four would read past the end of
         * the list for any level whose terminator has other bytes in its
         * second word. No shipped level here does, but matching the original
         * costs nothing and the difference is real. */
        if (e[0] == 0xFF && e[1] == 0xFF)
            break;
        level_terrain *t = &lv->terrain[lv->num_terrain++];
        t->x = (int16_t)((int32_t)(((e[0] & 0x1F) << 8) | e[1]) - 16);
        t->y = (int16_t)(sign9((uint16_t)((e[2] << 1) | (e[3] >> 7))) - 4);
        t->id = (uint8_t)(e[3] & 0x3F);
        t->no_overwrite = (uint8_t)((e[0] & 0x80) != 0);   /* word bit 15 */
        t->upside_down  = (uint8_t)((e[0] & 0x40) != 0);   /* word bit 14 */
        t->erase        = (uint8_t)((e[0] & 0x20) != 0);   /* word bit 13 */
    }

    /* Steel: 32 entries of 4 bytes at 0x760, all-zero means absent.
     * x and y are in units of 4 pixels; the last byte packs width and
     * height as nibbles, also in units of 4 pixels. */
    for (uint16_t i = 0; i < LEVEL_MAX_STEEL; i++) {
        const uint8_t *e = buf + 0x760 + i * 4;
        if (!e[0] && !e[1] && !e[2] && !e[3])
            continue;
        level_steel *s = &lv->steel[lv->num_steel++];
        s->x = (int16_t)((int32_t)(((e[0] << 1) | (e[1] >> 7)) * 4) - 16);
        s->y = (int16_t)((e[1] & 0x7F) * 4);
        s->w = (uint8_t)((((e[2] >> 4) & 0x0F) + 1) * 4);
        s->h = (uint8_t)(((e[2] & 0x0F) + 1) * 4);
    }

    memcpy(lv->name, buf + 0x7E0, 32);
    lv->name[32] = '\0';
    return 0;
}

/* A flat view of the parsed header, so tools/verify_level.py can compare
 * against the original without mirroring `level`'s layout in Python.
 * The order is the order the original stores them in.
 */
int level_header_fields(const uint8_t *buf, uint32_t len,
                        uint16_t *out, uint32_t n_out, const char **err)
{
    static level lv;
    if (n_out < 15) {
        if (err) *err = "need room for 15 fields";
        return -1;
    }
    if (level_parse(buf, len, &lv, err) != 0)
        return -1;
    out[0] = lv.release_rate;
    out[1] = lv.num_lemmings;
    out[2] = lv.num_to_rescue;
    out[3] = lv.time_limit;
    for (uint32_t i = 0; i < 8; i++)
        out[4 + i] = lv.skills[i];
    out[12] = lv.start_x;
    out[13] = lv.graphics_set;
    out[14] = lv.extended_graphics_set;
    return 0;
}

/* ======================================================================
 * NOT TRANSCRIBED - GROUNDxO.DAT, from the published description.
 * Where this lives in the original is not established.
 * ====================================================================== */

#define GROUND_SIZE 1056

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* A 6-bit DAC value as 8 bits, the way the hardware does it: the top two bits
 * repeated into the bottom, not a proportional scale. The two agree at 0 and
 * 63 and differ by one in between - 0x20 is 130 here and 129 if you compute
 * v*255/63 - and that one is enough to make every mid-tone pixel of a screen
 * compare as different against another emulator. */
static uint8_t widen6(uint8_t v)
{
    v = (uint8_t)(v & 0x3F);
    return (uint8_t)((v << 2) | (v >> 4));
}

/* NOT TRANSCRIBED - GROUNDxO.DAT, from the published description, checked
 * against the files. Where the original parses it is not established. */
int ground_load(const char *path, ground *g, const char **err)
{
    uint8_t buf[GROUND_SIZE];

    FILE *f = fopen(path, "rb");
    if (!f) { if (err) *err = "cannot open ground file"; return -1; }
    size_t got = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (got != sizeof(buf)) {
        if (err) *err = "ground file is not 1056 bytes";
        return -1;
    }
    memset(g, 0, sizeof(*g));

    /* 16 object entries of 28 bytes, from 0x000. */
    for (uint8_t i = 0; i < GROUND_OBJECTS; i++) {
        const uint8_t *e = buf + i * 28;
        ground_object *o = &g->objects[i];
        o->animation_flags = le16(e + 0x00);
        o->start_frame     = e[0x02];
        o->end_frame       = e[0x03];
        o->width           = e[0x04];
        o->height          = e[0x05];
        o->frame_data_size = le16(e + 0x06);
        o->mask_offset     = le16(e + 0x08);
        o->trigger_effect  = e[0x14];
        o->frames_base_loc = le16(e + 0x15);
        o->preview_frame   = e[0x17];
    }

    /* 64 terrain entries of 8 bytes, from 0x1C0. */
    for (uint8_t i = 0; i < GROUND_TERRAIN; i++) {
        const uint8_t *e = buf + 0x1C0 + i * 8;
        ground_terrain *t = &g->terrain[i];
        t->width     = e[0x00];
        t->height    = e[0x01];
        t->image_loc = le16(e + 0x02);
        t->mask_loc  = le16(e + 0x04);
    }

    /* Palettes from 0x3C0: three EGA sets of 8 bytes, then three VGA sets of
     * 8 entries x 3 bytes. Terrain only ever uses the upper eight colours,
     * which is why its fourth bitplane doubles as the mask.
     *
     * The upper eight come from this set's `custom` array. The LOWER eight do
     * NOT come from the file's `standard` array, whatever the published
     * description implies - the game carries them itself. Measured against the
     * original running under an emulator, fourteen of sixteen entries agreed
     * and the two that did not were both here:
     *
     *   entry 4  the file holds 3C 3C 00; the game uses 2C 2C 00 - a darker
     *            yellow. Six of the other seven file entries match the game
     *            exactly, which is what makes this easy to miss.
     *   entry 7  is not fixed at all. It is a copy of custom[0], so it equals
     *            entry 8. The published note does say "entry 7 is variable";
     *            it does not say the rest are unreliable.
     *
     * So the fixed eight are transcribed here as 6-bit DAC values, taken from
     * what the original actually programs. */
    static const uint8_t FIXED[7][3] = {
        { 0x00, 0x00, 0x00 },   /* 0 transparent / background */
        { 0x10, 0x10, 0x38 },   /* 1 blue    - lemming bodies */
        { 0x00, 0x2C, 0x00 },   /* 2 green   - lemming hair   */
        { 0x3C, 0x34, 0x34 },   /* 3 white   - skin           */
        { 0x2C, 0x2C, 0x00 },   /* 4 dark yellow              */
        { 0x3C, 0x08, 0x08 },   /* 5 red                      */
        { 0x20, 0x20, 0x20 },   /* 6 grey                     */
    };
    const uint8_t *vga_custom = buf + 0x3D8;

    for (uint8_t i = 0; i < 7; i++) {
        g->palette[i].r = widen6(FIXED[i][0]);
        g->palette[i].g = widen6(FIXED[i][1]);
        g->palette[i].b = widen6(FIXED[i][2]);
    }
    for (uint8_t i = 0; i < 8; i++) {
        lem_rgb c = { widen6(vga_custom[i * 3 + 0]),
                      widen6(vga_custom[i * 3 + 1]),
                      widen6(vga_custom[i * 3 + 2]) };
        g->palette[8 + i] = c;
    }
    g->palette[7] = g->palette[8];      /* entry 7 tracks custom[0] */
    return 0;
}

/* ======================================================================
 * NOT TRANSCRIBED - VGAGRx.DAT, from the published description.
 * Where this lives in the original is not established.
 * ====================================================================== */

uint8_t g_plane_bits[4] = { 0, 1, 2, 3 };
uint8_t g_msb_first = 1;

/* NOT TRANSCRIBED - a 4-bit planar bitmap out of VGAGRx.DAT. Where the
 * original decodes these is not established. */
int vgagr_decode(const uint8_t *sec, uint32_t len, uint32_t loc,
                 uint8_t w, uint8_t h, uint8_t *out)
{
    uint32_t stride = ((uint32_t)w + 7u) / 8u;
    uint32_t plane  = stride * (uint32_t)h;

    if (loc + plane * 4u > len)
        return -1;

    memset(out, 0, (size_t)w * h);

    for (uint8_t p = 0; p < 4; p++) {
        const uint8_t *bits = sec + loc + (uint32_t)p * plane;
        for (uint8_t y = 0; y < h; y++) {
            const uint8_t *row = bits + (uint32_t)y * stride;
            for (uint8_t x = 0; x < w; x++) {
                uint8_t sh = g_msb_first ? (uint8_t)(7 - (x & 7))
                                         : (uint8_t)(x & 7);
                uint8_t bit = (uint8_t)((row[x >> 3] >> sh) & 1u);
                out[(uint32_t)y * w + x] |=
                    (uint8_t)(bit << g_plane_bits[p]);
            }
        }
    }
    return 0;
}

/* NOT TRANSCRIBED - the 1-bit transparency mask beside a bitmap. */
int vgagr_decode_mask(const uint8_t *sec, uint32_t len, uint32_t loc,
                      uint8_t w, uint8_t h, uint8_t *out)
{
    uint32_t stride = ((uint32_t)w + 7u) / 8u;

    if (loc + stride * (uint32_t)h > len)
        return -1;

    for (uint8_t y = 0; y < h; y++) {
        const uint8_t *row = sec + loc + (uint32_t)y * stride;
        for (uint8_t x = 0; x < w; x++)
            out[(uint32_t)y * w + x] = (uint8_t)(
                (row[x >> 3] >> (g_msb_first ? (7 - (x & 7)) : (x & 7))) & 1u);
    }
    return 0;
}

/* ======================================================================
 * NOT TRANSCRIBED - terrain composition.
 *
 * Proven against what the original composes rather than against its
 * screen: tools/prove.sh, 49,741 pixels, zero differing. That is strong
 * evidence of equivalence and is not the same claim as transcription.
 * ====================================================================== */

terrain_mode g_terrain_mode = TERRAIN_REPLACE;
uint8_t g_terrain_reverse = 0;

/* The port's own: the original composes straight into video memory. */
int canvas_init(canvas *c)
{
    c->w = LEVEL_W;
    c->h = LEVEL_H;
    c->pix = calloc((size_t)c->w * c->h, 1);
    return c->pix ? 0 : -1;
}

/* The port's own. */
void canvas_free(canvas *c)
{
    free(c->pix);
    c->pix = NULL;
}

/* NOT TRANSCRIBED, but PROVEN equivalent. The original's loop is at image
 * 0x03752..0x03768, calling a blitter at image 0x07C67; this composes the
 * same picture without being a transcription of it. tools/prove.sh compares
 * against what the original composes: 49,741 pixels, zero differing. */
int render_terrain(canvas *c, const level *lv, const ground *g,
                   const uint8_t *sec, uint32_t sec_len, const char **err)
{
    /* The largest terrain bitmap in any shipped set comfortably fits this. */
    uint8_t *tile = malloc(256u * 256u);
    if (!tile) { if (err) *err = "out of memory"; return -1; }

    for (uint16_t k = 0; k < lv->num_terrain; k++) {
        uint16_t i = g_terrain_reverse
                   ? (uint16_t)(lv->num_terrain - 1u - k) : k;
        const level_terrain *t = &lv->terrain[i];
        const ground_terrain *gt = &g->terrain[t->id];

        if (!gt->width || !gt->height)
            continue;               /* a set need not define all 64 ids */

        if (vgagr_decode(sec, sec_len, gt->image_loc,
                         gt->width, gt->height, tile) != 0) {
            free(tile);
            if (err) *err = "terrain bitmap runs past the end of the section";
            return -1;
        }

        for (uint8_t sy = 0; sy < gt->height; sy++) {
            int32_t dy = t->y + sy;
            if (dy < 0 || dy >= c->h)
                continue;
            uint8_t src_y = t->upside_down
                          ? (uint8_t)(gt->height - 1u - sy) : sy;

            for (uint8_t sx = 0; sx < gt->width; sx++) {
                int32_t dx = t->x + sx;
                if (dx < 0 || dx >= c->w)
                    continue;

                uint8_t v = tile[(uint32_t)src_y * gt->width + sx];

                if (g_terrain_mode == TERRAIN_HW) {
                    /* What the original's blitter does, read from its own
                     * code at image 0x08B42-0x08BE2:
                     *
                     *   - the Graphics Controller is put in read mode 1 with
                     *     colour compare 0x08 and don't-care 0x08, so reading
                     *     video memory answers "is this pixel already solid
                     *     terrain?" one bit per pixel;
                     *   - NOT of that becomes the bit mask, so a write cannot
                     *     land on existing terrain - that is what "do not
                     *     overwrite" is, done in hardware;
                     *   - GC index 3 is set to 0x10, which selects the OR
                     *     function, so what is written is source OR latch;
                     *   - and the inner loop is a plain `lodsb / stosb` over
                     *     the whole row. It NEVER tests whether the source
                     *     pixel is transparent.
                     *
                     * That last point is the one a per-pixel renderer gets
                     * wrong by being sensible: the transparent parts of a
                     * piece still deposit their colour bits, and a later
                     * piece ORs on top of them. The result is pixels whose
                     * value is a superset of any single piece's - which is
                     * exactly what the original's screen shows and a
                     * skip-if-transparent renderer cannot produce.
                     */
                    uint8_t *d = &c->pix[(uint32_t)dy * c->w + (uint32_t)dx];
                    if (!(*d & 0x08))
                        *d = (uint8_t)(*d | v);
                    continue;
                }

                if (!(v & 0x08))
                    continue;       /* transparent */

                uint8_t *dst = &c->pix[(uint32_t)dy * c->w + (uint32_t)dx];
                if (t->erase)
                    *dst = 0;
                else if (g_terrain_mode == TERRAIN_OR_ALL)
                    *dst = (uint8_t)(*dst | v);
                else if (t->no_overwrite
                         && g_terrain_mode != TERRAIN_NOV_IGNORE) {
                    if (g_terrain_mode == TERRAIN_NOV_INVERT) {
                        if (*dst != 0)
                            *dst = v;
                    } else if (*dst == 0)
                        *dst = v;
                } else switch (g_terrain_mode) {
                case TERRAIN_OR:
                    *dst = (uint8_t)(*dst | v);
                    break;
                case TERRAIN_KEEP:
                    if (*dst == 0)
                        *dst = v;
                    break;
                default:
                    *dst = v;
                    break;
                }
            }
        }
    }
    free(tile);
    return 0;
}

/* NOT TRANSCRIBED, and NOT what the original does. Breaking at image
 * 0x03789 - the final `ret` after all five composition passes - shows the
 * original never composes objects into the level bitmap at all: it draws the
 * entrance and the exit as animated sprites at display time. This bakes them
 * in, which is why tools/prove.sh passes --no-objects. */
int render_objects(canvas *c, const level *lv, const ground *g,
                   const uint8_t *sec, uint32_t sec_len,
                   uint8_t frame, const char **err)
{
    uint8_t *pix  = malloc(256u * 256u);
    uint8_t *mask = malloc(256u * 256u);
    if (!pix || !mask) {
        free(pix); free(mask);
        if (err) *err = "out of memory";
        return -1;
    }

    for (uint16_t i = 0; i < lv->num_objects; i++) {
        const level_object *o = &lv->objects[i];
        if (o->id >= GROUND_OBJECTS)
            continue;
        const ground_object *go = &g->objects[o->id];
        if (!go->width || !go->height)
            continue;

        /* Frames sit one after another, each frame_data_size bytes: four
         * image planes and then the mask at mask_offset within the frame. */
        uint8_t f = frame;
        if (f < go->start_frame) f = go->start_frame;
        if (f > go->end_frame)   f = go->end_frame;
        uint32_t at = (uint32_t)go->frames_base_loc
                    + (uint32_t)f * go->frame_data_size;

        if (vgagr_decode(sec, sec_len, at, go->width, go->height, pix) != 0 ||
            vgagr_decode_mask(sec, sec_len, at + go->mask_offset,
                              go->width, go->height, mask) != 0) {
            free(pix); free(mask);
            if (err) *err = "object bitmap runs past the end of the section";
            return -1;
        }

        for (uint8_t sy = 0; sy < go->height; sy++) {
            int32_t dy = o->y + sy;
            if (dy < 0 || dy >= c->h)
                continue;
            uint8_t src_y = o->upside_down
                          ? (uint8_t)(go->height - 1u - sy) : sy;

            for (uint8_t sx = 0; sx < go->width; sx++) {
                int32_t dx = o->x + sx;
                if (dx < 0 || dx >= c->w)
                    continue;
                uint32_t si = (uint32_t)src_y * go->width + (uint32_t)sx;
                if (!mask[si])
                    continue;

                uint8_t *dst = &c->pix[(uint32_t)dy * c->w + (uint32_t)dx];
                if (o->only_on_terrain && *dst == 0)
                    continue;
                if (o->no_overwrite && *dst != 0)
                    continue;
                *dst = pix[si];
            }
        }
    }
    free(pix);
    free(mask);
    return 0;
}

/* ======================================================================
 * PARTLY TRANSCRIBED - the main menu screen.
 *
 * The palette (image 0x0E3C0) and both credit strings (image 0x0E56C and
 * 0x0E587) are transcribed and carry their addresses. The geometry was
 * MEASURED against the original's composed screen, and the compositing
 * code is the port's own. tools/verify_menu.py: 94.67%.
 * ====================================================================== */

/* MAIN.DAT section 3 holds the menu's graphics; section 4 ends with its font. */
#define MENU_SECTION 3
#define FONT_SECTION 4

/* The font: 16x16 glyphs, three 32-byte planes each, indexed straight by ASCII
 * code. 96 bytes a glyph, and the block runs from character 0 to 126 - which
 * ends at 23888 + 127*96 = 36080, exactly the length of section 4. That the
 * arithmetic lands on the end of the section is the confirmation that the
 * glyph size and the base are both right.
 *
 * Plane 2 doubles as the opacity mask, because every colour the font uses is
 * 4..7 and so has bit 2 set. Deriving the layout took three tries: the mask is
 * the LAST of the three planes, not the first, and reading it as the first
 * scores 51%. As the last, with plane 0 then plane 1 before it, it is 100.0%
 * over both credit lines - 5606 masked pixels, none differing. */
#define FONT_BASE   23888
#define FONT_GLYPH  96
#define FONT_PLANE  32
#define FONT_W      16
#define FONT_H      16
#define FONT_MASK_PLANE 2

/* THE RATING NAME, drawn over its plate.
 *
 * The plate's own bitmap carries a "FUN", and the original overdraws the
 * current rating on top of it - it has to, because the rating changes. The
 * artwork is in section 4, not section 3 with the rest of the menu, and it
 * sits BELOW the font's nominal base: the font is indexed from 23888 so that
 * ASCII lands correctly, but no printable character reaches below 23888+32*96,
 * and the ratings live in that unused space.
 *
 * 72x27, four planes of 243 bytes, at screen (486,145). Fitted at pixel
 * granularity: 100.00% over 1,760 ink pixels, none differing. */
#define RATING_SECTION 4
#define RATING_BASE    26084
#define RATING_W       72
#define RATING_H       27
#define RATING_STRIDE  9
#define RATING_PLANE   243
#define RATING_STEP    (RATING_PLANE * 4)
#define RATING_X       486
#define RATING_Y       145

/* Both lines come from the executable, not from a data file, so they are
 * transcribed here with the address they were read from - the same treatment a
 * routine gets. They sit in a table of the menu's and briefing's strings. */
static const char CREDIT_1[] = "(C)MCMXCI Psygnosis Ltd";   /* image 0x0E56C */
static const char CREDIT_2[] = "A DMA Design Game";         /* image 0x0E587 */

/* Where a line of n characters starts. Centring alone gives 136 and 184 for
 * the two lines; the original draws them at 128 and 176, so it centres and
 * then backs off half a character. Both lines agree on that, which is why it
 * is written as a rule rather than as two measured constants. */
#define TEXT_X(n)   (uint16_t)((MENU_W - (n) * FONT_W) / 2 - FONT_W / 2)
#define CREDIT_1_Y  272
#define CREDIT_2_Y  288

/* The background tile. Two planes only - it uses four colours - so values are
 * 0..3 and planes 2 and 3 are implicitly zero. Confirmed by measurement:
 * assembling it from four consecutive 4160-byte blocks scores 39%, and
 * forcing planes 2 and 3 to zero scores 95.6%. */
#define BG_W      320
#define BG_H      104
#define BG_STRIDE 40
#define BG_PLANE0 0
/* How much of each row the original actually fills: 79 bytes, not 80. */
#define BG_DRAWN_W 632
#define BG_PLANE1 4160

/* The logo, at screen (8,10). 632 wide, so it ends exactly at x=639. */
#define LOGO_BASE   8320
#define LOGO_W      632
#define LOGO_H      94
#define LOGO_STRIDE 79
#define LOGO_PLANE  7426
#define LOGO_X      8
#define LOGO_Y      10
/* Where its shadow falls, measured: eight pixels left, ten rows down. */
#define LOGO_SHADOW_DX (-8)
#define LOGO_SHADOW_DY  10

/* The six signs, one atlas entry each, 4 x 915 = 3660 bytes apart. The atlas
 * begins at 38024, which is exactly where the logo's fourth plane ends. */
#define SIGN_BASE   38024
#define SIGN_W      120
#define SIGN_H      61
#define SIGN_STRIDE 15
#define SIGN_PLANE  915
#define SIGN_STEP   3660
/* Where a plate's shadow falls, measured from where the blitter writes. */
#define SIGN_SHADOW_DX (-8)
#define SIGN_SHADOW_DY   8

/* The FX plate's label, and its alternate.
 *
 * Section 3's last 1,984 bytes are TWO 64x31 sprites of four planes each -
 * 8 x 31 = 248 a plane, 992 a sprite, plane-major. Sprite 1 is the "FX"
 * lettering the menu shows; sprite 0 is what it swaps to, which is why the
 * plate's own bitmap has no label baked in while the others do.
 *
 * Drawn at (351,146), which is NOT byte-aligned - and that is the whole
 * difference between 92.74% and 100.00%. A sweep in steps of 8 found the
 * best aligned position and looked convincing; sweeping single pixels found
 * the real one, 1,654 ink pixels with none differing.
 */
#define LABEL_BASE   59984
#define LABEL_W      64
#define LABEL_H      31
#define LABEL_STRIDE 8
#define LABEL_PLANE  (LABEL_STRIDE * LABEL_H)
#define LABEL_SPRITE (LABEL_PLANE * 4)
#define LABEL_FX     1        /* sprite 1 is "FX"; 0 is the alternate */
#define LABEL_X      351
#define LABEL_Y      146

static const struct { uint16_t x, y; } SIGN_POS[6] = {
    {  68, 120 },   /* F1  1 PLAYER   */
    { 196, 120 },   /* F2  NEW LEVEL  */
    { 324, 120 },   /* F3  FX         */
    { 452, 120 },   /* F4  FUN        */
    { 196, 197 },   /* ESC EXIT TO DOS*/
    { 324, 197 },   /* F4  ?          */
};

/* The menu palette, transcribed from image 0x0E3C0 - sixteen 6-bit RGB
 * triples, in attribute-register order. It is a table in the executable, not
 * in any data file, so it is transcribed here the way a routine would be.
 *
 * These go to DAC entries 0-5, 0x14, 7, 0x38-0x3F, which is the BIOS default
 * attribute palette for mode 10h - the game does not reprogram it. That
 * mapping matters only when comparing against the emulator's own screen dump,
 * which reports DAC indices; the port keeps the 4-bit values. */
static const uint8_t MENU_PAL6[48] = {
    0x00,0x00,0x00,  0x20,0x10,0x08,  0x18,0x0c,0x08,  0x0c,0x00,0x04,
    0x08,0x02,0x1f,  0x10,0x0b,0x24,  0x1a,0x16,0x29,  0x26,0x23,0x2f,
    0x00,0x14,0x00,  0x00,0x18,0x04,  0x00,0x1c,0x08,  0x00,0x20,0x10,
    0x34,0x34,0x34,  0x2c,0x2c,0x00,  0x10,0x14,0x2c,  0x38,0x20,0x24,
};

/* 6 bits to 8 by REPLICATING the top bits, not by scaling to 255. 0x3F must
 * come out 255 and 0x00 must come out 0, and v*255/63 gets both right and
 * everything between subtly wrong. */
static uint8_t dac8(uint8_t v)
{
    v &= 0x3F;
    return (uint8_t)((v << 2) | (v >> 4));
}

/* image 0x09347 - copy a sprite row into the scratch buffer, append a zero
 * byte, and shift the lot right by `bits`.
 *
 * This is the game's own way of drawing at an x that is not a multiple of
 * eight, and it is why so many of its blits read from anonymous memory: the
 * blit's source is this buffer, never the artwork.
 *
 *   0x0934e  lea di, [0x6706]      ; the scratch buffer, in DGROUP
 *   0x09352  rep movsb             ; CL bytes of the row
 *   0x09354  xor al, al
 *   0x09356  stosb                 ; then ONE ZERO BYTE
 *   0x09365  mov al, [0xa948]      ; the shift count, in bits
 *   0x0936c  shr byte ptr [si], 1  \ shift the whole buffer right one bit,
 *   0x09371  rcr byte ptr [si], 1   > the first byte with SHR so a zero comes
 *   0x09376  loop 0x9371           / in, the rest with RCR to carry across
 *   0x0937a  dec al / jne          ; once per bit
 *   0x09380  inc cl                ; and blit ONE MORE byte than was copied
 *
 * The appended byte is what catches the bits pushed off the right-hand end, so
 * a 15-byte sprite shifted by 4 occupies 16 bytes on screen.
 *
 * `out` must have room for len + 1 bytes. Returns how many bytes to draw.
 */
uint8_t shift_row(const uint8_t *src, uint8_t len, uint8_t bits, uint8_t *out)
{
    for (uint8_t i = 0; i < len; i++)
        out[i] = src[i];
    out[len] = 0;                       /* 0x09354: xor al,al / stosb */

    for (uint8_t b = 0; b < bits; b++) {
        uint8_t carry = 0;              /* SHR shifts a zero into the first */
        for (uint8_t i = 0; i <= len; i++) {
            uint8_t next = (uint8_t)(out[i] & 1u);
            out[i] = (uint8_t)((out[i] >> 1) | (carry << 7));
            carry = next;
        }
    }
    return (uint8_t)(len + 1);          /* 0x09380: inc cl */
}

/* image 0x04BC2 - write a byte as three decimal digits at DS:SI.
 *
 * This is what turns a level number into the digits of "level000.dat". The
 * original has no divide here: it subtracts 100 until it borrows, counts the
 * subtractions, and adds 0x2F rather than 0x30 because the counter starts one
 * too high.
 *
 *   0x04bc4  inc ah
 *   0x04bc6  sub al, 0x64
 *   0x04bc8  jae 0x4bc4          ; again while that did not borrow
 *   0x04bca  add al, 0x64        ; undo the one that did
 *   0x04bcc  add ah, 0x2f        ; '0' - 1 + the count
 *
 * Written as division, which is what it means; the repeated subtraction is the
 * 8086 not having a byte divide worth using, not a decision about behaviour.
 * Three digits, no terminator - it overwrites digits already in a template.
 */
void digits3(uint8_t v, char *out)
{
    out[0] = (char)('0' + v / 100);
    out[1] = (char)('0' + (v / 10) % 10);
    out[2] = (char)('0' + v % 10);
}

/* image 0x05B11 - build the eight pre-shifted copies of a sprite.
 *
 * The game cannot blit at an arbitrary x, so for the level's sprites it builds
 * all eight sub-byte alignments once, at load time, instead of shifting on
 * every draw the way the menu's 0x09347 does.
 *
 * Each copy is `rows` x `cols` groups of THREE bytes: two bytes of source
 * shifted right into a 24-bit window, so a shift of 7 still has somewhere to
 * go. After each copy comes one extra row of `cols` groups holding the OR of
 * every row in that column - a summary of which bits the sprite touches at
 * all.
 *
 *   0x05b1c  mov ah, [si] / mov al, [si+1]   ; two bytes in
 *   0x05b24  xor bl, bl                      ; the third starts empty
 *   0x05b2c  shr ah,1 / rcr al,1 / rcr bl,1  ; shift the 24 bits right, CL times
 *   0x05b36  mov es:[di], ah / +1, al / +2, bl
 *   0x05b63  or ah, es:[si] / or al, ... / or bl, ...   ; the column OR
 *   0x05b6e  add si, bp                      ; bp = 3 * cols, i.e. down a column
 *   0x05b8d  inc cl / cmp cl, 8              ; once per alignment
 *
 * `out` needs 8 * 3 * cols * (rows + 1) bytes.
 */
void preshift_sprite(const uint8_t *src, uint8_t cols, uint8_t rows,
                     uint8_t *out)
{
    size_t o = 0;
    for (uint8_t sh = 0; sh < 8; sh++) {
        const uint8_t *s = src;             /* 0x05b14: si is restored */
        size_t copy = o;
        for (uint8_t r = 0; r < rows; r++) {
            for (uint8_t c = 0; c < cols; c++) {
                uint8_t hi = s[0], mid = s[1], lo = 0;
                s += 2;
                for (uint8_t b = 0; b < sh; b++) {
                    uint8_t c1 = (uint8_t)(hi & 1u);
                    hi = (uint8_t)(hi >> 1);
                    uint8_t c2 = (uint8_t)(mid & 1u);
                    mid = (uint8_t)((mid >> 1) | (c1 << 7));
                    lo = (uint8_t)((lo >> 1) | (c2 << 7));
                }
                out[o++] = hi;
                out[o++] = mid;
                out[o++] = lo;
            }
        }
        for (uint8_t c = 0; c < cols; c++) {      /* the column OR */
            uint8_t hi = 0, mid = 0, lo = 0;
            for (uint8_t r = 0; r < rows; r++) {
                size_t k = copy + ((size_t)r * cols + c) * 3;
                hi = (uint8_t)(hi | out[k]);
                mid = (uint8_t)(mid | out[k + 1]);
                lo = (uint8_t)(lo | out[k + 2]);
            }
            out[o++] = hi;
            out[o++] = mid;
            out[o++] = lo;
        }
    }
}

/* One pixel out of a planar bitmap. `planes` is how far apart consecutive
 * planes are; pass 0 for the two-plane background and only bits 0 and 1 are
 * read. The leftmost pixel of a byte is its most significant bit. */
static uint8_t planar(const uint8_t *sec, uint32_t base, uint32_t plane_step,
                      uint16_t stride, uint8_t nplanes,
                      uint16_t x, uint16_t y)
{
    uint8_t v = 0;
    for (uint8_t p = 0; p < nplanes; p++) {
        uint32_t o = base + p * plane_step + (uint32_t)y * stride + (x >> 3);
        v = (uint8_t)(v | (((sec[o] >> (7 - (x & 7))) & 1u) << p));
    }
    return v;
}

/* Blit a 4-plane sprite, treating value 0 as transparent. That the logo and
 * the signs are keyed on 0 rather than drawn opaquely is measured: drawn
 * opaquely the logo's own bounding box scores 35%, keyed it scores 95%+, and
 * the background texture is plainly visible between the letters. */
static void blit(uint8_t *dst, const uint8_t *sec, uint32_t base,
                 uint16_t w, uint16_t h, uint16_t stride, uint32_t plane,
                 uint16_t at_x, uint16_t at_y)
{
    for (uint16_t sy = 0; sy < h; sy++) {
        uint16_t y = at_y + sy;
        if (y >= MENU_H) break;
        for (uint16_t sx = 0; sx < w; sx++) {
            uint16_t x = at_x + sx;
            if (x >= MENU_W) break;
            uint8_t v = planar(sec, base, plane, stride, 4, sx, sy);
            if (v) dst[(size_t)y * MENU_W + x] = v;
        }
    }
}

/* One glyph, keyed on plane 2. Characters outside the block are skipped
 * rather than clamped: a wrong glyph is worse than a gap, because it looks
 * like a font bug rather than a missing character. */
static void draw_char(uint8_t *dst, const uint8_t *font, uint32_t font_len,
                      uint8_t ch, uint16_t at_x, uint16_t at_y)
{
    /* A space draws NOTHING. Its slot in the font is not blank - it holds
     * something, and drawing it puts ink where the original leaves background,
     * which is the whole of the difference between 94.3% and 100% on the
     * credit lines. So the original's text routine skips the character rather
     * than relying on an empty glyph. */
    if (ch == ' ') return;
    uint32_t g = FONT_BASE + (uint32_t)ch * FONT_GLYPH;
    if (ch > 126 || g + FONT_GLYPH > font_len) return;
    for (uint16_t gy = 0; gy < FONT_H; gy++) {
        uint16_t y = at_y + gy;
        if (y >= MENU_H) break;
        for (uint16_t gx = 0; gx < FONT_W; gx++) {
            uint16_t x = at_x + gx;
            if (x >= MENU_W) break;
            uint32_t o = g + gy * 2 + (gx >> 3);
            uint8_t sh = (uint8_t)(7 - (gx & 7));
            uint8_t mask = (font[o + FONT_MASK_PLANE * FONT_PLANE] >> sh) & 1u;
            if (!mask) continue;
            uint8_t v = (uint8_t)(
                (1u << FONT_MASK_PLANE)
                | ((font[o] >> sh) & 1u)
                | (((font[o + FONT_PLANE] >> sh) & 1u) << 1));
            dst[(size_t)y * MENU_W + x] = v;
        }
    }
}

/* The port's own. */
static void draw_text(uint8_t *dst, const uint8_t *font, uint32_t font_len,
                      const char *text, uint16_t at_y)
{
    size_t n = strlen(text);
    uint16_t x = TEXT_X((uint16_t)n);
    for (size_t i = 0; i < n; i++)
        draw_char(dst, font, font_len, (uint8_t)text[i],
                  (uint16_t)(x + i * FONT_W), at_y);
}

/* NOT TRANSCRIBED - the port's own compositing of the original's own
 * graphics and geometry. Every constant it uses was measured against the
 * screen the original composes; tools/verify_menu.py scores it at 94.67%. */
/* The strings for the two sub-screens, transcribed from the executable with
 * the addresses they were read from. They are not in any data file. */
static const char S_ENTER_CODE[]  = "Enter Code";          /* image 0x0E874 */
static const char S_CODE_DOTS[]   = "..........";          /* image 0x0E882 */
static const char S_INPUT_TYPE[]  = "Choose Input Type";   /* image 0x0E8CD */
static const char S_INPUT_1[]     = "1. Microsoft Mouse";  /* image 0x0E901 */
static const char S_INPUT_2[]     = "2. Joystick";         /* image 0x0E8E2 */
static const char S_INPUT_3[]     = "3. Keyboard";         /* image 0x0E8F1 */

/* Where each line goes. MEASURED off the original's own screens, not derived:
 * the menu's two credit lines are centred and then backed off half a
 * character, and these are not - "Enter Code" sits a whole character further
 * left than that rule predicts, and the three input options share a left
 * margin rather than each being centred. So they are constants, and they are
 * constants because that is what the evidence says. Where the original keeps
 * them is not established. */
#define CODE_TITLE_X   224
#define CODE_TITLE_Y   112
#define CODE_DOTS_X    224
#define CODE_DOTS_Y    144
#define INPUT_TITLE_X  176
#define INPUT_TITLE_Y   80
#define INPUT_OPT_X    112
#define INPUT_OPT_Y    128
#define INPUT_OPT_STEP  32

/* Shared by all three screens: load main.dat, tile the background, and hand
 * back the section pointers the caller still needs. */
static int screen_start(const char *dir, menu_screen *m, dat_file *d,
                        const uint8_t **sec, const uint8_t **font,
                        uint32_t *fontlen, int tile_background,
                        const char **err)
{
    memset(m, 0, sizeof(*m));
    char path[1024];
    snprintf(path, sizeof(path), "%s/main.dat", dir);
    if (dat_load(path, d, err) != 0)
        return -1;
    if (d->count <= FONT_SECTION) {
        dat_free(d);
        if (err) *err = "main.dat is missing a section the menu needs";
        return -1;
    }
    *sec = d->sections[MENU_SECTION].data;
    *font = d->sections[FONT_SECTION].data;
    *fontlen = d->sections[FONT_SECTION].size;

    m->pix = calloc((size_t)MENU_W * MENU_H, 1);
    if (!m->pix) {
        dat_free(d);
        if (err) *err = "out of memory";
        return -1;
    }
    for (uint8_t i = 0; i < 16; i++) {
        m->palette[i].r = dac8(MENU_PAL6[i * 3 + 0]);
        m->palette[i].g = dac8(MENU_PAL6[i * 3 + 1]);
        m->palette[i].b = dac8(MENU_PAL6[i * 3 + 2]);
    }
    if (tile_background)
        for (uint16_t y = 0; y < MENU_H; y++)
            for (uint16_t x = 0; x < BG_DRAWN_W; x++)
                m->pix[(size_t)y * MENU_W + x] =
                    planar(*sec, BG_PLANE0, BG_PLANE1 - BG_PLANE0, BG_STRIDE,
                           2, x % BG_W, y % BG_H);
    return 0;
}

/* Text at a given cell, rather than centred. */
static void draw_at(uint8_t *dst, const uint8_t *font, uint32_t font_len,
                    const char *text, uint16_t at_x, uint16_t at_y)
{
    for (size_t i = 0; text[i]; i++)
        draw_char(dst, font, font_len, (uint8_t)text[i],
                  (uint16_t)(at_x + i * FONT_W), at_y);
}

/* The screen F2 reaches: the level-code entry. What it does with the code is
 * NOT reconstructed - this draws it and comes back. */
int menu_code_screen(const char *dir, menu_screen *m, const char **err)
{
    dat_file d;
    const uint8_t *sec, *font;
    uint32_t fontlen;
    if (screen_start(dir, m, &d, &sec, &font, &fontlen, 1, err) != 0)
        return -1;
    draw_at(m->pix, font, fontlen, S_ENTER_CODE, CODE_TITLE_X, CODE_TITLE_Y);

    /* The dots are an INPUT FIELD, and the original blacks its whole strip
     * out - every pixel of the ten 16x16 cells that is not a dot is 0, not
     * the background showing through. Drawing them keyed, as the title above
     * is drawn, leaves the tile between them and costs 1,230 pixels: the
     * entire difference on this screen. */
    for (uint16_t y = CODE_DOTS_Y; y < CODE_DOTS_Y + FONT_H; y++)
        for (uint16_t x = CODE_DOTS_X;
             x < CODE_DOTS_X + (uint16_t)(strlen(S_CODE_DOTS) * FONT_W); x++)
            if (y < MENU_H && x < BG_DRAWN_W)
                m->pix[(size_t)y * MENU_W + x] = 0;
    draw_at(m->pix, font, fontlen, S_CODE_DOTS, CODE_DOTS_X, CODE_DOTS_Y);
    dat_free(&d);
    return 0;
}

/* The screen F4 reaches: the input-device chooser. Choosing is NOT
 * reconstructed - the port always uses the mouse and the keyboard. */
int menu_input_screen(const char *dir, menu_screen *m, const char **err)
{
    dat_file d;
    const uint8_t *sec, *font;
    uint32_t fontlen;
    if (screen_start(dir, m, &d, &sec, &font, &fontlen, 1, err) != 0)
        return -1;
    draw_at(m->pix, font, fontlen, S_INPUT_TYPE, INPUT_TITLE_X, INPUT_TITLE_Y);
    draw_at(m->pix, font, fontlen, S_INPUT_1, INPUT_OPT_X, INPUT_OPT_Y);
    draw_at(m->pix, font, fontlen, S_INPUT_2, INPUT_OPT_X,
            INPUT_OPT_Y + INPUT_OPT_STEP);
    draw_at(m->pix, font, fontlen, S_INPUT_3, INPUT_OPT_X,
            INPUT_OPT_Y + 2 * INPUT_OPT_STEP);
    dat_free(&d);
    return 0;
}

/* WHICH FILE AND SECTION IS THE GAME'S LEVEL N?
 *
 * Transcribed from image 0x0EF7E (DS:0x54EE), and the decode from the loader at
 * image 0x01983:
 *
 *   0x01983  lea si, [0x54ee]        ; this table
 *   0x01987  mov cl, [bx + si]       ; n = table[level index]
 *   0x01989  dec cl
 *   0x0198e  shr cx, 1               ; \ section = ((n-1) >> 1) & 7
 *   0x01998  and cx, 7               ; /
 *   0x01990  mov ax, cx              ; \ file    = (n-1) >> 4
 *   0x01992  shr ax,1 / shr ax,1 / shr ax,1
 *   0x0199b  lea si, [0x2551]        ; the digits of "level000.dat"
 *   0x019a4  lea dx, [0x254c]        ; the name itself
 *
 * 120 entries - four ratings of thirty - terminated by a zero. They cover all
 * 80 (file, section) slots the data ships, with 40 slots referenced twice
 * because those levels appear in two ratings; ODDTABLE.DAT holds the differing
 * parameters for exactly those 40, which is the check that this reading is
 * right. It also settles what the game calls its levels: entry 0 is
 * level009 section 1, and the original's briefing for that level says
 * "Level 1".
 *
 * The low bit of n-1 is discarded by the shift and is not decoded here; the
 * loader tests something else with it further on. Unread, and said so.
 */
static const uint8_t LEVEL_ORDER[120] = {
    /* Fun 1-30 */
    147, 155, 157, 149, 151, 153, 159,  14,  22,  54,
     70,  16,  29,  32,  38,  42,  48,  72,  84, 104,
    138,  23,  68,  96,  98, 116,  78, 100, 108, 134,
    /* Tricky 1-30 */
      1,  30,  36,  50,  52,  56,  58,  80, 102, 120,
    128, 130, 136,   5, 148, 152, 154, 156, 160,   7,
     11,  13,  15,  17,  19,  21,  25,  27,  33,  31,
    /* Taxing 1-30 */
     37,  39,  41,  43,  45,  47,  49,  51,  53,  55,
     57,  59,  61,  63,   3,  65,  67,  69,  71,  73,
     75,  77,  79,  81,  83,  85,  87,  89,  35, 111,
    /* Mayhem 1-30 */
     91,  93,  95,  97,  99, 101, 103, 105, 107, 109,
    112, 113, 115, 117, 119, 121, 123, 125, 127, 150,
    129,   9, 131, 133, 135, 137, 139, 141, 143, 145,
};

/* Where the game's level `number` (1..30) of `rating` (0..3) lives. Returns 0
 * and fills in the container's name and the section index. */
int level_locate(uint8_t rating, uint8_t number, char *file, size_t file_len,
                 uint32_t *section)
{
    if (rating > 3 || number < 1 || number > 30) return -1;
    uint8_t n = LEVEL_ORDER[rating * 30 + (number - 1)];
    if (n == 0) return -1;
    /* The original writes the digits into a template rather than formatting a
     * name; digits3 is that routine, and using it here keeps the transcription
     * on the path rather than beside it. */
    snprintf(file, file_len, "level000.dat");
    digits3((uint8_t)((n - 1) >> 4), file + 5);
    *section = (uint32_t)(((n - 1) >> 1) & 7);
    return 0;
}

/* The level briefing, which the original shows between the menu and the level.
 *
 * The TEMPLATES are transcribed from the executable, with the addresses they
 * came from, and the numbers are written into them at fixed offsets - which is
 * what the original does: every one of these strings has exactly the run of
 * spaces the value needs, in the place it needs them.
 *
 * The text was not guessed. It was read back off the original's own briefing
 * screen by matching each 16x16 cell against the font's own glyph masks, so
 * these are the strings the original drew, character for character.
 */
static const char T_LEVEL[]   = "Level   ";               /* image 0x0E59D */
static const char T_COUNT[]   = "Number of Lemmings    ";  /* image 0x0E5CD */
static const char T_SAVED[]   = "     To Be Saved";        /* image 0x0E5E7 */
static const char T_RATE[]    = "Release Rate   ";         /* image 0x0E5FB */
static const char T_TIME[]    = "Time   Minutes";          /* image 0x0E60E */
static const char T_RATING[]  = "Rating  Mayhem";          /* image 0x0E620 */
static const char T_CONTINUE[] = "Press mouse button to continue"; /* 0x0E633 */

/* The four ratings, six characters each, exactly as they sit in the
 * executable: "Fun   TrickyTaxingMayhem". */
static const char RATINGS[] = "Fun   TrickyTaxingMayhem";  /* image 0x0E691 */

/* Cell positions, MEASURED off the original's briefing screen. They are not a
 * rule: this screen's last line is centred exactly, the menu's credit lines
 * are centred less half a character, and "Enter Code" is centred less a whole
 * one. Constants, because that is what the evidence supports. */
#define BR_TITLE_Y     80
#define BR_LEVEL_X      0
#define BR_NAME_X     128      /* the LVL's own 32-char field, padding and all */
#define BR_BODY_X     160
#define BR_BODY_Y     128
#define BR_BODY_STEP   32
#define BR_CONT_X      80
#define BR_CONT_Y     304

/* Write `n` right-aligned so that it ENDS at `end` in `dst`. The original
 * leaves exactly the right run of spaces for this in every template. */
static void put_num(char *dst, size_t end, uint32_t n)
{
    size_t i = end;
    do {
        dst[i--] = (char)('0' + n % 10);
        n /= 10;
    } while (n && i != (size_t)-1);
}

int menu_briefing_screen(const char *dir, const level *lv, uint8_t rating,
                         menu_screen *m, const char **err)
{
    dat_file d;
    const uint8_t *sec, *font;
    uint32_t fontlen;
    /* NO BACKGROUND, and it is not a stored bitmap to go and find.
     *
     * The briefing's background is NOT the menu's tile: the original draws it
     * in DAC 56..59, which on this screen are the LEVEL's terrain colours
     * (227,130,32) rather than the menu's greens, and the tile matches at no
     * bit offset - 1.68% with bit 3 set.
     *
     * tools/probe_briefing_bg.py narrows it. Two lit captures from separate
     * runs are identical pixel for pixel, so it is deterministic; and a search
     * of every data file and the executable, at all eight bit alignments,
     * finds ZERO matches for it while the same search finds 298 for the menu's
     * background, which is stored.
     *
     * Not "generated", though - that was this project's first write-up of it
     * and it overstated the evidence. Watching writes into the EGA window
     * shows the screen cleared at image 0x04E34 and then drawn by the ordinary
     * blitter at 0x089C4..0x098D4 from a buffer at linear 0x58000, whose first
     * bytes are MAIN.DAT section 3 exactly - the menu's own graphics. It is
     * data, and it is that file. But it is NOT the tile at section 3 offset 0:
     * drawn with bit 3 set, that scores 1.65% at its best vertical phase.
     * Where in section 3 it comes from is unfound. See STATUS.md.
     *
     * So this leaves it black rather than drawing the menu's tile underneath.
     * A plausible-looking wrong background is worse than an obviously missing
     * one: it invites the screen to be called finished, and it would score 66%
     * while looking about right. The TEXT is right - read back off the
     * original's own briefing by matching each cell against the font's glyph
     * masks - and that is what tools/verify_menu.py scores. */
    if (screen_start(dir, m, &d, &sec, &font, &fontlen, 0, err) != 0)
        return -1;

    char line[64];

    /* "Level   " with the number written into its trailing spaces, then the
     * level's own 32-character name field - whose padding is what centres the
     * title, so it is drawn whole rather than trimmed. */
    /* The number goes at offset 6, LEFT-aligned in the two spaces the
     * template leaves - "Level 1 ", "Level 10". Right-aligning it to the end
     * of the template gives "Level  1", which is what this drew until the
     * original's own briefing was read back glyph by glyph and did not
     * match. */
    snprintf(line, sizeof(line), "%s", T_LEVEL);
    {
        char num[8];
        snprintf(num, sizeof(num), "%u", lv->level_number);
        memcpy(line + 6, num, strlen(num));
    }
    draw_at(m->pix, font, fontlen, line, BR_LEVEL_X, BR_TITLE_Y);
    draw_at(m->pix, font, fontlen, lv->name, BR_NAME_X, BR_TITLE_Y);

    uint16_t y = BR_BODY_Y;

    snprintf(line, sizeof(line), "%s", T_COUNT);
    put_num(line, strlen(T_COUNT) - 2, lv->num_lemmings);
    draw_at(m->pix, font, fontlen, line, BR_BODY_X, y);
    y += BR_BODY_STEP;

    /* The percentage, not the count: "10%  To Be Saved" for 1 of 10. */
    snprintf(line, sizeof(line), "%s", T_SAVED);
    {
        uint32_t pct = lv->num_lemmings
            ? (uint32_t)lv->num_to_rescue * 100u / lv->num_lemmings : 0;
        char pc[16];
        snprintf(pc, sizeof(pc), "%u%%", pct);
        memcpy(line, pc, strlen(pc));
    }
    draw_at(m->pix, font, fontlen, line, BR_BODY_X, y);
    y += BR_BODY_STEP;

    snprintf(line, sizeof(line), "%s", T_RATE);
    put_num(line, strlen(T_RATE) - 1, lv->release_rate);
    draw_at(m->pix, font, fontlen, line, BR_BODY_X, y);
    y += BR_BODY_STEP;

    snprintf(line, sizeof(line), "%s", T_TIME);
    put_num(line, 5, lv->time_limit);
    draw_at(m->pix, font, fontlen, line, BR_BODY_X, y);
    y += BR_BODY_STEP;

    snprintf(line, sizeof(line), "%s", T_RATING);
    memcpy(line + 8, RATINGS + (rating % 4) * 6, 6);
    /* The template's own name is "Mayhem", six characters; a shorter one such
     * as "Fun   " brings its trailing spaces with it, which is why the slots
     * are padded to six in the executable. */
    draw_at(m->pix, font, fontlen, line, BR_BODY_X, y);

    draw_at(m->pix, font, fontlen, T_CONTINUE, BR_CONT_X, BR_CONT_Y);

    dat_free(&d);
    return 0;
}

/* The scrolling banner's BAR.
 *
 * Not a sprite, and worth saying why the search for one was wrong. Logging the
 * blits that land in these rows finds the source at image 0x191BC - in BSS -
 * and dumping it shows rows of 0xFF, 0xF0 and 0x00. Solid fills. The bar is a
 * strip the game FILLS and then renders scrolling text into, so there is no
 * bitmap in any file to go and find; it is drawn.
 *
 * The geometry is measured off the original's screen: x 36..604, y 334..349,
 * colour 13 (DAC 61, a dull yellow), with two dashed rules at y 336 and 347
 * where the colour appears only where (x & 7) >= 4. Checked over x 100..600 on
 * a captured menu: 499/500 for the solid rows and 495/500 for the dashed ones,
 * the handful of misses being the scrolling text that was passing through.
 *
 * The TEXT is not drawn, and neither are the two lemmings holding the ends
 * (x 12..35 and 605..628). The text scrolls, so a composed frame cannot match
 * it; the lemmings are sprites that have not been located. */
#define BAR_X0    36
#define BAR_X1   604
#define BAR_Y0   334
#define BAR_Y1   349
#define BAR_COL   13
#define BAR_RULE_A 336
#define BAR_RULE_B 347

static void draw_banner(uint8_t *dst)
{
    for (uint16_t y = BAR_Y0; y <= BAR_Y1; y++) {
        int dashed = (y == BAR_RULE_A || y == BAR_RULE_B);
        for (uint16_t x = BAR_X0; x <= BAR_X1; x++) {
            if (dashed && (x & 7) < 4)
                continue;
            dst[(size_t)y * MENU_W + x] = BAR_COL;
        }
    }
}

/* `rating` picks which of the four names goes on the plate: 0 Fun, 1 Tricky,
 * 2 Taxing, 3 Mayhem. ONLY RATING 0 IS VERIFIED - it is the one the original
 * shows on a fresh start, fitted at 100.00% over 1,760 ink pixels. The other
 * three are the next three blocks at the same stride, which is the obvious
 * reading and is not the same as a checked one. */
int menu_build_rating(const char *dir, uint8_t rating, menu_screen *m,
                      const char **err)
{
    memset(m, 0, sizeof(*m));

    char path[1024];
    snprintf(path, sizeof(path), "%s/main.dat", dir);
    dat_file d;
    if (dat_load(path, &d, err) != 0)
        return -1;
    if (d.count <= MENU_SECTION) {
        dat_free(&d);
        if (err) *err = "main.dat has no menu section";
        return -1;
    }
    if (d.count <= FONT_SECTION) {
        dat_free(&d);
        if (err) *err = "main.dat has no font section";
        return -1;
    }
    const uint8_t *sec = d.sections[MENU_SECTION].data;
    uint32_t seclen = d.sections[MENU_SECTION].size;
    const uint8_t *font = d.sections[FONT_SECTION].data;
    uint32_t fontlen = d.sections[FONT_SECTION].size;
    if (seclen < SIGN_BASE + 6u * SIGN_STEP) {
        dat_free(&d);
        if (err) *err = "menu section is shorter than the menu needs";
        return -1;
    }

    m->pix = calloc((size_t)MENU_W * MENU_H, 1);
    if (!m->pix) {
        dat_free(&d);
        if (err) *err = "out of memory";
        return -1;
    }
    for (uint8_t i = 0; i < 16; i++) {
        m->palette[i].r = dac8(MENU_PAL6[i * 3 + 0]);
        m->palette[i].g = dac8(MENU_PAL6[i * 3 + 1]);
        m->palette[i].b = dac8(MENU_PAL6[i * 3 + 2]);
    }

    /* The background, tiled from (0,0) - but only 632 pixels wide, NOT 640.
     * The original leaves the last byte of each row black: it has no non-black
     * pixel at all beyond x=635, while the left edge is drawn normally. Two
     * tile blits of 40 and 39 bytes cover 0..631 and the rest stays as the
     * screen clear left it.
     *
     * The vertical repeat of 104 does not divide 350, so the bottom tile is
     * cut off mid-way, which is what the original shows too. */
    for (uint16_t y = 0; y < MENU_H; y++)
        for (uint16_t x = 0; x < BG_DRAWN_W; x++)
            m->pix[(size_t)y * MENU_W + x] =
                planar(sec, BG_PLANE0, BG_PLANE1 - BG_PLANE0, BG_STRIDE, 2,
                       x % BG_W, y % BG_H);

    /* The logo's DROP SHADOW, before the logo itself.
     *
     * The original blits a second 632x94 sprite - image 0x097F4, 7,426 bytes,
     * the logo's exact size - into rows 20..113, ten rows below the logo and
     * eight pixels left of it, in black. That is the black halo that had this
     * project fitting seven models to it: keyed compositing was right all
     * along, and there was simply another sprite underneath.
     *
     * Its source is a separate buffer (page 0x5F000), not the logo's own data,
     * so this uses the logo's silhouette as a stand-in. That is a fit, not a
     * transcription, and it is marked as one: 99.36% against the original's
     * screen over the logo's rows, up from 93.80% with no shadow at all. The
     * real shadow bitmap has not been located. */
    for (uint16_t sy = 0; sy < LOGO_H; sy++) {
        uint16_t y = (uint16_t)(LOGO_Y + LOGO_SHADOW_DY + sy);
        if (y >= MENU_H) break;
        for (uint16_t sx = 0; sx < LOGO_W; sx++) {
            int32_t x = (int32_t)LOGO_X + LOGO_SHADOW_DX + sx;
            if (x < 0 || x >= BG_DRAWN_W) continue;
            if (planar(sec, LOGO_BASE, LOGO_PLANE, LOGO_STRIDE, 4, sx, sy))
                m->pix[(size_t)y * MENU_W + (uint16_t)x] = 0;
        }
    }

    blit(m->pix, sec, LOGO_BASE, LOGO_W, LOGO_H, LOGO_STRIDE, LOGO_PLANE,
         LOGO_X, LOGO_Y);
    /* The plates cast shadows too, and the same way: image 0x09128 writes into
     * rows 128..188 while the plates occupy 120..180, and one byte to the
     * left. Eight down, eight across. */
    for (uint8_t i = 0; i < 6; i++)
        for (uint16_t sy = 0; sy < SIGN_H; sy++) {
            uint16_t y = (uint16_t)(SIGN_POS[i].y + SIGN_SHADOW_DY + sy);
            if (y >= MENU_H) break;
            for (uint16_t sx = 0; sx < SIGN_W; sx++) {
                int32_t x = (int32_t)SIGN_POS[i].x + SIGN_SHADOW_DX + sx;
                if (x < 0 || x >= BG_DRAWN_W) continue;
                if (planar(sec, SIGN_BASE + (uint32_t)i * SIGN_STEP,
                           SIGN_PLANE, SIGN_STRIDE, 4, sx, sy))
                    m->pix[(size_t)y * MENU_W + (uint16_t)x] = 0;
            }
        }

    for (uint8_t i = 0; i < 6; i++)
        blit(m->pix, sec, SIGN_BASE + (uint32_t)i * SIGN_STEP,
             SIGN_W, SIGN_H, SIGN_STRIDE, SIGN_PLANE,
             SIGN_POS[i].x, SIGN_POS[i].y);

    /* The rating name, over its plate. Opaque for the same reason the FX
     * label is: it swaps, so drawing one rating has to erase the last. */
    for (uint16_t sy = 0; sy < RATING_H; sy++) {
        uint16_t y = (uint16_t)(RATING_Y + sy);
        if (y >= MENU_H) break;
        for (uint16_t sx = 0; sx < RATING_W; sx++) {
            uint16_t x = (uint16_t)(RATING_X + sx);
            if (x >= BG_DRAWN_W) break;
            m->pix[(size_t)y * MENU_W + x] =
                planar(font, RATING_BASE + (uint32_t)(rating % 4) * RATING_STEP,
                       RATING_PLANE, RATING_STRIDE, 4, sx, sy);
        }
    }

    /* The FX plate's label, which is not part of its plate bitmap - and which
     * is drawn OPAQUELY, unlike everything else on this screen.
     *
     * Its own black is real, not transparent: keying it out leaves the plate's
     * yellow showing where the original has black around the letters, and
     * costs 94.04% against 98.79% over the label's neighbourhood. That it is
     * opaque follows from what it is for - the label swaps between FX and its
     * alternate, so drawing one has to erase the other, and a keyed blit
     * cannot. */
    for (uint16_t sy = 0; sy < LABEL_H; sy++) {
        uint16_t y = (uint16_t)(LABEL_Y + sy);
        if (y >= MENU_H) break;
        for (uint16_t sx = 0; sx < LABEL_W; sx++) {
            uint16_t x = (uint16_t)(LABEL_X + sx);
            if (x >= BG_DRAWN_W) break;
            m->pix[(size_t)y * MENU_W + x] =
                planar(sec, LABEL_BASE + LABEL_FX * LABEL_SPRITE,
                       LABEL_PLANE, LABEL_STRIDE, 4, sx, sy);
        }
    }

    draw_text(m->pix, font, fontlen, CREDIT_1, CREDIT_1_Y);
    draw_text(m->pix, font, fontlen, CREDIT_2, CREDIT_2_Y);
    draw_banner(m->pix);

    dat_free(&d);
    return 0;
}

/* The port's own. */
void menu_free(menu_screen *m)
{
    if (!m) return;
    free(m->pix);
    m->pix = NULL;
}

/* The signs, as things the player can choose.
 *
 * The keys are the ones the original prints on the plates themselves - F1, F2,
 * F3, ESC and F4 - so the mapping is read off the game's own screen rather
 * than guessed at. The "FUN" plate carries two small arrow tabs instead of a
 * function key, which is why it answers to Up and Down.
 *
 * The box is the whole 120x61 sprite, which includes the lemming standing
 * behind the plate. Whether the original's own hit test is that generous has
 * not been checked; it is the obvious reading of the sprite and it is marked
 * here as unverified rather than presented as transcribed.
 */
static const view_hotspot SIGN_SPOT[6] = {
    { SIGN_W, SIGN_H, 0, 0, VIEW_KEY_F1,   MENU_PLAY      },
    { SIGN_W, SIGN_H, 0, 0, VIEW_KEY_F2,   MENU_NEW_LEVEL },
    { SIGN_W, SIGN_H, 0, 0, VIEW_KEY_F3,   MENU_FX        },
    /* The rating plate takes Up and Down, and this is read off the original's
     * own dispatcher rather than found by sweeping keys. The menu translates a
     * scancode through a table at ds:0x202E (image 0x0BABE) and compares the
     * result at image 0x0447B:
     *
     *     Up  (0x48) -> 0x8a  and  `  (0x29) -> 0x60   go to image 0x044EC
     *     Down(0x50) -> 0x8b  and  - (0x0c) -> 0x2d   go to image 0x0451C
     *     keypad - (0x4a) -> 0x2d as well
     *
     * Up caps at 3 (image 0x044F9), Down floors at 0 (image 0x04529), and
     * both then write ds:0x1F6B = rating * 30. tools/probe_rating.py drives
     * all five keys against the original and watches ds:0x1F6E.
     *
     * An earlier note here said "the rating plate answers to NOTHING that has
     * been found... neither Up nor Down moves it". That was wrong, and the
     * way it was wrong is worth keeping: the sweep that produced it watched
     * the plate's PIXELS from a rating already at Fun, where Down is supposed
     * to do nothing, and its key list did not contain Up at all. */
    { SIGN_W, SIGN_H, 0, 0, VIEW_KEY_UP,   MENU_RATING_UP },
    { SIGN_W, SIGN_H, 0, 0, VIEW_KEY_ESC,  MENU_EXIT      },
    { SIGN_W, SIGN_H, 0, 0, VIEW_KEY_F4,   MENU_HELP      },
};

/* The port's own. The KEYS are the original's, read off the plates it draws
 * on its own screen - F1, F2, F3, ESC, F4 - so the mapping is evidence. What
 * each does beyond F1 and ESC is not reconstructed. */
void menu_hotspots(view_hotspot *spots)
{
    /* The plates are KEYBOARD ONLY, with no click box of their own. That is
     * measured, not a simplification, and it cost three experiments:
     *
     *   - the menu calls INT 33h AX=0002h once - HIDE CURSOR - and draws no
     *     pointer: moving the position to (600,40) changes nothing anywhere
     *     near it, only the scroller keeps moving.
     *   - it polls AX=0003h 1342 times, so it is reading the BUTTON, and the
     *     position reaches it and is ignored.
     *   - clicking "EXIT TO DOS" does not exit. It starts the level, exactly
     *     as clicking "1 PLAYER" or clicking bare background does.
     *
     * So a click anywhere acts as F1, and that is the last hotspot below.
     * Giving each plate its own box would be inventing behaviour the original
     * does not have, which is a worse kind of wrong than leaving it out. */
    for (uint8_t i = 0; i < 6; i++) {
        spots[i].x = SIGN_POS[i].x;
        spots[i].y = SIGN_POS[i].y;
        spots[i].w = 0;
        spots[i].h = 0;
        spots[i].key = SIGN_SPOT[i].key;
        spots[i].id = SIGN_SPOT[i].id;
    }
    /* The rating plate's other three keys. A hotspot carries one key, so the
     * alternates need entries of their own; they are click-less, because the
     * plate's own box above is what a click would hit. */
    static const struct { view_key key; int32_t id; } RATING_ALT[3] = {
        { VIEW_KEY_BACKQUOTE, MENU_RATING_UP   },
        { VIEW_KEY_DOWN,      MENU_RATING_DOWN },
        { VIEW_KEY_MINUS,     MENU_RATING_DOWN },
    };
    for (uint8_t i = 0; i < 3; i++) {
        spots[6 + i].x = SIGN_POS[3].x;
        spots[6 + i].y = SIGN_POS[3].y;
        spots[6 + i].w = 0;
        spots[6 + i].h = 0;
        spots[6 + i].key = RATING_ALT[i].key;
        spots[6 + i].id = RATING_ALT[i].id;
    }
    /* A click anywhere on the screen, which the original treats as F1. */
    spots[9].x = 0;
    spots[9].y = 0;
    spots[9].w = MENU_W;
    spots[9].h = MENU_H;
    spots[9].key = VIEW_KEY_NONE;
    spots[9].id = MENU_PLAY;
}

/* The port's own. The original's own instruction on the briefing is "Press
 * mouse button to continue", so a click dismisses these screens; Esc does too,
 * which is the port's addition and harmless - the original has no other way
 * off the briefing, but a window that only closes on a mouse click is a poor
 * thing on a machine with a keyboard. */
void menu_show_until_dismissed(const menu_screen *m, const char *title,
                               uint8_t scale)
{
    view_hotspot any[2];
    any[0].x = 0;
    any[0].y = 0;
    any[0].w = MENU_W;
    any[0].h = MENU_H;
    any[0].key = VIEW_KEY_NONE;
    any[0].id = 1;
    any[1] = any[0];
    any[1].w = 0;
    any[1].h = 0;
    any[1].key = VIEW_KEY_ESC;
    view_interact(m->pix, MENU_W, MENU_H, m->palette, title, scale, any, 2);
}

menu_action menu_run(const menu_screen *m, uint8_t scale)
{
    view_hotspot spots[MENU_HOTSPOTS];
    menu_hotspots(spots);
    int32_t r = view_interact(m->pix, MENU_W, MENU_H, m->palette,
                              "PC Lemmings", scale, spots, MENU_HOTSPOTS);
    if (r == VIEW_CLOSED) return MENU_EXIT;
    return (menu_action)r;
}

/* The menu with the rating it starts on. */
int menu_build(const char *dir, menu_screen *m, const char **err)
{
    return menu_build_rating(dir, 0, m, err);
}
