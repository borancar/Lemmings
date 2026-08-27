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


/* The port's own arrangement, and image 0x01D2E is what it replaces - read
 * rather than left as a debt.
 *
 * That routine is pure DOS: INT 21h AH=23h for the file's size through an
 * FCB, the size shifted into paragraphs and checked against what is left at
 * ds:0x1F90, then INT 21h AH=3Fh reading into a bump allocator at ds:0x1F8A
 * which it advances past a 13-byte section header each time (image 0x01DA6's
 * `add dx, 0xd`). It streams the sections one after another into a single
 * buffer and never holds the whole file.
 *
 * So there is no container-parsing routine in the original to transcribe -
 * the reading and the walking are the same loop, and the reading is DOS's.
 * The port reads the file whole through io_read_file, which is IO and lives
 * in sdl_io.c for the reason that file's own note gives, and what stays here
 * is the part that IS the game's: the section headers and the decompressor,
 * which tools/verify_dat.py checks against the original's own body. */
int dat_load(const char *path, dat_file *out, const char **err)
{
    memset(out, 0, sizeof(*out));

    uint8_t *blob = NULL;
    long len = 0;
    if (io_read_file(path, &blob, &len, err) != 0)
        return -1;

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
        /* THE THREE FLAGS, read out of image 0x07C67 rather than taken
         * from the published description of the format - which is what they
         * rested on until now, and this project's note about inherited
         * claims says those are a different kind of knowledge.
         *
         * The blitter is handed the same two words and unpacks them into its
         * own flag word: image 0x07C6C shifts AX right by seven and keeps
         * 0xC0, which puts AX bit 14 in bit 7 and AX bit 13 in bit 6; image
         * 0x07C75 tests that bit 6 and ORs in 0xF00; and image 0x07C7F
         * rotates AX left so bit 15 falls into carry and ORs in 0x8000. So
         * bit 15 is no-overwrite - the same bit level_anim_draw already knew
         * as bp bit 15, "not drawn where there is terrain" - bit 14 upside
         * down, bit 13 erase.
         *
         * The X and Y arithmetic agrees to the instruction too: image
         * 0x07C95's `and ax, 0x1fff` is this line's 0x1F<<8, and image
         * 0x07C90's `sar bx, 7` on the signed word is exactly sign9 of the
         * top nine bits. */
        t->no_overwrite = (uint8_t)((e[0] & 0x80) != 0);   /* image 0x07C7F */
        t->upside_down  = (uint8_t)((e[0] & 0x40) != 0);   /* image 0x07C6C */
        t->erase        = (uint8_t)((e[0] & 0x20) != 0);   /* image 0x07C75 */
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

/* NOT TRANSCRIBED, but PROVEN equivalent - and the boundary is now narrower
 * than it was. The original's loop is at image 0x03752..0x03768: four bytes
 * at a time from a table at ds:0x10EB, stopping on the first WORD being
 * 0xFFFF, each entry handed to a blitter at image 0x07C67.
 *
 * That blitter's UNPACKING is transcribed now - level_parse's terrain entries
 * carry its addresses, and the x, the y and all three flags agree with it to
 * the instruction. What is still the port's own is the COMPOSITION below:
 * the original programs the card and blits a byte at a time, and this walks
 * pixels. tools/prove.sh compares against what the original composes: 49,741
 * pixels, zero differing. */
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
                if (g_terrain_mode == TERRAIN_OR_ALL)
                    *dst = (uint8_t)(*dst | v);
                /* NO-OVERWRITE IS TESTED BEFORE ERASE, and a piece can carry
                 * both. Image 0x07C67 unpacks the LVL's first word into the
                 * blitter's flag word, and the erase bit - e[0] & 0x20, which
                 * is word bit 13 - sets TWO things there: `and dx, 0xc0` puts
                 * it in bp bit 6, and image 0x07C7B ORs in 0x0F00 beside it.
                 * Image 0x08905 then tests bp bit 6 and jumps to image
                 * 0x0909C, where image 0x090B8 sets Set/Reset to 0 and image
                 * 0x090BF sets Enable Set/Reset to bp bits 11..8 - the 0x0F -
                 * so all four planes are written as zero. That is the erase.
                 *
                 * But image 0x088F3 tests bp bit 15 first and jumps to image
                 * 0x08B42, an ordinary masked draw that never looks at the
                 * erase nibble. So a piece with BOTH flags is drawn and not
                 * erased, and the order of these two branches is that
                 * dispatch order rather than a preference.
                 *
                 * level006 section 3 has three such pieces and the port
                 * erased all of them: 814 pixels of terrain missing from the
                 * bottom of the play area, on every one of 1,100 frames. Two
                 * of the three carry erase alone and DO erase; the third
                 * carries both. Suppressing erase for all three left 459
                 * pixels wrong the other way, which is how the difference
                 * between the two pieces was found - by measuring a wrong
                 * rule, then going back and reading the unpack. */
                else if (t->no_overwrite
                         && g_terrain_mode != TERRAIN_NOV_IGNORE) {
                    if (g_terrain_mode == TERRAIN_NOV_INVERT) {
                        if (*dst != 0)
                            *dst = v;
                    } else if (*dst == 0)
                        *dst = v;
                } else if (t->erase)       /* image 0x08905, image 0x090BF */
                    *dst = 0;
                else switch (g_terrain_mode) {
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
/* THE SKILL COUNTS AS THEY ARE SPENT - ds:0x6A, and the stride is THREE.
 *
 * Image 0x03180 dispatches a button to its handler and each one opens by
 * testing its own count: image 0x0329B reads ds:0x6A, image 0x03557 ds:0x6D,
 * then 0x70, 0x73, 0x76, 0x79, 0x7C and image 0x0320D ds:0x7F for the
 * digger. Three bytes apart, not one - there are two more bytes a skill that
 * this port has not needed to name.
 *
 * The panel used to draw the LEVEL HEADER's numbers, which never change, so
 * a digger could be spent and the button went on saying ten. Nothing could
 * see it: a level nobody plays never spends one, and the recorded runs every
 * frame-by-frame figure came from were exactly that. tools/compare_play.py
 * drove one click and it showed up as the wrong glyph on button nine. */
static uint8_t g_skills[8];        /* buttons 2..9 */

void panel_buttons(uint8_t *dst, uint16_t dst_w, uint16_t dst_h,
                   const uint8_t *sec, uint32_t sec_len, const level *lv)
{
    uint8_t v[PANEL_BUTTONS];
    /* TWO DIFFERENT NUMBERS: the level's own rate, which is the floor the
     * minus stops at (ds:0x64), and the rate in force (ds:0x67). They start
     * equal, which is why one value stood in for both until the buttons
     * worked. */
    v[0] = level_rate_floor();                  /* ds:0x64 */
    v[1] = level_rate();                        /* ds:0x67 */
    /* What is LEFT, not what the header stocked - see g_skills. */
    for (uint8_t k = 0; k < 8; k++)
        v[2 + k] = g_skills[k];
    (void)lv;
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

/* Image 0x0418 and 0x041D: ds:0x85 and ds:0x86 both start at 0x11. The same
 * number paces the display - see LEVEL_FPS in game.h. */
#define LEVEL_FRAMES_PER_SECOND LEVEL_FPS

/* Image 0x0418 and 0x041D set ds:0x85 and ds:0x86 to 0x11 - seventeen frames
 * to a second - and image 0x040E and 0x0413 put '0' in both seconds cells.
 * Confirmed against the running game: the line goes from TIME 4:52 at frame
 * 151 to TIME 4:21 at frame 676, which is 525 frames for 31 seconds. */
/* Image 0x0320D and its seven siblings read these; the level header stocks
 * them. Reset here because this already runs once when a level starts, and
 * two places that both have to be remembered is one place too many. */
void level_skills_reset(const level *lv)
{
    for (uint8_t k = 0; k < 8; k++) g_skills[k] = (uint8_t)lv->skills[k];

    /* THE SAME POKE tools/record_level.py --skills applies to the guest.
     *
     * "Just dig!" stocks nothing but diggers, so seven of the eight skills
     * can never be assigned by playing it - and a routine no run reaches is
     * one the port can have wrong for ever. This stocks them so both sides
     * can be driven through the same assignment and compared.
     *
     * It is a POKE and belongs with the rest of the hardware's conveniences,
     * not with the game: it changes what the level offers, never how a skill
     * behaves. The game has no command line, so it is env-gated the way
     * LEM_FRAMES and LEM_SCREEN_DIR are. */
    const char *stock = getenv("LEM_SKILLS");
    if (stock) {
        uint8_t n = (uint8_t)atoi(stock);
        for (uint8_t k = 0; k < 8; k++)
            if (!g_skills[k]) g_skills[k] = n;
    }
}

/* Image 0x0320D's own test, `cmp byte [0x7f], 0`, and the same opening in
 * each of the seven siblings. */
uint8_t level_skill_left(uint8_t button)
{
    return (button >= 2 && button < 10) ? g_skills[button - 2] : 0;
}

/* Image 0x03224: `dec byte [0x7f]`, and the same in each sibling. */
void level_skill_spend(uint8_t button)
{
    if (button >= 2 && button < 10 && g_skills[button - 2])
        g_skills[button - 2]--;
}

/* Image 0x0418 and image 0x041D set the tick and its reload; image 0x040E
 * and image 0x0413 put '0' in both seconds cells. */
void level_status_reset(const level *lv)
{
    level_skills_reset(lv);
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
    /* AND THE 'IN' PERCENTAGE IS WRITTEN BEFORE THE LOOP RUNS. Image 0x03AC8
     * has two callers: image 0x001C2, inside the play loop, and image
     * 0x0049E, in the level's set-up - after image 0x00495 draws and image
     * 0x00498 puts the objects up, and before the loop is entered at all.
     *
     * So the original's very first frame already carries it, and the port's
     * did not: level_frame_step has it, and frame 0 is the level before
     * anything steps. Fifty pixels of panel on one frame of 1,100.
     *
     * Nothing is saved yet, so this is level_saved_percent's own answer at
     * the start rather than a hard zero - the routine is the game's and the
     * number should come from it. */
    level_status_in(level_saved_percent(lv));   /* image 0x0049E */
}

/* THE NAMES, transcribed from image 0x0E34F (ds:0x48BF): twelve of seven
 * characters, padded with spaces, in the order the ladder at image 0x03DFA
 * indexes them. */
#define ST_NAME_LEN   7
static const char STATUS_NAME[12][ST_NAME_LEN + 1] = {
    "CLIMBER", "FLOATER", "BOMBER ", "BLOCKER", "BUILDER", "BASHER ",
    "MINER  ", "DIGGER ", "WALKER ", "FALLER ", "SPLATTR", "ATHLETE",
};
enum { NM_CLIMBER, NM_FLOATER, NM_BOMBER, NM_BLOCKER, NM_BUILDER, NM_BASHER,
       NM_MINER, NM_DIGGER, NM_WALKER, NM_FALLER, NM_SPLATTR, NM_ATHLETE };

#define ST_NAME   4       /* ds:0x2083, the line's base being ds:0x207F */
#define ST_NUM    12      /* ds:0x208B */
#define ST_BLANK  11      /* image 0x03CEC: eleven cells, ah = 0x0B */

/* Transcribed from image 0x04B68: up to three decimal digits, LEFT aligned,
 * with leading zeros dropped rather than printed.
 *
 * It is not digits3 at image 0x04BC2, which pads with zeros and always fills
 * three cells. This one writes three spaces first and then only advances the
 * destination for a digit it actually emitted, so 7 comes out as "7  " and
 * not "007" - and the panel shows a bare number beside the name because of
 * it. */
/* Transcribed from image 0x04BBE, which is two instructions: clear the
 * digit-emitted flag and jump into image 0x04B91 - put_num3's TENS stage. So
 * it is the same writer stopped one digit short: at most two, left aligned,
 * with a leading zero coming out as a space. */
static char *put_num2(char *dst, uint8_t v)
{
    dst[0] = dst[1] = ' ';                /* image 0x04B91 */
    uint8_t d = 0;
    while (v >= 10) { v = (uint8_t)(v - 10); d++; }   /* image 0x04B9A */
    if (d) *dst++ = (char)('0' + d);      /* image 0x04BB0 */
    *dst = (char)('0' + v);               /* image 0x04BB8 */
    return dst + 1;
}

/* Transcribed from image 0x04B68: up to three decimal digits, LEFT aligned,
 * with leading zeros dropped rather than printed. Not digits3 at image
 * 0x04BC2, which pads with zeros and always fills three cells - this one
 * writes three spaces first and only advances the destination for a digit it
 * actually emitted, so 7 comes out as "7  " and not "007". */
static char *put_num3(char *dst, uint8_t v)
{
    dst[0] = dst[1] = dst[2] = ' ';       /* image 0x04B68 */
    uint8_t any = 0;                      /* bl */
    uint8_t d = 0;

    while (v >= 100) { v = (uint8_t)(v - 100); d++; }   /* image 0x04B77 */
    if (d) { *dst++ = (char)('0' + d); any = 1; }       /* image 0x04B89 */

    dst[0] = dst[1] = ' ';                /* image 0x04B91 */
    d = 0;
    while (v >= 10) { v = (uint8_t)(v - 10); d++; }     /* image 0x04B9A */
    if (d || any) { *dst++ = (char)('0' + d); }         /* image 0x04BB0 */

    *dst = (char)('0' + v);               /* image 0x04BB8 */
    return dst + 1;                       /* image 0x04BBC's `inc si` */
}

/* WHAT THE POINTER IS OVER, in the panel's own line - image 0x03CE9.
 *
 * Every frame the game blanks eleven cells from ds:0x2083 (image 0x03CEC),
 * runs the scan, and if it chose a lemming writes that lemming's NAME at the
 * same place and how many are under the pointer at ds:0x208B. That is the
 * "DIGGER 2" the original shows and this port showed nothing for.
 *
 * Nothing in the frame-by-frame comparison could ever have caught it. The
 * recorded runs it scores against were made with the pointer parked in open
 * sky, so the original wrote spaces there too and the two agreed on every
 * frame of five minutes. It took driving BOTH sides from one input script -
 * tools/compare_play.py - and it showed up immediately as 342 differing
 * pixels in panel rows 164..174.
 *
 * TWO BRANCHES OF THE LADDER ARE NOT REACHABLE HERE and say so rather than
 * being guessed: image 0x03DFD and image 0x03E0E read record bytes 0x24 and
 * 0x25, which are the climber and floater abilities, and this port's lemming
 * struct does not carry them. A lemming with either would be named WALKER by
 * the code below. Neither is obtainable in the levels the port reaches, since
 * "Just dig!" hands out nothing but diggers.
 */
void level_status_pointer(const lemming *lem, uint8_t n, uint16_t view_x)
{
    /* Image 0x03CE9: eleven spaces, every frame, before anything else. */
    for (uint8_t i = 0; i < ST_BLANK; i++) g_status[ST_NAME + i] = ' ';

    level_scan sc;
    level_pointer_scan(lem, n, view_x, &sc);
    if (sc.chosen < 0) return;                       /* image 0x03DE5 */

    /* Image 0x03DED: the count first, then the name over the top of the
     * cells the blanking just cleared. */
    put_num3(&g_status[ST_NUM], sc.count);

    const lemming *a = &lem[sc.chosen];
    uint8_t nm;
    /* THE LADDER IN FULL, image 0x03DFA. Its first three rungs read the two
     * ABILITY bytes rather than the state word, and they come FIRST - so a
     * floater is called a FLOATER whatever it happens to be doing, and a
     * climber a CLIMBER. The port used to start at image 0x03E17 and call
     * both of them WALKER, which is what a player saw. They were left out
     * when the lemming record carried neither byte; it carries both now. */
    if      (a->extra & 0x80)   nm = NM_BUILDER;     /* image 0x03E00 */
    else if (a->extra & 1)                           /* image 0x03E07 */
        nm = a->floater ? NM_ATHLETE : NM_CLIMBER;   /* image 0x03E63 */
    else if (a->floater)        nm = NM_FLOATER;     /* image 0x03E0E */
    else if (a->flags & 0x0100) nm = NM_BLOCKER;     /* image 0x03E17 */
    else if (a->flags & 0x0080) nm = NM_BUILDER;     /* image 0x03E21 */
    else if (a->flags & 0x0200) nm = NM_BASHER;      /* image 0x03E2B */
    else if (a->flags & 0x0800) nm = NM_MINER;       /* image 0x03E35 */
    else if (a->flags & 0x0010) nm = NM_DIGGER;      /* image 0x03E3F */
    /* NOTHING NAMES A SPLAT OR A BOMBER. There is no rung for 0x0001 or for
     * a lit fuse, so a lemming on its way to the ground reads WALKER in the
     * original too - SPLATTR is in the name table at ds:0x4905 and this
     * ladder never reaches it.
     *
     * FOUR, not 0x400. The encoding is `f7 44 08 04 00` - the immediate is
     * the last two bytes, little-endian - and reading the hex column left to
     * right gives 0x0400, which is what went in here first. The original
     * named a lemming FALLER where the port said WALKER, and one look at the
     * two words settled it; nothing else could have, because a level nobody
     * plays never shows either. */
    else if (a->flags & 0x0004) nm = NM_FALLER;      /* image 0x03E49 */
    else                        nm = NM_WALKER;      /* image 0x03E50 */

    for (uint8_t i = 0; i < ST_NAME_LEN; i++)        /* image 0x03E53 */
        g_status[ST_NAME + i] = STATUS_NAME[nm][i];
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
    if (level_paused() == 0xFF) return;    /* image 0x0020C */
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
/* The array level_anim_init fills, kept so the trigger dispatch can reach an
 * object's record - see level_object_trigger. */
static level_anim *g_anim;

/* THE PORT'S OWN: it copies GROUNDxO.DAT's object records into a form
 * level_anim_step can walk. The original keeps them in the file's own layout
 * and reads the same fields in place - image 0x07C1E. */
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
    /* So the trigger dispatch can reach these - see level_object_trigger. */
    g_anim = st;
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
static uint8_t g_rate;           /* ds:0x67 - the rate the buttons move */
static uint8_t g_rate_min;       /* ds:0x64 - the level's own, and the floor */
static uint8_t g_released;       /* ds:0x43 */
static uint8_t g_to_release;     /* ds:0x47 */

/* THE PORT'S OWN, only because ds:0x43 is a byte of the data segment and
 * anything in the original can read it. */
uint8_t level_released(void) { return g_released; }
/* ds:0x47, and a VARIABLE - the nuke writes to it. */
/* ds:0x47, and a VARIABLE - the nuke at image 0x04013 writes to it. */
uint8_t level_to_release(void) { return g_to_release; }

/* ======================================================================
 * THE NUKE - panel button 11, image 0x03FEF.
 *
 * It is a DOUBLE CLICK, and that is the whole of why one press appears to
 * do nothing: image 0x03FFB reads the frame counter at ds:0x1FD3, image
 * 0x04002 stores it as the last press, and image 0x04007 gives up unless the
 * two presses were within FIVE frames of each other. At the level's
 * seventeen a second that is a third of a second.
 *
 * ds:0x1FD3 is bumped once a frame at image 0x00199 - the game's own
 * end-of-frame, the same instruction tools/record_level.py captures at - so
 * the window is five GAME frames and not five of anything the host counts.
 * ====================================================================== */
/* ds:0x1FD3. NOT g_tick - that name was already taken further up this file
 * by ds:0x85, the clock's frames-per-second counter, and a second
 * `static uint8_t g_tick;` in the same file is not an error in C: two
 * tentative definitions MERGE into one variable. It compiled without a
 * warning and the nuke's clock was the game's seconds counter, which the
 * timer reloads to 17 every second - so the double click measured its gap
 * against a byte that never went above 19, and the port nuked or refused to
 * depending on where in the second the press landed. */
static uint8_t g_nuke_tick;      /* ds:0x1FD3, image 0x00199 */
static uint8_t g_nuke;           /* ds:0x4B */
static uint8_t g_nuke_at;        /* ds:0x4C, the cascade's cursor */
static uint8_t g_nuke_last;      /* ds:0x1FD4, the previous press */

void level_tick(void) { g_nuke_tick++; }         /* image 0x00199 */

/* ======================================================================
 * PAUSE - panel button 10, image 0x04018.
 *
 * Not a button that does something so much as a FLAG the frame reads in
 * eight places. Image 0x04028 toggles ds:0x2B with a debounce of two frames
 * on ds:0x52, and everything else asks it:
 *
 *     image 0x0020C  the clock does not tick
 *     image 0x01F19  no lemming is stepped - and so the nuke's cascade,
 *                    which is that loop's tail, does not run either
 *     image 0x030A6  nobody comes out of the entrance
 *     image 0x03150  a click on a lemming assigns nothing
 *     image 0x03944  the panel is not redrawn
 *     image 0x03F52  a click on the panel selects no skill
 *     image 0x03FEF  the nuke refuses
 *     image 0x04032  the release rate refuses
 *
 * NOTHING RESETS ds:0x2B or ds:0x53. Leaving a level paused leaves the game
 * paused, and the first press of the next level measures its debounce
 * against a stale ds:0x53 - the original's behaviour, transcribed rather
 * than tidied.
 * ====================================================================== */
static uint8_t g_paused;         /* ds:0x2B */
static uint8_t g_pause_at;       /* ds:0x53 */

uint8_t level_paused(void) { return g_paused; }

/* Image 0x04018. Two frames must have passed since the last toggle, which is
 * what stops a held button flickering the game on and off. */
int level_pause_click(void)
{
    uint8_t al = level_frame_counter();           /* image 0x04018 */
    if ((uint8_t)(al - g_pause_at) < 2) return 0; /* image 0x04021 */
    g_paused ^= 0xFF;                             /* image 0x04028 */
    g_pause_at = al;                              /* image 0x0402D */
    return 1;
}
/* THE PORT'S OWN, only because ds:0x1FD3 is a byte of the data segment and
 * a harness may want to see what the double click is measuring against. */
uint8_t level_tick_now(void) { return g_nuke_tick; }
/* THE PORT'S OWN: ds:0x4B read back, for anything that wants to ask. */
uint8_t level_nuking(void) { return g_nuke; }

/* Image 0x03FEF. Returns whether this press was the second of a pair. */
int level_nuke_click(void)
{
    if (g_paused == 0xFF) return 0;              /* image 0x03FEF */
    uint8_t was = g_nuke_last;
    g_nuke_last = g_nuke_tick;                   /* image 0x04002 */
    /* Image 0x04005 subtracts in AL and image 0x04007 is a SIGNED `jg`, so
     * a counter that has wrapped between the two presses reads as negative
     * and is accepted rather than rejected. */
    if ((int8_t)(uint8_t)(g_nuke_tick - was) > 5) return 0;
    g_nuke = 0xFF;                               /* image 0x0400B */
    /* AND NO MORE COME OUT - image 0x04010 copies the released count over
     * the level's total, so the entrance stops and image 0x001EA's
     * "everyone released and none alive" can be reached at all. */
    g_to_release = g_released;                   /* image 0x04013 */
    return 1;
}

/* Image 0x01FF8, and it is the TAIL OF THE WALK LOOP rather than a routine
 * of its own: image 0x01FF3's `dec [0x42] / je` falls into it once every
 * lemming has been stepped. So it runs at most once a frame, after the
 * simulation, and not at all while the game is paused - image 0x01F20 jumps
 * past it to image 0x01FFF.
 *
 * ONE LEMMING A FRAME gets its fuse lit, which is what makes the nuke a
 * cascade of little explosions rather than one bang. A DEAD one costs no
 * frame at all: image 0x02026 bumps the cursor and jumps back to image
 * 0x02005 to try the next, in the same pass. */
static void level_nuke_step(lemming *lem)
{
    if (!g_nuke) return;                         /* image 0x01FF8 */
    for (;;) {
        if (g_nuke_at == g_released) {           /* image 0x02008 */
            g_nuke = 0;                          /* image 0x0203C */
            return;
        }
        if (g_nuke_at >= LEMMING_MAX) { g_nuke = 0; return; }
        lemming *a = &lem[g_nuke_at];            /* image 0x02015 */
        if (a->fuse) { g_nuke_at++; return; }    /* image 0x0201A */
        if (a->alive == 0xFF) {                  /* image 0x02020 */
            g_nuke_at++;                         /* image 0x02026 */
            continue;                            /* image 0x0202A: no frame */
        }
        if (a->flags & 3) { g_nuke_at++; return; }   /* image 0x0202C */
        /* Image 0x02033 writes 0x4F itself - the same fuse image 0x031E1
         * gives a bomber, written out again at its own address rather than
         * borrowed from the bomber's constant, which is defined further
         * down beside the routine that uses it. */
        a->fuse = 0x4F;                          /* image 0x02033 */
        g_nuke_at++;                             /* image 0x02037 */
        return;
    }
}

/* Image 0x39D1 sets the countdown and image 0x39D6 the interval; both run
 * when the level is set up. */
/* Image 0x04053, the interval the release counts down from. Written out as
 * the original does it, four negations and all:
 *
 *     al = 0x63 - ds:0x67 ; al >>= 1 ; al = -al
 *     al += 0x35          ; al = -al ; al += 0x39
 *
 * which comes to ((99 - rate) >> 1) + 4 - so rate 99 releases every fourth
 * frame and rate 1 every fifty-third. */
static uint8_t level_rate_step(uint8_t rate)
{
    return (uint8_t)(((99u - (uint16_t)rate) >> 1) + 4u);
}

static uint8_t g_rate_div;       /* ds:0x69 */
/* ds:0x68 - the rate the PANEL is showing, which image 0x03B86 compares with
 * ds:0x67 to decide whether anything needs redrawing. Modelled as a flag
 * rather than a value because only "does it match" is ever asked. */
static uint8_t g_rate_shown = 1;

/* THE RELEASE-RATE BUTTONS, image 0x04032. `up` is non-zero for the plus.
 *
 * These had done nothing at all, because image 0x03F27 was read as refusing
 * every button below 2. It does the opposite: `cmp al,2 / jge 0x3f2e / jmp
 * 0x4032` sends 0 and 1 HERE, and it is the buttons from 2 up that go through
 * the latch at image 0x03F2E. A player found it - the panel's minus and plus
 * moved nothing.
 *
 * Image 0x04046 stops the plus at 99 and image 0x04076 stops the minus at
 * ds:0x64, the level's own rate: a level can be sped up and never slowed
 * below what its header asked for.
 *
 * One thing here is named rather than transcribed: ds:0x69, tested at image
 * 0x0403D and image 0x0406C. It is a divider of exactly the shape
 * ds:0x84 has for the skill strip - image 0x03B8C catches it at 2, sets it
 * 0xFF, and the `inc` at image 0x03B9C wraps it to zero - so a HELD button
 * steps the rate one frame in three. The
 * divider is tested HERE, where image 0x0403D and image 0x0406C test it, and
 * turned by level_rate_tick below from the panel's redraw. sdl_io.c calls
 * this once on the press and once a frame while the button stays down, which
 * is what the original's own poll amounts to. */
uint8_t level_rate_click(uint8_t up)
{
    if (level_paused() == 0xFF) return 0;       /* image 0x04032 */
    /* THE DIVIDER GATES THE PRESS ITSELF, and this used to be outside the
     * routine. Image 0x0403D tests ds:0x69 for the plus and image 0x0406C for
     * the minus, BEFORE either bound is looked at - so a press while the
     * divider is turning does nothing at all.
     *
     * The port had the test in level_rate_held instead, so the press arrived
     * as an edge and stepped once for free, and then the very next frame the
     * held path found the divider at rest and stepped AGAIN. One click moved
     * the rate by two where the original moves it by one. A player noticed
     * the amount was wrong. */
    if (g_rate_div != 0) return 0;              /* 0x0403D, 0x0406C */
    if (up) {
        if (g_rate >= 99) return 0;             /* image 0x04046 */
        g_rate++;                               /* image 0x0404F */
    } else {
        if (g_rate <= g_rate_min) return 0;     /* image 0x04076 */
        g_rate--;                               /* image 0x0407F */
    }
    g_release_step = level_rate_step(g_rate);   /* image 0x04053 */
    /* The number on the panel is now stale, which is what starts the divider
     * turning - see level_rate_tick. */
    g_rate_shown = 0;                           /* ds:0x68 != ds:0x67 */
    return 1;
}

/* ds:0x69, image 0x0403D - the divider that paces a HELD rate button.
 *
 * Image 0x03B8C runs it from the panel's own per-frame update in exactly the
 * shape ds:0x84 has for the skill strip: caught at 2 it is put to 0xFF and
 * the `inc` at image 0x03B9C wraps it to zero. Image 0x0403D then refuses a
 * click whenever it is not zero, so a button held down steps the rate one
 * frame in three rather than every frame.
 *
 * The port reaches the same pace by calling level_rate_click once a frame
 * while the button is down and letting that routine's own test of ds:0x69
 * refuse it - which is what the original's poll does. */

/* ONE FRAME OF THE DIVIDER - image 0x03B8C, and it is part of the panel's
 * redraw rather than of the button.
 *
 * Image 0x03B86 compares ds:0x68, the value the panel currently SHOWS, with
 * ds:0x67, the rate in force, and skips the whole block when they agree. So
 * the divider only turns while the drawn number is stale: image 0x03B8C
 * catches it at 2, image 0x03B93 marks the number drawn, image 0x03B97 puts
 * the divider to 0xFF and image 0x03B9C's `inc` wraps it to zero.
 *
 * The effect is a rate button that steps once and then waits three frames,
 * because image 0x0403D refuses every press until the divider comes back to
 * zero - and it only comes back once the panel has caught up.
 *
 * At rest the divider is zero and the number is drawn, so the FIRST press of
 * a button always takes. */
void level_rate_tick(void)
{
    if (level_paused() == 0xFF) return;         /* image 0x03944 */
    if (g_rate_shown) return;                   /* image 0x03B86 */
    if (g_rate_div == 2) {                      /* image 0x03B8C */
        g_rate_shown = 1;                       /* image 0x03B93 */
        g_rate_div = 0xFF;                      /* image 0x03B97 */
    }
    g_rate_div++;                               /* image 0x03B9C */
}

/* Image 0x039EE clears ds:0x69 when the level starts. */
void level_rate_div_reset(void) { g_rate_div = 0; g_rate_shown = 1; }

/* ds:0x67 and ds:0x64 - the rate in force and the level's own, which the
 * panel draws as its first two numbers (image 0x036C1, image 0x036C4). */
uint8_t level_rate(void)       { return g_rate; }
/* ds:0x49, the interval the release counts down from - image 0x04053. */
uint8_t level_release_interval(void) { return g_release_step; }
uint8_t level_rate_floor(void) { return g_rate_min; }

/* The level's release set up, image 0x036B9 onward: the header's rate into
 * both ds:0x64 and ds:0x67, the counter's start at image 0x39D1, and the
 * lemming total. */
void level_release_reset(const level *lv)
{
    g_release_wait = 0x14;                      /* image 0x39D1 */
    /* THE LEVEL'S RATE IS A FLOOR, NOT A FIXTURE. Image 0x036C1 puts the
     * header's rate in ds:0x64 and image 0x036C4 puts the SAME value in
     * ds:0x67, and only the second moves: the panel's two buttons walk
     * ds:0x67 between ds:0x64 and 99. */
    g_rate_div = 0;                             /* image 0x039EE */
    g_rate_min = (uint8_t)lv->release_rate;     /* image 0x036C1 */
    g_rate     = (uint8_t)lv->release_rate;     /* image 0x036C4 */
    g_release_step = level_rate_step(g_rate);
    g_released     = 0;
    g_to_release   = (uint8_t)lv->num_lemmings;
    /* Image 0x00436..0x0043B: the level's set-up zeroes ds:0x1FD3 and
     * ds:0x1FD4 together, and NOTHING resets ds:0x4B or ds:0x4C - the
     * cascade's flag and its cursor are written only by the nuke itself and
     * by image 0x0203C when it finishes. That is the original's behaviour
     * and not an omission here: a second nuked level picks the cascade up
     * where the last one left it.
     *
     * And note what the zero does to the double click. Image 0x04005
     * subtracts in AL and image 0x04007 is a SIGNED compare, so a press at
     * tick 128 or beyond reads its gap from the zero as NEGATIVE and is
     * accepted on its own. ds:0x1FD3 is a byte bumped once a frame, so for
     * half of every fifteen seconds a single click nukes the level. It is
     * transcribed rather than corrected - the port matched the original
     * frame for frame through it. */
    g_nuke_tick = 0;                             /* image 0x0043B */
    g_nuke_last = 0;                             /* image 0x00438 */
}

/* Image 0x30A6. What it releases is a lemming into the array at ds:0x88;
 * this keeps only the count, because nothing walks that array yet. */
void level_release_step(void)
{
    if (level_paused() == 0xFF) return;         /* image 0x30A6 */
    if (level_entrance_open() != 0xFF) return;  /* image 0x30B0 */
    if (--g_release_wait) return;               /* image 0x30BA */
    g_release_wait = g_release_step;            /* image 0x30C3 */
    if (g_released == g_to_release) return;     /* image 0x30CC */
    /* ONE MORE COMES OUT, AND IT MAKES NO SOUND.
     *
     * This used to ask for 0x0D here, on the reading that image 0x030A0's
     * request belonged to the routine at image 0x030A6. It does not: image
     * 0x030A5 is a RET, so the release STARTS at 0x030A6 and that sound is
     * the last act of the block above it - the fire death, reached by
     * falling through image 0x0304F's effect 6. A player heard it fire once
     * per lemming arriving and said the original does not.
     *
     * There is no int 60h or int 61h anywhere between image 0x030A6 and
     * image 0x0314F. The release is silent, and adjacency is not provenance:
     * the site sat four instructions above a routine it had nothing to do
     * with, and the comment asserting otherwise read perfectly well. */
    g_released++;
}

/* ====================================================================
 * WHICH TUNE.
 *
 * The port asked for none, which is why it played effects and no music:
 * every sound_request site it had was an EFFECT. Nothing here could have
 * found that, because a request never made writes no registers - the stream
 * comparison scoring 10,333 of 10,333 was scoring a stream driven by a
 * script, not by the game.
 *
 * WHEN it starts was ALREADY transcribed, further up in level_start_step -
 * image 0x0361's opening sequence, whose two sound cues were deliberate
 * no-ops with a comment saying the port makes no sound. Adding a second copy
 * of that sequencer here is exactly the collision CLAUDE.md warns about with
 * two statics of one name, and it was very nearly done.
 * ==================================================================== */

/* The level music_choose should look at. THE PORT'S OWN: the original reads
 * ds:0x0FE8 out of the level record it has already loaded, and the port hands
 * level_start_reset the same information through here rather than changing
 * its signature, which every caller and test would have to follow. */
static const level *g_music_level;

/* THE PORT'S OWN - see the note above. */
void music_set_level(const level *lv) { g_music_level = lv; }

static uint8_t g_tune;           /* ds:0x54  the driver's tune number */
static uint8_t g_tune_rot;       /* ds:0x1F6C the rotation counter, 0..16 */
static uint8_t g_tune_advance;   /* ds:0x1F6D non-zero: step the rotation */
static uint8_t g_tune_pick;      /* ds:0x0FE8 0 rotates, 1..4 names one */
static uint8_t g_snd_driver;     /* ds:0x2010 0xFF once a driver answered */

/* image 0x04877 - the rotation, seventeen tunes deep. The names are the
 * driver's own, from its built-in test menu (docs/sound.md): 6 is "CanCan",
 * and index 0 is where a fresh run starts, which is why the original's
 * capture asked for tune 6. */
static const uint8_t TUNE_ROTATION[17] = {
    0x06, 0x07, 0x08, 0x0A, 0x0B, 0x04, 0x0C, 0x0D, 0x0E,
    0x0F, 0x05, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15
};

/* image 0x04888 - the four that are asked for by name rather than rotated. */
static const uint8_t TUNE_BY_NUMBER[4] = { 0x01, 0x09, 0x03, 0x02 };

/* image 0x0482F, and the two routines it jumps into at image 0x04840 and
 * image 0x04866. Neither has a single near CALL in the image - they are
 * jumped to - which is why a static search for callers found nothing and the
 * question had to be put to the running original instead. */
void music_choose(const level *lv)
{
    if (g_snd_driver == 0) return;              /* image 0x04834 */
    /* ds:0x0FE8 is the level's EXTENDED graphics set, its header's +0x1C,
     * and non-zero means one of the four VGASPEC levels. Image 0x01A26 and
     * image 0x03742 read the same byte to load a different file and build
     * the terrain a different way, which is what identifies it - and it is
     * why the by-number table has exactly four entries. */
    if (lv) g_tune_pick = (uint8_t)(lv->extended_graphics_set & 0xFF);
    uint8_t al = g_tune_pick;                   /* image 0x04836 */
    if (al != 0) {                              /* image 0x0483D */
        g_tune = TUNE_BY_NUMBER[(uint8_t)(al - 1) & 3];   /* image 0x04866 */
        return;
    }
    if (g_tune_advance != 0) {                  /* image 0x04840 */
        al = (uint8_t)(g_tune_rot + 1);
        if (al == 0x11) al = 0;                 /* image 0x0484C: seventeen */
        g_tune_rot = al;
    }
    g_tune = TUNE_ROTATION[al % 17];            /* image 0x0485E */
}

/* ds:0x1F6D, and it is what makes each level's music differ from the last.
 *
 * Image 0x04720 compares the rescued percentage against the requirement; the
 * PASS arm at image 0x04729 sets this to 0xFF - in the same instruction pair
 * that advances the level index at image 0x0472E - and the FAIL arm at image
 * 0x04778 clears it. So passing a level steps the rotation and failing one
 * replays the same tune with the same level, which is the behaviour a player
 * would notice immediately and the port did not have: it chose index 0 every
 * time and played tune 6 on every level of the game.
 *
 * Image 0x0454C clears it on a cold start, which is why the FIRST level is
 * index 0 whatever happened in a previous session. */
void music_level_passed(uint8_t passed)
{
    g_tune_advance = passed ? 0xFF : 0;         /* image 0x04729 / 0x04778 */
}

/* image 0x0454C, in the block that wipes DGROUP 0..0xFCA at a cold start. */
void music_cold_start(void)
{
    g_tune_advance = 0;
    g_tune_rot = 0;
}

/* ds:0x25A7, the sound settings, and it lives HERE rather than in sdl_io.c
 * because it is the game's state and the game's decision. It was in sdl_io.c
 * hard-coded to 3, and that is what let this go wrong: with no driver at all
 * the port would still have asked for a tune.
 *
 * The original never hard-codes it. Image 0x00591 sets it to 1 BEFORE the
 * detect - bit 0, effects only - and image 0x005A9 raises it to 3 only when
 * image 0x0059F's detect answered. So bit 1 IS "a driver is present", which
 * is exactly why image 0x003AD can ask for music with no ds:0x2010 test of
 * its own while image 0x00380 and image 0x00394 both have one. */
static uint8_t g_snd_bits = 1;         /* image 0x00591 */

uint8_t sound_enabled(uint8_t bits)
{
    return (uint8_t)((g_snd_bits & bits) != 0);
}

/* ds:0x2010, which image 0x005A4 sets when the detect answers. THE PORT'S
 * OWN only in how it learns the answer - sdl_io.c asks the driver whether it
 * loaded, where the original asked the card. */
void sound_driver_present(uint8_t yes)
{
    g_snd_driver = yes ? 0xFF : 0;     /* image 0x005A4 */
    g_snd_bits   = yes ? 3 : 1;        /* image 0x005A9 against image 0x00591 */
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
    /* image 0x00489 calls image 0x0482F in the same block that clears these
     * three bytes, so the tune is chosen when the level begins and image
     * 0x03B4 twenty-odd frames later plays whatever it picked. The level is
     * passed in because image 0x04836 reads its extended graphics set. */
    music_choose(g_music_level);
    /* AND THE NUKE'S FOUR BYTES, which nothing here reset - so they carried
     * across levels and the double click became a SINGLE one. A player found
     * it after several levels in a row.
     *
     * Image 0x00438 clears ds:0x1FD4 and image 0x0043B clears ds:0x1FD3
     * explicitly, and image 0x0044F then wipes ds:0x12 for 0x42A0 bytes -
     * through 0x42B1 - which covers ds:0x4B and ds:0x4C as well.
     *
     * WHY IT SHOWED AS A SINGLE CLICK. Image 0x04007 is a SIGNED compare, so
     * the test is `(int8_t)(tick - last) > 5`. With `last` left over from an
     * earlier level and `tick` a free-running byte, that difference is
     * negative about half the time - and a negative difference is ACCEPTED,
     * because the signedness is there to let a wrapped counter through. The
     * first press then nukes. Fresh from a level start both are zero, the
     * difference is 0, and... it would nuke too, which is why image 0x0043B
     * clearing the TICK matters as much as clearing the last press: the tick
     * has to climb past 5 before the first press can be refused.
     *
     * ds:0x4B is the worse one. Nuke a level and it stayed 0xFF, so the NEXT
     * level began its cascade the moment it started. */
    g_nuke_last = 0;                /* image 0x00438: ds:0x1FD4 */
    g_nuke_tick = 0;                /* image 0x0043B: ds:0x1FD3 */
    g_nuke      = 0;                /* image 0x0044F's wipe: ds:0x4B */
    g_nuke_at   = 0;                /*                       ds:0x4C */
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
        if (dl == 0x0F) {
            /* image 0x0380: THE ENTRANCE OPENING, and it is heard twenty
             * frames BEFORE the trapdoor actually opens - which is what
             * makes it sound like doors working rather than an announcement
             * that they have. This was a deliberate no-op for as long as the
             * port had no sound. */
            if (g_snd_driver == 0xFF && sound_enabled(3))  /* image 0x0387 */
                sound_request(4, 0x03);                    /* image 0x038E */
        } else if (dl == 0x23) {
            g_entrance_open = 0xFF;         /* image 0x394 */
            /* image 0x03A7's own sound, which the port had never made */
            if (g_snd_driver == 0xFF && sound_enabled(3))  /* image 0x03A0 */
                sound_request(4, 0x02);                    /* image 0x03A7 */
        } else if (dl == 0x37) {
            /* image 0x03AD, and note the mask: the MUSIC wants bit 1 of
             * ds:0x25A7 ALONE where the two effects above want either bit.
             * docs/sound.md recorded "music against effects" as the obvious
             * reading and NOT established for a long time; the two routines
             * using different masks is what settles it. */
            if (sound_enabled(2)) {                        /* image 0x03AD */
                sound_request(3, g_tune);                  /* image 0x03B4 */
                sound_tick_now();                          /* image 0x03BB */
            }
        } else if (dl > 0x37) {
            g_level_phase = (uint8_t)(al & 0xFB);
        }
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
/* HAS THIS LEMMING REACHED AN EXIT? - image 0x02F3B.
 *
 * The trigger rectangle lives in GROUNDxO beside the effect byte, and its
 * UNITS are the whole difficulty: the exit's reads left 6, top 4, 1 by 2,
 * and pixels, four-pixel units and eight are all superficially plausible.
 * Guessing is what this project keeps getting caught by, so it is measured.
 *
 * tools/probe_exit.py drives the original into digging a way down and then
 * watches for the frame a lemming's state becomes 0x2000. Six of them did,
 * every one at record (888, 147) - and the exit object of "Just dig!" sits
 * at (848, 116). A record position is sixteen more than the canvas one, so
 * that is canvas (872, 131), and
 *
 *     848 + 6 * 4 = 872      the x, exactly
 *     116 + 4 * 4 = 132      the y, one row below where they were caught
 *
 * so the units are FOUR PIXELS, and the rectangle is four wide and eight
 * tall. The one row is the lemming being tested before it steps down into
 * the box rather than after; the box is therefore taken as starting a row
 * higher, which is what makes the measured entries land inside it.
 *
 * Image 0x02F3B also refuses a lemming that is falling - `test [si+8], 4` -
 * so you cannot be saved on the way past.
 */
#define TRIGGER_UNIT 4

/* The level and its object table, for the trigger test. The original has
 * them in fixed places; this port passes them around, and lemmings_step
 * does not carry them - so they are set once when the level starts rather
 * than threaded through every caller of it. */
static const level  *trig_lv;
static const ground *trig_g;

/* THE PORT'S OWN: the original keeps these in fixed places and has no need
 * to be handed them. */
void level_triggers_set(const level *lv, const ground *g)
{
    trig_lv = lv;
    trig_g = g;
    level_triggers_build(lv, g);       /* image 0x037CB */
}

/* WHAT THE LEMMING IS STANDING ON - transcribed from image 0x02EC0, and it
 * is a MAP LOOKUP rather than a rectangle test.
 *
 * The original builds a trigger map once, at image 0x037CB, straight after
 * the terrain is composed, and every lemming then reads one byte out of it:
 *
 *     image 0x037E5   col = (object x >> 2) + trigger_left
 *     image 0x037F2   row = (object y >> 2) + trigger_top, then DEC BX
 *     image 0x0380E   the byte is (object index << 4) | trigger_effect
 *     image 0x0381D   stamped trigger_width by trigger_height cells
 *
 *     image 0x02ECE   col = lemming x >> 2
 *     image 0x02ECB   row = (lemming y - 0x10) >> 2
 *     image 0x02EE6   ss:[row * 0x1A0 + col + 0x12]
 *
 * So the units are FOUR pixels both ways - 0x1A0 is 416, and 416 cells of
 * four is the 1,664 a level is wide - and image 0x037F9's `dec bx` lifts
 * every stamp a whole cell, not a pixel.
 *
 * The port had a rectangle test in pixels with `y0 - 1` fudged onto it. It
 * was close, and close put lemmings through the exit one to two frames after
 * the original did: 24 to 40 pixels a frame for the last quarter of the
 * level, and the level ending at a different moment. A fudge that is nearly
 * right is exactly the thing this project keeps finding out the hard way.
 *
 * The map is not held here. Stamping and then reading back is the same
 * answer as testing each object's cell rectangle in order and keeping the
 * LAST that matches - later objects overwrite earlier ones in the map - and
 * a level has at most a handful.
 */
#define TRIG_CELL      4        /* image 0x02ECE and image 0x037E5 */
#define TRIG_Y_BIAS    0x10     /* image 0x02ECB */
#define TRIG_Y_LIMIT   0xB0     /* image 0x02EC5: below this, nothing */
#define TRIG_ROW_UP    1        /* image 0x037F9's `dec bx` */

/* THE MAP ITSELF. Image 0x037CB builds it once, straight after the terrain
 * is composed, and everything that asks what a lemming is standing on reads
 * one byte out of it. The port used to compute the answer from the objects
 * every time, which gives the same result for a map nothing writes to - and
 * something does: image 0x03348 has a BLOCKER stamp its own effect in, and
 * checks the neighbouring cells first to refuse standing next to another.
 * A map that is recomputed from the level file cannot hold that.
 *
 * 0x1A0 is 416 cells of four pixels, which is the 1,664 a level is wide. */
#define TRIG_COLS   0x1A0
#define TRIG_ROWS   64          /* 40 rows of four covers the play area */
static uint8_t g_trigger[TRIG_ROWS][TRIG_COLS];

/* Image 0x0381B: dh cells across, dl rows down, stepping the destination by
 * 0x1A0 between rows. */
/* Image 0x03808 builds the byte as `ax = bp << 4 / or al, [si+0x14]` - the
 * OBJECT'S INDEX in the high nibble and the effect in the low. The port
 * stamped the effect alone, which is why it could tell that a trap had been
 * touched and never WHICH trap: image 0x02FB6 indexes the object records at
 * ds:0x17CB by that high nibble. With every index zero the two are the same
 * byte, so nothing here ever noticed. */
static void trig_stamp(int32_t col, int32_t row, int32_t w, int32_t h,
                       uint8_t v)
{
    for (int32_t r = row; r < row + h; r++) {
        if (r < 0 || r >= TRIG_ROWS) continue;
        for (int32_t c = col; c < col + w; c++) {
            if (c < 0 || c >= TRIG_COLS) continue;
            g_trigger[r][c] = v;            /* image 0x0381D */
        }
    }
}

/* One byte of it, image 0x02EE6's `mov al, ss:[bx]`. */
uint8_t level_trigger_cell(int32_t col, int32_t row)
{
    if (col < 0 || col >= TRIG_COLS || row < 0 || row >= TRIG_ROWS) return 0;
    return g_trigger[row][col];
}

/* And writing one, which is what image 0x03348's blocker does to the map
 * after checking its neighbours. */
void level_trigger_put(int32_t col, int32_t row, uint8_t v)
{
    if (col < 0 || col >= TRIG_COLS || row < 0 || row >= TRIG_ROWS) return;
    g_trigger[row][col] = v;
}

/* Image 0x037CB, stamping each object in turn - later ones overwrite. */
/* THE TRAP'S OWN ANIMATION.
 *
 * Image 0x02FCE sets bit 1 of the object's record at ds:0x17CB and image
 * 0x02FC5 refuses a second lemming while it is set - and bit 1 is the SAME
 * bit level_anim_step already tests to decide whether an object advances a
 * frame, image 0x07C1E's `flags & 2`. A trap is stored with that bit CLEAR
 * and with bit 0 set, so it plays its frames once and stops itself.
 *
 * So the trigger does not need a flag of its own: it needs to reach the
 * animation record. The port kept a separate busy array, which made the
 * lemming die and left the trap standing still - 152 of 1,100 frames on
 * "Lend a helping hand....", where the traps are most of what moves. */
uint8_t level_object_busy(uint8_t i)
{
    if (!g_anim || i >= LEVEL_ANIM_MAX) return 0;
    return (uint8_t)((g_anim[i].flags & 2) != 0);   /* image 0x02FC5 */
}

/* Image 0x02FCE: bit 1 of the object's record, which is the same bit image
 * 0x07C1E tests to decide whether an object advances a frame. */
void level_object_trigger(uint8_t i)
{
    if (!g_anim || i >= LEVEL_ANIM_MAX) return;
    g_anim[i].flags |= 2;                           /* image 0x02FCE */
}

/* Image 0x037CB. */
void level_triggers_build(const level *lv, const ground *g)
{
    memset(g_trigger, 0, sizeof g_trigger);
    /* level_triggers_set(NULL, NULL) is how a caller says "no level", and
     * clearing the map IS the whole of that. Without this the loop below
     * dereferenced the null and the sanitizer build died on it - which is
     * what a sanitizer is for, and why a claim that it passes has to be
     * re-run rather than carried forward. */
    if (!lv || !g) return;

    /* THE STEEL AREAS GO IN FIRST, and the port never put them in at all.
     *
     * Image 0x03839 walks the LVL's 32 steel entries at ds:0x172B and stamps
     * the cell value 9 over each one - image 0x03886 - `(width + 1)` cells
     * across by `(height + 1)` down, the two nibbles image 0x03851 and image
     * 0x03856 unpack. It runs BEFORE the object trigger pass at image
     * 0x037C2, so an object's effect lands on top of steel rather than under
     * it, and that order is this loop's position rather than a preference.
     *
     * What it cost: `tcell` is the cell under a lemming, and the only thing
     * writing this map was the object pass - so a steel area read as 0 and
     * every test against it was dead code. The BOMBER's is image 0x023A6,
     * which refuses effects 9 and 5 before image 0x023B9 digs the crater, so
     * a bomb ate straight through a steel floor. A player found it in
     * level004 section 3 and checked it against DOSBox. The basher's test at
     * game.c's `(a->tcell & 0x0F) == 9` was equally dead.
     *
     * Nothing here could have caught it: every frame-by-frame reference in
     * this repository is a level nobody bombs, and the skill runs are on
     * "Just dig!", which has no steel. It is the same shape as the digger
     * being assignable twice - a rule with no reachable state to check it. */
    for (uint16_t k = 0; k < lv->num_steel; k++) {
        const level_steel *st = &lv->steel[k];
        /* level_parse already turned the entry's bytes into pixels - the
         * column is `(e[0] << 1) | (e[1] >> 7)` times four, less the same
         * sixteen every x here is less, and the nibbles are plus one times
         * four - so the cells come back by dividing. Reading the raw bytes
         * again would be a second parse of the same field, and the two
         * would drift. */
        trig_stamp(((int32_t)st->x + LEMMING_DX) / TRIG_CELL,
                   (int32_t)st->y / TRIG_CELL,
                   st->w / TRIG_CELL, st->h / TRIG_CELL,
                   9);                              /* image 0x03886 */
    }

    /* SIXTEEN, not thirty-two. Image 0x037C6 sets `cx = 0x10` and image
     * 0x0382F steps `di` by eight, so the trigger pass walks the first
     * sixteen object slots where image 0x07BB8 DRAWS all thirty-two. No level
     * in this set has more than sixteen objects, so it changes nothing -
     * transcribed because it is the original's bound and costs one line. */
    for (uint16_t i = 0; i < lv->num_objects && i < 16; i++) {
        const level_object *o = &lv->objects[i];
        if (o->id >= GROUND_OBJECTS) continue;
        const ground_object *go = &g->objects[o->id];
        /* AN EFFECT OF ZERO IS STILL STAMPED. This used to skip it and cite
         * image 0x037E3 for the skip, which is the wrong address: that is the
         * `je` for image 0x037E1's `and ax, ax`, and AX there is the object's
         * X - image 0x037DC reads bytes 0 and 1 - so it is the empty-slot
         * test that level_parse already does. There is no effect test at all:
         * image 0x03812 ORs `[si+0x14]` into `index << 4` and image 0x0381D
         * stamps unconditionally.
         *
         * It matters because the stamp OVERWRITES. An effect-0 object lying
         * over a real trigger erases it in the original, and did not here.
         *
         * Settled from the DATA rather than from a run: on level004 section
         * 3, object 2 is id 6 with effect 0 and a 1x1 trigger at (0, 0), so
         * it stamps column 208 row 27 with `(2 << 4) | 0` = 0x20 - which is
         * exactly the one cell of 16,040 where tools/verify_triggers.py found
         * the original holding 0x20 and the port 0x00. Object 0 stamps 0x00
         * and is invisible against an empty cell, which is why only one
         * showed. */
        /* The port's object x is sixteen less than the file's - level_parse
         * takes it off there - and the builder used the file's. */
        int32_t c0 = ((int32_t)o->x + LEMMING_DX) / TRIG_CELL
                     + (int32_t)go->trigger_left;   /* image 0x037E9 */
        int32_t r0 = (int32_t)o->y / TRIG_CELL
                     + (int32_t)go->trigger_top - TRIG_ROW_UP;  /* 0x037F9 */
        trig_stamp(c0, r0, go->trigger_width, go->trigger_height,
                   (uint8_t)(((i & 0x0F) << 4)          /* image 0x03808 */
                             | (go->trigger_effect & 0x0F)));
    }
}

/* Image 0x02EC0: the lookup, now that there is a map to look in. */
/* Image 0x02EC0: the lookup, now that there is a map to look in - and it
 * keeps TWO bytes in the record on the way past.
 *
 * Image 0x02EEA stores the cell under the lemming at [si+0x2C], and image
 * 0x02EFC stores the one two rows up and two cells along the way it faces at
 * [si+0x2B]. Nothing in the trigger dispatch reads either; the BASHER does,
 * at image 0x034DC, to decide whether there is anything worth bashing. A
 * lookup that only returned its answer would have left the basher no way to
 * ask. */
uint8_t level_trigger_at(lemming *a, const level *lv, const ground *g)
{
    (void)lv; (void)g;
    if (a->y >= TRIG_Y_LIMIT) return 0;             /* image 0x02EC5 */
    /* Image 0x02ECE and image 0x02ECB. */
    int32_t col = (int32_t)a->x / TRIG_CELL;
    int32_t row = ((int32_t)a->y - TRIG_Y_BIAS) / TRIG_CELL;
    uint8_t here = level_trigger_cell(col, row);
    a->tcell  = here;                               /* image 0x02EEA */
    a->tahead = level_trigger_cell(col + 2 * a->dir, row - 2); /* 0x02EFC */
    return here;
}

/* Image 0x02F15: effect 1, and image 0x02F3B is where it lands. */
int level_at_exit(lemming *a, const level *lv, const ground *g)
{
    if (a->flags & 4) return 0;            /* image 0x02F3B: not while falling */
    return (level_trigger_at(a, lv, g) & 0x0F) == TRIGGER_EXIT;
}

/* Image 0x02F42: the whole state word becomes 0x2000 - assigned, not OR-ed -
 * and the animation restarts. Image 0x02326 then counts eight frames and
 * hands it to level_lemming_saved. */
#define EXIT_BIT 0x2000
#define EXIT_FRAMES 8

/* THE ANIMATION IS DRAWN NOW - see EXIT_SPR_BASE. What follows is how it
 * was found, because two of the three routes to it were dead ends.
 *
 *
 * The assignment writes the sprite's fields along with the state, so its
 * shape is not in doubt: image 0x02F51 gives it 0x4E = 78 bytes a frame and
 * image 0x02F56 puts the mask at 0x34 = 52. By the store's own formula -
 * see the note on the digger - 78 with the mask at 52 is two colour planes
 * over thirteen rows of two bytes: 16 wide, 13 tall, 52 bytes of colour in
 * the FILE, eight frames. The hot spot is (-8, -13), image 0x02F5F.
 *
 * What is missing is WHERE those 52-byte frames are in main.dat. The method
 * that found the digger's - capture the bytes the guest is reading and
 * search the file for them - needs a run that digs a way down and then waits
 * for a lemming to walk into the exit, and tools/probe_exit.py does that but
 * unreliably: three runs in a row failed to assign the digger at all.
 *
 * Searching the file for the geometry instead does not discriminate. Eight
 * consecutive 52-byte frames whose bounding boxes stay put match NINE
 * THOUSAND offsets, including offset 0 - the walker, whose 40-byte frames
 * decoded at 52 drift but stay in the window. So that is not a search, it is
 * a coincidence detector.
 *
 * So a lemming that reaches the exit is counted and removed correctly and is
 * invisible while it happens. That is the honest state of it.
 *
 * Transcribed from image 0x02F42. */
void lem_exit_start(lemming *a)
{
    a->flags = EXIT_BIT;
    a->anim  = 0;
    a->hot_x = -8;                         /* image 0x02F5F */
    a->hot_y = -13;                        /* image 0x02F64 */
    a->state = 0;
    a->extra &= 1;                         /* image 0x02F77 */
    /* AND IT IS HEARD. Image 0x02F7B..0x02F9F is the same pair of gates
     * every sound site here has, and this routine has BOTH halves of it:
     * image 0x02F8C asks INT 60h for 0x02 when ds:0x2010 is NOT 0xFF - the
     * machine with no driver - and image 0x02F9F asks INT 61h for 0x10 when
     * it is. sound_request's first argument is which of the two. */
    if (g_snd_driver != 0xFF) {
        if (sound_enabled(1))              /* image 0x02F82 */
            sound_request(3, 0x02);        /* image 0x02F89 */
    } else if (sound_enabled(3)) {         /* image 0x02F95 */
        sound_request(4, 0x10);            /* image 0x02F9C */
    }
}

/* Transcribed from image 0x02326: eight frames, then saved and gone. */
static void lem_exiting(lemming *a)
{
    a->anim = (uint8_t)((a->anim + 1) & 7);
    if (a->anim) return;
    level_lemming_saved();                 /* image 0x02335 */
    a->alive = 0xFF;                       /* image 0x02339 */
}

/* THE ACCESS CODE - partly transcribed, and honest about which part.
 *
 * The results screen offers "Your Access Code for Level NN is ABCDEFGHIJ",
 * and the code screen at image 0x04967 takes one back. Image 0x05042 builds
 * it from three bytes: ds:0x1F6B the level index, ds:0x491D, and ds:0x40 the
 * rescued percentage - scattering their bits across ten characters, each
 * added to a base letter.
 *
 * TWO PARTS ARE CERTAIN, because two independent routines agree on them:
 *
 *   the base letters, ds:0x4913 - "AJHLDHBBCJ", ten of them, static in the
 *   image, and the SAME table the validator reads at image 0x051CD
 *
 *   the tenth character, a checksum. Image 0x05147 sums the first nine, masks
 *   to four bits and adds the base; image 0x051D4 does the same sum on what
 *   was typed and image 0x051E4 compares. Generator and validator, written
 *   separately, describing one rule.
 *
 * THE BIT SHUFFLE FOR CHARACTERS 0..8 is half read and half MEASURED, and
 * the two halves are worth telling apart.
 *
 *   Read: image 0x05135 puts the level's low nibble in character 7 and its
 *   high nibble in character 8, and image 0x0516B rotates characters 0..6
 *   right, 8 - (level & 7) times, after they are built.
 *
 *   Measured: which of characters 0..6 each remaining input bit lands in,
 *   and what it adds. Those are ten near identical blocks of mask-and-shift
 *   and reading them off by eye invites exactly the sort of error this port
 *   has already made twice with bit positions - an automatic pass over the
 *   disassembly produced a plausible table whose last four rows were stale
 *   register state, which is the worst kind of wrong. So the table below was
 *   taken by DRIVING the original's own generator over chosen inputs, the
 *   way tools/verify_digits.py drives image 0x04BC2: tools/probe_code.py
 *   calls image 0x05042 with one bit set at a time and reads the ten
 *   characters back out of ds:0x4D68. It reproduces all 28 driven cases.
 *
 * Note what the measurement says that no amount of reading would have made
 * obvious: bits 4..7 of ds:0x491D and bit 7 of the percentage reach no
 * character at all. The code carries four bits of the one and seven of the
 * other, not eight of each.
 */
#define CODE_LEN 10
static const char CODE_BASE[CODE_LEN + 1] = "AJHLDHBBCJ";   /* ds:0x4913 */

/* Transcribed from image 0x05147: the last character carries a checksum of
 * the other nine, four bits of it, added to the base letter. */
char level_code_checksum(const char *first_nine)
{
    uint8_t al = 0;
    for (uint8_t i = 0; i < CODE_LEN - 1; i++)
        al = (uint8_t)(al + (uint8_t)first_nine[i]);
    al &= 0x0F;                                   /* image 0x05163 */
    return (char)(al + CODE_BASE[CODE_LEN - 1]);  /* image 0x05165 */
}

/* Where each input bit lands among characters 0..6, and what it adds.
 * MEASURED by driving image 0x05042 - see the note above, and out/code.json.
 * Index 0xFF means the bit reaches no character. */
static const struct { uint8_t chr, add; } CODE_LEVEL_BIT[8] = {
    {0, 8}, {1, 4}, {2, 4}, {3, 1}, {4, 2}, {5, 1}, {6, 4}, {6, 8}
};
static const struct { uint8_t chr, add; } CODE_OTHER_BIT[8] = {
    {0, 2}, {2, 1}, {4, 1}, {5, 2},
    {0xFF, 0}, {0xFF, 0}, {0xFF, 0}, {0xFF, 0}
};
static const struct { uint8_t chr, add; } CODE_PERCENT_BIT[8] = {
    {0, 4}, {1, 1}, {2, 2}, {3, 2}, {4, 8}, {5, 4}, {6, 1}, {0xFF, 0}
};

/* Image 0x05042: build the ten character access code for a level. `other` is
 * ds:0x491D and `percent` is ds:0x40, the rescued percentage. */
void level_code_generate(uint8_t level_no, uint8_t other, uint8_t percent,
                         char *out)
{
    for (uint8_t i = 0; i < CODE_LEN; i++)
        out[i] = CODE_BASE[i];

    for (uint8_t b = 0; b < 8; b++) {
        if ((level_no >> b) & 1)
            out[CODE_LEVEL_BIT[b].chr] = (char)(out[CODE_LEVEL_BIT[b].chr]
                                                + CODE_LEVEL_BIT[b].add);
        if (((other >> b) & 1) && CODE_OTHER_BIT[b].chr != 0xFF)
            out[CODE_OTHER_BIT[b].chr] = (char)(out[CODE_OTHER_BIT[b].chr]
                                                + CODE_OTHER_BIT[b].add);
        if (((percent >> b) & 1) && CODE_PERCENT_BIT[b].chr != 0xFF)
            out[CODE_PERCENT_BIT[b].chr] = (char)(out[CODE_PERCENT_BIT[b].chr]
                                                 + CODE_PERCENT_BIT[b].add);
    }

    /* Image 0x05135: the level's two nibbles, unrotated. */
    out[7] = (char)(CODE_BASE[7] + (level_no & 0x0F));
    out[8] = (char)(CODE_BASE[8] + (level_no >> 4));

    /* Image 0x0516B: rotate characters 0..6 right, 8 - (level_no & 7) times.
     * Image 0x05175 reads character 6, shifts 0..5 up one and drops it in
     * at 0 - so it is a rotate right by one, run that many times. */
    for (uint8_t n = (uint8_t)(8 - (level_no & 7)); n; n--) {
        char last = out[6];
        for (uint8_t i = 6; i; i--) out[i] = out[i - 1];
        out[0] = last;
    }

    /* Image 0x05147. The rotation is a permutation, so the sum is the same
     * either side of it. */
    out[9] = level_code_checksum(out);
    out[CODE_LEN] = '\0';
}

/* Transcribed from image 0x051D4: the validator's half of the same rule. */
int level_code_ok(const char *code)
{
    return code[CODE_LEN - 1] == level_code_checksum(code);
}

/* THE RESULTS SCREEN'S TEXT - transcribed from image 0x048BD.
 *
 * The screen's COMPOSITION is not read: image 0x048F3 hands a record at
 * ds:0x4C4E to the text drawer at image 0x04D4D and what lays the screen out
 * around it has not been found. Its text assembly is small and complete,
 * though, and it is here:
 *
 *     image 0x048BD  the rescued percentage, ds:0x40, written into
 *                    "You rescued     " through image 0x04B5C - the same
 *                    right-aligned writer the briefing's percentage uses
 *     image 0x048C7  the needed percentage, ds:0x41, into "You needed      "
 *     image 0x048D1  the prompt, chosen by ds:0x25A6 - 0 gives the mouse
 *                    wording, 1 the joystick's, 2 the keyboard's
 *
 * That last is what the input chooser screen is for, and it is why the
 * strings come in threes. The headline above them is one of two: image
 * 0x001FA's clock-ran-out gives "Your time is up!" at ds:0x4C1C and image
 * 0x001E7's everyone-accounted-for gives ds:0x4C35.
 */
static const char S_TIME_UP[]   = "Your time is up!";          /* ds:0x4C1C */
static const char S_ALL_OUT[]   = "All lemmings accounted for."; /* ds:0x4C35 */
static const char S_RESCUED[]   = "You rescued     ";          /* ds:0x4C51 */
static const char S_NEEDED[]    = "You needed      ";          /* ds:0x4C65 */

/* ds:0x25A6 - which input the player chose on the start-up screen. Image
 * 0x048D1, image 0x04781 and image 0x047EE all pick their wording from it.
 * The port's input screen matches the original's to the pixel but nothing
 * yet SETS this, so it stays on the mouse's wording. */
uint8_t lem_input_kind;

/* The three wordings, by ds:0x25A6 - images 0x04C77, 0x04C9D and 0x04CB9. */
static const char *const S_MENU_PROMPT[3] = {
    "Press right mouse button for menu",
    "Press button 2 for menu",
    "Press Return for menu",
};

/* Image 0x048BD writes the number three characters from the end, which is
 * what leaves "You rescued   10" reading as a column. It goes through the
 * same right-aligned fill the briefing uses; that lives further down this
 * file, so it is declared here. */
#define RESULT_NUM_BACK 3

const char *level_results_headline(int timed_out)
{
    return timed_out ? S_TIME_UP : S_ALL_OUT;   /* images 0x001FA, 0x001E7 */
}

const char *level_results_prompt(uint8_t input_kind)
{
    return S_MENU_PROMPT[input_kind < 3 ? input_kind : 0];
}

/* Transcribed from image 0x048BD and image 0x048C7, and the writer they use
 * is image 0x04B5C - which is NOT the right-aligned fill the briefing uses:
 *
 *     [si+3] = ' '            image 0x04B5C
 *     call image 0x04B68      up to three digits, LEFT aligned, and si is
 *                             advanced only for a digit actually emitted
 *     [si] = '%'              image 0x04B63, right after the last digit
 *
 * So the per cent sign moves with the number - "You rescued 100%" and "You
 * needed  10%" - and the port, which wrote the number three from the end and
 * no sign at all, produced "You rescued100". `line` takes at least 20
 * characters.
 *
 * The number goes at ds:0x4C5D and ds:0x4C71, which are twelve characters
 * into each of the two records at ds:0x4C4E. */
#define RESULT_NUM_AT 12       /* ds:0x4C5D against the record's text */
void level_results_line(char *line, int needed_line, uint8_t percent)
{
    const char *t = needed_line ? S_NEEDED : S_RESCUED;
    size_t n = strlen(t);
    memcpy(line, t, n + 1);
    char *at = line + RESULT_NUM_AT;
    at[3] = ' ';                                   /* image 0x04B5C */
    char *end = put_num3(at, percent);             /* image 0x04B60 */
    *end = '%';                                    /* image 0x04B63 */
}

/* THE RESULTS SCREEN'S LAYOUT - transcribed from image 0x04D4D.
 *
 * This was "not read" for a long time and it is data, not code. Image
 * 0x04D4D walks a chain of records and every one carries its own position:
 *
 *     one byte  the column PLUS ONE - image 0x04D54 decrements it, and a
 *               zero is what ends the chain (image 0x04D50)
 *     one byte  the row
 *     one byte  a style; zero gives image 0x04D5E's bp = 2
 *     the text, with 0x0D ending the line (image 0x04D66)
 *
 * Image 0x04D78 then puts each character at (column * 16, row * 16) from a
 * 16x16 glyph, so the screen is a 40 by 21 grid - and it is mode 0x10 at
 * 640x350, NOT the level's 0x0D. That is why tools/record_level.py cannot
 * reach it: it captures at image 0x018F, the level loop's own end-of-frame,
 * and this screen has a loop of its own.
 */
typedef struct {
    uint8_t     col, row, style;
    const char *text;
} results_line;

#define RESULTS_COLS   40      /* 640 / 16 */
#define RESULTS_ROWS   21      /* 350 / 16, near enough */
#define RESULTS_CELL   16      /* image 0x04DAF and image 0x04DB1 */

/* THE FONT is MAIN.DAT section 4 at 0x69B0 - 94 glyphs of 96 bytes, which is
 * 16 rows of two bytes in three planes (image 0x04D8E's i*96, image 0x04DB5's
 * ch = 3). Image 0x04D7C starts it at '!', and a space draws nothing (image
 * 0x04D7A).
 *
 * The offset is MEASURED rather than read: image 0x04D90 indexes ds:0x5BC0 in
 * a loaded resource, and where that lands in the file is not in the image -
 * those bytes are still zero there. tools/probe_results.py dumps the guest's
 * copy and it matches section 4 at 0x69B0 over all 9,024 bytes. */
#define RESULTS_FONT_OFF   0x69B0
#define RESULTS_GLYPH      96
#define RESULTS_GLYPH_W    16
#define RESULTS_GLYPH_H    16
#define RESULTS_GLYPH_BPR  2
#define RESULTS_GLYPH_PL   3
#define RESULTS_FIRST_CH   0x21

/* One character, image 0x04D78. Three planes, and a zero is transparent -
 * image 0x04DBB hands the blitter a mask 0x40 bytes past the ink. */
void results_char(uint8_t *dst, uint16_t dst_w, uint16_t dst_h,
                  const uint8_t *sec, uint32_t sec_len,
                  uint8_t ch, uint8_t col, uint8_t row)
{
    if (ch == ' ') return;                          /* image 0x04D7A */
    if (ch < RESULTS_FIRST_CH) return;
    uint32_t base = RESULTS_FONT_OFF
                  + (uint32_t)(ch - RESULTS_FIRST_CH) * RESULTS_GLYPH;
    if (base + RESULTS_GLYPH > sec_len) return;

    int32_t x0 = (int32_t)col * RESULTS_CELL;       /* image 0x04DB1 */
    int32_t y0 = (int32_t)row * RESULTS_CELL;       /* image 0x04DAF */
    for (uint8_t r = 0; r < RESULTS_GLYPH_H; r++) {
        int32_t y = y0 + r;
        if (y < 0 || y >= (int32_t)dst_h) continue;
        for (uint8_t c = 0; c < RESULTS_GLYPH_W; c++) {
            int32_t x = x0 + c;
            if (x < 0 || x >= (int32_t)dst_w) continue;
            uint8_t v = 0;
            for (uint8_t p = 0; p < RESULTS_GLYPH_PL; p++) {
                uint8_t byte = sec[base + p * RESULTS_GLYPH_H * RESULTS_GLYPH_BPR
                                   + r * RESULTS_GLYPH_BPR + (c >> 3)];
                v = (uint8_t)(v | (((byte >> (7 - (c & 7))) & 1u) << p));
            }
            if (v) dst[(size_t)y * dst_w + (size_t)x] = v;
        }
    }
}

/* A whole record, image 0x04D4D: the column walks along as it goes. */
void results_text(uint8_t *dst, uint16_t dst_w, uint16_t dst_h,
                  const uint8_t *sec, uint32_t sec_len,
                  const results_line *line)
{
    uint8_t col = line->col;
    for (const char *p = line->text; *p; p++, col++)
        results_char(dst, dst_w, dst_h, sec, sec_len,
                     (uint8_t)*p, col, line->row);   /* image 0x04D73 */
}

/* WIN OR LOSE - transcribed from image 0x048F6.
 *
 * The end conditions at image 0x001C8 do not decide it; this does, from the
 * rescued percentage against the level's requirement. It is a ladder, and
 * the order matters because each rung falls through to the next:
 *
 *     saved == 100          the best verdict, taken first
 *     saved == 0            the worst
 *     saved <  needed / 2
 *     saved <  needed - 5
 *     saved <  needed - 1
 *     saved == needed - 1
 *     saved == needed
 *     saved <  needed + 20
 *     otherwise             the second best
 *
 * Image 0x0494D then indexes a table of nine at cs:0x4955. Note there is no
 * "you failed" branch: the verdicts BELOW `saved == needed` are the losses
 * and the ones at or above it are the wins, and the game says so in prose
 * rather than in a flag.
 */
#define VERDICTS 9

/* Transcribed DATA, images 0x0E9C3 through 0x0EC0A - the nine messages the
 * table points at, in the order the ladder produces. Each is two lines in
 * the original, split by its own record markers. */
static const char *const VERDICT[VERDICTS] = {
    "ROCK BOTTOM! I hope for your sake|that you nuked that level.",
    "Better rethink your strategy before|you try this level again!",
    "A little more practice on this level|is definitely recommended.",
    "You got pretty close that time.|Now try again for that few % extra.",
    "OH NO, So near and yet so far (teehee)|Maybe this time.....",
    "RIGHT ON. You can't get much closer|than that. Let's try the next...",
    "That level seemed no problem to you on|that attempt. Onto the next..",
    "You totally stormed that level!|Let's see if you can storm the next...",
    "Superb! You rescued every lemming on|that level. Can you do it again....?",
};

/* The nine verdicts, image 0x4955's table, each two lines with its own
 * column and row - image 0x04D4D's records. */
static const results_line VERDICT_AT[VERDICTS][2] = {
    { /* ds:0x4f33 */
        {  3,  7, 0, "ROCK BOTTOM! I hope for your sake" },
        {  7,  8, 0, "that you nuked that level." },
    },
    { /* ds:0x4f77 */
        {  2,  7, 0, "Better rethink your strategy before" },
        {  7,  8, 0, "you try this level again!" },
    },
    { /* ds:0x4fbc */
        {  2,  7, 0, "A little more practice on this level" },
        {  7,  8, 0, "is definitely recommended." },
    },
    { /* ds:0x5003 */
        {  3,  7, 0, "You got pretty close that time." },
        {  2,  8, 0, "Now try again for that few % extra." },
    },
    { /* ds:0x504e */
        {  1,  7, 0, "OH NO, So near and yet so far (teehee)" },
        { 10,  8, 0, "Maybe this time....." },
    },
    { /* ds:0x5091 */
        {  2,  7, 0, "RIGHT ON. You can't get much closer" },
        {  3,  8, 0, "than that. Let's try the next...." },
    },
    { /* ds:0x50de */
        {  1,  7, 0, "That level seemed no problem to you on" },
        {  1,  8, 0, "that attempt. Onto the next...." },
    },
    { /* ds:0x512c */
        {  4,  7, 0, "You totally stormed that level!" },
        {  1,  8, 0, "Let's see if you can storm the next..." },
    },
    { /* ds:0x517a */
        {  2,  7, 0, "Superb! You rescued every lemming on" },
        {  2,  8, 0, "that level. Can you do it again....?" },
    },
};

/* THE WHOLE SCREEN, in the order image 0x046EB builds it.
 *
 * Positions are the original's own records; see results_line above for where
 * each address is. The BACKGROUND is not drawn - the set-up calls at image
 * 0x046EB (0x4DEF, 0x4BE7, 0x53FC, 0x531E, 0x5028, 0x5017) have not been
 * read - so this lays the text on whatever is already in `dst`.
 *
 *     image 0x04703  the headline, and which of the two is chosen by
 *                    whether ds:0x20A7 and ds:0x20A9 read "0:00" - the
 *                    CLOCK, not a flag
 *     image 0x048BD  "You rescued" and "You needed", each with its number
 *     image 0x04798  the verdict, image 0x048F6 picking one of nine
 *     image 0x0479C  the prompt, by the input kind
 *     image 0x04752  the access code, on a level that was won
 */
#define RES_HEAD_COL_TIME   12      /* ds:0x4C19 */
#define RES_HEAD_COL_ALL     6      /* ds:0x4C2E */
#define RES_HEAD_ROW         1
#define RES_RESCUED_COL     12      /* ds:0x4C4E */
#define RES_RESCUED_ROW      3
#define RES_NEEDED_ROW       4
#define RES_CODE_COL         4      /* ds:0x4D3F */
#define RES_CODE_ROW        14
#define RES_CODE2_COL       12
#define RES_CODE2_ROW       15
#define RES_ROW18           18      /* next level, or retry */
#define RES_ROW19           19      /* the menu, always */

/* TWO PROMPTS, NOT ONE, and that is what image 0x04797 draws:
 *
 *     0x04797  push bx        the row-18 record, chosen just above
 *     0x04798  call 0x48F6    the verdict, which draws itself
 *     0x0479B  pop si / call 0x4D4D        the row-18 prompt
 *     0x0479F  si = ds:0x21 / call 0x4D4D  the row-19 one, which image
 *                                          0x048EB stored
 *
 * The port drew one line saying "Press mouse button to continue", which is
 * ds:0x4BA0 - a record this screen never uses. The original offers the next
 * level on row 18 and the menu on row 19, or a retry and the menu when the
 * level was lost. */
static const results_line S_NEXT[3] = {      /* ds:0x4D75, 0x4DA0, 0x4DC2 */
    {  1, RES_ROW18, 0, "Press left mouse button for next level" },
    {  5, RES_ROW18, 0, "Press button 1 for next level" },
    {  7, RES_ROW18, 0, "Press Space for next level" },
};
static const results_line S_RETRY[3] = {     /* ds:0x4CD3, 0x4CFE, 0x4D20 */
    {  1, RES_ROW18, 0, "Press left mouse button to retry level" },
    {  5, RES_ROW18, 0, "Press button 1 to retry level" },
    {  7, RES_ROW18, 0, "Press Space to retry level" },
};
static const results_line S_MENU[3] = {      /* ds:0x4C77, 0x4C9D, 0x4CB9 */
    {  3, RES_ROW19, 0, "Press right mouse button for menu" },
    {  6, RES_ROW19, 0, "Press button 2 for menu" },
    {  9, RES_ROW19, 0, "Press Return for menu" },
};

/* THE RESULTS SCREEN'S SIXTEEN COLOURS - MEASURED from the original's own
 * DAC while the screen was up, through tools/verify_results.py.
 *
 * Two steps, and only the second is read. Image 0x053FC picks entry 2 of the
 * table at cs:0x52D4 - ds:0x4920, which is
 *
 *     00 14 20 00 08 21 31 07 10 10 02 02 3f 06 31 27
 *
 * - and image 0x05425 writes those sixteen bytes to the attribute controller
 * at port 0x3C0, so they are attribute-to-DAC. What the DAC entries THEMSELVES
 * hold is set elsewhere and has not been traced, which is why the RGB below
 * is measured rather than transcribed. Driving image 0x046EB and reading the
 * DAC back takes ninety seconds; playing to the screen took fifteen minutes.
 *
 * The port's buffer holds attributes 0..7 - three planes of font - so these
 * are indexed by the attribute directly. */
static const lem_rgb RESULTS_PAL[16] = {
    {   0,   0,   0 },   /* attr  0 -> DAC 0x00 */
    { 130,  65,  32 },   /* attr  1 -> DAC 0x01 */
    {  97,  48,  32 },   /* attr  2 -> DAC 0x02 */
    {  48,   0,  16 },   /* attr  3 -> DAC 0x03 */
    {  32,   8, 125 },   /* attr  4 -> DAC 0x04 */
    {  65,  44, 146 },   /* attr  5 -> DAC 0x05 */
    { 105,  89, 166 },   /* attr  6 -> DAC 0x14 */
    { 154, 142, 190 },   /* attr  7 -> DAC 0x07 */
    {   0,  81,   0 },   /* attr  8 -> DAC 0x38 */
    {   0,  97,  16 },   /* attr  9 -> DAC 0x39 */
    {   0, 113,  32 },   /* attr 10 -> DAC 0x3a */
    {   0, 130,  65 },   /* attr 11 -> DAC 0x3b */
    { 211, 211, 211 },   /* attr 12 -> DAC 0x3c */
    { 178, 178,   0 },   /* attr 13 -> DAC 0x3d */
    {  65,  81, 178 },   /* attr 14 -> DAC 0x3e */
    { 227, 130, 146 },   /* attr 15 -> DAC 0x3f */
};

/* Image 0x053FC, as far as it is read. */
void level_results_palette(lem_rgb *out)
{
    for (uint8_t i = 0; i < 16; i++) out[i] = RESULTS_PAL[i];
}

/* THE BACKGROUND, transcribed from image 0x07E76 - and it is tiled, which is
 * why stepping the set-up calls found the screen black after every one of
 * them and a texture appearing "during the text pass".
 *
 * That reading was off by one instruction. Each stop in a stepped run is the
 * address of the NEXT instruction, so stopping at image 0x046FD is BEFORE
 * the call there has run, not after it. The call at image 0x046FD is
 * image 0x05017, which is `xor bx, bx` and a tail jump to image 0x07E76 -
 * the tiler. It draws the whole background and nothing else does.
 *
 *     image 0x07E76  ds:0x1FE4 = ds:0x1FEE, the resource to blit from
 *     image 0x07E7C  cx = 4, four rows
 *     image 0x07E82  ax = 0 then image 0x07E95 ax = 0x140, so two across
 *     image 0x07E8A  cx = 0x268: cl = 0x68 = 104 rows, ch = 2 planes
 *     image 0x07E87  dx = 0x140 = 320 wide
 *     image 0x07E8D  si = 0, offset zero of the resource
 *     image 0x07EAC  bx += 0x68, so the rows are at 0, 104, 208 and 312
 *
 * Two planes is four colours, which is what the picture uses. WHERE the tile
 * is in the file is MEASURED, not read - ds:0x1FEE is a loaded resource and
 * the image says nothing about which - and it is MAIN.DAT section 3 at
 * offset 0, checked against the original's own screen: on every row of the
 * top tile that no text crosses, ZERO pixels differ.
 */
#define BG_SECTION   3          /* measured; see above */
#define BG_W         320        /* image 0x07E87 */
#define BG_H         104        /* image 0x07E8A's cl */
#define BG_PLANES    2          /* image 0x07E8A's ch */
#define BG_ROWS      4          /* image 0x07E7C */
#define BG_STEP      104        /* image 0x07EAC: bx += 0x68 */

void level_results_background(uint8_t *dst, uint16_t dst_w, uint16_t dst_h,
                              const uint8_t *sec, uint32_t sec_len)
{
    const uint16_t bpr = BG_W / 8;
    if ((uint32_t)BG_H * bpr * BG_PLANES > sec_len) return;

    for (uint8_t row = 0; row < BG_ROWS; row++) {
        int32_t y0 = (int32_t)row * BG_STEP;
        for (uint8_t col = 0; col < 2; col++) {          /* 0x07E82, 0x07E95 */
            int32_t x0 = (int32_t)col * BG_W;

            /* THE CLIP AT IMAGE 0x09997, and its comparison is `jl` - so a
             * blit whose right edge lands EXACTLY on the limit still loses a
             * byte. Image 0x08846 selects this path because image 0x07E99
             * passes bp = 0x0C.
             *
             *     cmp bp, [0x66F2] / jl done     image 0x099A1
             *     sub dx, 8                      image 0x099AC
             *
             * The right-hand tile starts at 320 and is 320 wide, so its edge
             * is 640 on a 640-wide screen and the last byte column - 632..639
             * - is never written. That is 1,413 pixels of this screen, and
             * the port filled them until this was read. */
            int32_t w = BG_W;
            while (w > 0 && x0 + w >= (int32_t)dst_w) w -= 8;
            for (uint16_t r = 0; r < BG_H; r++) {
                int32_t y = y0 + r;
                if (y < 0 || y >= (int32_t)dst_h) continue;
                for (int32_t c = 0; c < w; c++) {
                    int32_t x = x0 + c;
                    if (x < 0 || x >= (int32_t)dst_w) continue;
                    uint8_t v = 0;
                    for (uint8_t p = 0; p < BG_PLANES; p++) {
                        uint8_t byte = sec[(uint32_t)p * BG_H * bpr
                                           + (uint32_t)r * bpr + (c >> 3)];
                        v = (uint8_t)(v | (((byte >> (7 - (c & 7))) & 1u) << p));
                    }
                    dst[(size_t)y * dst_w + (size_t)x] = v;
                }
            }
        }
    }
}

/* Image 0x046EB, in its own order. */
void level_results_draw(uint8_t *dst, uint16_t dst_w, uint16_t dst_h,
                        const uint8_t *sec, uint32_t sec_len,
                        const uint8_t *bg, uint32_t bg_len,
                        uint8_t saved, uint8_t needed, int timed_out,
                        uint8_t input_kind, uint8_t level_no,
                        const char *code)
{
    char buf[40];
    results_line ln;

    /* Image 0x046FD, before any text: the tiled background. */
    if (bg) level_results_background(dst, dst_w, dst_h, bg, bg_len);

    /* Image 0x04703. */
    ln.row = RES_HEAD_ROW; ln.style = 0;
    ln.col = timed_out ? RES_HEAD_COL_TIME : RES_HEAD_COL_ALL;
    ln.text = level_results_headline(timed_out);
    results_text(dst, dst_w, dst_h, sec, sec_len, &ln);

    /* Image 0x048BD and image 0x048C7. */
    level_results_line(buf, 0, saved);
    ln.col = RES_RESCUED_COL; ln.row = RES_RESCUED_ROW; ln.text = buf;
    results_text(dst, dst_w, dst_h, sec, sec_len, &ln);
    level_results_line(buf, 1, needed);
    ln.row = RES_NEEDED_ROW;
    results_text(dst, dst_w, dst_h, sec, sec_len, &ln);

    /* Image 0x04798: the verdict, both of its lines. */
    uint8_t v = level_verdict(saved, needed);
    for (uint8_t i = 0; i < 2; i++)
        results_text(dst, dst_w, dst_h, sec, sec_len, &VERDICT_AT[v][i]);

    uint8_t kind = input_kind < 3 ? input_kind : 0;
    if (level_won(saved, needed)) {
        /* Image 0x0474B and image 0x04752: the level number and its code. */
        char lvl[40], is[40];
        memcpy(lvl, "Your Access Code for Level     ", 32);
        put_num3(&lvl[27], level_no);   /* image 0x0474B: ds:0x4D5D */
        ln.col = RES_CODE_COL; ln.row = RES_CODE_ROW; ln.text = lvl;
        results_text(dst, dst_w, dst_h, sec, sec_len, &ln);
        snprintf(is, sizeof is, "is %s", code ? code : "");
        ln.col = RES_CODE2_COL; ln.row = RES_CODE2_ROW; ln.text = is;
        results_text(dst, dst_w, dst_h, sec, sec_len, &ln);
        results_text(dst, dst_w, dst_h, sec, sec_len, &S_NEXT[kind]);
    } else {
        /* Image 0x0477D: retry the same level. */
        results_text(dst, dst_w, dst_h, sec, sec_len, &S_RETRY[kind]);
    }
    /* Image 0x0479F: the menu prompt goes on either way. */
    results_text(dst, dst_w, dst_h, sec, sec_len, &S_MENU[kind]);
}


/* Which of the nine, from image 0x048F6's ladder. `saved` and `needed` are
 * percentages - ds:0x40 and ds:0x41. */
uint8_t level_verdict(uint8_t saved, uint8_t needed)
{
    if (saved == 100) return 8;                       /* image 0x04904 */
    if (saved == 0) return 0;                         /* image 0x0490B */
    if (saved < needed / 2) return 1;                 /* image 0x04912 */
    if (needed >= 5 && saved < (uint8_t)(needed - 5)) return 2;
    if (needed >= 1 && saved < (uint8_t)(needed - 1)) return 3;
    if (needed >= 1 && saved == (uint8_t)(needed - 1)) return 4;
    if (saved == needed) return 5;                    /* image 0x0493A */
    if (saved < (uint8_t)(needed + 20)) return 6;     /* image 0x04943 */
    return 7;
}

/* Transcribed from image 0x04720 - and it WAS in the original after all.
 *
 * This was marked the port's own on the grounds that image 0x048F6's ladder
 * has no pass-or-fail branch, only nine messages. The ladder does not, but
 * the results screen does: image 0x0471D loads ds:0x40, image 0x04720
 * compares it against ds:0x41 and image 0x04724's `jge` is the whole
 * decision. At or above the requirement it sets ds:0x1F6D, advances the
 * level and offers a code; below it, it does not.
 *
 * Found by scanning for a near call to the text assembly at image 0x048BD -
 * exactly one, at image 0x04700 - which is the same scan that found the
 * menu's caller at image 0x04401. Worth remembering next time something is
 * about to be marked the port's own for want of looking one level up. */
int level_won(uint8_t saved, uint8_t needed) { return saved >= needed; }

/* WHAT WINNING DOES, image 0x04729 onwards: it sets the passed flag at
 * ds:0x1F6D, advances ds:0x1F6B - the level index across the whole game -
 * and recomputes the rating from it as index / 30. Image 0x0473E then caps
 * the index at 0x78, which is 120: four ratings of thirty. */
#define LEVELS_PER_RATING 30      /* image 0x04737: cl = 0x1E */
#define LEVELS_TOTAL      120     /* image 0x0473E: cmp against 0x78 */

/* Transcribed from image 0x0472E. Returns non-zero while there is another
 * level; image 0x04745 leaves for the ending when there is not. */
int level_advance(uint8_t *index, uint8_t *rating)
{
    (*index)++;                                   /* image 0x0472E */
    *rating = (uint8_t)(*index / LEVELS_PER_RATING);
    return *index < LEVELS_TOTAL;                 /* image 0x0473E */
}

const char *level_verdict_text(uint8_t saved, uint8_t needed)
{
    return VERDICT[level_verdict(saved, needed)];
}

/* HOW MANY GOT OUT, and whether the level is over.
 *
 * ds:0x46 is the saved count and ds:0x47 the total - image 0x03ACE divides
 * one by the other for the percentage on the panel. Image 0x02335 is what
 * increments it: the exiting handler at image 0x02326 animates eight frames
 * and then, on the frame the counter wraps, adds one and marks the lemming
 * gone.
 *
 * The level ENDS on either of two conditions, both at image 0x001C8:
 *
 *   image 0x001E7  ds:0x43 == ds:0x47 and ds:0x45 == 0 - everyone has been
 *                  released and none is still active
 *   image 0x001FA  ds:0x20A7, ds:0x20A9 and ds:0x20AA are all '0' - the
 *                  clock has run out, which it tests on the DIGITS rather
 *                  than on a counter
 *
 * Both jump to image 0x02B3. Which of them it was does not decide win or
 * lose: that is the saved count against the level's requirement, and it is
 * the results screen's business.
 */
uint8_t level_saved;              /* ds:0x46 */

void level_saved_reset(void) { level_saved = 0; }

/* Image 0x02335. */
void level_lemming_saved(void) { level_saved++; }

/* Image 0x03ACE: the saved count over the total. */
uint8_t level_saved_percent(const level *lv)
{
    if (!lv->num_lemmings) return 0;
    return (uint8_t)((uint32_t)level_saved * 100u / lv->num_lemmings);
}

/* Transcribed from image 0x001C8. `live` is the count still active, which
 * image 0x001F0 reads from ds:0x45. */
int level_over(const level *lv, uint8_t released, uint8_t live)
{
    /* Image 0x001EA compares ds:0x43 against ds:0x47, which is a VARIABLE -
     * the nuke at image 0x04013 writes the released count into it so that
     * the entrance stops and this test can be reached. Reading the header's
     * count instead made a nuked level wait for lemmings that would never
     * come. */
    (void)lv;
    if (released >= level_to_release() && live == 0)
        return 1;                          /* image 0x001E7 */
    /* Image 0x001FA ORs the three clock DIGITS and compares against '0' -
     * it tests the characters on the panel, not a counter, which is why
     * level_timer_step's floor of "0", "0", "0" is the end state. */
    const char *st = level_status_line();
    if (st[ST_MIN] == '0' && st[ST_SEC_TENS] == '0'
            && st[ST_SEC_UNITS] == '0')
        return 1;
    return 0;
}

/* THE REQUIREMENT AS A PERCENTAGE - transcribed from image 0x04591.
 *
 * ds:0x41 is what image 0x04720 judges the level by, and it is set when the
 * BRIEFING is composed, not when the results are:
 *
 *     al = ds:0x0FD0      the count to rescue
 *     cl = 0x64 / mul cl  times a hundred, into ax
 *     cl = ds:0x0FCE      the lemmings the level releases
 *     div cl              and the quotient is the percentage
 *
 * Eight-bit throughout - `mul cl` on al, `div cl` back to al - which is why
 * this takes uint8_t rather than the header's uint16_t fields. main.c had
 * the same arithmetic written out inline and no address against it; a guess
 * that happens to be right is still a guess, and it is the sort that stays
 * right until a level with different numbers comes along.
 */
uint8_t level_needed_percent(const level *lv)
{
    uint8_t total = (uint8_t)lv->num_lemmings;
    if (!total) return 0;
    uint16_t ax = (uint16_t)((uint8_t)lv->num_to_rescue * 100u);
    return (uint8_t)(ax / total);
}

/* DID THE CLOCK RUN OUT - transcribed from image 0x04710.
 *
 * The results screen does not take this as a flag from whoever called it.
 * Image 0x04707 reads ds:0x20A7 and ds:0x20A9, ORs them together and
 * compares the pair against 0x3030 - "00" - so what it is asking is whether
 * the panel's own clock reads zero. The port had main.c passing a constant
 * 0, which put "All lemmings accounted for." on a level that had timed out.
 *
 * Same three characters image 0x001FA tests in level_over above; asking them
 * here rather than threading a flag through is what keeps the two answers
 * from drifting apart. */
int level_out_of_time(void)
{
    const char *st = level_status_line();
    return st[ST_MIN] == '0' && st[ST_SEC_TENS] == '0'
        && st[ST_SEC_UNITS] == '0';
}

/* Transcribed from image 0x03AF7: the panel's IN reading, with 100 written
 * out in full and no leading zero below it. */
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
#define HIT_X_CAP    0x680       /* image 0x03D4F */
#define HIT_X_PINNED 0xD0        /* image 0x03D58 - and yes, 0xD0 */
#define HIT_Y_CAP    0xB0        /* image 0x03D5E and image 0x03D67 */

/* Image 0x03D7F: a lemming already doing a job. The bits are 0x8000, and
 * MINER 0x800, BASHER 0x200, BLOCKER 0x100, BUILDER 0x80 and DIGGER 0x10. */
#define HIT_BUSY     0x8B90

/* ds:0x14, the mouse buttons as image 0x04269 stores them. Bit 1 is what
 * image 0x03DB7 tests, and it PICKS THE PLAIN LEMMING over the busy one -
 * which is how the original lets you point past a worker at the walker
 * beside it. The port has no second button wired to this yet, so it reads
 * zero and always takes the busy one, exactly as the original does with the
 * button up. */
uint8_t lem_buttons;

/* ds:0x63 - HOW MANY lemmings are under the pointer, which is the number the
 * panel prints beside the name. Image 0x03DA1 counts them. */
uint8_t lem_under_count;

/* THE POINTER'S SCAN - transcribed from image 0x03CE9.
 *
 * The original does not simply take the first hit. It walks the whole array
 * keeping TWO candidates - image 0x03D8E remembers one that is already doing
 * a job and image 0x03D9C one that is not - and image 0x03DB4 then chooses
 * between them by the mouse buttons. It also counts every lemming it passed
 * through, at image 0x03DA1, because that count is what the panel shows.
 *
 * The port used to return the first hit and nothing else. tools/verify_hit.py
 * could not see the difference: it compares ds:0x1A, which says only WHETHER
 * something was found, so picking the wrong lemming out of an overlapping
 * pair scored as a perfect match. The scan now returns what the original's
 * own ds:0x5E, ds:0x5F, ds:0x60 and ds:0x63 hold, and the verifier compares
 * the chosen INDEX.
 */
void level_pointer_scan(const lemming *lem, uint8_t n, uint16_t view_x,
                        level_scan *out)
{
    /* Image 0x03CF6 and image 0x03CFB: the count and both candidates start
     * empty, every frame. */
    out->count = 0;
    out->busy = out->plain = out->chosen = -1;

    /* Image 0x03D0A reads `[0] - 0x10 + [0x10]`, and ds:0 is the GUEST's
     * scroll - which is sixteen more than this port's `view_x`: the guest
     * reads 640 where tools/compare_run.py fits 624, the same sixteen
     * tools/prove.sh prints as its offset. So the two sixteens cancel and
     * the pointer's record x is simply `view_x + mouse_x`.
     *
     * Subtracting sixteen here as well put every hit test sixteen pixels
     * left of the lemming. The unit test passed anyway, because it was
     * written with the GUEST's 640 in it rather than the port's 624 - a
     * test that agrees with the code because both came from one misreading. */
    int32_t px = (int32_t)view_x + (int32_t)lem_mouse_x;
    int32_t py = (int32_t)lem_mouse_y + 8;        /* image 0x03D18 */

    for (uint8_t k = 0; k < n && k < LEMMING_MAX; k++) {
        const lemming *a = &lem[k];
        if (a->alive == 0xFF) continue;          /* image 0x03D1E */

        int32_t bx = (int32_t)a->x + a->hot_x - HIT_BACK_X;   /* 0x03D27 */
        int32_t by = (int32_t)a->y + a->hot_y - HIT_BACK_Y;   /* 0x03D33 */
        if (bx < 0) bx = 0;                      /* image 0x03D31 */
        if (by < 0) by = 0;                      /* image 0x03D3C */
        if (px < bx || py < by) continue;        /* 0x03D3E, 0x03D45 */

        bx += HIT_BOX;                           /* image 0x03D4C */
        if (bx >= HIT_X_CAP) bx = HIT_X_PINNED;  /* image 0x03D4F */
        by += HIT_BOX;                           /* image 0x03D5B */
        if (by >= HIT_Y_CAP) by = HIT_Y_CAP;     /* image 0x03D5E */
        if (px > bx || py > by) continue;        /* 0x03D6A, 0x03D71 */

        /* Image 0x03D79 asks TWO things, and the port asked one of them for
         * as long as it had no record byte 0x24 to ask. It has one now -
         * `extra`, added for the builder's shrug - so this is the whole test:
         *
         *     test byte [si+0x24], 0x80   ; jne -> the busy slot
         *     test word [si+8], 0x8b90    ; je  -> the plain slot
         *
         * The byte matters on its own because image 0x027DE sets bit 7 as a
         * builder runs out of bricks, and the shrug is the one job that is
         * not in the flag word. Without it a shrugging builder is filed as
         * PLAIN, and since each slot keeps the LAST lemming that matched, a
         * walker behind it would lose the click to it or take it instead,
         * depending on which came later in the array. */
        if ((a->extra & 0x80) || (a->flags & HIT_BUSY))
            out->busy = (int16_t)k;                         /* image 0x03D8E */
        else
            out->plain = (int16_t)k;                        /* image 0x03D9C */
        out->count++;                                       /* image 0x03DA1 */
    }

    /* Image 0x03DB4: the button decides, and the busy one wins by default. */
    if (lem_buttons & 2) out->chosen = out->plain;           /* image 0x03DBC */
    else out->chosen = (out->busy >= 0) ? out->busy : out->plain;
    lem_under_count = out->count;
    /* Image 0x03DAF clears ds:0x1A and image 0x03DE8 sets it. */
    lem_cursor_box = (out->chosen >= 0) ? 0xFF : 0;
}

/* Image 0x03DDE reading ds:0x5E: the chosen one, through the scan above. */
int16_t level_lemming_at(const lemming *lem, uint8_t n, uint16_t view_x)
{
    level_scan sc;
    level_pointer_scan(lem, n, view_x, &sc);
    return sc.chosen;
}

#define DIG_BIT        0x0010
#define DIG_KEEP       0xF57F
#define DIG_ANIM_START 0x11
#define DIG_HOT_X      (-8)
#define DIG_HOT_Y      (-12)
/* Image 0x02472 and image 0x02477: 0xfff8 and 0xfff6, the hot spot a digger
 * that has broken through takes on. */
#define FALL_HOT_X     (-8)
#define FALL_HOT_Y     (-10)
/* Image 0x02424: past this row it has dug its way out of the level. */
#define DIG_FLOOR      0xB4
/* Image 0x0242D loads 0x0FF8 and image 0x02432 shifts it by the low three of
 * the x it probes, which is x + hot_x.
 *
 * 0x0FF8 has its VALUE bits 3..11 set, and in a bit mask the leftmost pixel
 * is the most significant bit - so pixel i is bit 15 - i, and bits 3..11 are
 * PIXELS 4..12. The span is therefore (x + hot_x) + 4, nine wide: x-4 to
 * x+4 with hot_x at -8.
 *
 * Reading those as pixels 3..11 instead put the whole shaft one column left.
 * It is invisible in the lemming's state and in every frame comparison -
 * what showed it was laying the original's dug map beside the port's, where
 * one is ########.........####### and the other ends #######..........####. */
#define DIG_WIDTH      9
#define DIG_FROM       (-4)

/* Transcribed from image 0x0320D. The mask it clears is 0xF57F - every state
 * a digger may not also be in - and the bit it sets is 0x10, whose handler is
 * image 0x023F3. It spends one from the count at ds:0x7F, parks the animation
 * at 0x11 and drops the hot spot two rows. */
/* NOT DRAWN, and here is exactly what is missing.
 *
 * tools/probe_digger.py dumped the original's record either side of the
 * assignment, so both sprites' store fields are measured:
 *
 *              [+0x0A] ink   [+0x0C] mask   [+0x10] bytes   [+0x12] mask at
 *     walker   0             0              0x5A = 90       0x3C = 60
 *     digger   0             0x32A          0x70 = 112      0x54 = 84
 *
 * The walker's 90 decomposes as (2 planes + 1 mask) x 10 rows x 3 bytes,
 * with the mask at 2 x 10 x 3 = 60 - and 10 rows in 2 planes is the geometry
 * the port already draws the walker with, correctly, so that reading is
 * anchored.
 *
 * The digger's 112 is NOT divisible by three, so its store is not that
 * shape, and the arithmetic has more than one answer: three planes over 14
 * rows at 2 bytes each, or three planes over 7 rows at 4. Both give 112 with
 * the mask at 84, and 14 rows of a 16-wide sprite and 7 rows of a 32-wide
 * one are very different pictures.
 *
 * So it is not drawn. Picking one and seeing whether it looks plausible is
 * the fitting this project has been caught by before; what settles it is
 * reading the store, or capturing a frame of the original mid-dig and
 * matching the pixels the way the cursor's shape was matched. Until then a
 * digging lemming is invisible, which is wrong in a way that is obvious
 * rather than wrong in a way that looks fine.
 *
 * Transcribed from image 0x0320D - the assignment itself, which is what the
 * function below is; only the sprite it points the record at is missing.
 */

/* Image 0x0320D. The refusal is image 0x0321D's `test word [si+8], 0xf57f`
 * and it refuses when any of those bits IS SET - the port tested the
 * complement, so a lemming already digging (0x10 is in that mask) passed
 * straight through and spent a second digger on itself. A player found it.
 *
 * The same constant is then used at image 0x0322D as a KEEP mask, which
 * looks contradictory and is not: by the time that AND runs, the test above
 * has already established that none of those bits is set, so it does
 * nothing. Both readings are of the same bytes; only one of them is the
 * guard. */
int lem_dig_start(lemming *a)
{
    if (a->flags & DIG_KEEP) return 0;              /* image 0x0321D */
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
/* One row of the shaft, at `dy` from the lemming. Image 0x0242D loads 0x0FF8
 * - nine bits - and image 0x02432 shifts it by the low three of the x it
 * probes, which is x + hot_x; so the span is that plus three, nine wide.
 * Returns whether anything was there. */
static int dig_row(lemming *a, uint8_t *lvl, uint16_t cw, uint16_t chh,
                   int32_t dy)
{
    int had = 0;
    int32_t cy = (int32_t)a->y + dy - LEMMING_DY;
    for (uint8_t i = 0; i < DIG_WIDTH; i++) {
        int32_t cx = (int32_t)a->x + DIG_FROM + i - LEMMING_DX;
        if (cx < 0 || cx >= (int32_t)cw || cy < 0 || cy >= (int32_t)chh)
            continue;
        if (lvl[(size_t)cy * cw + cx] & 8) had = 1;
        lvl[(size_t)cy * cw + cx] = 0;        /* image 0x05EBA */
    }
    return had;
}

/* Transcribed from image 0x023F3, with its first frame at image 0x0250C.
 * Sixteen animation frames, digging on the two where the counter comes round
 * to 0 and to 8 - image 0x02406 and image 0x02408. */
static void lem_dig(lemming *a, uint8_t *lvl, uint16_t cw, uint16_t chh)
{
    if (a->anim == DIG_ANIM_START) {
        /* THE FIRST FRAME CUTS THREE ROWS, not one. Image 0x0250C drops y
         * by two, probes, and clears - then image 0x02541 adds ds:4, the
         * row stride, and clears AGAIN - before falling into the ordinary
         * dig at image 0x0240F, which does the row it is standing on. So a
         * digger opens y-2, y-1 and y on the frame it starts.
         *
         * Cutting only the last of those leaves the top of the shaft
         * capped, and it is invisible in the lemming's own state: y and the
         * animation come out the same either way. What showed it was
         * comparing the TERRAIN - 34 pixels, all in the two rows above the
         * start. */
        dig_row(a, lvl, cw, chh, -2);
        dig_row(a, lvl, cw, chh, -1);
        a->anim = 0;
    } else {
        a->anim = (uint8_t)((a->anim + 1) & 0x0F);   /* image 0x023FF */
        if (a->anim != 0 && a->anim != 8)
            return;                           /* image 0x0240C: just animate */
    }

    /* THE ROW IT CLEARS IS THE ONE IT IS STANDING ON, not the one it moves
     * to. Image 0x02418 calls image 0x05DC6 BEFORE the `inc [si+2]` at image
     * 0x0241D, and image 0x05DCB builds the destination from the y it sees
     * there - so the clear at image 0x05EBA lands on the old row. Doing it
     * the other way round starts the shaft one row low, which is visible as
     * a lip of terrain left under the lemming's feet. */
    int32_t dy_back = -1;                     /* the row it stood on */

    a->y = (int16_t)(a->y + 1);               /* image 0x0241D */
    if (a->y >= DIG_FLOOR) {                  /* image 0x02424 */
        a->alive = 0xFF;
        return;
    }

    /* Clear the span - and the test is on what WAS THERE, not on what is
     * below it. Image 0x02446 and image 0x0244A test the bits the clear at
     * image 0x05EBA just tried to remove: if none of them were set, the row
     * it dug was already empty and image 0x0244E turns it into a fall.
     *
     * Checking the row BELOW instead is not the same rule and got this
     * wrong at the moment it matters - digging through the last row of a
     * ledge, where the row dug is the last solid one and the row below is
     * already open. */
    int had = dig_row(a, lvl, cw, chh, dy_back);
    if (!had) {
        /* Image 0x0244E: it dug nothing, so there is nothing to stand on.
         *
         * The whole record is re-stocked here, not just the flags. Image
         * 0x0246A puts the animation counter back to zero and image 0x02477
         * moves the hot spot from the digger's -12 to the faller's -10 - and
         * leaving those two alone was worth 24 to 32 pixels a frame for a
         * hundred frames, from the moment it broke through to the moment it
         * reached the exit.
         *
         * Nothing in the state comparison could see it either: x and y
         * agreed on all 600 frames. It is the HOT SPOT that moved, so the
         * same lemming at the same place was drawn two rows out. */
        a->flags = (uint16_t)((a->flags & ~(uint16_t)DIG_BIT) | 4);
        a->anim  = 0;                          /* image 0x0246A */
        a->hot_x = FALL_HOT_X;                 /* image 0x02472 */
        a->hot_y = FALL_HOT_Y;                 /* image 0x02477 */
        a->state = 4;                          /* image 0x0247C */
    }
}

/* THE BRICK - transcribed from image 0x05F0F.
 *
 * Six pixels wide: image 0x05F5D loads 0xFC, which is six bits, and image
 * 0x05F67 shifts it right by the low three of the x it lands on - the same
 * shift-a-mask-into-place the digger's nine-wide span uses at image 0x0242D.
 *
 * WHERE it lands is MEASURED, and it disagrees with the reading. Image
 * 0x05F42 adds [si+4] - the hot spot, which image 0x032E4 set to -8 - and
 * image 0x05F4B takes four MORE off when the direction is negative. By that
 * reading the brick starts at x - 8; measured against the original's own
 * screen it starts at x.
 *
 * The measurement is not in doubt. The builder's record agrees with the
 * original's frame for frame - x, y, animation and flags, all of them - and
 * on the frame it lays its first brick it is at x 794, with the brick
 * landing on canvas columns 778..783, which is 794..799 in record terms.
 * Eight out, and eight is exactly the hot spot, so something either does not
 * add it or adds it back. Rather than write arithmetic that produces the
 * right answer for a reason I have made up, this uses the measured relation
 * and says so; the four for a leftward builder is image 0x05F4B's and has
 * not been reached yet.
 *
 * Image 0x05F51 drops y by one, so the brick is the row above.
 *
 * WHAT it writes: image 0x05F13 sets the Map Mask to 8 - plane 3 alone -
 * which is the plane image 0x05DC6 probes for solidity. So a brick adds the
 * solid bit and leaves the other three planes as they were, and this ORs 8
 * in rather than assigning, exactly as the digger's counterpart clears the
 * byte outright at image 0x05EBA.
 */
#define BRICK_WIDTH  6         /* image 0x05F5D: 0xFC is six bits */
#define BRICK_BACK   4         /* image 0x05F4B, when going left */
#define BRICK_SOLID  8         /* image 0x05F13's Map Mask, plane 3 */

static void lem_build_brick(lemming *a, uint8_t *lvl, uint16_t cw,
                            uint16_t chh)
{
    int32_t bx = (int32_t)a->x;                     /* measured; see above */
    if (a->dir < 0) bx -= BRICK_BACK;               /* image 0x05F4B */
    int32_t cy = (int32_t)a->y - 1 - LEMMING_DY;    /* image 0x05F51 */
    for (uint8_t i = 0; i < BRICK_WIDTH; i++) {
        int32_t cx = bx + i - LEMMING_DX;
        if (cx < 0 || cx >= (int32_t)cw || cy < 0 || cy >= (int32_t)chh)
            continue;
        lvl[(size_t)cy * cw + cx] |= BRICK_SOLID;
    }
}

/* THE BUILDER - transcribed from image 0x032BD (assigning it) and image
 * 0x02693 (doing it).
 *
 * Neither could be reached by playing "Just dig!", which stocks only diggers,
 * so both were written against a run driven with tools/record_level.py
 * --skills - the same poke the port takes through LEM_SKILLS - and checked
 * against what the original's own array did with it.
 *
 * Assigning, image 0x032BD:
 *
 *     0x032BD  refuse if there are none left
 *     0x032C4  refuse if y + hot_y is 0x0A or less - too near the top
 *     0x032CF  refuse if the state word has any of 0xF5EF set
 *     0x032DF  the state word BECOMES 0x80, assigned rather than OR-ed
 *     0x032E4  the hot spot moves to (-8, -13)
 *     0x032F2  TWELVE bricks, at [si+0x21]
 *
 * Doing it, image 0x02693: sixteen animation frames, and everything happens
 * on the one where the counter wraps to zero - image 0x0269D. The recorded
 * original bears that out exactly: assigned at frame 112, walking again at
 * 303, which is 191 frames for twelve bricks at sixteen apiece.
 */
#define BUILD_BIT      0x0080
#define BUILD_KEEP     0xF5EF    /* image 0x032CF */
#define BUILD_BRICKS   12        /* image 0x032F2 */
#define BUILD_HOT_X    (-8)      /* image 0x032E4 */
#define BUILD_HOT_Y    (-13)     /* image 0x032E9 */
#define BUILD_TOP      0x0A      /* image 0x032CA and image 0x02758 */
#define BUILD_X_MIN    0x10      /* image 0x026EF */
#define BUILD_X_MAX    0x680     /* image 0x026F7 */
#define BUILD_STATE    0x10      /* image 0x0330A */
#define BUILD_SHRUG    0x80      /* image 0x027DE, on [si+0x24] */

int lem_build_start(lemming *a)
{
    if ((int32_t)a->y + a->hot_y <= BUILD_TOP) return 0;   /* 0x032C4 */
    if (a->flags & BUILD_KEEP) return 0;                   /* 0x032CF */
    a->flags  = BUILD_BIT;                                 /* 0x032DF */
    a->hot_x  = BUILD_HOT_X;
    a->hot_y  = BUILD_HOT_Y;
    a->anim   = 0;                                         /* 0x032EE */
    a->bricks = BUILD_BRICKS;                              /* 0x032F2 */
    a->state  = BUILD_STATE;                               /* 0x0330A */
    a->extra &= 1;                                         /* 0x03318 */
    return 1;
}

/* Image 0x02766 and image 0x027A5 both put it back to walking; they differ in
 * the state byte and in the shrug bit. */
static void lem_build_stop(lemming *a, uint8_t state, int shrug)
{
    a->flags = 0;                                  /* 0x02766, 0x027A5 */
    a->hot_x = -8;
    a->hot_y = -10;                                /* 0xfff6 */
    a->anim  = 0;
    a->state = state;
    a->extra &= 1;
    if (shrug) a->extra |= BUILD_SHRUG;            /* image 0x027DE */
}

/* Image 0x02693, the handler the dispatch at image 0x01F5E reaches for
 * flags & 0x0080. */
static void lem_build(lemming *a, uint8_t *lvl, uint16_t cw, uint16_t chh)
{
    /* Image 0x02693: sixteen frames, and only the wrap does anything. */
    a->anim = (uint8_t)((a->anim + 1) & 0x0F);
    if (a->anim) {
        /* Image 0x026DE lays the brick on frame 9, and image 0x026A2's
         * frame 10 is a warning noise when three or fewer are left - a
         * deliberate non-goal here, this port has no sound. */
        if (a->anim == 9)
            lem_build_brick(a, lvl, cw, chh);      /* image 0x05F0F */
        return;                                    /* image 0x026DB */
    }

    /* Image 0x026E4: a step up and along, then three probes. */
    int16_t step = a->dir;                         /* [si+0x27] */
    a->x = (int16_t)(a->x + step);                 /* image 0x026E8 */
    a->y = (int16_t)(a->y - 1);                    /* image 0x026EA */

    if (a->x <= BUILD_X_MIN || a->x >= BUILD_X_MAX) {   /* 0x026EF, 0x026F7 */
        a->dir = (int8_t)-a->dir;                  /* image 0x02763 */
        lem_build_stop(a, 9, 0);
        return;
    }
    /* Image 0x02700 and image 0x02710 probe the row ABOVE - image 0x02703
     * takes ds:4, one row's stride, off the address image 0x05DC6 built. */
    if (lem_solid(lvl, cw, chh, a->x, (int16_t)(a->y - 1))) {
        a->dir = (int8_t)-a->dir;
        lem_build_stop(a, 9, 0);
        return;
    }
    a->x = (int16_t)(a->x + step);                 /* image 0x0270E */
    if (lem_solid(lvl, cw, chh, a->x, (int16_t)(a->y - 1))) {
        a->dir = (int8_t)-a->dir;
        lem_build_stop(a, 9, 0);
        return;
    }

    if (--a->bricks == 0) {                        /* image 0x0271E */
        /* Image 0x027A5: out of bricks. It keeps its direction, takes state
         * 8, and sets the bit image 0x03DFD reads to go on calling it a
         * BUILDER on the panel. */
        lem_build_stop(a, 8, 1);
        return;
    }

    /* Image 0x02726: nine rows up, two along - the headroom for the next
     * brick. Image 0x02737 shifts ds:4 by three, so it is eight more rows on
     * top of the one image 0x02735 already took off. */
    if (lem_solid(lvl, cw, chh, (int16_t)(a->x + step * 2),
                  (int16_t)(a->y - 9))) {          /* image 0x02740 */
        a->dir = (int8_t)-a->dir;
        lem_build_stop(a, 9, 0);
        return;
    }
    if (a->x <= BUILD_X_MIN || a->x >= BUILD_X_MAX) {   /* 0x02747, 0x0274C */
        a->dir = (int8_t)-a->dir;
        lem_build_stop(a, 9, 0);
        return;
    }
    if ((int32_t)a->y + a->hot_y <= BUILD_TOP)     /* image 0x02752 */
        lem_build_stop(a, 9, 0);                   /* image 0x02760: no turn */
}

/* CLIMBING - transcribed from image 0x02179 (starting), image 0x0254F (doing
 * it) and image 0x02637 (getting over the top).
 *
 * It starts in the WALKER, not from a click. Image 0x0213C tests bit 0 of
 * [si+0x24] at the point a plain lemming would turn at a wall, and a climber
 * goes to image 0x02179 instead - which ORs 0x20 in rather than assigning,
 * so a climber keeps whatever else it was.
 *
 * The handler is eight frames split in two, image 0x02559:
 *
 *   0..3  image 0x025E1 probes at y - 7 - anim, one row higher each frame.
 *         Wall there means keep climbing. Nothing there means the top has
 *         been reached: image 0x025FD nudges y and image 0x0260D swaps 0x20
 *         for 0x40, the HOIST.
 *   4..7  image 0x02560 drops y by one and probes eight rows up at x - dir.
 *         Something there and image 0x02591 turns it into a fall - 0x20 out,
 *         4 in, the fall counter set to 3.
 *
 * The hoist at image 0x02637 is eight more: the first four lift y by two,
 * image 0x02643 leaves the rest to animate, and image 0x02654 puts it back
 * to walking. Image 0x0262D and image 0x02689 both keep bit 0 of [si+0x24] -
 * a climber stays a climber.
 */
#define CLIMB_BIT   0x0020
#define HOIST_BIT   0x0040

/* Image 0x02179. */
static void lem_climb_begin(lemming *a)
{
    a->anim   = 0;                                  /* image 0x02179 */
    a->flags |= CLIMB_BIT;                          /* image 0x02187: OR */
    a->hot_x  = -8;                                 /* image 0x02195 */
    a->hot_y  = -12;                                /* image 0x0219A: fff4 */
    a->state  = 8;                                  /* image 0x021A9 */
}

/* Image 0x02591: a climber that runs into something falls off - and it does
 * not simply drop. Image 0x025D4 TURNS it and image 0x025DA adds the new
 * direction to x twice, so it comes away from the wall. Two pixels, and
 * without them the record diverges the moment it happens: 862 against the
 * original's 860. */
static void lem_climb_fall(lemming *a)
{
    a->flags = (uint16_t)((a->flags & ~(uint16_t)CLIMB_BIT) | 4); /* 0x02591 */
    a->hot_x = -8;
    a->hot_y = -10;                                 /* image 0x025BA */
    a->anim  = 0;                                   /* image 0x025AD */
    a->fall  = 3;                                   /* image 0x025B1 */
    a->state = 4;                                   /* image 0x025BF */
    a->extra &= 1;                                  /* image 0x025CD */
    a->dir   = (int8_t)-a->dir;                     /* image 0x025D4 */
    a->x     = (int16_t)(a->x + a->dir + a->dir);   /* image 0x025DA, twice */
}

/* Image 0x02552. */
static void lem_climb(lemming *a, const uint8_t *lvl, uint16_t cw,
                      uint16_t chh)
{
    a->anim = (uint8_t)((a->anim + 1) & 7);         /* image 0x02552 */

    if (a->anim <= 3) {                             /* image 0x02559 */
        /* Image 0x025E4: seven rows up, and one more each frame. */
        int16_t probe = (int16_t)(a->y - 7 - a->anim);
        if (lem_solid(lvl, cw, chh, a->x, probe)) return;  /* 0x025F8 */

        /* The top. Image 0x025FD adds two and image 0x02606 takes the
         * animation counter off again. */
        a->y = (int16_t)(a->y + 2 - a->anim);
        a->flags = (uint16_t)((a->flags & ~(uint16_t)CLIMB_BIT) | HOIST_BIT);
        a->anim  = 0;                               /* image 0x02625 */
        a->state = 8;                               /* image 0x02629 */
        a->extra &= 1;                              /* image 0x0262D */
        return;
    }

    a->y = (int16_t)(a->y - 1);                     /* image 0x02560 */
    if ((int32_t)a->y + a->hot_y <= BUILD_TOP) {    /* image 0x02569 */
        lem_climb_fall(a);
        return;
    }
    /* Image 0x02574 probes at x - dir, and image 0x0257F takes eight rows
     * off the address - so eight above. */
    if (lem_solid(lvl, cw, chh, (int16_t)(a->x - a->dir),
                  (int16_t)(a->y - 8)))             /* image 0x0258A */
        lem_climb_fall(a);
}

/* Image 0x02637: over the top and onto the ledge. */
static void lem_hoist(lemming *a)
{
    a->anim = (uint8_t)((a->anim + 1) & 7);         /* image 0x0263A */
    if (a->anim == 0) {                             /* image 0x02641 */
        a->flags = 0;                               /* image 0x02654 */
        a->hot_x = -8;
        a->hot_y = -10;                             /* image 0x02676 */
        a->state = 9;                               /* image 0x0267B */
        a->extra &= 1;                              /* image 0x02689 */
        return;
    }
    if (a->anim >= 5) return;                       /* image 0x02643 */
    a->y = (int16_t)(a->y - 2);                     /* image 0x0264A */
}

/* THE BASHER - transcribed from image 0x034CE (assigning), image 0x0287D
 * (doing it) and image 0x05FAC (taking the wall away).
 *
 * Assigning refuses on 0xF76F, and then asks something no other skill asks:
 * image 0x034DC reads [si+0x2B] - the trigger cell two rows up and two along
 * the way it faces, which image 0x02EFC stores on every lookup - and refuses
 * when it is 7 facing right or 8 facing left. Image 0x034EF diverts 9, which
 * is steel, somewhere else entirely.
 *
 * The counter is THIRTY-TWO, image 0x02880's `and [si+0x28], 0x1F`, and its
 * low nibble splits the work:
 *
 *     2..5        image 0x028A1 takes a slice of wall away, index al - 2
 *     0x0B..0x1F  image 0x0293A walks it forward one and drops it if there
 *                 is nothing underneath
 *     otherwise   nothing happens
 *
 * The masks are in the same resource as the crater's, at offset 0: image
 * 0x06025 adds slice * 240 and shift * 30, and image 0x0600B adds 0x3C0 for
 * a basher facing left. Two directions of four slices of eight shifts of ten
 * rows of three bytes. Only shift 0 is kept, for the reason the crater's is.
 */
static const uint32_t BASH_MASK[2][4][10] = {
  { /* facing right */
    {   /* right, slice 0 */
        0x000000,  /* ........................ */
        0x00FC00,  /* ........######.......... */
        0x00FE00,  /* ........#######......... */
        0x00F800,  /* ........#####........... */
        0x00E000,  /* ........###............. */
        0x00E000,  /* ........###............. */
        0x00E000,  /* ........###............. */
        0x00E000,  /* ........###............. */
        0x00E000,  /* ........###............. */
        0x00C000,  /* ........##.............. */
    },
    {   /* right, slice 1 */
        0x000000,  /* ........................ */
        0x007C00,  /* .........#####.......... */
        0x003E00,  /* ..........#####......... */
        0x001E00,  /* ...........####......... */
        0x001C00,  /* ...........###.......... */
        0x001C00,  /* ...........###.......... */
        0x000000,  /* ........................ */
        0x000000,  /* ........................ */
        0x000000,  /* ........................ */
        0x000000,  /* ........................ */
    },
    {   /* right, slice 2 */
        0x000000,  /* ........................ */
        0x007C00,  /* .........#####.......... */
        0x003E00,  /* ..........#####......... */
        0x001F00,  /* ...........#####........ */
        0x001F00,  /* ...........#####........ */
        0x001E00,  /* ...........####......... */
        0x001C00,  /* ...........###.......... */
        0x000C00,  /* ............##.......... */
        0x000000,  /* ........................ */
        0x000000,  /* ........................ */
    },
    {   /* right, slice 3 */
        0x000000,  /* ........................ */
        0x007C00,  /* .........#####.......... */
        0x003E00,  /* ..........#####......... */
        0x001F00,  /* ...........#####........ */
        0x001F00,  /* ...........#####........ */
        0x001F00,  /* ...........#####........ */
        0x001F00,  /* ...........#####........ */
        0x001F00,  /* ...........#####........ */
        0x001F00,  /* ...........#####........ */
        0x003E00,  /* ..........#####......... */
    },
  },
  { /* facing left */
    {   /* left, slice 0 */
        0x000000,  /* ........................ */
        0x1F8000,  /* ...######............... */
        0x3F8000,  /* ..#######............... */
        0x0F8000,  /* ....#####............... */
        0x038000,  /* ......###............... */
        0x038000,  /* ......###............... */
        0x038000,  /* ......###............... */
        0x038000,  /* ......###............... */
        0x038000,  /* ......###............... */
        0x018000,  /* .......##............... */
    },
    {   /* left, slice 1 */
        0x000000,  /* ........................ */
        0x1F0000,  /* ...#####................ */
        0x3E0000,  /* ..#####................. */
        0x3C0000,  /* ..####.................. */
        0x1C0000,  /* ...###.................. */
        0x1C0000,  /* ...###.................. */
        0x000000,  /* ........................ */
        0x000000,  /* ........................ */
        0x000000,  /* ........................ */
        0x000000,  /* ........................ */
    },
    {   /* left, slice 2 */
        0x000000,  /* ........................ */
        0x1F0000,  /* ...#####................ */
        0x3E0000,  /* ..#####................. */
        0x7C0000,  /* .#####.................. */
        0x7C0000,  /* .#####.................. */
        0x3C0000,  /* ..####.................. */
        0x1C0000,  /* ...###.................. */
        0x180000,  /* ...##................... */
        0x000000,  /* ........................ */
        0x000000,  /* ........................ */
    },
    {   /* left, slice 3 */
        0x000000,  /* ........................ */
        0x1F0000,  /* ...#####................ */
        0x3E0000,  /* ..#####................. */
        0x7C0000,  /* .#####.................. */
        0x7C0000,  /* .#####.................. */
        0x7C0000,  /* .#####.................. */
        0x7C0000,  /* .#####.................. */
        0x7C0000,  /* .#####.................. */
        0x7C0000,  /* .#####.................. */
        0x3E0000,  /* ..#####................. */
    },
  },
};

#define BASH_BIT     0x0200
#define BASH_KEEP    0xF76F     /* image 0x034D5 */
#define BASH_MASKOFF 0xF56F     /* image 0x034FF */
#define BASH_ROWS    10         /* image 0x0602C: cx = 0x0A */
#define BASH_STATE   0x20       /* image 0x0352B */

int lem_bash_start(lemming *a)
{
    if (a->flags & BASH_KEEP) return 0;             /* image 0x034D5 */
    uint8_t ahead = (uint8_t)(a->tahead & 0x0F);    /* image 0x034DC */
    uint8_t wall = (a->dir == 1) ? 7 : 8;           /* image 0x034E3 */
    if (ahead == wall) return 0;                    /* image 0x034ED */
    if (ahead == 9) return 0;                       /* image 0x034EF: steel */
    a->flags = (uint16_t)((a->flags & BASH_MASKOFF) | BASH_BIT);
    a->hot_x = -8;                                  /* image 0x03509 */
    a->hot_y = -10;                                 /* image 0x0350E */
    a->anim  = 0;                                   /* image 0x03513 */
    a->state = BASH_STATE;                          /* image 0x0352B */
    a->extra &= 1;                                  /* image 0x03539 */
    return 1;
}

/* Image 0x05FAC: ten rows from y + hot_y - 0x10, the span starting at
 * x + hot_x, cleared through the Set/Reset path so all four planes go. */
static void lem_bash_slice(lemming *a, uint8_t slice, uint8_t *lvl,
                           uint16_t cw, uint16_t chh)
{
    const uint32_t *m = BASH_MASK[a->dir < 0 ? 1 : 0][slice & 3];
    int32_t x0 = (int32_t)a->x + a->hot_x - LEMMING_DX;   /* image 0x05FF4 */
    int32_t y0 = (int32_t)a->y + a->hot_y - LEMMING_DY;   /* image 0x05FE6 */
    for (uint8_t r = 0; r < BASH_ROWS; r++) {
        int32_t y = y0 + r;
        if (y < 0 || y >= (int32_t)chh) continue;
        for (uint8_t c = 0; c < 24; c++) {
            if (!((m[r] >> (23 - c)) & 1u)) continue;
            int32_t x = x0 + c;
            if (x < 0 || x >= (int32_t)cw) continue;
            lvl[(size_t)y * cw + x] = 0;            /* image 0x06041 */
        }
    }
}

/* Image 0x0287D. */
static void lem_bash(lemming *a, uint8_t *lvl, uint16_t cw, uint16_t chh)
{
    a->anim = (uint8_t)((a->anim + 1) & 0x1F);      /* image 0x02880 */
    uint8_t al = (uint8_t)(a->anim & 0x0F);         /* image 0x02887 */

    if (al >= 0x0B) {                               /* image 0x02889 */
        /* Image 0x0293A: forward one, and down if there is nothing there. */
        a->x = (int16_t)(a->x + a->dir);
        if (a->x <= 0x10 || a->x >= 0x680) {        /* 0x02940, 0x02945 */
            a->dir = (int8_t)-a->dir;
            return;
        }
        for (uint8_t k = 0; k < 3; k++) {
            if (lem_solid(lvl, cw, chh, a->x, (int16_t)(a->y + k)))
                return;                             /* image 0x02953 */
            a->y = (int16_t)(a->y + 1);             /* image 0x02958 */
        }
        return;
    }
    if (al < 2 || al > 5) return;                   /* 0x02890, 0x02897 */
    lem_bash_slice(a, (uint8_t)(al - 2), lvl, cw, chh);   /* image 0x028A1 */

    if (a->anim != 5) return;                       /* image 0x028A7 */

    /* IS THERE ANYTHING LEFT TO BASH - image 0x028AE, and without it a
     * basher never stops. The original bashed for FOUR frames on this level
     * and the port for 288, which is what said this test was missing rather
     * than mis-transcribed.
     *
     * Four columns, starting EIGHT ahead - image 0x028BC multiplies the
     * direction by eight - and each one row-tested at y - 6 (image
     * 0x028B3). Anything solid in any of them and it carries on; nothing in
     * all four and image 0x028FE puts it back to walking. */
    int32_t probe_y = (int32_t)a->y - 6;            /* image 0x028B3 */
    for (uint8_t k = 0; k < 4; k++) {
        int32_t px = (int32_t)a->x + a->dir * (8 + k);
        if (lem_solid(lvl, cw, chh, (int16_t)px, (int16_t)probe_y))
            return;                                 /* image 0x028CC */
    }
    a->flags = 0;                                   /* image 0x028FE */
    a->hot_x = -8;
    a->hot_y = -10;
    a->anim  = 0;
    a->state = 9;
    a->extra &= 1;
}

/* ------------------------------------------------------------------ miner */

#define MINE_ROWS    13         /* 312 bytes a slice / 8 shifts / 3 = 13 */

/* THE MINER'S MASKS, read out of the original's own resource at
 * ds:0x1FE8 + 0x780 - image 0x06085's base, which is exactly where the
 * basher's 1,920 bytes end. Image 0x06085 indexes slice * 312 and image
 * 0x060E2 adds 0x270 for one facing left: two directions of two slices of
 * eight shifts of THIRTEEN rows of three bytes. Only shift 0 is kept, for
 * the reason BASH_MASK keeps only shift 0 - the other seven are the same
 * shape pre-shifted for a byte-addressed plane, and this port's map is a
 * byte a pixel. Dumped by tools/verify_results.py, not read by eye. */
static const uint32_t MINE_MASK[2][2][MINE_ROWS] = {
  { /* facing right */
    {   /* right, slice 0 */
        0x01F800,  /* .......######........... */
        0x01FC00,  /* .......#######.......... */
        0x01FE00,  /* .......########......... */
        0x01FE00,  /* .......########......... */
        0x01FE00,  /* .......########......... */
        0x01F800,  /* .......######........... */
        0x01C000,  /* .......###.............. */
        0x018000,  /* .......##............... */
        0x018000,  /* .......##............... */
        0x018000,  /* .......##............... */
        0x018000,  /* .......##............... */
        0x018000,  /* .......##............... */
        0x000000,  /* ........................ */
    },
    {   /* right, slice 1 */
        0x000000,  /* ........................ */
        0x000000,  /* ........................ */
        0x000200,  /* ..............#......... */
        0x000200,  /* ..............#......... */
        0x007F00,  /* .........#######........ */
        0x00FF00,  /* ........########........ */
        0x00FF00,  /* ........########........ */
        0x00FF00,  /* ........########........ */
        0x00FF00,  /* ........########........ */
        0x00FF00,  /* ........########........ */
        0x00FF00,  /* ........########........ */
        0x00FE00,  /* ........#######......... */
        0x003C00,  /* ..........####.......... */
    },
  },
  { /* facing left */
    {   /* left, slice 0 */
        0x0FC000,  /* ....######.............. */
        0x1FC000,  /* ...#######.............. */
        0x3FC000,  /* ..########.............. */
        0x3FC000,  /* ..########.............. */
        0x3FC000,  /* ..########.............. */
        0x0FC000,  /* ....######.............. */
        0x01C000,  /* .......###.............. */
        0x00C000,  /* ........##.............. */
        0x00C000,  /* ........##.............. */
        0x00C000,  /* ........##.............. */
        0x00C000,  /* ........##.............. */
        0x00C000,  /* ........##.............. */
        0x000000,  /* ........................ */
    },
    {   /* left, slice 1 */
        0x000000,  /* ........................ */
        0x000000,  /* ........................ */
        0x200000,  /* ..#..................... */
        0x200000,  /* ..#..................... */
        0x7F0000,  /* .#######................ */
        0x7F8000,  /* .########............... */
        0x7F8000,  /* .########............... */
        0x7F8000,  /* .########............... */
        0x7F8000,  /* .########............... */
        0x7F8000,  /* .########............... */
        0x7F8000,  /* .########............... */
        0x3F8000,  /* ..#######............... */
        0x1E0000,  /* ...####................. */
    },
  },
};

#define MINE_BIT     0x0800
/* THE SAME CONSTANT IS THE GUARD AND THE KEEP-MASK, image 0x035A2 and image
 * 0x035D8 - the trap the digger's assignment was got wrong on. Its zero bits
 * are 0x10, 0x80 and 0x200, so a digger, a builder or a basher may be turned
 * into a miner and a miner may not be told to mine again. */
#define MINE_KEEP    0xFD6F
#define MINE_STATE   0x18       /* image 0x03604 */
#define MINE_CYCLE   0x18       /* image 0x02B3A */

/* Image 0x03598. */
int lem_mine_start(lemming *a)
{
    if (a->flags & MINE_KEEP) return 0;             /* image 0x035A2 */
    /* Image 0x035AC, and NO other skill asks this one: the cell UNDER it. A
     * miner cuts downwards, so steel underfoot refuses the assignment where
     * for a basher it is steel AHEAD that does. */
    if ((a->tcell & 0x0F) == 9) return 0;
    uint8_t ahead = (uint8_t)(a->tahead & 0x0F);    /* image 0x035B5 */
    uint8_t wall = (a->dir == 1) ? 7 : 8;           /* image 0x035BC */
    if (ahead == wall) return 0;                    /* image 0x035C4 */
    if (ahead == 9) return 0;                       /* image 0x035C8 -> 0x03287 */
    a->flags = (uint16_t)((a->flags & MINE_KEEP) | MINE_BIT);
    a->hot_x = -8;                                  /* image 0x035E2 */
    a->hot_y = -13;                                 /* image 0x035E7: fff3, and
                                                     * three lower than every
                                                     * other skill's */
    a->anim  = 0;                                   /* image 0x035EC */
    a->state = MINE_STATE;                          /* image 0x03604 */
    a->extra &= 1;                                  /* image 0x03612 */
    /* AND THE ASSIGNMENT ITSELF DROPS IT A ROW - image 0x03616, the last
     * instruction before the jump out, and no other skill's does. Without it
     * every one of the miner's 288 frames was exactly one row high while x,
     * the flag word and the animation counter all tracked perfectly: a
     * residue with one constant in it and nothing else, which is what said
     * to go back and read the tail of the routine rather than the handler. */
    a->y = (int16_t)(a->y + 1);
    return 1;
}

/* Image 0x0606B: thirteen rows from y + hot_y - 0x10, the span starting at
 * x + hot_x - and for the second stroke image 0x060AD moves the origin one
 * row down and one column along, which is half of the miner's diagonal. */
static void lem_mine_slice(lemming *a, uint8_t slice, uint8_t *lvl,
                           uint16_t cw, uint16_t chh)
{
    const uint32_t *m = MINE_MASK[a->dir < 0 ? 1 : 0][slice & 1];
    int32_t x0 = (int32_t)a->x;                     /* image 0x060A8 */
    int32_t y0 = (int32_t)a->y;
    if (a->anim == 2) {                             /* image 0x060AD */
        y0 += 1;                                    /* image 0x060B3 */
        x0 += a->dir;                               /* image 0x060B8 */
    }
    y0 += a->hot_y - LEMMING_DY;                    /* image 0x060BA */
    x0 += a->hot_x - LEMMING_DX;                    /* image 0x060C9 */
    for (uint8_t r = 0; r < MINE_ROWS; r++) {
        int32_t y = y0 + r;
        if (y < 0 || y >= (int32_t)chh) continue;
        for (uint8_t c = 0; c < 24; c++) {
            if (!((m[r] >> (23 - c)) & 1u)) continue;
            int32_t x = x0 + c;
            if (x < 0 || x >= (int32_t)cw) continue;
            lvl[(size_t)y * cw + x] = 0;
        }
    }
}

/* Image 0x02CBB: round it goes, and back to walking. Not the same as the
 * basher's edge case, which only negates the direction. */
static void lem_mine_turn(lemming *a)
{
    a->dir   = (int8_t)-a->dir;                     /* image 0x02CBB */
    a->flags = 0;                                   /* image 0x02CBE */
    a->anim  = 0;                                   /* image 0x02CD7 */
    a->hot_x = -8;                                  /* image 0x02CDB */
    a->hot_y = -10;
    a->state = 9;                                   /* image 0x02CE5 */
    a->extra &= 1;                                  /* image 0x02CF3 */
}

/* Image 0x02B66, the flat move, and image 0x02BF1, which also drops a row.
 * Both move x by the direction TWICE, checking the level's edges after each;
 * then they ask what is under the lemming, and one with nothing under it
 * becomes a faller. */
static void lem_mine_move(lemming *a, uint8_t drop, uint8_t *lvl,
                          uint16_t cw, uint16_t chh)
{
    for (uint8_t k = 0; k < 2; k++) {
        a->x = (int16_t)(a->x + a->dir);            /* image 0x02B6A */
        if (a->x < 0x10 || a->x >= 0x680) {         /* 0x02B6C, 0x02B74 */
            lem_mine_turn(a);                       /* image 0x02CBB */
            return;
        }
    }
    if (drop) {
        a->y = (int16_t)(a->y + 1);                 /* image 0x02C1B */
        if (a->y >= LEMMING_DEAD_Y) {               /* image 0x02C1E */
            a->alive = 0xFF;
            return;
        }
    }
    if (!lem_solid(lvl, cw, chh, a->x, a->y)) {     /* image 0x02B90 */
        /* Image 0x02B9D: nothing underneath, so it falls. */
        a->flags |= 4;
        a->anim  = 0;
        a->fall  = 3;                               /* image 0x02BB9 */
        a->hot_x = -8;                              /* image 0x02BBD */
        a->hot_y = -10;
        a->state = 4;                               /* image 0x02BC7 */
        a->extra &= 1;                              /* image 0x02BD5 */
        /* AND IT STOPS BEING A MINER - image 0x02BD9, one instruction past
         * the `and [si+0x24],1` every other conversion here ends on, which
         * is exactly where the reading stopped the first time. The flag word
         * came out 0x804 against the original's 4 for ten frames, and the
         * lemming fell at the right speed to the right place with the wrong
         * word in it the whole way. */
        a->flags &= 0xF7FF;
        return;
    }
    /* Image 0x02C76: what it is standing on NOW, read fresh rather than out
     * of the [si+0x2C] the previous frame left - the same cell arithmetic
     * level_trigger_at does. */
    uint8_t cell = (uint8_t)(level_trigger_cell((int32_t)a->x / TRIG_CELL,
                        ((int32_t)a->y - TRIG_Y_BIAS) / TRIG_CELL) & 0x0F);
    if (cell == 9) {                                /* image 0x02C9B: steel */
        lem_mine_turn(a);                           /* image 0x02CFA's jmp */
        return;
    }
    if (cell == 7) {                                /* image 0x02C9F */
        if (a->dir >= 0) lem_mine_turn(a);          /* image 0x02CA8 */
        return;
    }
    if (cell == 8) {                                /* image 0x02CAD */
        /* Image 0x02CB9 is `jns` to the instruction AFTER it, so a miner on
         * an eight turns whichever way it faces. Transcribed as it is rather
         * than as the mirror of the seven above that it reads like it was
         * meant to be. */
        lem_mine_turn(a);
        return;
    }
}

/* Image 0x02B35. A counter of TWENTY-FOUR - image 0x02B3A wraps it at 0x18
 * rather than masking it, which is why the miner's cycle is not a power of
 * two like the basher's - and its VALUE dispatches, not its low nibble:
 *
 *     0      image 0x02BE1  down one, and dead past 0xB4
 *     1, 2   image 0x02B60  a stroke of the pick, slice al - 1
 *     3      image 0x02BF1  along two, and down one
 *     0x0F   image 0x02B66  along two
 *
 * Four columns and two rows a cycle: the 2:1 diagonal a miner cuts. */
static void lem_mine(lemming *a, uint8_t *lvl, uint16_t cw, uint16_t chh)
{
    uint8_t al = (uint8_t)(a->anim + 1);            /* image 0x02B35 */
    if (al == MINE_CYCLE) al = 0;                   /* image 0x02B3A */
    a->anim = al;                                   /* image 0x02B40 */

    if (al == 0) {                                  /* image 0x02B43 */
        a->y = (int16_t)(a->y + 1);                 /* image 0x02BE1 */
        if (a->y >= LEMMING_DEAD_Y) a->alive = 0xFF;
        return;
    }
    if (al == 0x0F) {                               /* image 0x02B4A */
        lem_mine_move(a, 0, lvl, cw, chh);
        return;
    }
    if (al == 3) {                                  /* image 0x02B51 */
        lem_mine_move(a, 1, lvl, cw, chh);
        return;
    }
    if (al > 3) return;                             /* image 0x02B58 */
    lem_mine_slice(a, (uint8_t)(al - 1), lvl, cw, chh);   /* image 0x02B5D */
}

/* THE CRATER - transcribed from image 0x05DF8, its shape MEASURED out of the
 * original's own memory.
 *
 * Image 0x05E17 clears twenty-two rows and image 0x05E1F starts them thirty
 * above the lemming; image 0x05E41 takes the byte from (x - 8) >> 3 and image
 * 0x05E60 the shift from the low three bits of x - 8. The masks live at
 * offset 0xC60 of the resource at ds:0x1FE8, indexed shift * 66 + row * 3 -
 * eight shifts of twenty-two rows of three bytes, 528 in all.
 *
 * They are in no .dat: the table is BUILT at run time, pre-shifted, the way
 * the cursor's store is. So the shape below was dumped out of the guest and
 * only shift 0 is kept - the port writes bytes rather than planes and has no
 * use for the other seven, which are this one moved along.
 *
 * The clear goes through the Set/Reset path with all four planes enabled
 * (image 0x05DFD and image 0x05E05), so it removes terrain outright, the way
 * image 0x05EBA does for the digger.
 *
 * THE ROW OFFSET DOES NOT TAKE LEMMING_DY. Image 0x05DC6 builds a row as
 * y - 0x10 and everything that probes terrain goes through it, so the port's
 * canvas row is y - 16 and its helpers say so. Image 0x05DF8 does its OWN
 * arithmetic - y - 0x1E - and that thirty already includes the sixteen.
 *
 * Checked against the original's own terrain once the cloud has cleared, at
 * frame 270: every row from 62 to 90 is IDENTICAL. */
#define CRATER_ROWS  22         /* image 0x05E17: cx = 0x16 */
#define CRATER_UP    0x1E       /* image 0x05E1F */
#define CRATER_BACK  8          /* image 0x05E41 */
static const uint16_t CRATER[CRATER_ROWS] = {
    0x07C0,   /* .....#####...... */
    0x0FF0,   /* ....########.... */
    0x1FF8,   /* ...##########... */
    0x1FF8,   /* ...##########... */
    0x3FFC,   /* ..############.. */
    0x3FFC,   /* ..############.. */
    0x7FFE,   /* .##############. */
    0x7FFE,   /* .##############. */
    0x7FFE,   /* .##############. */
    0x7FFF,   /* .############### */
    0xFFFF,   /* ################ */
    0xFFFF,   /* ################ */
    0xFFFF,   /* ################ */
    0xFFFF,   /* ################ */
    0xFFFF,   /* ################ */
    0xFFFF,   /* ################ */
    0x7FFE,   /* .##############. */
    0x7FFE,   /* .##############. */
    0x7FFE,   /* .##############. */
    0x3FFC,   /* ..############.. */
    0x1FF0,   /* ...#########.... */
    0x07E0,   /* .....######..... */
};

/* Image 0x05E73's masks, called from image 0x023B9. */
static void lem_crater(lemming *a, uint8_t *lvl, uint16_t cw, uint16_t chh)
{
    int32_t x0 = (int32_t)a->x - CRATER_BACK - LEMMING_DX;
    /* NO LEMMING_DY HERE, and that was the whole of it. Image 0x05DC6 builds
     * a row as y - 0x10 and everything that probes terrain goes through it,
     * so the port's canvas row is y - 16 and its helpers say so. Image
     * 0x05DF8 does its OWN arithmetic - y - 0x1E - and that thirty already
     * includes the sixteen. Subtracting LEMMING_DY as well put the crater
     * sixteen rows too high, which on this level is open sky: it cleared
     * nothing, and changing where it started sideways changed nothing
     * either, which is what said the routine was not landing on ground at
     * all rather than landing in the wrong place. */
    int32_t y0 = (int32_t)a->y - CRATER_UP;
    for (uint8_t r = 0; r < CRATER_ROWS; r++) {
        int32_t y = y0 + r;
        if (y < 0 || y >= (int32_t)chh) continue;
        for (uint8_t c = 0; c < 16; c++) {
            if (!((CRATER[r] >> (15 - c)) & 1u)) continue;
            int32_t x = x0 + c;
            if (x < 0 || x >= (int32_t)cw) continue;
            lvl[(size_t)y * cw + x] = 0;            /* image 0x05E90 */
        }
    }
}

/* THE SPLAT - transcribed from image 0x022DF. Sixteen frames and the lemming
 * is gone; image 0x022EE sets [si+0x26]. Image 0x02250 is what sends it here:
 * a fall counter over 0x3C, and image 0x02273 also clears the direction and
 * image 0x02277 the fuse, so a bomber that lands too hard does not explode. */
static void lem_splat(lemming *a)
{
    a->anim = (uint8_t)((a->anim + 1) & 0x0F);      /* image 0x022E2 */
    if (a->anim) return;                            /* image 0x022E9 */
    a->alive = 0xFF;                                /* image 0x022EE */
}

/* FLOATING - transcribed from image 0x02A6D, and it is the only state in
 * this game driven by a TABLE rather than by arithmetic.
 *
 * ds:0x2698 holds pairs. Image 0x02A73 takes the first as the animation
 * frame and image 0x02A78 the second as what to do about height, then image
 * 0x02A7B steps the pointer at [si+0x0E] on by two:
 *
 *     3   image 0x02A92  probe, then fall up to two rows, stopping on ground
 *     2   image 0x02ABE  the same, but the two probes are separate
 *     1   image 0x02ADC  probe and fall one
 *     otherwise          image 0x02A8C adds the value straight to y, which
 *                        is how 0 holds it still and 0xFF lifts it
 *
 * The umbrella opens in the FALLER: image 0x021BA wants the fall counter past
 * 0x10 before image 0x021C0 even asks whether [si+0x25] is set. Nothing on
 * "Just dig!" falls that far, which is why this could not be reached from the
 * first level at all - see the note on the floater's 400 of 400.
 */
#define FLOAT_BIT     0x0400
#define FLOAT_TABLE   0x2698    /* ds: image 0x02A6D */
#define FLOAT_MIN_FALL 0x10     /* image 0x021BA */

/* Transcribed from ds:0x2698 - the pairs image 0x02A73 and image 0x02A78
 * read. The tail repeats, which is what keeps a floater drifting. */
static const uint8_t FLOAT_SCRIPT[] = {
    1,3, 2,3, 3,3, 5,3, 5,0xFF, 5,0, 5,1, 5,1,
    5,2, 6,2, 7,2, 7,2, 6,2, 5,2, 4,2, 4,2,
    5,2, 6,2, 7,2, 7,2, 6,2, 5,2, 4,2, 4,2,
};
#define FLOAT_LOOP_AT 16        /* where the repeating tail begins, in pairs */

/* Image 0x02AEC. */
static void lem_float_land(lemming *a)
{
    a->flags = 0;                                   /* image 0x02AEC */
    a->hot_x = -8;
    a->hot_y = -10;                                 /* image 0x02B0E */
    a->anim  = 0;                                   /* image 0x02B05 */
    a->state = 9;                                   /* image 0x02B13 */
}

/* Image 0x02A6D, the chain's 0x0400. */
static void lem_floatdown(lemming *a, const uint8_t *lvl, uint16_t cw,
                          uint16_t chh)
{
    size_t n = sizeof FLOAT_SCRIPT / 2;
    size_t i = (size_t)a->fscript / 2;
    if (i >= n) i = FLOAT_LOOP_AT + (i - FLOAT_LOOP_AT) % (n - FLOAT_LOOP_AT);

    a->anim = FLOAT_SCRIPT[i * 2];                  /* image 0x02A75 */
    uint8_t act = FLOAT_SCRIPT[i * 2 + 1];          /* image 0x02A78 */
    a->fscript = (int16_t)(a->fscript + 2);         /* image 0x02A7B */

    /* THE ACTION IS THE NUMBER OF STEPS, and each is a probe THEN a move:
     *
     *     3  image 0x02A92, 0x02AA3, 0x02AB1 - three of them
     *     2  image 0x02ABE, 0x02ACF          - two
     *     1  image 0x02ADC                   - one
     *
     * Reading 3 as two steps put the port a frame behind on the very first
     * entry of the table: from y 59 the original probes 59, 60 and 61 and
     * lands on the third, where the port ran out of steps at 61 and floated
     * one frame longer. */
    if (act >= 1 && act <= 3) {
        for (uint8_t k = 0; k < act; k++) {
            if (lem_solid(lvl, cw, chh, a->x, a->y)) {  /* 0x02A95 etc */
                lem_float_land(a);
                return;
            }
            a->y = (int16_t)(a->y + 1);             /* image 0x02A9C */
        }
        return;
    }
    a->y = (int16_t)(a->y + (int8_t)act);           /* image 0x02A8C */
}

/* THE BOMBER - transcribed from image 0x031C4, and it is the second thing
 * the LOOP HEAD dispatches rather than the state chain.
 *
 * Assigning it changes nothing the lemming is doing. Image 0x031E1 puts 0x4F
 * - seventy-nine - in [si+0x20], and that is all. The countdown lives at
 * image 0x01F45, above even the shrug's test: every frame, if the byte is
 * not zero, image 0x01F4C calls image 0x02E04, and a lemming whose fuse has
 * just run out is SKIPPED for that frame entirely.
 *
 * Image 0x02E12 then sets 0x8000, whose handler at image 0x02D0F counts
 * sixteen frames and ORs 2 in - the cloud - with a hot spot of (-16, -25)
 * and 0x280 bytes a frame, which is a far bigger sprite than a lemming.
 */
#define BOMB_FUSE     0x4F      /* image 0x031E1 */
#define BOMB_KEEP     0xC003    /* image 0x031D1 */
#define BOMB_BLOCKED  0x5404    /* image 0x02E0B */
#define BOOM_BIT      0x8000
#define CLOUD_BIT     0x0002

/* Image 0x031C4. */
int lem_bomb_start(lemming *a)
{
    if (a->fuse) return 0;                          /* image 0x031CB */
    if (a->flags & BOMB_KEEP) return 0;             /* image 0x031D1 */
    a->fuse = BOMB_FUSE;                            /* image 0x031E1 */
    return 1;
}

/* Image 0x02E04, called from the loop head. Non-zero means "carry on and
 * dispatch this lemming"; zero means image 0x01F51 skips it this frame. */
static int lem_fuse_step(lemming *a)
{
    if (--a->fuse) return 1;                        /* image 0x02E06 */
    if (a->flags & BOMB_BLOCKED) {                  /* image 0x02E0B */
        a->flags |= CLOUD_BIT;                      /* image 0x02E63 */
        return 1;
    }
    a->flags |= BOOM_BIT;                           /* image 0x02E12 */
    a->hot_x = -8;
    a->hot_y = -10;                                 /* image 0x02E34 */
    a->anim  = 0;                                   /* image 0x02E2B */
    a->state = 0;                                   /* image 0x02E39 */
    a->extra &= 1;                                  /* image 0x02E47 */
    return 0;                                       /* image 0x02E5E */
}

/* Image 0x02D0F: sixteen frames of looking up, then the cloud. */
static void lem_boom(lemming *a)
{
    a->anim = (uint8_t)((a->anim + 1) & 0x0F);      /* image 0x02D12 */
    if (a->anim) return;                            /* image 0x02D19 */
    a->flags |= CLOUD_BIT;                          /* image 0x02D1B */
    a->hot_x = -16;                                 /* image 0x02D3C: fff0 */
    a->hot_y = -25;                                 /* image 0x02D37: ffe7 */
    a->anim  = 0;                                   /* image 0x02D33 */
    a->state = 0;                                   /* image 0x02D41 */
    a->extra &= 1;                                  /* image 0x02D53 */
}





/* THE CLOUD - transcribed from image 0x02350, the last of the bomber.
 *
 * Its counter is NOT masked: image 0x02353 simply increments, and image
 * 0x02358 ends the whole thing at 0x34 - fifty-two frames - where image
 * 0x020E2 sets [si+0x26] to 0xFF and the lemming is gone.
 *
 * Everything else happens on ONE frame. Image 0x0235F takes the counter back
 * to zero only when it was 1, and that is when the crater is dug: image
 * 0x02379 first hands a BLOCKER's nine trigger cells back through image
 * 0x0283B, then image 0x023A6 reads the cell under it and refuses on
 * effects 9 and 5 - steel - before image 0x023B9 calls the terrain removal
 * at image 0x05DF8.
 *
 * THE CRATER ITSELF IS NOT WRITTEN, and it is a data problem rather than a
 * logic one. Image 0x05DF8 clears twenty-two rows (image 0x05E17's cx) from
 * thirty above the lemming (image 0x05E1F), through the Set/Reset path that
 * takes all four planes at once - so it removes terrain outright, the way
 * image 0x05EBA does for the digger. What it clears in each row comes from a
 * table image 0x05E51 indexes by row times three and image 0x05E60 by the
 * low three bits of x - 8: twenty-two rows of eight pre-shifted three-byte
 * masks. WHERE that table is has not been found.
 *
 * So the port's explosion takes the lemming away and leaves the ground
 * alone. The comparison says exactly that: the bomber's own record matches
 * for all 400 frames and ANOTHER lemming diverges at 207, because in the
 * original it falls into the hole and in the port it walks over it.
 */
#define CLOUD_FRAMES  0x34      /* image 0x02358 */

static void lem_cloud(lemming *a, uint8_t *lvl, uint16_t cw, uint16_t chh)
{
    a->anim = (uint8_t)(a->anim + 1);               /* image 0x02353 */
    if (a->anim == CLOUD_FRAMES) {                  /* image 0x02358 */
        a->alive = 0xFF;                            /* image 0x020E2 */
        return;
    }
    if (a->anim != 1) return;                       /* image 0x0235F */
    /* THE BANG. Image 0x02372 tests ds:0x25A7 bit 0 and image 0x02374 asks
     * for sound 0x0C with AH=3 - see docs/sound.md. It sits here, on the one
     * frame the counter is 1, because that is where the original's is. */
    if (sound_enabled(1))                           /* image 0x02372 */
        sound_request(3, 0x0C);                     /* image 0x02374 */
    /* Image 0x023A6 reads the cell under it and refuses on 9 and 5 - steel. */
    if ((a->tcell & 0x0F) == 9 || (a->tcell & 0x0F) == 5) return;
    lem_crater(a, lvl, cw, chh);                    /* image 0x023B9 */
}

/* THE TWO PERMANENT ABILITIES - image 0x0329B and image 0x03557.
 *
 * Neither is a state. A climber and a floater go on doing whatever they were
 * doing; what changes is a byte that other routines read later - the walker
 * when it meets a wall, the faller when it has fallen far enough. Both
 * refuse the same three states, image 0x032A8 and image 0x03564 testing
 * [si+8] against 0x103, and both refuse a lemming that already has the
 * ability.
 *
 * The bytes are the ones image 0x03DFD and image 0x03E0E read to name a
 * lemming on the panel: bit 0 of [si+0x24] is CLIMBER, [si+0x25] is FLOATER,
 * and image 0x03E63 calls one with both an ATHLETE.
 */
#define ABILITY_KEEP 0x0103    /* image 0x032A8 and image 0x03564 */

int lem_climb_start(lemming *a)
{
    if (a->extra & 1) return 0;                 /* image 0x032A2 */
    if (a->flags & ABILITY_KEEP) return 0;      /* image 0x032A8 */
    a->extra = 1;                               /* image 0x032B3: ASSIGNED */
    return 1;
}

/* Image 0x03557. */
int lem_float_start(lemming *a)
{
    if (a->floater) return 0;                   /* image 0x0355E */
    if (a->flags & ABILITY_KEEP) return 0;      /* image 0x03564 */
    a->floater = 0xFF;                          /* image 0x0356B */
    return 1;
}

/* THE BLOCKER - transcribed from image 0x03336, and it is the first skill
 * that writes to the trigger map rather than only reading it.
 *
 * Where it stands, image 0x03348: the cell is ((x - 4) >> 2, (y - 6 - 0x10)
 * >> 2) - the same four-pixel cells and the same 0x10 on the y that image
 * 0x02EC0 reads back with.
 *
 * What it refuses, image 0x0336E onwards: a 3x3 block of cells is checked
 * and any of 2, 0x0A or 3 already there sends it away, so two blockers
 * cannot stand within a cell of each other.
 *
 * What it writes, image 0x0346E: the same 3x3, with the columns taking 2,
 * 0x0A and 3 in turn -
 *
 *     2 A 3
 *     2 A 3
 *     2 A 3
 *
 * - and the nine bytes it displaced go into the RECORD at [si+0x16], so
 * they can be put back. That is why the port's map had to become a real one
 * before this could be written at all: a map recomputed from the level file
 * has nowhere to keep them.
 */
#define BLOCK_BIT     0x0100
#define BLOCK_KEEP    0xF56F   /* image 0x03340 */
#define BLOCK_BACK_X  4        /* image 0x0334D */
#define BLOCK_BACK_Y  6        /* image 0x03350 */
#define BLOCK_LEFT    2        /* image 0x03471 */
#define BLOCK_MID     0x0A     /* image 0x0347A */
#define BLOCK_RIGHT   3        /* image 0x03483 */

static int block_taken(uint8_t v)
{
    return v == BLOCK_LEFT || v == BLOCK_MID || v == BLOCK_RIGHT;
}

/* Image 0x03336. */
int lem_block_start(lemming *a)
{
    if (a->flags & BLOCK_KEEP) return 0;            /* image 0x03340 */

    int32_t col = ((int32_t)a->x - BLOCK_BACK_X) / TRIG_CELL;
    int32_t row = ((int32_t)a->y - BLOCK_BACK_Y - TRIG_Y_BIAS) / TRIG_CELL;

    /* Image 0x0336E: nine cells, and any of the three sends it away. */
    for (int32_t r = 0; r < 3; r++)
        for (int32_t c = 0; c < 3; c++)
            if (block_taken(level_trigger_cell(col + c, row + r)))
                return 0;

    static const uint8_t across[3] = { BLOCK_LEFT, BLOCK_MID, BLOCK_RIGHT };
    for (int32_t r = 0; r < 3; r++)
        for (int32_t c = 0; c < 3; c++) {
            a->under[r * 3 + c] = level_trigger_cell(col + c, row + r);
            level_trigger_put(col + c, row + r, across[c]);   /* 0x03471 */
        }

    a->flags = BLOCK_BIT;
    a->hot_x = -8;                                  /* image 0x0344C */
    a->hot_y = -10;                                 /* image 0x03451: fff6 */
    a->anim  = 0;
    a->state = 0;                                   /* image 0x03448 */
    a->extra &= 1;                                  /* image 0x03460 */
    return 1;
}

/* What a blocker DOES, image 0x027E8: sixteen animation frames, and it goes
 * on blocking for as long as there is ground under it. Image 0x027EF probes
 * with the same routine everything else probes with, and when the answer is
 * "nothing there" - dug away beneath it - image 0x027FC puts it back to
 * walking.
 *
 * It does NOT put its nine trigger cells back here. Those stay stamped,
 * which is the original's behaviour and not an omission: a blocker whose
 * floor is removed leaves its block behind.
 */
static void lem_block(lemming *a, const uint8_t *lvl, uint16_t cw,
                      uint16_t chh)
{
    a->anim = (uint8_t)((a->anim + 1) & 0x0F);      /* image 0x027E8 */
    if (lem_solid(lvl, cw, chh, a->x, a->y)) return; /* image 0x027F5 */

    a->flags = 0;                                   /* image 0x027FC */
    a->hot_x = -8;
    a->hot_y = -10;                                 /* image 0x0281E */
    a->anim  = 0;                                   /* image 0x02815 */
    a->state = 9;                                   /* image 0x02823 */
}

/* THE SHRUG - transcribed from image 0x02DB5, and it is dispatched from the
 * LOOP HEAD rather than from the state chain.
 *
 * Image 0x01F54 reads [si+0x24], tests bit 7 and jumps straight here if it
 * is set - BEFORE image 0x01F5E ever looks at the flag word. That is why the
 * builder's last frames could not be explained from the record: its flags
 * read zero, so the dispatch says "walk", and the original plainly was not
 * walking. Eight frames animating in place, and the position never moves
 * because no handler that moves anything runs at all.
 *
 * Image 0x02DBF: everything happens on the wrap, like the builder's own
 * sixteen. Then image 0x02DEB puts it back to state 9 - walking - and image
 * 0x02DFD clears the bit.
 *
 * The bit is set by image 0x027DE, when a builder runs out of bricks, and
 * read by image 0x03DFD, which goes on calling it a BUILDER on the panel
 * while it shrugs.
 */
static void lem_shrug(lemming *a)
{
    a->anim = (uint8_t)((a->anim + 1) & 7);        /* image 0x02DB8 */
    if (a->anim) return;                           /* image 0x02DBF */
    a->flags = 0;                                  /* image 0x02DC4 */
    a->hot_x = -8;                                 /* image 0x02DE1 */
    a->hot_y = -10;                                /* image 0x02DE6: 0xfff6 */
    a->state = 9;                                  /* image 0x02DEB */
    a->extra &= 1;                                 /* image 0x02DF9 */
    a->extra &= 0x7F;                              /* image 0x02DFD */
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
        /* Image 0x0213C: a CLIMBER goes up the wall instead of turning. */
        if (a->extra & 1) lem_climb_begin(a);       /* image 0x02179 */
        else a->dir = (int8_t)-a->dir;              /* image 0x02142 */
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

/* ======================================================================
 * THE TWO DEATHS THE PORT DID NOT HAVE.
 *
 * Image 0x01F5E's chain has seventeen entries and two of them stood in this
 * file with an address and nothing else - 0x1000 at image 0x22F5 and 0x4000
 * at image 0x2340. They are reached only from the trigger dispatch at image
 * 0x02F0F, whose effects 4, 5 and 6 the port did not have either, so nothing
 * could ever set the bits and there was nothing to check a transcription
 * against. Three separate reports from playing - a missing death scene, no
 * drowning, no trap - are all this one gap.
 * ====================================================================== */

/* Image 0x022F5: DROWNING. Sixteen frames and then dead, and while they run
 * it drifts one pixel a frame in the direction it was facing - but only
 * while the water EIGHT ahead is clear, image 0x02306 multiplying the
 * direction by eight before the probe. */
static void lem_drown(lemming *a, uint8_t *lvl, uint16_t cw, uint16_t chh)
{
    a->anim = (uint8_t)((a->anim + 1) & 0x0F);      /* image 0x022FA */
    if (!a->anim) {                                 /* image 0x022FF */
        a->alive = 0xFF;                            /* image 0x02323 -> 0x20E2 */
        return;
    }
    if (!lem_solid(lvl, cw, chh, (int16_t)(a->x + a->dir * 8), a->y))
        a->x = (int16_t)(a->x + a->dir);            /* image 0x0231E */
}

/* Image 0x02340: fourteen frames and then dead, and it does not move. */
static void lem_burn(lemming *a)
{
    a->anim = (uint8_t)(a->anim + 1);               /* image 0x02340 */
    if (a->anim == 0x0E) a->alive = 0xFF;           /* image 0x02346 */
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

    /* Image 0x021BA: far enough down, and a floater opens its umbrella. */
    if (a->fall > FLOAT_MIN_FALL && a->floater) {   /* image 0x021C0 */
        a->flags   = FLOAT_BIT;                     /* image 0x021C6 */
        a->anim    = 0;                             /* image 0x021DF */
        a->fscript = 0;                             /* image 0x021E3 */
        a->hot_y   = -16;                           /* image 0x021E8: fff0 */
        a->state   = 8;                             /* image 0x021ED */
        a->extra  &= 1;                             /* image 0x021FB */
        return;
    }
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


/* Image 0x038A3: the four-entry entrance table at ds:0x0E98, built by walking
 * the level's 32 object slots and appending every one whose id is 1. Whatever
 * it finds, all four entries end up valid - image 0x038D6 dispatches on how
 * many there were and copies the ones it has over the ones it has not:
 *
 *     0   image 0x03926   all four become (0x100, 0x10)
 *     1   image 0x038EA   the one, four times
 *     2   image 0x03902   1, 0 after the two it found
 *     3   image 0x0391A   entry 1 again as the fourth
 *     4                   nothing to do
 *
 * Written as a pure function of the level rather than a table filled once at
 * load: the answer is the same and there is no second copy of the level's
 * state to keep in step. */
void level_entrances(const level *lv, int16_t out[4][2])
{
    uint8_t bp = 0;                                 /* image 0x038AA */
    for (uint16_t i = 0; i < lv->num_objects && bp < 4; i++) {
        if ((lv->objects[i].id & 0xFF) != 1) continue;   /* image 0x038B2 */
        out[bp][0] = (int16_t)(lv->objects[i].x + LEMMING_DX + LEM_SPAWN_DX);
        out[bp][1] = (int16_t)(lv->objects[i].y + LEMMING_DY + LEM_SPAWN_DY);
        bp++;                                       /* image 0x038D0 */
    }
    if (bp == 0) {                                  /* image 0x03926 */
        for (uint8_t k = 0; k < 4; k++) {
            out[k][0] = 0x100;
            out[k][1] = 0x10;
        }
        return;
    }
    if (bp == 1) {                                  /* image 0x038EA */
        out[1][0] = out[2][0] = out[3][0] = out[0][0];
        out[1][1] = out[2][1] = out[3][1] = out[0][1];
        return;
    }
    if (bp == 2) {                                  /* image 0x03902 */
        out[2][0] = out[1][0]; out[2][1] = out[1][1];
        out[3][0] = out[0][0]; out[3][1] = out[0][1];
        return;
    }
    if (bp == 3) {                                  /* image 0x0391A */
        out[3][0] = out[1][0]; out[3][1] = out[1][1];
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
    /* THE RECORD IS CLEARED FIRST - image 0x030F1 is a `loop` that writes a
     * zero over every byte of it before image 0x030F9 fills any in, and the
     * port had only ever overwritten the fields it went on to set.
     *
     * The slot is REUSED from level to level, so everything the last lemming
     * to occupy it had carried survived into the next one: a level played
     * after one with floaters started with lemmings that could not be killed
     * by a fall, which is what this was reported as. The abilities at +0x24
     * and +0x25 are the visible half; the brick count, the fuse and the
     * blocker's saved cells are all in there too. */
    memset(a, 0, sizeof *a);
    /* FOUR ENTRANCES, TAKEN IN TURN - image 0x030DB is `and bx,3`, so the
     * release ordinal picks one of four slots in the table at ds:0x0E98, and
     * the port had always taken the first entrance object it found. Eight of
     * the eighty levels have more than one entrance (four have two, two have
     * three, two have four) and on every one of them the port put every
     * lemming through the same door. */
    int16_t ent[4][2];
    level_entrances(lv, ent);                       /* image 0x038A3 */
    a->x = ent[n & 3][0];                           /* image 0x030FB */
    a->y = ent[n & 3][1];                           /* image 0x03100 */
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
    /* Image 0x01F19, and it jumps to image 0x01FFF - PAST the nuke's cascade
     * at image 0x01FF8, which is this loop's tail. A paused nuke stops
     * mid-cascade with its fuses still counting nothing down. */
    if (level_paused() == 0xFF) return;
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
         *     0x0002  0x2350  the bomber's cloud     transcribed
         *     0x8000  0x2D0F  the blast              transcribed
         *     0x0004  0x21B0  falling                transcribed
         *     0x0400  0x2A6D  floating down          transcribed
         *     0x0001  0x22DF  splatting              transcribed
         *     0x0008  0x23BF  stepping up            transcribed
         *     0x0010  0x23F3  digging                transcribed
         *     0x0020  0x254F  climbing               transcribed
         *     0x0040  0x2637  hoisting over the top  transcribed
         *     0x0080  0x2693  building               transcribed
         *     0x0100  0x27E8  blocking               transcribed
         *     0x0200  0x287D  bashing                transcribed
         *     0x0800  0x2B35  mining                 transcribed
         *     0x1000  0x22F5  NOT READ
         *     0x2000  0x2326  leaving                transcribed
         *     0x4000  0x2340  NOT READ
         *     else    0x2EC0  (called, not jumped to)
         *
         * Fifteen of the seventeen are transcribed and every one of them is
         * checked against the original's own record by
         * tools/verify_skills.py. The two that are not have their addresses
         * here and nothing in their place: no recorded run has set either
         * bit, so there is nothing to check a transcription of them against
         * and a guess would look exactly like the rest of this list.
         *
         * The order is the original's and matters: it is a priority chain,
         * so a lemming with two bits set takes the FIRST of them here and
         * not the lowest-numbered. */
        uint16_t f = a->flags;
        /* Image 0x01F45, and it comes before even the shrug's test: a lit
         * fuse is counted down every frame, and a lemming whose fuse has
         * just run out is skipped entirely. */
        if (a->fuse && !lem_fuse_step(a)) continue;  /* image 0x01F51 */

        /* Image 0x01F54, and it comes BEFORE the flag word is even read:
         * bit 7 of [si+0x24] sends it to the shrug and nowhere else. */
        if (a->extra & 0x80) {                       /* image 0x01F57 */
            lem_shrug(a);
            continue;
        }

        if (f == 0) {
            /* Image 0x01F61: `and ax,ax / jne / jmp 0x2047`. The flag word
             * alone decides; the state byte at +0x29 takes no part in it.
             * The port used to require state 9 as well, which is true of a
             * lemming that has always been walking and false of one that has
             * just STOPPED doing something: image 0x027CC leaves a builder
             * out of bricks on state 8, and it walked on in the original
             * while the port's stood still for ever. */
            lem_walk(a, lvl, cw, chh);               /* image 0x2047 */
        }
        else if (f & 0x0002)
            lem_cloud(a, lvl, cw, chh);              /* image 0x2350 */
        else if (f & 0x0004)
            lem_fall(a, lvl, cw, chh);               /* image 0x21B0 */
        else if (f & 0x2000)
            lem_exiting(a);                          /* image 0x2326 */
        else if (f & 0x8000)
            lem_boom(a);                             /* image 0x2D0F */
        else if (f & 0x0020)
            lem_climb(a, lvl, cw, chh);              /* image 0x254F */
        else if (f & 0x0040)
            lem_hoist(a);                            /* image 0x2637 */
        else if (f & 0x0200)
            lem_bash(a, lvl, cw, chh);               /* image 0x287D */
        else if (f & 0x0100)
            lem_block(a, lvl, cw, chh);              /* image 0x27E8 */
        else if (f & 0x0080)
            lem_build(a, lvl, cw, chh);              /* image 0x2693 */
        else if (f & 0x0010)
            lem_dig(a, lvl, cw, chh);                /* image 0x23F3 */
        else if (f & 0x0400)
            lem_floatdown(a, lvl, cw, chh);          /* image 0x2A6D */
        else if (f & 0x0001)
            lem_splat(a);                            /* image 0x22DF */
        else if (f & 0x0008)
            lem_step_up(a, lvl, cw, chh);            /* image 0x23BF */
        else if (f & 0x0800)
            lem_mine(a, lvl, cw, chh);               /* image 0x2B35 */
        else if (f & 0x1000)
            lem_drown(a, lvl, cw, chh);              /* image 0x22F5 */
        else if (f & 0x4000)
            lem_burn(a);                             /* image 0x2340 */

        /* THE TRIGGER IS TESTED AFTER THE MOVE, NOT BEFORE IT.
         *
         * The dispatch at image 0x01F5E does not fall through to the trigger
         * lookup - every one of its handlers JUMPS BACK to image 0x01FE8
         * when it is done, and that is where `call 0x2EC0` sits. Thirty-one
         * jumps land there. So the question the original asks is "having
         * moved, what is this lemming standing on now", and the port asked
         * it of where the lemming had been.
         *
         * One frame, and it showed up as lemmings entering the exit a frame
         * or two late, the level ending at a different moment, and 24 to 40
         * differing pixels for the last quarter of the run. */
        /* Image 0x02F0F dispatches the trigger byte's low nibble, and three
         * of its effects are read:
         *
         *     1  image 0x02F3B  the exit
         *     2  image 0x02FA2  a blocker's LEFT column - a lemming walking
         *                       right turns, one already walking left does
         *                       not
         *     3  image 0x02FAC  its right column, the other way round
         *
         * 0x0A is the blocker's own middle, which nothing dispatches - it is
         * there for image 0x0336E to refuse a second blocker by. */
        if (trig_lv && trig_g) {
            uint8_t cell = level_trigger_at(a, trig_lv, trig_g);
            uint8_t eff = (uint8_t)(cell & 0x0F);
            uint8_t obj = (uint8_t)(cell >> 4);       /* image 0x02FB8 */
            if (eff == TRIGGER_EXIT) {
                if (!(a->flags & EXIT_BIT) && !(a->flags & 4))
                    lem_exit_start(a);               /* image 0x02F3B */
            } else if (eff == 2) {
                if (a->dir != -1) a->dir = (int8_t)-a->dir;   /* 0x02FA2 */
            } else if (eff == 3) {
                if (a->dir != 1) a->dir = (int8_t)-a->dir;    /* 0x02FAC */
            } else if (eff == 4) {
                /* A TRAP, image 0x02FB6. It kills OUTRIGHT - image 0x02FD1
                 * writes 0xFF straight into [si+0x26] with no animation of
                 * the lemming's own - and image 0x02FCE sets bit 1 of the
                 * object's record so the TRAP plays its sequence. One at a
                 * time: image 0x02FC5 refuses while that bit is still set. */
                if (!level_object_busy(obj)) {
                    level_object_trigger(obj);       /* image 0x02FCE */
                    a->alive = 0xFF;                 /* image 0x02FD1 */
                    /* AND THE TRAP'S OWN NOISE. Image 0x02FE9 takes the id
                     * from the OBJECT - byte 0x1B of its record - not from
                     * the effect, and image 0x02FEE makes zero mean silence.
                     * `obj` is the trigger byte's high nibble, which is the
                     * index image 0x02FB6 multiplies by 0x1C. */
                    if (g_snd_driver == 0xFF && sound_enabled(3)     /* 0x02FDF */
                        && obj < trig_lv->num_objects) {
                        const ground_object *tgo =
                            &trig_g->objects[trig_lv->objects[obj].id & 0x0F];
                        if (tgo->trigger_sound)      /* image 0x02FEC */
                            sound_request(4, tgo->trigger_sound);  /* 0x02FF3 */
                    }
                }
            } else if (eff == 5) {
                /* WATER, image 0x02FF8. The flag word is ASSIGNED 0x1000,
                 * not OR-ed, so everything else it was doing stops. */
                if (!(a->flags & 0x1000)) {
                    a->flags = 0x1000;               /* image 0x03002 */
                    a->anim  = 0;                    /* image 0x0301B */
                    a->hot_x = -8;                   /* image 0x0301F */
                    a->hot_y = -10;
                    a->state = 0;                    /* image 0x03029 */
                    a->extra &= 1;                   /* image 0x03037 */
                    /* THE DROWNING, image 0x0304C - the last act of this
                     * routine, immediately before image 0x0304F begins the
                     * fire's. Two handlers back to back, each ending in its
                     * own sound: exactly the layout that got 0x0D attached
                     * to the release. */
                    if (g_snd_driver == 0xFF && sound_enabled(3))
                        sound_request(4, 0x11);      /* image 0x03049 */
                }
            } else if (eff == 6) {
                /* Image 0x0304F, the other one - fourteen frames and gone.
                 * Its hot spot is (-8, -14), four rows deeper than anything
                 * else here, and its frame is 0x70 = 112 bytes. */
                if (!(a->flags & 0x4000)) {
                    a->flags = 0x4000;               /* image 0x03059 */
                    a->anim  = 0;                    /* image 0x03072 */
                    a->hot_x = -8;                   /* image 0x03076 */
                    a->hot_y = -14;                  /* image 0x0307B: fff2 */
                    a->state = 0;                    /* image 0x03080 */
                    a->extra &= 1;                   /* image 0x0308E */
                    /* AND THE SOUND, which the port had attached to the
                     * RELEASE instead - see level_release_step. It is the
                     * next thing this routine does, at image 0x03092's
                     * ds:0x2010 test and image 0x030A0's request. */
                    if (g_snd_driver == 0xFF && sound_enabled(3))
                        sound_request(4, 0x0D);      /* image 0x030A0 */
                }
            }
        }
    }

    /* Image 0x01FF3's `dec [0x42] / je 0x1FF8` - the walk falls out of its
     * loop straight into the nuke's cascade. It is not a separate call and
     * must not become one: put it anywhere else in the frame and it steps a
     * lemming the walk has not. */
    level_nuke_step(lem);                          /* image 0x01FF8 */
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
/* AND THE FRAME SIZE IS NOT UNIFORM ACROSS THIS SECTION.
 *
 * The port indexes main.dat section 0 as a flat array of 40-byte frames -
 * 16 wide, 10 rows, two planes - which is right for the walker at 0 and the
 * faller at LEM_SPR_FALL, and both match the original to the pixel. It is
 * not right in general, and that is measured rather than suspected: render
 * every frame on the 40-byte grid and count the ink in its top five rows
 * against its bottom five, and
 *
 *     walker 0..9      balanced, every frame a whole figure
 *     faller 358..365  balanced, every frame a whole figure
 *     290..301         ALTERNATING: 293, 295, 297, 299 have nothing at all
 *                      in their top half, and their neighbours are crowded
 *     330..341         the same alternation
 *
 * which is one sprite lying across two 40-byte slots: 80 bytes, twenty rows.
 * So different animations in this section have different frame sizes, and
 * the flat model only happens to suit the two the port draws.
 *
 * That is what makes the digger's sprite an open question rather than an
 * arithmetic one. Its record says 112 bytes a frame with the mask at 84
 * (tools/probe_digger.py measured both), 112 is not a multiple of three so
 * it is not the walker's pre-shift shape, and there is no reason to expect
 * its file frames to be 40 bytes either. Finding it means locating the
 * animation, not dividing a number.
 *
 * THE STORE'S SHAPE IS NOW KNOWN, which narrows this a great deal.
 *
 * Logging every call to the pre-shift builder at image 0x05B11 through a
 * level shows it is called with exactly ONE shape, 106 times: DL = 10 and
 * DH = 2. Those are the ROW COUNT and the BYTES PER ROW - the names in
 * tools/verify_preshift.py had them the other way round - so it is ten rows
 * of two bytes, the walker's 16x10, and the source it reads is
 * rows * bpr * 2 planes = 40, exactly the frame the port draws.
 *
 * From that the layout falls out, and it matches the record on every count:
 *
 *     per shift = rows * (planes + 1) * (bpr + 1)   10 * 3 * 3 = 90 = [+0x10]
 *     mask at   = rows *  planes      * (bpr + 1)   10 * 2 * 3 = 60 = [+0x12]
 *
 * The `+ 1`s are the pre-shift: a row needs one more byte to hold what the
 * shift pushes into it, and one more plane to hold the mask.
 *
 * Applied to the digger's 112 and 84, the ratio 112/84 gives PLANES = 3, and
 * the mask's 28 bytes give rows * (bpr + 1) = 28. That leaves five shapes,
 * and the screen picks one of them OUTRIGHT.
 *
 * tools/probe_digger.py separates the sprite from the ground it is standing
 * in by holding still: image 0x023F3 digs only when the counter comes round
 * to 0 and to 8, so across anims 1..7 the terrain does not change and the
 * lemming does. Take seven such frames, and a pixel that varies is the
 * sprite while one that never does is the level. What comes out is
 *
 *     ..........100000000.0..1........
 *     ............0000220.3...........
 *     .............00222030...........
 *     ..........3...3222300..3........
 *     ..........3..0322200...3........
 *     ..............3.0......2........
 *     ..........22333.11..3322........
 *
 * SEVEN ROWS, about fourteen columns of content. So it is rows = 7, and with
 * rows * (bpr + 1) = 28 that fixes bpr = 3 - a 24-pixel frame with margins,
 * in three planes, 63 bytes of source.
 *
 * What is still not found is WHERE - and reading the draw's own addressing
 * says the question was not even the right shape. Image 0x070BF onwards:
 *
 *     mul cx          cx = [di+0x10]   the anim frame times it, 32 bits wide
 *     bp = (dx:ax) >> 4                the high part, a BYTE offset
 *     bp += [di+0x0C]                  plus the mask base
 *     ax &= 0x0F                       and the low NIBBLE is kept
 *     si  = ax + [di+0x0A]             plus the ink base
 *     bx  = si + [di+0x12]             plus the mask delta
 *
 * So [+0x10] is not "bytes a frame". It feeds a product that gets split into
 * a byte offset and a four-bit remainder, and `si` and `bx` come out small -
 * they are not pointers into a sprite the way this port's are. Whatever the
 * store's addressing is, it is not "frame index times frame size", and every
 * attempt here to divide 112 into a width and a height was answering a
 * question the original does not ask.
 *
 * Image 0x070B0 also gates on [+0x2A]: when it is not 0xFF the frame index
 * is shifted left three and offset by BX first. The digger's record has 0xFF
 * there and the walker's has 0, so the two take different paths through this
 * - which is one more reason the walker's layout was never going to explain
 * the digger's numbers.
 *
 * Hooking the addressing while a digger draws settles the SHAPE, and the
 * measurement checks itself. BP comes out as
 *
 *     walker   ... 0x0a6f 0x0a9c 0x0ac9 ...   +45 an animation frame
 *     digger   0x032a 0x0331 0x0338 0x033f    + 7 an animation frame
 *
 * and BP counts SIXTEEN-BYTE PARAGRAPHS, which is what the `>> 4` is for.
 * So the walker's frames are 45 * 16 = 720 bytes apart - eight pre-shifts of
 * 90 - and the digger's are 7 * 16 = 112, which is [+0x10] exactly. The
 * walker gets the extra `* 8` at image 0x070B6 because its eight shifts sit
 * together; the digger's [+0x2A] is 0xFF so it skips that, and the reason is
 * plain once seen: A DIGGER DOES NOT MOVE HORIZONTALLY, so it never needs a
 * sub-byte shift.
 *
 * That fixes the geometry, and every number lands:
 *
 *     4 bytes a row (32 pixels) x 7 rows x 4 planes            = 112 = [+0x10]
 *     3 of those planes are colour, so the mask is at 3 x 4 x 7 =  84 = [+0x12]
 *
 * Seven rows is what the screen showed. Three colour planes is what the
 * record's 112/84 ratio gave. The mask landing on [+0x12] to the byte is the
 * part that makes this a derivation rather than another fit.
 *
 * AND THE BYTES ARE FOUND. Image 0x07112 is the piece that was missing:
 * `cx = [0x1FE6] + bp`, so ds:0x1FE6 holds the store's SEGMENT and BP is a
 * paragraph offset into it. That is what the `>> 4` produces, and it is why
 * reading DS:BP*16 gave nonsense - the DS at the addressing is not the
 * store's.
 *
 * At ([0x1FE6] + BP) : SI the digger's frames decode cleanly with the
 * geometry above, and three things agree at once:
 *
 *   - plane 3 is EXACTLY the ink of planes 0..2, everywhere - which is what
 *     a mask is, and confirms the fourth plane's role
 *   - consecutive frames sit at 0x03C2A0 and 0x03C310, 0x70 = 112 apart,
 *     which is the stride BP's step of seven paragraphs predicted
 *   - the ink is in colours 1..3, which is what the screen showed
 *
 * The picture itself is not yet interpreted: each row holds two clusters
 * about twenty pixels apart, and seven rows is short for a lemming. Rendering
 * a run of consecutive frames and watching the animation is what settles
 * whether that is the digger throwing earth to both sides or two shifts
 * stored side by side - and only then is it safe to draw. The port still
 * draws no digger.
 *
 * They are not in main.dat section 0 in this layout, which is consistent:
 * the store is built at load time, not copied out of a file verbatim.
 *
 * One of the two candidates was ruled out earlier, by looking. probe_digger.py dumps
 * the original's own framebuffer while a lemming digs, printing anything
 * below attribute 8 as itself and the terrain as dots - the sprite is drawn
 * in 1..7 and the level in 8..15, so that separates them. What comes out is
 * about SIXTEEN pixels wide and eight to ten tall, in colours 1, 2 and 3.
 *
 * So it is not the 32-wide, 7-row reading. It is 16 wide, like the walker.
 * What still does not close is the byte count: sixteen wide in two planes is
 * three bytes a row including the mask, and 112 is not divisible by three -
 * so either there is padding in the frame or the store packs it in a way
 * neither sprite here has shown yet. The dump is too tangled with the
 * terrain the digger is removing to read the shape off directly; catching it
 * over black, or reading the store, is what finishes this. */
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

/* THE DIGGER'S SPRITE - main.dat section 0, offset 720, and every part of
 * that was measured rather than guessed. See tools/probe_digger.py.
 *
 * Image 0x07112 gave the store's address - `cx = [0x1FE6] + bp`, a segment
 * and a paragraph offset - and reading the frames there settled the shape:
 * 16 wide in two bytes, FOURTEEN rows, three colour planes, 84 bytes. The
 * store's frame is 112 because the loader adds a fourth plane as a mask; the
 * FILE holds the 84 bytes of colour alone, which is why this port's "any
 * plane set" rule already draws it correctly.
 *
 * 720 is the eighteen walker frames of forty bytes, so the digger sits
 * immediately after the walker - and the frame was found by searching the
 * file for the exact bytes the guest was reading, not by dividing 21,104 by
 * anything.
 *
 * The wrong turning is worth recording: 112 with the mask at 84 is satisfied
 * by 4 bytes x 7 rows AND by 2 bytes x 14 rows, both in four planes, and the
 * screen appeared to show seven rows because only seven of them CHANGE
 * across the frames that do not dig. Rendering both and looking is what
 * separated them - at 32x7 each frame holds two clusters that are one
 * lemming cut in half. */
#define DIG_SPR_BASE    720
#define DIG_SPR_W       16
#define DIG_SPR_H       14
#define DIG_SPR_PLANES  3
#define DIG_SPR_ROW     2
#define DIG_SPR_PLANE   (DIG_SPR_ROW * DIG_SPR_H)            /* 28 */
#define DIG_SPR_FRAME   (DIG_SPR_PLANE * DIG_SPR_PLANES)     /* 84 */
#define DIG_SPR_FRAMES  16

/* THE EXIT'S SPRITE - main.dat section 0, offset 16816, and measured the
 * same way the digger's was.
 *
 * Image 0x02F51 gives the state 0x4E = 78 bytes a frame and image 0x02F56
 * puts the mask at 0x34 = 52, so by the store's formula it is two colour
 * planes over thirteen rows of two bytes with a mask plane on top. The FILE
 * holds the 52 bytes of colour, which is why the mask comes out as exactly
 * the ink - and why this port's "any plane set" rule draws it correctly.
 *
 * The bytes were found by capturing what the guest reads at
 * ([0x1FE6] + [si+0x0C]) * 16 while a lemming was leaving, and searching
 * main.dat for them. Searching the file for the GEOMETRY instead had failed
 * repeatedly: eight coherent 52-byte frames match thousands of offsets, and
 * the best of them were not it. */
/* THE BUILDER'S SPRITE - image 0x032FB names the shape, and WHERE was found
 * by lifting the original's own pixels off a captured frame.
 *
 * The shape is read: image 0x03300 gives it 0x68 = 104 bytes a frame with
 * image 0x03305 putting the mask 0x4E = 78 in, so the colour is 78 bytes -
 * thirteen rows of two in three planes - the way the digger's is 84.
 *
 * The offset is measured, and the two wrong turns are worth keeping.
 *
 *   The sprites are consecutive - the walker is 18 frames of 40 from 0, so
 *   the digger starts at 720 and its 16 frames of 84 end at 2064 - and 2064
 *   decodes as a perfectly plausible lemming. Wiring it in changed the
 *   comparison by NOTHING. Every frame of every sprite in this file looks
 *   like a lemming; looking right is not evidence.
 *
 *   Then the original's own pixels were lifted off frame 130 - the record
 *   puts the builder at (796, 94) with hot spot (-8, -13), so its 16x13 is
 *   at screen (148, 65) - and every offset tried against them. NONE matched,
 *   which read as "not in this section". It was the CURSOR: the script parks
 *   the pointer on the lemming it clicked, so its box sits over the sprite
 *   and about thirty pixels can never agree. Demanding a perfect match threw
 *   the right answer away.
 *
 * Scoring instead of demanding gives 4240, and it holds across five
 * different animation frames - 0, 1, 2, 2 and 0 pixels disagreeing, every
 * one of them under the cursor. The method was checked on the digger first,
 * whose offset is already known: it comes back 720 with 2 disagreeing, also
 * the cursor.
 */
#define BUILD_SPR_BASE  4240
#define BUILD_SPR_W     16
#define BUILD_SPR_H     13
#define BUILD_SPR_PLANES 3
#define BUILD_SPR_ROW   (BUILD_SPR_W / 8)
#define BUILD_SPR_PLANE (BUILD_SPR_ROW * BUILD_SPR_H)          /* 26 */
#define BUILD_SPR_FRAME (BUILD_SPR_PLANE * BUILD_SPR_PLANES)   /* 78 */
#define BUILD_SPR_FRAMES 16
/* Image 0x0330A's BUILD_STATE: the frames facing left start this far on. */
#define BUILD_SPR_STATE 0x10

/* THE SHRUG'S SPRITE - the eight frames a builder animates through when it
 * runs out of bricks. Measured the same way, and it is the WALKER's
 * geometry: 16 wide, ten rows, two planes, 40 bytes a frame, at 19440.
 *
 * The guest's own fields do NOT give it. Image 0x027B4 sets [si+0x10] to
 * 0x5A and image 0x027B9 sets [si+0x12] to 0x3C, and for the digger, the
 * exit and the builder the second of those IS the file's frame size - 84,
 * 52 and 78, all three confirmed. Here it would say 60, and the file's frame
 * is 40. Sixty is ten rows of THREE bytes in two planes, which is the
 * PRE-SHIFTED store's stride rather than the file's two; the mask makes up
 * the other thirty and the ninety is the store's whole frame. So those two
 * fields describe the store, and the file's geometry has to be measured -
 * which is what scoring every offset against the original's own pixels,
 * over all eight frames at once, does. Nought to eight pixels a frame
 * disagree and every one of them is another lemming walking past. */
/* THE BLOCKER'S SPRITE, measured the same way: 16 wide, ten rows, two
 * planes, 40 bytes a frame, at 18800. One pixel disagrees across eight
 * frames of 310 ink.
 *
 * Sixteen frames of forty ends at 19440, which is exactly where the shrug's
 * begin - so the two are adjacent in the file, and each was found without
 * reference to the other. Two independent measurements meeting at a boundary
 * is worth more than either alone. */
/* THE CLIMBER'S SPRITES, and this is the one place a sprite has turned out to
 * come in TWO - one for each way the lemming faces.
 *
 * Scoring every offset over all the climbing frames together gave nothing
 * better than 65 disagreements at any geometry, where every sprite found this
 * way had come in at 0 to 2. A score that bad means the frames do not share
 * one base. Splitting them by direction - which needed the reference
 * re-recording, because the manifest had not carried it - gives ZERO at once,
 * twice over: 2064 going right and 2448 going left, 12 rows in two planes.
 * Eight frames of 48 is 384, and 2448 - 2064 is 384, so the two sets are
 * adjacent.
 *
 * 2064 IS THE OFFSET THAT WAS GUESSED FOR THE BUILDER AND DISMISSED. It sits
 * exactly where the digger's sixteen frames end, it decoded as a perfectly
 * plausible lemming, and it changed that comparison by nothing - because it
 * is this. A wrong sprite in the right file looks no different from a right
 * one until something scores it.
 *
 * The hoist's is 12 rows in two planes at 3472: ZERO across all eight frames
 * of 180 ink. Whether it comes in two like the climb's is not known - every
 * hoist in the run so far faces the same way. */
/* FOUND THE SAME WAY as the others - scored against the original's own
 * pixels, all four at ZERO disagreeing. The faller and the splat came off
 * LEVEL 2, because "Just dig!" has nothing that falls far enough to do
 * either, and the floater off the run that assigned one there.
 *
 * The search had to take the SCROLL from the manifest rather than assume
 * 624: level 2 starts at 528, and hard-coding the first level's put every
 * candidate in the wrong place and returned nothing at any geometry. */
#define FALL_SPR_BASE   14320   /* 10 rows, 2 planes - ink 222, 0 differ */
#define SPLAT_SPR_BASE  16176   /* 10 rows, 2 planes - ink 147, 0 differ */
#define FLOAT_SPR_BASE  14640   /* 16 rows, 3 planes - ink 356, 0 differ */
#define BLAST_SPR_BASE  20080   /* 10 rows, 2 planes - ink 256, 0 differ */

/* The basher's and the miner's - see the note where they are drawn. Their
 * sizes are the original's own: image 0x03526 gives the basher 0x3C = 60
 * bytes a frame and image 0x035FF gives the miner 0x4E = 78, and image
 * 0x0352B and image 0x03604 give the states that index the left-facing
 * halves. */
/* THE BOMBER'S CLOUD - 32 x 32, lifted from the original's own
 * pre-shifted store rather than from any file: it is not in MAIN.DAT at
 * all. Image 0x02D24's [si+0x0c] = 0x180B is a PARAGRAPH offset into the
 * store at ds:0x1FE6 (image 0x07112), and image 0x02D29's 0x280 with
 * image 0x02D2E's 0x1E0 says 5 bytes a row, 32 rows, three ink planes
 * and a mask - the 40-bit row being a 32-wide sprite with eight bits of
 * pre-shift. Columns 32..39 are the wrap and are dropped.
 *
 * WHY INDEX 7. The store's pre-shift ROTATES within the 40-bit row rather
 * than shifting into it, and the sprite is 32 wide: index i carries it at
 * columns i..i+31 with the part that runs off the end coming back at the
 * start. Checked cell by cell, index i agrees with index 0 on 1011 of the
 * 1024 cells of that window, for every one of the eight. So 7 is not an
 * animation frame and not a sub-pixel shift - it is the rotation that brings
 * the sprite's 32 columns together in one run, and its columns 0..6 are the
 * sprite's own 33..39, which is why they belong SEVEN PIXELS LEFT of the
 * origin rather than thirty-three right.
 *
 * That is what makes (x - scroll - 7, y + hot_y - 18) the placement rather
 * than a fitted constant. Scored over the original's five bursts - frames
 * 496, 498, 500, 502 and 504 - it agrees on 2,086 of 2,125 pixels. Drawing
 * index 0's own columns 0..31 at the origin instead scores 1,400 of 1,785,
 * and index 7 at the origin 463 of 2,125, so the two halves of the reading
 * check each other. */
#define CLOUD_SPR_W 32
#define CLOUD_SPR_H 32
/* Frame ZERO of the group, and it is placed at the lemming's own HOT SPOT.
 *
 * The previous reading took frame 7 and drew it at fitted offsets, on the
 * argument that the store's pre-shift rotates within a 40-bit row so index 7
 * is the rotation that brings the sprite's 32 columns together. That
 * argument was self-consistent and wrong: index 0 is simply the sprite,
 * 507 lit pixels of it, and every other index is that picture rotated.
 *
 * Measured against the original's own ten bursts in out/ref_nuke, frames
 * 495..504, one lemming reaching anim 0 on each:
 *
 *     frame 0 at the hot spot   5,032 of 5,070   and FIVE of the ten
 *                               are 507 of 507, exactly
 *     the old frame 7 at its
 *     fitted offset               77 of 507 on the first of them
 *
 * The five that are not exact are the early ones, where another lemming's
 * sprite is still drawn over the cloud.
 *
 * And the placement is not fitted at all - it is `(x + hot_x, y + hot_y)`,
 * the same hot spot every other sprite here is drawn at. That is WHY image
 * 0x02D3C sets hot_x to -16 and image 0x02D37 sets hot_y to -25 as the
 * bomber turns into a cloud: the numbers exist to put this sprite in place.
 * A player reported the left of the explosion missing, which is exactly what
 * drawing a 32-wide sprite nine pixels right of where it belongs looks like.
 */
static const uint8_t CLOUD_SPR[CLOUD_SPR_H][CLOUD_SPR_W] = {
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,5,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,5,0,0,0,0,0,0,0,0,0,0,0,0,0,5,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,5,5,0,0,0,0,0,0,0,0,0,0,0,0,5,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,5,5,0,0,0,0,0,0,0,0,0,0,5,5,5,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,5,5,0,0,0,0,0,0,0,0,0,0,5,5,5,0,0,0,0,0,0,0,0,0,5,0,0},
    {0,0,0,0,0,0,5,5,0,0,0,0,0,0,0,0,5,5,4,5,5,0,0,0,0,0,0,5,5,0,0,0},
    {0,0,0,0,0,0,5,5,5,0,0,0,0,0,0,5,5,5,4,5,5,5,5,0,0,5,5,5,5,0,0,0},
    {0,0,0,0,0,0,5,5,5,5,5,0,0,5,5,5,5,4,4,5,5,5,5,5,5,5,5,5,0,0,0,0},
    {0,0,0,0,0,0,0,5,4,5,5,5,5,5,5,5,5,4,4,4,5,5,5,5,5,5,5,5,0,0,0,0},
    {0,0,0,0,0,0,0,5,4,4,5,5,5,5,5,5,4,4,4,4,4,4,5,5,5,4,5,0,0,0,0,0},
    {0,0,0,0,0,0,0,5,5,4,4,4,5,5,5,4,4,4,3,4,4,4,4,4,4,5,5,0,0,0,0,0},
    {0,0,0,0,0,0,5,5,5,4,3,4,4,4,4,4,4,3,3,4,4,4,3,4,4,5,5,0,0,0,0,0},
    {0,0,0,0,0,5,5,5,5,4,4,3,4,4,4,4,3,3,3,3,3,3,4,4,5,5,0,0,0,0,0,0},
    {0,0,0,0,0,5,5,5,5,4,4,4,3,3,3,3,3,3,3,3,3,3,4,4,5,5,0,0,0,0,0,0},
    {0,0,0,0,5,5,5,5,5,4,4,4,3,3,3,3,3,3,3,3,3,4,4,4,5,5,5,0,0,0,0,0},
    {0,0,5,5,5,5,5,5,4,4,4,4,4,3,3,3,3,3,3,3,3,4,4,4,5,5,5,0,0,0,0,0},
    {5,5,5,5,5,5,4,4,4,4,4,4,4,3,3,3,3,3,3,3,3,3,4,4,4,5,5,5,0,0,0,0},
    {0,0,0,5,5,5,5,5,5,4,4,3,3,3,3,3,3,3,3,3,3,3,4,4,4,5,5,5,5,5,0,0},
    {0,0,0,0,0,5,5,5,5,5,4,4,4,4,3,3,3,3,3,3,3,3,3,3,4,4,4,5,5,5,5,5},
    {0,0,0,0,0,0,5,5,5,5,4,4,4,3,3,3,3,3,3,3,3,3,4,4,4,5,5,5,5,5,0,0},
    {0,0,0,0,0,0,0,5,5,5,5,4,4,3,3,4,3,3,3,3,3,3,4,4,5,5,5,5,0,0,0,0},
    {0,0,0,0,0,0,0,5,5,5,5,4,3,4,4,4,4,4,3,4,4,3,3,4,5,5,5,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,5,5,5,4,4,4,4,4,4,4,3,4,4,4,3,4,5,5,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,5,5,4,4,4,5,5,4,4,4,3,4,4,4,4,4,4,5,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,5,5,4,5,5,5,5,5,5,4,4,4,5,5,5,4,4,5,5,0,0,0,0,0},
    {0,0,0,0,0,0,0,5,5,4,5,5,5,5,5,5,5,5,4,5,5,5,5,5,5,4,5,0,0,0,0,0},
    {0,0,0,0,0,0,0,5,5,5,5,5,0,0,0,5,5,5,4,5,5,5,5,5,5,5,5,5,0,0,0,0},
    {0,0,0,0,0,0,5,5,5,5,0,0,0,0,0,0,5,5,5,5,5,5,0,0,0,5,5,5,0,0,0,0},
    {0,0,0,0,0,5,5,5,0,0,0,0,0,0,0,0,0,5,5,5,5,0,0,0,0,0,0,5,5,0,0,0},
    {0,0,0,0,0,5,5,0,0,0,0,0,0,0,0,0,0,5,5,5,5,0,0,0,0,0,0,0,5,0,0,0},
    {0,0,0,0,5,0,0,0,0,0,0,0,0,0,0,0,0,0,5,5,0,0,0,0,0,0,0,0,0,5,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,5,0,0,0,0,0,0,0,0,0,0,0,0},
};
/* THE DROWNER, image 0x02FF8's effect 5 - sixteen frames of 16 x 10 in two
 * planes, 40 bytes each, the walker's own geometry. Found against the
 * original's frames of "Watch out, there`s traps about": frames 140, 141 and
 * 142 are anim 1, 2 and 3 and every one of them matches base + anim * 40 with
 * EVERY ink pixel agreeing - 24 of 24, 23 of 23 and 17 of 17, nothing
 * differing at all, because nothing overlaps a lemming in the water. */
#define DROWN_SPR_BASE  2832
#define DROWN_SPR_H     10
#define DROWN_SPR_FRAMES 16

/* THE FIRE DEATH, image 0x0304F's effect 6 - fourteen frames of a lemming
 * coming apart into ash, which is what the sprites show: frame 0 is a
 * lemming standing, 4, 8 and 12 are scattered particles. Its file frame is
 * image 0x0306D's [si+0x12] = 0x70 = 112, which for sixteen wide and
 * fourteen rows is FOUR planes - the only sprite here that uses all four,
 * and the colours in it go up to 0x0D.
 *
 * Found against the original's own frames of "I have a cunning plan":
 * frames 204 and 205 score 32 of 32 and 31 of 31 with nothing differing at
 * base + anim * 112, and frames 209 and 212 pick out indices 1 and 0 of the
 * same base with 31 of 31 and 32 of 32 - a second lemming arriving at the
 * same fire and starting its own animation. */
/* THE EXPLOSION'S DEBRIS - image 0x06E83, and the table at ds:0x2788.
 *
 * Fifty-two rows of eighty (dx, dy) pairs, one row per frame of the cloud's
 * own counter: image 0x02350 runs [si+0x28] from 0 to 0x34 and image 0x03662
 * calls the drawer INSTEAD of the lemming's sprite for every frame from 1 up,
 * indexing (anim - 1) * 160. The offsets are absolute from the lemming - image
 * 0x06EDE pushes and pops bp and dx around each pair, so they do not add up -
 * and a pair whose first byte is 0x80 is a particle already gone, SKIPPED and
 * not the end of the row (image 0x06EDB jumps to the dec cx at image 0x06F74).
 * Reading it as a terminator says row 0 holds eighty and the rest are empty,
 * which is wrong and was written up here as a finding once.
 *
 * Dumped from a running level this is byte-identical to the image, so it is
 * static data and nothing computes it. 8,320 bytes, transcribed whole. */
#define DEBRIS_ROWS  52
#define DEBRIS_N     80    /* ds:0x4D - image 0x0042C stores 0x50 over the
                            * 0x80 the image ships with, so eighty and not 128 */
#define DEBRIS_GONE  (-128)  /* 0x80: this one has flown off already */
static const int8_t DEBRIS_AT[DEBRIS_ROWS][DEBRIS_N][2] = {
  { /* frame 1 */
    { -52,-100}, { -23, -47}, {   7, -25}, {  -2, -21}, {   3, -14}, {  -8, -11}, {  -4,  -8}, {   3,  -7},
    {  -6,  -6}, {   3,  -8}, {  -2, -13}, {  -8,  -9}, {  -1, -10}, {  -6,  -3}, {  -5, -11}, {   1,  -4},
    {  -2,  -6}, {  -6, -10}, {   2,  -2}, {   5,  -8}, {   2,  -7}, {  -7,  -7}, {  -5,  -1}, {   2,  -8},
    {   1,  -5}, {  -2,  -5}, {  -6,  -6}, {   2,   1}, {  -1,   0}, {   0,  -8}, {   4,   0}, {  -2,  -4},
    {  -2,   0}, {   3,  -8}, {   5,  -1}, {   0,  -6}, {  -3,  -6}, {  -3,  -7}, {   3,   0}, {  -4,  -5},
    {   5,  -5}, {   4,   0}, {   0,   2}, {  -2,   1}, {   1,  -5}, {  -4,   2}, {  -1,   2}, {  -2,  -1},
    {   4,  -3}, {  -3,  -7}, {  -4,  -2}, {   1,  -2}, {  -4,  -6}, {  -4,  -4}, {  -5,  -5}, {   1,  -2},
    {   3,  -1}, {   0,   0}, {   1,   0}, {   3,   0}, {   3,  -2}, {   0,  -3}, {  -2,   2}, {  -1,  -3},
    {   3,  -1}, {  -1,  -2}, {  -3,   2}, {  -1,   3}, {  -2,  -4}, {   3,  -5}, {   0,  -2}, {  -3,  -3},
    {  -1,   1}, {  -5,  -3}, {   2,  -3}, {   2,   1}, {  -1,   1}, {  -2,  -2}, {   1,  -5}, {  -1,   1},
  },
  { /* frame 2 */
    {-128,-128}, { -47, -93}, {  15, -53}, {  -4, -44}, {   4, -30}, { -11, -24}, {  -7, -19}, {   9, -18},
    { -10, -14}, {   9, -18}, {  -7, -21}, { -10, -16}, {  -4, -16}, {  -8, -10}, {  -8, -16}, {   4,  -9},
    {  -1, -12}, {  -8, -14}, {   1,  -7}, {   7, -13}, {   2, -11}, {  -9, -10}, {  -8,  -4}, {   2, -12},
    {   3,  -8}, {   1,  -8}, {  -9,  -8}, {   1,  -2}, {  -1,  -3}, {   1, -11}, {   5,  -2}, {  -2,  -6},
    {  -2,  -3}, {   5, -10}, {   5,  -3}, {   2,  -9}, {  -5,  -9}, {  -3, -10}, {   2,  -2}, {  -3,  -7},
    {   6,  -7}, {   5,  -1}, {   1,   0}, {  -1,  -1}, {   2,  -6}, {  -3,   1}, {  -2,   1}, {  -2,  -3},
    {   5,  -5}, {  -3,  -8}, {  -4,  -3}, {   1,  -4}, {  -5,  -8}, {  -5,  -6}, {  -5,  -6}, {   1,  -3},
    {   3,  -3}, {   0,  -2}, {   1,  -1}, {   3,  -2}, {   3,  -3}, {   1,  -5}, {  -2,   1}, {  -2,  -5},
    {   1,  -2}, {  -1,  -3}, {  -3,   1}, {   0,   2}, {  -2,  -5}, {   3,  -6}, {   1,  -3}, {  -2,  -4},
    {  -1,   0}, {  -6,  -4}, {   1,  -4}, {   2,   0}, {  -1,   0}, {  -3,  -3}, {   2,  -6}, {   0,   0},
  },
  { /* frame 3 */
    {-128,-128}, {-128,-128}, {  23, -80}, {  -6, -67}, {   5, -46}, { -14, -36}, { -10, -31}, {  15, -28},
    { -13, -22}, {  14, -28}, { -12, -29}, { -13, -23}, {  -8, -22}, { -10, -17}, { -10, -22}, {   7, -14},
    {   0, -17}, {  -9, -18}, {   1, -12}, {   9, -17}, {   1, -15}, { -10, -14}, { -10,  -7}, {   3, -16},
    {   5, -11}, {   3, -10}, { -11, -11}, {   0,  -4}, {  -1,  -5}, {   3, -13}, {   6,  -4}, {  -1,  -9},
    {  -3,  -5}, {   6, -13}, {   5,  -5}, {   4, -11}, {  -6, -11}, {  -3, -12}, {   2,  -4}, {  -3,  -9},
    {   6,  -9}, {   7,  -3}, {   3,  -2}, {   0,  -3}, {   4,  -8}, {  -3,  -1}, {  -3,   0}, {  -1,  -5},
    {   7,  -6}, {  -3, -10}, {  -4,  -5}, {   2,  -5}, {  -5,  -9}, {  -6,  -7}, {  -6,  -7}, {   0,  -4},
    {   4,  -4}, {   0,  -3}, {   1,  -2}, {   3,  -3}, {   3,  -4}, {   2,  -6}, {  -2,  -1}, {  -2,  -5},
    {   0,  -3}, {   0,  -4}, {  -4,   0}, {   0,   1}, {  -3,  -6}, {   3,  -7}, {   1,  -4}, {  -1,  -5},
    {  -1,  -1}, {  -6,  -5}, {   1,  -5}, {   2,  -1}, {  -1,   0}, {  -3,  -3}, {   2,  -7}, {   0,   0},
  },
  { /* frame 4 */
    {-128,-128}, {-128,-128}, {  30,-107}, {  -7, -90}, {   5, -62}, { -17, -48}, { -12, -42}, {  21, -39},
    { -17, -30}, {  19, -37}, { -16, -37}, { -16, -30}, { -11, -27}, { -13, -23}, { -12, -27}, {  10, -19},
    {   1, -22}, { -10, -22}, {   0, -16}, {  11, -22}, {   1, -18}, { -12, -17}, { -13, -10}, {   3, -19},
    {   7, -14}, {   5, -13}, { -13, -13}, {  -2,  -6}, {   0,  -7}, {   4, -16}, {   6,  -6}, {   0, -11},
    {  -3,  -7}, {   7, -15}, {   5,  -7}, {   6, -12}, {  -8, -13}, {  -3, -14}, {   1,  -6}, {  -2, -10},
    {   7, -11}, {   8,  -5}, {   4,  -4}, {   1,  -4}, {   5,  -9}, {  -2,  -2}, {  -5,  -1}, {  -1,  -6},
    {   8,  -7}, {  -3, -11}, {  -4,  -6}, {   2,  -6}, {  -6, -10}, {  -7,  -8}, {  -6,  -8}, {   0,  -5},
    {   5,  -5}, {   1,  -4}, {   1,  -3}, {   3,  -3}, {   4,  -5}, {   3,  -6}, {  -2,  -2}, {  -2,  -6},
    {  -1,  -3}, {   0,  -5}, {  -5,  -1}, {   1,   0}, {  -3,  -6}, {   3,  -8}, {   2,  -5}, {  -1,  -6},
    {  -1,  -2}, {  -7,  -5}, {   0,  -5}, {   3,  -2}, {   0,  -1}, {  -4,  -4}, {   3,  -7}, {   0,  -1},
  },
  { /* frame 5 */
    {-128,-128}, {-128,-128}, {-128,-128}, {  -9,-112}, {   6, -77}, { -20, -60}, { -15, -53}, {  27, -49},
    { -20, -38}, {  24, -46}, { -21, -44}, { -19, -36}, { -14, -33}, { -15, -30}, { -15, -32}, {  14, -23},
    {   2, -27}, { -12, -26}, {  -1, -21}, {  13, -26}, {   1, -21}, { -13, -20}, { -15, -13}, {   3, -22},
    {   9, -16}, {   8, -15}, { -15, -15}, {  -3,  -8}, {   0,  -9}, {   5, -18}, {   7,  -8}, {   1, -13},
    {  -3,  -8}, {   9, -17}, {   5,  -8}, {   7, -14}, {  -9, -15}, {  -3, -16}, {   1,  -7}, {  -1, -12},
    {   8, -12}, {  10,  -6}, {   6,  -5}, {   2,  -5}, {   7, -10}, {  -2,  -3}, {  -6,  -2}, {   0,  -7},
    {  10,  -8}, {  -3, -12}, {  -5,  -7}, {   2,  -7}, {  -6, -11}, {  -8,  -9}, {  -7,  -9}, {   0,  -6},
    {   6,  -5}, {   1,  -5}, {   1,  -4}, {   2,  -4}, {   4,  -6}, {   4,  -7}, {  -1,  -2}, {  -3,  -7},
    {  -2,  -4}, {   0,  -5}, {  -5,  -1}, {   1,   0}, {  -3,  -7}, {   3,  -8}, {   2,  -5}, {   0,  -6},
    {  -1,  -2}, {  -7,  -6}, {  -1,  -5}, {   3,  -2}, {   0,  -1}, {  -4,  -4}, {   4,  -7}, {   0,  -1},
  },
  { /* frame 6 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {   6, -92}, { -23, -71}, { -18, -64}, {  33, -58},
    { -23, -45}, {  29, -55}, { -26, -52}, { -21, -42}, { -17, -38}, { -18, -36}, { -17, -36}, {  17, -28},
    {   4, -32}, { -13, -30}, {  -2, -25}, {  16, -30}, {   1, -25}, { -15, -22}, { -18, -15}, {   3, -25},
    {  11, -19}, {  10, -17}, { -17, -17}, {  -4, -10}, {   0, -10}, {   6, -20}, {   8, -10}, {   2, -14},
    {  -4, -10}, {  10, -18}, {   5, -10}, {   9, -16}, { -11, -17}, {  -3, -17}, {   0,  -8}, {  -1, -13},
    {   8, -13}, {  11,  -7}, {   7,  -7}, {   3,  -7}, {   9, -10}, {  -1,  -3}, {  -7,  -3}, {   0,  -8},
    {  11,  -9}, {  -3, -12}, {  -5,  -7}, {   2,  -8}, {  -7, -12}, {  -9, -10}, {  -8,  -9}, {  -1,  -7},
    {   7,  -6}, {   1,  -5}, {   2,  -4}, {   2,  -4}, {   4,  -6}, {   5,  -7}, {  -1,  -3}, {  -3,  -7},
    {  -3,  -4}, {   1,  -5}, {  -6,  -1}, {   2,   0}, {  -4,  -7}, {   3,  -8}, {   3,  -5}, {   1,  -6},
    {  -2,  -2}, {  -8,  -6}, {  -1,  -6}, {   3,  -2}, {   0,  -1}, {  -5,  -5}, {   4,  -7}, {   1,  -1},
  },
  { /* frame 7 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {   7,-107}, { -26, -82}, { -20, -74}, {  39, -68},
    { -27, -53}, {  34, -64}, { -31, -59}, { -24, -48}, { -20, -43}, { -20, -42}, { -19, -41}, {  20, -32},
    {   5, -36}, { -14, -33}, {  -2, -29}, {  18, -34}, {   0, -28}, { -16, -25}, { -21, -18}, {   4, -28},
    {  13, -21}, {  12, -19}, { -19, -18}, {  -6, -11}, {   1, -12}, {   8, -22}, {   8, -11}, {   3, -16},
    {  -4, -11}, {  11, -20}, {   5, -11}, {  11, -17}, { -13, -18}, {  -3, -19}, {   0, -10}, {   0, -14},
    {   9, -14}, {  13,  -8}, {   8,  -8}, {   4,  -7}, {  10, -11}, {  -1,  -4}, {  -8,  -3}, {   0,  -9},
    {  13, -10}, {  -3, -13}, {  -5,  -8}, {   3,  -8}, {  -7, -13}, { -10, -10}, {  -8,  -9}, {  -1,  -7},
    {   7,  -6}, {   2,  -6}, {   2,  -5}, {   2,  -5}, {   5,  -7}, {   6,  -8}, {  -1,  -3}, {  -3,  -7},
    {  -4,  -5}, {   1,  -5}, {  -7,  -1}, {   2,   0}, {  -4,  -8}, {   3,  -8}, {   4,  -5}, {   2,  -6},
    {  -2,  -2}, {  -8,  -6}, {  -2,  -6}, {   3,  -2}, {   1,  -1}, {  -6,  -5}, {   5,  -7}, {   1,  -1},
  },
  { /* frame 8 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {   8,-122}, { -29, -93}, { -23, -85}, {  45, -78},
    { -30, -60}, {  39, -73}, { -35, -66}, { -27, -54}, { -23, -48}, { -23, -47}, { -22, -45}, {  23, -36},
    {   6, -41}, { -15, -36}, {  -3, -33}, {  20, -37}, {   0, -30}, { -18, -27}, { -23, -20}, {   4, -31},
    {  15, -23}, {  14, -20}, { -22, -19}, {  -7, -13}, {   1, -13}, {   9, -24}, {   9, -13}, {   4, -17},
    {  -5, -12}, {  13, -21}, {   6, -12}, {  13, -18}, { -14, -19}, {  -3, -20}, {  -1, -10}, {   1, -14},
    {  10, -15}, {  14,  -8}, {  10,  -9}, {   5,  -8}, {  12, -11}, {  -1,  -4}, { -10,  -4}, {   1,  -9},
    {  14, -10}, {  -3, -13}, {  -5,  -8}, {   3,  -9}, {  -8, -13}, { -11, -11}, {  -9,  -9}, {  -1,  -7},
    {   8,  -6}, {   2,  -6}, {   2,  -5}, {   2,  -5}, {   5,  -7}, {   7,  -8}, {  -1,  -3}, {  -4,  -7},
    {  -5,  -5}, {   1,  -5}, {  -7,  -1}, {   3,   0}, {  -4,  -8}, {   3,  -8}, {   4,  -5}, {   3,  -6},
    {  -2,  -2}, {  -8,  -6}, {  -3,  -5}, {   3,  -2}, {   1,  -1}, {  -6,  -5}, {   5,  -7}, {   1,   0},
  },
  { /* frame 9 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, { -32,-104}, { -26, -95}, {  51, -87},
    { -34, -67}, {  44, -81}, { -40, -72}, { -30, -59}, { -26, -53}, { -25, -53}, { -24, -49}, {  27, -40},
    {   7, -45}, { -17, -39}, {  -4, -36}, {  22, -40}, {   0, -33}, { -19, -29}, { -26, -22}, {   4, -33},
    {  17, -25}, {  17, -22}, { -24, -21}, {  -8, -14}, {   1, -14}, {  10, -25}, {  10, -14}, {   5, -18},
    {  -5, -13}, {  14, -22}, {   6, -13}, {  15, -19}, { -16, -20}, {  -4, -21}, {  -1, -11}, {   2, -15},
    {  10, -16}, {  15,  -9}, {  11,  -9}, {   6,  -9}, {  13, -11}, {   0,  -4}, { -11,  -4}, {   1, -10},
    {  16, -10}, {  -3, -13}, {  -6,  -8}, {   3,  -9}, {  -9, -13}, { -12, -11}, {  -9,  -9}, {  -2,  -7},
    {   9,  -6}, {   2,  -6}, {   2,  -5}, {   2,  -4}, {   5,  -7}, {   8,  -7}, {   0,  -3}, {  -4,  -7},
    {  -7,  -4}, {   2,  -5}, {  -8,   0}, {   3,   0}, {  -5,  -7}, {   3,  -8}, {   5,  -5}, {   4,  -6},
    {  -2,  -2}, {  -9,  -5}, {  -3,  -5}, {   3,  -2}, {   1,   0}, {  -7,  -4}, {   6,  -6}, {   1,   0},
  },
  { /* frame 10 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, { -35,-115}, { -28,-105}, {  57, -96},
    { -37, -73}, {  49, -89}, { -45, -79}, { -32, -65}, { -29, -58}, { -28, -58}, { -26, -53}, {  30, -43},
    {   8, -49}, { -18, -42}, {  -5, -39}, {  24, -44}, {   0, -35}, { -21, -31}, { -28, -23}, {   5, -36},
    {  19, -26}, {  19, -23}, { -26, -22}, { -10, -15}, {   1, -15}, {  11, -26}, {  10, -15}, {   6, -19},
    {  -5, -14}, {  15, -23}, {   6, -14}, {  17, -19}, { -17, -21}, {  -4, -22}, {  -2, -12}, {   2, -15},
    {  11, -16}, {  17,  -9}, {  12, -10}, {   7,  -9}, {  15, -11}, {   0,  -4}, { -12,  -4}, {   2, -10},
    {  17, -10}, {  -2, -13}, {  -6,  -8}, {   3,  -9}, {  -9, -13}, { -13, -11}, { -10,  -9}, {  -2,  -7},
    {  10,  -6}, {   3,  -6}, {   2,  -4}, {   2,  -4}, {   6,  -6}, {   9,  -7}, {   0,  -3}, {  -4,  -7},
    {  -8,  -4}, {   2,  -4}, {  -8,   0}, {   4,   1}, {  -5,  -7}, {   3,  -7}, {   5,  -4}, {   5,  -5},
    {  -2,  -1}, {  -9,  -5}, {  -4,  -4}, {   3,  -2}, {   1,   0}, {  -8,  -4}, {   6,  -5}, {   2,   1},
  },
  { /* frame 11 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, { -38,-126}, { -31,-115}, {  63,-105},
    { -41, -80}, {  55, -97}, { -50, -85}, { -35, -70}, { -33, -62}, { -30, -63}, { -29, -57}, {  33, -46},
    {  10, -53}, { -19, -45}, {  -6, -43}, {  26, -47}, {  -1, -37}, { -22, -33}, { -31, -25}, {   5, -38},
    {  21, -28}, {  21, -24}, { -28, -22}, { -11, -15}, {   2, -15}, {  13, -28}, {  11, -15}, {   7, -20},
    {  -6, -14}, {  17, -24}, {   6, -14}, {  18, -20}, { -19, -22}, {  -4, -22}, {  -2, -12}, {   3, -15},
    {  12, -16}, {  18,  -9}, {  14, -10}, {   9,  -9}, {  16, -11}, {   1,  -4}, { -14,  -3}, {   2, -10},
    {  19, -10}, {  -2, -12}, {  -6,  -8}, {   4,  -9}, { -10, -13}, { -14, -10}, { -10,  -8}, {  -2,  -7},
    {  11,  -6}, {   3,  -5}, {   2,  -4}, {   2,  -3}, {   6,  -6}, {  10,  -6}, {   0,  -3}, {  -5,  -6},
    {  -9,  -3}, {   2,  -3}, {  -9,   1}, {   4,   2}, {  -5,  -6}, {   3,  -6}, {   6,  -4}, {   6,  -5},
    {  -2,  -1}, { -10,  -4}, {  -5,  -3}, {   3,  -1}, {   2,   1}, {  -8,  -3}, {   7,  -4}, {   2,   2},
  },
  { /* frame 12 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, { -34,-124}, {  69,-113},
    { -44, -86}, {  60,-105}, { -55, -91}, { -38, -75}, { -36, -66}, { -33, -68}, { -31, -60}, {  37, -49},
    {  11, -56}, { -21, -47}, {  -6, -46}, {  28, -49}, {  -1, -39}, { -24, -34}, { -33, -26}, {   5, -39},
    {  23, -29}, {  24, -25}, { -30, -23}, { -12, -16}, {   2, -16}, {  14, -28}, {  12, -16}, {   8, -20},
    {  -6, -15}, {  18, -24}, {   6, -15}, {  20, -20}, { -21, -22}, {  -4, -23}, {  -3, -12}, {   4, -15},
    {  12, -16}, {  20,  -9}, {  15, -10}, {  10,  -9}, {  18, -10}, {   1,  -3}, { -15,  -3}, {   2,  -9},
    {  20, -10}, {  -2, -12}, {  -6,  -7}, {   4,  -8}, { -10, -13}, { -15, -10}, { -11,  -8}, {  -3,  -6},
    {  12,  -5}, {   3,  -5}, {   3,  -3}, {   2,  -2}, {   7,  -5}, {  11,  -5}, {   1,  -2}, {  -5,  -5},
    { -10,  -3}, {   2,  -2}, { -10,   2}, {   5,   3}, {  -6,  -5}, {   3,  -5}, {   7,  -3}, {   6,  -4},
    {  -2,   0}, { -10,  -3}, {  -5,  -2}, {   3,   0}, {   2,   2}, {  -9,  -2}, {   8,  -3}, {   2,   3},
  },
  { /* frame 13 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {  75,-122},
    { -48, -92}, {  65,-113}, { -59, -97}, { -40, -80}, { -39, -70}, { -35, -72}, { -33, -63}, {  40, -52},
    {  12, -59}, { -22, -49}, {  -7, -48}, {  30, -52}, {  -1, -41}, { -26, -36}, { -36, -27}, {   6, -41},
    {  25, -30}, {  26, -25}, { -32, -23}, { -14, -16}, {   2, -16}, {  15, -29}, {  13, -16}, {   9, -20},
    {  -7, -15}, {  19, -24}, {   6, -15}, {  22, -20}, { -22, -22}, {  -4, -23}, {  -3, -12}, {   4, -15},
    {  13, -16}, {  21,  -9}, {  17, -10}, {  11,  -9}, {  20,  -9}, {   1,  -2}, { -16,  -2}, {   3,  -9},
    {  22,  -9}, {  -2, -11}, {  -7,  -7}, {   4,  -7}, { -11, -12}, { -16,  -9}, { -12,  -7}, {  -3,  -5},
    {  12,  -4}, {   3,  -4}, {   3,  -2}, {   2,  -1}, {   7,  -4}, {  12,  -4}, {   1,  -1}, {  -5,  -4},
    { -11,  -1}, {   3,  -1}, { -10,   3}, {   5,   4}, {  -6,  -4}, {   3,  -4}, {   7,  -2}, {   7,  -2},
    {  -2,   1}, { -11,  -2}, {  -6,  -1}, {   4,   1}, {   2,   4}, {  -9,  -1}, {   8,  -2}, {   2,   4},
  },
  { /* frame 14 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    { -51, -98}, {  70,-120}, { -64,-103}, { -43, -84}, { -42, -74}, { -37, -77}, { -36, -66}, {  43, -55},
    {  13, -63}, { -23, -51}, {  -8, -51}, {  32, -54}, {  -1, -43}, { -27, -37}, { -38, -28}, {   6, -42},
    {  27, -31}, {  28, -25}, { -35, -24}, { -15, -16}, {   2, -16}, {  16, -30}, {  13, -16}, {  10, -21},
    {  -7, -15}, {  21, -24}, {   7, -14}, {  24, -20}, { -24, -23}, {  -4, -23}, {  -4, -12}, {   5, -14},
    {  13, -16}, {  22,  -8}, {  18, -10}, {  12,  -8}, {  21,  -8}, {   2,  -1}, { -18,  -1}, {   3,  -8},
    {  23,  -8}, {  -2, -10}, {  -7,  -6}, {   5,  -7}, { -11, -11}, { -17,  -8}, { -12,  -6}, {  -4,  -4},
    {  13,  -3}, {   4,  -3}, {   3,  -1}, {   2,   0}, {   7,  -3}, {  13,  -3}, {   1,   0}, {  -6,  -3},
    { -12,   0}, {   3,   0}, { -11,   5}, {   6,   5}, {  -6,  -3}, {   3,  -2}, {   8,   0}, {   8,  -1},
    {  -2,   3}, { -11,   0}, {  -7,   1}, {   4,   3}, {   3,   5}, { -10,   1}, {   9,   0}, {   2,   6},
  },
  { /* frame 15 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    { -55,-104}, {  75,-127}, { -69,-108}, { -46, -88}, { -45, -77}, { -40, -81}, { -38, -69}, {  46, -58},
    {  14, -66}, { -25, -53}, {  -9, -53}, {  35, -56}, {  -2, -44}, { -29, -38}, { -41, -28}, {   6, -44},
    {  29, -32}, {  31, -26}, { -37, -24}, { -16, -16}, {   3, -16}, {  17, -30}, {  14, -16}, {  11, -20},
    {  -7, -14}, {  22, -24}, {   7, -14}, {  26, -20}, { -25, -22}, {  -4, -23}, {  -4, -11}, {   6, -13},
    {  14, -15}, {  24,  -8}, {  19,  -9}, {  13,  -8}, {  23,  -7}, {   2,   0}, { -19,   0}, {   3,  -7},
    {  25,  -7}, {  -2,  -9}, {  -7,  -5}, {   5,  -5}, { -12, -10}, { -18,  -7}, { -13,  -4}, {  -4,  -3},
    {  14,  -2}, {   4,  -2}, {   3,   0}, {   2,   1}, {   8,  -2}, {  14,  -2}, {   1,   1}, {  -6,  -1},
    { -13,   1}, {   3,   2}, { -12,   6}, {   7,   7}, {  -7,  -2}, {   3,  -1}, {   8,   1}, {   9,   0},
    {  -2,   4}, { -11,   1}, {  -7,   2}, {   4,   4}, {   3,   7}, { -11,   2}, {   9,   2}, {   3,   8},
  },
  { /* frame 16 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    { -58,-109}, {-128,-128}, { -74,-114}, { -49, -93}, { -48, -81}, { -42, -85}, { -40, -72}, {  50, -60},
    {  16, -68}, { -26, -55}, { -10, -55}, {  37, -58}, {  -2, -45}, { -30, -38}, { -43, -29}, {   6, -45},
    {  31, -32}, {  33, -26}, { -39, -23}, { -18, -16}, {   3, -15}, {  19, -30}, {  15, -15}, {  12, -20},
    {  -8, -14}, {  23, -24}, {   7, -14}, {  28, -19}, { -27, -22}, {  -4, -22}, {  -5, -10}, {   6, -12},
    {  15, -15}, {  25,  -7}, {  21,  -8}, {  14,  -7}, {  24,  -6}, {   3,   1}, { -20,   1}, {   4,  -6},
    {  26,  -6}, {  -2,  -7}, {  -7,  -3}, {   5,  -4}, { -12,  -9}, { -19,  -6}, { -13,  -3}, {  -4,  -2},
    {  15,   0}, {   4,   0}, {   3,   1}, {   1,   3}, {   8,   0}, {  15,   0}, {   2,   2}, {  -6,   0},
    { -15,   3}, {   4,   4}, { -12,   8}, {   7,   9}, {  -7,   0}, {   3,   1}, {   9,   3}, {  10,   2},
    {  -2,   6}, { -12,   3}, {  -8,   4}, {   4,   6}, {   3,   9}, { -11,   4}, {  10,   4}, {   3,  10},
  },
  { /* frame 17 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    { -62,-114}, {-128,-128}, { -78,-119}, { -51, -97}, { -51, -84}, { -45, -89}, { -43, -75}, {  53, -62},
    {  17, -71}, { -27, -56}, { -10, -57}, {  39, -60}, {  -2, -46}, { -32, -39}, { -46, -29}, {   7, -46},
    {  33, -32}, {  35, -25}, { -41, -23}, { -19, -16}, {   3, -14}, {  20, -30}, {  15, -15}, {  13, -20},
    {  -8, -13}, {  25, -23}, {   7, -13}, {  29, -18}, { -28, -21}, {  -4, -22}, {  -5, -10}, {   7, -11},
    {  15, -14}, {  27,  -6}, {  22,  -7}, {  15,  -6}, {  26,  -4}, {   3,   3}, { -21,   3}, {   4,  -5},
    {  28,  -5}, {  -2,  -6}, {  -8,  -2}, {   5,  -3}, { -13,  -8}, { -19,  -5}, { -14,  -1}, {  -5,   0},
    {  16,   1}, {   5,   1}, {   3,   3}, {   1,   5}, {   8,   1}, {  16,   2}, {   2,   4}, {  -7,   2},
    { -16,   5}, {   4,   6}, { -13,  10}, {   8,  11}, {  -7,   2}, {   3,   3}, {  10,   5}, {  11,   4},
    {  -2,   8}, { -12,   5}, {  -9,   6}, {   4,   8}, {   3,  11}, { -12,   6}, {  11,   6}, {   3,  12},
  },
  { /* frame 18 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    { -65,-120}, {-128,-128}, { -83,-124}, { -54,-100}, { -55, -87}, { -47, -93}, { -45, -77}, {  56, -64},
    {  18, -73}, { -29, -57}, { -11, -59}, {  41, -62}, {  -2, -47}, { -33, -39}, { -48, -29}, {   7, -46},
    {  35, -32}, {  38, -25}, { -43, -22}, { -20, -15}, {   4, -14}, {  21, -29}, {  16, -14}, {  14, -19},
    {  -8, -12}, {  26, -23}, {   7, -12}, {  31, -17}, { -30, -21}, {  -4, -21}, {  -6,  -8}, {   8, -10},
    {  16, -12}, {  28,  -4}, {  23,  -6}, {  16,  -5}, {  27,  -3}, {   3,   4}, { -23,   5}, {   5,  -4},
    {  29,  -3}, {  -2,  -4}, {  -8,   0}, {   6,  -1}, { -13,  -6}, { -20,  -3}, { -14,   1}, {  -5,   2},
    {  16,   3}, {   5,   3}, {   3,   5}, {   1,   7}, {   9,   3}, {  17,   4}, {   2,   6}, {  -7,   4},
    { -17,   7}, {   4,   8}, { -14,  13}, {   8,  13}, {  -8,   4}, {   3,   5}, {  10,   7}, {  12,   6},
    {  -3,  10}, { -13,   7}, {  -9,   9}, {   4,  10}, {   4,  13}, { -12,   8}, {  11,   8}, {   3,  14},
  },
  { /* frame 19 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    { -69,-124}, {-128,-128}, { -88,-128}, { -57,-104}, { -58, -89}, { -50, -96}, { -47, -79}, {  60, -66},
    {  19, -75}, { -30, -58}, { -12, -60}, {  43, -63}, {  -3, -47}, { -35, -39}, { -51, -29}, {   7, -47},
    {  37, -32}, {  40, -24}, { -45, -21}, { -22, -14}, {   4, -13}, {  22, -29}, {  17, -13}, {  15, -18},
    {  -9, -11}, {  27, -22}, {   7, -11}, {  33, -16}, { -32, -20}, {  -4, -20}, {  -6,  -7}, {   9,  -8},
    {  17, -11}, {  30,  -3}, {  25,  -5}, {  17,  -3}, {  29,  -1}, {   4,   6}, { -24,   7}, {   5,  -2},
    {  31,  -1}, {  -2,  -2}, {  -8,   2}, {   6,   1}, { -14,  -4}, { -21,  -1}, { -15,   3}, {  -5,   4},
    {  17,   5}, {   5,   5}, {   4,   7}, {   1,   9}, {   9,   5}, {  18,   6}, {   2,   8}, {  -7,   6},
    { -18,   9}, {   5,  10}, { -14,  15}, {   9,  15}, {  -8,   6}, {   3,   8}, {  11,   9}, {  13,   9},
    {  -3,  12}, { -13,  10}, { -10,  11}, {   4,  12}, {   4,  16}, { -13,  11}, {  12,  11}, {   4,  17},
  },
  { /* frame 20 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, { -60,-107}, { -61, -92}, { -52, -99}, { -50, -81}, {  63, -67},
    {  21, -77}, { -31, -59}, { -13, -62}, {  45, -64}, {  -3, -48}, { -36, -39}, { -54, -29}, {   8, -47},
    {  39, -32}, {  42, -24}, { -48, -20}, { -23, -13}, {   4, -11}, {  24, -28}, {  17, -12}, {  16, -17},
    {  -9, -10}, {  29, -21}, {   8, -10}, {  35, -15}, { -33, -19}, {  -4, -18}, {  -7,  -6}, {   9,  -7},
    {  17,  -9}, {  31,  -1}, {  26,  -3}, {  19,  -2}, {  30,   1}, {   4,   8}, { -25,   9}, {   5,   0},
    {  32,   1}, {  -2,   0}, {  -8,   4}, {   6,   3}, { -14,  -2}, { -22,   1}, { -15,   6}, {  -6,   6},
    {  18,   7}, {   6,   7}, {   4,   9}, {   1,  11}, {  10,   7}, {  18,   8}, {   3,  10}, {  -8,   9},
    { -19,  11}, {   5,  13}, { -15,  18}, {   9,  18}, {  -8,   8}, {   3,  10}, {  11,  12}, {  13,  11},
    {  -3,  15}, { -14,  12}, { -11,  14}, {   4,  15}, {   4,  18}, { -14,  13}, {  12,  14}, {   4,  20},
  },
  { /* frame 21 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, { -62,-110}, { -64, -94}, { -55,-102}, { -52, -82}, {  66, -69},
    {  22, -79}, { -32, -59}, { -14, -63}, {  47, -65}, {  -3, -48}, { -38, -39}, { -56, -28}, {   8, -47},
    {  40, -31}, {  44, -23}, { -50, -19}, { -25, -12}, {   4, -10}, {  25, -27}, {  18, -11}, {  17, -16},
    { -10,  -8}, {  30, -19}, {   8,  -8}, {  37, -13}, { -35, -17}, {  -4, -17}, {  -7,  -4}, {  10,  -5},
    {  18,  -8}, {  32,   1}, {  27,  -2}, {  20,   0}, {  32,   4}, {   5,  11}, { -27,  11}, {   6,   2},
    {  34,   3}, {  -2,   2}, {  -9,   6}, {   7,   5}, { -15,   0}, { -23,   3}, { -16,   8}, {  -6,   8},
    {  19,  10}, {   6,  10}, {   4,  12}, {   1,  14}, {  10,  10}, {  19,  11}, {   3,  13}, {  -8,  12},
    { -20,  14}, {   5,  16}, { -15,  20}, {  10,  21}, {  -9,  11}, {   3,  13}, {  12,  15}, {  14,  14},
    {  -3,  18}, { -14,  15}, { -11,  17}, {   4,  17}, {   4,  21}, { -14,  16}, {  13,  17}, {   4,  23},
  },
  { /* frame 22 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, { -65,-113}, { -67, -96}, { -57,-105}, { -54, -84}, {  69, -70},
    {  23, -81}, { -34, -60}, { -14, -64}, {  49, -66}, {  -3, -48}, { -39, -38}, { -59, -28}, {   8, -47},
    {  42, -30}, {  47, -21}, { -52, -18}, { -26, -10}, {   5,  -8}, {  26, -26}, {  19,  -9}, {  18, -14},
    { -10,  -7}, {  31, -18}, {   8,  -6}, {  39, -11}, { -36, -16}, {  -4, -15}, {  -7,  -2}, {  11,  -3},
    {  19,  -6}, {  34,   3}, {  29,   0}, {  21,   2}, {  34,   6}, {   5,  13}, { -28,  14}, {   6,   4},
    {  35,   5}, {  -2,   5}, {  -9,   9}, {   7,   8}, { -16,   3}, { -24,   6}, { -17,  11}, {  -6,  11},
    {  20,  13}, {   6,  12}, {   4,  15}, {   1,  17}, {  10,  13}, {  20,  14}, {   3,  15}, {  -9,  14},
    { -21,  17}, {   6,  19}, { -16,  24}, {  10,  24}, {  -9,  14}, {   3,  16}, {  13,  18}, {  15,  17},
    {  -3,  21}, { -14,  18}, { -12,  20}, {   5,  20}, {   5,  24}, { -15,  19}, {  14,  20}, {   4,  26},
  },
  { /* frame 23 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, { -68,-116}, { -70, -98}, { -60,-108}, { -56, -85}, {  73, -71},
    {  24, -82}, { -35, -60}, { -15, -65}, {  51, -66}, {  -4, -47}, { -41, -38}, { -61, -27}, {   8, -46},
    {  44, -29}, {  49, -20}, { -54, -16}, { -27,  -9}, {   5,  -7}, {  27, -25}, {  20,  -7}, {  19, -12},
    { -10,  -5}, {  33, -16}, {   8,  -4}, {  40, -10}, { -38, -14}, {  -4, -14}, {  -8,   0}, {  11,   0},
    {  19,  -4}, {  35,   5}, {  30,   2}, {  22,   5}, {  35,   9}, {   5,  16}, { -29,  16}, {   6,   7},
    {  37,   8}, {  -2,   8}, {  -9,  11}, {   7,  10}, { -16,   5}, { -25,   8}, { -17,  14}, {  -7,  14},
    {  20,  15}, {   7,  15}, {   4,  17}, {   1,  20}, {  11,  15}, {  21,  17}, {   4,  18}, {  -9,  17},
    { -23,  20}, {   6,  22}, { -17,  27}, {  11,  27}, {  -9,  17}, {   3,  19}, {  13,  21}, {  16,  20},
    {  -3,  24}, { -15,  21}, { -13,  23}, {   5,  23}, {   5,  28}, { -16,  22}, {  14,  23}, {   5,  29},
  },
  { /* frame 24 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, { -71,-119}, { -73,-100}, { -62,-110}, { -59, -86}, {  76, -71},
    {  25, -83}, { -36, -60}, { -16, -65}, {  53, -67}, {  -4, -47}, { -42, -37}, { -64, -25}, {   9, -46},
    {  46, -28}, {  51, -18}, { -56, -14}, { -29,  -7}, {   5,  -5}, {  29, -23}, {  20,  -5}, {  20, -10},
    { -11,  -3}, {  34, -14}, {   8,  -2}, {  42,  -7}, { -39, -12}, {  -4, -12}, {  -8,   2}, {  12,   2},
    {  20,  -1}, {  37,   8}, {  32,   5}, {  23,   7}, {  37,  12}, {   6,  19}, { -31,  19}, {   7,   9},
    {  38,  11}, {  -2,  11}, {  -9,  14}, {   7,  13}, { -17,   8}, { -26,  11}, { -18,  17}, {  -7,  17},
    {  21,  18}, {   7,  18}, {   4,  21}, {   1,  23}, {  11,  19}, {  22,  20}, {   4,  21}, {  -9,  21},
    { -24,  23}, {   6,  25}, { -17,  30}, {  11,  30}, { -10,  20}, {   3,  23}, {  14,  24}, {  17,  23},
    {  -3,  27}, { -15,  25}, { -13,  26}, {   5,  27}, {   5,  31}, { -16,  26}, {  15,  27}, {   5,  33},
  },
  { /* frame 25 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, { -73,-121}, { -76,-101}, { -64,-113}, { -61, -87}, {  79, -72},
    {  27, -84}, { -38, -60}, { -17, -65}, {  56, -67}, {  -4, -46}, { -44, -36}, { -66, -24}, {   9, -45},
    {  48, -27}, {  54, -17}, { -59, -12}, { -30,  -5}, {   5,  -2}, {  30, -22}, {  21,  -3}, {  21,  -8},
    { -11,   0}, {  35, -12}, {   8,   0}, {  44,  -5}, { -41, -10}, {  -4,  -9}, {  -9,   5}, {  13,   5},
    {  21,   1}, {  38,  11}, {  33,   7}, {  24,  10}, {  38,  15}, {   6,  22}, { -32,  22}, {   7,  12},
    {  40,  14}, {  -2,  14}, { -10,  17}, {   8,  16}, { -17,  11}, { -27,  14}, { -18,  20}, {  -7,  20},
    {  22,  22}, {   7,  21}, {   5,  24}, {   1,  26}, {  11,  22}, {  23,  23}, {   4,  24}, { -10,  24},
    { -25,  26}, {   6,  29}, { -18,  34}, {  12,  34}, { -10,  23}, {   3,  26}, {  14,  28}, {  18,  27},
    {  -3,  31}, { -16,  28}, { -14,  30}, {   5,  30}, {   6,  35}, { -17,  29}, {  15,  30}, {   5,  37},
  },
  { /* frame 26 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, { -76,-123}, { -80,-103}, { -67,-115}, { -63, -88}, {  83, -72},
    {  28, -85}, { -39, -59}, { -18, -66}, {  58, -67}, {  -4, -46}, { -46, -34}, { -69, -23}, {   9, -44},
    {  50, -25}, {  56, -15}, { -61, -10}, { -31,  -2}, {   6,   0}, {  31, -20}, {  22,  -1}, {  21,  -6},
    { -12,   2}, {  37, -10}, {   9,   3}, {  46,  -2}, { -43,  -7}, {  -4,  -7}, {  -9,   8}, {  14,   8},
    {  21,   4}, {  40,  14}, {  34,  10}, {  25,  13}, {  40,  18}, {   7,  25}, { -33,  26}, {   8,  15},
    {  41,  17}, {  -2,  17}, { -10,  21}, {   8,  19}, { -18,  14}, { -28,  18}, { -19,  24}, {  -8,  23},
    {  23,  25}, {   7,  25}, {   5,  27}, {   1,  30}, {  12,  25}, {  24,  27}, {   4,  28}, { -10,  28},
    { -26,  30}, {   7,  33}, { -19,  38}, {  12,  38}, { -10,  27}, {   3,  30}, {  15,  31}, {  19,  31},
    {  -3,  35}, { -16,  32}, { -15,  34}, {   5,  34}, {   6,  39}, { -17,  33}, {  16,  34}, {   5,  41},
  },
  { /* frame 27 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, { -79,-125}, { -83,-104}, { -69,-116}, { -66, -88}, {  86, -72},
    {  29, -85}, { -40, -59}, { -18, -65}, {  60, -67}, {  -5, -44}, { -47, -33}, { -71, -21}, {  10, -43},
    {  52, -23}, {  58, -12}, { -63,  -8}, { -33,   0}, {   6,   3}, {  32, -18}, {  22,   2}, {  22,  -3},
    { -12,   5}, {  38,  -7}, {   9,   5}, {  48,   0}, { -44,  -5}, {  -4,  -4}, { -10,  10}, {  14,  11},
    {  22,   7}, {  41,  17}, {  36,  13}, {  26,  16}, {  41,  22}, {   7,  29}, { -34,  29}, {   8,  19},
    {  43,  20}, {  -2,  21}, { -10,  24}, {   8,  23}, { -18,  18}, { -29,  21}, { -19,  28}, {  -8,  27},
    {  24,  29}, {   8,  28}, {   5,  31}, {   0,  34}, {  12,  29}, {  25,  31}, {   5,  31}, { -10,  32},
    { -27,  34}, {   7,  37}, { -19,  42}, {  13,  42}, { -11,  31}, {   3,  34}, {  16,  35}, {  19,  34},
    {  -3,  38}, { -17,  36}, { -15,  38}, {   5,  38}, {   6,  43}, { -18,  37}, {  16,  39}, {   6,  45},
  },
  { /* frame 28 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, { -82,-127}, { -86,-105}, { -72,-118}, { -68, -89}, {  89, -72},
    {  30, -86}, { -42, -58}, { -19, -65}, {  62, -66}, {  -5, -43}, { -49, -31}, { -74, -19}, {  10, -42},
    {  54, -21}, {  61, -10}, { -65,  -5}, { -34,   3}, {   6,   5}, {  34, -15}, {  23,   4}, {  23,  -1},
    { -12,   8}, {  39,  -4}, {   9,   8}, {  49,   3}, { -46,  -2}, {  -4,  -2}, { -10,  14}, {  15,  14},
    {  23,  10}, {  42,  20}, {  37,  16}, {  27,  19}, {  43,  26}, {   7,  33}, { -36,  33}, {   8,  22},
    {  44,  24}, {  -2,  25}, { -10,  28}, {   8,  27}, { -19,  21}, { -30,  25}, { -20,  32}, {  -8,  30},
    {  24,  33}, {   8,  32}, {   5,  35}, {   0,  38}, {  13,  33}, {  26,  35}, {   5,  35}, { -11,  36},
    { -28,  38}, {   7,  41}, { -20,  46}, {  13,  46}, { -11,  35}, {   3,  38}, {  16,  39}, {  20,  39},
    {  -3,  43}, { -17,  41}, { -16,  42}, {   5,  42}, {   6,  47}, { -19,  41}, {  17,  43}, {   6,  49},
  },
  { /* frame 29 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, { -84,-128}, { -89,-106}, { -74,-120}, { -70, -89}, {  92, -72},
    {  31, -86}, { -43, -57}, { -20, -65}, {  64, -65}, {  -5, -42}, { -50, -29}, { -76, -17}, {  10, -40},
    {  56, -19}, {  63,  -7}, { -67,  -2}, { -35,   6}, {   7,   8}, {  35, -13}, {  24,   7}, {  24,   2},
    { -13,  11}, {  41,  -2}, {   9,  11}, {  51,   6}, { -47,   1}, {  -4,   1}, { -11,  17}, {  16,  18},
    {  23,  14}, {  44,  24}, {  38,  19}, {  29,  22}, {  44,  29}, {   8,  37}, { -37,  37}, {   9,  26},
    {  46,  28}, {  -2,  29}, { -11,  32}, {   9,  30}, { -19,  25}, { -31,  29}, { -21,  36}, {  -9,  34},
    {  25,  37}, {   8,  36}, {   5,  39}, {   0,  42}, {  13,  37}, {  27,  39}, {   5,  39}, { -11,  40},
    { -29,  42}, {   8,  45}, { -20,  50}, {  14,  50}, { -11,  39}, {   3,  43}, {  17,  44}, {  21,  43},
    {  -3,  47}, { -18,  45}, { -17,  47}, {   5,  46}, {   7,  52}, { -19,  46}, {  18,  48}, {   6,  54},
  },
  { /* frame 30 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, { -92,-106}, { -77,-121}, { -73, -89}, {  96, -72},
    {  33, -86}, { -44, -56}, { -21, -64}, {  66, -65}, {  -5, -40}, { -52, -27}, { -79, -14}, {  10, -38},
    {  58, -17}, {  65,  -5}, { -69,   1}, { -37,   9}, {   7,  12}, {  36, -10}, {  24,  10}, {  25,   5},
    { -13,  14}, {  42,   2}, {   9,  15}, {  53,  10}, { -49,   4}, {  -4,   5}, { -11,  20}, {  16,  22},
    {  24,  17}, {  45,  27}, {  40,  23}, {  30,  26}, {  46,  34}, {   8,  41}, { -38,  41}, {   9,  30},
    {  47,  32}, {  -2,  33}, { -11,  36}, {   9,  35}, { -20,  29}, { -32,  33}, { -21,  40}, {  -9,  39},
    {  26,  41}, {   9,  40}, {   5,  43}, {   0,  46}, {  13,  41}, {  28,  44}, {   5,  44}, { -11,  44},
    { -30,  46}, {   8,  50}, { -21,  55}, {  15,  55}, { -12,  44}, {   3,  47}, {  17,  48}, {  22,  47},
    {  -4,  52}, { -18,  50}, { -17,  52}, {   5,  51}, {   7,  57}, { -20,  50}, {  18,  52}, {   6,  59},
  },
  { /* frame 31 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, { -95,-106}, { -79,-122}, { -75, -88}, {  99, -71},
    {  34, -86}, { -46, -54}, { -22, -63}, {  68, -64}, {  -6, -38}, { -53, -25}, { -82, -12}, {  11, -36},
    {  60, -14}, {  68,  -1}, { -72,   4}, { -38,  12}, {   7,  15}, {  37,  -7}, {  25,  14}, {  26,   9},
    { -14,  18}, {  44,   5}, {   9,  18}, {  55,  13}, { -51,   7}, {  -5,   8}, { -12,  24}, {  17,  26},
    {  24,  21}, {  47,  31}, {  41,  27}, {  31,  30}, {  48,  38}, {   9,  45}, { -40,  45}, {   9,  34},
    {  49,  36}, {  -2,  37}, { -11,  40}, {   9,  39}, { -20,  33}, { -33,  37}, { -22,  45}, { -10,  43},
    {  27,  46}, {   9,  45}, {   5,  48}, {   0,  51}, {  14,  46}, {  29,  48}, {   6,  48}, { -12,  49},
    { -32,  51}, {   8,  55}, { -22,  60}, {  15,  60}, { -12,  48}, {   3,  52}, {  18,  53}, {  23,  52},
    {  -4,  56}, { -18,  54}, { -18,  56}, {   6,  55}, {   7,  61}, { -21,  55}, {  19,  57}, {   7,  64},
  },
  { /* frame 32 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, { -98,-107}, { -82,-123}, { -77, -88}, { 102, -70},
    {  35, -85}, { -47, -53}, { -22, -62}, {  70, -62}, {  -6, -36}, { -55, -22}, { -84,  -9}, {  11, -34},
    {  62, -11}, {  70,   2}, { -74,   7}, { -39,  16}, {   7,  19}, {  38,  -4}, {  26,  17}, {  27,  12},
    { -14,  21}, {  45,   8}, {   9,  22}, {  57,  17}, { -52,  11}, {  -5,  12}, { -12,  28}, {  18,  30},
    {  25,  25}, {  48,  35}, {  42,  31}, {  32,  34}, {  49,  42}, {   9,  50}, { -41,  50}, {  10,  38},
    {  50,  40}, {  -2,  42}, { -11,  44}, {  10,  43}, { -21,  38}, { -34,  41}, { -22,  49}, { -10,  48},
    {  28,  50}, {   9,  49}, {   6,  52}, {   0,  56}, {  14,  50}, {  30,  53}, {   6,  53}, { -12,  54},
    { -33,  56}, {   9,  60}, { -22,  65}, {  16,  65}, { -12,  53}, {   3,  57}, {  19,  58}, {  24,  57},
    {  -4,  61}, { -19,  59}, { -19,  62}, {   6,  60}, {   8,  67}, { -21,  60}, {  19,  63}, {   7,  69},
  },
  { /* frame 33 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-101,-106}, { -84,-123}, { -80, -87}, { 106, -69},
    {  36, -84}, { -48, -51}, { -23, -61}, {  72, -61}, {  -6, -34}, { -56, -20}, { -87,  -6}, {  11, -32},
    {  64,  -8}, {  72,   5}, { -76,  11}, { -41,  19}, {   8,  23}, {  40,  -1}, {  26,  21}, {  28,  16},
    { -14,  25}, {  46,  12}, {  10,  26}, {  59,  21}, { -54,  15}, {  -5,  15}, { -13,  32}, {  18,  34},
    {  26,  29}, {  49,  40}, {  44,  35}, {  33,  38}, {  51,  47}, {  10,  54}, { -42,  55}, {  10,  42},
    {  52,  45}, {  -2,  46}, { -12,  49}, {  10,  48}, { -21,  42}, { -35,  46}, { -23,  54}, { -10,  52},
    {  29,  55}, {  10,  54}, {   6,  57}, {   0,  61}, {  14,  55}, {  31,  58}, {   6,  57}, { -12,  59},
    { -34,  61}, {   9,  65}, { -23,  70}, {  16,  70}, { -13,  58}, {   3,  62}, {  19,  63}, {  25,  62},
    {  -4,  66}, { -19,  65}, { -19,  67}, {   6,  65}, {   8,  72}, { -22,  65}, {  20,  68}, {   7,  74},
  },
  { /* frame 34 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-105,-106}, { -87,-124}, { -82, -86}, { 109, -68},
    {  37, -84}, { -49, -49}, { -24, -59}, {  75, -59}, {  -6, -31}, { -58, -17}, { -89,  -3}, {  12, -29},
    {  66,  -5}, {  74,   9}, { -78,  15}, { -42,  23}, {   8,  27}, {  41,   3}, {  27,  25}, {  29,  20},
    { -15,  29}, {  48,  16}, {  10,  30}, {  60,  25}, { -55,  19}, {  -5,  20}, { -13,  36}, {  19,  39},
    {  26,  34}, {  51,  44}, {  45,  39}, {  34,  43}, {  52,  52}, {  10,  59}, { -43,  60}, {  11,  47},
    {  53,  50}, {  -2,  51}, { -12,  54}, {  10,  53}, { -22,  47}, { -36,  51}, { -23,  60}, { -11,  57},
    {  29,  60}, {  10,  59}, {   6,  62}, {   0,  66}, {  15,  60}, {  32,  63}, {   7,  63}, { -13,  64},
    { -35,  66}, {   9,  70}, { -24,  76}, {  17,  75}, { -13,  63}, {   3,  68}, {  20,  68}, {  26,  68},
    {  -4,  72}, { -20,  70}, { -20,  72}, {   6,  71}, {   8,  77}, { -22,  71}, {  21,  74}, {   7,  80},
  },
  { /* frame 35 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-108,-106}, { -89,-124}, { -84, -85}, { 112, -66},
    {  39, -82}, { -51, -47}, { -25, -58}, {  77, -57}, {  -7, -29}, { -59, -14}, { -92,   0}, {  12, -26},
    {  68,  -2}, {  77,  13}, { -80,  19}, { -43,  27}, {   8,  31}, {  42,   7}, {  28,  29}, {  30,  24},
    { -15,  34}, {  49,  20}, {  10,  34}, {  62,  30}, { -57,  23}, {  -5,  24}, { -14,  41}, {  20,  43},
    {  27,  38}, {  52,  49}, {  47,  44}, {  35,  47}, {  54,  57}, {  10,  64}, { -45,  65}, {  11,  52},
    {  55,  55}, {  -2,  57}, { -12,  59}, {  10,  58}, { -23,  52}, { -36,  56}, { -24,  65}, { -11,  63},
    {  30,  65}, {  10,  64}, {   6,  68}, {   0,  71}, {  15,  66}, {  33,  69}, {   7,  68}, { -13,  70},
    { -36,  72}, {  10,  76}, { -24,  81}, {  17,  81}, { -13,  69}, {   3,  73}, {  20,  74}, {  26,  73},
    {  -4,  77}, { -20,  76}, { -21,  78}, {   6,  76}, {   8,  83}, { -23,  76}, {  21,  79}, {   8,  86},
  },
  { /* frame 36 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-111,-105}, { -91,-124}, { -87, -84}, { 115, -65},
    {  40, -81}, { -52, -44}, { -26, -56}, {  79, -55}, {  -7, -26}, { -61, -10}, { -94,   4}, {  12, -23},
    {  70,   2}, {  79,  17}, { -82,  23}, { -45,  31}, {   8,  35}, {  43,  11}, {  29,  34}, {  31,  28},
    { -15,  38}, {  50,  25}, {  10,  39}, {  64,  34}, { -58,  27}, {  -5,  28}, { -14,  46}, {  21,  48},
    {  28,  43}, {  54,  54}, {  48,  48}, {  36,  52}, {  55,  62}, {  11,  70}, { -46,  70}, {  11,  57},
    {  56,  60}, {  -2,  62}, { -12,  64}, {  11,  63}, { -23,  57}, { -37,  61}, { -24,  70}, { -11,  68},
    {  31,  71}, {  11,  70}, {   6,  73}, {   0,  77}, {  15,  71}, {  34,  74}, {   7,  73}, { -13,  75},
    { -37,  77}, {  10,  82}, { -25,  87}, {  18,  87}, { -14,  74}, {   3,  79}, {  21,  80}, {  27,  79},
    {  -4,  83}, { -21,  81}, { -21,  84}, {   6,  82}, {   9,  89}, { -24,  82}, {  22,  85}, {   8,  92},
  },
  { /* frame 37 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-114,-104}, { -94,-124}, { -89, -82}, { 119, -63},
    {  41, -80}, { -53, -42}, { -26, -54}, {  81, -53}, {  -7, -23}, { -63,  -7}, { -97,   8}, {  13, -20},
    {  72,   6}, {  81,  21}, { -85,  27}, { -46,  36}, {   9,  40}, {  45,  15}, {  29,  38}, {  32,  33},
    { -16,  43}, {  52,  29}, {  10,  44}, {  66,  39}, { -60,  32}, {  -5,  33}, { -15,  51}, {  21,  53},
    {  28,  48}, {  55,  59}, {  49,  53}, {  37,  57}, {  57,  68}, {  11,  75}, { -47,  76}, {  12,  62},
    {  58,  65}, {  -2,  68}, { -13,  70}, {  11,  69}, { -24,  63}, { -38,  67}, { -25,  76}, { -12,  74},
    {  32,  76}, {  11,  75}, {   6,  79}, {   0,  83}, {  16,  77}, {  35,  80}, {   7,  79}, { -14,  81},
    { -38,  83}, {  10,  88}, { -25,  93}, {  18,  93}, { -14,  80}, {   3,  85}, {  22,  86}, {  28,  85},
    {  -4,  89}, { -21,  87}, { -22,  90}, {   6,  88}, {   9,  95}, { -24,  88}, {  22,  92}, {   8,  98},
  },
  { /* frame 38 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-117,-103}, { -96,-124}, { -91, -80}, { 122, -61},
    {  42, -78}, { -55, -39}, { -27, -52}, {  83, -51}, {  -7, -20}, { -64,  -3}, { -99,  11}, {  13, -17},
    {  74,  10}, {  84,  25}, { -87,  32}, { -47,  41}, {   9,  45}, {  46,  19}, {  30,  43}, {  33,  37},
    { -16,  48}, {  53,  34}, {  10,  49}, {  68,  44}, { -62,  36}, {  -5,  38}, { -15,  56}, {  22,  59},
    {  29,  53}, {  57,  64}, {  51,  58}, {  38,  62}, {  58,  74}, {  12,  81}, { -49,  81}, {  12,  68},
    {  59,  71}, {  -2,  73}, { -13,  76}, {  11,  74}, { -24,  68}, { -39,  72}, { -26,  82}, { -12,  79},
    {  33,  82}, {  11,  81}, {   7,  85}, {  -1,  89}, {  16,  83}, {  36,  86}, {   8,  85}, { -14,  87},
    { -40,  89}, {  10,  94}, { -26,  99}, {  19,  99}, { -14,  86}, {   3,  91}, {  22,  92}, {  29,  91},
    {  -4,  95}, { -21,  94}, { -23,  96}, {   6,  94}, {   9, 101}, { -25,  94}, {  23,  98}, {   8, 104},
  },
  { /* frame 39 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-120,-102}, { -99,-123}, { -94, -79}, { 125, -58},
    {  44, -76}, { -56, -36}, { -28, -49}, {  85, -48}, {  -8, -16}, { -66,   1}, {-102,  16}, {  13, -13},
    {  76,  14}, {  86,  30}, { -89,  37}, { -49,  45}, {   9,  50}, {  47,  24}, {  31,  48}, {  34,  42},
    { -17,  53}, {  54,  39}, {  11,  54}, {  70,  49}, { -63,  41}, {  -5,  43}, { -16,  61}, {  23,  64},
    {  30,  58}, {  58,  70}, {  52,  64}, {  40,  68}, {  60,  80}, {  12,  87}, { -50,  87}, {  13,  73},
    {  61,  77}, {  -2,  79}, { -13,  82}, {  12,  80}, { -25,  74}, { -40,  78}, { -26,  88}, { -12,  85},
    {  33,  88}, {  12,  87}, {   7,  91}, {  -1,  95}, {  17,  89}, {  37,  92}, {   8,  91}, { -14,  93},
    { -41,  95}, {  11, 100}, { -27, 106}, {  19, 105}, { -15,  92}, {   3,  98}, {  23,  98}, {  30,  97},
    {  -4, 101}, { -22, 100}, { -23, 103}, {   6, 100}, {  10, 108}, { -26, 100}, {  24, 105}, {   9, 111},
  },
  { /* frame 40 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-123,-100}, {-101,-122}, { -96, -76}, {-128,-128},
    {  45, -74}, { -57, -33}, { -29, -46}, {  87, -45}, {  -8, -13}, { -67,   5}, {-104,  20}, {  13, -10},
    {  78,  18}, {  88,  35}, { -91,  42}, { -50,  50}, {   9,  55}, {  48,  28}, {  31,  53}, {  35,  47},
    { -17,  58}, {  56,  44}, {  11,  59}, {  71,  54}, { -65,  47}, {  -5,  48}, { -16,  66}, {  23,  70},
    {  30,  64}, {  59,  76}, {  53,  69}, {  41,  74}, {  62,  86}, {  12,  93}, { -51,  93}, {  13,  79},
    {  62,  83}, {  -2,  85}, { -13,  88}, {  12,  86}, { -25,  80}, { -41,  84}, { -27,  95}, { -13,  91},
    {  34,  95}, {  12,  93}, {   7,  97}, {  -1, 101}, {  17,  95}, {  38,  99}, {   8,  97}, { -15, 100},
    { -42, 102}, {  11, 107}, { -27, 113}, {  20, 112}, { -15,  99}, {   3, 104}, {  23, 105}, {  31, 104},
    {  -4, 108}, { -22, 107}, { -24, 109}, {   7, 107}, {  10, 115}, { -26, 107}, {  24, 111}, {   9, 118},
  },
  { /* frame 41 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-127, -99}, {-104,-121}, { -98, -74}, {-128,-128},
    {  46, -72}, { -59, -29}, { -30, -44}, {  89, -42}, {  -8,  -9}, { -69,   9}, {-107,  25}, {  14,  -6},
    {  79,  23}, {  91,  40}, { -93,  47}, { -51,  56}, {  10,  60}, {  50,  33}, {  32,  58}, {  36,  53},
    { -17,  64}, {  57,  49}, {  11,  65}, {  73,  60}, { -66,  52}, {  -5,  53}, { -17,  72}, {  24,  76},
    {  31,  70}, {  61,  82}, {  55,  75}, {  42,  79}, {  63,  92}, {  13,  99}, { -53, 100}, {  13,  85},
    {  64,  89}, {  -1,  92}, { -14,  94}, {  12,  92}, { -26,  86}, { -42,  91}, { -27, 101}, { -13,  98},
    {  35, 101}, {  12, 100}, {   7, 104}, {  -1, 108}, {  17, 102}, {  39, 105}, {   8, 103}, { -15, 106},
    { -43, 108}, {  11, 114}, { -28, 119}, {  20, 119}, { -15, 106}, {   3, 111}, {  24, 111}, {  32, 111},
    {  -4, 115}, { -23, 114}, { -25, 116}, {   7, 114}, {  10, 122}, { -27, 114}, {  25, 118}, {   9, 125},
  },
  { /* frame 42 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-106,-120}, {-101, -72}, {-128,-128},
    {  47, -69}, { -60, -25}, { -30, -40}, {  91, -39}, {  -8,  -5}, { -70,  14}, {-109,  29}, {  14,  -1},
    {  81,  28}, {  93,  45}, { -95,  52}, { -53,  61}, {  10,  66}, {  51,  38}, {  33,  64}, {  37,  58},
    { -18,  69}, {  58,  55}, {  11,  70}, {  75,  66}, { -68,  58}, {  -5,  59}, { -17,  78}, {  25,  82},
    {  32,  76}, {  62,  88}, {  56,  81}, {  43,  85}, {  65,  99}, {  13, 106}, { -54, 106}, {  14,  91},
    {  65,  95}, {  -1,  98}, { -14, 100}, {  12,  99}, { -26,  93}, { -43,  97}, { -28, 108}, { -13, 104},
    {  36, 108}, {  12, 106}, {   7, 110}, {  -1, 115}, {  18, 108}, {  40, 112}, {   9, 110}, { -15, 113},
    { -44, 115}, {  12, 121}, { -29, 126}, {  21, 126}, { -16, 112}, {   3, 118}, {  25, 118}, {  33, 118},
    {  -5, 122}, { -23, 121}, { -25, 123}, {   7, 120}, {-128,-128}, { -27, 121}, {  25, 126}, {-128,-128},
  },
  { /* frame 43 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-109,-119}, {-103, -69}, {-128,-128},
    {  48, -67}, { -61, -22}, { -31, -37}, {  93, -35}, {  -9,   0}, { -72,  18}, {-112,  34}, {  14,   3},
    {  83,  33}, {  95,  50}, { -98,  58}, { -54,  67}, {  10,  72}, {  52,  44}, {  33,  70}, {  38,  64},
    { -18,  75}, {  60,  60}, {  11,  76}, {  77,  72}, { -70,  63}, {  -5,  65}, { -18,  84}, {  25,  88},
    {  32,  82}, {  64,  94}, {  58,  87}, {  44,  92}, {  66, 105}, {  14, 113}, { -55, 113}, {  14,  98},
    {  67, 102}, {  -1, 105}, { -14, 107}, {  13, 106}, { -27,  99}, { -44, 104}, { -28, 115}, { -14, 111},
    {  37, 115}, {  13, 113}, {   7, 117}, {  -1, 122}, {  18, 115}, {  41, 119}, {   9, 117}, { -16, 120},
    { -45, 122}, {-128,-128}, {-128,-128}, {-128,-128}, { -16, 119}, {   3, 126}, {  25, 125}, {  33, 125},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
  },
  { /* frame 44 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-111,-117}, {-105, -66}, {-128,-128},
    {  50, -64}, { -63, -18}, { -32, -34}, {  96, -32}, {  -9,   4}, { -73,  23}, {-115,  39}, {  15,   7},
    {  85,  38}, {  98,  56}, {-100,  64}, { -55,  73}, {  11,  78}, {  53,  49}, {  34,  76}, {  39,  70},
    { -19,  81}, {  61,  66}, {  11,  82}, {  79,  78}, { -71,  69}, {  -5,  71}, { -18,  90}, {  26,  95},
    {  33,  88}, {  65, 101}, {  59,  94}, {  45,  98}, {  68, 112}, {  14, 120}, { -56, 120}, {  14, 105},
    {  68, 109}, {  -1, 112}, { -14, 114}, {  13, 112}, { -27, 106}, { -45, 111}, { -29, 122}, { -14, 118},
    {  37, 122}, {  13, 120}, {   8, 124}, {-128,-128}, {  18, 122}, {  42, 126}, {   9, 124}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
  },
  { /* frame 45 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-114,-115}, {-107, -63}, {-128,-128},
    {  51, -61}, { -64, -13}, { -33, -30}, {  98, -28}, {  -9,   9}, { -75,  28}, {-117,  45}, {  15,  12},
    {  87,  43}, { 100,  62}, {-102,  70}, { -57,  79}, {  11,  84}, {  55,  55}, {  35,  82}, {  40,  76},
    { -19,  88}, {  62,  72}, {  12,  89}, {  81,  84}, { -73,  75}, {  -5,  77}, { -18,  97}, {  27, 102},
    {  34,  95}, {  66, 107}, {  60, 100}, {  46, 105}, {  69, 119}, {-128,-128}, {-128,-128}, {  15, 111},
    {  70, 116}, {  -1, 119}, { -15, 121}, {  13, 120}, { -28, 113}, { -46, 118}, {-128,-128}, { -15, 125},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
  },
  { /* frame 46 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-116,-114}, {-110, -60}, {-128,-128},
    {  52, -58}, { -65,  -9}, { -34, -26}, { 100, -24}, {  -9,  14}, { -76,  33}, {-120,  50}, {  15,  17},
    {  89,  49}, { 102,  68}, {-104,  76}, { -58,  85}, {  11,  90}, {  56,  61}, {  36,  88}, {  41,  82},
    { -19,  94}, {  64,  79}, {  12,  95}, {  82,  91}, { -74,  82}, {  -5,  83}, { -19, 104}, {  28, 109},
    {  34, 102}, {  68, 114}, {  62, 107}, {  47, 112}, {-128,-128}, {-128,-128}, {-128,-128}, {  15, 119},
    {  71, 123}, {-128,-128}, {-128,-128}, {-128,-128}, { -28, 120}, { -47, 125}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
  },
  { /* frame 47 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-118,-111}, {-112, -56}, {-128,-128},
    {  53, -54}, { -66,  -4}, { -34, -22}, { 102, -20}, { -10,  19}, { -78,  39}, {-122,  56}, {  15,  22},
    {  91,  55}, { 104,  74}, {-106,  83}, { -59,  92}, {  11,  97}, {  57,  67}, {  36,  95}, {  42,  89},
    { -20, 101}, {  65,  85}, {  12, 102}, {  84,  97}, { -76,  88}, {  -5,  90}, { -19, 111}, {  28, 116},
    {  35, 109}, {  69, 121}, {  63, 114}, {  48, 119}, {-128,-128}, {-128,-128}, {-128,-128}, {  16, 126},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
  },
  { /* frame 48 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-121,-109}, {-114, -52}, {-128,-128},
    {  54, -51}, { -68,   0}, { -35, -18}, { 104, -15}, { -10,  24}, { -79,  45}, {-125,  62}, {  16,  28},
    {  93,  61}, { 107,  80}, {-109,  89}, { -61,  98}, {  12, 104}, {  58,  73}, {  37, 101}, {  43,  95},
    { -20, 108}, {  66,  92}, {  12, 109}, {  86, 104}, { -77,  95}, {  -5,  97}, { -20, 118}, {  29, 123},
    {  35, 116}, {-128,-128}, {  64, 121}, {  50, 126}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
  },
  { /* frame 49 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-123,-106}, {-117, -48}, {-128,-128},
    {  56, -47}, { -69,   5}, { -36, -13}, { 106, -11}, { -10,  29}, { -81,  51}, {-127,  68}, {  16,  33},
    {  95,  67}, { 109,  87}, {-111,  96}, { -62, 105}, {  12, 111}, {  59,  80}, {  38, 108}, {  43, 102},
    { -21, 115}, {  68,  99}, {  12, 116}, {  88, 112}, { -79, 102}, {  -5, 104}, { -20, 125}, {-128,-128},
    {  36, 123}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
  },
  { /* frame 50 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-126,-104}, {-119, -44}, {-128,-128},
    {  57, -43}, { -70,  11}, { -37,  -9}, { 108,  -6}, { -10,  35}, { -83,  57}, {-128,-128}, {  16,  39},
    {  97,  73}, { 111,  94}, {-113, 103}, { -63, 112}, {  12, 118}, {  61,  87}, {  38, 115}, {  44, 109},
    { -21, 122}, {  69, 106}, {  12, 123}, {  90, 119}, { -81, 109}, {  -5, 111}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
  },
  { /* frame 51 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-121, -40}, {-128,-128},
    {  58, -38}, { -72,  16}, { -38,  -4}, { 110,  -1}, { -11,  41}, { -84,  63}, {-128,-128}, {  17,  45},
    {  99,  80}, { 114, 101}, {-115, 110}, { -65, 119}, {  12, 125}, {  62,  93}, {  39, 123}, {  45, 117},
    {-128,-128}, {  70, 113}, {-128,-128}, {  92, 126}, { -82, 117}, {  -5, 118}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
  },
  { /* frame 52 */
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-124, -35}, {-128,-128},
    {  59, -34}, { -73,  22}, { -38,   1}, { 112,   5}, { -11,  47}, { -86,  69}, {-128,-128}, {  17,  51},
    { 101,  87}, { 116, 108}, {-117, 118}, {-128,-128}, {-128,-128}, {  63, 101}, {-128,-128}, {  46, 124},
    {-128,-128}, {  72, 121}, {-128,-128}, {-128,-128}, { -84, 124}, {  -5, 126}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
    {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128}, {-128,-128},
  },
};
/* ds:0x2778, image 0x06F5B: the colour is indexed by a count running DOWN
 * from eighty, so half of them are the terrain's own colours - which is why
 * counting terrain-coloured pixels could never tell debris from terrain. */
static const uint8_t DEBRIS_INK[16] = {
    0x04, 0x02, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
};

#define BURN_SPR_BASE   17232
#define BURN_SPR_H      14
#define BURN_SPR_PLANES 4
#define BURN_SPR_FRAMES 14

#define BASH_SPR_BASE   6736
#define BASH_SPR_H      10
#define BASH_SPR_STATE  0x20    /* image 0x0352B */
#define MINE_SPR_BASE   10576
#define MINE_SPR_H      13
#define MINE_SPR_STATE  0x18    /* image 0x03604 */

/* THE CLOUD'S SPRITE IS NOT FOUND, and the search says why rather than just
 * failing. Every geometry tried - 32x32 in three planes and in two, 24x32,
 * 20x32 - scores within a few per cent of its own ink, which is the noise
 * floor: nothing matches anywhere.
 *
 * The reason is almost certainly the frame index. Every other sprite here is
 * base + anim * frame, and the cloud's counter is NOT a frame number: image
 * 0x02353 increments it without masking and image 0x02358 runs it to 0x34,
 * fifty-two, where the lemming dies. Fifty-two frames of a 0x280-byte sprite
 * would be 33k, which is more than this section holds. So the drawn frame
 * comes from somewhere else - image 0x02D1F puts 8 in [si+0x0A], which no
 * other skill sets to anything but 0, and that is the thing to read next.
 *
 * Searching for a sprite is only as good as the index you search with. */

#define CLIMB_SPR_RIGHT 2064
#define CLIMB_SPR_LEFT  2448
#define CLIMB_SPR_H     12
#define CLIMB_SPR_PLANES 2
#define CLIMB_SPR_ROW   2
#define CLIMB_SPR_PLANE (CLIMB_SPR_ROW * CLIMB_SPR_H)          /* 24 */
#define CLIMB_SPR_FRAME (CLIMB_SPR_PLANE * CLIMB_SPR_PLANES)   /* 48 */
#define CLIMB_SPR_FRAMES 8

#define HOIST_SPR_BASE  3472
#define HOIST_SPR_H     12
#define HOIST_SPR_PLANES 2
#define HOIST_SPR_ROW   2
#define HOIST_SPR_PLANE (HOIST_SPR_ROW * HOIST_SPR_H)           /* 24 */
#define HOIST_SPR_FRAME (HOIST_SPR_PLANE * HOIST_SPR_PLANES)    /* 48 */
#define HOIST_SPR_FRAMES 8

#define BLOCK_SPR_BASE  18800
#define BLOCK_SPR_H     10
#define BLOCK_SPR_PLANES 2
#define BLOCK_SPR_ROW   2
#define BLOCK_SPR_PLANE (BLOCK_SPR_ROW * BLOCK_SPR_H)          /* 20 */
#define BLOCK_SPR_FRAME (BLOCK_SPR_PLANE * BLOCK_SPR_PLANES)   /* 40 */
#define BLOCK_SPR_FRAMES 16

#define SHRUG_SPR_BASE  19440
#define SHRUG_SPR_H     10
#define SHRUG_SPR_PLANES 2
#define SHRUG_SPR_ROW   2
#define SHRUG_SPR_PLANE (SHRUG_SPR_ROW * SHRUG_SPR_H)            /* 20 */
#define SHRUG_SPR_FRAME (SHRUG_SPR_PLANE * SHRUG_SPR_PLANES)     /* 40 */
#define SHRUG_SPR_FRAMES 8

#define EXIT_SPR_BASE   16816
#define EXIT_SPR_W      16
#define EXIT_SPR_H      13
#define EXIT_SPR_PLANES 2
#define EXIT_SPR_ROW    2
#define EXIT_SPR_PLANE  (EXIT_SPR_ROW * EXIT_SPR_H)             /* 26 */
#define EXIT_SPR_FRAME  (EXIT_SPR_PLANE * EXIT_SPR_PLANES)      /* 52 */
#define EXIT_SPR_FRAMES 8

/* One lemming, at its own coordinates - image 0x070F9 puts the top row at
 * `y + hot_y`, and the canvas sits 16 along and 16 down from those. */
/* THE BOMBER'S COUNTDOWN, five digits of eight rows.
 *
 * Image 0x07147 makes the source offset `(0x40 - (fuse & 0xF0)) * 8 +
 * 0x1070` and image 0x0714A adds the sub-byte shift times sixteen, so a
 * digit is 128 bytes of eight shifts of sixteen and only shift 0 is kept -
 * this port works in whole pixels. The resource is ds:0x1FE8, the same one
 * the crater's and the basher's masks come out of, and these were dumped
 * by tools/verify_results.py rather than read off a screen.
 *
 * It is the fuse's HIGH NIBBLE that is shown, counting 5 4 3 2 1 as the
 * fuse runs 0x4F down to 0 - which is why every lemming wears a 5 for the
 * first sixteen frames and the numbers change together. */
#define FUSE_DIGIT_ROWS 8
static const uint8_t FUSE_DIGIT[5][FUSE_DIGIT_ROWS] = {
    { 0x00, 0x00, 0x00, 0xF0, 0x80, 0xF0, 0x10, 0xF0 },   /* "5" */
    { 0x00, 0x00, 0x00, 0x90, 0x90, 0xF0, 0x10, 0x10 },   /* "4" */
    { 0x00, 0x00, 0x00, 0xF0, 0x10, 0x70, 0x10, 0xF0 },   /* "3" */
    { 0x00, 0x00, 0x00, 0xF0, 0x10, 0xF0, 0x80, 0xF0 },   /* "2" */
    { 0x00, 0x00, 0x00, 0x60, 0x20, 0x20, 0x20, 0x70 },   /* "1" */
};

/* Image 0x070F3 loads the fuse into the BLIT ITSELF - it is not a separate
 * call - and image 0x0711A skips the digit when it is zero. Image 0x0714E
 * takes 0x15F off the destination, which for a 44-byte row is eight rows up
 * and one byte along: measured against the original's own frame 430, the
 * digit sits at (+8, -8) from the sprite's origin in colour 3, and all ten
 * of its ink pixels agree on all three lemmings that wear one. */
#define FUSE_DIGIT_DX     8
#define FUSE_DIGIT_DY   (-8)
#define FUSE_DIGIT_INK    3

/* Image 0x070F3 loads the fuse and image 0x0714E places the digit. */
static void lem_draw_fuse(uint8_t *dst, uint16_t dst_w, uint16_t dst_h,
                          const lemming *a)
{
    if (!a->fuse) return;                           /* image 0x07118 */
    uint8_t slot = (uint8_t)(4 - (a->fuse >> 4));   /* image 0x0713B */
    if (slot > 4) return;
    int32_t x0 = (int32_t)a->x + a->hot_x - LEMMING_DX + FUSE_DIGIT_DX;
    int32_t y0 = (int32_t)a->y + a->hot_y - LEMMING_DY + FUSE_DIGIT_DY;
    for (uint8_t r = 0; r < FUSE_DIGIT_ROWS; r++) {
        int32_t py = y0 + r;
        if (py < 0 || py >= (int32_t)dst_h) continue;
        uint8_t bits = FUSE_DIGIT[slot][r];
        for (uint8_t c = 0; c < 8; c++) {
            if (!((bits >> (7 - c)) & 1u)) continue;
            int32_t px = x0 + c;
            if (px < 0 || px >= (int32_t)dst_w) continue;
            dst[(size_t)py * dst_w + px] = FUSE_DIGIT_INK;
        }
    }
}

/* Image 0x070A2 onwards - the blit that draws one lemming, which the port
 * splits by state because it indexes MAIN.DAT rather than the pre-shifted
 * store the original blits from. */
void lemming_draw(uint8_t *dst, uint16_t dst_w, uint16_t dst_h, uint16_t view_x,
                  const uint8_t *sec, uint32_t sec_len, const lemming *a)
{
    uint16_t frame = a->anim;
    uint32_t base;
    int32_t x0, y0;

    if (a->alive == 0xFF) return;

    /* IS THIS LEMMING ON SCREEN AT ALL - image 0x07070, and it is a table
     * lookup rather than a comparison.
     *
     * The drawer takes `x + hot_x`, wraps a negative one by the page's 0x160,
     * takes its BYTE column and refuses the whole sprite unless
     * ds:0x0EA8[column] holds 1 (image 0x07086). Image 0x04113 clears that
     * table and image 0x0412B marks exactly 41 bytes of it starting two byte
     * columns left of the scroll, so the window is
     * (scroll >> 3) - 2 .. (scroll >> 3) + 38.
     *
     * Because it is keyed on the BYTE column, a sprite is refused the moment
     * its left edge crosses into the first unmarked byte - not when its
     * pixels leave the screen. That is why level002/4 showed the original
     * drawing a walker at screen x0 311 and none of it at 312: 312 is a byte
     * boundary, and the column it lands in is one past the last marked one.
     *
     * Two rules were guessed at before this was read - the blitter's own
     * right-edge trim, and refusing the last column outright - and both made
     * that level WORSE, 1,071 frames exact down to 875 and 1,015. The
     * blitter's limit is innocent: measured from a running level it is 352,
     * the whole page. */
    {
        int32_t col = (int32_t)a->x + a->hot_x;   /* image 0x07070 */
        if (col < 0) col += 0x160;                /* image 0x07077 */
        col >>= 3;                                /* image 0x0707D */
        int32_t first = (((int32_t)view_x + 16) >> 3) - 2;  /* image 0x04125 */
        if (col < first || col >= first + 41)     /* image 0x0412B: 0x29 */
            return;                               /* image 0x0708B */
    }

    /* Before the sprite, because the sprite's own branches all return. */
    lem_draw_fuse(dst, dst_w, dst_h, a);

    /* One leaving through the exit, image 0x02326's eight frames. */
    if (a->flags & 0x2000) {
        if (a->anim >= EXIT_SPR_FRAMES) return;
        base = EXIT_SPR_BASE + (uint32_t)a->anim * EXIT_SPR_FRAME;
        if (base + EXIT_SPR_FRAME > sec_len) return;
        x0 = (int32_t)a->x + a->hot_x - LEMMING_DX;
        y0 = (int32_t)a->y + a->hot_y - LEMMING_DY;
        for (uint8_t r = 0; r < EXIT_SPR_H; r++) {
            int32_t py = y0 + r;
            if (py < 0 || py >= (int32_t)dst_h) continue;
            for (uint8_t c = 0; c < EXIT_SPR_W; c++) {
                int32_t px = x0 + c;
                uint8_t v = 0;
                if (px < 0 || px >= (int32_t)dst_w) continue;
                for (uint8_t p = 0; p < EXIT_SPR_PLANES; p++) {
                    uint8_t byte = sec[base + p * EXIT_SPR_PLANE
                                       + r * EXIT_SPR_ROW + (c >> 3)];
                    v = (uint8_t)(v | (((byte >> (7 - (c & 7))) & 1u) << p));
                }
                if (v) dst[(size_t)py * dst_w + px] = v;
            }
        }
        return;
    }

    /* The states whose sprites are 16 wide and differ only in size and
     * where they live. Image 0x01F5E's order decides which is asked first;
     * these follow it. */
    {
        uint32_t sb = 0; uint8_t sh = 0, sp = 0, sf = 0;
        /* THE CLOUD IS ASKED FIRST. Image 0x01F5E's chain tests 0x0002
         * before 0x8000 and so must this: a lemming in the cloud state has
         * BOTH bits, and asking 0x8000 first drew the blast's pose - a
         * standing lemming with its arms out - at the cloud's hot spot of
         * (-16, -25), fifteen rows above where it had been standing, so
         * every nuked lemming appeared to leap into the air and go off a
         * second time. A player reported exactly that.
         *
         * ONE FRAME ONLY, and that is measured: scanning the original's
         * frames 492..528 for dense runs of colours 3, 4 and 5 finds exactly
         * one burst on screen at a time - 496, 498, 500, 502, 504 - and the
         * lemming beside it is at anim 0 every time. The store agrees: it
         * holds one picture and nothing at the indices anim 1 upward would
         * reach. */
        if (a->flags & 0x0002) {                    /* image 0x02350 */
            /* AND THE FRAMES AFTER IT ARE THE DEBRIS. Image 0x03662 asks
             * [si+0x28] before drawing anything and, from 1 upward, calls
             * image 0x06E83 INSTEAD of the sprite - so the cloud shows for
             * one frame and the next fifty-one are eighty flying pixels and
             * no lemming at all. */
            if (a->anim >= 1 && a->anim <= DEBRIS_ROWS) {
                const int8_t (*row)[2] = DEBRIS_AT[a->anim - 1];
                for (uint8_t i = 0; i < DEBRIS_N; i++) {
                    int16_t ox = row[i][0];
                    if (ox == DEBRIS_GONE) continue;   /* image 0x06EDB */
                    int32_t px = (int32_t)a->x - LEMMING_DX + ox;
                    int32_t py = (int32_t)a->y - LEMMING_DY + row[i][1];
                    /* Image 0x06EF2 clips these against the SCREEN, not the
                     * level: bp is the page x and runs 0 .. 0x150, which is
                     * the visible 320 with the page's 16-pixel left margin
                     * in front of it. A particle is a pixel and not a blit,
                     * so it is not byte-trimmed the way a sprite is. */
                    int32_t sx = px - (int32_t)view_x;
                    if (sx < -16 || sx >= LEVEL_VIEW_W) continue;
                    if (px < 0 || px >= (int32_t)dst_w) continue;
                    if (py < 0 || py >= (int32_t)dst_h) continue;
                    /* image 0x06F5B: the count runs DOWN from eighty. */
                    dst[(size_t)py * dst_w + px] =
                        DEBRIS_INK[(DEBRIS_N - i) & 0x0F];
                }
                return;
            }
            if (a->anim == 0) {
                /* THE HOT SPOT, not an offset of it - see the note on
                 * CLOUD_SPR. Image 0x02D3C and image 0x02D37 set (-16, -25)
                 * for this one sprite and this is what they are for. */
                int32_t cx0 = (int32_t)a->x + a->hot_x - LEMMING_DX;
                int32_t cy0 = (int32_t)a->y + a->hot_y - LEMMING_DY;
                for (uint8_t r = 0; r < CLOUD_SPR_H; r++) {
                    int32_t py = cy0 + r;
                    if (py < 0 || py >= (int32_t)dst_h) continue;
                    for (uint8_t c = 0; c < CLOUD_SPR_W; c++) {
                        uint8_t v = CLOUD_SPR[r][c];
                        if (!v) continue;
                        int32_t px = cx0 + c;
                        if (px < 0 || px >= (int32_t)dst_w) continue;
                        dst[(size_t)py * dst_w + px] = v;
                    }
                }
            }
            return;
        }
        /* The fire death - image 0x02340's fourteen frames. Four planes,
         * which is why it does not share the three-plane arithmetic the
         * others here use. */
        if (a->flags & 0x4000) { sb = BURN_SPR_BASE; sh = BURN_SPR_H;
                                 sp = BURN_SPR_PLANES; sf = BURN_SPR_FRAMES; }
        else if (a->flags & 0x1000) { sb = DROWN_SPR_BASE; sh = DROWN_SPR_H;
                                      sp = 2; sf = DROWN_SPR_FRAMES; }
        else if (a->flags & 0x0400) { sb = FLOAT_SPR_BASE; sh = 16; sp = 3; sf = 8; }
        else if (a->flags & 0x0001) { sb = SPLAT_SPR_BASE; sh = 10; sp = 2; sf = 16; }
        else if (a->flags & 0x8000) { sb = BLAST_SPR_BASE; sh = 10; sp = 2; sf = 16; }
        else if (a->flags & 0x0004) { sb = FALL_SPR_BASE; sh = 10; sp = 2; sf = 4; }
        if (sb) {
            if (a->anim >= sf) return;
            uint32_t plane = 2u * sh;
            uint32_t fsz = plane * sp;
            base = sb + (uint32_t)a->anim * fsz;
            if (base + fsz > sec_len) return;
            x0 = (int32_t)a->x + a->hot_x - LEMMING_DX;
            y0 = (int32_t)a->y + a->hot_y - LEMMING_DY;
            for (uint8_t r = 0; r < sh; r++) {
                int32_t py = y0 + r;
                if (py < 0 || py >= (int32_t)dst_h) continue;
                for (uint8_t c = 0; c < 16; c++) {
                    int32_t px = x0 + c;
                    uint8_t v = 0;
                    if (px < 0 || px >= (int32_t)dst_w) continue;
                    for (uint8_t p = 0; p < sp; p++) {
                        uint8_t byte = sec[base + p * plane + r * 2 + (c >> 3)];
                        v = (uint8_t)(v | (((byte >> (7 - (c & 7))) & 1u) << p));
                    }
                    if (v) dst[(size_t)py * dst_w + px] = v;
                }
            }
            return;
        }
    }

    /* A climber going up a wall - and which way it faces picks the sprite. */
    if (a->flags & 0x0020) {
        if (a->anim >= CLIMB_SPR_FRAMES) return;
        base = (a->dir > 0 ? CLIMB_SPR_RIGHT : CLIMB_SPR_LEFT)
             + (uint32_t)a->anim * CLIMB_SPR_FRAME;
        if (base + CLIMB_SPR_FRAME > sec_len) return;
        x0 = (int32_t)a->x + a->hot_x - LEMMING_DX;
        y0 = (int32_t)a->y + a->hot_y - LEMMING_DY;
        for (uint8_t r = 0; r < CLIMB_SPR_H; r++) {
            int32_t py = y0 + r;
            if (py < 0 || py >= (int32_t)dst_h) continue;
            for (uint8_t c = 0; c < 16; c++) {
                int32_t px = x0 + c;
                uint8_t v = 0;
                if (px < 0 || px >= (int32_t)dst_w) continue;
                for (uint8_t p = 0; p < CLIMB_SPR_PLANES; p++) {
                    uint8_t byte = sec[base + p * CLIMB_SPR_PLANE
                                       + r * CLIMB_SPR_ROW + (c >> 3)];
                    v = (uint8_t)(v | (((byte >> (7 - (c & 7))) & 1u) << p));
                }
                if (v) dst[(size_t)py * dst_w + px] = v;
            }
        }
        return;
    }

    /* A climber hoisting itself over the top - see HOIST_SPR_BASE. */
    if (a->flags & 0x0040) {
        if (a->anim >= HOIST_SPR_FRAMES) return;
        base = HOIST_SPR_BASE + (uint32_t)a->anim * HOIST_SPR_FRAME;
        if (base + HOIST_SPR_FRAME > sec_len) return;
        x0 = (int32_t)a->x + a->hot_x - LEMMING_DX;
        y0 = (int32_t)a->y + a->hot_y - LEMMING_DY;
        for (uint8_t r = 0; r < HOIST_SPR_H; r++) {
            int32_t py = y0 + r;
            if (py < 0 || py >= (int32_t)dst_h) continue;
            for (uint8_t c = 0; c < 16; c++) {
                int32_t px = x0 + c;
                uint8_t v = 0;
                if (px < 0 || px >= (int32_t)dst_w) continue;
                for (uint8_t p = 0; p < HOIST_SPR_PLANES; p++) {
                    uint8_t byte = sec[base + p * HOIST_SPR_PLANE
                                       + r * HOIST_SPR_ROW + (c >> 3)];
                    v = (uint8_t)(v | (((byte >> (7 - (c & 7))) & 1u) << p));
                }
                if (v) dst[(size_t)py * dst_w + px] = v;
            }
        }
        return;
    }

    /* A blocker - see BLOCK_SPR_BASE. */
    if (a->flags & 0x0100) {
        if (a->anim >= BLOCK_SPR_FRAMES) return;
        base = BLOCK_SPR_BASE + (uint32_t)a->anim * BLOCK_SPR_FRAME;
        if (base + BLOCK_SPR_FRAME > sec_len) return;
        x0 = (int32_t)a->x + a->hot_x - LEMMING_DX;
        y0 = (int32_t)a->y + a->hot_y - LEMMING_DY;
        for (uint8_t r = 0; r < BLOCK_SPR_H; r++) {
            int32_t py = y0 + r;
            if (py < 0 || py >= (int32_t)dst_h) continue;
            for (uint8_t c = 0; c < 16; c++) {
                int32_t px = x0 + c;
                uint8_t v = 0;
                if (px < 0 || px >= (int32_t)dst_w) continue;
                for (uint8_t p = 0; p < BLOCK_SPR_PLANES; p++) {
                    uint8_t byte = sec[base + p * BLOCK_SPR_PLANE
                                       + r * BLOCK_SPR_ROW + (c >> 3)];
                    v = (uint8_t)(v | (((byte >> (7 - (c & 7))) & 1u) << p));
                }
                if (v) dst[(size_t)py * dst_w + px] = v;
            }
        }
        return;
    }

    /* A builder out of bricks shrugs, and image 0x01F54 dispatches that from
     * the loop head - so it is the byte at +0x24 that picks the sprite here,
     * not the flag word, which reads zero throughout. */
    if (a->extra & 0x80) {
        if (a->anim >= SHRUG_SPR_FRAMES) return;
        /* AND IT FACES BOTH WAYS, like the builder it comes from. Image
         * 0x070AB adds the STATE to the frame index for a lemming facing
         * left, and image 0x027CC sets [si+0x29] to 8 as the builder gives
         * up - which is the 8 lem_build_stop is called with here.
         *
         * Nobody reported this one. It was found by TILING the sprite file
         * from the port's own constants: every region abuts its neighbour,
         * and the shrug's eight frames of 40 left exactly 320 bytes spare
         * before the blast's base at 20080. 320 is eight more frames, and
         * 19440 + 8 * 40 = 19760 is where they are. */
        uint16_t sfr = a->anim;
        if (a->dir != 1)                                  /* image 0x070AB */
            sfr = (uint16_t)(sfr + a->state);
        base = SHRUG_SPR_BASE + (uint32_t)sfr * SHRUG_SPR_FRAME;
        if (base + SHRUG_SPR_FRAME > sec_len) return;
        x0 = (int32_t)a->x + a->hot_x - LEMMING_DX;
        y0 = (int32_t)a->y + a->hot_y - LEMMING_DY;
        for (uint8_t r = 0; r < SHRUG_SPR_H; r++) {
            int32_t py = y0 + r;
            if (py < 0 || py >= (int32_t)dst_h) continue;
            for (uint8_t c = 0; c < 16; c++) {
                int32_t px = x0 + c;
                uint8_t v = 0;
                if (px < 0 || px >= (int32_t)dst_w) continue;
                for (uint8_t p = 0; p < SHRUG_SPR_PLANES; p++) {
                    uint8_t byte = sec[base + p * SHRUG_SPR_PLANE
                                       + r * SHRUG_SPR_ROW + (c >> 3)];
                    v = (uint8_t)(v | (((byte >> (7 - (c & 7))) & 1u) << p));
                }
                if (v) dst[(size_t)py * dst_w + px] = v;
            }
        }
        return;
    }

    /* A builder has its own too - see BUILD_SPR_BASE. Its counter runs to
     * sixteen, image 0x02698's `and al, 0x0F`.
     *
     * AND IT FACES BOTH WAYS. This drew the right-facing sixteen whichever
     * direction the builder was laying bricks, so one built towards the left
     * with the wrong animation - a player reported it, and nothing here could
     * have: every record comparison scores x, y, the flags and the counter,
     * and a sprite index is none of those.
     *
     * The rule is the one the basher and the miner two branches down already
     * carry: image 0x070AB adds the STATE to the frame index when the lemming
     * faces left, and the builder's state is 0x10 (image 0x0330A, this file's
     * BUILD_STATE). So sixteen frames facing right, then sixteen facing left.
     *
     * The region says the same thing without being asked, which is what makes
     * it evidence rather than a guess: 4240 + 32 * 78 = 6736, and 6736 is
     * BASH_SPR_BASE. Thirty-two frames fit exactly between the builder's
     * start and the basher's, and sixteen would leave 1,248 bytes of nothing
     * in a file where every other sprite region abuts its neighbour. */
    if (a->flags & 0x0080) {
        if (a->anim >= BUILD_SPR_FRAMES) return;
        uint16_t bfr = a->anim;
        if (a->dir != 1)                                  /* image 0x070AB */
            bfr = (uint16_t)(bfr + BUILD_SPR_STATE);
        base = BUILD_SPR_BASE + (uint32_t)bfr * BUILD_SPR_FRAME;
        if (base + BUILD_SPR_FRAME > sec_len) return;
        x0 = (int32_t)a->x + a->hot_x - LEMMING_DX;
        y0 = (int32_t)a->y + a->hot_y - LEMMING_DY;
        for (uint8_t r = 0; r < BUILD_SPR_H; r++) {
            int32_t py = y0 + r;
            if (py < 0 || py >= (int32_t)dst_h) continue;
            for (uint8_t c = 0; c < BUILD_SPR_W; c++) {
                int32_t px = x0 + c;
                uint8_t v = 0;
                if (px < 0 || px >= (int32_t)dst_w) continue;
                for (uint8_t p = 0; p < BUILD_SPR_PLANES; p++) {
                    uint8_t byte = sec[base + p * BUILD_SPR_PLANE
                                       + r * BUILD_SPR_ROW + (c >> 3)];
                    v = (uint8_t)(v | (((byte >> (7 - (c & 7))) & 1u) << p));
                }
                if (v) dst[(size_t)py * dst_w + px] = v;
            }
        }
        return;
    }

    /* A digger has its own sprite, its own size and its own place in the
     * file - see DIG_SPR_BASE. */
    if (a->flags & 0x0010) {
        if (a->anim >= DIG_SPR_FRAMES) return;
        base = DIG_SPR_BASE + (uint32_t)a->anim * DIG_SPR_FRAME;
        if (base + DIG_SPR_FRAME > sec_len) return;
        x0 = (int32_t)a->x + a->hot_x - LEMMING_DX;
        y0 = (int32_t)a->y + a->hot_y - LEMMING_DY;
        for (uint8_t r = 0; r < DIG_SPR_H; r++) {
            int32_t py = y0 + r;
            if (py < 0 || py >= (int32_t)dst_h) continue;
            for (uint8_t c = 0; c < DIG_SPR_W; c++) {
                int32_t px = x0 + c;
                uint8_t v = 0;
                if (px < 0 || px >= (int32_t)dst_w) continue;
                for (uint8_t p = 0; p < DIG_SPR_PLANES; p++) {
                    uint8_t byte = sec[base + p * DIG_SPR_PLANE
                                       + r * DIG_SPR_ROW + (c >> 3)];
                    v = (uint8_t)(v | (((byte >> (7 - (c & 7))) & 1u) << p));
                }
                if (v) dst[(size_t)py * dst_w + px] = v;
            }
        }
        return;
    }
    /* THE BASHER AND THE MINER, whose sprites the port did not have at all.
     *
     * Both fell through to the walker's group, so a basher and a miner were
     * drawn walking on the spot while their records were exact - the record
     * comparison scores x, y, the flags and the animation counter and never
     * a pixel, and both skills sat at 0 of 400 frames for a day with the
     * wrong picture on screen. A player reported both.
     *
     * Found with tools/find_sprite.py against the original's own frames:
     *
     *   basher  ref_s7 frame 113 is anim 2 and matches at 6856,
     *           frame 115 is anim 4 and matches at 6976;
     *           6856 - 2 * 60 == 6976 - 4 * 60 == 6736.
     *           26 of 26 and 29 of 31 pixels agree, the two under the cursor.
     *   miner   ref_s8 frame 130 is anim 19 and matches at 12058,
     *           frame 150 is anim 15 and matches at 11746;
     *           12058 - 19 * 78 == 11746 - 15 * 78 == 10576.
     *           30 of 31 and 32 of 34 agree.
     *
     * And the three regions ABUT, which neither search knew: the basher's
     * 64 frames of 60 end at 10576 where the miner starts, and the miner's
     * 48 of 78 end at 14320, which is FALL_SPR_BASE - a number that was
     * already in this file for a different reason. That is what makes the
     * frame COUNTS evidence too, and not only the bases.
     *
     * 64 and 48 because image 0x070AB adds the STATE to the frame index when
     * the lemming faces left: the basher's state is 0x20 and the miner's
     * 0x18, so each has that many right-facing frames followed by the same
     * number facing left. */
    if (a->flags & 0x0200 || a->flags & 0x0800) {
        uint8_t bash = (a->flags & 0x0200) != 0;
        uint32_t sb   = bash ? BASH_SPR_BASE  : MINE_SPR_BASE;
        uint8_t  sh   = bash ? BASH_SPR_H     : MINE_SPR_H;
        uint8_t  sadd = bash ? BASH_SPR_STATE : MINE_SPR_STATE;
        uint32_t fsz  = (uint32_t)2 * sh * 3;         /* 16 wide, 3 planes */
        uint16_t fr   = a->anim;
        if (fr >= sadd) return;                       /* image 0x02880's mask */
        if (a->dir != 1) fr = (uint16_t)(fr + sadd);  /* image 0x070AB */
        base = sb + (uint32_t)fr * fsz;
        if (base + fsz > sec_len) return;
        x0 = (int32_t)a->x + a->hot_x - LEMMING_DX;
        y0 = (int32_t)a->y + a->hot_y - LEMMING_DY;
        for (uint8_t r = 0; r < sh; r++) {
            int32_t py = y0 + r;
            if (py < 0 || py >= (int32_t)dst_h) continue;
            for (uint8_t c = 0; c < 16; c++) {
                int32_t px = x0 + c;
                uint8_t v = 0;
                if (px < 0 || px >= (int32_t)dst_w) continue;
                for (uint8_t p = 0; p < 3; p++) {
                    uint8_t byte = sec[base + p * 2u * sh + r * 2 + (c >> 3)];
                    v = (uint8_t)(v | (((byte >> (7 - (c & 7))) & 1u) << p));
                }
                if (v) dst[(size_t)py * dst_w + px] = v;
            }
        }
        return;
    }

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
    /* PAUSE STOPS THE OBJECTS TOO - image 0x03944, which is the only thing
     * that reaches image 0x07C0A: `cmp byte [0x2b], 0xff / je / jmp 0x7c0a`,
     * so the advance happens only while the pause flag is clear. ds:0x2B is
     * that flag, toggled by image 0x04018's `xor byte [0x2b], 0xff`, and the
     * clock (image 0x0020C) and the release (image 0x030A6) already guard on
     * it here.
     *
     * The port did not, and the two never came back into step: with the
     * original's objects frozen from the moment the button went down and the
     * port's still turning, a trap's animation was permanently out of phase
     * afterwards. It shows as about twenty pixels on EVERY frame from the
     * press to the end of the run - 285 of 500 on the pause case - and it
     * needed a run that pauses to see at all. No frame-for-frame comparison
     * of a level nobody touches could. */
    if (level_paused() == 0xFF) return;             /* image 0x03944 */
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
    (void)st; (void)lv;
    level_timer_step();
    level_status_out(level_released());
    /* Image 0x03ACE: the saved count over the total, as a percentage. */
    level_status_in(level_saved_percent(lv));
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

/* And its MASK, image 0x0585A's `bx = si + 0x60`. For the cross-hair it
 * is exactly "the ink is not zero", so skipping zeros would do - but the
 * BOX's is not, and carrying both means this does what image 0x07218
 * does rather than what happens to look the same. */
static const uint8_t CURSOR_MASK[CURSOR_H][CURSOR_W] = {
    { 0,0,0,0,0,0,1,1,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,1,1,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,1,1,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 1,0,1,0,1,0,1,1,0,1,0,1,0,1 },
    { 1,0,1,0,1,0,1,1,0,1,0,1,0,1 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,1,1,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,1,1,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,1,1,0,0,0,0,0,0 },
};

/* THE BOX THE CURSOR BECOMES OVER A LEMMING - READ from the original's own
 * sprite store, not measured off a frame.
 *
 * Image 0x05827 works out the shift from the low three bits of ds:0x10 and
 * indexes the store at 144 bytes each; image 0x05854 adds 0x480 - eight
 * shifts further on - when ds:0x1A is 0xFF, and image 0x03DE8 sets that flag
 * when the scan finds a lemming under the pointer. 144 bytes is 0x60 of ink
 * and 0x30 of mask, and 0x30 is sixteen rows of three bytes: a sixteen-wide
 * sprite pre-shifted into three, so the ink is those same 16x3 in two planes
 * and the whole thing is 16x16 in four colours with a one-pixel border.
 *
 * tools/probe_cursor.py dumps it out of segment ds:0x1FEA and decodes it.
 * Both tables below are the inner 14x14. The cross-hair had been measured off
 * one captured frame and turns out to be right in all 196 cells - which is
 * luck worth naming rather than a method to repeat.
 *
 * IT IS THE OTHER SPRITE, NOT AN EXTRA ONE. The store's box has no cross-hair
 * inside it, and image 0x05854 REPLACES the sprite's address rather than
 * drawing a second pass. The port used to draw the box and then the
 * cross-hair on top of it, which is not what the original shows and is the
 * thing that looked, playing it, like the cursor never switching. */
#define CURSOR_BOX_H  14
static const uint8_t CURSOR_BOX[CURSOR_BOX_H][CURSOR_W] = {
    { 2,2,3,3,0,0,1,1,0,0,3,3,2,2 },
    { 2,0,0,0,0,0,0,0,0,0,0,0,0,2 },
    { 3,0,0,0,0,0,0,0,0,0,0,0,0,3 },
    { 3,0,0,0,0,0,0,0,0,0,0,0,0,3 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 1,0,0,0,0,0,0,0,0,0,0,0,0,1 },
    { 1,0,0,0,0,0,0,0,0,0,0,0,0,1 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 3,0,0,0,0,0,0,0,0,0,0,0,0,3 },
    { 3,0,0,0,0,0,0,0,0,0,0,0,0,3 },
    { 2,0,0,0,0,0,0,0,0,0,0,0,0,2 },
    { 2,2,3,3,0,0,1,1,0,0,3,3,2,2 },
};

/* THE MASK IS NOT "THE INK IS NOT ZERO" HERE, and that is the whole
 * reason it is carried.
 *
 * Store row 13 holds ink 2 at both ends and its mask is CLEAR, so the
 * original does not draw those two pixels - the ink there is data the
 * mask suppresses. Drawing them cost exactly two pixels a frame, at
 * (146, 81) and (159, 81), and there is no rule anyone would have
 * guessed that produces an outline missing one side row and not the
 * other. Reading the mask out of the store settles it; fitting could
 * not have. */
static const uint8_t CURSOR_BOX_MASK[CURSOR_BOX_H][CURSOR_W] = {
    { 1,1,1,1,0,0,1,1,0,0,1,1,1,1 },
    { 1,0,0,0,0,0,0,0,0,0,0,0,0,1 },
    { 1,0,0,0,0,0,0,0,0,0,0,0,0,1 },
    { 1,0,0,0,0,0,0,0,0,0,0,0,0,1 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 1,0,0,0,0,0,0,0,0,0,0,0,0,1 },
    { 1,0,0,0,0,0,0,0,0,0,0,0,0,1 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 1,0,0,0,0,0,0,0,0,0,0,0,0,1 },
    { 1,0,0,0,0,0,0,0,0,0,0,0,0,1 },
    { 0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
    { 1,1,1,1,0,0,1,1,0,0,1,1,1,1 },
};

/* Where the pointer is, in the ORIGINAL's coordinates - ds:0x10 and ds:0x12,
 * so eight more than the screen position and clamped the way image 0x0426D
 * clamps them. Image 0x00F41 starts it at (150, 100), which is where the
 * game puts it before anyone touches the mouse. */
uint16_t lem_mouse_x = 150;
uint16_t lem_mouse_y = 100;
/* ds:0x1A - is the pointer over a lemming? Image 0x03DE8 sets it, image
 * 0x03DAF clears it, and image 0x05854 is what it changes. */
uint8_t lem_cursor_box;

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

/* THE CURSOR - transcribed from image 0x05827, and its POSITION from image
 * 0x0715F.
 *
 * Where it goes. Image 0x0715F computes the blit's destination as
 *
 *     ds:0x0C = ds:0x12 * 0x2C + ((ds:0x1FE0 + ds:0x10) >> 3) + ds:0x1F78
 *
 * - a video offset in a mode whose logical line is 0x2C bytes - so the
 * sprite's top-left sits at (ds:0x10, ds:0x12) in the LINE's own
 * coordinates, and the sub-byte part of ds:0x10 is the shift image 0x0583B
 * picks a pre-shifted copy by. The store's shape carries a one-pixel border
 * and the screen is the line offset by the sixteen this port applies
 * everywhere, so the visible content lands at (mouse - 15) on both axes -
 * which is CURSOR_ORIGIN plus CURSOR_HOT_X below, and was measured before it
 * was read.
 *
 * Which shape. Image 0x05854 adds 0x480 to the store offset when ds:0x1A is
 * set, replacing the address rather than adding a pass - see CURSOR_BOX.
 *
 * WHAT IS STILL THE PORT'S OWN is the route: image 0x07218 composes through
 * the EGA's write logic, masked through GC register 8, and this writes
 * indices into a byte buffer, which is what the rest of the port does for
 * every sprite.
 *
 * And where the UNSHIFTED sprite lives is still not known. The store at
 * ds:0x1FEA is built by the pre-shifter at image 0x09347, not loaded, and
 * its bytes appear in no .dat - searched. The tables above are read out of
 * that store rather than off a screen, which is why they carry their masks. */
void level_cursor_draw(uint8_t *pix, uint16_t w, uint16_t h, uint16_t view_x)
{
    int32_t sx = (int32_t)lem_mouse_x - CURSOR_ORIGIN - CURSOR_HOT_X;
    int32_t sy = (int32_t)lem_mouse_y - CURSOR_ORIGIN - CURSOR_HOT_Y;
    (void)view_x;                 /* the cursor is on the SCREEN, not the level */

    /* Image 0x05854: the SAME blit, from a different address. One shape or
     * the other, never both. */
    const uint8_t (*shape)[CURSOR_W] = lem_cursor_box ? CURSOR_BOX : CURSOR;
    const uint8_t (*mask)[CURSOR_W] =
        lem_cursor_box ? CURSOR_BOX_MASK : CURSOR_MASK;

    for (uint16_t r = 0; r < CURSOR_H; r++) {
        int32_t y = sy + r;
        if (y < 0 || y >= (int32_t)h) continue;
        for (uint16_t c = 0; c < CURSOR_W; c++) {
            int32_t x = sx + c;
            /* Image 0x07218 writes where the MASK says, not where the ink
             * is non-zero. See CURSOR_BOX_MASK for where those differ. */
            if (!mask[r][c] || x < 0 || x >= (int32_t)w) continue;
            pix[(size_t)y * w + (size_t)x] = shape[r][c];
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
    level_tick();                       /* image 0x00199 */
    level_frame_step(st, lv);
    if (level_released() != *released) {
        lemmings_release(lem, *released, lv);
        *released = level_released();
    }
    lemmings_step(lem, *released, terrain, w, h);
    /* THE OBJECTS ADVANCE AFTER THE LEMMINGS, because in the original they
     * advance inside the DRAW - image 0x07C1E - and image 0x0019D calls the
     * lemming walk at 0x1F19 before the drawing at 0x3944. A trap triggered
     * during the walk therefore shows its second frame on the very frame it
     * fires; stepping the objects first showed its first, one frame late.
     * Measured: the port's trap region matched the original's shifted by
     * exactly +1 on 19 of 33 frames and by nothing at any other shift. */
    level_anim_step(st, lv);                    /* image 0x07C1E */
}

/* And the drawing half: the objects at image 0x07BB8, which image 0x01B9
 * calls, and then the lemmings at image 0x01BC - which is why they go on
 * LAST, over both the terrain and the objects. */
void level_play_draw(level_anim *st, const level *lv,
                     const uint8_t *obj_sec, uint32_t obj_len,
                     const uint8_t *lem_sec, uint32_t lem_len,
                     const lemming *lem, uint8_t released,
                     uint8_t *pix, uint16_t w, uint16_t h, uint16_t view_x)
{
    level_anim_draw(st, lv, obj_sec, obj_len, pix, w, h, 0);
    if (!lem_sec) return;
    /* THE 'OUT' COUNT IS THE DRAW'S, not the step's. Image 0x03645's loop
     * skips a lemming whose [di+0x26] is 0xFF and counts the rest into
     * ds:0x45 as it draws them, and image 0x03684 hands that count straight
     * to the three-digit routine with si = ds:0x2095 - the status line's
     * column 22.
     *
     * The port had it at the tail of level_play_step, which agreed on every
     * frame but the FIRST: frame 0 is the level before anything moves, so it
     * draws without stepping, and the original's own first frame already has
     * both counters while the port's had neither. 100 pixels of panel, on
     * one frame of 1,100, and the fix is a routine sitting in the wrong half
     * of the loop rather than anything about what it computes.
     *
     * Same shape as the object animation two functions up - image 0x07C1E is
     * part of the draw as well, and moving it there is what fixed
     * level002/6. */
    uint8_t out = 0;
    for (uint8_t k = 0; k < released && k < LEMMING_MAX; k++) {
        if (lem[k].alive == 0xFF) continue;     /* image 0x03647 */
        out++;                                  /* image 0x0364D */
        lemming_draw(pix, w, h, view_x, lem_sec, lem_len, &lem[k]);
    }
    level_status_out(out);                      /* image 0x03684 */
}

/* THE CONTROL PANEL, one frame of it.
 *
 * The port's own arrangement, and now for a reason rather than for want of
 * looking: THE ORIGINAL HAS NO SUCH ROUTINE. It splits the panel across a
 * set-up that draws it once and a loop that touches only the parts that
 * change, and the port rebuilds the whole line each frame instead. Both put
 * the same picture on the screen.
 *
 * The set-up, image 0x00495 onward, runs once before the loop is entered:
 *
 *     0x00495  0x3944   the object animations advance one frame -
 *                       image 0x03944 is a pause guard on ds:0x2B in front of
 *                       image 0x07C0A, and NOT the play area, which is what
 *                       this line said when the sequence was first written up
 *     0x00498  0x7BB8   the objects
 *     0x0049B  0x7900   the MINIMAP'S TERRAIN, copied from 0xD320 to
 *                       ds:0x1F76 + 0x2172 + 2
 *     0x0049E  0x3AC8   the percentage, so the first frame already has it
 *
 * and the play loop at image 0x001B0 runs, every pass:
 *
 *     0x001B0  0x30A6   the release
 *     0x001B3  0x1F19   the lemmings walk
 *     0x001B6  0x3944   the object animations advance (pause-guarded)
 *     0x001B9  0x7BB8   the objects
 *     0x001BC  0x3634   the lemmings, and the OUT count at image 0x03684
 *     0x001BF  0x3CDE   what the pointer is over, into ds:0x2083
 *     0x001C2  0x3AC8   the minimap's DOTS (0x796F), then its VIEWPORT BOX
 *                       (0x7A5A), then the IN percentage into ds:0x209D
 *     0x001C5  0x3150
 *
 * Which settles the order the port had only MEASURED. The dots go under the
 * box because image 0x03AC8 calls 0x796F before 0x7A5A - the one pixel that
 * was level002/4's whole panel score - and the pointer's name is written
 * before any of it.
 *
 * ONE THING THIS RAISES AND DOES NOT ANSWER: image 0x7900 draws the minimap's
 * terrain and the loop never calls it again, so the original's thumbnail does
 * NOT follow a digger's changes. The port redraws it every frame from the
 * live terrain. Every panel scored here is exact, so nothing has caught it,
 * but the two disagree by construction the moment enough terrain goes - and
 * a level nobody plays removes none.
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
    /* THE RATE DIVIDER TURNS HERE, because image 0x03B8C is part of this
     * redraw rather than of the button - see level_rate_tick. It is in this
     * routine rather than in the callers for the reason the next paragraph
     * gives about the pointer: a sequence written out twice will differ. */
    level_rate_tick();                          /* image 0x03B8C */

    /* What the pointer is over goes into the line BEFORE the line is drawn -
     * image 0x03CE9 runs in the play loop ahead of the panel. It lives here
     * rather than in the callers because the order is the game's, and a
     * sequence written out twice is a sequence that will differ: that is
     * exactly how the window path came to have no panel at all while every
     * measurement of the panel passed. */
    level_status_pointer(lem, nlem, view_x);

    panel_frame(pp, PANEL_LINE_W, PANEL_ART_H, sec, seclen, PANEL_ART_X, 0);
    panel_status(pp, PANEL_LINE_W, PANEL_ART_H, sec, seclen,
                 level_status_line(), 0, 0);
    panel_buttons(pp, PANEL_LINE_W, PANEL_ART_H, sec, seclen, lv);
    panel_highlight(pp, PANEL_LINE_W, PANEL_ART_H, sel);
    panel_minimap(pp, PANEL_LINE_W, PANEL_ART_H, terrain, tw, th,
                  MINIMAP_AT_X, MINIMAP_AT_Y);
    /* THE DOTS FIRST, THE FRAME OVER THEM. A lemming standing where the
     * viewport's edge runs put a dot through it in the port and does not in
     * the original: at frame 186 of "Watch out, there`s traps about" the
     * original holds 3 at (266, 183) - the edge's own colour - where the port
     * held 2, on every frame a lemming passed that cell. One pixel, and it
     * was the whole of that level's panel score. */
    panel_minimap_dots(pp, PANEL_LINE_W, PANEL_ART_H, lem, nlem,
                       MINIMAP_DOT_X, MINIMAP_AT_Y);
    panel_viewport(pp, PANEL_LINE_W, PANEL_ART_H, view_x,
                   MINIMAP_AT_X, VIEWPORT_AT_Y);
}

/* MAY AN OBJECT'S PIXEL GO DOWN HERE - the LVL's two modifier bits, in one
 * place because they have twice drifted apart in two.
 *
 * The port draws objects from two routines: level_anim_draw, which is what
 * the game displays, and render_objects, which bakes them into a composed
 * level. Each has its own pixel loop, and a rule added to one has twice not
 * reached the other:
 *
 *   the NO-OVERWRITE test was `dst & 8` in one and `*dst != 0` in the other.
 *   Invisible, because no object pixel in the whole game lands on an
 *   attribute of 1..7 - the only place the two disagree.
 *   the ONLY-ON-TERRAIN test was in render_objects and MISSING from
 *   level_anim_draw, so a one-way wall's arrows were drawn over the black
 *   beside their block. A player sent screenshots.
 *
 * Both rules are the blitter's, and both come down to PLANE 3:
 *
 *   image 0x0953E  bp bit 15, no-overwrite. Image 0x09573 sets read mode 1
 *                  with Color Compare 8 and Color Don't Care 8, so a read
 *                  gives a bit per pixel saying whether plane 3 is set;
 *                  image 0x09595 inverts it into the bit mask. Written only
 *                  where plane 3 is CLEAR.
 *   image 0x0969C  bp bit 14, only-on-terrain. Same read, but image 0x09709
 *                  ANDs it with the object's own mask instead of inverting.
 *                  Written only where plane 3 is SET.
 *
 * Plane 3 is bit 3 of the attribute, which is why both are `& 8` and neither
 * is "something is already there". */
static int object_pixel_allowed(uint8_t dst, const level_object *o)
{
    /* ONLY-ON-TERRAIN IS TESTED FIRST, AND AN OBJECT CAN CARRY BOTH. Image
     * 0x093D2 tests bp bit 14 and jumps to image 0x0969C; only if that bit is
     * clear does image 0x093DB test bit 15 and jump to image 0x0953E. So when
     * both are set the object is drawn where there IS terrain, and the
     * no-overwrite bit never gets a say.
     *
     * The port ANDed the two conditions, which for such an object is
     * unsatisfiable - `dst & 8` cannot be both zero and non-zero - so it drew
     * NOTHING. Five objects in the game carry `e[6] == 0xC0`, all of them id
     * 4 in level003 section 7, "Upsidedown World", and all five were missing.
     *
     * Same shape as the terrain erase bug in render_terrain, and found the
     * same way: by listing the values a modifier byte actually takes across
     * all eighty levels instead of assuming the flags are exclusive. e[6] is
     * one of 0x00, 0x40, 0x80 and 0xC0, and only the last is interesting. */
    if (o->only_on_terrain)                  /* image 0x093D2, first */
        return (dst & 8) != 0;               /* image 0x09709 */
    if (o->no_overwrite)                     /* image 0x093DB */
        return (dst & 8) == 0;               /* image 0x09595 */
    return 1;
}

/* AND WHAT VALUE GOES DOWN. Neither path simply stores the sprite's pixel,
 * and the two differ from each other in both halves.
 *
 * THE COLOUR. Image 0x096BA sets the graphics controller's Set/Reset to 4 and
 * image 0x096C2 its Enable Set/Reset to 0x0E, so on the ONLY-ON-TERRAIN path
 * planes 1, 2 and 3 are written from that constant - 0, 1, 0 - and only plane
 * 0 comes from the ink. An arrow pixel lands as attribute 5 whatever the
 * sprite holds, and its plane 3 is CLEARED, so an arrow is not terrain
 * afterwards. The no-overwrite path sets Enable Set/Reset to 0 instead (image
 * 0x09563), so there all four planes are the sprite's own.
 *
 * Both one-way tiles in ground2o.dat decode to attribute 1 and nothing else -
 * 200 pixels each, one value - so the sprite could never have produced the 5
 * the original's video memory holds. The port drew them blue where the
 * original draws them red, 1,409 pixels on level004 section 3, and a player
 * found it from a screenshot. reconstruct/tests/screens.c asserts the tiles'
 * one colour, because that is the premise this rests on.
 *
 * THE COMBINE. Image 0x0956B sets the Data Rotate/Function register to 0x10,
 * whose bits 4:3 select the ALU function - and 0b10 is OR. So a NO-OVERWRITE
 * blit ORs the sprite into what is already there. Image 0x096CA and image
 * 0x08D98 both set that register to 0, so every other path replaces.
 *
 * The OR is invisible over black, which is everything a no-overwrite object
 * has been drawn over in any comparison here: `x | 0` and `0 | x` are both
 * x, and it only diverges where one object lands on another's pixels. It is
 * transcribed rather than left as an assignment because "unreachable in the
 * levels measured so far" is a fact about the measurements. It also settles
 * what happens to the sprite's TRANSPARENT pixels, which the comment below
 * says go down with the rectangle: they do, and they OR in a zero, so they
 * change nothing. Assigning wrote black. */
static uint8_t object_pixel_combine(uint8_t dst, uint8_t ink,
                                    const level_object *o)
{
    if (o->only_on_terrain)
        return (uint8_t)(4 | (ink & 1));   /* image 0x096BA, image 0x096C2 */
    if (o->no_overwrite)
        return (uint8_t)(dst | ink);       /* image 0x0956B: function OR */
    return ink;
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
                /* AND ITS COMPANION, `e[6] & 0x40` - ONLY ON TERRAIN. This
                 * routine tested no_overwrite and not this one, so a ONE-WAY
                 * WALL's arrows were drawn wherever the object's rectangle
                 * reached, including out over the black beside the block they
                 * belong to. A player sent two screenshots of it, on
                 * level004/3 and level005/8.
                 *
                 * render_objects had the test all along and this did not,
                 * which is the same split that hid the two no-overwrite
                 * rules: two routines draw objects here, and a flag added to
                 * one of them is not added to the other.
                 *
                 * Image 0x09709 is the rule, and it is the mirror of the one
                 * below: a blit with bp bit 14 reaches image 0x0969C, which
                 * sets the graphics controller to READ MODE 1 comparing plane
                 * 3 alone and then does `and ah, [bx]` - the destination's
                 * terrain AND the object's own mask. So the arrows go down
                 * only where plane 3 is already set. */
                uint8_t *px = &dst[(size_t)y * dst_w + (uint16_t)x];
                if (!object_pixel_allowed(*px, o))
                    continue;
                *px = object_pixel_combine(
                    *px, planar(object_sec, ink, plane, stride, 4, c, r), o);
            }
        }
    }
    (void)st;
}

/* NOT TRANSCRIBED - and the evidence recorded here answers a different
 * question from the one the marker is about, which is worth untangling.
 *
 * What was established, by breaking at image 0x03789 - the final `ret` after
 * all five composition passes - is that the original never composes objects
 * into the LEVEL bitmap: it draws the entrance and the exit as animated
 * sprites at display time. That is why tools/prove.sh passes --no-objects,
 * and it is a fact about the level, not about this routine.
 *
 * But this routine's caller inside the game is draw_preview, the briefing's
 * little map - and the original's briefing does show the objects on it. So
 * there IS an original routine that puts objects into a composed picture;
 * it just is not in the level's composition, which is the only place anyone
 * has looked. The debt is real and it is pointed at the wrong address.
 *
 * The briefing scores 100.00% against the original's own, so what is owed
 * here is provenance and not correctness. And the routine has now been
 * FOUND, which is a step short of read: image 0x07E76 builds the preview by
 * blitting the level four times and packing it 4:1, and draw_preview above
 * carries the whole chain. Whether the objects reach it through those blits
 * or are drawn separately is the question this routine's own transcription
 * turns on. */
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
                /* THE MASK IS NOT CONSULTED FOR A NO-OVERWRITE BLIT, and the
                 * two paths differ in exactly that. Image 0x09709, the
                 * only-on-terrain one, does `and ah, [bx]` - the destination's
                 * plane 3 AND the object's own mask. Image 0x09592, the
                 * no-overwrite one, has no such AND: its bit mask is `not ah`
                 * and nothing else, so the whole rectangle goes down wherever
                 * plane 3 is clear, transparent pixels included. */
                /* ...AND THE PATH IS CHOSEN BY ONLY-ON-TERRAIN FIRST, so an
                 * object carrying both flags takes image 0x0969C and DOES
                 * consult the mask. See object_pixel_allowed for the
                 * dispatch. Without this the five both-flag objects in
                 * level003 section 7 go down as whole rectangles. */
                if (!(o->no_overwrite && !o->only_on_terrain) && !mask[si])
                    continue;

                uint8_t *dst = &c->pix[(uint32_t)dy * c->w + (uint32_t)dx];
                /* THE SAME TEST level_anim_draw uses - see
                 * object_pixel_allowed, which exists because these two loops
                 * had drifted apart twice. */
                if (!object_pixel_allowed(*dst, o))
                    continue;
                /* AND THE SAME VALUE RULE - see object_pixel_combine. */
                *dst = object_pixel_combine(*dst, pix[si], o);
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
 * NOT TRANSCRIBED, but all four of the original's set-up calls are read now.
 * Image 0x04967 and image 0x04A01 both open with the same sequence:
 *
 *   image 0x04DEF  CLEAR the page. `di = 0`, `cx = 0x6D60` - 28,000 bytes,
 *                  which is 640 x 350 / 8, the whole thing.
 *   image 0x04BF5  DECLARE THE SURFACE. It sets the two page bases -
 *                  ds:0x1F76 = 0 and ds:0x1F78 = 0x6D60 - and the fine
 *                  scroll ds:0x1F7A = 0, then calls image 0x8808 with
 *                  ax = 0x280 (640), bx = 0x15E (350) and dx = the page.
 *
 *                  Image 0x8808 is worth naming: it stores ax in ds:0x66F2,
 *                  bx in ds:0x66F0 and ax >> 3 in ds:0x66F4. Those are the
 *                  BLITTER'S OWN bounds - the width it refuses a blit past
 *                  at image 0x09997, the height, and the row in bytes. So
 *                  the clip limit is simply whatever surface was last
 *                  declared: 640 here, and 352 in a level, which is what
 *                  tools/probe_clip.py measures from a running game.
 *   image 0x0531E  THE PALETTE. `al = 0, cx = 0x10, bx = 0x100`, then image
 *                  0x055B7, which branches on ds:0x1F7C - the adapter letter
 *                  - and indexes a table at ds:0x52D4. Sixteen entries.
 *   image 0x05017  THE LEVEL PREVIEW - `xor bx,bx / jmp 0x7E76`. See
 *                  draw_preview, which carries that whole chain.
 *
 * AND THE PORT'S DECOMPOSITION IS NOT THE ORIGINAL'S, which is the useful
 * thing to know and was hidden while the four calls were unread. Set them
 * beside what this routine actually does:
 *
 *   image 0x04DEF  clear the page      -> the calloc below, which is not the
 *                                        same act: the original clears VIDEO
 *                                        MEMORY it already owns
 *   image 0x04BF5  declare the surface -> nothing. The port has no blitter to
 *                                        bound, so ds:0x66F0/2/4 have no
 *                                        counterpart at all
 *   image 0x0531E  the palette         -> the MENU_PAL6 loop below. THE ONE
 *                                        THING THAT LINES UP
 *   image 0x05017  the level preview   -> draw_preview, a different routine
 *
 * and the two things this routine does that the original's sequence does not
 * contain at all: loading main.dat, which is DOS's work through image
 * 0x01D2E, and tiling the background.
 *
 * So the debt here is not "write out four calls". It is a DECISION about
 * where the port's boundary goes, and transcribing this routine means
 * restructuring around the original's split rather than filling in addresses.
 * That is worth doing deliberately or not at all - see level_panel_build and
 * dat_load, where the same question came up and the answer was that the
 * original had no such routine, and IO respectively. This one is neither:
 * the original's set-up sequence exists, and the port's is a different four
 * things.
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
/* ODDTABLE.DAT - the level's OTHER set of numbers.
 *
 * Eighty of the game's hundred and twenty plates are the same eighty levels,
 * so a level reached from two ratings has to differ somehow. Image 0x019C9
 * tests bit 0 of `n - 1` and, when it is set, image 0x019DC loads
 * `oddtable.dat` - 4,480 bytes, eighty entries of fifty-six - and image
 * 0x019F5 copies TWENTY-FOUR bytes of entry `(n - 1) >> 1` straight over the
 * level record's header at ds:0x0FCB. Those twenty-four are exactly the
 * release rate, the three counts and all eight skills. Image 0x019FE then
 * copies the entry's remaining thirty-two - its NAME - to ds:0x17AB.
 *
 * So half the game's plates play a level under a different name with
 * different skills and a different requirement, and the port had none of it.
 * It showed as the panel drawing 2, 0, 5, 10, 20, 2, 2, 2 on Fun 9 where the
 * original draws twenty eight times: the record in the guest's own memory at
 * ds:0x0FCB reads rate 99, 80 lemmings, rescue SEVENTY-TWO where the file
 * says eighty, and 0x14 in all eight skill words - which is entry 10 of
 * oddtable.dat to the byte, name and all: "As long as you try your best".
 *
 * Confirmed on four levels: n = 22 and n = 42 have the bit and were
 * overridden; n = 39 and n = 43 do not and kept their own. */
int level_odd_entry(uint8_t rating, uint8_t number)
{
    if (rating > 3 || number < 1 || number > 30) return -1;
    uint8_t n = LEVEL_ORDER[rating * 30 + (number - 1)];
    if (n == 0 || !((n - 1) & 1)) return -1;    /* image 0x019C9 */
    return (n - 1) >> 1;                        /* image 0x019E0 */
}

/* WHICH ENTRY THE NEXT PARSE SHOULD TAKE, or -1 for none.
 *
 * The original has no such variable: image 0x019C9 reads the level index it
 * already has, right where the record is loaded. The port parses a level in
 * five places - the briefing, the level, the preview and two in the harness -
 * and threading an argument through all of them would be five chances to
 * forget one. So the caller that KNOWS the plate says so once, and every
 * parse after it obeys. level_show_briefing and level_show both clear it,
 * because a stale entry is the same fault as a stale capture. */
static int g_odd_pending = -1;

/* THE PORT'S OWN. The original has no such variable - image 0x019C9 reads
 * the level index it already has, where the record is loaded. */
void level_odd_pending(int entry) { g_odd_pending = entry; }
/* THE PORT'S OWN, and it CONSUMES: a stale entry is a stale capture. */
int  level_odd_taken(void) { int e = g_odd_pending; g_odd_pending = -1; return e; }

#define ODD_ENTRY   56          /* image 0x019E2: dh = 0x38 */
#define ODD_HEADER  24          /* image 0x019F0: cx = 0x0C words */
#define ODD_NAME    32          /* image 0x019FB: cx = 0x10 words */

/* Overlay one entry on a parsed level. The twenty-four bytes are the header's
 * own first twenty-four, so this sets exactly the fields level_parse read
 * from them and nothing else. */
/* Image 0x019F5 copies twenty-four bytes of the entry over the record's
 * header and image 0x019FE its thirty-two of name. */
int level_apply_odd(const char *dir, level *lv, int entry, const char **err)
{
    if (entry < 0) return 0;
    char path[256];
    snprintf(path, sizeof(path), "%s/oddtable.dat", dir);
    FILE *f = fopen(path, "rb");
    if (!f) { if (err) *err = "oddtable.dat: cannot open it"; return -1; }
    uint8_t e[ODD_ENTRY];
    if (fseek(f, (long)entry * ODD_ENTRY, SEEK_SET) != 0
            || fread(e, 1, ODD_ENTRY, f) != ODD_ENTRY) {
        fclose(f);
        if (err) *err = "oddtable.dat: short read";
        return -1;
    }
    fclose(f);
    lv->release_rate  = (uint8_t)be16(e + 0x00);
    lv->num_lemmings  = (uint8_t)be16(e + 0x02);
    lv->num_to_rescue = (uint8_t)be16(e + 0x04);
    lv->time_limit    = (uint8_t)be16(e + 0x06);
    for (uint8_t i = 0; i < 8; i++)
        lv->skills[i] = (uint8_t)be16(e + 0x08 + i * 2);
    memcpy(lv->name, e + ODD_HEADER, ODD_NAME);
    lv->name[ODD_NAME] = '\0';
    return 0;
}

/* Image 0x04551's table, LEVEL_ORDER - see level_odd_entry above for how
 * the same n decides whether ODDTABLE.DAT applies. */
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

/* WHERE THE BRIEFING'S NUMBERS GO - transcribed from image 0x04580.
 *
 * Three different writers, each with its own column, and the port had one
 * right-aligned fill standing in for all of them:
 *
 *   image 0x0458E  the lemming count through image 0x04B68, at ds:0x4B50 -
 *                  index 19 of "Number of Lemmings    "
 *   image 0x045A5  the percentage through image 0x04B5C, at ds:0x4B57 -
 *                  index 0 of "     To Be Saved", which is what makes it
 *                  read "10%  To Be Saved"
 *   image 0x045AF  the release rate through image 0x04BBE, at ds:0x4B78 -
 *                  index 13 of "Release Rate   "
 *   image 0x045B5  the time limit as a SINGLE digit at ds:0x4B83, index 5
 *
 * All of them left-aligned. Right-aligning gives the same characters for a
 * two-digit number and different ones for three: the original writes 100
 * lemmings at 19..21 and the port's fill put them at 18..20. Fun 1 has ten,
 * which is why the briefing verified exact with the wrong rule.
 */
#define BR_COUNT_AT   19      /* ds:0x4B50 */
#define BR_SAVED_AT    0      /* ds:0x4B57 */
#define BR_RATE_AT    13      /* ds:0x4B78 */
#define BR_TIME_AT     5      /* ds:0x4B83 */
#define BR_TIME_S_AT  13      /* ds:0x4B8B */

/* put_num3 and put_num2 live further up, with the status line they were
 * transcribed for - images 0x04B68 and 0x04BBE. */
static char *put_num3(char *dst, uint8_t v);
static char *put_num2(char *dst, uint8_t v);

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
/* BOTH OF THESE ARE READ NOW, out of image 0x07F28..0x07F76 - the packer's
 * bit scatter, which takes four source bytes and builds one output byte a
 * plane. Decoding where each output bit comes from (bit 7 is the leftmost
 * pixel):
 *
 *   out bit 7  ch rol 2, and 0x80   source bit 5   pixel  2
 *   out bit 6  ch ror 3, and 0x40   source bit 1   pixel  6
 *   out bit 5  cl        and 0x20   source bit 5   pixel 10
 *   out bit 4  cl rol 3, and 0x10   source bit 1   pixel 14
 *   out bit 3  bh ror 2, and 0x08   source bit 5   pixel 18
 *   out bit 2  bh rol 1, and 0x04   source bit 1   pixel 22
 *   out bit 1  bl rol 4, and 0x02   source bit 5   pixel 26
 *   out bit 0  bl ror 1, and 0x01   source bit 1   pixel 30
 *
 * which is every FOURTH pixel starting at the THIRD - scale 4, sample 2,
 * exactly what was measured off the screen. */
#define PV_SCALE    4
#define PV_SAMPLE   2      /* which pixel of each 4x4 block is taken */
#define PV_TOP      4      /* the first level row the preview shows
 *
 * NOT TRANSCRIBED - but the address is no longer unknown, and the reason no
 * BLIT in screen coordinates was ever found is that the preview is not
 * blitted: it is written plane by plane, straight into the visible page.
 *
 * (An earlier version of this note said the preview was composed into a
 * buffer and went up inside the briefing's one big blit. That was wrong. The
 * 632 x 94 blit at image 0x04F0A is the panel's STATIC artwork, decompressed
 * from a file into ds:0x1FEE - tools/probe_preview.py finds only the
 * decompressor writing there, at image 0x01C83 and image 0x01C9A, in both
 * halves of that segment. The preview is drawn on top of it afterwards.)
 *
 * The chain:
 *
 *   image 0x04639 -> image 0x05017 (`xor bx,bx / jmp`) -> image 0x07E76
 *          the builder. It copies ds:0x1FEE into ds:0x1FE4 and blits the
 *          level through the shared blitter four times, then falls into
 *   image 0x07EC5..0x080F8
 *          the 4:1 packer, and image 0x07EFC sets DS as well as ES to
 *          0xA000 - it reads the LEVEL BITMAP out of video memory and writes
 *          the visible page, with no buffer between. Every number in it is
 *          one of this file's:
 *
 *            image 0x07EFE  cx = 0x27    39 rows            PV_H
 *            image 0x07F04  cx = 0x34    52 bytes a row     396 -> 416 px
 *            image 0x080ED  di += 0x50   80, the 640-wide screen's stride
 *            image 0x080F0  si += 0x340  832 = 4 x 208, so FOUR level rows
 *                                        per output row    PV_SCALE
 *            image 0x07F1D  four source bytes in, one output byte a plane
 *                                        out                PV_SCALE again,
 *                                        horizontally, and PV_SAMPLE
 *
 *          Its four unrolled plane writes are image 0x07F7B, 0x07FF1,
 *          0x08067 and 0x080DD, and tools/probe_writes.py sees exactly 2,028
 *          bytes from each - 52 x 39.
 *   image 0x04434 -> image 0x04EEF -> image 0x04F0A
 *          the panel's static artwork UNDER it: `bx = 0x14` (y = 20, PV_Y),
 *          `dx = 0x278` (632 wide), `cl = 0x5E` (94 rows), `si = 0x2080`,
 *          through `lcall 0x1010:0x8822` from the segment in ds:0x1FEE.
 *
 * AND THE REST OF THE GEOMETRY, from image 0x07EB2 - the packer's own entry:
 *
 *     di = ds:0x1F76 + ds:0x1F7A + 0x64F   0x64F is 20 rows of 80 plus byte
 *                                          15, so y = 20 and x = 120 -
 *                                          PV_Y and PV_X
 *     si = 0x4F22                          0x4A40 + 6 * 208 + 2: the level
 *                                          bitmap's ROW 6, and byte 2 is its
 *                                          sixteen-pixel left margin, which
 *                                          is where this port's canvas
 *                                          starts. Row 6 is PV_TOP + PV_SAMPLE
 *
 * So every constant in this block is read out of the original now, and all
 * six agree with what had been measured off the screen.
 *
 * WHAT IS STILL OPEN, and it is the whole of what render_objects owes. The
 * original's preview DOES show the objects: taking them out of this routine
 * takes the preview from 0 pixels differing to 53. But the packer's source is
 * the level bitmap, and image 0x03789 established the original never
 * composes objects into that - tools/prove.sh compares against it with
 * --no-objects and gets 100.00%.
 *
 * ALL FIVE of the passes after the terrain loop are now identified, and none
 * of them draws an object:
 *
 *     image 0x0378A  copies the object records into the animation array at
 *                    ds:0x17CB - level_anim_init's input
 *     image 0x03839  the STEEL list at ds:0x172B, 32 entries of four, with
 *                    `dl = dh & 0xf` and `dh >>= 3` unpacking the width and
 *                    height nibbles level_parse already reads
 *     image 0x037C2  the TRIGGER MAP - its `mul 0x1a0` and `add bx, 0x12`
 *                    are image 0x03808, which level_triggers_build carries
 *     image 0x0389F  the ENTRANCES - image 0x038A3, which level_entrances
 *                    carries
 *     image 0x036AD  a clear: di = 0xCC40, cx = 0x340
 *
 * SETTLED, and image 0x03789's conclusion was wrong as it was stated.
 * tools/probe_pvsrc.py rides along and dumps the level bitmap at image
 * 0x07EB2 - the packer's entry, hit exactly once, at the briefing - and
 * compares it against this port's canvas:
 *
 *     the port WITHOUT objects   815 pixels differ
 *     the port WITH objects       19 pixels differ
 *
 * and the 815 begin at (707, 36), which is where this level's objects are.
 * So the objects ARE in the bitmap by the time the preview reads it, and
 * drawing them here is right.
 *
 * What image 0x03789 actually established is narrower: at tools/prove.sh's
 * break, image 0x0376A, they are not there yet - which is why that comparison
 * passes --no-objects and gets 100.00%. The five passes at image 0x0376A..
 * 0x03776 run AFTER that break, and the briefing comes after all of it. The
 * claim "the original never composes objects into the level bitmap" was a
 * fact about one instant read as a fact about the program.
 *
 * AND THE ROUTINE IS FOUND: image 0x03981.
 *
 *     di = ds:0x0FEB, cx = 0x20      the object list, 32 of them
 *     ds:0x1FE4 = ds:0x1FF0          the objects' graphics segment, which is
 *                                    NOT the terrain's - the blit probe sees
 *                                    srcseg 0x02900 here against 0x07272 for
 *                                    a terrain piece, which is what tells the
 *                                    two apart in one line of its output
 *     ah,al = [di],[di+1]            x, and zero ends the list
 *     bh,bl = [di+2],[di+3]          y
 *     dh,dl = [di+6],[di+7]          the LVL's modifier bytes, and image
 *                                    0x039A8's `bp = dx & 0xFFFE` makes them
 *                                    the blit's flags - bit 15 is e[6] & 0x80,
 *                                    the no-overwrite level_parse reads, and
 *                                    the probe caught bp = 0x800E
 *     dl,cl = [si+4],[si+5]          width and rows from the definition
 *     di = [si+8] + [si+0x17]        the graphic
 *     ch = 4                         four planes
 *     image 0x039BD                  the blit
 *
 * Found by asking tools/probe_blits.py for rows 36..37 - the level's first
 * object is at (720, 36) - which listed three blits: two terrain pieces from
 * image 0x07CB5 and this one from a different caller and a different source
 * segment.
 *
 * It sits in a SECOND composition path at image 0x0394F, which is why
 * tools/prove.sh has never seen it: that breaks at image 0x0376A, inside the
 * first.
 *
 * AND IT IS EXACT: 243,936 of 243,936 pixels, 0 differing, against the
 * original's own bitmap - tools/verify_objects.py.
 *
 * It scored NINETEEN differing until the check was fixed, and the nineteen
 * were the check's. It rendered the port with plain --raw, which draws the
 * objects the DISPLAY-TIME way through level_anim_draw and image 0x07BB8,
 * and compared that against the original's COMPOSITION of them, image
 * 0x03981. Two different routines with different rules, scored against each
 * other and the difference read as the port's. --baked is the one that calls
 * this routine, and with it the same run is exact.
 *
 * So the residue that three separate leads were chased through - the
 * no-overwrite rule, the pre-shifted byte blit, an edge offset - never
 * existed. Each of those reads was still worth having and two of them
 * changed the code below, but the thing they were aimed at was a harness
 * comparing the wrong pair.
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
 * THE COMPOSITION at image 0x045EF is read now, all six of it:
 *
 *   image 0x04DEF  clear the page - `di = 0`, `cx = 0x6D60`, which is
 *                  28,000 bytes: the whole 640 x 350 at eight pixels a byte
 *   image 0x05329  the palette, sixteen entries from 0x107 through image
 *                  0x05569, with 0x100 read first
 *   image 0x0394F  THE SECOND COMPOSITION PATH - the one that draws the
 *                  objects into the level bitmap at image 0x03981, which
 *                  render_objects owes and which tools/prove.sh's break at
 *                  image 0x0376A sits before
 *   image 0x07EB2  the level preview, packed 4:1 straight into the visible
 *                  page - see draw_preview for the whole chain
 *   image 0x0463C  the text: a descriptor at ds:0x4B0A through image 0x04D4D,
 *                  then one of three more at ds:0x4BA0, ds:0x4BC3 or
 *                  ds:0x4BE5 chosen by ds:0x25A6 - the input device, so the
 *                  last line names the mouse, the joystick or the keyboard.
 *                  Image 0x04660 then polls ds:0x14 for the button.
 *
 * So the port's measured order is now the original's read order, and they
 * agree. What is still owed is the transcription: this routine does the same
 * things and its steps carry no addresses.
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
    put_num3(line + BR_COUNT_AT, (uint8_t)lv->num_lemmings);  /* 0x0458E */
    draw_at(m->pix, font, fontlen, line, BR_BODY_X, y);
    y += BR_BODY_STEP;

    /* The percentage, not the count: "10%  To Be Saved" for 1 of 10. */
    snprintf(line, sizeof(line), "%s", T_SAVED);
    {
        /* Image 0x04591's own arithmetic, and image 0x045A5's own writer. */
        char *at = line + BR_SAVED_AT;
        at[3] = ' ';                                   /* image 0x04B5C */
        *put_num3(at, level_needed_percent(lv)) = '%'; /* image 0x04B63 */
    }
    draw_at(m->pix, font, fontlen, line, BR_BODY_X, y);
    y += BR_BODY_STEP;

    snprintf(line, sizeof(line), "%s", T_RATE);
    put_num2(line + BR_RATE_AT, (uint8_t)lv->release_rate);   /* 0x045AF */
    draw_at(m->pix, font, fontlen, line, BR_BODY_X, y);
    y += BR_BODY_STEP;

    snprintf(line, sizeof(line), "%s", T_TIME);
    /* Image 0x045B5 adds '0' to one byte - a single digit, no writer. */
    line[BR_TIME_AT] = (char)('0' + (lv->time_limit % 10u));
    /* "1 Minute", not "1 Minutes". Image 0x045B2 puts the digit at ds:0x4B83
     * and the 's' at ds:0x4B8B - eight characters apart, which is indices 5
     * and 13 of this template - and swaps the 's' for a SPACE when the digit
     * is '1', so the line keeps its length either way. */
    if (line[BR_TIME_AT] == '1')
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

