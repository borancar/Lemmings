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


/* ======================================================================
 * THE DISK CHECK - image 0x0062B, transcribed, and DELIBERATELY NO-OP.
 *
 * The original decides whether to run its copy protection from the media
 * descriptor byte it saved at start-up: image 0x00090 asks DOS for the drive
 * data (INT 21h AH=1Bh), keeps `[bx]` in ds:0x1FDE, and image 0x0062B reads
 * it back. 0xF8 is a fixed disk, and the routine jumps clear of the whole
 * thing to image 0x00E66. Anything else is a floppy, and it calls image
 * 0x0063E and treats carry-clear as a pass; carry set falls into image
 * 0x00EDF, which prints "Lemmings Disk 1 Not found, Insert into drive A or B
 * and retry" from ds:0x20D4 and exits through INT 21h AX=4C00.
 *
 * Image 0x0063E is not a disk read in any ordinary sense and is NOT
 * transcribed. It pushes the low interrupt vectors 0..0x14, points vector 3
 * at itself and executes `int3` - it is the trap-flag protection that
 * decrypts itself one instruction at a time and re-encrypts behind itself,
 * the same one image/README.md describes and tools/decrypt_image.py had to
 * drive at media 0xF9 to recover 651 bytes of plaintext.
 *
 * So it is a no-op here, behind a define, rather than a gap:
 *
 *   - There is no floppy to check. The port loads from a directory, which is
 *     the media byte's own 0xF8 case, so the honest transcription of image
 *     0x0062B for this port takes the branch it already takes.
 *   - Reconstructing a protection check would be reconstructing a thing whose
 *     only purpose is to refuse to run, and it cannot pass without the
 *     physical disk it was written to look for.
 *   - It stays visible instead of being quietly dropped, because "the port
 *     does not do this, on purpose" and "nobody has read this yet" are
 *     different claims and this file keeps them apart.
 *
 * Set LEM_DISK_CHECK to 1 and the shape of the original is still here to
 * hang an implementation on; disk_protection() is where image 0x0063E would
 * go, and it is not written.
 * ====================================================================== */

#define LEM_DISK_CHECK 0

#define DISK_MEDIA_FIXED 0xF8      /* image 0x0062E */

#if LEM_DISK_CHECK
/* NOT TRANSCRIBED. Image 0x0063E, the trap-flag protection - see above. */
static int disk_protection(void);
#endif

/* Transcribed from image 0x0062B. Returns 0 to carry on, -1 for the path
 * that prints ds:0x20D4 and exits. */
int disk_check(uint8_t media)
{
    /* Image 0x0062E: a fixed disk, so there is nothing to check. */
    if (media == DISK_MEDIA_FIXED)
        return 0;

#if LEM_DISK_CHECK
    /* Image 0x00635 calls it; image 0x00638 takes carry-clear as a pass and
     * image 0x0063A is the failure. */
    return disk_protection() ? -1 : 0;
#else
    /* The no-op: every medium passes. Nothing in this port calls disk_check
     * yet either - the start-up at image 0x00090 that reads the media byte
     * is not transcribed - so this is the decision recorded, not a check
     * being performed. */
    return 0;
#endif
}


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

/* ======================================================================
 * TRANSCRIBED - image 0x036B6: the LVL header parse; image 0x07C90:
 * the terrain entry decode. The rest of this section (objects, steel,
 * the name) is written from the published format, not transcribed.
 * Checked by tools/verify_level.py: 15 fields, all agreeing.
 * ====================================================================== */

/* be16 is defined once, above: both the DAT header and the LVL header
 * are big-endian, which is the Amiga heritage showing through. */

/* A 9-bit two's-complement field, as the terrain y position uses.
 *
 * Transcribed from image 0x07C90 - `sar bx,cl` with cl=7, an ARITHMETIC
 * shift that sign-extends the 9-bit field.
 */
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
        o->x = (int16_t)(x - 16);
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

/* ======================================================================
 * NOT TRANSCRIBED - GROUNDxO.DAT, from the published description.
 * Where this lives in the original is not established.
 * ====================================================================== */




/* ======================================================================
 * NOT TRANSCRIBED - VGAGRx.DAT, from the published description.
 * Where this lives in the original is not established.
 * ====================================================================== */

uint8_t g_plane_bits[4] = { 0, 1, 2, 3 };
uint8_t g_msb_first = 1;



/* ======================================================================
 * NOT TRANSCRIBED - terrain composition.
 *
 * Proven against what the original composes rather than against its
 * screen: tools/prove.sh, 49,741 pixels, zero differing. That is strong
 * evidence of equivalence and is not the same claim as transcription.
 * ====================================================================== */

terrain_mode g_terrain_mode = TERRAIN_REPLACE;
uint8_t g_terrain_reverse = 0;

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



/* ======================================================================
 * THE LEVEL'S CONTROL PANEL - its font and its status line.
 *
 * The level is mode 0x0D, 320x200, and it draws nothing through the blitter
 * at image 0x8822 that every menu screen uses. tools/probe_writes.py finds
 * the path instead, by attributing every write into the EGA window to the
 * instruction that made it: three `stosb` sites at images 0x077B9, 0x077DD
 * and 0x077F5, and a fourth at 0x07804 that writes zeroes.
 *
 * Three, not four: this font has THREE planes. The glyph stride confirms it -
 * image 0x07782 shifts the index left four times for *16, then 0x0778C shifts
 * once more and adds again for *48, and 48 is three planes of sixteen rows.
 *
 * The character dispatch is image 0x07756:
 *
 *     ' '  -> image 0x077FE, sixteen rows of zero
 *     '%'  -> table 0
 *     ':'  -> table 0x210
 *     '0'..'9' -> table 0x30
 *     'A'..'Z' -> table 0x240
 *     anything else draws nothing
 *
 * and every glyph is reached as base + 0xA0 + table + index * 48. That
 * arithmetic closes exactly on the font's size: 0xA0 + 0x240 + 26*48 = 1984,
 * which is the byte count image 0x0593C copies (cx = 0x3E0 WORDS) from offset
 * 0x1900 of the loaded section. 6400 + 1984 is main.dat section 2's length to
 * the byte, so the font is the tail of section 2.
 * ====================================================================== */
#define PANEL_FONT_OFF  6400        /* image 0x0592C: mov si, 0x1900 */
#define PANEL_FONT_LEN  1984        /* image 0x0593C: cx = 0x3E0 words */
#define PANEL_GLYPH     48          /* image 0x0778C: three planes of 16 */
#define PANEL_GLYPH_H   16
#define PANEL_PLANE     16
#define PANEL_HEAD      0xA0        /* image 0x077A8: add si, 0xa0 */
#define PANEL_T_PERCENT 0x000
#define PANEL_T_DIGIT   0x030
#define PANEL_T_COLON   0x210
#define PANEL_T_LETTER  0x240

/* The panel's status line, image 0x0BB10 (ds:0x2080). Forty-four characters,
 * which is the panel's own stride - image 0x077BB steps the destination by
 * 0x2B and `stosb` has already added the forty-fourth. */
const char PANEL_TEMPLATE[PANEL_COLS + 1] =
    "230.              OUT      IN   %  TIME  :  ";

/* Where character `ch`'s glyph starts, or PANEL_FONT_LEN for "draws
 * nothing" - image 0x07756's dispatch, in the order the original tests. */
static uint32_t panel_glyph(uint8_t ch)
{
    if (ch == '%') return PANEL_HEAD + PANEL_T_PERCENT;
    if (ch == ':') return PANEL_HEAD + PANEL_T_COLON;
    if (ch >= '0' && ch <= '9')
        return PANEL_HEAD + PANEL_T_DIGIT + (uint32_t)(ch - '0') * PANEL_GLYPH;
    if (ch >= 'A' && ch <= 'Z')
        return PANEL_HEAD + PANEL_T_LETTER + (uint32_t)(ch - 'A') * PANEL_GLYPH;
    return PANEL_FONT_LEN;          /* space, and everything unmapped */
}

#define PANEL_NUM_OFF   0        /* image 0x07695: add ax, 0 */
#define PANEL_NUM_H     8        /* image 0x0769F: cx = 8 */
#define PANEL_NUM_STEP  16       /* image 0x0768D: ax *= 16 */
#define PANEL_NUM_X     4        /* pixels into the button */
#define PANEL_NUM_Y     17       /* rows into the panel: screen 177 */
#define PANEL_BUTTON_W  16
#define PANEL_BUTTONS   10
#define PANEL_NUM_INK   3
#define PANEL_NUM_BG    0

/* ======================================================================
 * THE SELECTED BUTTON'S BOX - image 0x0780D.
 *
 *     ax += ax / di = ax + 0x2100 + [0x1f76] + 2     two bytes a button
 *     Enable Set/Reset 0x0C                          planes 2,3 cleared,
 *                                                    0,1 from the CPU - so
 *                                                    writing 0xFF is colour 3
 *     al = 0xFF / stosb / stosb / add di,0x2a        the top edge, 16 px
 *     cx = 0x16                                      twenty-two middle rows
 *       Bit Mask 0x80 / stosb 0x80                     the leftmost pixel
 *       Bit Mask 0x01 / stosb 0x01                     and the rightmost
 *       add di,0x2a
 *     Bit Mask 0xFF / al = 0xFF / stosb / stosb      the bottom edge
 *
 * Twenty-four rows from video row 192, which is the panel's row 16, and
 * 0x2102 puts button 0 at byte 2 - so button b starts 16 + 16b pixels into
 * the 352-wide line. The colour is 3, which is what a recorded frame shows
 * and what Enable Set/Reset 0x0C makes of a 0xFF.
 *
 * Which button is boxed is ds:0x83, and image 0x0427 starts it at 3 - the
 * third skill button, whether or not the level offers that skill. ds:0x82 is
 * the one boxed before it, so image 0x3B29 can rub one out and draw the
 * other; with nothing clicked they stay 2 and 3.
 * ====================================================================== */
#define PANEL_SEL_ROW    16      /* image 0x2102: video row 192 */
#define PANEL_SEL_H      24
#define PANEL_SEL_X0     16      /* image 0x2102: byte 2 */
#define PANEL_SEL_INK     3
#define PANEL_SEL_START   3      /* image 0x0427: ds:0x83 starts at 3 */

void panel_highlight(uint8_t *dst, uint16_t dst_w, uint16_t dst_h,
                     uint8_t button)
{
    uint16_t x0 = (uint16_t)(PANEL_SEL_X0 + button * PANEL_BUTTON_W);
    for (uint8_t r = 0; r < PANEL_SEL_H; r++) {
        uint16_t y = (uint16_t)(PANEL_SEL_ROW + r);
        if (y >= dst_h) break;
        if (r == 0 || r == PANEL_SEL_H - 1) {
            for (uint8_t c = 0; c < PANEL_BUTTON_W; c++)
                if (x0 + c < dst_w)
                    dst[(size_t)y * dst_w + x0 + c] = PANEL_SEL_INK;
        } else {
            if (x0 < dst_w)
                dst[(size_t)y * dst_w + x0] = PANEL_SEL_INK;
            if (x0 + PANEL_BUTTON_W - 1 < dst_w)
                dst[(size_t)y * dst_w + x0 + PANEL_BUTTON_W - 1] =
                    PANEL_SEL_INK;
        }
    }
}


/* ======================================================================
 * THE NUMBERS ON THE SKILL BUTTONS - image 0x0765F.
 *
 * A different font from the status line's, and one nobody had found: ten
 * digits of 16 bytes at offset 0 of the loaded panel font, which is
 * main.dat section 2 offsets 6400..6559 - the 160 bytes immediately before
 * PANEL_HEAD, which is why 0xA0 is where the big font starts.
 *
 * Each digit is FOUR pixels wide and stored twice, pre-shifted: bytes 0..7
 * hold it in the low nibble and 8..15 in the high nibble, so a two-digit
 * number is the tens from `d*16` through Bit Mask 0x0F (image 0x0766F) into
 * one byte and the units from `d*16+8` through 0xF0 (image 0x076C2) into the
 * next. That puts the pair at pixels 4..11 of the button's sixteen, which is
 * exactly where a recorded frame has them.
 *
 *     and ah,ah / je 0x76db      a count of zero is not blank
 *     inc al / sub ah,0x0a / jae al = tens, ah = units
 *     ax *= 16 / add ax,0        the glyph, from font offset 0
 *     cx = 8 / lodsb / stosb / add di,0x2b    eight rows, stepping 44
 *
 * A count of zero takes image 0x076DB, which writes 0xFF for both nibbles -
 * so an unavailable skill is a SOLID block, not an empty one. That is why a
 * recorded frame has ink in all ten button positions when the level offers
 * only diggers.
 *
 * The colours are measured, not read out of the EGA state: a set font bit is
 * index 3 and a clear one index 0. Counting says so exactly - "50" has 40
 * set bits between its two glyphs and the original shows 40 pixels of index
 * 3 and 24 of index 0, and a zero count shows 64 of index 3.
 * ====================================================================== */
/* One button's number, at (x0, y0) in the panel's own 352-wide line.
 * Transcribed from image 0x0765F - see the note above for every constant. */
void panel_button_num(uint8_t *dst, uint16_t dst_w, uint16_t dst_h,
                      const uint8_t *sec, uint32_t sec_len,
                      uint8_t value, uint16_t x0, uint16_t y0)
{
    uint32_t font = PANEL_FONT_OFF + PANEL_NUM_OFF;
    uint8_t tens = (uint8_t)(value / 10u), units = (uint8_t)(value % 10u);

    if (font + 10u * PANEL_NUM_STEP > sec_len) return;

    for (uint8_t r = 0; r < PANEL_NUM_H; r++) {
        uint16_t py = (uint16_t)(y0 + r);
        if (py >= dst_h) break;
        /* image 0x076DB: no count at all, so both nibbles get 0xFF */
        uint8_t lo = value ? sec[font + tens * PANEL_NUM_STEP + r]
                           : 0xFFu;
        uint8_t hi = value ? sec[font + units * PANEL_NUM_STEP + 8 + r]
                           : 0xFFu;
        for (uint8_t c = 0; c < 4; c++) {
            uint16_t px = (uint16_t)(x0 + c);
            if (px < dst_w)
                dst[(size_t)py * dst_w + px] =
                    (lo >> (3 - c)) & 1u ? PANEL_NUM_INK : PANEL_NUM_BG;
            px = (uint16_t)(x0 + 4 + c);
            if (px < dst_w)
                dst[(size_t)py * dst_w + px] =
                    (hi >> (7 - c)) & 1u ? PANEL_NUM_INK : PANEL_NUM_BG;
        }
    }
}

/* All ten: the release rate twice - image 0x0765F is called for the minimum
 * and the current, which a level that does not let you change it shows the
 * same - then the eight skills in the order the header stores them. */
void panel_buttons(uint8_t *dst, uint16_t dst_w, uint16_t dst_h,
                   const uint8_t *sec, uint32_t sec_len, const level *lv)
{
    uint8_t v[PANEL_BUTTONS];
    v[0] = v[1] = (uint8_t)lv->release_rate;
    for (uint8_t k = 0; k < 8; k++)
        v[2 + k] = (uint8_t)lv->skills[k];
    for (uint8_t k = 0; k < PANEL_BUTTONS; k++)
        panel_button_num(dst, dst_w, dst_h, sec, sec_len, v[k],
                         (uint16_t)(PANEL_ART_X + k * PANEL_BUTTON_W
                                    + PANEL_NUM_X),
                         PANEL_NUM_Y);
}

/* Which video plane each of a glyph's three stored planes goes to - image
 * 0x077A8, 0x077CC and 0x077E5, read off their Map Mask writes. */
static const uint8_t PANEL_PLANE_BIT[3] = { 0, 1, 3 };

/* One panel character at (x, y) in pixels.
 *
 * Three stored planes, but NOT video planes 0, 1, 2. Image 0x077A8 makes
 * three passes and each names its own plane through the Map Mask:
 *
 *     pass 1  Map Mask 0x0F, Enable Set/Reset 0x0E   plane 0 from the CPU,
 *                                                    planes 1..3 cleared
 *     pass 2  Map Mask 0x02  (image 0x077CC)         plane 1
 *     pass 3  Map Mask 0x08  (image 0x077E5)         plane 3
 *
 * so the values a glyph can take are 0..3 and 8..11, never 4..7. Reading the
 * third plane into bit 2 instead of bit 3 is wrong in a way that still draws
 * legible text, which is how it survived: against a recorded frame the port
 * was writing 5 where the original had video 9, 177 pixels of it, and every
 * character looked right.
 *
 * A space clears its cell, which is image 0x077FE writing sixteen rows of
 * zero rather than skipping. */
void panel_char(uint8_t *dst, uint16_t dst_w, uint16_t dst_h,
                const uint8_t *sec, uint32_t sec_len,
                uint8_t ch, uint16_t x, uint16_t y)
{
    uint32_t g = panel_glyph(ch);
    int blank = (g >= PANEL_FONT_LEN);
    uint32_t base = PANEL_FONT_OFF + g;
    if (!blank && base + PANEL_GLYPH > sec_len) return;

    for (uint16_t r = 0; r < PANEL_GLYPH_H; r++) {
        uint16_t py = (uint16_t)(y + r);
        if (py >= dst_h) break;
        for (uint16_t c = 0; c < 8; c++) {
            uint16_t px = (uint16_t)(x + c);
            if (px >= dst_w) break;
            uint8_t v = 0;
            if (!blank)
                for (uint8_t p = 0; p < 3; p++)
                    v = (uint8_t)(v | (((sec[base + p * PANEL_PLANE + r]
                                         >> (7 - c)) & 1u)
                                       << PANEL_PLANE_BIT[p]));
            dst[(size_t)py * dst_w + px] = v;
        }
    }
}

/* Transcribed from image 0x0417B:
 *
 *     lodsb / and al,al / je      a NUL ends the line
 *     push ax / push si
 *     call 0x7708                 draw it, cell index in AH
 *     pop si / pop ax
 *     inc ah                      the next cell
 *
 * One character per cell, and the cell index is a BYTE offset - image 0x07710
 * adds it straight to 0x1E40 - so cells are eight pixels apart.
 *
 * What still is not read is what writes the counts INTO the template before
 * this walks it. */
void panel_status(uint8_t *dst, uint16_t dst_w, uint16_t dst_h,
                  const uint8_t *sec, uint32_t sec_len,
                  const char *text, uint16_t x, uint16_t y)
{
    for (uint16_t i = 0; i < PANEL_COLS && text[i]; i++)
        panel_char(dst, dst_w, dst_h, sec, sec_len, (uint8_t)text[i],
                   (uint16_t)(x + i * 8), y);
}

/* ======================================================================
 * THE PANEL'S ARTWORK - image 0x057D1, set up at image 0x05914.
 *
 * The call site fixes every dimension:
 *
 *     si = 0        main.dat section 2, from its start
 *     di = 0xF91F   into video memory, near the top of the 64K plane: the
 *                   panel is stashed off-screen once and blitted per frame
 *     cx = 0x28     40 rows
 *     ax = 0x2C     44, the destination stride - the logical line is 352
 *                   pixels of which 320 are displayed
 *     bx = 0x428    bl = 40 bytes a row (320 pixels), bh = 4 planes
 *
 * and the loop settles the source layout. Each pass sets the Map Mask to one
 * plane, copies cx rows of bl bytes stepping the destination by ax, then pops
 * di and shifts the mask - but does NOT restore si, which runs on. So the
 * source is plane-major: all forty rows of plane 0, then all forty of plane 1.
 * That is 40 * 40 * 4 = 6,400 bytes, which is exactly where the status font
 * begins - see PANEL_FONT_OFF above. The two together are section 2's length.
 *
 * Where it lands, and in what colours, is measured rather than read: against a
 * frame recorded by tools/record_level.py the artwork sits at screen row 160,
 * and the attribute controller maps its values 0..7 to themselves and 8..15 to
 * 16..23. On that map 89.31% of the panel's pixels agree. The rest is what the
 * game draws ON the artwork - the status line, the skill counts, the minimap,
 * the selected button - by routines that have not been read yet.
 * ====================================================================== */
#define PANEL_ART_OFF   0    /* image 0x05914: si = 0 */

/* Transcribed from image 0x057D1. The original programs the Map Mask and lets
 * the card place the bits; here the four planes are assembled per pixel, which
 * is what planar() is for and why it lives in sdl_io.c. */
void panel_frame(uint8_t *dst, uint16_t dst_w, uint16_t dst_h,
                 const uint8_t *sec, uint32_t sec_len,
                 uint16_t x0, uint16_t y0)
{
    uint32_t stride = PANEL_ART_W / 8;
    uint32_t plane  = stride * PANEL_ART_H;
    if ((uint32_t)PANEL_ART_OFF + plane * PANEL_ART_PL > sec_len) return;

    for (uint16_t r = 0; r < PANEL_ART_H; r++) {
        uint16_t py = (uint16_t)(y0 + r);
        if (py >= dst_h) break;
        for (uint16_t x = 0; x < PANEL_ART_W; x++) {
            uint16_t px = (uint16_t)(x0 + x);
            if (px >= dst_w) break;
            dst[(size_t)py * dst_w + px] =
                planar(sec, PANEL_ART_OFF + (uint32_t)r * stride,
                       plane, 0, PANEL_ART_PL, x, 0);
        }
    }
}

/* ======================================================================
 * THE STATUS LINE'S CONTENTS - the 44 cells at ds:0x207F.
 *
 * Where the line sits was measured against a recorded frame rather than
 * assumed, and it is not where a 320-pixel screen would suggest. The line is
 * 44 cells of eight pixels - 352, the mode's logical line - and the panel's
 * artwork occupies its last forty, cells 4..43. So the screen's 320 pixels
 * ARE cells 4..43, and the line's own cell 0 is 32 pixels off the left edge.
 * Every character lands where that predicts: the template's 'O' of OUT at
 * col 18 draws at screen cell 14, its ':' at col 41 at screen cell 37, and
 * the four TIME characters at cols 40..43 at screen cells 36..39.
 *
 * The glyphs are sixteen rows starting at screen row 160, the panel's own
 * top - their ink lands in rows 164..174, which is what a recorded frame
 * shows.
 *
 * The base is ds:0x207F and not ds:0x2080, which the comment on
 * PANEL_TEMPLATE above had wrong. Dumping the live buffer settles it: the
 * "230." the template starts with is at 0x207F..0x2082, and the timer the
 * game writes at ds:0x20A7 and ds:0x20A9 is then cols 40 and 42, which is
 * where the recorded frames put the minutes and the seconds.
 * ====================================================================== */
#define ST_MIN        40      /* ds:0x20A7 */
#define ST_COLON      41      /* ds:0x20A8 */
#define ST_SEC_TENS   42      /* ds:0x20A9 - image 0x0222 reads this as bl */
#define ST_SEC_UNITS  43      /* ds:0x20AA - and this as bh */

static char    g_status[PANEL_COLS + 1];
static uint8_t g_tick;        /* ds:0x85 */
static uint8_t g_tick_load;   /* ds:0x86 */

const char *level_status_line(void) { return g_status; }

/* Image 0x0418 and 0x041D: ds:0x85 and ds:0x86 both start at 0x11. */
#define LEVEL_FRAMES_PER_SECOND 0x11

/* Image 0x0418 and 0x041D set ds:0x85 and ds:0x86 to 0x11 - seventeen frames
 * to a second - and image 0x040E and 0x0413 put '0' in both seconds cells.
 * Confirmed against the running game: the line goes from TIME 4:52 at frame
 * 151 to TIME 4:21 at frame 676, which is 525 frames for 31 seconds. */
void level_status_reset(const level *lv)
{
    for (uint16_t i = 0; i < PANEL_COLS; i++)
        g_status[i] = PANEL_TEMPLATE[i];
    g_status[PANEL_COLS] = 0;
    /* The level header's time limit, in minutes; one digit is all the line
     * has room for and all any shipped level needs. */
    g_status[ST_MIN]       = (char)('0' + (lv->time_limit % 10u));
    g_status[ST_COLON]     = ':';
    g_status[ST_SEC_TENS]  = '0';
    g_status[ST_SEC_UNITS] = '0';
    g_tick_load = LEVEL_FRAMES_PER_SECOND;
    /* One MORE than the reload, for the first second only. The original's
     * timer is image 0x0213, which sits later in the play loop than image
     * 0x018F, where a frame is finished and captured - so a frame shows the
     * time from before its own pass's tick. Measured: with both at 17 the
     * port and the original disagree on exactly one frame in every
     * seventeen - 850, 867, 884 - and agree on every frame either side. */
    g_tick      = LEVEL_FRAMES_PER_SECOND + 1;
}

/* Transcribed from image 0x0213. The time is kept as ASCII in the line
 * itself - there is no binary countdown behind it - so this borrows the way
 * a hand-written decimal decrement does, digit by digit:
 *
 *     dec byte [0x85] / jne          not a second yet
 *     al = [0x86] / [0x85] = al      reload
 *     al = [0x20a7]  bx = [0x20a9]   minutes, and the two seconds cells
 *     dec bh / cmp bh,'/' / jne      units, wrapping below '0'
 *     bh = '9' / dec bl / cmp bl,'/' tens
 *     bl = '5' / dec al / cmp al,'/' minutes; tens wraps to 5, not 9
 *     al = '0' / bx = 0x3030         out of time: stop at 0:00
 */
void level_timer_step(void)
{
    if (--g_tick) return;
    g_tick = g_tick_load;

    char al = g_status[ST_MIN];
    char bl = g_status[ST_SEC_TENS];
    char bh = g_status[ST_SEC_UNITS];

    if (--bh < '0') {
        bh = '9';
        if (--bl < '0') {
            bl = '5';
            if (--al < '0') {
                al = '0';
                bl = '0';
                bh = '0';
            }
        }
    }
    g_status[ST_MIN]       = al;
    g_status[ST_SEC_TENS]  = bl;
    g_status[ST_SEC_UNITS] = bh;
}

/* ======================================================================
 * THE LEVEL'S OBJECTS, ANIMATED AT DISPLAY TIME - images 0x07BB8 (draw) and
 * 0x07C0A (step), with a 28-byte state per object at ds:0x17CB.
 *
 * Both walk the same two arrays in step: the level's own 32-entry object list
 * at ds:0x0FEB, eight bytes each, and the state array, 0x1C bytes each. An
 * entry whose x reads zero ends that object's turn - image 0x07BCA and
 * 0x07C1C both `and ax,ax / je`.
 * ====================================================================== */

/* Image 0x07BB8's set-up, as the original's own loader leaves it. The fields
 * are the graphics set's, so this is the port assembling what the original
 * assembles somewhere it has not been read - marked accordingly. */
void level_anim_init(level_anim *st, const level *lv, const ground *g)
{
    memset(st, 0, sizeof(*st) * LEVEL_ANIM_MAX);
    for (uint16_t i = 0; i < lv->num_objects && i < LEVEL_ANIM_MAX; i++) {
        const level_object *o = &lv->objects[i];
        const ground_object *go = &g->objects[o->id & 0x0F];
        level_anim *a = &st[i];
        a->live       = 1;
        a->flags      = go->animation_flags;
        a->frame      = go->start_frame;
        a->end_frame  = go->end_frame;
        a->w          = go->width;
        a->rows       = go->height;
        a->frame_size = go->frame_data_size;
        a->mask_delta = go->mask_offset;
        a->base       = go->frames_base_loc;
        a->ink        = (uint16_t)(a->base + a->frame * a->frame_size);
    }
}

/* ======================================================================
 * PUTTING A NUMBER IN THE STATUS LINE - image 0x04B68.
 *
 * Not the same routine as the briefing's put_num below, which is still
 * unread and right-aligns; this one is the status line's own and is
 * left-aligned. digits3 at image 0x04BC2 is a third.
 *
 * Three cells, blanked first, then filled LEFT-aligned with leading zeros
 * suppressed - not right-aligned, which is what the line looks like at a
 * glance because the counts it holds are small. `si` only advances when a
 * digit is actually printed, which is what makes it left-aligned:
 *
 *     [si] = [si+1] = [si+2] = ' '
 *     ah = 0 / bl = 0                bl remembers a digit has been printed
 *     inc ah / sub al,100 / jae      count the hundreds
 *     add al,100 / dec ah
 *     and ah,ah / je                 no hundreds: leave si where it is
 *     add ah,'0' / inc bl / [si] = ah / inc si
 *     ...the same for tens, which also prints when bl says a hundreds digit
 *     already went out, so 105 does not come out as "1 5"
 *     add al,'0' / [si] = al / inc si
 * ====================================================================== */
void status_put_num(char *dst, uint8_t v)
{
    uint8_t al = v, ah, bl = 0;
    dst[0] = dst[1] = dst[2] = ' ';

    ah = 0;
    do { ah++; } while (al >= 100 ? (al = (uint8_t)(al - 100), 1) : 0);
    ah--;
    if (ah) { *dst++ = (char)(ah + '0'); bl++; }

    dst[0] = ' ';
    dst[1] = ' ';
    ah = 0;
    do { ah++; } while (al >= 10 ? (al = (uint8_t)(al - 10), 1) : 0);
    ah--;
    if (ah || bl) { *dst++ = (char)(ah + '0'); }

    *dst = (char)(al + '0');
}

/* ======================================================================
 * RELEASING THE LEMMINGS - image 0x30A6, and the interval at image 0x39D6.
 *
 * The interval is not the release rate: image 0x39D6 computes it as
 *
 *     al = 99 - rate / al >>= 1 / neg al / add al,0x35 / neg al / add al,0x39
 *
 * which is ((99 - rate) >> 1) + 4 once the two negations cancel - 28 frames
 * at the release rate of 50 that "Just dig!" carries. The countdown starts
 * at 0x14, twenty, from image 0x39D1, and only runs once the entrance is
 * open, because image 0x30B0 tests ds:0x55 before image 0x30BA decrements.
 *
 * Checked against the running game, which is where the numbers came from:
 * the gate opens on pass 36 and ds:0x43 goes to 1 on pass 56, then 2, 3, 4
 * ... on 84, 112, 140, 168, 196, 224, 252, 280 and 308. Twenty then
 * twenty-eight, exactly.
 * ====================================================================== */
static uint8_t g_release_wait;   /* ds:0x48 */
static uint8_t g_release_step;   /* ds:0x49 */
static uint8_t g_released;       /* ds:0x43 */
static uint8_t g_to_release;     /* ds:0x47 */

/* THE PORT'S OWN, only because ds:0x43 is a byte of the data segment and
 * anything in the original can read it. */
uint8_t level_released(void) { return g_released; }

/* Image 0x39D1 sets the countdown and image 0x39D6 the interval; both run
 * when the level is set up. */
void level_release_reset(const level *lv)
{
    g_release_wait = 0x14;                      /* image 0x39D1 */
    g_release_step = (uint8_t)(((99u - (uint16_t)lv->release_rate) >> 1) + 4u);
    g_released     = 0;
    g_to_release   = (uint8_t)lv->num_lemmings;
}

/* Image 0x30A6. What it releases is a lemming into the array at ds:0x88;
 * this keeps only the count, because nothing walks that array yet. */
void level_release_step(void)
{
    if (level_entrance_open() != 0xFF) return;  /* image 0x30B0 */
    if (--g_release_wait) return;               /* image 0x30BA */
    g_release_wait = g_release_step;            /* image 0x30C3 */
    if (g_released == g_to_release) return;     /* image 0x30CC */
    g_released++;
}

/* ======================================================================
 * THE LEVEL'S OPENING SEQUENCE - image 0x0361, called from the play loop's
 * first line at image 0x019D.
 *
 *     al = [0x4e]            the phase flags
 *     test al,4 / je         bit 2 clear: the opening is over
 *     dl = [0x52]            the play loop's own frame counter, bumped at
 *                            image 0x01A3 once per pass
 *     cmp dl,0x0f / je       frame 15: a sound, and nothing else
 *     cmp dl,0x23 / je 0x394 frame 35: OPEN THE ENTRANCE - image 0x394 sets
 *                            ds:0x55 to 0xFF and plays the trapdoor
 *     cmp dl,0x37 / je       frame 55: image 0x3AD, the music
 *     jb / and al,0xfb       past 55: clear bit 2, the opening is done
 *     [0x4e] = al
 *
 * ds:0x55 is what image 0x07C1E tests before animating object id 1, and what
 * image 0x30B0 tests before releasing a lemming - so this one byte holds back
 * both the trapdoor and the lemmings. Nothing in the image appears to CALL
 * 0x394: it is reached by falling through this chain, and the return address
 * on the stack when it runs is 0x1A0, which is the play loop's next line.
 *
 * The frame number was confirmed against the running game before it was
 * transcribed: watching writes to ds:0x55 shows 0xFF arriving from image
 * 0x394 at play-loop frame 35, which is 0x23.
 * ====================================================================== */
static uint8_t g_level_frame;      /* ds:0x52 */
static uint8_t g_level_phase;      /* ds:0x4E */
static uint8_t g_entrance_open;    /* ds:0x55 */

/* Image 0x0436..0x0447 clears these when a level starts; bit 2 of ds:0x4E is
 * what makes the opening sequence run at all. */
void level_start_reset(void)
{
    g_level_frame  = 0;
    g_level_phase  = 4;
    g_entrance_open = 0;
}

/* THE PORT'S OWN, and only because these two bytes are `ds:0x55` and
 * `ds:0x52` in the original and anything can read a byte of the data segment.
 * They exist so a test can assert the trapdoor is shut without a screenshot. */
uint8_t level_entrance_open(void) { return g_entrance_open; }
uint8_t level_frame_counter(void) { return g_level_frame; }

/* Transcribed from image 0x0361, with the counter bumped where the play loop
 * bumps it - image 0x01A3, AFTER this call, not before. */
void level_start_step(void)
{
    uint8_t al = g_level_phase;
    if (al & 4) {
        uint8_t dl = g_level_frame;
        if (dl == 0x23) {
            g_entrance_open = 0xFF;         /* image 0x394 */
        } else if (dl != 0x0F && dl != 0x37 && dl > 0x37) {
            g_level_phase = (uint8_t)(al & 0xFB);
        }
        /* Frames 15 and 55 are a sound and the music. The port makes no
         * sound, so they are deliberate no-ops rather than gaps - see the
         * note on non-goals in CLAUDE.md. */
    }
    g_level_frame++;                        /* image 0x01A3 */
}

#define ST_IN_HUNDREDS 29
#define ST_IN_TENS     30
#define ST_IN_UNITS    31

/* The IN percentage, image 0x03AF8. Three cells at cols 29..31, and the
 * trick in it is worth keeping: with no tens digit it loads ah with 0xF0 and
 * then adds '0', which wraps a byte to 0x20 - a space. So 0% shows as " 0"
 * and not "00". A hundred per cent is special-cased before any of that and
 * writes "100" across all three.
 *
 * What it is a percentage OF is not established here: nothing counts the
 * saved lemmings yet, so this is called with zero, which is what the whole of
 * the first seven hundred frames of "Just dig!" shows anyway. */
void level_status_in(uint8_t percent)
{
    uint8_t al = percent, ah;
    if (al == 100) {                       /* image 0x03AFB */
        g_status[ST_IN_HUNDREDS] = '1';
        g_status[ST_IN_TENS]     = '0';
        g_status[ST_IN_UNITS]    = '0';
        return;
    }
    ah = 0;
    do { ah++; } while (al >= 10 ? (al = (uint8_t)(al - 10), 1) : 0);
    ah--;
    /* image 0x03B17: no tens, so 0xF0 + '0' wraps to a space */
    g_status[ST_IN_TENS]  = ah ? (char)(ah + '0') : ' ';
    g_status[ST_IN_UNITS] = (char)(al + '0');
}

/* ======================================================================
 * THE LEMMINGS - image 0x1F19, and the handler at image 0x2047 it reaches
 * when [si+8] is zero.
 *
 * That word is a state bitmask and image 0x1F5E dispatches on it to sixteen
 * handlers. With no player input it is zero on every lemming on every frame,
 * so image 0x2047 - walking, and image 0x21B0 - falling, which it hands over
 * to, are the whole of what a level does on its own. The other fourteen are
 * the skills and are not read yet.
 *
 * Both are checked against the original's own array rather than against a
 * screenshot: tools/probe_lemmings.py records it frame by frame and
 * `--check` replays these rules over every step. 1,127 walker steps and 54
 * faller steps, none wrong.
 * ====================================================================== */
#define LEM_X_MIN      0x10     /* image 0x2057 */
#define LEM_X_MAX      0x680    /* image 0x205F */
#define LEM_FALL_SPLAT 0x3C     /* image 0x2250 */
#define LEM_SPAWN_DX   24       /* the entrance table at ds:0x0E98 holds */
#define LEM_SPAWN_DY   14       /* 744,66 where the object is at 704,36 */
/* The dot's colour, measured against a recorded frame the way the
 * minimap's own index 7 was. */
#define MINIMAP_DOT    2

/* Image 0x5DC6: the row is `y - 0x10` and the byte `x >> 3`, and the plane it
 * reads is the one terrain lives in - which is why GROUNDxO.DAT's fourth
 * bitplane doubles as its mask, so the test is bit 3. */
static int lem_solid(const uint8_t *lvl, uint16_t cw, uint16_t chh,
                     int16_t x, int16_t y)
{
    int32_t cx = (int32_t)x - LEMMING_DX;
    int32_t cy = (int32_t)y - LEMMING_DY;
    if (cx < 0 || cx >= (int32_t)cw || cy < 0 || cy >= (int32_t)chh)
        return 0;
    return (lvl[(size_t)cy * cw + cx] & 8) != 0;
}

/* Image 0x2148, and image 0x214E which turns it round at the ceiling. */
static void lem_settle(lemming *a, int16_t bp)
{
    a->y = bp;
    if (bp + a->hot_y <= 10) {
        a->y = (int16_t)(9 - a->hot_y);
        a->dir = (int8_t)-a->dir;
    }
}

/* Image 0x2047. */
/* THE DIGGER - assignment from image 0x0320D, behaviour from image 0x023F3.
 *
 * The first skill this port implements, and the first of the fourteen
 * handlers behind the dispatch at image 0x01F5E that anything has ever
 * reached. tools/probe_digger.py reaches it: it drives the original into
 * assigning one and records the array either side, so this is checked against
 * the original doing it rather than against a reading of the disassembly
 * alone.
 *
 * What the original does, recorded:
 *
 *     f102  flags=0x0000  anim= 0  diggers=10
 *     f103  flags=0x0010  anim=17  diggers= 9     assigned
 *     f104  flags=0x0010  anim= 0  y 92->93       digs
 *     f112  flags=0x0010  anim= 8  y 93->94       digs
 *
 * and x freezes the moment it is assigned.
 */

/* WHICH LEMMING IS UNDER THE POINTER - transcribed from image 0x03D0A.
 *
 * The pointer's position in LEVEL coordinates is `scroll - 16 + mouse_x`
 * across and `mouse_y + 8` down (image 0x03D0A and image 0x03D14), and image
 * 0x03D27 gives each lemming a 12x12 box at (x + hot_x - 4, y + hot_y - 1).
 * The scan walks the array and takes a hit.
 *
 * Both halves of that were got wrong on the way to reading it: the y relation
 * is level y MINUS eight, not plus, and a probe that had it the other way
 * round clicked sixteen pixels below every lemming and reported that the game
 * simply did not respond. See tools/probe_digger.py.
 */
#define HIT_BOX      12          /* image 0x03D4C and image 0x03D5B */
#define HIT_BACK_X   4           /* image 0x03D2C */
#define HIT_BACK_Y   1           /* image 0x03D39 */

int16_t level_lemming_at(const lemming *lem, uint8_t n, uint16_t view_x)
{
    int32_t px = (int32_t)view_x - 16 + (int32_t)lem_mouse_x;
    int32_t py = (int32_t)lem_mouse_y + 8;

    for (uint8_t k = 0; k < n && k < LEMMING_MAX; k++) {
        const lemming *a = &lem[k];
        if (a->alive == 0xFF) continue;          /* image 0x03D1E */
        int32_t bx = (int32_t)a->x + a->hot_x - HIT_BACK_X;
        int32_t by = (int32_t)a->y + a->hot_y - HIT_BACK_Y;
        if (bx < 0) bx = 0;
        if (by < 0) by = 0;
        if (px >= bx && px <= bx + HIT_BOX && py >= by && py <= by + HIT_BOX)
            return (int16_t)k;
    }
    return -1;
}

#define DIG_BIT        0x0010
#define DIG_KEEP       0xF57F
#define DIG_ANIM_START 0x11
#define DIG_HOT_X      (-8)
#define DIG_HOT_Y      (-12)
/* Image 0x02424: past this row it has dug its way out of the level. */
#define DIG_FLOOR      0xB4
/* Image 0x0242D: 0x0FF8 is nine bits, and image 0x02432 shifts it by the
 * low three of the x it probes - which is x + hot_x. So the span cleared
 * runs from that + 3, nine pixels wide: x-5 to x+3. */
#define DIG_WIDTH      9
#define DIG_FROM       (-5)

/* Transcribed from image 0x0320D. The mask it clears is 0xF57F - every state
 * a digger may not also be in - and the bit it sets is 0x10, whose handler is
 * image 0x023F3. It spends one from the count at ds:0x7F, parks the animation
 * at 0x11 and drops the hot spot two rows. */
int lem_dig_start(lemming *a)
{
    if (a->flags & ~(uint16_t)DIG_KEEP) return 0;   /* image 0x0321D */
    a->flags = (uint16_t)((a->flags & DIG_KEEP) | DIG_BIT);
    a->anim  = DIG_ANIM_START;
    a->hot_x = DIG_HOT_X;
    a->hot_y = DIG_HOT_Y;
    a->state = 0;
    return 1;
}

/* Image 0x023F3. Sixteen animation frames, and it digs on the two where the
 * counter comes round to 0 and to 8 - image 0x02406 and image 0x02408. The
 * frame it is parked on when assigned, 0x11, is not one of the sixteen and
 * image 0x023F7 sends it straight to the digging path, which is why the very
 * next frame is both anim 0 AND a dig. */
static void lem_dig(lemming *a, uint8_t *lvl, uint16_t cw, uint16_t chh)
{
    if (a->anim == DIG_ANIM_START) {
        a->anim = 0;                          /* image 0x0250C */
    } else {
        a->anim = (uint8_t)((a->anim + 1) & 0x0F);   /* image 0x023FF */
        if (a->anim != 0 && a->anim != 8)
            return;                           /* image 0x0240C: just animate */
    }

    a->y = (int16_t)(a->y + 1);               /* image 0x0241D */
    if (a->y >= DIG_FLOOR) {                  /* image 0x02424 */
        a->alive = 0xFF;
        return;
    }

    /* Clear the span, and see whether anything solid is left under it. */
    int solid = 0;
    int32_t cy = (int32_t)a->y - LEMMING_DY;
    for (uint8_t i = 0; i < DIG_WIDTH; i++) {
        int32_t cx = (int32_t)a->x + DIG_FROM + i - LEMMING_DX;
        if (cx < 0 || cx >= (int32_t)cw || cy < 0 || cy >= (int32_t)chh)
            continue;
        lvl[(size_t)cy * cw + cx] = 0;        /* image 0x05EBA */
        if (lem_solid(lvl, cw, chh, (int16_t)(a->x + DIG_FROM + i),
                      (int16_t)(a->y + 1)))
            solid = 1;
    }
    if (!solid) {
        /* image 0x0244E: nothing left to stand on, so fall. */
        a->flags = (uint16_t)((a->flags & ~(uint16_t)DIG_BIT) | 4);
        a->state = 4;
    }
}

/* Image 0x2047. */
static void lem_walk(lemming *a, const uint8_t *lvl, uint16_t cw, uint16_t chh)
{
    a->anim = (uint8_t)((a->anim + 1) & 7);
    a->x = (int16_t)(a->x + a->dir);
    if (a->x < LEM_X_MIN || a->x >= LEM_X_MAX) {   /* image 0x2142 */
        a->dir = (int8_t)-a->dir;
        return;
    }
    if (lem_solid(lvl, cw, chh, a->x, a->y)) {
        for (int8_t k = 1; k < 4; k++)             /* image 0x20E9 */
            if (!lem_solid(lvl, cw, chh, a->x, (int16_t)(a->y - k))) {
                lem_settle(a, (int16_t)(a->y - (k - 1)));
                return;
            }
        for (int8_t k = 4; k < 8; k++) {           /* image 0x2169 */
            if (!lem_solid(lvl, cw, chh, a->x, (int16_t)(a->y - k))) {
                a->flags |= 8;
                a->anim = 8;
                lem_settle(a, (int16_t)(a->y - 2));
                return;
            }
        }
        a->dir = (int8_t)-a->dir;                  /* image 0x2142 */
        return;
    }
    for (int8_t k = 1; k < 4; k++)                 /* image 0x20D6 */
        if (lem_solid(lvl, cw, chh, a->x, (int16_t)(a->y + k))) {
            a->y = (int16_t)(a->y + k);
            if (a->y >= LEMMING_DEAD_Y) a->alive = 0xFF;
            return;
        }
    /* image 0x209A: nothing under it, so it falls */
    a->y = (int16_t)(a->y + 4);
    a->flags |= 4;
    a->anim = 0;
    a->fall = 3;
    a->state = 4;
    if (a->y >= LEMMING_DEAD_Y) a->alive = 0xFF;
}

/* Image 0x23BF - what image 0x2169 turns a walker into when the step up is
 * four rows or more. It climbs one row a frame while there is still terrain
 * above, and the frame it finds air it clears bit 3 and hands back to the
 * walker.
 *
 *     dec [si+2] / probe the new row / empty -> stop, anim = 0, y + 1
 *     dec [si+2] / probe again       / empty -> stop, anim = 0, y + 1
 *     otherwise settle where it is and keep the bit set
 */
static void lem_step_up(lemming *a, const uint8_t *lvl,
                        uint16_t cw, uint16_t chh)
{
    for (uint8_t k = 0; k < 2; k++) {
        a->y = (int16_t)(a->y - 1);
        if (!lem_solid(lvl, cw, chh, a->x, a->y)) {
            a->flags = (uint16_t)(a->flags & ~8u);   /* image 0x23E4 */
            a->anim  = 0;
            lem_settle(a, (int16_t)(a->y + 1));
            return;
        }
    }
    lem_settle(a, a->y);                             /* image 0x23DE */
}

/* Image 0x21B0 and image 0x2202. */
static void lem_fall(lemming *a, const uint8_t *lvl, uint16_t cw, uint16_t chh)
{
    a->anim = (uint8_t)((a->anim + 1) & 3);
    for (int8_t k = 0; k < 3; k++) {
        if (lem_solid(lvl, cw, chh, a->x, (int16_t)(a->y + k))) {
            a->y = (int16_t)(a->y + k);            /* image 0x224D */
            if (a->fall <= LEM_FALL_SPLAT) {       /* image 0x22A3 */
                a->flags = 0;
                a->anim  = 0;
                a->state = 9;
            } else {
                a->flags = 1;                      /* image 0x2256: splat */
                a->anim  = 0;
                a->dir   = 0;
                a->state = 0;
            }
            return;
        }
        if (a->y + k + 1 >= LEMMING_DEAD_Y) {      /* image 0x2210 */
            a->y = (int16_t)(a->y + k + 1);
            a->alive = 0xFF;
            return;
        }
    }
    a->fall = (uint8_t)(a->fall + 3);              /* image 0x223A */
    a->y = (int16_t)(a->y + 3);
    if (a->y >= LEMMING_DEAD_Y) a->alive = 0xFF;
}

/* THE PORT'S OWN. The original clears the whole record when it releases one
 * - image 0x030F1 - and never needs an array-wide reset, because the count at
 * ds:0x43 is what says which entries mean anything. */
void lemmings_reset(lemming *lem)
{
    for (uint8_t k = 0; k < LEMMING_MAX; k++) {
        lem[k].alive = 0xFF;
        lem[k].state = 0;
    }
}

/* Image 0x030F1: the record is cleared, then filled from the entrance table
 * at ds:0x0E98 - which for "Just dig!" holds 744, 66 against an entrance
 * object at 704, 36. */
void lemmings_release(lemming *lem, uint8_t n, const level *lv)
{
    lemming *a;
    if (n >= LEMMING_MAX) return;
    a = &lem[n];
    a->x = a->y = 0;
    for (uint16_t i = 0; i < lv->num_objects; i++)
        if ((lv->objects[i].id & 0xFF) == 1) {
            a->x = (int16_t)(lv->objects[i].x + LEMMING_DX + LEM_SPAWN_DX);
            a->y = (int16_t)(lv->objects[i].y + LEMMING_DY + LEM_SPAWN_DY);
            break;
        }
    a->hot_x = -8;                                 /* image 0x312F */
    a->hot_y = -10;
    a->flags = 4;                                  /* image 0x310F */
    a->fall  = 3;                                  /* image 0x312B */
    a->alive = 0;
    a->dir   = 1;                                  /* image 0x3103 */
    a->anim  = 0;
    a->state = 4;                                  /* image 0x3139 */
}

/* One pass of image 0x1F19's walk over the array: image 0x1F3C skips the dead
 * and image 0x1F5E dispatches on [si+8]. Only the zero case is read, so only
 * that is stepped. */
void lemmings_step(lemming *lem, uint8_t n,
                   uint8_t *lvl, uint16_t cw, uint16_t chh)
{
    for (uint8_t k = 0; k < n && k < LEMMING_MAX; k++) {
        lemming *a = &lem[k];
        if (a->alive == 0xFF) continue;            /* image 0x1F3C */

        /* THE STATE DISPATCH, image 0x01F5E - and it is a PRIORITY CHAIN,
         * not a jump table. It tests the bits of [si+8] one at a time, in an
         * order that is not the order of the bits, and takes the first that
         * is set. This file used to call it "16 handlers via 0x1F5E", which
         * had 0x1F5E down as a table; it is the address the chain starts at.
         *
         * The whole of it, in the order the original tests:
         *
         *     (none)  0x2047  walking          transcribed
         *     0x0002  0x2350
         *     0x8000  0x2D0F
         *     0x0004  0x21B0  falling          transcribed
         *     0x0400  0x2A6D
         *     0x0001  0x22DF
         *     0x0008  0x23BF  stepping up      transcribed
         *     0x0010  0x23F3  digging          transcribed
         *     0x0020  0x254F
         *     0x0040  0x2637
         *     0x0080  0x2693
         *     0x0100  0x27E8
         *     0x0200  0x287D
         *     0x0800  0x2B35
         *     0x1000  0x22F5
         *     0x2000  0x2326
         *     0x4000  0x2340
         *     else    0x2EC0  (called, not jumped to)
         *
         * Three are transcribed. The rest are the skills and the ways a
         * lemming can die, and NOTHING REACHES THEM without a player
         * assigning something - which is why no recorded run has ever
         * exercised one, and why they are left alone here rather than
         * stepped wrongly.
         *
         * The order is kept even though only two of the bits can currently
         * be set, because it stops being harmless the moment a third can. */
        uint16_t f = a->flags;
        if (f == 0) {
            if (a->state == 9)
                lem_walk(a, lvl, cw, chh);           /* image 0x2047 */
        }
        else if (f & 0x0002) { /* image 0x2350, not read */ }
        else if (f & 0x8000) { /* image 0x2D0F, not read */ }
        else if (f & 0x0004)
            lem_fall(a, lvl, cw, chh);               /* image 0x21B0 */
        else if (f & 0x0010)
            lem_dig(a, lvl, cw, chh);                /* image 0x23F3 */
        else if (f & 0x0400) { /* image 0x2A6D, not read */ }
        else if (f & 0x0001) { /* image 0x22DF, not read */ }
        else if (f & 0x0008)
            lem_step_up(a, lvl, cw, chh);            /* image 0x23BF */
        /* 0x0020 0x254F, 0x0040 0x2637, 0x0080 0x2693,
         * 0x0100 0x27E8, 0x0200 0x287D, 0x0800 0x2B35, 0x1000 0x22F5,
         * 0x2000 0x2326, 0x4000 0x2340 - none read. */
    }
}

/* Image 0x03647: the count image 0x03634 accumulates as it walks, and what
 * image 0x3684 puts in the OUT field. */
uint8_t lemmings_live(const lemming *lem, uint8_t n)
{
    uint8_t live = 0;
    for (uint8_t k = 0; k < n && k < LEMMING_MAX; k++)
        if (lem[k].alive != 0xFF) live++;          /* image 0x03647 */
    return live;
}

/* ======================================================================
 * DRAWING A LEMMING - image 0x0706F, and the store image 0x59BB fills.
 *
 * The original never draws from the file. Image 0x59BB walks every creature
 * through the pre-shift builder at image 0x05B11 - the lemming first, `cx =
 * 0x12` frames of `dx = 0x020A`, two planes of ten rows, source stepping 0x28
 * a frame - and image 0x0706F then indexes the result at
 *
 *     (anim * 8 + shift) * frame_size,  frame_size = 90
 *
 * with the mask 60 bytes in. That was checked against the game's own
 * registers rather than reasoned about: at image 0x070E7 the walker's mask
 * offsets come out 60, 870, 1680, 2490, 3300, 4110, 4920, 5730 for its eight
 * animation frames - steps of exactly 810, which is 9 * 90, because `anim`
 * and `x & 7` both advance by one every frame.
 *
 * The port has no reason to pre-shift: it can put a pixel anywhere. So it
 * reads the SOURCE the builder reads, which is `main.dat` section 0 from
 * offset 0 - found by taking the store's own bytes back to the file - laid
 * out as 40 bytes a frame, plane 0's ten rows of two bytes then plane 1's.
 *
 * Two planes give values 1..3, and 0 is transparent: the builder's second
 * pass ORs the planes together to make the mask, so a pixel is drawn exactly
 * when either plane has it.
 *
 * Which frame: `anim` facing right, and image 0x070AB adds `[di+0x29]` - the
 * state, 9 for a walker - facing left. Eight frames each way inside the
 * eighteen the loader builds.
 * ====================================================================== */
#define LEM_SPR_SECTION 0
#define LEM_SPR_W       16      /* image 0x05B1C reads two bytes a row */
#define LEM_SPR_H       10      /* image 0x059CC: dl = 0x0A */
#define LEM_SPR_PLANES  2       /* image 0x059CC: dh = 0x02 */
#define LEM_SPR_ROW     2
#define LEM_SPR_PLANE   (LEM_SPR_ROW * LEM_SPR_H)          /* 20 */
#define LEM_SPR_FRAME   (LEM_SPR_PLANE * LEM_SPR_PLANES)   /* 40 */
#define LEM_SPR_FRAMES  0x12    /* image 0x059C8: cx = 0x12 - the WALKER's */
/* The faller is a different group. Image 0x030F1 gives a released lemming
 * `[si+0x0c] = 0xA42`, which is the paragraph its group starts at in the
 * pre-shift store - the walker's is 0 - and image 0x05A48 builds a group of
 * eight there, `dx = 0x020A` through the builder that adds a mask, the same
 * two planes of ten rows.
 *
 * Reading those frames back out of the store at 0xA42 * 16 and looking for
 * them in the file puts them at source frames 358..361, one match each and no
 * others - and they are unmistakably falling lemmings, arms out. Eight frames
 * is four each way, which is what `anim & 3` and image 0x070AB's `+ state`
 * need. */
#define LEM_SPR_FALL    358

/* One lemming, at its own coordinates - image 0x070F9 puts the top row at
 * `y + hot_y`, and the canvas sits 16 along and 16 down from those. */
void lemming_draw(uint8_t *dst, uint16_t dst_w, uint16_t dst_h,
                  const uint8_t *sec, uint32_t sec_len, const lemming *a)
{
    uint16_t frame = a->anim;
    uint32_t base;
    int32_t x0, y0;

    if (a->alive == 0xFF) return;
    if (a->dir != 1) frame = (uint16_t)(frame + a->state);  /* image 0x070AB */
    /* Which group, from the state - the store base the original carries in
     * [si+0x0c] is a group, and the port indexes the same groups in the file. */
    if (a->state == 4) frame = (uint16_t)(LEM_SPR_FALL + frame);
    else if (frame >= LEM_SPR_FRAMES) return;

    base = (uint32_t)frame * LEM_SPR_FRAME;
    if (base + LEM_SPR_FRAME > sec_len) return;

    x0 = (int32_t)a->x + a->hot_x - LEMMING_DX;
    y0 = (int32_t)a->y + a->hot_y - LEMMING_DY;

    for (uint8_t r = 0; r < LEM_SPR_H; r++) {
        int32_t py = y0 + r;
        if (py < 0 || py >= (int32_t)dst_h) continue;
        for (uint8_t c = 0; c < LEM_SPR_W; c++) {
            int32_t px = x0 + c;
            uint8_t v = 0;
            if (px < 0 || px >= (int32_t)dst_w) continue;
            for (uint8_t p = 0; p < LEM_SPR_PLANES; p++) {
                uint8_t byte = sec[base + p * LEM_SPR_PLANE
                                   + r * LEM_SPR_ROW + (c >> 3)];
                v = (uint8_t)(v | (((byte >> (7 - (c & 7))) & 1u) << p));
            }
            if (v)                       /* the mask is the planes OR'd */
                dst[(size_t)py * dst_w + px] = v;
        }
    }
}

/* Image 0x06FE2: the row is `(y - 0x20) >> 3` and the pixel `x >> 4`, and a
 * y of 0xB0 or more is off the map. */
void panel_minimap_dots(uint8_t *dst, uint16_t dst_w, uint16_t dst_h,
                        const lemming *lem, uint8_t n,
                        uint16_t x0, uint16_t y0)
{
    for (uint8_t k = 0; k < n && k < LEMMING_MAX; k++) {
        const lemming *a = &lem[k];
        int16_t ay;
        if (a->alive == 0xFF) continue;
        if (a->y >= 0xB0) continue;                /* image 0x06FE5 */
        ay = (int16_t)(a->y - 0x20);
        if (ay < 0) continue;                      /* image 0x06FF0 */
        {
            uint16_t r = (uint16_t)(y0 + (ay >> 3));
            uint16_t p = (uint16_t)(x0 + (a->x >> 4));
            if (r < dst_h && p < dst_w)
                dst[(size_t)r * dst_w + p] = MINIMAP_DOT;
        }
    }
}

#define MINIMAP_W      104      /* 13 bytes, image 0x0794E: cx = 0x0D */
#define MINIMAP_H      20       /* image 0x079F1: cx = 0x14 */
#define MINIMAP_XSTEP  16       /* image 0x07A2F: si += 2, two bytes */
#define MINIMAP_YSTEP  8        /* image 0x079FB: si += 0x680, eight rows */
/* Image 0x07992: si += 0xD00 before anything else, which at the canvas's
 * 208-byte stride is SIXTEEN rows. Dropping it puts the whole map two rows
 * out and - the tell - stops the lemming dots landing on the ground they are
 * standing on, because image 0x06FE2's row is (y - 0x20) >> 3 and the two
 * only agree when this is here. */
#define MINIMAP_YFROM  16
#define MINIMAP_VOTE   8        /* image 0x07A4E: cmp dh, 8 / jg */
#define MINIMAP_INK    7
#define MINIMAP_X      241      /* screen 209, in the panel's 352-wide line */
#define MINIMAP_Y      17       /* screen 177, rows into the panel */

/* ======================================================================
 * THE MINIMAP - images 0x0796F, 0x07A25 and 0x07A39 to build it, 0x07900 to
 * put it on the screen.
 *
 * The game keeps it as a 13-byte by 20-row bitmap stashed off-screen at
 * 0xA000:0xD320 and rebuilds ONE byte-column of it a frame, cycling ds:0x56
 * by 16 and wrapping at 0xD0 - thirteen frames to refresh the lot. The port
 * has no reason to spread the work out, so it builds all thirteen at once;
 * the result is the same bitmap.
 *
 * How a pixel is decided is the part that had to be read rather than guessed.
 * Image 0x079C9 puts the graphics controller in READ MODE 1 with Color
 * Compare and Color Don't Care both 0x08, so a read returns a byte of "is
 * plane 3 set" - and plane 3 is where terrain lives, which is why
 * GROUNDxO.DAT's fourth bitplane doubles as its mask. Then image 0x07A39
 * reads TWO of those bytes, counts the sixteen bits, and:
 *
 *     cmp dh, 8 / jg  ->  or byte cs:[bx], dl
 *
 * sets the minimap pixel only when MORE THAN EIGHT of the sixteen are
 * terrain. A majority, not "any" - which is why the top of a level with a few
 * stalactites in it shows as empty on the map.
 *
 * Vertically there is no reduction at all: image 0x079FB steps the source by
 * 0x680, which is eight rows of the 208-byte canvas, so it samples every
 * eighth row and ignores the seven between.
 *
 * The colour is index 7, and that is what the EGA state says it must be:
 * Set/Reset 0x06 with Enable Set/Reset 0x08 (images 0x079B9 and 0x079C1)
 * leaves plane 3 clear and gives planes 0, 1 and 2 the CPU's bits, so a set
 * bit is 0b0111. A recorded frame agrees - 308 of the 389 pixels the rule
 * lights are index 7 there, the rest being the viewport box and the lemming
 * dots drawn over it.
 *
 * Where it sits on the screen is measured, like PANEL_TOP: x 209, y 177, the
 * sharp minimum of a search over 7 x 7 origins - 137 differing pixels at that
 * one against 216 and 217 either side of it.
 *
 * ONE LOOSE END, written down rather than papered over. The copy is
 * byte-aligned, so the minimap must start on a byte boundary of the 352-pixel
 * line; x 209 on screen is x 241 in that line, and 241 is not a multiple of
 * eight. Something in the mapping from a page offset to a screen column is
 * still off by a pixel, and the proof of it is that clearing the full
 * 104-pixel span - which the original's copy does - rubs out part of the
 * artwork's frame. Until that is read, only the ink is drawn.
 * ====================================================================== */
#define VIEWPORT_W   24      /* image 0x07AAC: cx:bx = 0xFFFF:0xFF00 */
#define VIEWPORT_H   20
#define VIEWPORT_INK  3      /* image 0x07A91: Enable Set/Reset 0x0C */
#define VIEWPORT_Y   18      /* screen row 178 */

/* THE MINIMAP'S VIEWPORT BOX - image 0x07A5A.
 *
 *     di = [0] >> 7                   the scroll, in minimap BYTES
 *     di += 0x2172 + [0x1f76] + 2     the minimap's own base
 *     Enable Set/Reset 0x0C           so a written bit is colour 3
 *     cx = 0xFFFF / bx = 0xFF00       a 24-bit run...
 *     ax = ([0] >> 4) & 7             ...shifted right by the sub-byte part
 *     shr cx,1 / rcr bx,1             of the scroll, `ax` times
 *
 * So it is 24 minimap pixels wide - not the 20 that 320 screen pixels come to
 * at this scale - starting at `scroll >> 4`. The 24 is the mask's own width
 * and there is no arithmetic behind it to get wrong.
 *
 * Checked against recorded frames: with the scroll at 624, 624 >> 4 is 39, so
 * the box starts at minimap pixel 39, which is screen x 248 - and 248..271 is
 * exactly where two separate frames put it, with solid top and bottom rows and
 * a single pixel down each side.
 */
void panel_viewport(uint8_t *dst, uint16_t dst_w, uint16_t dst_h,
                    uint16_t scroll, uint16_t x0, uint16_t y0)
{
    uint16_t bx0 = (uint16_t)(x0 + (scroll >> 4));
    for (uint16_t r = 0; r < VIEWPORT_H; r++) {
        uint16_t y = (uint16_t)(y0 + r);
        if (y >= dst_h) break;
        if (r == 0 || r == VIEWPORT_H - 1) {
            for (uint16_t c = 0; c < VIEWPORT_W; c++)
                if (bx0 + c < dst_w)
                    dst[(size_t)y * dst_w + bx0 + c] = VIEWPORT_INK;
        } else {
            if (bx0 < dst_w)
                dst[(size_t)y * dst_w + bx0] = VIEWPORT_INK;
            if (bx0 + VIEWPORT_W - 1 < dst_w)
                dst[(size_t)y * dst_w + bx0 + VIEWPORT_W - 1] = VIEWPORT_INK;
        }
    }
}

/* Transcribed from images 0x0796F, 0x07A25 and 0x07A39 - see the note above.
 * Built and drawn in one pass: the original's rolling column, ds:0x56 at
 * image 0x0797D, is a way of spreading the cost over thirteen frames and not
 * part of the picture. */
void panel_minimap(uint8_t *dst, uint16_t dst_w, uint16_t dst_h,
                   const uint8_t *lvl, uint16_t cw, uint16_t chh,
                   uint16_t x0, uint16_t y0)
{
    for (uint16_t r = 0; r < MINIMAP_H; r++) {
        uint16_t y = (uint16_t)(MINIMAP_YFROM + r * MINIMAP_YSTEP);
        uint16_t py = (uint16_t)(y0 + r);
        if (py >= dst_h) break;
        for (uint16_t p = 0; p < MINIMAP_W; p++) {
            uint16_t px = (uint16_t)(x0 + p);
            if (px >= dst_w) break;
            uint8_t n = 0;
            if (y < chh) {
                uint16_t sx = (uint16_t)(p * MINIMAP_XSTEP);
                for (uint8_t k = 0; k < MINIMAP_XSTEP; k++)
                    if (sx + k < cw && lvl[(size_t)y * cw + sx + k])
                        n++;
            }
            /* Ink only. The original's copy writes all thirteen bytes a
             * row, background included, but doing that here rubs out part of
             * the artwork's own frame - which says the 104-pixel span is not
             * quite in the right place yet, and the note above records it. So
             * this leaves what is underneath: measured, that is 137 differing
             * pixels against 232 for clearing. */
            if (n > MINIMAP_VOTE)
                dst[(size_t)py * dst_w + px] = MINIMAP_INK;
        }
    }
}

/* Image 0x3684: the walk over the lemming array ends by putting the LIVE
 * count - ds:0x45, the entries whose +0x26 is not 0xFF - into the line at
 * ds:0x2095, which is col 22.
 *
 * Nothing walks that array here yet, so what gets passed in is the RELEASED
 * count instead. The two agree only for as long as no lemming is lost, which
 * over the first 700 frames of "Just dig!" they do - it is not the same
 * quantity and this says so rather than letting a number that happens to
 * match stand as if it were. */
void level_status_out(uint8_t live)
{
    status_put_num(&g_status[22], live);
}

/* Image 0x07C0A, line for line.
 *
 *     bx = [si]              the flags word
 *     test bx,2 / je         bit 1 clear: this one does not animate
 *     al = [si+2]  ah = [si+3]
 *     inc al / cmp al,ah / jne
 *     xor al,al              wrapped
 *     test bx,1 / je / and bx,0xfffd    bit 0 set: one-shot, stop animating
 *     [si+2] = al
 *     mul word [si+6]        frame * frame_data_size
 *     add ax, [si+0x15]      + the base
 *     [si+0x17] = ax         the ink pointer the draw loop reads
 *
 * The `cmp al,ah` is against the END frame, not one past it, so the last
 * frame is never shown - that is what the original does, not a fencepost. */
void level_anim_step(level_anim *st, const level *lv)
{
    for (uint16_t i = 0; i < LEVEL_ANIM_MAX; i++) {
        level_anim *a = &st[i];
        if (!a->live) continue;
        /* Image 0x07C1E: `dl = [di+5]` is the low byte of the object's
         * big-endian id, and id 1 - the entrance - only animates once
         * ds:0x55 reads 0xFF. That byte is set at frame 35 by the opening
         * sequence above, which is why the trapdoor stays shut at the start
         * of a level. */
        if (i < lv->num_objects && (lv->objects[i].id & 0xFF) == 1
                && g_entrance_open != 0xFF)
            continue;
        if (!(a->flags & 2)) continue;
        uint8_t f = (uint8_t)(a->frame + 1);
        if (f == a->end_frame) {
            f = 0;
            if (a->flags & 1) a->flags = (uint16_t)(a->flags & ~2u);
        }
        a->frame = f;
        a->ink = (uint16_t)(a->base + (uint16_t)f * a->frame_size);
    }
}

/* One pass of the level's frame - as much of the chain at image 0x019C..
 * 0x01C5 as has been read, in its order:
 *
 *     image 0x019D  call 0x031E   the opening sequence
 *     image 0x01B0  call 0x30A6   release a lemming
 *     image 0x01B6  call 0x3944   which tail-jumps to 0x07C0A, the objects
 *     image 0x01BC  call 0x3634   walk the lemmings - NOT READ, so the OUT
 *                                 count is fed the released count instead
 *     image 0x0213                the clock, later in the same pass
 *
 * The calls this does not make are the ones nobody has read: 0x4085, 0x80FD,
 * 0x7900, 0x3150, 0x1F19, 0x3634, 0x3AC8 (the minimap) and 0x3CDE (what the
 * cursor is over). */
void level_frame_step(level_anim *st, const level *lv)
{
    level_start_step();
    level_release_step();
    level_anim_step(st, lv);
    level_timer_step();
    level_status_out(level_released());
    level_status_in(0);          /* nothing is counted saved yet */
}

/* WHAT THE POINTER IS OVER - transcribed from image 0x03EF9.
 *
 * The original's own hit test, and it is three comparisons:
 *
 *     [0x12] + 8 >= 0xC0   the panel, not the play area. The coordinates are
 *                          the screen's plus eight, so this is screen row 176
 *                          - which is where the panel's artwork starts.
 *     [0x10] - 8 <  0xD0   a button: the index is that >> 4, sixteen pixels
 *                          each (image 0x03F1F). 0 and 1 are the release rate
 *                          (image 0x03F27 sends anything below 2 elsewhere),
 *                          2..9 the skills, and image 0x03F3D picks 10, 11
 *                          and 12 out by name.
 *     otherwise            the minimap.
 *
 * The minimap, image 0x03F92: it takes the screen x, subtracts 0xDA, clamps
 * to 0..0x4F and that is the new scroll in SIXTEENTHS - image 0x03FAA builds
 * the viewport box's left edge as `(scroll >> 4) + 0xD0`, the same units. In
 * this port's pixels that is a scroll of 0..1264, which is exactly the range
 * a 1584-wide level leaves for a 320-wide window; the two agreeing is the
 * check on this reading.
 *
 * The 0xDA is ten past the minimap's left edge, so a click lands about the
 * middle of the 24-wide box rather than at its corner.
 */
#define HIT_PANEL_Y   0xC0     /* image 0x03EFF */
#define HIT_MINIMAP_X 0xD0     /* image 0x03F0D */
#define HIT_MAP_BIAS  0xDA     /* image 0x03FBE */
#define HIT_MAP_MAX   0x4F     /* image 0x03FC5 */

level_hit level_pointer_hit(void)
{
    level_hit h;
    h.what = LEVEL_HIT_PLAY;
    h.button = 0;
    h.scroll = 0;

    if ((int32_t)lem_mouse_y + 8 < HIT_PANEL_Y)
        return h;                       /* image 0x03F04: the play area */

    int32_t x = (int32_t)lem_mouse_x - 8;
    if (x < HIT_MINIMAP_X) {
        h.what = LEVEL_HIT_BUTTON;
        h.button = (uint8_t)(x >> 4);   /* image 0x03F1F */
        return h;
    }

    /* image 0x03FBE..0x03FCA */
    int32_t v = x - HIT_MAP_BIAS;
    if (v < 0) v = 0;
    if (v > HIT_MAP_MAX) v = HIT_MAP_MAX;
    h.what = LEVEL_HIT_MINIMAP;
    h.scroll = (uint16_t)(v * 16);
    return h;
}

/* THE PALETTE FADE - transcribed from image 0x0570C and image 0x056E2.
 *
 * Image 0x055B7 is the whole of it: it takes a source and a destination
 * palette out of the pointer table for whichever adapter ds:0x1F7C names,
 * programs the DAC from the source (image 0x054C4), steps it one notch
 * towards the destination (image 0x056E2), waits a retrace (image 0x01550)
 * and goes round until nothing moved. The EGA path does the same through the
 * attribute controller and waits THREE retraces a step; the VGA path waits
 * one.
 *
 * One component, image 0x0570C, reads as:
 *
 *     cmp al,ah / je ret        already there
 *     jg  down                  overshooting downwards
 *     inc al                    one step up
 *     mov bl,0xff               and something moved
 *     cmp al,ah / je ret        arrived after one
 *     jg  down-again / inc al   a SECOND step, whichever way
 *
 * so it moves by two, or by one if one gets there. tools/probe_dac.py sees
 * exactly that on the briefing: the six-bit components climb by two a frame,
 * which is c3c300, cbcb00, d3d300, dbdb00, e3e300, ebeb00 widened.
 *
 * The port keeps palettes widened to eight bits, so this works on `v >> 2`
 * and widens back - which round-trips exactly, widen6 being the inverse.
 */
static int fade_component(uint8_t *cur8, uint8_t tgt8)
{
    uint8_t a = (uint8_t)(*cur8 >> 2);
    uint8_t b = (uint8_t)(tgt8 >> 2);
    if (a == b) return 0;
    a = (a < b) ? (uint8_t)(a + 1) : (uint8_t)(a - 1);
    if (a != b)
        a = (a > b) ? (uint8_t)(a - 1) : (uint8_t)(a + 1);
    *cur8 = widen6(a);
    return 1;
}

/* One pass, image 0x056E2: three components of every entry, and a non-zero
 * return while anything is still moving - the original's `bl` flag, which
 * image 0x0562F tests to decide whether to go round again. */
int palette_fade_step(lem_rgb *cur, const lem_rgb *tgt, uint8_t n)
{
    int moved = 0;
    for (uint8_t i = 0; i < n; i++) {
        moved |= fade_component(&cur[i].r, tgt[i].r);
        moved |= fade_component(&cur[i].g, tgt[i].g);
        moved |= fade_component(&cur[i].b, tgt[i].b);
    }
    return moved;
}

/* THE MOUSE CURSOR - the game's own dithered cross-hair.
 *
 * The original hides the DOS pointer (INT 33h AX=2 at image 0x00F43) and
 * draws this itself. Its handler at image 0x04255 asks INT 33h function 3
 * and stores the position at ds:0x10 and ds:0x12, clamped to 8..0x148 and
 * 8..0xC8 - so the coordinates are the screen's OFFSET BY EIGHT, and a
 * pointer at (160, 100) is at (152, 92) on a 320x200 screen.
 *
 * Image 0x07218 draws it, masked through GC register 8, from a pre-shifted
 * store: image 0x05827 takes the low three bits of ds:0x10 as the phase and
 * multiplies by 144, the ink three bytes a row for sixteen rows in two
 * planes, and the mask at +0x60. Images 0x0719C and 0x071CA save and restore
 * the 3x16 bytes underneath it, at the 44-byte stride of a 352-pixel line.
 *
 * THE SHAPE HERE IS MEASURED, not read out of a file: it is the pixels of
 * the original's own recorded frames that the port did not draw, and it is
 * the same 28 pixels, the same three colour indices and the same place in
 * every one of 5,100 frames. Where the unshifted sprite lives in MAIN.DAT
 * has not been traced, so this is not a transcription and does not claim to
 * be one - see level_cursor_draw.
 *
 * Zero is transparent; the original's mask says which pixels it touches.
 */
#define CURSOR_W 14
#define CURSOR_H 14
/* The cross-hair's centre, and the point the pointer is actually at. */
#define CURSOR_HOT_X 7
#define CURSOR_HOT_Y 7
/* The eight the coordinates are offset by - image 0x0426D and image 0x04282
 * clamp to 8 at the low end. */
#define CURSOR_ORIGIN 8

static const uint8_t CURSOR[CURSOR_H][CURSOR_W] = {
    { 0,0,0,0,0,0,3,2,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,3,2,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,3,2,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 2,0,2,0,2,0,1,1,0,3,0,3,0,3 },
    { 3,0,3,0,3,0,1,1,0,2,0,2,0,2 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,2,3,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,2,3,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,2,3,0,0,0,0,0,0 },
};

/* Where the pointer is, in the ORIGINAL's coordinates - ds:0x10 and ds:0x12,
 * so eight more than the screen position and clamped the way image 0x0426D
 * clamps them. Image 0x00F41 starts it at (150, 100), which is where the
 * game puts it before anyone touches the mouse. */
uint16_t lem_mouse_x = 150;
uint16_t lem_mouse_y = 100;

/* Transcribed from image 0x0426D: the original's own handler clamps the
 * position it got from INT 33h function 3 to 8..0x148 across and 8..0xC8
 * down (image 0x04282) before storing it, so the game never sees a pointer
 * outside its own range. */
void lem_mouse_move(int32_t x, int32_t y)
{
    if (x < 8) x = 8;
    if (x > 0x148) x = 0x148;
    if (y < 8) y = 8;
    if (y > 0xC8) y = 0xC8;
    lem_mouse_x = (uint16_t)x;
    lem_mouse_y = (uint16_t)y;
}

/*
 * NOT TRANSCRIBED. Draw it wherever the pointer is.
 *
 * The original composes it through the EGA's write logic - image 0x07218,
 * masked through GC register 8, out of the pre-shifted store image 0x05827
 * indexes by the low three bits of ds:0x10 - and this writes indices into a
 * byte buffer instead, the same picture by a different route, which is what
 * the rest of the port does for every sprite.
 *
 * The SHAPE is measured against the original rather than read out of a file:
 * where the unshifted sprite lives in MAIN.DAT has not been traced. See
 * CURSOR above for what the measurement was. */
void level_cursor_draw(uint8_t *pix, uint16_t w, uint16_t h, uint16_t view_x)
{
    int32_t sx = (int32_t)lem_mouse_x - CURSOR_ORIGIN - CURSOR_HOT_X;
    int32_t sy = (int32_t)lem_mouse_y - CURSOR_ORIGIN - CURSOR_HOT_Y;
    (void)view_x;                 /* the cursor is on the SCREEN, not the level */
    for (uint16_t r = 0; r < CURSOR_H; r++) {
        int32_t y = sy + r;
        if (y < 0 || y >= (int32_t)h) continue;
        for (uint16_t c = 0; c < CURSOR_W; c++) {
            int32_t x = sx + c;
            uint8_t v = CURSOR[r][c];
            if (!v || x < 0 || x >= (int32_t)w) continue;
            pix[(size_t)y * w + (size_t)x] = v;
        }
    }
}

/* THE PANEL'S OWN UPPER EIGHT COLOURS - transcribed data, image 0x0C080.
 *
 * Which is ds:0x25F0, and it is reached through the branch at image 0x055B7:
 * that compares ds:0x1F7C against 4 and takes the pointer table at cs:0x52E4
 * on a match, cs:0x52D4 otherwise. The adapter letter sets the variable -
 * image 0x01312 stores 4 for 'v', which is what LEMMINGS.BAT passes as
 * `lemvga.com /v`.
 *
 * The screen is EGA mode 0x0D in sixteen colours and the game SWAPS the upper
 * eight of them part-way down it, so the play area can be the terrain's
 * browns while the status line and the icons below are greens and greys. The
 * lower eight never change.
 *
 * Measured, not inferred: tools/probe_dac.py samples the emulator's palette
 * every 512 instructions between two of the game's own end-of-frame points at
 * image 0x018F, and every settled frame holds exactly TWO palettes, entries
 * 16..23 alternating between
 *
 *     d38220 c35110 922010 610010 414151 616171 719200 206120   the terrain
 *     00aa00 55ff55 555555 aa0000 aa5500 00aaaa ff55ff aa00aa   this table
 *
 * and ending on the terrain's, which is the one the next frame starts with.
 * The second is byte-identical to the eight triples here.
 *
 * A capture cannot see this. tools/record_level.py dumps the DAC once a
 * frame, so the reference holds whichever palette was current - and the port
 * was rendering the whole 320x200 from it, which is why the status line came
 * out with the terrain's oranges through it while every INDEX matched.
 */
static const uint8_t PANEL_PAL6[8 * 3] = {
    0x00, 0x2A, 0x00,   /* 8  green      */
    0x15, 0x3F, 0x15,   /* 9  bright green */
    0x15, 0x15, 0x15,   /* 10 dark grey  */
    0x2A, 0x00, 0x00,   /* 11 red        */
    0x2A, 0x15, 0x00,   /* 12 brown      */
    0x00, 0x2A, 0x2A,   /* 13 cyan       */
    0x3F, 0x15, 0x3F,   /* 14 magenta    */
    0x2A, 0x00, 0x2A,   /* 15 dark magenta */
};

/* Fill `out` with those eight, widened - the table at image 0x0C080 above.
 *
 * They are attributes 8..15: the
 * attribute controller maps those onto DAC 16..23 - read off the hardware as
 * `attr_pal = 00 01 02 03 04 05 06 07 10 11 12 13 14 15 16 17`, which is the
 * `v < 8 ? v : v + 8` that tools/compare_frame.py has always used. */
void level_panel_palette(lem_rgb *out)
{
    for (uint8_t i = 0; i < 8; i++) {
        out[i].r = widen6(PANEL_PAL6[i * 3 + 0]);
        out[i].g = widen6(PANEL_PAL6[i * 3 + 1]);
        out[i].b = widen6(PANEL_PAL6[i * 3 + 2]);
    }
}

/* ONE FRAME OF A LEVEL, the simulation half - image 0x019C's order.
 *
 * The play loop at image 0x019C..0x01C5 runs the frame counter (image
 * 0x01A3), the release (image 0x030A6 through image 0x01B0), the simulation
 * (image 0x01F19 through image 0x01B3), then the drawing. This is everything
 * up to the drawing, and level_play_draw is the rest.
 *
 * It is one routine because it was two: sdl_io.c stepped the objects and
 * nothing else while devmain.c ran the whole thing, so every frame-by-frame
 * measurement passed against a harness that exercised what the game did not
 * - the playable game showed an empty level. A sequence written out twice is
 * a sequence that will differ, so both callers use this.
 *
 * `terrain` is the buffer with NO objects on it, because that is what the
 * original's lemmings probe. `released` is carried across frames.
 */
void level_play_step(level_anim *st, const level *lv, lemming *lem,
                     uint8_t *released, uint8_t *terrain,
                     uint16_t w, uint16_t h)
{
    level_frame_step(st, lv);
    if (level_released() != *released) {
        lemmings_release(lem, *released, lv);
        *released = level_released();
    }
    lemmings_step(lem, *released, terrain, w, h);
    /* level_frame_step wrote the RELEASED count; the panel wants the LIVE
     * one, so this goes after the simulation has had its say. */
    level_status_out(lemmings_live(lem, *released));
}

/* And the drawing half: the objects at image 0x07BB8, which image 0x01B9
 * calls, and then the lemmings at image 0x01BC - which is why they go on
 * LAST, over both the terrain and the objects. */
void level_play_draw(level_anim *st, const level *lv,
                     const uint8_t *obj_sec, uint32_t obj_len,
                     const uint8_t *lem_sec, uint32_t lem_len,
                     const lemming *lem, uint8_t released,
                     uint8_t *pix, uint16_t w, uint16_t h)
{
    level_anim_draw(st, lv, obj_sec, obj_len, pix, w, h, 0);
    if (!lem_sec) return;
    for (uint8_t k = 0; k < released && k < LEMMING_MAX; k++)
        lemming_draw(pix, w, h, lem_sec, lem_len, &lem[k]);
}

/* THE CONTROL PANEL, one frame of it.
 *
 * NOT TRANSCRIBED. Every piece it calls is transcribed and carries its own
 * address, but the routine that calls them in order has not been found, so
 * the SEQUENCE here is measured - the same standing as menu_build_rating had
 * before image 0x04401 turned up, and it should be chased the same way.
 *
 * It is here rather than in a caller because the order is the game's
 * whichever way it was arrived at. It had been written out once in devmain.c
 * for --panel and not at all in the window path, so the game you could
 * actually play showed no panel while every measurement of the panel passed.
 * Two copies of a sequence, one of them missing, is how that happens.
 *
 * `pp` is PANEL_LINE_W wide, not 320: the artwork sits PANEL_ART_X into the
 * logical line and the status text starts at the line's first cell. The
 * screen shows it windowed from PANEL_ART_X.
 *
 * `view_x` is where the play area has scrolled to, which is what the
 * viewport box on the minimap marks.
 */
void level_panel_build(uint8_t *pp, const uint8_t *sec, uint32_t seclen,
                       const level *lv, const uint8_t *terrain,
                       uint16_t tw, uint16_t th,
                       const lemming *lem, uint8_t nlem,
                       uint16_t view_x, uint8_t sel)
{
    panel_frame(pp, PANEL_LINE_W, PANEL_ART_H, sec, seclen, PANEL_ART_X, 0);
    panel_status(pp, PANEL_LINE_W, PANEL_ART_H, sec, seclen,
                 level_status_line(), 0, 0);
    panel_buttons(pp, PANEL_LINE_W, PANEL_ART_H, sec, seclen, lv);
    panel_highlight(pp, PANEL_LINE_W, PANEL_ART_H, sel);
    panel_minimap(pp, PANEL_LINE_W, PANEL_ART_H, terrain, tw, th,
                  MINIMAP_AT_X, MINIMAP_AT_Y);
    panel_viewport(pp, PANEL_LINE_W, PANEL_ART_H, view_x,
                   MINIMAP_AT_X, VIEWPORT_AT_Y);
    panel_minimap_dots(pp, PANEL_LINE_W, PANEL_ART_H, lem, nlem,
                       MINIMAP_DOT_X, MINIMAP_AT_Y);
}

/* Image 0x07BB8.
 *
 *     ax = be16([di]) ; x from the LEVEL's own list, unbiased
 *     sub ax,[0]      ; minus the scroll
 *     add ax,0x10     ; +16 - the same sixteen the canvas is offset by, and
 *                     ;   the reason level_parse subtracts it there instead
 *     bx = be16([di+2])                 y
 *     dl = [si+4] dh = 0                width in pixels
 *     cl = [si+5] ch = 4                rows, four planes
 *     si = [si+0x17]                    the ink
 *     di = [si+8] + si                  the mask
 */
void level_anim_draw(const level_anim *st, const level *lv,
                     const uint8_t *object_sec, uint32_t object_len,
                     uint8_t *dst, uint16_t dst_w, uint16_t dst_h,
                     uint16_t scroll)
{
    for (uint16_t i = 0; i < lv->num_objects && i < LEVEL_ANIM_MAX; i++) {
        const level_anim *a = &st[i];
        if (!a->live || !a->w || !a->rows) continue;
        const level_object *o = &lv->objects[i];
        /* level_parse has already taken the 16 off, so this is the original's
         * `sub ax,[0] / add ax,0x10` with the +16 already applied. */
        int32_t x0 = (int32_t)o->x - (int32_t)scroll;
        int32_t y0 = o->y;
        uint16_t stride = (uint16_t)((a->w + 7u) / 8u);
        uint32_t plane  = (uint32_t)stride * a->rows;
        for (uint8_t r = 0; r < a->rows; r++) {
            int32_t y = y0 + r;
            if (y < 0 || y >= dst_h) continue;
            for (uint8_t c = 0; c < a->w; c++) {
                int32_t x = x0 + c;
                if (x < 0 || x >= dst_w) continue;
                uint32_t ink  = a->ink;
                uint32_t mask = ink + a->mask_delta;
                if (mask + plane > object_len) continue;
                uint8_t m = (object_sec[mask + (uint32_t)r * stride + (c >> 3)]
                             >> (7 - (c & 7))) & 1u;
                if (!m) continue;
                /* The LVL's own modifier - `e[6] & 0x80`, which level_parse
                 * already reads as no_overwrite, and which reaches the
                 * blitter as bit 15 of bp: the object is not drawn where
                 * there is terrain. Every object in "Just dig!" carries it.
                 *
                 * Measured, and the separation is total: of the pixels where
                 * the port drew the exit and the original did not, 23 of 23
                 * had terrain under them; of the pixels where both drew it,
                 * 49 of 49 did not. Terrain is the only thing with bit 3
                 * set, which is what makes the test exact rather than
                 * "something is already there". */
                if (o->no_overwrite
                        && (dst[(size_t)y * dst_w + (uint16_t)x] & 8))
                    continue;
                dst[(size_t)y * dst_w + (uint16_t)x] =
                    planar(object_sec, ink, plane, stride, 4, c, r);
            }
        }
    }
    (void)st;
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
/* The four rating names, as 72x27 four-plane sprites in section 4. They are
 * stored in REVERSE - Mayhem first, Fun last - which is why only Fun was ever
 * right while the step was read as positive: at rating 0 the sign of the step
 * does not matter, and no other rating could be reached to contradict it.
 *
 * The offsets are measured, not guessed. Each was found by cropping the
 * original's own plate at (486,145) out of a capture taken at that rating and
 * searching section 4 for the block that decodes to it; each match is exact,
 * zero pixels differing:
 *
 *     Mayhem 23168   Taxing 24140   Tricky 25112   Fun 26084
 *
 * That is a uniform 972 = RATING_PLANE * 4 apart, descending. */
#define RATING_BASE    23168        /* Mayhem, the FIRST of the four */
#define RATING_W       72
#define RATING_H       27
#define RATING_STRIDE  9
#define RATING_PLANE   243
#define RATING_STEP    (RATING_PLANE * 4)
#define RATING_X       486
#define RATING_Y       145

/* Where rating r's sprite starts: they run backwards, so Fun is last. */

/* Both lines come from the executable, not from a data file, so they are
 * transcribed here with the address they were read from - the same treatment a
 * routine gets. They sit in a table of the menu's and briefing's strings. */
static const char CREDIT_1[] = "(C)MCMXCI Psygnosis Ltd";   /* image 0x0E56C */
static const char CREDIT_2[] = "A DMA Design Game";         /* image 0x0E587 */

/* Where a line of n characters starts. Centring alone gives 136 and 184 for
 * the two lines; the original draws them at 128 and 176, so it centres and
 * then backs off half a character. Both lines agree on that, which is why it
 * is written as a rule rather than as two measured constants. */
/* WHERE THE TEXT GOES IS DATA, NOT A FORMULA.
 *
 * The original's text routine at image 0x04D4D reads a three-byte header off
 * the front of every string before it draws a character:
 *
 *     lodsb / mov cl,al / dec cl     cl = x in 16-pixel cells
 *     lodsb / mov ch,al              ch = y in 16-pixel cells
 *     lodsb                          a flag; 0 sets bp=2, else bp=0
 *     ... characters, terminated by 0x0D
 *
 * and the two credit lines carry theirs in the string table: `09 11` before
 * "(C)MCMXCI Psygnosis Ltd" is x=(9-1)*16=128, y=17*16=272, and `0C 12`
 * before "A DMA Design Game" is x=176, y=288.
 *
 * Those are exactly the numbers this file used to reach by centring and then
 * subtracting half a character - a rule fitted to two data points that were
 * never computed at all. The rule got the right answer for the two strings it
 * was fitted to, which is the whole trouble with it. */
#define TEXT_CELL   16          /* the header's units, image 0x04D4D */
#define CREDIT_1_CELL_X  9      /* the stored byte; x is (it - 1) * 16 */
#define CREDIT_1_CELL_Y 17
#define CREDIT_2_CELL_X 12
#define CREDIT_2_CELL_Y 18
#define CELL_X(b)   (uint16_t)(((b) - 1) * TEXT_CELL)
#define CELL_Y(b)   (uint16_t)((b) * TEXT_CELL)
#define CREDIT_1_Y  CELL_Y(CREDIT_1_CELL_Y)
#define CREDIT_2_Y  CELL_Y(CREDIT_2_CELL_Y)

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

/* The six signs, one atlas entry each, 4 x 915 = 3660 bytes apart. The atlas
 * begins at 38024, which is exactly where the logo's fourth plane ends. */
#define SIGN_BASE   38024
#define SIGN_W      120
#define SIGN_H      61
#define SIGN_STRIDE 15
#define SIGN_PLANE  915
#define SIGN_STEP   3660
/* Where a plate's shadow falls, measured from where the blitter writes. */

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

/* Where the six plates land. NOT a table in the original - image 0x04F31 runs
 * two loops, four from (68,120) and two from (196,197), each `add ax, 0x80`.
 * This exists only because menu_hotspots needs the boxes, and it is written as
 * the loops so there is one source of truth rather than two.
 *
 * SIGN_ROW is the loop's own parameters; draw_plate_row takes the same ones. */
static const struct { uint8_t first, count; uint16_t x, y; } SIGN_ROW[2] = {
    { 0, 4,  68, 120 },
    { 4, 2, 196, 197 },
};
#define SIGN_STRIDE_X 0x80          /* image 0x04F68: add ax, 0x80 */



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

/* Blit a 4-plane sprite the way image 0x0890E does, which is not "keyed".
 *
 * The blitter branches on bit 6 of bp at image 0x08905; the menu passes
 * bp=0x03 for a sprite and bp=0xF43 for its shadow, and 0x40 is the only bit
 * that differs. Both paths take a mask pointer, which the menu supplies as
 * ink + 3 * plane_size - the plates at si+2745 with planes of 915, the logo at
 * si+22278 with planes of 7426. The mask is the sprite's own fourth plane.
 *
 * THE SHADOW, image 0x0909C. Set/Reset (GC 0) = 0, Enable Set/Reset (GC 1) =
 * (bp >> 8) & 0xF = 0xF, so all four planes are driven to zero: solid black.
 * Its silhouette is the mask.
 *
 * THE SPRITE, image 0x0890E, in FOUR PASSES over the planes, and the passes
 * do not do the same thing:
 *
 *   pass 0   Enable Set/Reset = 0x0E, so planes 1..3 take Set/Reset = 0 while
 *            plane 0 takes the CPU byte. The mask row goes into GC 8, the BIT
 *            MASK (image 0x089B5: `mov ah,[bx]` / `mov al,8` / `out`), with a
 *            latch load before each write. So where the mask bit is set, this
 *            writes plane 0 from the ink and CLEARS planes 1..3; where it is
 *            clear, the byte is left alone.
 *
 *   passes 1..3   image 0x089ED turns Enable Set/Reset off, sets the function
 *            to OR (GC 3 = 0x10), sets the bit mask back to 0xFF, and selects
 *            one plane at a time through the Map Mask. Bit mask 0xFF means
 *            these are NOT masked: every byte in the rectangle is OR-ed.
 *
 * Net effect per pixel, which is the thing to port:
 *
 *     mask set    dst  = ink                (planes 1..3 cleared, then OR-ed)
 *     mask clear  dst |= ink & 0x0E         (plane 0 untouched, 1..3 OR-ed)
 *
 * And that is exactly what the screen shows. Sprite value 3 over background 0
 * gives 0 | (3 & 0xE) = 2 - the original shows 2, 210 times, where keying on
 * "value != 0" drew 3. Value 2 gives 2; value 1 gives 0, 22 times. Values
 * 4..7 never occur.
 *
 * Four models were measured before the code was read, and the winner was the
 * least defensible of them:
 *
 *     ink != 0,   shadow != 0        326 differ
 *     ink plane 3, shadow != 0       460
 *     ink plane 3, shadow plane 3    377
 *     ink != 0,   shadow plane 3     257
 *     THIS                              0   <- transcribed, not fitted
 *
 * Zero. All four ratings, every pixel of the screen. The lesson is not that
 * measurement was useless - it narrowed four models to one and found the
 * shadow rule - but that the best-scoring model was still wrong, and no
 * amount of further fitting was going to reach zero from 257. Reading the
 * routine took less time than the fitting did. */
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
            size_t o = (size_t)y * MENU_W + x;
            if (v & 8) dst[o] = v;              /* mask set: the ink wins */
            else       dst[o] |= (uint8_t)(v & 0x0E);   /* else OR planes 1..3 */
        }
    }
}

/* One glyph, keyed on plane 2. Characters outside the block are skipped
 * rather than clamped: a wrong glyph is worse than a gap, because it looks
 * like a font bug rather than a missing character.
 *
 * Transcribed from image 0x04D78. The space-skip is the original's own first
 * instruction, `cmp al,0x20 / je`; the glyph is `(ch - 0x21) * 96` from
 * 0x5BC0 with the mask at +0x40.
 */
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
            /* Same two-phase rule as blit(): the glyph's last plane is the
             * bit mask for pass 0 and an ink plane for the OR passes. */
            uint8_t p0 = (font[o] >> sh) & 1u;
            uint8_t p1 = (font[o + FONT_PLANE] >> sh) & 1u;
            uint8_t mask = (font[o + FONT_MASK_PLANE * FONT_PLANE] >> sh) & 1u;
            uint8_t v = (uint8_t)(p0 | (p1 << 1) | (mask << FONT_MASK_PLANE));
            size_t d = (size_t)y * MENU_W + x;
            if (mask) dst[d] = v;
            else      dst[d] |= (uint8_t)(v & ~1u);
        }
    }
}

/* Transcribed from image 0x04D4D. The x and y come from the string's own
 * three-byte header there - cl = x in 16-pixel cells with one added, ch = y in
 * cells - which is why this takes cells rather than pixels and no longer
 * centres anything.
 */
static void draw_text(uint8_t *dst, const uint8_t *font, uint32_t font_len,
                      const char *text, uint8_t cell_x, uint8_t cell_y)
{
    uint16_t x = CELL_X(cell_x);
    uint16_t y = CELL_Y(cell_y);
    for (size_t i = 0; text[i]; i++)
        draw_char(dst, font, font_len, (uint8_t)text[i],
                  (uint16_t)(x + i * FONT_W), y);
}

/* NOT TRANSCRIBED - the port's own compositing of the original's own
 * graphics and geometry. Every constant it uses was measured against the
 * screen the original composes; tools/verify_menu.py scores it at 94.67%. */
/* The strings for the two sub-screens, transcribed from the executable with
 * the addresses they were read from. They are not in any data file. */
static const char S_ENTER_CODE[]  = "Enter Code";          /* image 0x0E874 */
static const char S_CODE_DOTS[]   = "..........";          /* image 0x0E882 */
/* Image 0x04C92 keeps the pair in CX: cl is '.' and ch is '_'. */
#define CODE_CARET  '_'
/* Image 0x04C95 and image 0x04CB3: twenty retraces each way. */
#define CODE_BLINK  0x14

/* Is the caret showing, and which of the ten cells is it on? The game's own
 * state - ds:0x4DF2's contents and the cursor image 0x04C8C keeps in DI - so
 * it is here rather than in sdl_io, and the accessors are not. */
uint8_t menu_caret;
uint8_t menu_caret_cell;
static uint8_t caret_ticks = CODE_BLINK;

/* One retrace of image 0x04C7E's loop: image 0x04CAD counts down and image
 * 0x04CB8 flips the cell when it reaches zero. */
void menu_caret_step(void)
{
    if (--caret_ticks) return;
    caret_ticks = CODE_BLINK;
    menu_caret = (uint8_t)!menu_caret;
}
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

/*
 * NOT TRANSCRIBED. The original does set these screens up - image 0x04967
 * and 0x04A01 both open with 0x04DEF, 0x04BF5, 0x0531E and 0x05017 - and
 * none of those four has been read.
 * in place and has no equivalent shared entry.
 */
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
        m->palette[i].r = widen6(MENU_PAL6[i * 3 + 0]);
        m->palette[i].g = widen6(MENU_PAL6[i * 3 + 1]);
        m->palette[i].b = widen6(MENU_PAL6[i * 3 + 2]);
    }
    /* `tile_background` is a vertical ORIGIN plus one, so 1 means "tile from
     * row 0" and 0 means "do not tile". The briefing tiles the very same
     * bitmap from row 78, which is the whole of what made it look like a
     * different background for so long. */
    if (tile_background) {
        uint16_t org = (uint16_t)(tile_background - 1);
        for (uint16_t y = org; y < MENU_H; y++)
            for (uint16_t x = 0; x < BG_DRAWN_W; x++)
                m->pix[(size_t)y * MENU_W + x] =
                    planar(*sec, BG_PLANE0, BG_PLANE1 - BG_PLANE0, BG_STRIDE,
                           2, x % BG_W, (uint16_t)((y - org) % BG_H));
    }
    return 0;
}

/* Text at a given cell, rather than centred.
 *
 * Transcribed from image 0x04D4D - it walks a string calling image 0x04D78
 * per character and stops on 0x0D.
 */
static void draw_at(uint8_t *dst, const uint8_t *font, uint32_t font_len,
                    const char *text, uint16_t at_x, uint16_t at_y)
{
    for (size_t i = 0; text[i]; i++)
        draw_char(dst, font, font_len, (uint8_t)text[i],
                  (uint16_t)(at_x + i * FONT_W), at_y);
}

/* The screen F2 reaches: the level-code entry. What it does with the code is
 * NOT reconstructed - this draws it and comes back.
 *
 * Transcribed from image 0x04967. Its ten dots are the original's own:
 * 0x0497D stores 0x2E ten times into ds:0x4DF2.
 *
 * The CARET is one of those ten cells, not a sprite. Image 0x04C7E is the
 * entry loop, and image 0x04CB8 is the whole of the blink: every 0x14
 * retraces - the counter at ds:0x491E, reloaded at image 0x04CB3 - it looks
 * at the cell the cursor is on and swaps it between '.' (0x2E) and '_'
 * (0x5F). This file used to describe it as "a bar over the first code slot",
 * which is what an underscore looks like when you have only seen it in a
 * capture.
 */
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
    {
        /* The cell under the cursor, with the caret in whatever phase it is
         * in - see the blink above. Everything else stays a dot. */
        char cells[sizeof S_CODE_DOTS];
        memcpy(cells, S_CODE_DOTS, sizeof cells);
        if (menu_caret && menu_caret_cell < sizeof S_CODE_DOTS - 1)
            cells[menu_caret_cell] = CODE_CARET;
        draw_at(m->pix, font, fontlen, cells, CODE_DOTS_X, CODE_DOTS_Y);
    }
    dat_free(&d);
    return 0;
}

/* The screen F4 reaches: the input-device chooser. Choosing is NOT
 * reconstructed - the port always uses the mouse and the keyboard.
 *
 * Transcribed from image 0x04A01. The second line is conditional there too,
 * on ds:0x1F73.
 */
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
 * and fills in the container's name and the section index.
 *
 * Transcribed from image 0x01983: it reads the order table at ds:0x54EE
 * (image 0x0EF7E), takes the entry minus one, shifts it four right for the
 * container and masks 7 for the section, and calls digits3 at image 0x019A0.
 */
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
/* Where the briefing's background tiling starts - see menu_briefing_screen. */
#define BR_BG_Y        78
#define BR_TITLE_Y     80
#define BR_LEVEL_X      0
#define BR_NAME_X     128      /* the LVL's own 32-char field, padding and all */
#define BR_BODY_X     160
#define BR_BODY_Y     128
#define BR_BODY_STEP   32
#define BR_CONT_X      80
#define BR_CONT_Y     304

/*
 * NOT TRANSCRIBED, but its callers are now known. The briefing's own numbers
 * go through THREE different routines, and image 0x04566 names each: the
 * lemming count through image 0x04B68 (status_put_num, transcribed), the
 * percentage through image 0x04B5C, and the level number and release rate
 * through image 0x04BBE. This is one right-aligned fill standing in for all
 * three, which is why it is owed: the port gets the same characters into the
 * same columns by its own arithmetic, and image 0x04BBE has not been read.
 */
static void put_num(char *dst, size_t end, uint32_t n)
{
    size_t i = end;
    do {
        dst[i--] = (char)('0' + n % 10);
        n /= 10;
    } while (n && i != (size_t)-1);
}

/* THE LEVEL PREVIEW, above the briefing's text.
 *
 * The palette said so and it took saying out loud to hear it: GROUNDxO.DAT's
 * third upper-eight palette is called `preview`, and what it is the preview
 * OF is this. Rows 0..77 of the briefing hold the level's own terrain at
 * QUARTER SCALE.
 *
 * Measured against a capture of the original's briefing:
 *
 *   the preview's ink box is 127 x 39; the composed terrain's is 510 x 160,
 *   which is 4.016 and 4.103 - a 4:1 reduction on both axes
 *
 *   sampling pixel (2,2) of each 4x4 block, drawn at (120,19): 98.95%,
 *   167 of 15,840 differing
 *
 * TERRAIN ONLY. Adding the objects makes it worse - 187 against 167 - which
 * agrees with what tools/prove.sh already established about the composition:
 * the original never bakes the entrance and exit in, it draws them as sprites
 * at display time.
 *
 * The remaining 167 are almost all at piece edges, and in only 4 of them does
 * the original's colour appear anywhere in the 4x4 block being sampled. So
 * this is probably not how the original builds it: it more likely scales each
 * terrain piece as it draws it, rounding each piece's position on its own,
 * which would agree everywhere except where those roundings disagree. That is
 * a hypothesis, and it is written as one - what is measured is the 4:1
 * reduction, the origin, and the sample point.
 *
 * A failed search worth remembering: this was hunted for as a stored bitmap
 * and then as a 1:1 region of the level, which scored 71.4% and read as "no
 * match". It is the level, at a scale nobody had tried. */
#define PV_X      120
#define PV_Y       20
#define PV_W      396      /* LEVEL_W / 4 */
#define PV_H       39      /* not 40: the top row is not drawn - see below */
#define PV_SCALE    4
#define PV_SAMPLE   2      /* which pixel of each 4x4 block is taken */
#define PV_TOP      4      /* the first level row the preview shows
 *
 * NOT TRANSCRIBED, address unknown. The geometry is measured against the
 * original's screen, not read from its code; no blit in screen coordinates
 * was ever found for it.
 */

static void draw_preview(const char *dir, const level *lv, const ground *g,
                         uint8_t *dst)
{
    char path[1024];
    dat_file gfx;
    const char *err = NULL;
    snprintf(path, sizeof(path), "%s/vgagr%u.dat", dir,
             (unsigned)(lv->graphics_set & 0x0F));
    if (dat_load(path, &gfx, &err) != 0)
        return;                       /* no preview is better than a wrong one */
    if (gfx.count < 1) { dat_free(&gfx); return; }

    canvas c;
    if (canvas_init(&c) != 0) { dat_free(&gfx); return; }
    if (render_terrain(&c, lv, g, gfx.sections[0].data,
                       gfx.sections[0].size, &err) == 0) {
        /* THE OBJECTS ARE IN THE PREVIEW - the entrance and the exit.
         *
         * This file said "terrain only" for one commit, on the strength of an
         * aggregate score: drawing them made the preview worse, 187 differing
         * against 167. That was true and the conclusion drawn from it was
         * wrong. Looking at WHERE the differences were - which the score
         * cannot say - put them in two clusters, at level (708..747, 36..47)
         * and (860..887, 108..131), and the level's three objects are at
         * (720,36), (864,116) and (864,108). They were there all along; the
         * port was drawing them sixteen pixels to the right. See level_parse.
         *
         * With that fixed the preview goes to 114 differing, better than
         * leaving them out. An aggregate score is a poor way to decide whether
         * a feature belongs on a screen. */
        if (gfx.count >= 2)
            render_objects(&c, lv, g, gfx.sections[1].data,
                           gfx.sections[1].size, 0, &err);
        for (uint16_t sy = 0; sy < PV_H; sy++) {
            uint16_t y = (uint16_t)(PV_Y + sy);
            uint32_t ly = PV_TOP + (uint32_t)sy * PV_SCALE + PV_SAMPLE;
            if (y >= MENU_H || ly >= c.h) break;
            for (uint16_t sx = 0; sx < PV_W; sx++) {
                uint16_t x = (uint16_t)(PV_X + sx);
                uint32_t lx = (uint32_t)sx * PV_SCALE + PV_SAMPLE;
                if (x >= MENU_W || lx >= c.w) break;
                uint8_t v = c.pix[(size_t)ly * c.w + lx];
                if (v) dst[(size_t)y * MENU_W + x] = v;
            }
        }
    }
    canvas_free(&c);
    dat_free(&gfx);
}

/* The briefing's OWN lower seven DAC entries, transcribed from image 0x0C04B
 * (ds:0x25BB) - seven 6-bit RGB triples, entry 1 first.
 *
 * The briefing does not keep the menu's lower eight. The menu's are the
 * background's browns; the briefing's are the bright set its text is drawn in
 * - blue, green, white, yellow, red, grey, orange. Every pixel of the screen
 * matched by INDEX long before this, which is exactly why it went unnoticed:
 * a palette-only difference is invisible to a comparison of indices and
 * changes every colour on the screen.
 *
 * Six of the seven are byte-for-byte what a capture of the original shows.
 * The seventh, entry 4, reads (44,44,0) here and (60,60,0) on the screen -
 * brighter. Something modifies it at run time and that has not been read, so
 * it is left as the file has it rather than patched to match one capture. */
/* THE BRIEFING'S LOWER PALETTE - transcribed data, image 0x0E3C0.
 *
 * Which is ds:0x4930, entry [2] of the pointer table at cs:0x52E4 that image
 * 0x055B7 uses when ds:0x1F7C is 4. The article that named this behaviour
 * calls them "the brown earth colours and the blue shades for the font", and
 * that is what they are: 824120, 613020 and 300010 are earth, 20087d, 412c92
 * and 9a8ebe are the text.
 *
 * Measured against the running original, not inferred. tools/probe_dac.py on
 * the briefing shows the palette alternating between the preview's colours
 * and these, and these widen to exactly the DAC values it records:
 *
 *     000000 824120 613020 300010 20087d 412c92 (6959a6 at DAC 20) 9a8ebe
 *
 * The seventh entry landing on DAC 20 rather than 6 is the mode 0x10
 * attribute map - attribute 6 goes to DAC 0x14 - which is the same table
 * tools/verify_menu.py has always used to score this screen.
 */
static const uint8_t BRIEF_LOWER6[8 * 3] = {
    0x00, 0x00, 0x00,   /* 0  black       */
    0x20, 0x10, 0x08,   /* 1  earth       */
    0x18, 0x0C, 0x08,   /* 2  darker      */
    0x0C, 0x00, 0x04,   /* 3  darkest     */
    0x08, 0x02, 0x1F,   /* 4  deep blue   */
    0x10, 0x0B, 0x24,   /* 5  blue        */
    0x1A, 0x16, 0x29,   /* 6  pale blue   */
    0x26, 0x23, 0x2F,   /* 7  lavender    */
};

/* Where the swap happens. Rows 60..77 of the briefing are entirely black -
 * measured on the original's own capture - so any row in that band gives the
 * same picture, and this takes the first row after it. Those black scanlines
 * are the point: they are the margin the game needs to change the palette
 * without the change being visible. */
#define BRIEF_SPLIT_Y 78

static const uint8_t BRIEF_PAL6[7 * 3] = {
    0x10, 0x10, 0x38,   /* 1  blue   */
    0x00, 0x2C, 0x00,   /* 2  green  */
    0x3C, 0x34, 0x34,   /* 3  white  */
    /* 4  yellow. The FILE says 0x2C,0x2C,0x00 here and the running game
     * programs 0x3C,0x3C,0x00 - measured, repeatedly, by tools/probe_dac.py
     * reading the DAC on the briefing, where the fade climbs c3c300, cbcb00,
     * d3d300, dbdb00, e3e300, ebeb00 and settles on f3f300. No table in the
     * image holds the seven entries with 0x3C,0x3C,0x00 among them, so the
     * game builds its destination palette in RAM and adjusts this one on the
     * way; ds:0x2608 and ds:0x2638, the buffers image 0x055B7 fades between,
     * are zeros in the image and filled at run time.
     *
     * WHAT DOES THE ADJUSTING HAS NOT BEEN READ. This is the value the
     * original puts in the DAC, not a value fitted to make pixels agree. */
    0x3C, 0x3C, 0x00,
    0x3C, 0x08, 0x08,   /* 5  red    */
    0x20, 0x20, 0x20,   /* 6  grey   */
    0x38, 0x20, 0x08,   /* 7  orange */
};

/*
 * NOT TRANSCRIBED - but image 0x0454C HAS now been read, and this says what
 * it found so the next pass starts from the disassembly rather than from
 * nothing.
 *
 * Image 0x0454C zeroes ds:0..0xFCA, calls image 0x0197D, and calls image
 * 0x04566, which is the text assembly. That routine is transcribed rule by
 * rule below, each carrying its own address:
 *
 *     0x04566  the level number: rating * 30 subtracted from the index + 1,
 *              so the ratings are thirty levels apart
 *     0x0458A  the lemming count, through image 0x04B68 - status_put_num
 *     0x04594  the percentage: rescue * 100 / total, integer divide
 *     0x045AB  the release rate, through image 0x04BBE
 *     0x045B2  the minutes, and the plural: the 's' becomes a SPACE when the
 *              digit is '1', so the line keeps its length
 *     0x045C6  the level's name, 32 bytes copied whole - which is what
 *              centres it, padding and all
 *     0x045D5  the rating's name, six bytes from the table at ds:0x4C01
 *
 * What is still unread is the COMPOSITION that follows at image 0x045EF: the
 * clear at image 0x04DEF, the fade at image 0x05329, image 0x0394F, image
 * 0x07EB2, the page registers, and the draw at image 0x0463C. The port's
 * order is measured, the same standing menu_build_rating had before its
 * caller turned up - and that one was found by scanning for a near call to a
 * piece it drew, which is worth trying here too.
 */
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
    /* THE BACKGROUND IS THE MENU'S OWN TILE, TILED FROM ROW 78.
     *
     * This screen's background was recorded here for a long time as "not the
     * menu's tile, and in neither MAIN.DAT nor VGAGR0.DAT" - searched for as a
     * stored bitmap, at all eight bit alignments, and not found. It was the
     * tile all along; what was wrong was the vertical phase.
     *
     * tools/probe_blits.py --driven settles it by logging the blits rather
     * than searching for pixels. Three pairs of them, at y = 78, 182 and 286,
     * from srcseg 0x587E offset 0 - MAIN.DAT section 3's start, the menu's own
     * background - 320x104, two planes, exactly as the menu tiles it.
     *
     * Measured against a capture of the original's briefing, over rows 78..349
     * and x < 632 and background-coloured pixels: **100.00%, 0 of 156,720**.
     * Tiled from row 0 instead, the same comparison gives 44%.
     *
     * Rows 0..77 are NOT the tile - they are black with the level's own
     * preview art over them, about 3,000 pixels in colours 8..15, and that
     * preview is not reconstructed. So the tile starts at 78 and above it the
     * screen stays black, which is what the original has minus the preview. */
    if (screen_start(dir, m, &d, &sec, &font, &fontlen, BR_BG_Y + 1, err) != 0)
        return -1;

    /* PALETTE ENTRIES 8..15 ARE THE LEVEL'S, NOT THE MENU'S.
     *
     * This screen borrows the terrain colours of the level it is announcing.
     * Measured, not assumed: the original's own DAC on a captured briefing
     * reads 227,130,32 / 195,81,16 / 146,32,16 / 97,0,16 / 65,65,81 /
     * 97,97,113 / 113,146,0 / 32,97,32 at attribute entries 8..15, and those
     * are GROUND0O.DAT's **preview** palette exactly - all eight, byte for byte,
     * after the 6-bit-to-8-bit bit replication.
     *
     * The PREVIEW one, not the custom one the level itself is drawn in. The
     * file carries three upper-eight palettes and for graphics set 0 the two
     * differ in a single byte - entry 8's red, 0x38 against 0x34, 227 against
     * 211. Using `palette[8..15]` got seven of the eight and left exactly one
     * entry wrong, which is the sort of near-miss that reads as a rounding
     * error rather than as the wrong array.
     *
     * Entries 0..7 stay the menu's, which is what the text is drawn in. If
     * the ground file will not load the menu palette is left in place: a
     * briefing with the wrong eight colours is still a briefing, and refusing
     * to draw the screen over it would be worse. */
    {
        char gpath[256];
        ground g;
        const char *gerr = NULL;
        snprintf(gpath, sizeof(gpath), "%s/ground%uo.dat", dir,
                 (unsigned)(lv->graphics_set & 0x0F));
        if (ground_load(gpath, &g, &gerr) == 0) {
            for (uint8_t i = 8; i < 16; i++)
                m->palette[i] = g.preview[i - 8];
            draw_preview(dir, lv, &g, m->pix);
        }
    }

    /* And its lower seven are its own, not the menu's - see BRIEF_PAL6. */
    {
        for (uint8_t i = 1; i < 8; i++) {
            m->palette[i].r = widen6(BRIEF_PAL6[(i - 1) * 3 + 0]);
            m->palette[i].g = widen6(BRIEF_PAL6[(i - 1) * 3 + 1]);
            m->palette[i].b = widen6(BRIEF_PAL6[(i - 1) * 3 + 2]);
        }
    }

    /* The TEXT half of the screen has a palette of its own - see
     * BRIEF_LOWER6 - and the game swaps it in across the black band. */
    for (uint8_t i = 0; i < 16; i++)
        m->palette_low[i] = m->palette[i];
    for (uint8_t i = 0; i < 8; i++) {
        /* Attribute 6 goes to DAC 0x14 in mode 0x10, which is why the table's
         * seventh entry does not land on attribute 6; the port indexes by
         * ATTRIBUTE, so it goes where the attribute controller sends it. */
        uint8_t at = (i == 6) ? 6 : i;
        m->palette_low[at].r = widen6(BRIEF_LOWER6[i * 3 + 0]);
        m->palette_low[at].g = widen6(BRIEF_LOWER6[i * 3 + 1]);
        m->palette_low[at].b = widen6(BRIEF_LOWER6[i * 3 + 2]);
    }
    m->split = BRIEF_SPLIT_Y;

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
    /* "1 Minute", not "1 Minutes". Image 0x045B2 puts the digit at ds:0x4B83
     * and the 's' at ds:0x4B8B - eight characters apart, which is indices 5
     * and 13 of this template - and swaps the 's' for a SPACE when the digit
     * is '1', so the line keeps its length either way. */
    if (line[5] == '1')
        line[13] = ' ';
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

/* THE SCROLLING BANNER.
 *
 * Not a sprite, and worth saying why the search for one was wrong. Logging the
 * blits that land in these rows finds the source at image 0x191BC - in BSS -
 * and dumping it shows rows of 0xFF, 0xF0 and 0x00. That is the strip the game
 * SCROLLS, not artwork; the artwork is a single 16x16 column it tiles.
 *
 * The whole thing is readable in the original, at image 0x07CBC (set up) and
 * 0x07D0D (one step), and it works like this:
 *
 *   - a strip of 35 columns, 16 pixels each, four planes, 70 bytes to a row
 *     (image 0x07D54 shifts exactly that: cx=4 planes, cx=0x10 rows, cx=0x46
 *     bytes, shifting the row left four bits at a time)
 *   - every step shifts it left 4 pixels and adds 4 to ds:0x21
 *   - when that crosses a multiple of 16 a new character is due: image 0x07D8D
 *     stamps the BAR TEMPLATE into the rightmost column, then image 0x07DB7
 *     masks a glyph over it
 *   - 0xFF in the text is not a glyph. It sets ds:0x23 = 100 - a hundred-step
 *     pause - and the character after it is drawn instead
 *   - 0x0D rewinds to the start of the string
 *
 * So the dashed rules are not a property of the bar's position: they are part
 * of the tile, and they SCROLL. An earlier version of this file had them as
 * `(x & 7) < 4`, fitted to one capture, and that is wrong in the same way the
 * `>= 4` before it was wrong - the phase is the scroll offset, and captures
 * taken at different moments disagree. Four captures give phases 0, 0, 4 and 4
 * mod 8, which is what a 4-pixel step produces.
 *
 * The tile is section 4 offset 23040 - the original's ds:0x4C10, in the
 * segment at [0x1FEE]+0x1000, which is section 4 shifted by 3568. Decoded it
 * is colour 13 everywhere except rows 2 and 13, which run 0000dddd0000dddd:
 * exactly the two dashed rules at screen y 336 and 347.
 *
 * The font is the SAME font the rest of the menu uses. The original computes
 * the glyph as 0x5BC0 + (ch - 0x21) * 96, and 0x5BC0 + 3568 - 0x21 * 96 is
 * 23888 - FONT_BASE. docs/menu.md called it "a different font"; it is not.
 *
 * Still not drawn: the two lemmings holding the ends (x 12..35 and 605..628).
 * They are what puts colour 12 in the margins, a stripe of colour 6 at x=592,
 * and the disturbance in the dash near the right edge, where the clean runs
 * stop at 584..587. */
/* The PAINTED extent of the bar, measured off the original's screen. */
#define BAR_X0    36
#define BAR_X1   602
/* Where the STRIP itself is blitted, which is not the same thing. Image
 * 0x07CEE loads ax=0x30, bx=0x14E, dx=0x230 and calls the shared blitter at
 * 0x1010:0x8822, whose convention that entry establishes: ax is x in pixels,
 * bx is y, cl is rows, ch is planes, dx is the width in pixels. So the strip
 * is 560 pixels at x=48, y=334 - and the painted bar runs 36..602, which the
 * strip does not reach at either end.
 *
 * The 12 pixels at 36..47 and the few past 602 belong to the two lemmings
 * holding the bar: their sprites carry its end caps. Until those are located
 * the port tiles the bar's own artwork through that gap, which is the right
 * colour and the wrong provenance, and is marked as such rather than folded
 * into the strip's geometry. A capture cannot tell the two apart - the tile
 * repeats every 16 pixels, so a 12-pixel error in the origin is absorbed by
 * the scroll phase, and the fit is degenerate. The blitter's arguments are
 * not. */
#define BAR_STRIP_X 48
/* The phase between this port's scroll and the original's own counter.
 *
 * Image 0x07E2F takes the end lemmings' frame from `ds:0x21` - the same word
 * the ribbon runs off - as `(c >> 2) & 15`: sixteen frames over a 64-pixel
 * period, each 384 bytes, the right-hand one 0x1800 further on. The port's
 * table has that same 0x1800 between its two bases, so the geometry is the
 * original's; only the ORIGIN differs, the port indexing a loaded section
 * where the original indexes ds.
 *
 * Four is measured, not reasoned. Forcing each of the sixteen frames against
 * four captures and scoring the two lemming rectangles alone puts the winner
 * at exactly ZERO differing pixels every time, with the runner-up about a
 * hundred away - and the winner is the original's own `(c >> 2) & 15` less
 * two at all four. The port's base therefore sits two frames into the
 * original's table, and `scroll + 4` restores the index.
 *
 * A previous attempt to unify this with the 140 that tools/verify_menu.py
 * uses for the ribbon made it worse; the two constants are not the same
 * quantity, and the scoring above is what settled it.
 */
#define BAR_SCROLL_INIT 4

/* The port's end-lemming table holds the original's sixteen frames, but its
 * first entry is the original's frame TWO: forcing each frame against four
 * captures and scoring the two lemming rectangles alone puts the winner at
 * exactly zero differing pixels every time, and the winner is the original's
 * own `(ds:0x21 >> 2) & 15` less two at all four ratings.
 *
 * It is a bias on the INDEX and not on the base. Dropping LEM_L_BASE by the
 * same two frames looks equivalent and is not - the wrap is taken modulo
 * sixteen against the base, so moving the base moves the wrap point with it.
 * Three ratings went to zero that way and mayhem, the one whose index wraps,
 * went to 1,218 differing pixels. */
#define LEM_FRAME_BIAS 2
#define BAR_Y0   334
#define BAR_Y1   349
#define BAR_TILE  23040       /* section 4: the bar's own 16x16 column */
#define BAR_TILE_W 16
#define BAR_TILE_PLANE 32     /* 16 rows x 2 bytes */

/* THE FIVE LEMMINGS THAT WALK ON, transcribed from image 0x04F96.
 *
 * The menu loop calls it once a pass. It walks a ten-entry table at ds:0x4960
 * (image 0x0E3F0), eleven bytes each - counter, reload, x, y, sprite base -
 * and the whole of the routine is this:
 *
 *     counter == 0x8000        the entry is switched off; five of the ten are
 *     counter-- each pass
 *     counter == -17           reload it, and skip
 *     counter >= 0             still waiting: draw nothing
 *     counter in -1..-16       frame = (-counter - 1) >> 1, so eight frames
 *                              held for two passes each
 *
 * and then a 32x12 four-plane blit at (x, y) from base + frame * 192. The
 * frame arithmetic is worth writing out because it does not look like a
 * multiply: image 0x04FC4 shifts the index into AH (`xchg al,ah` makes it
 * frame<<8) and then adds frame<<7 and frame<<6, which is frame * 192.
 *
 * So each one is on screen for 32 passes out of its reload period of 400 to
 * 560 - about seven per cent of the time. **That is why no capture shows one**,
 * and why this is transcribed rather than fitted: all four captures agree with
 * the port to the pixel inside all five boxes, which says the port was right
 * to draw nothing there, not that there is nothing to draw.
 *
 * Sprite offsets are `bp - 29200`: the table's bp is an offset in the segment
 * at [0x1FEE]+0x800, and 29200 is where section 4 begins in it - entry 0's bp
 * exactly, which is what makes the subtraction more than a fitted constant. */
#define ANIM_W      32
#define ANIM_H      12
#define ANIM_STRIDE 4
#define ANIM_PLANE  (ANIM_STRIDE * ANIM_H)    /* 48 */
#define ANIM_FRAME  (ANIM_PLANE * 4)          /* 192, and image 0x04FCB */
#define ANIM_FRAMES 8
#define ANIM_OFF    29200      /* table bp -> section 4 offset */
#define ANIM_N      MENU_ANIM_N
/* Image 0x04FA9 compares the counter against 0x8000. The counter is signed -
 * the routine's whole structure is "count down past zero" - so as an int16_t
 * that bit pattern is INT16_MIN, and writing it as 0x8000 overflows the
 * initialiser. Same sixteen bits, said in the type the value actually has. */
#define ANIM_IDLE   INT16_MIN  /* the original's 0x8000 */
#define ANIM_RESET  (-17)      /* image 0x04FB1 */

/* The table itself, image 0x0E3F0. Five live entries and five switched off;
 * the dead ones are kept because they are in the original and their positions
 * say where else it once put one. */
static const struct {
    int16_t  reload;
    uint16_t x, y;
    uint16_t bp;
} ANIM[ANIM_N] = {
    { 0x0190,  32,  49, 29200 },
    { 0x01B8, 256,  47, 30736 },
    { 0x01E0, 496,  40, 32272 },
    { 0x0208, 106, 132, 33808 },
    { 0x0230, 235, 132, 36880 },
    { ANIM_IDLE, 363, 132, 38416 },
    { ANIM_IDLE, 495, 130, 38416 },
    { ANIM_IDLE, 175, 207, 38416 },
    { ANIM_IDLE, 298, 209, 33808 },
    { ANIM_IDLE,  42,   9, 38416 },
};

/* Their counters - the original's own array, image 0x0E3F0, which is ds:0x4960
 * with eleven bytes an entry and the counter at +0. Image 0x04FF8 starts entry
 * i at 0x28 * (i + 1), so they do not all walk on at once.
 *
 * Not static, for the same reason menu_scroll is not: a capture of the
 * original shows ONE phase of these, and the port has to be told which. The
 * accessor is the port's own, so it lives in sdl_io.c. */
int16_t anim_ctr[ANIM_N];
int     anim_started;

/*
 * Transcribed from image 0x04FF8 - counter i starts at 0x28 * (i + 1) unless
 * the entry is switched off.
 */
void menu_anim_reset(void)
{
    anim_started = 1;
    for (uint8_t i = 0; i < ANIM_N; i++)
        anim_ctr[i] = (ANIM[i].reload == ANIM_IDLE)
                    ? ANIM_IDLE : (int16_t)(0x28 * (i + 1));
}

/* One pass of image 0x04F96, minus the drawing. Starts the counters if
 * nothing has yet: zero is a VALID counter value meaning "walk on next pass",
 * so leaving the array at its static zero would put all five on screen at
 * once on the first frame - which is the opposite of what the staggered
 * start at image 0x04FF8 is for. */
void menu_anim_step(void)
{
    if (!anim_started) menu_anim_reset();
    for (uint8_t i = 0; i < ANIM_N; i++) {
        if (anim_ctr[i] == ANIM_IDLE) continue;
        anim_ctr[i]--;
        if (anim_ctr[i] == ANIM_RESET) anim_ctr[i] = ANIM[i].reload;
    }
}


/* Draw whichever of them are on screen this pass. Keyed on 0, like the rest
 * of this screen: the sprites set plane 3 wherever they have ink, so their
 * own colours are 8..15 and 0 is genuinely nothing.
 *
 * Transcribed from image 0x04FD3: ax and bx come from the table entry, si is
 * the frame, di is si + 0x90 for the mask, and the blit is cx=0x40C (12
 * rows, 4 planes) by dx=0x20 (32 pixels).
 */
static void draw_anim(uint8_t *dst, const uint8_t *gfx, uint32_t len)
{
    if (!anim_started) menu_anim_reset();
    for (uint8_t i = 0; i < ANIM_N; i++) {
        int16_t c = anim_ctr[i];
        if (c == ANIM_IDLE || c >= 0) continue;
        uint8_t frame = (uint8_t)(((-c) - 1) >> 1);
        if (frame >= ANIM_FRAMES) continue;
        uint32_t g = (uint32_t)(ANIM[i].bp - ANIM_OFF)
                   + (uint32_t)frame * ANIM_FRAME;
        if (g + ANIM_FRAME > len) continue;
        for (uint16_t r = 0; r < ANIM_H; r++) {
            uint16_t y = (uint16_t)(ANIM[i].y + r);
            if (y >= MENU_H) break;
            for (uint16_t x2 = 0; x2 < ANIM_W; x2++) {
                uint16_t x = (uint16_t)(ANIM[i].x + x2);
                if (x >= MENU_W) break;
                /* Four planes of colour, drawn OPAQUELY - the sprite's
                 * own black included - exactly as the banner's end lemmings
                 * next door are.
                 *
                 * This used to key on `v != 0`, which is not the same thing
                 * and was never caught because no capture showed one of
                 * these: game.h said so in as many words. Driving the
                 * original to each rating puts entry 0 on screen at frames
                 * 4, 5 and 6, and against those three captures the four
                 * candidate rules score 14, 22, 22 and ZERO - opaque being
                 * the zero. Keying on non-zero costs the pixels where the
                 * sprite's own black should cover the logo.
                 *
                 * Image 0x04FE2 does load bp with 1 where image 0x07E65
                 * clears it, so the blitter is in a different mode here and
                 * image 0x04FDE hands it si+0x90 - three planes in - as
                 * well. Reading that as a mask and taking colour from three
                 * planes gets frame 1 to within 3 pixels and is still wrong:
                 * the body of these sprites is index 12, which no 3-plane
                 * colour can produce. What bp=1 and that pointer actually do
                 * has not been read; the compositing is measured. */
                dst[(size_t)y * MENU_W + x] =
                    planar(gfx, g, ANIM_PLANE, ANIM_STRIDE, 4, x2, r);
            }
        }
    }
}

/* THE TWO LEMMINGS HOLDING THE BANNER, and the bar's end caps.
 *
 * Found by hooking the shared blitter rather than by looking for a sprite:
 * tools/probe_banner_ends.py logs every call to image 0x8822 landing in the
 * banner's rows, and two of them are not the strip -
 *
 *     x=0    y=334  rows=16  planes=4  width=48
 *     x=592  y=334  rows=16  planes=4  width=48
 *
 * with the source in the segment at ds:0x1FE4, which image 0x04F96 sets to
 * [0x1FEE]+0x800. That resolves to section 4, and the sprites decode as a
 * lemming with THE BAR'S END CAP built into the same 48 pixels - dashed rows
 * and all. Which is why the painted bar runs 36..602 while the strip is only
 * 48..607: the caps are not the bar, they are part of these two sprites.
 *
 * 16 frames, 384 bytes each, left at 10752 and right at 10752 + 6144. Every
 * one of the four captures matches a frame EXACTLY, zero pixels differing, on
 * both sides, and in each the two are on the same frame index:
 *
 *     Fun frame 14   Tricky 13   Taxing 15   Mayhem 2
 *
 * THE FRAME FOLLOWS THE SCROLL, and this file said the opposite. The routine
 * is image 0x07E2F, found by having tools/probe_blits.py record the far
 * return address on the stack at the blitter's entry:
 *
 *     mov si, 0x1c10        the left sprite's base
 *     mov ax, [0x21]        the scroll offset
 *     shr ax,1 / shr ax,1   scroll / 4 - one frame per step
 *     and ax, 0x0f          sixteen of them
 *     xchg al,ah            frame * 256
 *     add si,ax
 *     shr ax,1 / add si,ax  + frame * 128, so si += frame * 384
 *     xor ax,ax / call      x = 0; the right one is si + 0x1800 and x = 0x250
 *     dx=0x30 cx=0x410 bx=0x14e     48 wide, 16 rows, 4 planes, y = 334
 *
 * so frame = (scroll >> 2) & 15. Every constant MEASURED here was right - the
 * 6,144 between the two sprites, the 384 a frame, the sixteen frames, both
 * positions - and the one thing FITTED was wrong. The fit said "no consistent
 * c, so they run on a counter of their own"; it never had the information, a
 * capture pins the scroll only modulo 16 and this needs it modulo 64.
 * Underdetermined was read as independent. */
#define LEM_W       48
#define LEM_H       16
#define LEM_STRIDE  6
#define LEM_PLANE   (LEM_STRIDE * LEM_H)   /* 96 */
#define LEM_FRAME   (LEM_PLANE * 4)        /* 384 */
#define LEM_FRAMES  16
#define LEM_L_BASE  10752
#define LEM_R_BASE  (LEM_L_BASE + LEM_FRAMES * LEM_FRAME)   /* 16896 */
#define LEM_L_X     0
#define LEM_R_X     592

/* The scroller's text, image 0x0E45E (ds:0x49CE), 266 bytes, terminated by
 * 0x0D. Transcribed here with its address for the same reason a routine is:
 * it is in the executable, not in a data file. 0xFF is the pause marker and
 * is not a glyph - see above - so it is stripped when the ribbon is built. */
/* THE RIBBON'S MESSAGE - transcribed DATA, image 0x0E45E.
 *
 * Which is ds:0x49CE: image 0x07CBC loads that address into ds:0x1D and
 * ds:0x1F, the pair the scroll step reads its next character through. All
 * 266 bytes are byte-identical to the image, and the byte after the final
 * `\xff ` is 0x0D, which is the next record starting.
 *
 * It lives in the executable's data segment, not in any DAT file, so the
 * port cannot load it the way it loads the artwork - it is copied here. */
static const char SCROLL_TEXT[] =
    "         Lemmings By DMA Design      \xff"
    "       Programming By Russell Kay    \xff"
    "       Animation By Gary Timmons     \xff"
    "       Graphics By Scott Johnston    \xff"
    " Music By Brian Johnston & Tim Wright       PC Music By Tony Williams     \xff"
    "      Copyright 1991 Psygnosis Ltd.  \xff ";

/* Where the ribbon has scrolled to, in pixels. The port composes ONE frame,
 * so this has to be a parameter of the composition rather than something the
 * drawing invents: two captures of the original taken seconds apart disagree
 * about it, and a fixed value is right only by luck. menu_set_scroll moves
 * it; the animation lives in whoever calls that. */
/* Which PC this is pretending to be - see lem_pc in game.h. High performance
 * by default, because that is the build the palette work above describes and
 * the one the start-up menu's second entry selects. */
lem_pc lem_machine = LEM_PC_HIGH;

uint32_t menu_scroll;
/* Which frame the two lemmings holding the banner are on. Separate from the
 * scroll because it is separate in the original - see draw_end_lemming. */
uint8_t  menu_lem_frame;


/* How many 16-pixel columns the strip holds. Image 0x07CBC stamps 0x23 of
 * them at start-up and image 0x07D54 shifts 0x46 bytes a row, which is the
 * same 35 columns counted the other way. */
#define BAR_COLS 35

/* One character of the ribbon: the text with the 0xFF pause markers removed,
 * cycling. `k` counts characters that have ENTERED at the right-hand edge, so
 * it is negative for a column nothing has reached yet - at start-up the strip
 * is 35 columns of bare template, and the text scrolls in from the right. That
 * is not a detail: composing at scroll 0 with the ribbon already in place puts
 * "Lemmings By DMA Design" on a bar the original still has empty. */
static char  ribbon[sizeof SCROLL_TEXT];
static uint8_t ribbon_pause[sizeof SCROLL_TEXT];  /* 0xFF came before this */
static uint16_t ribbon_len;

/*
 * The port's own INDEX over transcribed data - not a routine the original
 * has, and this used to be on the owed list by mistake.
 *
 * "Owed" means game logic the original has a routine for that nobody has
 * read. Image 0x07D33 has now been read: it does `lodsb`, compares against
 * 0xFF, and where it finds one it takes the pause and reads the next
 * character, all inline as it shifts the strip. There is no build step in
 * the original to transcribe, because the original never holds the message
 * as anything but a pointer walking forward.
 *
 * The port composes a whole frame instead of shifting a buffer, so it has to
 * be able to ask "which character is at position k" - hence one pass to
 * split SCROLL_TEXT into characters and their pause flags. The DATA is
 * transcribed (image 0x0E45E, byte-identical) and the RULE is image
 * 0x07D33's; only the once-instead-of-each-step is the port's.
 */
static void ribbon_build(void)
{
    if (ribbon_len) return;
    uint8_t pending = 0;
    for (size_t i = 0; i + 1 < sizeof SCROLL_TEXT; i++) {
        if ((uint8_t)SCROLL_TEXT[i] == 0xFF) { pending = 1; continue; }
        ribbon_pause[ribbon_len] = pending;
        ribbon[ribbon_len++] = SCROLL_TEXT[i];
        pending = 0;
    }
}

/*
 * Transcribed from image 0x07D33: 0xFF is consumed and sets the pause, 0x0D
 * rewinds to the start, and the character after either is the one drawn.
 */
static uint8_t scroll_char(int32_t k)
{
    ribbon_build();
    if (k < 0) return ' ';               /* nothing has scrolled in yet */
    return (uint8_t)ribbon[(uint32_t)k % ribbon_len];
}

/* Does a pause fall as character `k` enters? The original sets ds:0x23 = 100
 * when it reads 0xFF and then draws the character after it (image 0x07D3C),
 * so the hold belongs to the character that follows the marker. */
static int scroll_pauses_at(uint32_t k)
{
    ribbon_build();
    if (k < BAR_COLS) return 0;
    return ribbon_pause[(k - BAR_COLS) % ribbon_len];
}

/* Draw the banner with the ribbon scrolled `scroll` pixels to the left.
 *
 * The whole band is black underneath, all 640 columns: the original does not
 * let the background tile show either side of the bar, and letting it did cost
 * about 1,100 pixels on its own. */
/* Drawn OPAQUELY, including their black. That is measured, not assumed: with
 * every pixel compared, index 0 included, each capture matches its frame with
 * zero differing - so the sprite's own black is what the original leaves
 * there, and keying it out would be inventing transparency.
 *
 * Transcribed from image 0x07E2F. Its caller was found by having
 * tools/probe_blits.py record the far return address at the blitter's entry -
 * a pixel search had failed and a geometry fit had located the sprites
 * without ever finding the code.
 */
static void draw_end_lemming(uint8_t *dst, const uint8_t *gfx, uint32_t len,
                             uint32_t base, uint16_t at_x, uint8_t frame)
{
    uint32_t g = base + (uint32_t)(frame % LEM_FRAMES) * LEM_FRAME;
    if (g + LEM_FRAME > len) return;
    for (uint16_t r = 0; r < LEM_H; r++) {
        uint16_t y = (uint16_t)(BAR_Y0 + r);
        for (uint16_t c = 0; c < LEM_W; c++) {
            uint16_t x = (uint16_t)(at_x + c);
            if (x >= MENU_W) break;
            dst[(size_t)y * MENU_W + x] =
                planar(gfx, g, LEM_PLANE, LEM_STRIDE, 4, c, r);
        }
    }
}

/*
 * Transcribed from image 0x07CE6 (the strip blit, ax=0x30 bx=0x14E dx=0x230)
 * and image 0x07D54 (the four-bit shift). The tile is section 4 offset
 * 23040, the original's ds:0x4C10.
 */
void draw_banner(uint8_t *dst, const uint8_t *font, uint32_t font_len,
                 uint32_t scroll, uint8_t frame)
{
    for (uint16_t y = BAR_Y0; y <= BAR_Y1; y++)
        for (uint16_t x = 0; x < MENU_W; x++)
            dst[(size_t)y * MENU_W + x] = 0;

    for (uint16_t x = BAR_STRIP_X; x <= BAR_STRIP_X + 559; x++) {
        /* Ribbon pixel, measured from where the STRIP starts. */
        uint32_t r = scroll + (uint32_t)(x - BAR_STRIP_X);
        uint16_t cx = (uint16_t)(r % BAR_TILE_W);
        uint8_t ch = scroll_char((int32_t)(r / BAR_TILE_W) - BAR_COLS);
        for (uint16_t y = BAR_Y0; y <= BAR_Y1; y++) {
            uint16_t ty = (uint16_t)(y - BAR_Y0);
            uint8_t v = planar(font, BAR_TILE, BAR_TILE_PLANE, 2, 4, cx, ty);
            /* The glyph is masked over the tile, exactly as image 0x07DB7
             * does it: mask plane clear means the tile shows through. A space
             * has a glyph in the font but the original never draws it. */
            if (ch != ' ') {
                uint32_t g = FONT_BASE + (uint32_t)ch * FONT_GLYPH;
                if (ch <= 126 && g + FONT_GLYPH <= font_len) {
                    uint32_t o = g + (uint32_t)ty * 2 + (cx >> 3);
                    uint8_t sh = (uint8_t)(7 - (cx & 7));
                    if ((font[o + FONT_MASK_PLANE * FONT_PLANE] >> sh) & 1u)
                        v = (uint8_t)((1u << FONT_MASK_PLANE)
                                      | ((font[o] >> sh) & 1u)
                                      | (((font[o + FONT_PLANE] >> sh) & 1u) << 1));
                }
            }
            dst[(size_t)y * MENU_W + x] = v;
        }
    }

    /* Last, and over the strip: their caps are the bar's own ends. */
    draw_end_lemming(dst, font, font_len, LEM_L_BASE, LEM_L_X, frame);
    draw_end_lemming(dst, font, font_len, LEM_R_BASE, LEM_R_X, frame);
}


/* ======================================================================
 * THE THREE ROUTINES THAT COMPOSE THIS SCREEN - images 0x04EEF, 0x04F31 and
 * 0x04EC2, transcribed.
 *
 * They replace constants that agreed with the original to the pixel and got
 * its STRUCTURE wrong: six literal plate coordinates where the original runs
 * two loops, and arithmetic progressions where it indexes tables. Both
 * reproduced the screen exactly, which is why neither was ever questioned -
 * and one of them, the rating's, had already been got backwards once.
 *
 * The call sites came from tools/probe_blits.py recording the far return
 * address at the blitter's entry. Composing this screen from measured
 * geometry had never found a single entry point.
 * ====================================================================== */

/* The plate sprites, image CS:0x4F8A - six words the original reads with
 * `mov si, cs:[di]`. The port had SIGN_BASE + i * SIGN_STEP; that they happen
 * to be 3,660 apart is a fact about this data, not about how it is read. */
static const uint16_t SIGN_SPRITE[6] = {
    38024, 41684, 45344, 49004, 52664, 56324,
};

/* The four rating names, image CS:0x4EE7, indexed `shl bx,1` by ds:0x1F6E.
 * The stored words are 3,568 lower - the offset between section 4 and the
 * segment the original blits from - added once here rather than carried
 * through every constant.
 *
 * A table cannot be got backwards. The arithmetic version was, and stayed
 * wrong at three of the four ratings until one of them could be reached. */
static const uint16_t RATING_SPRITE[4] = {
    22516 + 3568, 21544 + 3568, 20572 + 3568, 19600 + 3568,
};

/* Image 0x04EEF: TWO BLITS WITH LITERAL POSITIONS - the shadow at (0,20) with
 * bp=0xF42, then the logo at (8,10) with bp=2. Not one position and a delta;
 * the original never computes the second from the first. */
static void draw_logo(uint8_t *dst, const uint8_t *sec)
{
    for (uint16_t sy = 0; sy < LOGO_H; sy++) {
        uint16_t y = (uint16_t)(20 + sy);
        if (y >= MENU_H) break;
        for (uint16_t sx = 0; sx < LOGO_W; sx++) {
            if (sx >= BG_DRAWN_W) break;
            if (planar(sec, LOGO_BASE, LOGO_PLANE, LOGO_STRIDE, 4, sx, sy) & 8)
                dst[(size_t)y * MENU_W + sx] = 0;
        }
    }
    blit(dst, sec, LOGO_BASE, LOGO_W, LOGO_H, LOGO_STRIDE, LOGO_PLANE, 8, 10);
}

/* Image 0x04F4A, entered twice from 0x04F31: four plates from (68,120) and
 * two from (196,197), each stepping `add ax, 0x80`. Per plate a shadow at
 * (x-8, y+8) with bp=0xF43, then the sprite with bp=3. */
static void draw_plate_row(uint8_t *dst, const uint8_t *sec, uint8_t first,
                           uint8_t count, uint16_t x, uint16_t y)
{
    for (uint8_t i = 0; i < count; i++) {
        uint32_t base = SIGN_SPRITE[first + i];
        for (uint16_t sy = 0; sy < SIGN_H; sy++) {
            uint16_t dy = (uint16_t)(y + 8 + sy);
            if (dy >= MENU_H) break;
            for (uint16_t sx = 0; sx < SIGN_W; sx++) {
                int32_t dx = (int32_t)x - 8 + sx;
                if (dx < 0 || dx >= BG_DRAWN_W) continue;
                if (planar(sec, base, SIGN_PLANE, SIGN_STRIDE, 4, sx, sy) & 8)
                    dst[(size_t)dy * MENU_W + (uint16_t)dx] = 0;
            }
        }
        blit(dst, sec, base, SIGN_W, SIGN_H, SIGN_STRIDE, SIGN_PLANE, x, y);
        x = (uint16_t)(x + 0x80);
    }
}

/* Image 0x04EC2: ax=0x1E6, bx=0x91, dx=0x48, cx=0x41B - (486,145), 72 wide,
 * 27 rows, four planes - from the table above. Opaque, because the name swaps
 * and drawing one has to erase the last. */
static void draw_rating(uint8_t *dst, const uint8_t *font, uint8_t rating)
{
    uint32_t base = RATING_SPRITE[rating & 3];
    for (uint16_t sy = 0; sy < RATING_H; sy++) {
        uint16_t y = (uint16_t)(RATING_Y + sy);
        if (y >= MENU_H) break;
        for (uint16_t sx = 0; sx < RATING_W; sx++) {
            uint16_t x = (uint16_t)(RATING_X + sx);
            if (x >= BG_DRAWN_W) break;
            dst[(size_t)y * MENU_W + x] =
                planar(font, base, RATING_PLANE, RATING_STRIDE, 4, sx, sy);
        }
    }
}


/*
 * Transcribed from image 0x04401 - the caller, which this file used to say
 * had not been found. It was found by scanning the image for a near `call`
 * whose target is 0x04EEF: exactly one, at image 0x04434, and image 0x04401
 * is the routine around it - the same address the code screen jumps back to.
 *
 * Its sequence, and what each call is:
 *
 *     0x04425  call 0x531E   palette to black, 16 entries from ds:0x100
 *     0x04428  call 0x4DEF   clear 0x6D60 bytes of video - 640x350/8
 *     0x0442B  call 0x5017   the background tile (tail-jumps to 0x7E76)
 *     0x0442E  call 0x7CBC   the banner: ds:0x1D and ds:0x1F to the message
 *                            at ds:0x49CE, ds:0x21 to 4, then the strip
 *     0x04431  call 0x4FF8   menu_anim_reset
 *     0x04434  call 0x4EEF   the logo
 *     0x04437  call 0x4F2B   the plate rows - four, through 0x04F4A
 *     0x0443A  si = 0x4AD9, call 0x4D4D    the two credit lines
 *     0x04441  call 0x4EA2   the rating plate; 0x04EC2 is its draw, and
 *                            0x04EA2 the wrapper that compares ds:0x1F6F
 *                            against ds:0x1F6E and only redraws on a change
 *     0x04444  call 0x4E37   the FX plate, the same shape on ds:0x1F72/0x1F70
 *     0x0444D  call 0x4C32   the CRTC start address
 *     0x04450  call 0x536C   palette on, 16 entries from ds:0x200
 *
 * This port composes one frame rather than driving a display, so it keeps the
 * DRAWING and drops the palette, the clear and the page registers - those are
 * in sdl_io.c or are not the port's to do. The animation is drawn last
 * because the original draws it in its LOOP (image 0x04F96, called at image
 * 0x04461) rather than in this build, and a single composed frame is the
 * build followed by one pass of that loop.
 *
 * The order here used to be measured. Putting it back to the original's is
 * byte-identical on all four ratings - 0 of 224,000 each - which is what
 * proves the pieces write disjoint regions, rather than it being assumed.
 */
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
        m->palette[i].r = widen6(MENU_PAL6[i * 3 + 0]);
        m->palette[i].g = widen6(MENU_PAL6[i * 3 + 1]);
        m->palette[i].b = widen6(MENU_PAL6[i * 3 + 2]);
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

    /* Image 0x07E2F derives the frame from the scroll; --lem-frame stays only
     * so one frame can be forced when comparing a single capture. */
    draw_banner(m->pix, font, fontlen, menu_scroll,
                menu_lem_frame ? menu_lem_frame
                : (uint8_t)(((((menu_scroll + BAR_SCROLL_INIT) >> 2)
                              - LEM_FRAME_BIAS)) & (LEM_FRAMES - 1)));

    draw_logo(m->pix, sec);                          /* image 0x04EEF */
    for (uint8_t r = 0; r < 2; r++)                  /* image 0x04F31 */
        draw_plate_row(m->pix, sec, SIGN_ROW[r].first, SIGN_ROW[r].count,
                       SIGN_ROW[r].x, SIGN_ROW[r].y);
    draw_text(m->pix, font, fontlen, CREDIT_1,
              CREDIT_1_CELL_X, CREDIT_1_CELL_Y);
    draw_text(m->pix, font, fontlen, CREDIT_2,
              CREDIT_2_CELL_X, CREDIT_2_CELL_Y);
    draw_rating(m->pix, font, rating);               /* image 0x04EC2 */

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


    draw_anim(m->pix, font, fontlen);

    /* Keep section 4 so the banner can be redrawn as it scrolls; menu_build
     * composes one frame and the strip moves. Failing to copy it is not fatal
     * - the screen is already complete - so the banner simply stays still. */
    m->gfx = malloc(fontlen);
    if (m->gfx) {
        memcpy(m->gfx, font, fontlen);
        m->gfx_len = fontlen;
    }

    dat_free(&d);
    return 0;
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

/*
 * Where plate i is, from the loop parameters at image 0x04F31 - four from
 * (68,120) and two from (196,197), each stepping 0x80. The original never
 * asks: its loop knows where it is. menu_hotspots needs it after the fact.
 */
static uint16_t sign_x(uint8_t i)
{
    const uint8_t r = (i < SIGN_ROW[0].count) ? 0 : 1;
    return (uint16_t)(SIGN_ROW[r].x
                      + (i - SIGN_ROW[r].first) * SIGN_STRIDE_X);
}

/*
 * As sign_x, from the same loop at image 0x04F31.
 */
static uint16_t sign_y(uint8_t i)
{
    return (i < SIGN_ROW[0].count) ? SIGN_ROW[0].y : SIGN_ROW[1].y;
}

/*
 * How many walk-on lemmings are on screen, read off the counters the table at
 * image 0x0E3F0 drives. Exposed because no capture of the original shows one,
 * so the logic at image 0x04F96 cannot be checked against pixels.
 */
uint8_t menu_anim_visible(void)
{
    if (!anim_started) menu_anim_reset();
    uint8_t n = 0;
    for (uint8_t i = 0; i < ANIM_N; i++)
        if (anim_ctr[i] != ANIM_IDLE && anim_ctr[i] < 0) n++;
    return n;
}

/* Transcribed from image 0x0447B for the KEYS; the boxes are the port's,
 * because the original ignores where a click lands. The keys are the plates' it draws
 * on its own screen - F1, F2, F3, ESC, F4 - so the mapping is evidence. What
 * each does beyond F1 and ESC is not reconstructed.
 *
 * Transcribed from image 0x0447B - the compare chain that turns a translated
 * key into a screen. The boxes are the port's, because the original ignores
 * where a click lands; the KEYS are its own.
 */
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
        spots[i].x = sign_x(i);
        spots[i].y = sign_y(i);
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
        spots[6 + i].x = sign_x(3);
        spots[6 + i].y = sign_y(3);
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

/* One frame of the banner. The original advances 4 pixels per pass of its
 * menu loop (image 0x07D54), and that loop waits on the display once per
 * pass, so a step per displayed frame is the reading of its structure. The
 * RATE is not measured - what is measured is the 4-pixel step and the
 * hundred-step pause - so this is marked as inferred rather than transcribed.
 *
 * The pause is honoured: 0xFF in the text stops the scroll for 100 steps, and
 * without it the credits run past too fast to read, which is visibly not what
 * the original does. */
#define BANNER_PAUSE 100

/* ONE PASS of the original's menu loop, image 0x07D0D: step the scroll four
 * pixels, hold for a hundred passes on a 0xFF marker, advance the walk-on
 * lemmings, and redraw the banner. The RATE is not transcribed - how often the
 * original takes a pass is inferred from its loop waiting on the display once
 * per pass - and neither is the loop itself, which is the port's and lives in
 * sdl_io.c. This is the game's half of it. */
int menu_frame_step(void *ctx, uint8_t *pix)
{
    menu_screen *m = (menu_screen *)ctx;
    static uint16_t pause;
    if (!m->gfx) return 0;
    if (pause) { pause--; return 0; }
    uint32_t next = menu_get_scroll() + MENU_SCROLL_STEP;
    /* A character boundary just crossed: if the one that entered was preceded
     * by a pause marker, hold. */
    if ((next % BAR_TILE_W) == 0 && scroll_pauses_at(next / BAR_TILE_W))
        pause = BANNER_PAUSE;
    menu_set_scroll(next);
    menu_anim_step();
    draw_anim(pix, m->gfx, m->gfx_len);
    draw_banner(pix, m->gfx, m->gfx_len, next,
                (uint8_t)(((((next + BAR_SCROLL_INIT) >> 2)
                            - LEM_FRAME_BIAS)) & (LEM_FRAMES - 1)));
    return 1;
}

