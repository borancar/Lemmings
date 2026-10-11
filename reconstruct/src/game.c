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
#include "data.h"

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
        /* DEAD IN EVERY SHIPPED LEVEL, and now known to be. e[7] is 0x0F
         * in all 454 objects of all eighty levels, so this is never true.
         * Left as it is rather than removed: it came from the published
         * format description, the original's own use of the byte is bp's
         * low bits, and "no level exercises it" is not the same as "it is
         * wrong". Recorded so the next reader does not spend time on a rule
         * nothing can reach. */
        o->upside_down     = (uint8_t)(e[7] == 0x8F);
        /* Image 0x039A0 and image 0x039A6: the two bytes as a word, bit 0
         * masked off, which is what the blitter is handed. */
        o->bp = (uint16_t)((((uint16_t)e[6] << 8) | e[7]) & 0xFFFEu);
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

uint8_t g_terrain_reverse = 0;

/* ======================================================================
 * IMAGE 0x088EA - WHICH OF THE BLITTER'S FIVE PATHS A `bp` TAKES.
 *
 * The original has ONE blitter, reached by a far call to 1010:8822, and its
 * mode word picks the path:
 *
 *     0x088EA  test bp, 0x4000 / jne -> 0x08D66   planes 1..3 forced to zero
 *     0x088F3  test bp, 0x8000 / jne -> 0x08B42   masked, no overwrite
 *     0x088FC  test bp, 0x0020 / jne -> 0x08FF7
 *     0x08905  test bp, 0x0040 / jne -> 0x0909C   ERASE, all four planes 0
 *     0x0890E                                     the four-pass sprite draw
 *
 * THE ORDER IS THE DISPATCH'S, not a preference, and this port has already
 * paid for reading it as one: no-overwrite is tested BEFORE erase, so a
 * terrain piece carrying both is drawn rather than erased. That cost 814
 * pixels on level006/3 and a wrong rule that improved the number to 459
 * before the unpack was read properly.
 *
 * The rule lived inline at the one call site that needed it. It is a
 * dispatch the whole program shares, so it is a routine - the same reason
 * level_play_step exists, and the reason CLAUDE.md gives for reading the
 * eighteen Set/Reset loads rather than following flags to one of them.
 * ====================================================================== */
/* ======================================================================
 * IMAGE 0x09967 - THE CLIPPER, which bp bits 2 and 3 ask for.
 *
 * Image 0x08846 tests `bp & 0x0C` at the blitter's entry, before the
 * dispatch, and calls this when either is set: bit 2 clips horizontally and
 * bit 3 vertically. The preview builder at image 0x07E76 passes bp = 0x0C,
 * so it asks for both.
 *
 * The entry has already computed the blit's geometry into three words:
 *
 *     0x0882D  ds:0x66FA = dx >> 3     the source row stride, in BYTES
 *     0x08834  ds:0x66F8 = that * cx   one plane's size
 *     0x0883A  ds:0x66FE = 0           what the vertical clip skipped
 *     0x08840  ds:0x66FC = 0           what the horizontal clip skipped
 *
 * HORIZONTALLY IT CLIPS IN BYTES, NOT PIXELS. Image 0x09981 advances si and
 * di by ONE, counts ds:0x66FC up, and image 0x0998C takes EIGHT off the
 * width while image 0x0998F adds eight to x - repeating while x is still
 * negative. So a sprite hanging off the left edge loses whole bytes at a
 * time, and what remains starts on a byte boundary. Vertically image 0x099C6
 * shrinks by rows and steps si and di by the stride, which is per-row.
 *
 * Off the edge entirely - image 0x0997E and image 0x099C4 - it jumps to
 * image 0x09A0C and the blit does not happen at all.
 *
 * THE PORT CLIPS PER PIXEL, at every draw site, with `if (px < 0 || px >=
 * w) continue`. For a sprite that is byte-aligned to begin with the two
 * agree; for one that is not, they need not, and no comparison here has ever
 * put a lemming half off the left edge. It is written down rather than
 * changed, because changing it would be fitting a rule nothing has measured.
 * ====================================================================== */
#define BLIT_CLIP_X   0x0004      /* image 0x0996E */
#define BLIT_CLIP_Y   0x0008      /* image 0x099B6 */
#define BLIT_CLIP_STEP 8          /* image 0x0998C: whole bytes */

/* Does this bp ask for clipping - image 0x08846's `test bp, 0xc`. */
int blit_clips(uint16_t bp) { return (bp & (BLIT_CLIP_X | BLIT_CLIP_Y)) != 0; }

/* ======================================================================
 * THE FIVE MODES, and what each does to the card.
 *
 * Every one of them programs the graphics controller and then moves bytes.
 * The port has no card, so what transcribes from each is its RULE, and each
 * rule already lives at the call site that needs it - the port's shape, not
 * a gap:
 *
 *   image 0x0890E  BLIT_SPRITE. Sequencer map mask 0x0F, GC 0 = 0, GC 1 =
 *                  0x0E, GC 5 = 0, GC 3 = 0, then four passes - the first
 *                  masked through GC 8 clearing planes 1..3, the other three
 *                  OR-ing their plane in unmasked.
 *                  -> blit() below carries it, with those addresses.
 *   image 0x08B42  BLIT_NO_OVERWRITE, reached from image 0x088F3 BEFORE
 *                  erase is ever tested - which is why a terrain piece
 *                  carrying both flags is drawn.
 *                  -> render_terrain's no-overwrite arm.
 *   image 0x0909C  BLIT_ERASE. Image 0x090B8 sets GC 0 (set/reset) to ZERO
 *                  and image 0x090BF sets GC 1 (enable set/reset) to bp bits
 *                  11..8, so those planes are written as 0.
 *                  -> render_terrain's erase arm.
 *   image 0x08FF7  AND ITS EXACT COMPLEMENT, which nothing in this port has
 *                  had a name for. Image 0x09013 sets GC 0 to 0x0F and image
 *                  0x0901B sets GC 1 to the same bp nibble - so it writes
 *                  ONE to the selected planes where erase writes zero. The
 *                  two routines are identical but for that constant, which
 *                  is what makes the pairing certain rather than plausible.
 *                  -> NOT TRANSCRIBED, and it is the one mode with no
 *                  counterpart here. Nothing in the port's own composition
 *                  fills planes with ones.
 *
 *                  AND NOTHING IN THE SHIPPED GAME ASKS FOR IT. The only
 *                  caller that could is image 0x03981's object pass, whose
 *                  bp comes from the level file - and across all eighty
 *                  levels, 454 objects, e[7] is 0x0F EVERY TIME and e[6] is
 *                  one of 0x00, 0x40, 0x80 or 0xC0. Bit 5 is never set. Of
 *                  the other fourteen call sites, one sets an immediate with
 *                  bit 5 clear (image 0x04F0A, bp = 0xF42, which is bit 6 -
 *                  an erase) and the rest compute bp from data that has just
 *                  been shown not to contain it.
 *
 *                  So image 0x08FF7 is unreached in this build. That is a
 *                  measurement over the whole game, not a guess from the
 *                  levels this repository happens to compare.
 *
 *                  REACHED ONLY BY THE DISPATCH, at image 0x08902. An
 *                  earlier note here said it was also called directly from
 *                  image 0x068FA; that was a scratch byte-scan for `E8`
 *                  finding a sequence that is not an instruction boundary -
 *                  image 0x068FB is a `cli`. The code map has none of the
 *                  five modes as a routine head, because they are jump
 *                  targets inside the blitter and nothing calls them.
 *                  So what sets bp bit 5 is the open question, and it is a
 *                  question about the CALLERS of 1010:8822, not about a
 *                  caller of this.
 *
 *                  AND THE TERRAIN PATH CANNOT EXPRESS IT, which is a
 *                  proof rather than a survey. Image 0x07C67 builds a
 *                  piece's bp as `shr dx,7 / and dx,0xC0 / or dx,0x0F`,
 *                  then ORs 0x0F00 for erase (image 0x07C75) and 0x8000
 *                  for no-overwrite (image 0x07C83). The `and dx,0xC0`
 *                  masks bit 5 OUT before anything else runs and nothing
 *                  afterwards ORs 0x20, so no terrain piece can ask for
 *                  this mode whatever its flags say. The object survey
 *                  above is empirical - 454 objects, bit 5 never set - and
 *                  this one is structural, so between them both
 *                  data-driven callers are excluded.
 *   image 0x08D66  BLIT_SETRESET_ZERO, which forces planes 1..3 to zero so
 *                  the blit can only write 0 or 1.
 *                  -> the one-way arrows; CLAUDE.md records the reading.
 *
 * So the "route through 1010:8822" that render_terrain, render_objects and
 * draw_preview each name is narrower than it sounded: four of the five rules
 * are transcribed at their call sites and what is shared is the byte-moving,
 * which a byte-per-pixel canvas does differently by construction. The fifth,
 * image 0x08FF7, is a real gap and is now stated as one.
 * ======================================================================
 *
 * Image 0x088EA's five-way dispatch. */
blit_path blit_path_of(uint16_t bp)
{
    if (bp & 0x4000) return BLIT_SETRESET_ZERO;   /* image 0x088EA */
    if (bp & 0x8000) return BLIT_NO_OVERWRITE;    /* image 0x088F3 */
    if (bp & 0x0020) return BLIT_MODE_20;         /* image 0x088FC */
    if (bp & 0x0040) return BLIT_ERASE;           /* image 0x08905 */
    return BLIT_SPRITE;                           /* image 0x0890E */
}

/* The flag word image 0x07C67 unpacks for a terrain piece, rebuilt from the
 * fields level_parse kept. Image 0x07C7F puts the no-overwrite bit at 15 and
 * image 0x07C75 puts erase at 6 - and ORs 0x0F00 beside it, which is what
 * image 0x090BF turns into "all four planes", so the nibble travels with the
 * bit and is carried here for the same reason. */
uint16_t terrain_bp(const level_terrain *t)
{
    uint16_t bp = 0;
    if (t->no_overwrite) bp |= 0x8000;            /* image 0x07C7F */
    if (t->erase)        bp |= 0x0040 | 0x0F00;   /* image 0x07C75, 0x07C7B */
    return bp;
}

/* ======================================================================
 * IMAGE 0x08822 - THE BLITTER, AND ALL FIVE OF ITS MODES.
 *
 * Every picture in the program that is not the level's panel goes through this
 * far call. Image 0x07CB5 hands it a graphic, a MASK, a width in bytes, a row
 * count and four planes; image 0x088EA..0x0890E then picks one of five paths
 * off the flag word in bp, in that order:
 *
 *     0x088EA  bp & 0x4000 -> 0x08D66   BLIT_SETRESET_ZERO
 *     0x088F3  bp & 0x8000 -> 0x08B42   BLIT_NO_OVERWRITE
 *     0x088FC  bp & 0x0020 -> 0x08FF7   BLIT_MODE_20
 *     0x08905  bp & 0x0040 -> 0x0909C   BLIT_ERASE
 *     0x0890E  otherwise                BLIT_SPRITE
 *
 * blit_path_of has named these for a long time and none of the five was
 * written out. What stood in their place was a family of SEVEN candidate rules
 * chosen by a flag - replace, or, keep, nov-ignore, nov-invert, or-all, hw -
 * of which `replace` was the fitted winner and the default. It scores 100.00%
 * on the level tools/prove.sh drives.
 *
 * That is the trap this repository has a section about. The fitted winner was
 * right about that level and the rule was not read; `hw`, which WAS read off
 * image 0x08B42, scored 95.04% - not because the reading was wrong but because
 * it applied ONE mode to every piece where the blitter picks per piece.
 *
 * WHAT EACH MODE DOES, from the registers each one programs. `ink` is the
 * source pixel assembled from its four planes, `m` its mask bit, `d` the
 * destination pixel:
 *
 *   0x08B42  NO_OVERWRITE   map mask 0x0F, GC 5 = 8 (read mode 1), GC 2 = 8
 *                           and GC 7 = 8, so reading video memory answers
 *                           "is plane 3 set here" - already solid terrain.
 *                           Image 0x08BD5 inverts that into GC 8, the bit
 *                           mask, and GC 3 = 0x10 selects OR. So:
 *                               d & 8 ? d : d | ink
 *                           It never tests the source for transparency: OR
 *                           with a transparent pixel's zero bits is a no-op,
 *                           which is why it does not have to.
 *
 *   0x0909C  ERASE          map mask 0x0F, GC 0 = 0 and GC 1 = bp's 0x0F00
 *                           nibble (image 0x090BF), so all four planes come
 *                           from Set/Reset and Set/Reset is ZERO. GC 8 is the
 *                           source byte, so the shape decides where:
 *                               m ? 0 : d
 *
 *   0x08D66  SETRESET_ZERO  GC 0 = 0 and GC 1 = 0x0E (image 0x08D90), so
 *                           planes 1..3 are forced to zero and only plane 0
 *                           comes from the source. It can write nothing but 0
 *                           or 1, which is why an arrow drawn this way is not
 *                           terrain afterwards:
 *                               m ? (ink & 1) : d
 *
 *   0x08FF7  MODE_20        GC 0 = 0x0F (image 0x09013) with GC 1 = bp's
 *                           nibble - Set/Reset ONES rather than zeros, so it
 *                           writes solid white through the shape:
 *                               m ? 0x0F : d
 *
 *   0x0890E  SPRITE         four passes: the first masked through GC 8 and
 *                           clearing planes 1..3, the other three OR-ing their
 *                           plane in unmasked. CLAUDE.md records this one
 *                           being read after four fitted candidates had
 *                           narrowed to the wrong answer:
 *                               m ? ink : d | (ink & 0x0E)
 *
 * THE GRANULARITY IS THE ORIGINAL'S: plane, row, BYTE. This was written per
 * pixel first, on the argument that the byte-at-a-time loop and the shift are
 * artefacts of a planar layout the port does not have. That argument is
 * plausible and it throws away the thing the passes are FOR: image 0x08B42
 * makes four of them, one per plane, and recomputes its bit mask from the
 * destination's plane 3 on every one - so a per-pixel rule gets the right
 * answer only because plane 3 happens to be written last. It assumes what the
 * pass order guarantees.
 *
 * So the row is copied to scratch and shifted right by x & 7 (image 0x08BA1,
 * image 0x08BBB), the write loop runs w+1 bytes (image 0x08BCF's `inc cl`),
 * and one `stosb` is one plane's bit across eight pixels. The only thing that
 * cannot carry over is the STORE itself: the card spread a byte across a plane
 * through the map mask and the latches, and here the eight pixels are separate
 * bytes, so it is a read-modify-write of one bit in each. The latches were
 * exactly that read.
 *
 * It reads the planar source where it lies, which is why there is no longer a
 * routine that decodes a piece into a tile first. The original materialises no
 * tile either.
 * ====================================================================== */

/* THE PORT'S OWN, and it exists because of a difference in LAYOUT rather than
 * in behaviour. The original never assembles a pixel: image 0x08BDE moves a
 * whole byte and the card's four planes and latches do the rest. This canvas
 * is byte-per-pixel, so the four plane bits have to be gathered here.
 *
 * The plane order and the bit order are g_plane_bits and g_msb_first, which
 * lemdev can still sweep - they were MEASURED, not read, and those flags are
 * where that is admitted. */
uint8_t blit_ink(const uint8_t *src, uint32_t plane, uint32_t stride,
                 uint8_t x, uint8_t y)
{
    uint8_t v = 0, p;
    uint8_t sh = g_msb_first ? (uint8_t)(7 - (x & 7)) : (uint8_t)(x & 7);
    for (p = 0; p < 4; p++) {
        const uint8_t *row = src + (uint32_t)p * plane + (uint32_t)y * stride;
        v |= (uint8_t)(((row[x >> 3] >> sh) & 1u) << g_plane_bits[p]);
    }
    return v;
}

/* THE PORT'S OWN, as blit_ink above: the mask is one plane and the original
 * reads it a byte at a time into GC 8. Image 0x07CAE is the pointer. */
uint8_t blit_mask(const uint8_t *mask, uint32_t stride,
                  uint8_t x, uint8_t y)
{
    uint8_t sh = g_msb_first ? (uint8_t)(7 - (x & 7)) : (uint8_t)(x & 7);
    return (uint8_t)((mask[(uint32_t)y * stride + (x >> 3)] >> sh) & 1u);
}

/* THE ROW, COPIED TO SCRATCH AND SHIFTED - image 0x08BA1 and image 0x08BBB.
 *
 *     0x08BA1  rep movsb            the row into ds:0x6706
 *     0x08BA3  a zero byte after it, so bits shifted off the end land
 *                                   somewhere
 *     0x08BBB  shr [si],1 / rcr [si+1],1 ...   the whole row right one bit,
 *                                   repeated ds:0xA948 times - the destination
 *                                   x's low three bits
 *
 * This is the part a byte-per-pixel port is tempted to skip, because it can
 * address any x directly. It is kept because the ROW is what the original
 * moves: a byte is eight pixels, the write loop runs w+1 of them (image
 * 0x08BCF's `inc cl`), and the extra byte is where the shifted-out bits go. */
static void blit_shift_row(uint8_t *scratch, const uint8_t *row,
                           uint32_t bytes, uint8_t shift)
{
    uint32_t i;
    uint8_t n;
    for (i = 0; i < bytes; i++) scratch[i] = row[i];
    scratch[bytes] = 0;                          /* image 0x08BA3 */
    for (n = 0; n < shift; n++) {                /* image 0x08BB7's AL */
        uint8_t carry = 0;
        for (i = 0; i <= bytes; i++) {           /* image 0x08BBB, 0x08BC0 */
            uint8_t next = (uint8_t)(scratch[i] & 1u);
            scratch[i] = (uint8_t)((scratch[i] >> 1) | (carry ? 0x80u : 0u));
            carry = next;
        }
    }
}

/* IMAGE 0x08BDF's `stosb` - ONE PLANE'S BIT ACROSS EIGHT PIXELS, with this
 * plane selected by the sequencer's map mask and `gc_mask` in GC 8.
 *
 * The original stores one byte and the card spreads it over the plane the map
 * mask selects, leaving the other three alone by way of the latches. Here the
 * eight pixels are separate bytes, so the same thing is a read-modify-write of
 * one bit in each - which is what the latches were. `or_in` is GC 3 = 0x10,
 * the OR function, against 0 for replace. */
static void blit_plane_byte(uint8_t *dst, int32_t dst_w, int32_t dst_h,
                            int32_t bx, int32_t by, uint8_t plane,
                            uint8_t bits, uint8_t gc_mask, uint8_t or_in)
{
    uint8_t k;
    if (by < 0 || by >= dst_h) return;
    for (k = 0; k < 8; k++) {
        int32_t dx = bx + (int32_t)k;
        uint8_t bit;
        uint8_t *d;
        if (dx < 0 || dx >= dst_w) continue;
        if (!(gc_mask & (uint8_t)(0x80u >> k))) continue;     /* GC 8 */
        bit = (uint8_t)((bits >> (7 - k)) & 1u);
        d = &dst[(uint32_t)by * (uint32_t)dst_w + (uint32_t)dx];
        if (or_in) *d = (uint8_t)(*d | (uint8_t)(bit << plane));
        else       *d = (uint8_t)((*d & (uint8_t)~(1u << plane))
                                  | (uint8_t)(bit << plane));
    }
}

/* IMAGE 0x08822, AT THE ORIGINAL'S GRANULARITY: plane, row, BYTE.
 *
 * `bp` is the flag word image 0x07C67 unpacked; `mask` may be NULL, and then
 * the shape is the ink being non-zero.
 *
 * The modes that program the sequencer's map mask to 0x0F touch all four
 * planes in one pass. The two that walk it 0x01, 0x02, 0x04, 0x08 make FOUR,
 * re-reading the destination each time - and that is not decoration. Image
 * 0x08B42's no-overwrite computes its bit mask from the destination's plane 3
 * on every pass, so once plane 3 has been written the mask would change for
 * any plane after it. Plane 3 is last, so it does not; a per-pixel rule gets
 * the same answer by assuming the thing the pass ORDER guarantees. */
void blit_planar(uint8_t *dst, int32_t dst_w, int32_t dst_h,
                 int32_t at_x, int32_t at_y,
                 const uint8_t *src, const uint8_t *mask,
                 uint8_t w, uint8_t rows, uint8_t flip, uint16_t bp)
{
    uint32_t stride = ((uint32_t)w + 7u) / 8u;
    uint32_t plane  = stride * (uint32_t)rows;
    blit_path path  = blit_path_of(bp);          /* image 0x088EA..0x0890E */
    uint8_t shift   = (uint8_t)(at_x & 7);       /* ds:0xA948 */
    int32_t base_x  = at_x - (int32_t)shift;     /* image 0x08868's bx */
    /* image 0x08BA1's scratch at ds:0x6706, and image 0x090E4's at ds:0x6748.
     * The widest piece in any shipped set is far inside this. */
    uint8_t ink_row[64], mask_row[64];
    uint32_t bytes = stride;
    uint8_t sy, p, b;

    if (bytes + 1u > sizeof ink_row) return;

    for (sy = 0; sy < rows; sy++) {
        int32_t dy = at_y + (int32_t)sy;
        uint8_t row = flip ? (uint8_t)(rows - 1u - sy) : sy;
        if (dy < 0 || dy >= dst_h) continue;

        if (mask)
            blit_shift_row(mask_row, mask + (uint32_t)row * stride,
                           bytes, shift);

        switch (path) {
        case BLIT_ERASE:          /* image 0x0909C - map mask 0x0F, ONE pass */
        case BLIT_MODE_20:        /* image 0x08FF7 - the same, Set/Reset 0x0F */
            /* image 0x090BF and image 0x09013: Enable Set/Reset is bp's 0x0F00
             * nibble, so every plane comes from Set/Reset - zero for the
             * erase, ones for the other - and GC 8 is the SHAPE. */
            if (!mask) break;
            for (b = 0; b <= bytes; b++) {
                uint8_t v = (path == BLIT_ERASE) ? 0x00u : 0x0Fu;
                for (p = 0; p < 4; p++)
                    blit_plane_byte(dst, dst_w, dst_h,
                                    base_x + (int32_t)b * 8, dy, p,
                                    ((v >> p) & 1u) ? 0xFFu : 0x00u,
                                    mask_row[b], 0);
            }
            break;

        case BLIT_SETRESET_ZERO:  /* image 0x08D66 - GC 1 = 0x0E, one pass */
            /* Planes 1..3 come from Set/Reset, which is zero; only plane 0
             * comes from the source. It can write nothing but 0 or 1, which is
             * why an arrow drawn this way is not terrain afterwards. */
            blit_shift_row(ink_row, src + (uint32_t)row * stride, bytes, shift);
            for (b = 0; b <= bytes; b++) {
                uint8_t m = mask ? mask_row[b] : ink_row[b];
                blit_plane_byte(dst, dst_w, dst_h, base_x + (int32_t)b * 8,
                                dy, 0, ink_row[b], m, 0);
                for (p = 1; p < 4; p++)
                    blit_plane_byte(dst, dst_w, dst_h,
                                    base_x + (int32_t)b * 8, dy, p,
                                    0x00, m, 0);
            }
            break;

        case BLIT_NO_OVERWRITE:   /* image 0x08B42 - FOUR passes */
        case BLIT_SPRITE:         /* image 0x0890E - four passes as well */
        default:
            for (p = 0; p < 4; p++) {          /* image 0x08BF9, image 0x08C07 */
                blit_shift_row(ink_row,
                               src + (uint32_t)p * plane
                                   + (uint32_t)row * stride,
                               bytes, shift);
                for (b = 0; b <= bytes; b++) {
                    int32_t bx = base_x + (int32_t)b * 8;
                    uint8_t m = mask ? mask_row[b] : ink_row[b];
                    if (path == BLIT_NO_OVERWRITE) {
                        /* image 0x08BD2 reads the destination through read
                         * mode 1 with compare 8 - "already solid" - and image
                         * 0x08BD5 inverts it into GC 8. GC 3 = 0x10 is OR. It
                         * never consults the source's shape: OR with a
                         * transparent pixel's zero bits is a no-op. */
                        uint8_t gc = 0, k;
                        for (k = 0; k < 8; k++) {
                            int32_t dx = bx + (int32_t)k;
                            if (dx < 0 || dx >= dst_w) continue;
                            if (!(dst[(uint32_t)dy * (uint32_t)dst_w
                                      + (uint32_t)dx] & 0x08u))
                                gc |= (uint8_t)(0x80u >> k);
                        }
                        blit_plane_byte(dst, dst_w, dst_h, bx, dy, p,
                                        ink_row[b], gc, 1);
                    } else if (p == 0) {
                        /* image 0x0890E's FIRST pass: masked through GC 8, and
                         * it clears planes 1..3 as it goes. */
                        uint8_t q;
                        blit_plane_byte(dst, dst_w, dst_h, bx, dy, 0,
                                        ink_row[b], m, 0);
                        for (q = 1; q < 4; q++)
                            blit_plane_byte(dst, dst_w, dst_h, bx, dy, q,
                                            0x00, m, 0);
                    } else {
                        /* and the other three OR their plane in UNMASKED. */
                        blit_plane_byte(dst, dst_w, dst_h, bx, dy, p,
                                        ink_row[b], 0xFF, 1);
                    }
                }
            }
            break;
        }
    }
}

/* TRANSCRIBED from image 0x03752's loop and image 0x07C67's blit set-up, and
 * what remains owed is named at the end - the same one thing draw_preview and
 * menu_briefing_screen owe.
 *
 * The loop is image 0x03752..0x03768: four bytes at a time from a table at
 * ds:0x10EB, stopping on the first WORD being 0xFFFF, each entry handed to
 * image 0x07C67.
 *
 * THE UNPACK, which level_parse carries with these addresses on it and which
 * agrees to the instruction - the x, the y and all three flags:
 *
 *     0x07C6C  dx = (ax >> 7) & 0xC0   AX bit 14 -> bp 7, bit 13 -> bp 6
 *     0x07C72  dx |= 0x0F              the low nibble, always
 *     0x07C75  test dx, 0x40 -> 0x07C7B  dx |= 0x0F00, the erase nibble
 *     0x07C7F  rcl ax,1 -> 0x07C83     AX bit 15 -> bp 15, no-overwrite
 *     0x07C8D  dx = bx & 0x3F          the piece's ID
 *     0x07C90  sar bx, 7 then sub bx,4 the Y, biased by four
 *     0x07C95  and ax, 0x1FFF          the X
 *
 * AND THE BLIT'S OWN PARAMETERS, which were never read until now:
 *
 *     0x07C9A  ax = 8 * id + 0x1D0B    an EIGHT-BYTE definition per piece,
 *                                      in a table at ds:0x1D0B that the load
 *                                      builds from GROUNDxO.DAT
 *     0x07CA4  dl = [bx]               the width
 *     0x07CA8  cl = [bx+1]             the rows
 *     0x07CAB  si = [bx+2]             the graphic
 *     0x07CAE  di = [bx+4]             the MASK, a pointer of its own
 *     0x07CB3  ch = 4                  four planes
 *     0x07CB5  lcall 1010:8822
 *
 * The port reads the same four numbers out of the ground set rather than out
 * of a prepared table; where that table is built is not read, and it is the
 * same shape as image 0x03981's parallel array for objects.
 *
 * THE ROUTE IS NO LONGER OWED. Image 0x07CB5's far call is blit_planar above:
 * all five of image 0x08822's modes are written out and the dispatch between
 * them is image 0x088EA's, so this routine now does what image 0x03752's loop
 * does - unpack an entry and hand it to the blitter - and decides nothing
 * about how a pixel combines.
 *
 * What it cost to leave owed is worth recording. In its place stood seven
 * candidate rules chosen by a flag, of which the fitted winner was the
 * default; it scored 100.00% here and was still a guess, and the one candidate
 * actually READ off the blitter scored 95.04% because it applied a single mode
 * where the original picks one per piece. The transcription renders this level
 * byte-identically to the fitted winner, and 120 frames of a driven run
 * byte-identically too - which is what makes it a replacement rather than a
 * change.
 *
 * The MASK is used now as well. Image 0x07CAE loads a pointer of its own and
 * the port had been testing bit 3 of the assembled pixel instead - an
 * equivalence that held on every level scored here and was never read.
 *
 * tools/prove.sh compares what the original COMPOSES against what this
 * composes: 49,741 pixels, zero differing. */
int render_terrain(canvas *c, const level *lv, const ground *g,
                   const uint8_t *sec, uint32_t sec_len, const char **err)
{
    for (uint16_t k = 0; k < lv->num_terrain; k++) {
        uint16_t i = g_terrain_reverse
                   ? (uint16_t)(lv->num_terrain - 1u - k) : k;
        const level_terrain *t = &lv->terrain[i];
        const ground_terrain *gt = &g->terrain[t->id];
        uint32_t stride, plane;

        if (!gt->width || !gt->height)
            continue;               /* a set need not define all 64 ids */

        stride = ((uint32_t)gt->width + 7u) / 8u;
        plane  = stride * (uint32_t)gt->height;
        if (gt->image_loc + plane * 4u > sec_len
                || gt->mask_loc + plane > sec_len) {
            if (err) *err = "terrain bitmap runs past the end of the section";
            return -1;
        }

        /* image 0x07CAB is the graphic and image 0x07CAE the MASK - a pointer
         * of its own, which the port used to ignore in favour of testing bit 3
         * of the assembled pixel. That equivalence held on every level scored
         * here and it was still a guess. */
        blit_planar(c->pix, c->w, c->h, t->x, t->y,
                    sec + gt->image_loc, sec + gt->mask_loc,
                    gt->width, gt->height, t->upside_down,
                    terrain_bp(t));                  /* image 0x07CB5 */
    }
    return 0;
}

/* ======================================================================
 * THE FOUR VGASPEC LEVELS - one whole picture as the terrain.
 *
 * Image 0x03742 tests ds:0x0FE8, the header's extended graphics set, and a
 * level that has one never reaches the piece loop at image 0x0374F: image
 * 0x03749 calls image 0x01A7B and jumps straight to image 0x0376A, where the
 * ordinary composition also ends. Image 0x0368E has already cleared the
 * master by then, so the picture goes onto an empty 1664 x 164.
 *
 * Its source is section 0 of vgaspecN.dat, which image 0x01A5A unpacked into
 * ds:0x1FF4 at +0x10 - so image 0x01A7B's `si = 0x10` is the section's first
 * byte. The section is
 *
 *     +0x00  eight VGA colours, three bytes each
 *     +0x18  eight EGA attributes          +0x20  eight more
 *     +0x28  four strips, each run-length coded by image 0x01B42
 *
 * and all four files here are exactly that: four strips of 14,400 bytes
 * apiece, and nothing left over in any of them.
 * ====================================================================== */
#define SPEC_PAL        0x00    /* image 0x01A83: si = 0x10, less the 0x10 */
#define SPEC_DATA       0x28    /* image 0x01AC5: si = 0x38, less the 0x10 */
#define SPEC_STRIPS     4       /* images 0x08238, 0x08243, 0x08249, 0x0824F */
#define SPEC_ROWS       40      /* image 0x082C2: ch = 0x28 */
#define SPEC_ROW_BYTES  120     /* image 0x082C2: cl = 0x78 - 960 pixels */
#define SPEC_PLANE      (SPEC_ROWS * SPEC_ROW_BYTES)    /* 0x12C0, image 0x08262 */
#define SPEC_PLANES     3       /* image 0x08262 and 0x08267: three ORed */
#define SPEC_AT_X       (0x28 * 8 - 16)  /* image 0x0827D: byte 0x28 of the
                                         * master, less the canvas's sixteen */

/* IMAGE 0x01B42 - the strips' run-length code. A byte of 0x80 ends the
 * strip; below it, n copies the next n + 1 bytes; above it, the next byte is
 * repeated 257 - n times (`neg al / inc al`). Image 0x01B53's segment step
 * every 64K is the 8086's way of writing more than one segment and changes
 * no byte. Returns the bytes written, or -1 if the source runs out or the
 * output would overflow - the original checks neither. */
static int32_t vgaspec_unpack(const uint8_t *src, uint32_t len, uint32_t *pos,
                              uint8_t *out, uint32_t cap)
{
    uint32_t n = 0;
    for (;;) {
        if (*pos >= len) return -1;
        uint8_t b = src[(*pos)++];                  /* image 0x01B42 */
        if (b == 0x80) return (int32_t)n;           /* image 0x01B45 */
        if (b < 0x80) {                             /* image 0x01B62 */
            uint16_t k = (uint16_t)(b + 1);
            if (*pos + k > len || n + k > cap) return -1;
            memcpy(out + n, src + *pos, k);         /* image 0x01B68 */
            *pos += k;
            n += k;
        } else {                                    /* image 0x01B49 */
            uint16_t k = (uint16_t)(257 - b);
            if (*pos >= len || n + k > cap) return -1;
            memset(out + n, src[(*pos)++], k);      /* image 0x01B51 */
            n += k;
        }
    }
}

/* IMAGE 0x01A7B - the palettes, then the four strips.
 *
 * THE PALETTES. Image 0x01A8D copies the eight VGA colours over ds:0x1F23,
 * the level's custom eight - palette entries 8..15 here - and image 0x01A90
 * then writes 1F 1F 00 over the first of them. Image 0x01AA4 copies the same
 * eight, unaltered, to ds:0x1F53, which is ds:0x1F3B's entries 8..15: the
 * briefing's preview palette. Entry 7 follows entry 8 as it always does,
 * because image 0x053A1 copies custom[0] there when the palette goes up. The
 * EGA attributes at image 0x01AB0 and image 0x01ABA are the EGA path's, and
 * that half of the program is a non-goal - see image 0x05425.
 *
 * THE STRIPS. Each goes through image 0x01B42 into ds:0x1FF0 at 0x8000 and
 * then to one of four entries that differ only in bx, the strip's first row
 * - 0, 0x28, 0x50, 0x78 - before image 0x08252. That ORs the three planes
 * into a fourth at image 0x0825F, and writes the strip into the master at
 * row bx, byte 0x28, 120 bytes by 40 rows: plane 0 at image 0x082C2 with
 * Set/Reset clearing planes 1..3, then planes 1 and 2 through the map mask,
 * then the OR as plane 3. So a pixel is its three bits as a colour 0..7,
 * with the solidity bit set wherever that colour is not 0 - attribute
 * `c ? 8 | c : 0`, and every pixel of the 960 x 160 is written.
 *
 * Image 0x01B19's reload of vgagrN.dat is the OBJECTS - the strips used
 * ds:0x1FF0 as scratch, and that is where the objects' graphics live. The
 * caller loads that file for them; nothing here needs it. */
int render_vgaspec(canvas *c, ground *g, const uint8_t *sec, uint32_t len,
                   const char **err)
{
    if (len < SPEC_DATA) {
        if (err) *err = "VGASPEC section shorter than its palettes";
        return -1;
    }
    for (uint8_t i = 0; i < 8; i++) {
        lem_rgb v = { widen6(sec[SPEC_PAL + i * 3 + 0]),
                      widen6(sec[SPEC_PAL + i * 3 + 1]),
                      widen6(sec[SPEC_PAL + i * 3 + 2]) };
        g->palette[8 + i] = v;                      /* image 0x01A8D */
        g->preview[i] = v;                          /* image 0x01AA4 */
    }
    g->palette[8].r = widen6(0x1F);                 /* image 0x01A90 */
    g->palette[8].g = widen6(0x1F);
    g->palette[8].b = widen6(0x00);
    g->palette[7] = g->palette[8];                  /* image 0x053AF */

    static uint8_t strip[SPEC_PLANES * SPEC_PLANE];
    uint32_t pos = SPEC_DATA;
    for (uint8_t s = 0; s < SPEC_STRIPS; s++) {
        int32_t got = vgaspec_unpack(sec, len, &pos, strip, sizeof strip);
        if (got != (int32_t)sizeof strip) {
            if (err) *err = "VGASPEC strip is not three planes of 40 x 120";
            return -1;
        }
        uint16_t y0 = (uint16_t)(s * SPEC_ROWS);    /* image 0x08272 */
        for (uint16_t r = 0; r < SPEC_ROWS; r++) {
            uint16_t y = (uint16_t)(y0 + r);
            if (y >= c->h) break;
            for (uint16_t b = 0; b < SPEC_ROW_BYTES; b++) {
                uint32_t at = (uint32_t)r * SPEC_ROW_BYTES + b;
                uint8_t p0 = strip[at];
                uint8_t p1 = strip[SPEC_PLANE + at];
                uint8_t p2 = strip[2 * SPEC_PLANE + at];
                for (uint8_t k = 0; k < 8; k++) {
                    uint16_t x = (uint16_t)(SPEC_AT_X + b * 8 + k);
                    if (x >= c->w) break;
                    uint8_t m = (uint8_t)(0x80u >> k);
                    uint8_t v = (uint8_t)(((p0 & m) ? 1 : 0)
                                        | ((p1 & m) ? 2 : 0)
                                        | ((p2 & m) ? 4 : 0));
                    c->pix[(size_t)y * c->w + x] =
                        v ? (uint8_t)(v | 8) : 0;   /* image 0x0825F */
                }
            }
        }
    }
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

/* ds:0x82 AND ds:0x83 - WHICH SKILL, AND WHICH BOX.
 *
 * Image 0x00422 starts ds:0x82 at 2 and image 0x00427 starts ds:0x83 at 3, in
 * the level's own set-up at image 0x003E7. They are the game's state and were
 * living in sdl_io.c's per-frame struct, which is where a window's scroll
 * position belongs and a selected skill does not: every level began with
 * skill 0 there, where the game begins with 2.
 *
 * Image 0x03F66 writes ds:0x82 when a button is chosen, by click or by key. */

uint8_t level_skill(void)    { return g_skill; }
uint8_t level_selected(void) { return g_selected; }

/* THE SKILL STRIP'S DIVIDER - ds:0x84, and it does two jobs.
 *
 *     0x03B3F  cmp ds:0x84, 2
 *     0x03B44  jne 0x03B53
 *     0x03B46    ds:0x83 = ds:0x82     the DRAWN skill follows the selected
 *     0x03B4E    ds:0x84 = 0xFF
 *     0x03B53  inc ds:0x84
 *
 * So it is 0 on one frame in three, and image 0x03F15, image 0x03F5C, image
 * 0x040B5 and image 0x040CD all refuse to act on any other - which is what
 * stops a HELD key or button running through the skills. Image 0x03A1B sets
 * it to 0 when the panel starts.
 *
 * AND THE HIGHLIGHT LAGS THE SELECTION, up to two frames of it, because
 * ds:0x83 is only copied when the divider fires. The port set both at once,
 * which is the tidier reading and not the original's. */

uint8_t level_skill_div(void) { return g_skill_div; }

/* image 0x03B3F, from the panel's own per-frame update.
 *
 * IT DOES NOT MOVE THE HIGHLIGHT, and a version of this that did was shipped
 * and felt like a lag on every skill click. Read the four instructions before
 * it: image 0x03B33 loads ds:0x82 and draws the box at image 0x0780D, then
 * image 0x03B39 loads ds:0x83 and ERASES at image 0x0789E. ds:0x83 is the
 * PREVIOUSLY DRAWN button, kept so the old box can be rubbed out; the new one
 * goes down from ds:0x82 at once. All image 0x03B46 does is let ds:0x83 catch
 * up afterwards, one frame in three.
 *
 * This port has no counterpart for it: it recomposes the whole panel every
 * frame, so there is no old box to erase and nothing to remember. The counter
 * itself is kept because image 0x03F15 and image 0x040B5 gate on it. */
void level_skill_div_step(void)
{
    /* IMAGE 0x03B29 AND 0x03B2D GATE THE WHOLE BLOCK, and the port had neither.
     *
     *     0x03B29  ah = ds:0x82            the selected skill
     *     0x03B2D  cmp ds:0x83, ah         against the DRAWN one
     *     0x03B31  je 0x03B57              the same: skip all of it
     *     0x03B33  draw the box at ds:0x82
     *     0x03B39  erase the box at ds:0x83
     *     0x03B3F  ds:0x84 == 2 ->
     *     0x03B46     ds:0x83 = ds:0x82    what is drawn is now recorded
     *     0x03B4E     ds:0x84 = 0xFF
     *     0x03B53  inc ds:0x84
     *
     * So the divider only runs WHILE A CHANGE IS PENDING - in the steady
     * state, where the drawn skill already is the selected one, ds:0x84 does
     * not move at all. The port advanced it on every frame, which is why the
     * comparison against the original's own byte differed on 268 of 400
     * frames, and it never copied ds:0x82 into ds:0x83 at all, which is why
     * ds:0x83 sat at PANEL_SEL_START while the original's followed.
     *
     * This is also why an earlier attempt at the copy "felt like a lag on
     * every skill click": without image 0x03B2D's gate the divider is running
     * all the time, so a click waits for wherever in the cycle it happens to
     * land. Gated, the divider starts from rest when the change arrives. */
    if (g_selected == g_skill) return;              /* image 0x03B31 */
    if (g_skill_div == 2) {                         /* image 0x03B3F */
        g_selected = g_skill;                       /* image 0x03B46 */
        g_skill_div = 0xFF;                         /* image 0x03B4E */
    }
    g_skill_div++;                                  /* image 0x03B53 */
}

/* image 0x03F66, reached by a click at image 0x03F27 and by a function key
 * at image 0x03EA0 - the same store either way.
 *
 * The box moves with it, IMMEDIATELY: image 0x03B33 draws from ds:0x82 on
 * the very next panel pass. The `+ 1` is the port's - PANEL_SEL_X0 is 16
 * where the numbers start at PANEL_ART_X, 32, so the box for button k is
 * drawn at k + 1. */
void level_skill_select(uint8_t button)
{
    /* IMAGE 0x03F66 STORES ONE BYTE. This also set ds:0x83, which is not what
     * that byte is: image 0x03B33 draws the box from ds:0x82 and image 0x03B39
     * ERASES the old one from ds:0x83, so ds:0x83 is what was drawn LAST and
     * image 0x03B46 lets it catch up one frame in three. Setting it here made
     * the two bytes one, and the port then had no ds:0x83 to compare. */
    g_skill = button;                              /* image 0x03F66 */
}

/* IMAGE 0x040B5 AND IMAGE 0x040CD - Z AND X, AND THEY ARE UNREACHABLE.
 *
 * NOT WIRED TO A KEY, deliberately, and the reason is a THREE-INSTRUCTION
 * GAP rather than anything about these routines.
 *
 * ds:0x1FD0 does hold a scancode: image 0x01707 is `mov [0x1FD0], ah` on a
 * press and image 0x01705 branches to clear it on the release. Two routines
 * read it, and WHERE THEY ARE CALLED FROM decides everything:
 *
 *   image 0x03E6F  the F-keys - called at image 0x01874, inside the TIMER
 *                  ISR (image 0x017C7, on INT 8), on every tick. It sees
 *                  the key whenever a tick falls between the press and the
 *                  next frame's clear. They work.
 *   image 0x04085  these - called at image 0x001A0, from the frame loop, and
 *                  image 0x00194 SETS ds:0x1FD0 TO 0 three instructions
 *                  earlier. It can only ever see zero.
 *
 * So `cmp al, 0x2C` never matches in practice and Z and X do nothing, which
 * is what a player found in DOSBox. What image 0x04085 does reach is its own
 * fall-through at image 0x04096 - the edge scroll - and that works.
 *
 * IT IS KEPT, transcribed and unreachable, because that is what the original
 * contains and the next reader will find image 0x04085 and have the same
 * idea. Wiring it up adds a feature the game does not have.
 *
 * THE LESSON IS MINE, TWICE. First I transcribed a dispatch without asking
 * whether it RUNS, wrote tests that drove my own function directly, watched
 * them pass, and shipped a key the game never had - a test that calls a
 * routine proves its arithmetic and nothing about its reachability.
 *
 * Then, told it did not work, I searched the image for writes to ds:0x1FD0
 * with three opcode forms, found only stores of 0, and concluded the byte was
 * never set. I had missed `mov [mem], ah` - opcode 88 26 - which is the one
 * that stores it. The conclusion happened to be right and the reason was
 * wrong, which is worse than being wrong outright: it would have been written
 * down as settled. Searching for the ADDRESS and decoding what surrounds it
 * finds every form; searching for the opcodes you thought of finds the ones
 * you thought of.
 *
 *     0x040B5  cmp ds:0x84, 0 / jne     the divider, so a held key repeats
 *     0x040BC  dec ds:0x82              one back
 *     0x040C0  cmp ds:0x82, 1 / jne
 *     0x040C7  ds:0x82 = 9              ... wrapping 1 to 9
 *
 *     0x040CD  the same upward: image 0x040D4 increments and image 0x040D8
 *              wraps 10 to 2
 *
 * The eight skills are buttons 2..9, so the wrap is off the ends of that run.
 * Image 0x04085 reaches both from the scancodes 0x2C and 0x2D.
 *
 * The port had neither: ds:0x82 was set from the panel click alone, so the
 * keyboard selected nothing. */
void level_skill_key(uint8_t up)
{
    if (g_skill_div != 0) return;                  /* image 0x040B5 */
    if (up) {
        g_skill++;                                 /* image 0x040D4 */
        if (g_skill == 0x0A) g_skill = 2;          /* image 0x040D8 */
    } else {
        g_skill--;                                 /* image 0x040BC */
        if (g_skill == 1) g_skill = 9;             /* image 0x040C0 */
    }
}

/* image 0x02102's box, from the block comment above. */
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

const char *level_status_line(void) { return g_status; }

/* Image 0x0418 and 0x041D: ds:0x85 and ds:0x86 both start at 0x11. The same
 * number paces the display - see LEVEL_CLOCK_FRAMES in game.h. */
#define LEVEL_FRAMES_PER_SECOND LEVEL_CLOCK_FRAMES

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
    level_scan_keep(&sc);
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
    /* THE CLOCK STOPS WHEN THE LEVEL ENDS. Image 0x00213 comes after the end
     * tests: image 0x001C8 jumps to image 0x002C3 once ds:0x4F is set, and
     * image 0x001E7 / 0x001FA jump to image 0x002B3 - which sets it - in the
     * pass that ends the level, so neither reaches the decrement. This runs
     * at the top of the NEXT pass (level_frame_step), by which time
     * level_end_begin has set ds:0x4F in both cases. Without it the panel's
     * clock ran on through the fade; a played level to its exit showed it,
     * ds:0x85 8 against 7 on the frame after the last lemming went home. */
    if (g_level_over == 0xFF) return;      /* image 0x001C8, 0x001E7 */
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
 * Not the same routine as the briefing's put_num3 below, which is still
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
 * NOTHING RESETS ds:0x2B OR ds:0x53 BY NAME, and this block used to conclude
 * from that "leaving a level paused leaves the game paused - the original's
 * behaviour, transcribed rather than tidied". That was wrong, and wrong in
 * the direction that excuses a bug: image 0x04551 clears BOTH, because they
 * are inside the 0xFCA bytes it wipes at every level entry.
 *
 * The reading was a search for a reset OF THIS VARIABLE, and the original
 * does not reset variables one at a time - it erases the whole of ds:0..0xFC9
 * and lets the level fill it in again. level_state_wipe is that, and a level
 * now starts unpaused because the original starts it unpaused.
 * ====================================================================== */

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
    /* Image 0x03FF6 takes the key, so F12 held down is ONE press: image
     * 0x03E6F runs every tick and would otherwise find it there five times
     * a frame. */
    g_key_last = 0;                              /* image 0x03FF6 */
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
    if (!g_nuke) {                               /* image 0x01FF8 */
        g_walking = 0;                           /* image 0x01FFF */
        return;
    }
    for (;;) {
        if (g_nuke_at == g_released) {           /* image 0x02008 */
            g_nuke = 0;                          /* image 0x0203C */
            g_walking = 0;                       /* image 0x02041 */
            return;
        }
        if (g_nuke_at >= LEMMING_MAX) { g_nuke = 0; g_walking = 0; return; }
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

/* ds:0x68 - THE RATE THE PANEL IS SHOWING, and it is a VALUE.
 *
 * This was a flag - "does it match ds:0x67" - because only that question is
 * ever asked of it. The reading is sound and the model is not: image 0x03A25
 * puts 0xFF in it when the panel starts, which is not any rate, so the panel
 * begins with a redraw PENDING and image 0x03B8C's divider running. A flag
 * cannot hold 0xFF; forcing it to "pending" instead moved the cursor and the
 * panel on the very first frames, because the flag was also standing in for
 * things the byte does not decide.
 *
 * As a value it needs no such forcing: image 0x03B86 compares it with ds:0x67
 * and image 0x03B93 copies. A click changes ds:0x67 and the two differ by
 * themselves. */
static uint8_t g_rate_shown = 0xFF;   /* image 0x03A25 */

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
 * turned by level_rate_tick below from the panel's redraw.
 *
 * WHO CALLS IT, AND HOW OFTEN, IS THE WHOLE STORY of what one click does.
 * Image 0x03E6F runs on every TIMER TICK, five to a frame, and while the
 * button is down every tick comes here - so a click steps the rate once per
 * tick until the panel's redraw turns the divider. The port used to step
 * once on the press and once more from a held-button repeat, +2 a click,
 * where the original in the emulator made +3 and on a fast machine +5. See
 * level_wait_ticks. */
/* IMAGE 0x0403D AND IMAGE 0x04068 - the plus and the minus, and they are
 * ENTRIES as well as arms: image 0x03E7D and image 0x03E76 jump straight to
 * them for F2 and F1, past image 0x04032's pause test, so the keys move the
 * rate while the game is paused and the buttons do not.
 *
 * Every way out is image 0x03FE9, which sets the press latch - a step, a
 * bound, or the divider saying no. */
static uint8_t rate_step(uint8_t up)
{
    g_press_latch = 0xFF;                       /* image 0x03FE9 */
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
    /* Nothing to set: changing ds:0x67 is what makes them differ, which
     * is image 0x03B86's whole test. */
    return 1;
}

/* Image 0x04032 itself: the pause test, then image 0x04039's `and al,al`
 * picks the arm. A refusal here leaves by image 0x03FEE, which does not
 * touch the latch. */
uint8_t level_rate_click(uint8_t up)
{
    if (level_paused() == 0xFF) return 0;       /* image 0x04032 */
    return rate_step(up);                       /* image 0x04039 */
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
    if (g_rate_shown == g_rate) return;         /* image 0x03B86 */
    if (g_rate_div == 2) {                      /* image 0x03B8C */
        g_rate_shown = g_rate;                  /* image 0x03B93 */
        g_rate_div = 0xFF;                      /* image 0x03B97 */
    }
    g_rate_div++;                               /* image 0x03B9C */
}

/* Image 0x039EE clears ds:0x69 when the level starts, and image 0x03A25 puts
 * 0xFF in ds:0x68 - which is not any rate, so the panel starts with a redraw
 * PENDING and the divider running. This set the flag to "matches" instead, so
 * image 0x03B86's gate returned at once and ds:0x69 never moved: the original
 * held 2 at the first captured frame where the port held 0. */
void level_rate_div_reset(void) { g_rate_div = 0; g_rate_shown = 0xFF; }

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
 * WHEN it starts was ALREADY transcribed, further up in level_phase_step -
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

uint8_t sound_enabled(uint8_t bits)
{
    return (uint8_t)((g_snd_bits & bits) != 0);
}

/* ======================================================================
 * THE GAME'S OWN TIMER - image 0x017C7, installed on INT 8 at image 0x01646.
 *
 * Everything the game paces comes from here, and the handler is paced by the
 * DISPLAY: image 0x017FE waits for a vertical retrace before image 0x01815
 * reloads the PIT from ds:0x1FC9. A tick is a retrace.
 * ====================================================================== */

/* image 0x017C7. The port has none of the handler's video work - the page
 * flip at image 0x017E5 and the PIT reload at image 0x01815 are things the
 * CRTC and the 8253 did, and sdl_io.c owns their replacements - so what is
 * left is what the handler does to the GAME. */
void timer_isr(void)
{
    /* IMAGE 0x01861 - AND THIS GATE IS THE ONLY THING THAT EVER STOPS THE
     * MUSIC ON A SETTING.
     *
     *     0x01861  cmp byte ptr [0x2010], 0xff    a driver answered
     *     0x01866  jne 0x1874
     *     0x01868  test byte ptr [0x25a7], 3      the settings want sound
     *     0x0186d  je 0x1874
     *     0x0186f  mov ax, 0
     *     0x01872  int 0x61                       the driver's TICK
     *
     * The port ticked unconditionally, and that is a bug the pull model made
     * audible: with the device asking for samples for ever, a driver nothing
     * gates is SEQUENCED for ever. Under the old push model a screen that did
     * not pump stopped it by accident.
     *
     * The pair is the same pair image 0x002FE's stop is guarded on, and that
     * is not a coincidence - it is why the original can afford to skip the
     * stop. F3 issues NO request at all: image 0x044B3 writes ds:0x25A7 and
     * returns, so with sound turned off the level's end asks for nothing and
     * this test is what silences the tune. Transcribe one guard without the
     * other and the game has no way to stop playing.
     *
     * image 0x0185F's INT 60h twin runs on odd ticks only (image 0x0184A's
     * `and al,1`) and tests bit 0 alone - it is the fallback device, which
     * this port does not have. */
    if (g_snd_driver == 0xFF && sound_enabled(3))   /* image 0x01861, 0x01868 */
        sound_tick_now();                           /* image 0x01872 */
    g_tick_pending++;               /* image 0x018A3 */
}

/* image 0x00186's `cmp al, 4 / jle` and image 0x0018F's clear. FIVE ticks to
 * a frame, which with a 70 Hz retrace is fourteen frames a second - not the
 * seventeen this port used to pace at, and the difference is why its clock
 * ran 1.22x fast against DOSBox. */
int timer_frame_due(void)
{
    if (g_tick_pending <= 4) return 0;      /* image 0x0018B */
    g_tick_pending = 0;                     /* image 0x0018F */
    return 1;
}

/* ds:0x2010, which image 0x005A4 sets when the detect answers. THE PORT'S
 * OWN only in how it learns the answer - sdl_io.c asks the driver whether it
 * loaded, where the original asked the card. */
void sound_driver_present(uint8_t yes)
{
    g_snd_driver = yes ? 0xFF : 0;     /* image 0x005A4 */
    g_snd_bits   = yes ? 3 : 1;        /* image 0x005A9 against image 0x00591 */
    g_snd_setting = yes ? 2 : 1;       /* image 0x005AE against image 0x00596 */
}

/* THE MENU'S F3, image 0x044B3 - the sound settings, cycled.
 *
 *     0x044BF  al = ds:0x1F71 + 1        the setting, one on
 *     0x044C4  ah = 3
 *     0x044C6  ...unless ds:0x2010 says no driver answered, then ah = 2
 *     0x044D1  al == ah, so wrap to 0
 *     0x044E0  al = the table at image 0x044E9
 *     0x044E3  ds:0x25A7 = al
 *
 * The table is three bytes, 00 01 03, and they are the two gate bits this
 * port already tests everywhere:
 *
 *     0  0x00   silent
 *     1  0x01   effects only - bit 0
 *     2  0x03   effects and music - bits 0 and 1
 *
 * Which is why the count is three with a driver and two without: setting 2
 * is the music, and there is nothing to play it on. Image 0x00596 starts at
 * 1 and image 0x005AE raises it to 2 when the detect answers, so a machine
 * with an AdLib begins with everything on and F3 takes things AWAY.
 */
void sound_settings_cycle(void)
{
    /* IMAGE 0x044B3 - AND IT IS A REFUSAL, not the inert arm this used to
     * call it. ds:0x1F72 holding 2 means the plate was changed last pass and
     * still owes a second draw, and F3 is DROPPED for that pass: the handler
     * jumps straight back to image 0x0445B without touching anything.
     *
     * The port had no ds:0x1F72 to ask, so it said so and moved on. It has
     * one now - the plate redraws itself from its own dirty byte the way
     * image 0x04E37 does - so the guard is a guard.
     *
     * This is a one-frame debounce and a player can feel it: held or
     * hammered, F3 advances one step every two passes rather than one every
     * pass. Both rating keys have exactly the same guard on their own plate,
     * image 0x044EC and image 0x0451C against ds:0x1F6F. */
    if (g_fx_drawn == 2) return;                    /* image 0x044B3 */
    g_fx_dirty = (uint8_t)(g_fx_dirty ^ 0xFF);      /* image 0x044BA */
    uint8_t al = (uint8_t)(g_snd_setting + 1);      /* image 0x044BF */
    uint8_t ah = (g_snd_driver == 0xFF) ? 3 : 2;    /* image 0x044C4 */
    if (al == ah) al = 0;                           /* image 0x044D3 */
    g_snd_setting = al;                             /* image 0x044D5 */
    static const uint8_t BITS[3] = { 0x00, 0x01, 0x03 };  /* image 0x044E9 */
    g_snd_bits = BITS[al % 3];                      /* image 0x044E3 */
}

/* THE PORT'S OWN - so a caller can show which setting is up. */
uint8_t sound_setting(void) { return g_snd_setting; }

/* THE PORT'S OWN, and only for the harness - see game.h.
 *
 * It exists because of a REAL DISAGREEMENT it turned up. The menu's FX label
 * is chosen by image 0x04E5A from ds:0x25A7 AND ds:0x2010, so a guest whose
 * AdLib detect answered draws a different sprite from a port with no driver
 * loaded - 728 pixels of one label, scored as the port's for as long as the
 * emulator has been answering that detect. Neither side was wrong; they were
 * in different states, and nothing said so. */
void sound_state_set(uint8_t bits, uint8_t driver)
{
    g_snd_bits   = bits;                /* ds:0x25A7 */
    g_snd_driver = driver ? 0xFF : 0;   /* ds:0x2010 */
}

/* ds:0x2010, which image 0x04E65 tests before choosing the label. */
uint8_t sound_driver_answered(void) { return g_snd_driver == 0xFF; }

/* ======================================================================
 * THE LEVEL'S PHASE MACHINE - image 0x0031E, called from the play loop's
 * first line at image 0x0019D.
 *
 * THREE bits of ds:0x4E, tested in that order, and the port had the third:
 *
 *     test al,1 / jne        bit 0: the FADE IN, image 0x0032C
 *     test al,2 / jne        bit 1: the FADE OUT, image 0x0034D
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
 * The two fades are not a screen the level passes through on the way in and
 * on the way out - they are BRANCHES OF THE PLAY LOOP, and the loop keeps
 * running while they happen. That is what image 0x002B3 does at the end: it
 * sets ds:0x4F, ORs bit 1 into ds:0x4E and jumps back to image 0x00105,
 * rather than leaving. The port ran its own fade after the loop had already
 * exited, which is the same picture assembled a second way.
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

/* Image 0x0436..0x0447 clears these when a level starts; bit 2 of ds:0x4E is
 * what makes the opening sequence run at all. */
/* IMAGE 0x003E7 - the level's whole set-up, and the port had a handful of it.
 *
 *     0x003F3  ds:0x1A   = 0        nothing under the pointer yet
 *     0x003F8  ds:0x10   = 0x96     the pointer, at (150,100)
 *     0x003FE  ds:0x12   = 0x64
 *     0x00404  ds:0x47   = 0xC8     to release, until the header says
 *     0x0040E  ds:0x20A9 = '0'      the clock's digits
 *     0x00413  ds:0x20AA = '0'
 *     0x00418  ds:0x85   = 0x11     the clock's frame counter
 *     0x0041D  ds:0x86   = 0x11
 *     0x00422  ds:0x82   = 2        the skill a click would hand out
 *     0x00427  ds:0x83   = 3        and the box drawn round it
 *     0x0042C  ds:0x4D   = 0x50     eighty pieces of debris
 *     0x00431  ds:0x4E   = 1        the opening's phase
 *     0x00436  ds:0x1FD4, ds:0x1FD3, ds:0x4F, ds:0x2A, ds:0x52, ds:0x55 = 0
 *     0x0044F  ss:0x12 for 0x42A0   the trigger map
 *
 * ds:0x4E STARTS AT 1, AND 1 IS THE FADE IN. The port started it at 4 -
 * image 0x0031E's third branch, the timed cues - because what turned bit 2
 * on had not been found, and starting at 1 would have lost the cues.
 *
 * IMAGE 0x003C1 IS WHAT TURNS IT ON, and it is the fade's own first
 * instruction: `or al, 4`. The cues are armed BY the fade in, on every pass
 * of it, and bit 0 clears when the palette stops moving (image 0x003D4).
 * That is exactly what the probe saw and could not explain - ds:0x4E at 5
 * and then 4: 5 is the fade running with the cues already armed, and 4 is
 * the first frame after it finished.
 *
 * So the port's 4 was right about where the level ends up and wrong about
 * how it gets there. It skipped the fade, and the level appeared at full
 * brightness in a single frame. */
void level_start_reset(void)
{
    g_level_frame  = 0;              /* image 0x00444: ds:0x52 */
    g_level_phase  = 1;              /* image 0x00431: ds:0x4E */
    g_level_over   = 0;              /* image 0x0043E: ds:0x4F */
    g_entrance_open = 0;             /* image 0x00447: ds:0x55 */
    g_skill    = 2;                  /* image 0x00422 */
    g_selected = PANEL_SEL_START;    /* image 0x00427 */
    g_skill_div = 0;                 /* image 0x03A1B: ds:0x84 */
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

/* Image 0x003C1 - one pass of the fade IN, and the first thing it does is
 * arm the cues. `al` is ds:0x4E on the way in and on the way out. */
static uint8_t level_fade_in_step(uint8_t al)
{
    al = (uint8_t)(al | 4);                     /* image 0x003C1 */
    if (!level_fade_notch(0))                   /* image 0x003CE: call 0x534B */
        al = (uint8_t)(al & 0xFE);              /* image 0x003D4 */
    return al;
}

/* Image 0x003D7 - and the way out, clearing bit 1 when the palette has
 * stopped moving. Note what it does NOT do: it never touches bit 2, so a
 * level that ends before play-loop frame 55 fades out with its cues still
 * armed. They are simply never reached again. */
static uint8_t level_fade_out_step(uint8_t al)
{
    if (!level_fade_notch(1))                   /* image 0x003DE: call 0x5329 */
        al = (uint8_t)(al & 0xFD);              /* image 0x003E4 */
    return al;
}

/* Transcribed from image 0x0031E, with the counter bumped where the play
 * loop bumps it - image 0x01A3, AFTER this call, not before. */
void level_phase_step(void)
{
    uint8_t al = g_level_phase;                 /* image 0x0031E */

    /* The fades hold the play loop. With a display present image 0x0032C
     * runs the whole fade here and now, a retrace and a DAC upload to a
     * pass; without one image 0x0033D takes a single step and returns, and
     * the loop outside is what repeats it. */
    if (al & 1) {                               /* image 0x00321 */
        if (video_present()) {                  /* image 0x00325: ds:0x200E */
            do {
                al = level_fade_in_step(al);    /* image 0x0032C */
                video_retrace_wait();           /* image 0x00330: call 0x1550 */
                video_palette_upload();         /* image 0x00333: call 0x553D */
            } while (al & 1);                   /* image 0x00339 */
        } else {
            al = level_fade_in_step(al);        /* image 0x0033D */
        }
    } else if (al & 2) {                        /* image 0x00342 */
        if (video_present()) {                  /* image 0x00346 */
            do {
                al = level_fade_out_step(al);   /* image 0x0034D */
                video_retrace_wait();           /* image 0x00351 */
                video_palette_upload();         /* image 0x00354 */
            } while (al & 2);                   /* image 0x0035A */
        } else {
            al = level_fade_out_step(al);       /* image 0x0035E */
        }
    }

    if (al & 4) {                               /* image 0x00361 */
        uint8_t dl = g_level_frame;             /* image 0x00365 */
        if (dl == 0x0F) {                       /* image 0x00369 */
            /* image 0x0380: THE ENTRANCE OPENING, and it is heard twenty
             * frames BEFORE the trapdoor actually opens - which is what
             * makes it sound like doors working rather than an announcement
             * that they have. This was a deliberate no-op for as long as the
             * port had no sound. */
            if (g_snd_driver == 0xFF && sound_enabled(3))  /* image 0x0387 */
                sound_request(0x61, 4, 0x03);                    /* image 0x038E */
        } else if (dl == 0x23) {
            g_entrance_open = 0xFF;         /* image 0x394 */
            /* image 0x03A7's own sound, which the port had never made */
            if (g_snd_driver == 0xFF && sound_enabled(3))  /* image 0x03A0 */
                sound_request(0x61, 4, 0x02);                    /* image 0x03A7 */
        } else if (dl == 0x37) {
            /* image 0x03AD, and note the mask: the MUSIC wants bit 1 of
             * ds:0x25A7 ALONE where the two effects above want either bit.
             * docs/sound.md recorded "music against effects" as the obvious
             * reading and NOT established for a long time; the two routines
             * using different masks is what settles it. */
            if (sound_enabled(2)) {                        /* image 0x03AD */
                sound_request(0x61, 3, g_tune);                  /* image 0x03B4 */
                sound_tick_now();                          /* image 0x03BB */
            }
        } else if (dl > 0x37) {                 /* image 0x00378 */
            al = (uint8_t)(al & 0xFB);          /* image 0x0037A */
        }
    }
    g_level_phase = al;                         /* image 0x0037C */
    g_level_frame++;                            /* image 0x001A3 */
}

/* ======================================================================
 * IMAGE 0x002B3 - THE LEVEL IS OVER, and what that means is two bytes.
 *
 *     0x002B3  ds:0x4F = 0xFF      it has ended
 *     0x002BB  ds:0x4E |= 2        arm the fade out
 *     0x002C0  jmp 0x105           BACK ROUND THE PLAY LOOP
 *
 * It does not leave. Image 0x001C8 sees ds:0x4F on the next pass and sends
 * the loop to image 0x002C3, which tests bit 1 and goes back round again
 * while the fade is still running; only once it has cleared does the game
 * reach the results. So the picture that fades is the level's own last
 * composed frame, still being drawn by the loop that composed it.
 * ====================================================================== */

void level_end_begin(void)
{
    g_level_over  = 0xFF;                           /* image 0x002B3 */
    g_level_phase = (uint8_t)(g_level_phase | 2);   /* image 0x002BB */
}

/* Image 0x002C6's test, the one that lets the loop out. */
int level_end_done(void)
{
    return g_level_over == 0xFF && !(g_level_phase & 2);
}

/* IMAGE 0x002CD - what happens once it has. The level is over, the fade has
 * finished, and this is the last thing before image 0x00311 jumps to the
 * results screen at image 0x046EB:
 *
 *     0x002CD  call 0x5447                          the screen off
 *     0x002D0  ds:0x2010 != 0xFF && ds:0x25A7 & 1   ax=0x100, int 60h
 *     0x002E3  ds:0x2010 != 0xFF && ds:0x25A7 & 1   ax=0,     int 60h
 *     0x002F6  ds:0x2A = 0
 *     0x002FB  call 0x16A9
 *     0x002FE  ds:0x2010 == 0xFF && ds:0x25A7 & 3   ax=0x200, int 61h
 *     0x00311  jmp 0x46EB
 *
 * THE LAST LINE IS THE ONE THE PORT WAS MISSING, and it is why the music
 * would not change with the level: nothing ever stopped the driver, so the
 * tune a level started was still running when the next one asked for its
 * own at image 0x003B4 twenty frames in.
 *
 * Note the mask. The two INT 60h calls test bit 0 - EFFECTS - because the
 * fallback device only ever made effects; the stop tests BOTH bits, because
 * it is the device being shut down rather than a sound being made. The port
 * would have written `sound_enabled(2)` here by symmetry with image 0x003AD
 * and been wrong on the fx-only setting.
 *
 * The two INT 60h calls are the fallback's, on a machine where no driver
 * answered - see sound_request, which has no such device. */
void level_end_finish(void)
{
    if (g_snd_driver == 0xFF && sound_enabled(3))   /* image 0x002FE */
        sound_request(0x61, 2, 0);                  /* image 0x0030C */
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
            sound_request(0x60, 3, 0x02);   /* image 0x02F89 */
    } else if (sound_enabled(3)) {         /* image 0x02F95 */
        sound_request(0x61, 4, 0x10);            /* image 0x02F9C */
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

/* ======================================================================
 * IMAGE 0x051A2 - TAKING A CODE BACK, and it does far more than the
 * checksum level_code_ok stops at.
 *
 *   0x051A6  "BILLANDTED" - five word compares, and a match sets ds:0x1F74.
 *            The cheat, tested before anything else and NOT part of the
 *            validation: a wrong code that begins with it still fails below.
 *   0x051D4  the checksum, sum of the first nine masked to a nibble, against
 *            character 9 less the base's - the same relation level_code_ok
 *            already has, written the other way round.
 *   0x051EC  THE LEVEL, out of characters 7 and 8 as two nibbles:
 *            (c7 - base7) & 0x0F  |  ((c8 - base8) & 0x0F) << 4
 *            into ds:0x1F6B.
 *   0x0520E  undo the rotation - `dh = 7 - (index & 7)` passes of shifting
 *            characters 0..6 left by one, which is image 0x05042's own
 *            rotation run backwards.
 *   0x0523A  rebuild the index a SECOND time, one bit at a time out of the
 *            unrotated characters 0..6, and image 0x05292 REQUIRES the two
 *            to agree. That is what makes this a real check rather than a
 *            round trip: the index is carried twice, by different means, and
 *            a code that satisfies one and not the other is refused.
 *   0x05298  ds:0x1F6E = ds:0x1F6B / 30 - the rating, the same division
 *            level_rating does.
 *   0x052A4  the percentage, four bits from characters 0, 2, 4 and 5, into
 *            ds:0x491D.
 *
 * Returns with ZF SET on success - image 0x052D1's `sub ah,ah` - and the
 * caller at image 0x04999 is a `jne`. Here that is 1 for accepted.
 *
 * THE PORT ONLY HAD THE CHECKSUM, and nothing set ds:0x1F6B, so typing a
 * code changed nothing. A player found that too.
 * ====================================================================== */
#define CODE_CHEAT "BILLANDTED"          /* image 0x051A6 */

uint8_t level_cheat_on(void)  { return g_cheat; }
uint8_t level_code_pct(void)  { return g_code_pct; }

/* One bit of the index out of an unrotated character - image 0x0523A's
 * chain. Each character contributes one bit, at its own shift. */
static uint8_t code_bit(const char *c, uint8_t i, int8_t shift, uint8_t mask)
{
    uint8_t al = (uint8_t)c[i];
    al = (shift < 0) ? (uint8_t)(al >> (uint8_t)(-shift))
                     : (uint8_t)(al << (uint8_t)shift);
    return (uint8_t)(al & mask);
}

/* Image 0x051A2 itself - see the block above for what each step is, and
 * tools/probe_codein.py for the original being driven over it. */
int level_code_apply(const char *code)
{
    char c[CODE_LEN];
    uint8_t al, ah, sum, idx, dh;

    for (uint8_t i = 0; i < CODE_LEN; i++) c[i] = code[i];

    /* Image 0x051A6, and it is checked on the TYPED code before the rotation
     * is undone. A match arms the cheat and then falls straight into the
     * validation, which the cheat itself does not pass. */
    if (!memcmp(c, CODE_CHEAT, CODE_LEN))
        g_cheat = 0xFF;                             /* image 0x051C8 */

    sum = 0;                                        /* image 0x051D4 */
    for (uint8_t i = 0; i < CODE_LEN - 1; i++)
        sum = (uint8_t)(sum + (uint8_t)c[i]);
    sum &= 0x0F;                                    /* image 0x051DD */
    ah = (uint8_t)((uint8_t)c[9] - sum);            /* image 0x051E2 */
    if ((uint8_t)CODE_BASE[9] != ah) return 0;      /* image 0x051E4 */

    /* Image 0x051EC: the level, as two nibbles in characters 7 and 8. */
    al = (uint8_t)((uint8_t)c[7] - (uint8_t)CODE_BASE[7]);
    ah = (uint8_t)((uint8_t)c[8] - (uint8_t)CODE_BASE[8]);
    al &= 0x0F; ah &= 0x0F;                         /* image 0x051F8 */
    idx = (uint8_t)(al | (uint8_t)(ah << 4));       /* image 0x05203 */

    /* Image 0x0520E: undo image 0x05042's rotation, 7 - (index & 7) times. */
    dh = (uint8_t)(7 - (idx & 7));                  /* image 0x05208 */
    for (int8_t n = (int8_t)dh; n >= 0; n--) {      /* image 0x05238's jns */
        char first = c[0];
        for (uint8_t i = 0; i < 6; i++) c[i] = c[i + 1];
        c[6] = first;
    }

    /* Image 0x0523A: and now the index AGAIN, a bit per character. */
    ah  = code_bit(c, 0, -3, 0x01);                 /* image 0x0523C */
    ah |= code_bit(c, 1, -1, 0x02);                 /* image 0x05248 */
    ah |= code_bit(c, 2,  0, 0x04);                 /* image 0x05251 */
    ah |= code_bit(c, 3,  3, 0x08);                 /* image 0x05258 */
    ah |= code_bit(c, 4,  3, 0x10);                 /* image 0x05265 */
    ah |= code_bit(c, 5,  5, 0x20);                 /* image 0x05272 */
    ah |= code_bit(c, 6,  4, 0xC0);                 /* image 0x05283 */
    if (ah != idx) return 0;                        /* image 0x05292 */

    level_index_set(idx);                           /* image 0x05205 / 0x052A1 */

    /* Image 0x052A4: the percentage, four bits from four characters. */
    ah  = code_bit(c, 0, -1, 0x01);
    ah |= code_bit(c, 2,  1, 0x02);
    ah |= code_bit(c, 4,  2, 0x04);
    ah |= code_bit(c, 5,  2, 0x08);
    g_code_pct = ah;                                /* image 0x052CD */
    return 1;                                       /* image 0x052D1 */
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

/* THE RESULTS SCREEN, image 0x046EB - what it DECIDES, which is everything.
 *
 *     0x0471D  al = ds:0x40                 the rescued percentage
 *     0x04720  cmp al, ds:0x41              against the requirement
 *     0x04724  jge 0x04729                  PASSED
 *     0x04726  jmp 0x04778                  failed: ds:0x1F6D = 0
 *     0x04729  ds:0x1F6D = 0xFF             passed: step the music
 *     0x0472E  inc ds:0x1F6B                and the level
 *     0x04737  ds:0x1F6E = ds:0x1F6B / 30   and the rating
 *     0x0473E  cmp ds:0x1F6B, 0x78          out of game at 120
 *     0x04752  call 0x05042                 the access code, for the level
 *                                           it has just advanced TO
 *
 * None of that was here. main.c judged the level, advanced the index,
 * generated the code and handed the answers to a drawing routine - so the
 * screen drew whatever it was told, and every one of those decisions was the
 * port's own arrangement of the game's.
 *
 * Returns RESULTS_OUT_OF_GAME when image 0x04745 has run off the end,
 * RESULTS_PASSED or RESULTS_FAILED otherwise. What the screen then does with
 * a button is image 0x047A9's, in sdl_io.c, because that is input.
 */
int level_results_judge(char *code)
{
    if (code) code[0] = '\0';
    if (level_saved_pct() < level_needed_pct())   /* image 0x04720 */
        { level_failed(); return RESULTS_FAILED; }/* image 0x04778 */

    if (!level_passed()) return RESULTS_OUT_OF_GAME;  /* image 0x0473E */
    /* image 0x04752, and note WHICH level it is for: the index has already
     * been advanced by image 0x0472E, so the code names the level about to
     * be played and not the one just finished. */
    if (code) level_code_generate(level_index(), 0, level_saved_pct(), code);
    return RESULTS_PASSED;
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
/* ======================================================================
 * WHICH LEVEL, and it is ONE BYTE.
 *
 * ds:0x1F6B is the index into the game's running order, 0..119, and ds:0x1F6E
 * is its rating. Everything else is derived: image 0x04566 works the NUMBER
 * within a rating out as
 *
 *     ah = ds:0x1F6E * 30        by four shifts and three adds
 *     al = ds:0x1F6B + 1 - ah
 *
 * so there is no separate "number" anywhere in the game. main.c kept one, and
 * a rating, and an index, and had to remember to move all three together.
 *
 * Four routines move it and they are the whole of it:
 * ====================================================================== */

uint8_t level_index(void)  { return g_level_index; }
uint8_t level_rating(void) { return g_level_rating; }

/* image 0x04566: ds:0x1F6B + 1 - rating * 30, one-based. Image 0x0488C,
 * which the results screen (image 0x04748) and the code screen (image
 * 0x049A1) call instead, divides ds:0x1F6B + 1 by 30 and turns a remainder
 * of 0 into 30 with the quotient less one - the same number, and a quotient
 * equal to ds:0x1F6E, which both callers' paths have just set from the same
 * index. */
uint8_t level_number(void)
{
    return (uint8_t)(g_level_index + 1
                     - g_level_rating * LEVELS_PER_RATING);
}

/* image 0x04729, the results screen's PASS arm: the advance flag, the index,
 * and the rating derived from it at image 0x04737's `div cl` by 30. Returns
 * zero when image 0x0473E's `cmp byte [0x1f6b], 0x78` has run out of game. */
int level_passed(void)
{
    music_level_passed(1);                        /* image 0x04729 */
    g_level_index++;                              /* image 0x0472E */
    g_level_rating = (uint8_t)(g_level_index / LEVELS_PER_RATING);
    return g_level_index < LEVELS_TOTAL;          /* image 0x0473E */
}

/* image 0x04778, the FAIL arm: the same level again, and the same tune. */
void level_failed(void) { music_level_passed(0); }

/* image 0x0454C. F1 from the MENU clears the advance flag before falling into
 * image 0x04551; the results screen's own "next level" at image 0x047DD jumps
 * to 0x04551 and does NOT, which is the whole difference between replaying a
 * level's tune and stepping to the next one.
 *
 * The port had no idea which way it had arrived, so the rotation advanced
 * whenever it felt like it. */
void level_from_menu(void) { music_level_passed(0); }   /* image 0x0454C */

/* image 0x044EC and image 0x0451C: the rating plate's two keys. Up caps at 3
 * (image 0x044F9), Down floors at 0 (image 0x04529), and both then set
 * ds:0x1F6B to rating * 30 - the first level of the new rating. */
void level_rating_up(void)
{
    /* Image 0x044EC, the same refusal F3 has: while ds:0x1F6F holds 4 the
     * rating plate still owes a draw, and the key is dropped for that pass. */
    if (g_rating_drawn == 4) return;                    /* image 0x044EC */
    if (g_level_rating >= MENU_RATING_MAX) return;      /* image 0x044F9 */
    g_level_rating++;                                   /* image 0x04500 */
    g_level_index = (uint8_t)(g_level_rating * LEVELS_PER_RATING);
}

/* image 0x0451C, the other way. */
void level_rating_down(void)
{
    if (g_rating_drawn == 4) return;                    /* image 0x0451C */
    if (g_level_rating == 0) return;                    /* image 0x04529 */
    g_level_rating--;                                   /* image 0x04530 */
    g_level_index = (uint8_t)(g_level_rating * LEVELS_PER_RATING);
}

/* image 0x0454C's block wipes ds:0..0xFC9 at a cold start; ds:0x1F6B and
 * ds:0x1F6E are above that and start at zero with the rest of BSS. */
void level_index_reset(void) { g_level_index = 0; g_level_rating = 0; }

/* THE PORT'S OWN, and only for the harness. The original never sets ds:0x1F6B
 * to an arbitrary level: it starts at 0, image 0x0472E advances it and the
 * rating keys move it in thirties. `lemdev --rating taxing --number 2` has to
 * get there somehow, and the alternative was every routine that reads the
 * index taking a rating and a number instead - which is what they all did. */
void level_index_set(uint8_t index)
{
    g_level_index  = index;
    g_level_rating = (uint8_t)(index / LEVELS_PER_RATING);
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

void level_saved_reset(void) { level_saved = 0; }

/* Image 0x02335. */
void level_lemming_saved(void) { level_saved++; }

/* ds:0x40 and ds:0x41 - THE TWO PERCENTAGES THE RESULTS SCREEN READS.
 *
 * Image 0x0471D reads ds:0x40 and image 0x04720 compares it against ds:0x41.
 * It takes neither as an argument: both are written long before, ds:0x40 by
 * image 0x03AF4 as the panel draws its IN field every frame and ds:0x41 by
 * image 0x0459E as the briefing works the requirement out.
 *
 * main.c passed both in, which meant it had to know when to compute them -
 * and the port's results screen could be handed numbers the game would never
 * have had at that moment. */

uint8_t level_saved_pct(void)  { return g_saved_pct; }
uint8_t level_needed_pct(void) { return g_needed_pct; }

/* IMAGE 0x04551 - THE WIPE, and the rest of image 0x0454C's chain.
 *
 *     0x0454C  mov byte [0x1F6D], 0    the advance flag - level_from_menu
 *     0x04551  es = ds, di = 0
 *     0x04556  cx = 0xFCA
 *     0x0455B  rep stosb               ds:0x0000..0x0FC9 to zero
 *     0x0455D  call 0x0197D            the level's files
 *     0x04560  call 0x04566            the briefing
 *     0x04563  jmp 0x000FB             the one main loop
 *
 * FOUR THOUSAND AND FORTY-TWO BYTES, which is every byte of the level's
 * running state: the counters, the flags, the skill and status cells, and
 * everything up to the level record the load then writes at ds:0x0FCB. The
 * original starts a level by ERASING, and the two entries share it - image
 * 0x047DD, the results screen's "next level", jumps to 0x04551 and not to
 * 0x0454C, which is how it skips the advance flag and keeps the wipe.
 *
 * The port has no contiguous DGROUP to memset, so this is the wipe's MEANING
 * rather than its `rep stosb`: the state that lives in that range goes to
 * zero. Most of it is already covered by the resets the level start calls -
 * level_skills_reset, level_status_reset, level_release_reset,
 * level_start_reset, level_saved_reset and lemmings_reset - and these five
 * were covered by NONE of them:
 *
 *     ds:0x2B  g_paused      toggled by image 0x04028 and never cleared, so
 *                            a level paused when it ended left the NEXT one
 *                            paused. main.c plays them in a loop, so this is
 *                            reachable by playing.
 *     ds:0x53  g_pause_at    that toggle's debounce stamp - image 0x0402D
 *     ds:0x40  g_saved_pct   image 0x03AF4 writes it every panel frame
 *     ds:0x41  g_needed_pct  image 0x0459E writes it from the briefing
 *     ds:0x54  g_tune        image 0x04866 picks it per level
 *
 * The last three are written before anything reads them, so only the first
 * two could be SEEN - which is exactly why a wipe is the right shape and
 * five separate resets are not. Wiping the state is one decision; remembering
 * which of sixty variables still needs it is sixty.
 */
void level_state_wipe(void)
{
    g_paused     = 0;            /* ds:0x2B */
    g_pause_at   = 0;            /* ds:0x53 */
    g_saved_pct  = 0;            /* ds:0x40 */
    g_needed_pct = 0;            /* ds:0x41 */
    g_tune       = 0;            /* ds:0x54 */
}

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
    uint8_t pct = 0;
    if (total) {
        uint16_t ax = (uint16_t)((uint8_t)lv->num_to_rescue * 100u);
        pct = (uint8_t)(ax / total);
    }
    g_needed_pct = pct;                    /* image 0x0459E */
    return pct;
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
    g_saved_pct = percent;                 /* image 0x03AF4 */
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
 * bitplane doubles as its mask, so the test is bit 3.
 *
 * AND IMAGE 0x05D94 IS WHAT MAKES BIT 3 THE ANSWER. It runs once before the
 * lemming loop - image 0x01F23 calls it - and programs the card so that a
 * read returns that plane and nothing else: sequencer map mask 0x0F, then
 * graphics controller 5 = 0x08, 7 = 0x08 and 2 = 0x08, which is colour
 * don't care with only plane 3 left in. It also sets ds:4 to 0xD0, the row
 * stride image 0x05DC6 adds - 208 bytes, 1,664 pixels.
 *
 * So `& 8` here is not a convention this port chose; it is the register the
 * original loads. The port keeps the four planes as one byte per pixel, so
 * what the card does with a mask this does with a mask. */
static int lem_solid(const uint8_t *lvl, uint16_t cw, uint16_t chh,
                     int16_t x, int16_t y)
{
    int32_t cx = (int32_t)x - LEMMING_DX;
    int32_t cy = (int32_t)y - LEMMING_DY;
    if (cx < 0 || cx >= (int32_t)cw || cy < 0 || cy >= (int32_t)chh)
        return 0;
    return (lvl[(size_t)cy * cw + cx] & 8) != 0;
}

/* ======================================================================
 * IMAGE 0x03150's ASSIGNMENT, AND ITS SHARED TAIL.
 *
 * Every skill handler ends in one of exactly two jumps:
 *
 *     success -> image 0x031E5      refusal -> image 0x0320C
 *     0x0326E  0x032B7  0x0331C     0x032BA  0x0331F  0x03345
 *     0x034C8  0x0353D  0x03578     0x034CB  0x03540  0x0357B
 *     0x03619                       0x0361C  0x03285
 *
 * Image 0x0320C is a bare `ret`. Image 0x031E5 is the SOUND - the fallback
 * beep at image 0x031F3 and effect 0x04 at image 0x03206 - and then image
 * 0x0320B's `ret`. It is the only site in the whole image that asks for
 * effect 0x04, and the bomber's own routine at image 0x031C4 falls into it
 * the same way, having set the fuse at image 0x031E1.
 *
 * SO THE CLICK IS SILENT UNLESS A SKILL WAS ACTUALLY GIVEN. A player
 * reported the port clicking at empty sky and at a lemming that already had
 * the skill: it played the sound from the click handler in sdl_io.c, gated
 * only on "a skill is left in the count". That is a decision the game makes,
 * made in the IO layer - the same shape CLAUDE.md records for main.c, one
 * file over - and no test here could reach it, because every check compares
 * a recorded run in which the click lands on a lemming that takes it.
 * ====================================================================== */
/* THE SECOND CHANCE. Image 0x0317C hands every handler ds:0x58, the lemming
 * the pointer's scan CHOSE - which with the left button is the one already
 * doing a job, if any is under the pointer (image 0x03DCC). Four handlers
 * then do not simply refuse it: the digger, builder, basher and miner each
 * fall back to ds:0x5A, the scan's PLAIN candidate, test it against their
 * mask, and give it the skill instead (image 0x03271, 0x03322, 0x03543,
 * 0x0361F). So clicking a digger with walkers in its shaft gives the walker
 * the digger.
 *
 * The port refused, and every click on such a stack went to the lemming that
 * already had the job. A player found it on "Just dig!", where every
 * lemming after the first walks into the first one's shaft; the original
 * spends all ten diggers on the same clicks.
 *
 * The bomber, climber, blocker and floater have no such fallback. The
 * floater's code has one at image 0x0357E, but its own refusal at image
 * 0x0355C..0x03569 goes to image 0x0357B and nothing in the handler reaches
 * it. */
int level_assign(lemming *a, lemming *plain, uint8_t skill)
{
    int took = 0;
    switch (skill) {                                   /* image 0x03180 */
    case SKILL_DIGGER:  took = lem_dig_start(a, plain);   break;  /* 0x0320D */
    case SKILL_BUILDER: took = lem_build_start(a, plain); break;  /* 0x032BD */
    case SKILL_BASHER:  took = lem_bash_start(a, plain);  break;  /* 0x034CE */
    case SKILL_MINER:   took = lem_mine_start(a, plain);  break;  /* 0x03598 */
    case SKILL_BLOCKER: took = lem_block_start(a); break;  /* image 0x03336 */
    case SKILL_BOMBER:  took = lem_bomb_start(a);  break;  /* image 0x031C4 */
    case SKILL_CLIMBER: took = lem_climb_start(a); break;  /* image 0x0329B */
    case SKILL_FLOATER: took = lem_float_start(a); break;  /* image 0x03557 */
    default: break;
    }
    if (!took) return 0;                               /* image 0x0320C */

    level_skill_spend(skill);                          /* image 0x03224 */
    /* Image 0x031E5's tail. The fallback at image 0x031F3 is INT 60h's and
     * this port has no such device - see sound_request. */
    if (sound_driver_answered() && sound_enabled(3))   /* image 0x031F8 */
        sound_request(0x61, 4, 0x04);                  /* image 0x03206 */
    return 1;                                          /* image 0x0320B */
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
 * beside it. sdl_io.c sets it from the right button, and an input script
 * carries it as `rdown` / `rup`. With it up the busy one is chosen, and four
 * of the skill handlers then fall back to the plain one themselves - see
 * level_assign. */

/* ds:0x63 - HOW MANY lemmings are under the pointer, which is the number the
 * panel prints beside the name. Image 0x03DA1 counts them. */

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

/* WHAT THE SCAN LEAVES BEHIND for image 0x03150 to act on.
 *
 * The three indices - ds:0x5E, ds:0x5F, ds:0x60 - start at 0xFF every scan
 * (image 0x03CFB..0x03D05), but the three RECORD POINTERS beside them do not:
 * image 0x03D8E stores ds:0x5C only when a busy lemming is found and image
 * 0x03D9C ds:0x5A only when a plain one is, so each keeps its last value.
 * Image 0x03DB4 then copies one of them into ds:0x58 - and with the right
 * button down and no plain lemming under the pointer that is ds:0x5A's OLD
 * value, a lemming found on some earlier frame. The click then goes to it.
 * The fallback to the plain one is guarded: image 0x03271 tests ds:0x5F, this
 * scan's index, before it trusts ds:0x5A.
 *
 * Indices here where the original keeps pointers; -1 is a pointer the
 * original has never set, which a cold start leaves at whatever image
 * 0x0454C's wipe put there. */
static int16_t g_scan_busy_rec  = -1;            /* ds:0x5C */
static int16_t g_scan_plain_rec = -1;            /* ds:0x5A */
static int16_t g_scan_plain_now = -1;            /* ds:0x5F */
static int16_t g_scan_chosen    = -1;            /* ds:0x58 */

void level_scan_keep(const level_scan *sc)
{
    if (sc->busy >= 0)  g_scan_busy_rec  = sc->busy;     /* image 0x03D8E */
    if (sc->plain >= 0) g_scan_plain_rec = sc->plain;    /* image 0x03D9C */
    g_scan_plain_now = sc->plain;                        /* image 0x03D99 */
    if ((lem_buttons & 2) || sc->busy < 0)               /* image 0x03DB7, 0x03DCF */
        g_scan_chosen = g_scan_plain_rec;                /* image 0x03DC6 */
    else
        g_scan_chosen = g_scan_busy_rec;                 /* image 0x03DDA */
}

/* ds:0x58, as image 0x0317C loads it. */
int16_t level_scan_chosen(void) { return g_scan_chosen; }

/* ds:0x5A, when image 0x03271's test of ds:0x5F lets it be used. */
int16_t level_scan_plain(void)
{
    return g_scan_plain_now >= 0 ? g_scan_plain_rec : -1;
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
 * STALE FROM HERE - AND IT MISLED SOMEBODY. This block used to end "so it
 * is not drawn... a digging lemming is invisible". That stopped being true
 * when DIG_SPR_BASE was worked out further down this file, by rendering both
 * candidate geometries and looking - the note there records the wrong
 * turning - and this paragraph was never updated.
 *
 * The consequence: a later pass read THIS and believed it, added a second
 * digger draw beside the working one, and wrote "the digger is drawn now, it
 * was not drawn at all before" into a commit message. Measured, the two
 * paths score identically (659 of 709 frames exact, worst 40), so the
 * duplicate changed nothing and the claim was false.
 *
 * The answer is 3 planes x 14 rows x 2 bytes = 84, with image 0x05BB0
 * appending the mask to make 112 - which image 0x059BB's build table states
 * outright, so the geometry now has a reading behind it as well as a
 * measurement. See DIG_SPR_BASE and LEM_SPRITE.
 *
 * Transcribed from image 0x0320D - the assignment itself, which is what the
 * function below is.
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
/* Transcribed from image 0x0320D, with its second chance at image 0x03271.
 * `plain` is what the pointer's scan left in ds:0x5A - the last lemming under
 * the pointer doing no job - or NULL where ds:0x5F was 0xFF. See
 * level_assign. */
int lem_dig_start(lemming *a, lemming *plain)
{
    if ((a->tcell & 0x0F) == 9) return 0;           /* image 0x03214: steel */
    if (a->flags & DIG_KEEP) {                      /* image 0x0321D */
        /* Image 0x03271: the lemming the click chose cannot dig, so try the
         * plain one - and test ONLY the mask again. Image 0x03283 jumps to
         * image 0x03224, past the steel test above, so the second chance is
         * not asked about the ground it stands on. */
        if (!plain || (plain->flags & DIG_KEEP)) return 0;  /* 0x0327C */
        a = plain;                                  /* image 0x03278 */
    }
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

/* IMAGE 0x02490 - AND WHAT IT DUG INTO MIGHT BE STEEL.
 *
 * Image 0x02446 and image 0x0244A test the bits image 0x05EBA just cleared,
 * and BOTH arms of that test are used: `jne 0x2491` when something was
 * there, and the fall-through at image 0x0244E when nothing was. The port
 * had only the second, so a digger that reached steel dug straight on
 * through it.
 *
 * Image 0x02491 asks the trigger map - the same cell arithmetic every other
 * reader of it uses, `x >> 2` and `(y - 0x10) >> 2` on the lemming's own
 * record coordinates, masked to the low nibble - and effect 9 is steel. On
 * anything else image 0x024BA jumps to image 0x01FE8 and the dig carries on,
 * which is why this is a stop rather than a test that has to be passed.
 *
 * On steel the lemming becomes a WALKER, and the conversion is the one image
 * 0x027FC does for a blocker whose floor went away: the flag word cleared
 * outright, the hot spot back to a walker's (-8, -10), the animation counter
 * to zero, the state to 9 and image 0x02505's `and [si+0x24],1`. The four
 * fields the port does not model - [si+0x0A], [si+0x0C], [si+0x10] = 0x5A
 * and [si+0x12] = 0x3C - are set here too and are noted rather than
 * invented; nothing in this port reads them.
 *
 * AND IT MAKES A SOUND, image 0x024CB's effect 0x0A, which is the one this
 * repository has been carrying as "nothing to attach it to". This is what it
 * attaches to. Note its mask is ds:0x25A7 & 3 - either setting - like every
 * other effect and unlike the music.
 */
static int dig_hit_steel(lemming *a)
{
    uint8_t cell = (uint8_t)(level_trigger_cell((int32_t)a->x / TRIG_CELL,
                        ((int32_t)a->y - TRIG_Y_BIAS) / TRIG_CELL) & 0x0F);
    if (cell != 9) return 0;                  /* image 0x024B6 */

    if (sound_driver_answered() && sound_enabled(3))   /* image 0x024BD */
        sound_request(0x61, 4, 0x0A);                  /* image 0x024CB */

    a->flags = 0;                             /* image 0x024D0 */
    a->anim  = 0;                             /* image 0x024E9 */
    a->hot_x = -8;                            /* image 0x024ED */
    a->hot_y = -10;                           /* image 0x024F2 */
    a->state = 9;                             /* image 0x024F7 */
    a->extra &= 1;                            /* image 0x02505 */
    return 1;
}

/* Transcribed from image 0x023F3, with its first frame at image 0x0250C.
 * Sixteen animation frames, digging on the two where the counter comes round
 * to 0 and to 8 - image 0x02406 and image 0x02408. */
static uint8_t lem_dig(lemming *a, uint8_t *lvl, uint16_t cw, uint16_t chh)
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
            return 0;                         /* image 0x0240C: just animate */
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
        return 0;                             /* image 0x0242A -> 0x020E2 */
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
    if (had && dig_hit_steel(a)) return 1;    /* image 0x02509 */
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
        /* AND THE FALL COUNTER GOES BACK TO 3 - image 0x0246E, one
         * instruction between the two this used to cite, and skipped.
         *
         * A player found it on Fun 13, "We all fall down": dig through the
         * beam and the original's lemming survives the drop off the bottom,
         * because the fall it is about to take is measured from HERE. The
         * port carried whatever ds:[si+0x23] already held - and on that
         * level the lemmings arrive by falling a long way out of the
         * entrance, and image 0x022A3's landing does NOT clear it - so the
         * counter was already past image 0x02250's 0x3C and the lemming
         * splatted on a drop of a few pixels.
         *
         * Nothing here could have caught it. The digger's own record agrees
         * frame for frame either way while it is inside the ground, which is
         * the exact gap verify_dig.py already says it has. */
        a->fall  = 3;                          /* image 0x0246E */
        a->hot_x = FALL_HOT_X;                 /* image 0x02472 */
        a->hot_y = FALL_HOT_Y;                 /* image 0x02477 */
        a->state = 4;                          /* image 0x0247C */
    }
    return 1;                                 /* image 0x0248E, 0x024BA */
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

int lem_build_start(lemming *a, lemming *plain)
{
    if ((int32_t)a->y + a->hot_y <= BUILD_TOP) return 0;   /* 0x032C4 */
    if (a->flags & BUILD_KEEP) {                           /* 0x032CF */
        /* Image 0x03322: the plain candidate, mask only - image 0x03334
         * rejoins at image 0x032D6, past the height test. */
        if (!plain || (plain->flags & BUILD_KEEP)) return 0;   /* 0x0332D */
        a = plain;                                         /* 0x03329 */
    }
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
static uint8_t lem_build(lemming *a, uint8_t *lvl, uint16_t cw, uint16_t chh)
{
    /* Image 0x02693: sixteen frames, and only the wrap does anything. */
    a->anim = (uint8_t)((a->anim + 1) & 0x0F);
    if (a->anim) {
        /* Image 0x026DE lays the brick on frame 9. */
        if (a->anim == 9) {
            lem_build_brick(a, lvl, cw, chh);      /* image 0x05F0F */
            return 0;                              /* image 0x026E1 */
        }
        /* AND FRAME 10 IS THE WARNING, image 0x026A2, when three or fewer
         * bricks are left - image 0x026A6 reads [si+0x21] and image 0x026AB
         * lets anything above three past without a sound. It is the noise a
         * player counts the last bricks by, and this was a deliberate no-op
         * for as long as the port had no sound. */
        else if (a->anim == 0x0A && a->bricks <= 3) {   /* image 0x026A4 */
            if (g_snd_driver == 0xFF && sound_enabled(3))  /* image 0x026C2 */
                sound_request(0x61, 4, 0x12);                    /* image 0x026D0 */
        }
        return 1;                                  /* image 0x026DB */
    }

    /* Image 0x026E4: a step up and along, then three probes. */
    int16_t step = a->dir;                         /* [si+0x27] */
    a->x = (int16_t)(a->x + step);                 /* image 0x026E8 */
    a->y = (int16_t)(a->y - 1);                    /* image 0x026EA */

    if (a->x <= BUILD_X_MIN || a->x >= BUILD_X_MAX) {   /* 0x026EF, 0x026F7 */
        a->dir = (int8_t)-a->dir;                  /* image 0x02763 */
        lem_build_stop(a, 9, 0);
        return 1;
    }
    /* Image 0x02700 and image 0x02710 probe the row ABOVE - image 0x02703
     * takes ds:4, one row's stride, off the address image 0x05DC6 built. */
    if (lem_solid(lvl, cw, chh, a->x, (int16_t)(a->y - 1))) {
        a->dir = (int8_t)-a->dir;
        lem_build_stop(a, 9, 0);
        return 1;
    }
    a->x = (int16_t)(a->x + step);                 /* image 0x0270E */
    if (lem_solid(lvl, cw, chh, a->x, (int16_t)(a->y - 1))) {
        a->dir = (int8_t)-a->dir;
        lem_build_stop(a, 9, 0);
        return 1;
    }

    if (--a->bricks == 0) {                        /* image 0x0271E */
        /* Image 0x027A5: out of bricks. It keeps its direction, takes state
         * 8, and sets the bit image 0x03DFD reads to go on calling it a
         * BUILDER on the panel. */
        lem_build_stop(a, 8, 1);
        return 1;
    }

    /* Image 0x02726: nine rows up, two along - the headroom for the next
     * brick. Image 0x02737 shifts ds:4 by three, so it is eight more rows on
     * top of the one image 0x02735 already took off. */
    if (lem_solid(lvl, cw, chh, (int16_t)(a->x + step * 2),
                  (int16_t)(a->y - 9))) {          /* image 0x02740 */
        a->dir = (int8_t)-a->dir;
        lem_build_stop(a, 9, 0);
        return 1;
    }
    if (a->x <= BUILD_X_MIN || a->x >= BUILD_X_MAX) {   /* 0x02747, 0x0274C */
        a->dir = (int8_t)-a->dir;
        lem_build_stop(a, 9, 0);
        return 1;
    }
    if ((int32_t)a->y + a->hot_y <= BUILD_TOP)     /* image 0x02752 */
        lem_build_stop(a, 9, 0);                   /* image 0x02760: no turn */
    return 1;
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
static uint8_t lem_hoist(lemming *a)
{
    a->anim = (uint8_t)((a->anim + 1) & 7);         /* image 0x0263A */
    if (a->anim == 0) {                             /* image 0x02641 */
        a->flags = 0;                               /* image 0x02654 */
        a->hot_x = -8;
        a->hot_y = -10;                             /* image 0x02676 */
        a->state = 9;                               /* image 0x0267B */
        a->extra &= 1;                              /* image 0x02689 */
        return 1;
    }
    if (a->anim >= 5) return 0;                     /* image 0x02647 */
    a->y = (int16_t)(a->y - 2);                     /* image 0x0264A */
    return 1;
}


#define BASH_BIT     0x0200
#define BASH_KEEP    0xF76F     /* image 0x034D5 */
#define BASH_MASKOFF 0xF56F     /* image 0x034FF */
#define BASH_ROWS    10         /* image 0x0602C: cx = 0x0A */
#define BASH_STATE   0x20       /* image 0x0352B */

int lem_bash_start(lemming *a, lemming *plain)
{
    if (a->flags & BASH_KEEP) {                     /* image 0x034D5 */
        /* Image 0x03543: the plain candidate, mask only, and image 0x03555
         * rejoins at image 0x034DC - so the wall tests below are asked of
         * it as they would have been of the first. */
        if (!plain || (plain->flags & BASH_KEEP)) return 0;   /* 0x0354E */
        a = plain;                                  /* image 0x0354A */
    }
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
static uint8_t lem_bash(lemming *a, uint8_t *lvl, uint16_t cw, uint16_t chh)
{
    a->anim = (uint8_t)((a->anim + 1) & 0x1F);      /* image 0x02880 */
    uint8_t al = (uint8_t)(a->anim & 0x0F);         /* image 0x02887 */

    if (al >= 0x0B) {                               /* image 0x02889 */
        /* Image 0x0293A: forward one, and down if there is nothing there. */
        a->x = (int16_t)(a->x + a->dir);
        if (a->x <= 0x10 || a->x >= 0x680) {        /* 0x02940, 0x02945 */
            a->dir = (int8_t)-a->dir;
            return 1;
        }
        /* THREE ROWS, EACH PROBED WHERE THE LEMMING NOW IS. The original
         * probes, then `inc [si+2]` and moves di down ONE row - image
         * 0x02958 and image 0x0295B - so the second probe is at y+1 and the
         * third at y+2. This added k to an ALREADY incremented y, so it
         * looked at y, y+2 and y+4 and stepped over a row each time. */
        for (uint8_t k = 0; k < 3; k++) {
            if (lem_solid(lvl, cw, chh, a->x, a->y))
                return 1;                             /* image 0x02953 */
            a->y = (int16_t)(a->y + 1);             /* image 0x02958 */
        }
        /* AND WITH NOTHING UNDER ANY OF THE THREE IT FALLS - image 0x0297D,
         * which the port did not have at all: it stepped down and returned,
         * so a basher that bashed its way into open air went on bashing in
         * mid-air. Found by auditing every write to the fall counter in the
         * image against the port; this site is one of the four that were
         * missing. */
        a->flags |= 4;                              /* image 0x0297D */
        a->anim   = 0;                              /* image 0x02995 */
        a->fall   = 3;                              /* image 0x02999 */
        a->hot_x  = -8;                             /* image 0x0299D */
        a->hot_y  = -10;                            /* image 0x029A2 */
        a->state  = 4;                              /* image 0x029A7 */
        a->extra &= 1;                              /* image 0x029B5 */
        a->flags &= 0xFDFF;                         /* image 0x029B9 */
        return 1;
    }
    if (al < 2 || al > 5) return 0;                 /* 0x02894, 0x0289B */
    lem_bash_slice(a, (uint8_t)(al - 2), lvl, cw, chh);   /* image 0x028A1 */

    if (a->anim != 5) return 0;                     /* image 0x028AB */

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
            return 0;                               /* image 0x028CE.. */
    }
    a->flags = 0;                                   /* image 0x028FE */
    a->hot_x = -8;
    a->hot_y = -10;
    a->anim  = 0;
    a->state = 9;
    a->extra &= 1;
    return 0;
}

/* ------------------------------------------------------------------ miner */


#define MINE_BIT     0x0800
/* THE SAME CONSTANT IS THE GUARD AND THE KEEP-MASK, image 0x035A2 and image
 * 0x035D8 - the trap the digger's assignment was got wrong on. Its zero bits
 * are 0x10, 0x80 and 0x200, so a digger, a builder or a basher may be turned
 * into a miner and a miner may not be told to mine again. */
#define MINE_KEEP    0xFD6F
#define MINE_STATE   0x18       /* image 0x03604 */
#define MINE_CYCLE   0x18       /* image 0x02B3A */

/* Image 0x03598. */
int lem_mine_start(lemming *a, lemming *plain)
{
    if (a->flags & MINE_KEEP) {                     /* image 0x035A2 */
        /* Image 0x0361F: the plain candidate, mask only, and image 0x03631
         * rejoins at image 0x035AC - the steel and wall tests still run. */
        if (!plain || (plain->flags & MINE_KEEP)) return 0;   /* 0x0362A */
        a = plain;                                  /* image 0x03626 */
    }
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
    /* AND THE ORIGINAL PROBES A SECOND TIME BEFORE THIS, which the port does
     * not: image 0x02C2E is another `and al,ah / jne 0x2C76`, and its
     * not-solid arm at image 0x02C32 is the same become-a-faller conversion
     * as image 0x02B9D above, fall counter and all (image 0x02C4E). The port
     * has one probe where the original has two, so a miner that cuts its own
     * floor away between them keeps mining.
     *
     * NOT TRANSCRIBED - where the second probe is taken needs the whole of
     * image 0x02B60..0x02C2E read, and guessing which of the two the port's
     * single check corresponds to is how a plausible wrong structure gets
     * written. Found by auditing every write to the fall counter in the
     * image; recorded here rather than filled in.
     *
     * Image 0x02C76: what it is standing on NOW, read fresh rather than out
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
static uint8_t lem_mine(lemming *a, uint8_t *lvl, uint16_t cw, uint16_t chh)
{
    uint8_t al = (uint8_t)(a->anim + 1);            /* image 0x02B35 */
    if (al == MINE_CYCLE) al = 0;                   /* image 0x02B3A */
    a->anim = al;                                   /* image 0x02B40 */

    if (al == 0) {                                  /* image 0x02B43 */
        a->y = (int16_t)(a->y + 1);                 /* image 0x02BE1 */
        if (a->y >= LEMMING_DEAD_Y) a->alive = 0xFF;
        return 1;
    }
    if (al == 0x0F) {                               /* image 0x02B4A */
        lem_mine_move(a, 0, lvl, cw, chh);
        return 1;
    }
    if (al == 3) {                                  /* image 0x02B51 */
        lem_mine_move(a, 1, lvl, cw, chh);
        return 1;
    }
    if (al > 3) return 0;                           /* image 0x02B5A */
    lem_mine_slice(a, (uint8_t)(al - 1), lvl, cw, chh);   /* image 0x02B5D */
    return 0;
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
#define CRATER_UP    0x1E       /* image 0x05E1F */
#define CRATER_BACK  8          /* image 0x05E41 */

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
    /* THE FUSE HAS RUN OUT - image 0x02E59. This is the one the nuke makes
     * over and over as it lights each lemming in turn. */
    if (g_snd_driver == 0xFF && sound_enabled(3))   /* image 0x02E4B */
        sound_request(0x61, 4, 0x05);                     /* image 0x02E59 */
    return 0;                                       /* image 0x02E5E */
}

/* Image 0x02D0F: sixteen frames of looking up, then the cloud.
 *
 * AND IT FALLS WHILE IT LOOKS UP. Frames 1..15 do not just animate: image
 * 0x02D83 probes under the lemming and moves it down up to three rows,
 * stopping on the first solid one, then jumps to image 0x020D6 - the
 * walker's own tail, which stores y, kills it at the bottom of the level and
 * otherwise goes through the trigger lookup at image 0x01FE8. The port had
 * the animation alone, so a bomber standing over nothing hung in the air for
 * fifteen frames and never looked at what it stood on. Found on Fun 11, a
 * blocker given a bomb: the original read its own 0x0A back on the first
 * frame of the countdown and the port never did.
 *
 * Image 0x02D78 compares the frame with 5 and ds:0x4B with 0xFF, and both
 * arms of both tests go to image 0x02D83 - it does nothing.
 *
 * Returns whether it left through image 0x01FE8 - see lemmings_step. */
static uint8_t lem_boom(lemming *a, const uint8_t *lvl, uint16_t cw,
                        uint16_t chh)
{
    a->anim = (uint8_t)((a->anim + 1) & 0x0F);      /* image 0x02D12 */
    if (a->anim) {                                  /* image 0x02D19 */
        uint8_t k = 0;
        while (k < 3 && !lem_solid(lvl, cw, chh, a->x,
                                   (int16_t)(a->y + k)))   /* image 0x02D89.. */
            k++;                                    /* image 0x02D93, 0x02DA2 */
        a->y = (int16_t)(a->y + k);                 /* image 0x020D6 */
        if (a->y >= LEMMING_DEAD_Y) a->alive = 0xFF;   /* image 0x020DD */
        return 1;                                   /* image 0x020DF */
    }
    a->flags |= CLOUD_BIT;                          /* image 0x02D1B */
    a->hot_x = -16;                                 /* image 0x02D3C: fff0 */
    a->hot_y = -25;                                 /* image 0x02D37: ffe7 */
    a->anim  = 0;                                   /* image 0x02D33 */
    a->state = 0;                                   /* image 0x02D41 */
    a->extra &= 1;                                  /* image 0x02D53 */
    /* THE EXPLOSION - image 0x02D65. The cloud's own sound at image 0x02374
     * is a different one on the OTHER interrupt; this is the bang. */
    if (g_snd_driver == 0xFF && sound_enabled(3))   /* image 0x02D57 */
        sound_request(0x61, 4, 0x0C);                     /* image 0x02D65 */
    return 0;                                       /* image 0x02D75 */
}


/* Image 0x0283B - transcribed below, with the blocker it belongs to. */
static void block_restore(const lemming *a);

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
    /* THE BANG, and it is on the FALLBACK device. Image 0x02366 tests
     * ds:0x2010 FIRST and skips the whole thing when a driver answered -
     * this is what a machine with no AdLib does instead. The port had only
     * the ds:0x25A7 test, and its sound_request sent AH=3 to the driver's
     * TUNE slot whatever the interrupt was, so bombing a lemming started
     * tune 12 - which is what "fx only, and it is still playing music"
     * turned out to be. */
    if (g_snd_driver != 0xFF && sound_enabled(1))   /* image 0x02366, 0x02372 */
        sound_request(0x60, 3, 0x0C);               /* image 0x02374 */
    /* A BLOCKER'S CELLS GO BACK FIRST, image 0x02379 / 0x02383, and then
     * image 0x02386..0x023A6 reads the MAP under it afresh - at x >> 2 and
     * (y - 0x10) >> 2 - and refuses on 9 and 5. This read [si+0x2C]
     * instead, the cell the last lookup saw: for a bombed blocker that is
     * its own 0x0A, so it cut a crater through the steel the original's
     * map gives back. A player found it on Fun 11. */
    if (a->flags & 0x0100) block_restore(a);        /* 0x02379, 0x02383 */
    uint8_t under = (uint8_t)(level_trigger_cell((int32_t)a->x / TRIG_CELL,
                        ((int32_t)a->y - TRIG_Y_BIAS) / TRIG_CELL) & 0x0F);
    if (under == 9 || under == 5) return;           /* 0x023AB, 0x023B2 */
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

/* IMAGE 0x0283B - THE NINE CELLS GIVEN BACK. The same column and row image
 * 0x03336 stamped at - (x - 4) >> 2 and (y - 6 - 0x10) >> 2 - and three
 * `movsw / movsb` rows from [si+0x16], 0x1A0 apart, which put back exactly
 * what lem_block_start saved in `under`. Two callers: the blocker losing its
 * floor (image 0x02835) and the cloud of one that was given a bomb (image
 * 0x02383). */
static void block_restore(const lemming *a)
{
    int32_t col = ((int32_t)a->x - BLOCK_BACK_X) / TRIG_CELL;       /* 0x02840 */
    int32_t row = ((int32_t)a->y - BLOCK_BACK_Y - TRIG_Y_BIAS) / TRIG_CELL;
    for (int32_t r = 0; r < 3; r++)
        for (int32_t c = 0; c < 3; c++)
            level_trigger_put(col + c, row + r, a->under[r * 3 + c]);  /* 0x0286B */
}

/* What a blocker DOES, image 0x027E8: sixteen animation frames, and it goes
 * on blocking for as long as there is ground under it. Image 0x027EF probes
 * with the same routine everything else probes with, and when the answer is
 * "nothing there" - dug away beneath it - image 0x027FC puts it back to
 * walking.
 *
 * AND IT GIVES ITS NINE CELLS BACK - image 0x02835 calls image 0x0283B on
 * the way out. This comment used to say the opposite, that the cells stayed
 * stamped "which is the original's behaviour and not an omission", and no
 * routine was cited for it: a blocker whose floor was dug away left an
 * invisible wall behind it in the port and none in the original.
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
    a->extra &= 1;                                  /* image 0x02831 */
    block_restore(a);                               /* image 0x02835 */
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
static uint8_t lem_shrug(lemming *a)
{
    a->anim = (uint8_t)((a->anim + 1) & 7);        /* image 0x02DB8 */
    if (a->anim) return 0;                         /* image 0x02DC1 */
    a->flags = 0;                                  /* image 0x02DC4 */
    a->hot_x = -8;                                 /* image 0x02DE1 */
    a->hot_y = -10;                                /* image 0x02DE6: 0xfff6 */
    a->state = 9;                                  /* image 0x02DEB */
    a->extra &= 1;                                 /* image 0x02DF9 */
    a->extra &= 0x7F;                              /* image 0x02DFD */
    return 1;
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

/* THE PORT'S OWN, and it is the array as DOS left it rather than a state of
 * its own invention.
 *
 * The original clears each record when it RELEASES one - image 0x030F1 - and
 * never resets the array, because ds:0x43 is what says which entries mean
 * anything. Its DGROUP arrives zeroed and stays that way in the slots nothing
 * has used.
 *
 * This used to write 0xFF into every +0x26, which is the port's "not in use".
 * Comparing the array against the original's own at ds:0x0088 - 200 frames,
 * eighty records - that ONE BYTE was the only thing that differed in the whole
 * record: x, y, the hot spot, the flags, the direction, the animation counter,
 * the state, the fuse, the bricks and the nine trigger cells all agreed on
 * every frame of every lemming. A single invented initial value, and it was
 * invisible until the record was laid out at its own offsets.
 *
 * DG is static and outlives a level, so the zeroing is still needed; what is
 * gone is the 0xFF. */
void lemmings_reset(lemming *lem)
{
    memset(lem, 0, (size_t)LEMMING_MAX * sizeof *lem);
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
    if (level_paused() == 0xFF) {
        g_walking = 0;                             /* image 0x01FFF */
        return;
    }
    g_walking = 0xFF;                              /* image 0x01F26 */
    if (n == 0) {                                  /* image 0x01F34 */
        g_walking = 0;                             /* image 0x01FFF */
        return;
    }
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
        /* WHETHER IT LEAVES THROUGH THE LOOKUP. Every handler ends in a jump
         * back to the loop, and there are TWO places to land: image 0x01FE8,
         * which is `call 0x2EC0` - the trigger lookup - and image 0x01FEB,
         * one instruction past it. Thirty-one jumps go to the first and
         * thirty to the second, so `look` is which one this handler's exit
         * took. The ones that never call it say 0 here; the six that do on
         * some paths and not others return it. */
        uint8_t look = 1;
        if (a->extra & 0x80) {                       /* image 0x01F57 */
            look = lem_shrug(a);                     /* image 0x2DB5 */
        }
        else if (f == 0) {
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
            { lem_cloud(a, lvl, cw, chh); look = 0; }   /* image 0x2350 */
        else if (f & 0x0004)
            lem_fall(a, lvl, cw, chh);               /* image 0x21B0 */
        else if (f & 0x2000)
            { lem_exiting(a); look = 0; }            /* image 0x2326 */
        else if (f & 0x8000)
            look = lem_boom(a, lvl, cw, chh);        /* image 0x2D0F */
        else if (f & 0x0020)
            lem_climb(a, lvl, cw, chh);              /* image 0x254F */
        else if (f & 0x0040)
            look = lem_hoist(a);                     /* image 0x2637 */
        else if (f & 0x0200)
            look = lem_bash(a, lvl, cw, chh);        /* image 0x287D */
        else if (f & 0x0100)
            { lem_block(a, lvl, cw, chh); look = 0; }   /* image 0x27E8 */
        else if (f & 0x0080)
            look = lem_build(a, lvl, cw, chh);       /* image 0x2693 */
        else if (f & 0x0010)
            look = lem_dig(a, lvl, cw, chh);         /* image 0x23F3 */
        else if (f & 0x0400)
            lem_floatdown(a, lvl, cw, chh);          /* image 0x2A6D */
        else if (f & 0x0001)
            { lem_splat(a); look = 0; }              /* image 0x22DF */
        else if (f & 0x0008)
            lem_step_up(a, lvl, cw, chh);            /* image 0x23BF */
        else if (f & 0x0800)
            look = lem_mine(a, lvl, cw, chh);        /* image 0x2B35 */
        else if (f & 0x1000)
            { lem_drown(a, lvl, cw, chh); look = 0; }   /* image 0x22F5 */
        else if (f & 0x4000)
            { lem_burn(a); look = 0; }               /* image 0x2340 */

        /* AND A LEMMING THAT DIED IN ITS HANDLER NEVER LOOKS. Every write of
         * 0xFF to [si+0x26] in a handler - image 0x020E2, which the faller,
         * the floater, the digger, the miner, the cloud, the drowning and the
         * burning all jump to, image 0x022EE's splat and image 0x02339's
         * exit - is followed by `jmp 0x1FEB`. */
        if (a->alive == 0xFF) look = 0;

        /* THE TRIGGER IS TESTED AFTER THE MOVE, NOT BEFORE IT.
         *
         * The dispatch at image 0x01F5E does not fall through to the trigger
         * lookup - its handlers JUMP BACK to image 0x01FE8 when they are
         * done, and that is where `call 0x2EC0` sits. Thirty-one jumps land
         * there - and thirty land on image 0x01FEB, past it, which is what
         * `look` above is. This comment used to say EVERY handler went to
         * 0x01FE8: a blocker then looked up its own cell every frame, read
         * its own 0x0A back into [si+0x2C], and a bomb given to it later
         * tested that instead of the steel underneath (image 0x023A6) and
         * cut a crater through steel. A player found it on Fun 11. So the question the original asks is "having
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
        if (look && trig_lv && trig_g) {             /* image 0x01FE8 */
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
                            sound_request(0x61, 4, tgo->trigger_sound);  /* 0x02FF3 */
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
                        sound_request(0x61, 4, 0x11);      /* image 0x03049 */
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
                        sound_request(0x61, 4, 0x0D);      /* image 0x030A0 */
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
/* Image 0x059BB's group 9 is EIGHT frames of 40, not four: four facing each
 * way, the same split the climber has at CLIMB_SPR_LEFT. */
#define FALL_SPR_LEFT   (FALL_SPR_BASE + 4 * 40)
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
 * and one byte along: the digit sits at (+8, -8) from the sprite's origin.
 *
 * The colour is image 0x06F7B's, the blit image 0x07152 calls: Set/Reset 3
 * enabled on planes 1..3 (0x0E01), write mode 0, and the mask byte stored
 * through itself as the bit mask, so plane 0 takes 1 wherever the mask is
 * set. A digit pixel is `mask ? 3 : dst`, eight rows of two bytes. That was
 * first measured off the original's frame 430 - all ten ink pixels on all
 * three lemmings that wear one - and the reading agrees. */
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

/* ======================================================================
 * THE PAGE PAIR - image 0x081CC's set-up, image 0x080FD's copy and the
 * three routines that decide what it copies.
 *
 * The original never recomposes the play area. It keeps two pages in video
 * memory, draws into one while the CRTC shows the other, and swaps them at
 * image 0x00105. Each page has its own array of 20 x 40 flags, one per 8 x 8
 * tile, and a flag means "something was drawn over the master here". Image
 * 0x080FD runs at the top of every frame and copies only the flagged tiles
 * from the master at 0x4A40 back onto the page being drawn, then clears the
 * flags. The objects and the lemmings are then drawn onto the page, and each
 * marks the tiles it covered, so two frames later, when this page is drawn
 * into again, those tiles are put back before anything new goes over them.
 *
 * What it does NOT do is notice the master changing underneath. A digger, a
 * basher, a miner and a bomb all write the master directly - image 0x05EBA
 * is a bare stosb run - and mark nothing. Their work reaches a page only
 * where some sprite happened to mark the tile, so a pixel they clear outside
 * every marked tile stays on that page until a scroll redraws it. That is
 * the original's behaviour, a player can see it, and it is why this is here.
 *
 * THE MARKERS. There are three and they are separate routines:
 *
 *     image 0x09A15  a rectangle, called from inside the blitter at image
 *                    0x0885B when ds:0x2A is 0xFF. In a level the only blits
 *                    are the objects, image 0x07BF9.
 *     image 0x05726  a lemming, called by image 0x03654 before every draw.
 *     image 0x06F01  one tile, for each pixel of a bomber's debris.
 *
 * Image 0x07178, which docs/page-model.md once named as each lemming's
 * background save, is the CURSOR's: image 0x05827 calls it straight after
 * the flip, and its restore puts back exactly what its save took, so the
 * cursor leaves nothing on a page. The port drawing it last over the window
 * is the same picture.
 *
 * THE PAGES ARE CANVASES THE MASTER'S SIZE, which is the port's own
 * arrangement: the original's are 44 bytes wide, but a window that only ever
 * moves by redrawing the whole of itself (image 0x0413B) can as well live at
 * the master's coordinates, and then every draw in this file keeps the
 * coordinates it already has. The tiles are the screen's: column c of a row
 * is canvas x view_x + 8c.
 * ====================================================================== */

/* The two flag arrays live in the STACK segment, like the trigger map: image
 * 0x081DE puts them at ss:0x42B2 and ss:0x45D3, each 0x321 bytes - a
 * redraw-everything byte, then 0x320 tiles. They are contiguous, which
 * matters: image 0x09A15 bounds a rectangle only between rows, so the first
 * row of one can run off the end of its array into the next. The slack is
 * for the same thing running off the second. */
#define PAGE_FLAGS_SS    0x42B2u            /* image 0x081DE */
#define PAGE_FLAGS_SS2   0x45D3u            /* image 0x081E4 */
#define PAGE_FLAGS_LEN   0x321u             /* image 0x09A29 */
#define PAGE_TILES_W     0x28u              /* image 0x08140: 40 a row */
#define PAGE_TILES_H     0x14u              /* image 0x0813C: 20 rows */
#define PAGE_SHOWN_INIT  0x2520u            /* image 0x081D2 */
#define PAGE_X_LIMIT     0x160              /* ds:0x66F2, image 0x0821C */
#define PAGE_Y_LIMIT     0xA0               /* ds:0x66F0, image 0x0821F */

static uint8_t g_page_tiles[2 * PAGE_FLAGS_LEN + 0x40];

/* One byte of the stack segment's flag area. Not transcribed - it is the
 * port's bounds on a write the original makes without any: a degenerate
 * rectangle (width 0 after clipping) would have image 0x09A15 store 65,536
 * ones through ss. No level reaches it. */
static void page_tile_set(uint32_t ss)
{
    if (ss >= PAGE_FLAGS_SS && ss - PAGE_FLAGS_SS < sizeof g_page_tiles)
        g_page_tiles[ss - PAGE_FLAGS_SS] = 1;
}

/* IMAGE 0x09A15 - mark the tiles a rectangle covers, on the page being
 * drawn. ax is the blitter's x, which carries the page's sixteen-pixel left
 * margin; bx the row; cl the rows and dx the width in pixels. A rectangle
 * that starts part-way into a tile reaches one further, which is the two
 * `add 8`s, and the bound at image 0x09A81 is tested only after a row has
 * been written. */
static void page_mark_rect(int16_t ax, int16_t bx, uint8_t cl, uint16_t dx)
{
    ax = (int16_t)(ax - 0x10);                      /* image 0x09A1C */
    if (ax < 0) ax = 0;                             /* image 0x09A21 */
    uint16_t si = DG.page_flags;                    /* image 0x09A23 */
    uint16_t bp = (uint16_t)(si + PAGE_FLAGS_LEN);  /* image 0x09A29 */
    si++;                                           /* image 0x09A2D */
    if (ax & 7) dx = (uint16_t)(dx + 8);            /* image 0x09A33 */
    si = (uint16_t)(si + ((uint16_t)ax >> 3));      /* image 0x09A3C */
    if (bx & 7) cl = (uint8_t)(cl + 8);             /* image 0x09A44 */
    uint16_t row = (uint16_t)bx & 0xFFF8u;          /* image 0x09A47 */
    si = (uint16_t)(si + row + (uint16_t)(row << 2));   /* image 0x09A4A */
    if (cl & 7) cl = (uint8_t)(cl + 8);             /* image 0x09A57 */
    uint16_t cx = (uint16_t)(cl >> 3);              /* image 0x09A5A */
    if (dx & 7) dx = (uint16_t)(dx + 8);            /* image 0x09A68 */
    dx = (uint16_t)(dx >> 3);                       /* image 0x09A6B */
    do {
        uint16_t n = dx;
        uint16_t at = si;
        do page_tile_set(at++); while (--n && at - si < 0x100);
        si = (uint16_t)(si + PAGE_TILES_W);         /* image 0x09A7E */
        if (si >= bp) break;                        /* image 0x09A81 */
    } while (--cx);                                 /* image 0x09A85 */
}

/* IMAGE 0x07BB8's MARKS - the half of each object's blit that is not the
 * drawing. Image 0x07BCF takes the object's x to the page (`sub ax,[0] /
 * add ax,0x10`), the blitter clips it at image 0x09967 when the flag word
 * asks, and image 0x0885B marks what survived if ds:0x2A is 0xFF. The
 * drawing is level_anim_draw; the two read the same object and are kept
 * apart because the port draws at the master's coordinates.
 *
 * The clip, image 0x09967, is in eights: an object hanging off the left edge
 * loses whole bytes until its x is non-negative, and one off the right loses
 * them until it fits the page's 0x160. Off the top it loses rows; a
 * rectangle with nothing left in it is refused before it is marked. */
static void level_anim_mark(const level_anim *st, const level *lv,
                            uint16_t view_x)
{
    if (DG.page_mark != 0xFF) return;               /* image 0x08853 */
    for (uint16_t i = 0; i < lv->num_objects && i < LEVEL_ANIM_MAX; i++) {
        const level_anim *a = &st[i];
        if (!a->live || !a->w || !a->rows) continue;    /* image 0x07BCA */
        const level_object *o = &lv->objects[i];
        /* level_parse took the 16 off o->x, and ds:0x00 is view_x + 16 */
        int16_t  ax = (int16_t)(o->x - (int16_t)view_x + 0x10); /* 0x07BCF */
        int16_t  bx = o->y;                         /* image 0x07BD6 */
        int16_t  dx = a->w;                         /* image 0x07BE7 */
        int16_t  cx = a->rows;                      /* image 0x07BEC */
        if (o->bp & 4) {                            /* image 0x0996E */
            if (ax < 0) {
                if (ax + dx < 0) continue;          /* image 0x0997C */
                do { dx -= 8; ax += 8; } while (ax < 0);  /* image 0x0998C */
            } else {
                if (ax >= PAGE_X_LIMIT) continue;   /* image 0x09997 */
                while (ax + dx >= PAGE_X_LIMIT) dx -= 8;  /* image 0x099A1 */
            }
        }
        if (o->bp & 8) {                            /* image 0x099B6 */
            if (bx < 0) {
                if (bx + cx < 0) continue;          /* image 0x099C4 */
                cx += bx;                           /* image 0x099C6 */
                bx = 0;                             /* image 0x099E0 */
            } else {
                if (bx >= PAGE_Y_LIMIT) continue;   /* image 0x099E8 */
                if (bx + cx >= PAGE_Y_LIMIT)
                    cx = PAGE_Y_LIMIT - bx;         /* image 0x099F8 */
                if (!dx || !cx) continue;           /* image 0x099FE */
            }
        }
        page_mark_rect(ax, bx, (uint8_t)cx, (uint16_t)dx);  /* 0x0885B */
    }
}

/* IMAGE 0x07070 - is this lemming on screen at all. A table lookup rather
 * than a comparison: the drawer takes `x + hot_x`, wraps a negative one by
 * the page's 0x160, takes its BYTE column and refuses the whole sprite
 * unless ds:0x0EA8[column] holds 1 (image 0x07086). Image 0x04113 clears
 * that table and image 0x0412B marks exactly 41 bytes of it starting two
 * byte columns left of the scroll, so the window is
 * (scroll >> 3) - 2 .. (scroll >> 3) + 38. Image 0x05726 asks the same
 * question of the same table at image 0x0573D. */
static int lemming_on_screen(const lemming *a, uint16_t view_x)
{
    int32_t col = (int32_t)a->x + a->hot_x;         /* image 0x07070 */
    if (col < 0) col += 0x160;                      /* image 0x07077 */
    col >>= 3;                                      /* image 0x0707D */
    int32_t first = (((int32_t)view_x + 16) >> 3) - 2;  /* image 0x04125 */
    return col >= first && col < first + 41;        /* image 0x0412B: 0x29 */
}

/* IMAGE 0x05726 - mark the tiles a lemming is about to be drawn over. Image
 * 0x03654 calls it for every lemming still alive, just before image
 * 0x0706F draws it, whatever it is drawn as.
 *
 * The rectangle starts eight rows above the hot spot, sixteen with a fuse
 * burning, because image 0x0714E puts the digit there. It is three tiles
 * wide - five while the cloud bit 0x0002 is set (image 0x05799) - and as
 * tall as that top is above the feet. A top above the play area is walked
 * down in eights at image 0x0577B and then put back one step, so it is not
 * clipped so much as started at row 0. */
static void lemming_mark(const lemming *a, uint16_t view_x)
{
    if (!lemming_on_screen(a, view_x)) return;      /* image 0x0573D */
    int16_t ax = (int16_t)(a->x + a->hot_x);        /* image 0x05726 */
    if (ax < 0) ax = (int16_t)(ax + 0x160);         /* image 0x0572D */
    ax = (int16_t)(ax - (int16_t)(view_x + 16));    /* image 0x05745 */
    if (ax < 0) ax = 0;                             /* image 0x0574B */
    uint16_t si = (uint16_t)(DG.page_flags + 1 + ((uint16_t)ax >> 3));
    uint16_t bp = (uint16_t)(DG.page_flags + PAGE_FLAGS_LEN);   /* 0x0575B */
    int16_t cx = (int16_t)(a->hot_y - 8);           /* image 0x05765 */
    if (a->fuse) cx = (int16_t)(cx - 8);            /* image 0x05771 */
    int16_t y = (int16_t)(a->y + cx - 0x10);        /* image 0x05774 */
    if (y < 0) {
        do { cx = (int16_t)(cx + 8); y = (int16_t)(y + 8); }
        while (y < 0);                              /* image 0x0577B */
        y = 0;                                      /* image 0x05783 */
        cx = (int16_t)(cx - 8);                     /* image 0x05785 */
    }
    cx = (int16_t)-cx;                              /* image 0x05788 */
    uint16_t row = (uint16_t)y & 0xFFF8u;           /* image 0x0578C */
    si = (uint16_t)(si + row + (uint16_t)(row << 2));
    uint8_t cols = (a->flags & 0x0002) ? 5 : 3;     /* image 0x05797 */
    uint8_t h = (uint8_t)cx;                        /* image 0x057A2 */
    uint8_t rows = (uint8_t)(h >> 3);               /* image 0x057A4 */
    if (y & 7) rows++;                              /* image 0x057AF */
    if (h & 7) rows++;                              /* image 0x057B5 */
    do {
        for (uint8_t c = 0; c < cols; c++)          /* image 0x057BB */
            page_tile_set((uint16_t)(si + c));
        si = (uint16_t)(si + PAGE_TILES_W);         /* image 0x057C5 */
        if (si >= bp) break;                        /* image 0x057C8 */
    } while (--rows);                               /* image 0x057CC */
}

/* IMAGE 0x06F01 - one tile, for one pixel of a bomber's debris. bp is the
 * page x with its sixteen-pixel margin, dx the row; image 0x06EF2 has
 * already refused anything off the page. */
static void lemming_mark_particle(int32_t bp, int32_t dx)
{
    int32_t col = bp - 0x10;                        /* image 0x06F06 */
    if (col < 0) col = 0;                           /* image 0x06F0B */
    page_tile_set((uint32_t)(DG.page_flags + 1 + (col >> 3)
                             + (dx & ~7) * 5));     /* image 0x06F20 */
}

/* IMAGE 0x080FD - put the master back where the page was drawn over.
 *
 * Byte 0 of the array is REDRAW EVERYTHING (image 0x0811C), and that path,
 * image 0x0819D, copies the whole window and clears byte 0 alone - the
 * tiles stay flagged and are copied again next time, which changes nothing.
 * Otherwise image 0x0813F walks the twenty rows, finds a run of flagged
 * tiles with `repne scasb` and its end with `repe scasb`, and copies it.
 *
 * THE RUN IS ONE TILE LONGER THAN ITS FLAGS whenever it ends on an unflagged
 * one: image 0x08176's length is the count before the `repe` less the count
 * after it, plus one, and the `repe` has already consumed the tile that
 * stopped it. Only a run reaching the end of the row is exact. That is
 * transcribed with the two counts as the registers had them rather than as
 * "a run", because it is where a tidier reading would differ.
 *
 * Every flag is then cleared at image 0x08153.
 *
 * The full copy is 44 bytes a row from ds:0x02, which reaches 16 pixels
 * either side of the screen and wraps the last two bytes onto the next
 * page row - all of it outside the 320 the CRTC shows. This copies the 320,
 * because a canvas-wide page has no next row to wrap onto and nothing reads
 * the margin back. */
static void level_page_refresh(const canvas *m, uint8_t *page,
                               uint16_t view_x)
{
    uint8_t *f = &g_page_tiles[DG.page_flags - PAGE_FLAGS_SS];
    if (f[0]) {                                     /* image 0x0811C */
        f[0] = 0;                                   /* image 0x0819D */
        for (uint16_t y = 0; y < LEVEL_PLAY_H; y++) /* image 0x081BA: 0xA0 */
            memcpy(page + (size_t)y * m->w + view_x,
                   m->pix + (size_t)y * m->w + view_x, LEVEL_VIEW_W);
        return;
    }
    const uint8_t *t = f + 1;                       /* image 0x08124 */
    for (uint16_t r = 0; r < PAGE_TILES_H; r++, t += PAGE_TILES_W) {
        uint16_t di = 0;                            /* into this row */
        uint16_t cx = PAGE_TILES_W;                 /* image 0x08140 */
        for (;;) {
            /* image 0x08143: repne scasb for a 1 */
            int found = 0;
            while (cx) { cx--; if (t[di++] == 1) { found = 1; break; } }
            if (!found) break;                      /* image 0x08145 */
            uint16_t before = cx;                   /* image 0x08160 */
            /* image 0x08161: repe scasb while 1 */
            while (cx) { cx--; if (t[di++] != 1) break; }
            uint16_t p   = (uint16_t)(PAGE_TILES_W - (before + 1)); /* 0x08170 */
            uint16_t len = (uint16_t)(before + 1 - cx);   /* image 0x08176 */
            for (uint16_t y = 0; y < 8; y++) {      /* image 0x08185: al = 8 */
                size_t at = (size_t)(r * 8u + y) * m->w + view_x + p * 8u;
                memcpy(page + at, m->pix + at, (size_t)len * 8u);
            }
            if (!cx) break;                         /* image 0x08199 */
        }
    }
    memset(f + 1, 0, PAGE_TILES_W * PAGE_TILES_H);  /* image 0x08153 */
}

/* The page ds:0x1F76 names - the one being drawn. */
static uint8_t *level_page_drawn(const struct level_frame *A)
{
    return A->page[DG.page_draw == 0 ? 0 : 1];      /* image 0x081CC's two */
}

/* IMAGE 0x00105's TWO EXCHANGES - the pages, and their flag arrays with
 * them, so ds:0x26 always belongs to the page being drawn. */
static void level_pages_swap(void)
{
    uint16_t t = DG.page_shown;                     /* image 0x00105 */
    DG.page_shown = DG.page_draw;
    DG.page_draw = t;
    t = DG.page_flags_other;                        /* image 0x0010F */
    DG.page_flags_other = DG.page_flags;
    DG.page_flags = t;
}

/* IMAGE 0x0413B - REDRAW EVERYTHING, on both pages. Image 0x0410F does it
 * after every scroll, from the edge (image 0x04100, image 0x0416A) and from
 * the minimap (image 0x03FDE), because a page that only refreshes what was
 * drawn over cannot follow the master sliding underneath it. */
static void level_pages_redraw_all(void)
{
    g_page_tiles[DG.page_flags - PAGE_FLAGS_SS] = 0xFF;         /* 0x0413F */
    g_page_tiles[DG.page_flags_other - PAGE_FLAGS_SS] = 0xFF;   /* 0x04147 */
}

/* IMAGE 0x081CC - the pages for a new level. Both are drawn whole from the
 * master, the first before the swap at image 0x08202 and the second after
 * it, so the loop's own first swap starts it drawing on page 0 with page
 * 0x2520 on show. ds:0x2A turns the blitter's marks on for the level.
 *
 * Image 0x08216 calls image 0x039CE between the two, the panel's counters;
 * that is level_panel_build's business and not the pages'. Image 0x08226
 * points the blitter at the page and image 0x0822B the cursor's two save
 * buffers at 0xD2C0 and 0xD2F0, which this port's cursor does not need. */
static void level_pages_init(struct level_frame *A)
{
    memset(g_page_tiles, 0, sizeof g_page_tiles);
    DG.page_draw = 0;                               /* image 0x081CC */
    DG.page_shown = PAGE_SHOWN_INIT;                /* image 0x081D2 */
    DG.page_flags = PAGE_FLAGS_SS;                  /* image 0x081DE */
    DG.page_flags_other = PAGE_FLAGS_SS2;           /* image 0x081E4 */
    level_pages_redraw_all();                       /* image 0x081EA */
    DG.page_mark = 0xFF;                            /* image 0x081FA */
    level_page_refresh(A->from, level_page_drawn(A), A->view_x); /* 0x081FF */
    level_pages_swap();                             /* image 0x08202 */
    level_page_refresh(A->from, level_page_drawn(A), A->view_x); /* 0x08219 */
}

/* Image 0x070A2 onwards - the blit that draws one lemming, which the port
 * splits by state because it indexes MAIN.DAT rather than the pre-shifted
 * store the original blits from. */
/* WHY THIS FUNCTION IS A CHAIN OF FLAGS AND THE ORIGINAL'S IS NOT.
 *
 * The record's +0x14 is not a sprite base. It is a FUNCTION POINTER: image
 * 0x070F0 pushes `[di+0x14]` and image 0x0715B is `call ax` after popping
 * it, so each state carries the address of the routine that draws it. That
 * is why every conversion in this file ends with the same two lines - image
 * 0x022CE's `ax = ds:0x2E` then `[si+0x14] = ax`, and its siblings - which
 * the port has been transcribing as "the sprite base" without a name for
 * what ds:0x2E holds. It holds the WALKER's draw routine.
 *
 * AND THE POINTERS COME FROM A TABLE AT ds:0x2E. There are 32 writes to
 * [si+0x14] in the image and they load from nine different words, two bytes
 * apart, which is the dispatch table this port replaces with its flag chain:
 *
 *     ds:0x2E   most conversions          the ordinary walk
 *     ds:0x30   image 0x021A2
 *     ds:0x32   image 0x02F70             beside the cloud, image 0x02F51
 *     ds:0x34   image 0x03536             the BASHER, image 0x034CE
 *     ds:0x36   image 0x0360B             the MINER, image 0x03598
 *     ds:0x38   image 0x0325F             the DIGGER, image 0x0320D
 *     ds:0x3A   image 0x021F4
 *     ds:0x3C   image 0x02D48, image 0x02E90
 *     ds:0x3E   image 0x03087
 *
 * Four are identified by the routine that assigns them; the rest are not,
 * and are listed by address rather than guessed at. What matters here is
 * that "the sprite base" was never one thing - a state does not carry where
 * its artwork is, it carries WHO DRAWS IT, and the artwork is that routine's
 * own business.
 *
 * So the original dispatches by pointer and this dispatches on the flag
 * word. Both reach the same drawing for the same state; the port's is an
 * arrangement, and this says so rather than leaving the reader to wonder why
 * a transcription looks nothing like its original.
 *
 * It also explains the shape of image 0x0706F: everything before image
 * 0x0715B is common - the on-screen test at image 0x07086, the frame index
 * at image 0x070A2, the store address at image 0x070B6, the video offset at
 * image 0x07100 - and only the last step differs per state.
 */
void lemming_draw(uint8_t *dst, uint16_t dst_w, uint16_t dst_h, uint16_t view_x,
                  const uint8_t *sec, uint32_t sec_len, const lemming *a)
{
    /* THE DIRECTION RULE IS THE ORIGINAL'S STORE'S, NOT THIS PORT'S INDEX.
     *
     * Image 0x070A2 computes the frame as the animation counter, and image
     * 0x070AB ADDS [di+0x29] - the state - when [di+0x27] is not 1, so a
     * lemming facing the other way draws from further along. That reading is
     * right about the original and does NOT transfer here, and the
     * measurement is unambiguous: applied at this line, three of the seven
     * levels that are exact on all 1,100 frames collapsed to 88, 103 and 91,
     * with residues of 209, 275 and 305 pixels.
     *
     * Why: the original indexes its PRE-SHIFTED STORE, where the two
     * directions are laid out one after the other and `+ state` reaches the
     * second set. This port indexes MAIN.DAT, whose layout is the file's -
     * which is exactly what the note above this function already says, and
     * which is why lemming_draw splits by state at all. The same arithmetic
     * over a different layout is a different picture.
     *
     * Kept as a comment because the reading cost a measurement to disprove
     * as a change, and the next person to find image 0x070AB should not
     * have to spend it again. */
    uint16_t frame = a->anim;
    uint32_t base;
    int32_t x0, y0;

    if (a->alive == 0xFF) return;

    /* IS THIS LEMMING ON SCREEN AT ALL - see lemming_on_screen.
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
    if (!lemming_on_screen(a, view_x)) return;      /* image 0x0708B */

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
                    if (py < 0 || py >= (int32_t)dst_h) continue;
                    /* Marked before the canvas test, which is the port's:
                     * a particle in the page's margin left of a level
                     * scrolled hard left is off the canvas and still on the
                     * page, and image 0x06F01 marks it all the same. */
                    lemming_mark_particle(sx + 16, py);  /* image 0x06F01 */
                    if (px < 0 || px >= (int32_t)dst_w) continue;
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
        else if (a->flags & 0x0004) {
            /* THE FALLER HAS TWO DIRECTIONS, like the climber. Group 9 of
             * image 0x059BB's table is EIGHT frames of 40 from 14320 where
             * this drew four, and the climber - group 3, confirmed - splits
             * its sixteen as eight right then eight left with
             * CLIMB_SPR_LEFT at exactly half. The same split here puts the
             * left-facing set at 14320 + 4 * 40.
             *
             * That is what image 0x070AB reaches by adding the state; this
             * port picks a base instead, which is how it already handles the
             * climber. */
            sb = (a->dir == 1) ? FALL_SPR_BASE : FALL_SPR_LEFT;
            sh = 10; sp = 2; sf = 4;
        }
        if (sb) {
            if (a->anim >= sf) return;
            uint32_t plane = 2u * sh;
            uint32_t fsz = plane * sp;
            uint16_t fr = a->anim;
            /* THE FLOATER FACES BOTH WAYS. Image 0x070AB adds the state when
             * the lemming does not face right, and a floater's is 8 - so its
             * left-facing eight follow the right-facing ones, which is the
             * two-way shape tools/tile_sprites.py already measures for this
             * region. The port drew the right-facing set whichever way it
             * faced; ref_l2f's floater faces right, so nothing caught it until
             * Fun 10 had one going left. */
            if ((a->flags & 0x0400) && !(a->flags & (0x4000 | 0x1000))
                    && a->dir != 1)
                fr = (uint16_t)(fr + a->state);     /* image 0x070AB */
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
 * The game keeps it as a 13-byte by 19-row bitmap stashed off-screen at
 * 0xA000:0xD320 and rebuilds ONE byte-column of it a frame, cycling ds:0x56
 * by 16 and wrapping at 0xD0 - thirteen frames to refresh the lot. Image
 * 0x03779 builds all thirteen when the level is composed; after that a
 * column is only as fresh as its turn.
 *
 * THAT IS VISIBLE, and the port used to say it was not. It built the whole
 * map every frame on the grounds that "the result is the same bitmap", which
 * holds for a level nothing digs into. A miner, a basher or a bomb changes
 * the master, and the original's map shows the change only when that
 * column's turn comes round - up to thirteen frames late. Measured on the
 * miner's run: one minimap pixel, (258..260, 187..189), where the original
 * still showed ink 7 for several frames after the port had cleared it, on 16
 * of 399 frames. That is the whole of what this store is for.
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
 * THE LOOSE END THAT WAS HERE is settled: the map sat at a fitted 241 to make
 * up for sampling the canvas sixteen pixels right of the master, and it is at
 * the byte-aligned 240 now - see MINIMAP_AT_X. The copy still draws only the
 * ink: the original's writes all thirteen bytes a row, background included,
 * and whether that now lands on the artwork's frame correctly is a separate
 * measurement nobody has taken.
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
 * `scroll` is ds:0x00, the port's view_x plus sixteen. Checked against
 * recorded frames: with view_x at 624, ds:0x00 is 640 and 640 >> 4 is 40, so
 * the box starts at minimap pixel 40, which is screen x 248 - and 248..271 is
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

/* 0xA000:0xD320 - THE MINIMAP'S STORE, one bit a pixel, thirteen bytes a
 * row. Image 0x07A0D writes NINETEEN rows of it (cx = 0x13) though image
 * 0x079F1 builds twenty: the twentieth is computed and dropped. */
#define MINIMAP_COLS   13       /* image 0x07984: 0xD0 / 0x10 */
#define MINIMAP_KEPT   19       /* image 0x07A0D: cx = 0x13 */
static uint8_t g_minimap[MINIMAP_KEPT][MINIMAP_COLS];

/* IMAGE 0x0796F - rebuild one column of the store.
 *
 * Image 0x0797D steps ds:0x56 by 0x10 and wraps it to 0 at 0xD0, so the
 * column is ds:0x56 >> 4 and each call takes the next. The twenty rows are
 * built into cs:0x83C1 first - image 0x07970 clears it - by image 0x07A25,
 * which walks the column's eight pixels from bit 0x80 down, and image
 * 0x07A39, which counts the terrain bits under one of them and sets it on a
 * majority. Then image 0x07A14 copies nineteen of them into the store. */
void minimap_column_step(const uint8_t *lvl, uint16_t cw, uint16_t chh)
{
    uint8_t col[MINIMAP_H];                         /* cs:0x83C1 */
    memset(col, 0, sizeof col);                     /* image 0x07970 */
    uint16_t si = (uint16_t)(DG.minimap_col + 0x10);    /* image 0x07981 */
    if ((int16_t)si >= 0xD0) si = 0;                /* image 0x07984 */
    DG.minimap_col = si;                            /* image 0x0798C */
    uint16_t c = (uint16_t)(si >> 4);               /* image 0x0799A */
    for (uint16_t r = 0; r < MINIMAP_H; r++) {      /* image 0x079F1: 0x14 */
        uint16_t y = (uint16_t)(MINIMAP_YFROM + r * MINIMAP_YSTEP);
        for (uint8_t k = 0; k < 8; k++) {           /* image 0x07A29 */
            uint16_t p = (uint16_t)(c * 8u + k);
            uint8_t n = 0;
            if (y < chh) {
                /* Image 0x07996 reads the MASTER from byte 0, which is canvas
                 * x -16: the canvas is the master less sixteen. */
                int32_t sx = (int32_t)p * MINIMAP_XSTEP - 16;
                for (uint8_t b = 0; b < MINIMAP_XSTEP; b++)
                    if (sx + b >= 0 && sx + b < cw
                            && lvl[(size_t)y * cw + (size_t)(sx + b)])
                        n++;                        /* image 0x07A46 */
            }
            if (n > MINIMAP_VOTE)                   /* image 0x07A4E */
                col[r] = (uint8_t)(col[r] | (0x80u >> k));  /* image 0x07A55 */
        }
    }
    for (uint16_t r = 0; r < MINIMAP_KEPT; r++)     /* image 0x07A0D: 0x13 */
        g_minimap[r][c] = col[r];                   /* image 0x07A1A */
}

/* IMAGE 0x03779 - the whole store, thirteen calls, when the level is
 * composed. It leaves ds:0x56 where it found it. */
void minimap_build(const uint8_t *lvl, uint16_t cw, uint16_t chh)
{
    for (uint8_t i = 0; i < MINIMAP_COLS; i++)      /* image 0x03779: 0x0D */
        minimap_column_step(lvl, cw, chh);          /* image 0x0377D */
}

/* The store onto the panel - image 0x07900's copy of 0xD320, which image
 * 0x001AA makes every frame. Row r of the store is the master's row
 * MINIMAP_YFROM + 8r, as it was when its column was last rebuilt. */
void panel_minimap(uint8_t *dst, uint16_t dst_w, uint16_t dst_h,
                   uint16_t x0, uint16_t y0)
{
    for (uint16_t r = 0; r < MINIMAP_KEPT; r++) {
        uint16_t py = (uint16_t)(y0 + r);
        if (py >= dst_h) break;
        for (uint16_t p = 0; p < MINIMAP_W; p++) {
            uint16_t px = (uint16_t)(x0 + p);
            if (px >= dst_w) break;
            uint8_t on = (uint8_t)(g_minimap[r][p >> 3] & (0x80u >> (p & 7)));
            /* Ink only. The original's copy writes all thirteen bytes a
             * row, background included, but doing that here rubs out part of
             * the artwork's own frame - which says the 104-pixel span is not
             * quite in the right place yet, and the note above records it. So
             * this leaves what is underneath: measured, that is 137 differing
             * pixels against 232 for clearing. */
            if (on)
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
 *     image 0x019D  call 0x031E   the phase machine: the fades and the cues
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
    level_phase_step();
    level_release_step();
    (void)st; (void)lv;
    level_timer_step();
    /* THE TWO COUNTS ARE NOT SET HERE, and they used to be.
     *
     * OUT is image 0x3684's, the tail of the walk image 0x01BC calls, and IN
     * is image 0x03ACE's, inside the panel image 0x01C2 calls. Both of those
     * run AFTER image 0x01B3 has stepped the lemmings; this routine is image
     * 0x0019D's share and runs before it. Setting them here read the counts
     * one step early, so a lemming that reached the exit during the step
     * showed up in the panel on the NEXT frame - measured as ten frames of
     * one character cell, the IN percentage's tens digit, over a 671-frame
     * playthrough. They are now at the two addresses that own them. */
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
/* ======================================================================
 * THE PALETTE SERVICE - image 0x052F4..0x0570C, and it is a TABLE OF EIGHT.
 *
 * Every palette in the program is a slot in a table of eight pointers at
 * CS:0x52D4, and a second table at CS:0x52E4 that the adapter word ds:0x1F7C
 * selects between: 4 means the DAC, anything else the EGA's attribute
 * registers. The EGA table's entries are one byte to a colour and the DAC's
 * are three, which is why image 0x053CA adds 8 to one and 0x18 to the other
 * to reach the same eighth entry.
 *
 *   slot  EGA       DAC       what it is
 *   ----  --------  --------  -----------------------------------------
 *    0    ds:0x2638 ds:0x2638 the palette being SHOWN - what a fade walks
 *    1    ds:0x2608 ds:0x2608 a second working buffer
 *    2    ds:0x4920 ds:0x4930 THE MENU'S, and its bytes are MENU_PAL6 here
 *    3    ds:0x1F13 ds:0x1F3B
 *    4    ds:0x25E8 ds:0x25F0 THE PANEL'S - image 0x0C080, which CLAUDE.md
 *                             already names as the eight the level swaps in
 *    5    ds:0x1F0B ds:0x1F23
 *    6    ds:0x25A8 ds:0x25B8 NOT IDENTIFIED - see below
 *    7    ds:0x2668 ds:0x2668
 *
 * SLOT 6 IS NOT IDENTIFIED, and was called "the level's" here on nothing but
 * its position between the panel's and the working buffers. Checked: its
 * eight entries at image 0x0C048 are
 *
 *     000000 101038 002c00 3c3434 2c2c00 3c0808 202020 382008
 *
 * - black, dark blue, green, near-white, olive, red, grey, orange - and they
 * match NONE of the palettes this port carries: not MENU_PAL6, not
 * PANEL_PAL6, not BRIEF_PAL6, not BRIEF_LOWER6, and not RESULTS_PAL widened.
 * Slot 4 by contrast is PANEL_PAL6 byte for byte, so the comparison works
 * and the negative is real.
 *
 * Left unnamed rather than guessed. A palette that matches nothing the port
 * draws is either a screen this port has not reached or a default something
 * overwrites, and those are different facts.
 *
 * The selectors over it, and who calls each:
 *
 *   image 0x0536C  fade slot 0 to slot 2, sixteen entries - THE FADE IN, and
 *                  every screen runs it: image 0x04450 (the menu), image
 *                  0x047A6 (results), image 0x04807, image 0x04993 (the code
 *                  screen), image 0x04A2B (the input chooser), image 0x04AE5
 *                  and image 0x04B38. Seven callers, one routine.
 *   image 0x052F4  show slot 1, sixteen - image 0x00465, the level's set-up
 *   image 0x05377  copy slot 5 into slot 6 - image 0x00486, also the level's
 *   image 0x053CA  show slot AL's UPPER EIGHT, registers 8..15 - image
 *                  0x0466E and image 0x046A8. This is the swap CLAUDE.md
 *                  records: the play area keeps the terrain's colours and
 *                  the panel gets its own.
 *   image 0x053F6  show slot AL, eight   (image 0x053FC picks slot 2 for it)
 *   image 0x053FE  show slot AL, sixteen - image 0x04600
 *   image 0x05447  a variant nothing in a VGA run reaches
 *   image 0x05473  image 0x017DD, and that is its ONLY caller. A second at
 *                  image 0x0524C was written here and is a false positive
 *                  from the byte-scan CLAUDE.md warns about: those bytes are
 *                  `and al, 2` inside image 0x051A2's bit decode. The code
 *                  map says one caller and it is right.
 *   image 0x05569  the two-slot selector a fade is given, si = table[bl] and
 *                  di = table[bh] - image 0x05329 and image 0x0534B, the
 *                  level's own fade in and out, both go through it
 *
 * And two writers, which are the CARD and belong in sdl_io.c by this port's
 * own rule - they replace something a card did:
 *
 *   image 0x05425  CX attribute registers from si, starting at register CH
 *   image 0x0548C  CX DAC entries from si, from CH, with ds:0x200C's blank
 *
 * THE EGA HALF IS A NON-GOAL. This port reconstructs vgalemmi.exe's VGA
 * path only - CLAUDE.md's first page says so - so image 0x05425 and the EGA
 * fade step at image 0x0563E, with its component helpers at image 0x0566C,
 * image 0x05685, image 0x05696 and image 0x0569B, are named here and not
 * written. They are the other adapter, not a gap: ds:0x1F7C is 4 in every
 * run this repository makes.
 *
 * TWO MORE OF THE CARD'S, in the same range and the same category:
 *
 *   image 0x0587F  restore the sequencer and graphics-controller state from
 *                  a saved block at 0x244E - map mask, read mode, function
 *                  select, bit mask. Pure card state; the port has no card.
 *   image 0x05726  the per-lemming blit ADDRESS, called from image 0x03654
 *                  inside the lemming draw: it takes the record's x and hot
 *                  spot, tests a per-column table at ds:0x0EA8 for 1, and
 *                  builds a video offset from ds:0 - the scroll - and
 *                  ds:0x26, the page. The port draws into a canvas at a
 *                  known stride and needs no such address.
 *
 * WHAT THE PORT DOES INSTEAD. It has no card and no 64 KB segment, so a slot
 * is a `lem_rgb[16]` owned by whatever screen needs it, and the fade walks
 * that. The SELECTION is what transcribes - which slot, how many entries,
 * and from where - and the numbers above are why level_panel_palette exists
 * and why it is eight and not sixteen.
 * ====================================================================== */
#define PAL_SLOTS       8        /* CS:0x52D4 and CS:0x52E4 */
#define PAL_SLOT_SHOWN  0        /* what a fade walks */
#define PAL_SLOT_MENU   2        /* MENU_PAL6's bytes */
#define PAL_SLOT_PANEL  4        /* image 0x0C080 */
#define PAL_SLOT_LEVEL  6
/* Image 0x053CA: the split writes EIGHT entries starting at register 8. */
#define PAL_SPLIT_FIRST 8
#define PAL_SPLIT_COUNT 8

/* Image 0x053CA's two numbers: `cx = 0x808` is eight entries from register
 * eight. See the palette block above. */
uint8_t palette_split_first(void) { return PAL_SPLIT_FIRST; }
uint8_t palette_split_count(void) { return PAL_SPLIT_COUNT; }

/* ======================================================================
 * IMAGE 0x055B7 - THE FADE LOOP, and it does not run at the display's rate.
 *
 *     si = table[bl]      the palette buffer being shown
 *     di = table[bh]      the one it is heading for
 *   loop:
 *     0x055E1  call 0x5425   write cx entries from si at the card
 *     0x055E9  call 0x563E   step si one notch toward di; ZF when arrived
 *     0x055EC  je            arrived: return
 *     0x055EE  call 0x1550   \
 *     0x055F1  call 0x1550    > THREE retraces
 *     0x055F4  call 0x1550   /
 *     0x055FA  jmp
 *
 * THREE, not one. A notch every third retrace is 23 a second where the port
 * ran one a retrace and faded at 70 - three times too fast, on every screen
 * that fades. Nothing here could have seen it: every comparison in this
 * repository scores INDICES, a fade is a palette, and the one tool that
 * measures duration (verify_pace.py) times the play loop and the menu loop
 * rather than the fade inside them.
 *
 * The LEVEL's fade is a different rate on purpose - image 0x0031E's branch
 * calls image 0x01550 ONCE per notch (image 0x00330), so it runs at 70. Two
 * fades, two rates, both the original's.
 * ====================================================================== */
#define FADE_RETRACES 3          /* image 0x055EE..0x055F4 */

/* Image 0x055EE, image 0x055F1 and image 0x055F4 - three waits to a notch. */
uint8_t palette_fade_retraces(void) { return FADE_RETRACES; }

/* Image 0x056E2 - one notch of a component, two at a time. (The EGA's own
 * step is image 0x0563E and is the other adapter; see the palette block.) */
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
/* The cross-hair's centre, and the point the pointer is actually at. */
#define CURSOR_HOT_X 7
#define CURSOR_HOT_Y 7
/* The eight the coordinates are offset by - image 0x0426D and image 0x04282
 * clamp to 8 at the low end. */
#define CURSOR_ORIGIN 8


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


/* Where the pointer is, in the ORIGINAL's coordinates - ds:0x10 and ds:0x12,
 * so eight more than the screen position and clamped the way image 0x0426D
 * clamps them. Image 0x00F41 starts it at (150, 100), which is where the
 * game puts it before anyone touches the mouse. */

/* ======================================================================
 * THE GAME'S DATA SEGMENT - one object at the original's own offsets.
 *
 * These were fifty-one separate globals, each carrying a `ds:` address in a
 * comment that nothing could hold it to. dgroup.h lays the segment out and
 * asserts every offset, so the address is the LAYOUT rather than a claim, and
 * the whole thing can be compared against the guest's own DS in one pass.
 *
 * The names below are #defines for now - see game.h - so every use site is
 * unchanged and the risk of the move is one line per variable rather than a
 * thousand. They become DG.field directly once this has been measured.
 *
 * The nine initialisers are the ones the definitions carried; the rest of the
 * segment starts zeroed, which is what a DOS BSS did.
 * ====================================================================== */
dgroup DG = {
    .code_cells = "..........",
    .fx_drawn = 0xFF,
    .mouse_x = 150,
    .mouse_y = 100,
    .rating_drawn = 0xFF,
    .selected = PANEL_SEL_START,
    .skill = 2,
    .snd_bits = 1,
    .snd_setting = 1
};

/* ds:0x1A - is the pointer over a lemming? Image 0x03DE8 sets it, image
 * 0x03DAF clears it, and image 0x05854 is what it changes. */

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
            if (x < 0 || x >= (int32_t)w) continue;
            uint8_t *p = &pix[(size_t)y * w + (size_t)x];
            /* TWO PASSES, and only the first is masked. Image 0x07218
             * writes plane 0 through the mask with planes 1..3 cleared
             * (GC 1 = 0x0E, GC 0 = 0); image 0x074AF then sets the write
             * function to OR (GC 3 = 0x10), the bit mask to 0xFF and the
             * map mask to plane 1 alone, and writes plane 1 over the WHOLE
             * window. So inside the mask the pixel is the ink, and outside
             * it plane 1 of the ink is ORed into whatever is there.
             *
             * That matters exactly where the store has ink the mask does
             * not cover - CURSOR_BOX's row 13 ends, at (146, 81) and
             * (159, 81) over the digger in "Just dig!". Over terrain, 14|2
             * is 14 and nothing shows; once the digger has cleared the
             * ground under a pointer parked there, 0|2 is a 2 on screen.
             * This port wrote it off for a long time as the page model's
             * stale tile, which it is not: the page holds 0, and the 2 is
             * drawn over it every frame. */
            if (mask[r][c]) *p = shape[r][c];       /* image 0x07239 */
            else *p = (uint8_t)(*p | (shape[r][c] & 2));   /* image 0x074B9 */
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
 *
 * AND IT IS IN THE IMAGE, so the measurement was never the source. These
 * twenty-four bytes are ds:0x25F0, image 0x0C080, byte for byte - the eight
 * the swap puts into DAC 16..23. Measuring them off a running DAC and writing
 * the answer down as a literal is how a transcription turns into a
 * reimplementation that happens to agree: the numbers were right and nothing
 * said where they came from. */
static const uint8_t PANEL_PAL6[8 * 3] = {   /* ds:0x25F0, image 0x0C080 */
    0x00, 0x2A, 0x00,   /* 8  green      */
    0x15, 0x3F, 0x15,   /* 9  bright green */
    0x15, 0x15, 0x15,   /* 10 dark grey  */
    0x2A, 0x00, 0x00,   /* 11 red        */
    0x2A, 0x15, 0x00,   /* 12 brown      */
    0x00, 0x2A, 0x2A,   /* 13 cyan       */
    0x3F, 0x15, 0x3F,   /* 14 magenta    */
    0x2A, 0x00, 0x2A,   /* 15 dark magenta */
};

/* Fill `out` with those eight, widened - the table at image 0x0C080 above,
 * which is SLOT 4 of the palette table (see the block at image 0x052F4).
 *
 * EIGHT because image 0x053CA writes eight: `cx = 0x808` is a count of 8
 * starting at register 8, and the slot pointer is stepped by 8 for the EGA's
 * one byte a colour or 0x18 for the DAC's three. That is the whole of the
 * swap - the upper half of the palette is replaced and the lower half is
 * left alone - and it is why this routine takes eight entries rather than
 * sixteen. The count was right before it was read; now it has the address
 * that fixes it.
 *
 * They are attributes 8..15: the
 * attribute controller maps those onto DAC 16..23 - read off the hardware as
 * `attr_pal = 00 01 02 03 04 05 06 07 10 11 12 13 14 15 16 17`, which is the
 * `v < 8 ? v : v + 8` that tools/compare_frame.py has always used. */
void level_panel_palette(lem_rgb *out)
{
    for (uint8_t i = 0; i < palette_split_count(); i++) {
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
/* ======================================================================
 * IMAGE 0x000FB - THE MAIN LOOP, AND THE ORDER OF A FRAME.
 *
 * The original has ONE loop and every screen jumps into it: image 0x0454C
 * ends `jmp 0x000FB` and so do the others. Its head flips the page - image
 * 0x00105 swaps ds:0x1F76 with ds:0x1F78, image 0x00142 hands the CRTC the
 * new start address through registers 0x0C and 0x0D - and image 0x00164
 * spins until ds:0x1FCD, the retrace ISR's counter, has reached five. THAT
 * IS THE SEVENTEEN FRAMES A SECOND: five retraces at 70 Hz.
 *
 * Then image 0x0018F runs the frame, and this is the whole of it:
 *
 *     0x0018F  ds:0x1FCD = 0     the ticks are spent
 *     0x00194  ds:0x1FD0 = 0     and the pending key
 *     0x00199  inc ds:0x1FD3     the nuke's own frame counter
 *     0x0019D  call 0x0031E      the fade, and the timed cues it arms
 *     0x001A0  call 0x04085      THE LEVEL'S KEYS and the edge scroll
 *     0x001A3  inc ds:0x52       the level frame counter
 *     0x001A7  call 0x080FD      the EGA set up for the frame's copies -
 *                                map mask 0x0F, GC mode 0x105, latched
 *     0x001AA  call 0x07900      window the master terrain onto the page
 *     0x001AD  call 0x03150      a click assigns a skill
 *     0x001B0  call 0x030A6      the entrance lets one out
 *     0x001B3  call 0x01F19      THE LEMMINGS STEP
 *     0x001B6  call 0x03944      the objects step, through image 0x07C0A
 *     0x001B9  call 0x07BB8      the objects are DRAWN
 *     0x001BC  call 0x03634      the lemmings are DRAWN, counting ds:0x45
 *     0x001BF  call 0x03CDE      the status line's text
 *     0x001C2  call 0x03AC8      the panel
 *     0x001C5  call 0x03150      a click assigns a skill AGAIN
 *     0x001C8  the end-of-frame tests - level_over and the rest
 *
 * FOUR OF THESE SKIP THEMSELVES WHEN PAUSED, each testing ds:0x2B at its own
 * head: 0x03150, 0x030A6, 0x01F19 and 0x03944. The pause is not a branch in
 * this loop, which is why the loop reads as an unconditional list.
 *
 * WHAT THIS CONFIRMS, and it is the reason to write it down: the port's split
 * below is the original's order. Step is 0x03150, 0x030A6, 0x01F19, 0x03944 -
 * click, release, lemmings, objects - and draw is 0x07BB8 then 0x03634,
 * objects then lemmings. Both halves already carried a MEASURED justification
 * for that order; this is the routine those measurements were measuring.
 *
 * TWO THINGS HERE WERE NOT IN THE PORT, and neither is in this function:
 *
 *   - image 0x04085's KEYS. Scan 0x2C and 0x2D - Z and X - step ds:0x82
 *     through the skills, image 0x040BC down with a wrap from 1 to 9 and
 *     image 0x040D4 up with a wrap from 10 to 2, both gated on ds:0x84, the
 *     same repeat divider the rate buttons use. Transcribed since, beside
 *     the keyboard's other skill keys - see image 0x040BC above.
 *   - image 0x03150 is called TWICE a frame, at 0x001AD and 0x001C5. The
 *     port does too now, from level_frame_compose - see level_play_click.
 *
 * Both are in now. They were named so the next reader would find them
 * already located rather than reading this loop again to find them.
 * ====================================================================== */
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
    level_anim_mark(st, lv, view_x);            /* image 0x0885B */
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
        lemming_mark(&lem[k], view_x);          /* image 0x03654 */
        /* DS:0x50 AGAIN. Image 0x03670 sets it and image 0x03678 clears it
         * around every lemming but a cloud under way - image 0x0365A's
         * `[di+0x14] == ds:0x3C`, the pointer only the cloud's entry stores
         * (image 0x02D45, 0x02E8D), with image 0x03662's frame 1 or more. So
         * it reads 0 after the draw whenever anything else is on screen,
         * a nuke or not, and image 0x001C5's click is not refused by it.
         * Found by comparing the data segment through two nukes. */
        if (!((lem[k].flags & CLOUD_BIT) && lem[k].anim >= 1))
            g_walking = 0;                      /* image 0x03678 */
        lemming_draw(pix, w, h, view_x, lem_sec, lem_len, &lem[k]);
    }
    level_status_out(out);                      /* image 0x03684 */
}

/* image 0x05976's section, handed over by level_show so level_panel_build
 * need not know about files. NULL on a High Performance machine. */
const uint8_t *g_panel_compat_sec;
uint32_t       g_panel_compat_len;

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
    /* IMAGE 0x03ACE - THE 'IN' READING IS THE PANEL'S OWN, and it is
     * computed HERE because image 0x01C2 calls this after image 0x01B3 has
     * stepped the lemmings. The port had it in level_frame_step, which is
     * image 0x0019D's share and runs BEFORE the step, so a lemming that
     * reached the exit during the step was not in the percentage until the
     * next frame. Ten frames of a 671-frame playthrough differed by one
     * character cell - the tens digit - and by nothing else. */
    level_status_in(level_saved_percent(lv));
    /* THE RATE DIVIDER TURNS HERE, because image 0x03B8C is part of this
     * redraw rather than of the button - see level_rate_tick. It is in this
     * routine rather than in the callers for the reason the next paragraph
     * gives about the pointer: a sequence written out twice will differ. */
    level_rate_tick();                          /* image 0x03B8C */
    /* AND THE SKILL STRIP'S, two instructions before it in the same routine:
     * image 0x03B3F turns ds:0x84 and copies ds:0x82 into ds:0x83 when it
     * fires, which is why the highlight follows the selection rather than
     * arriving with it. */
    level_skill_div_step();                     /* image 0x03B3F */

    /* What the pointer is over goes into the line BEFORE the line is drawn -
     * image 0x03CE9 runs in the play loop ahead of the panel. It lives here
     * rather than in the callers because the order is the game's, and a
     * sequence written out twice is a sequence that will differ: that is
     * exactly how the window path came to have no panel at all while every
     * measurement of the panel passed. */
    level_status_pointer(lem, nlem, view_x);

    /* IMAGE 0x05976 - a PC compatible unpacks the panel from ANOTHER main.dat
     * section. Image 0x059A1 calls the same image 0x057D1 this routine is
     * transcribed from, but with ds pointing at a scratch segment holding
     * that section rather than the usual one.
     *
     * main.dat has seven sections; the game uses 0, 2, 3 and 4, and section 6
     * is 8,224 bytes. Image 0x059B4 then copies 0x720 of its bytes from
     * 0x1900 - which is 8,224 exactly - over the prepared panel at 0xA0, and
     * a dump of ds:0x1FEC in both machines differs in exactly 0xA0..0x7BD. */
    const uint8_t *art = sec; uint32_t art_len = seclen;
    if (lem_machine == LEM_PC_COMPATIBLE && g_panel_compat_sec) {
        art = g_panel_compat_sec; art_len = g_panel_compat_len;
    }
    panel_frame(pp, PANEL_LINE_W, PANEL_ART_H, art, art_len, PANEL_ART_X, 0);
    /* THE STATUS FONT AND THE BUTTON NUMBERS STAY ON SECTION 2. Routing them
     * through section 6 as well was tried and measured: the green vanished
     * entirely - attribute 2 went from 812 to 0 - and the button rows went
     * from exact to 158 out. Only the ARTWORK moves. */
    panel_status(pp, PANEL_LINE_W, PANEL_ART_H, sec, seclen,
                 level_status_line(), 0, 0);
    panel_buttons(pp, PANEL_LINE_W, PANEL_ART_H, sec, seclen, lv);
    panel_highlight(pp, PANEL_LINE_W, PANEL_ART_H, sel);
    /* THE COPY BEFORE THE REBUILD. Image 0x001AA puts the store on the page
     * (image 0x07900) and only later does image 0x001C2's image 0x03AC8
     * rebuild a column of it (image 0x0796F), off-screen - so what a frame
     * shows is the store as the PREVIOUS frame left it. Stepping first put
     * every change on screen a frame early. */
    panel_minimap(pp, PANEL_LINE_W, PANEL_ART_H,
                  MINIMAP_AT_X, MINIMAP_AT_Y);       /* image 0x001AA */
    minimap_column_step(terrain, tw, th);       /* image 0x03AC8 -> 0x0796F */
    /* THE DOTS FIRST, THE FRAME OVER THEM. A lemming standing where the
     * viewport's edge runs put a dot through it in the port and does not in
     * the original: at frame 186 of "Watch out, there`s traps about" the
     * original holds 3 at (266, 183) - the edge's own colour - where the port
     * held 2, on every frame a lemming passed that cell. One pixel, and it
     * was the whole of that level's panel score. */
    panel_minimap_dots(pp, PANEL_LINE_W, PANEL_ART_H, lem, nlem,
                       MINIMAP_DOT_X, MINIMAP_AT_Y);
    /* ds:0x00, which is what image 0x07A5A shifts - view_x plus sixteen. */
    panel_viewport(pp, PANEL_LINE_W, PANEL_ART_H, (uint16_t)(view_x + 16),
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
/* ======================================================================
 * TRANSCRIBED FROM IMAGE 0x03981, the object composition pass.
 *
 *     0x03981  di = ds:0x0FEB, cx = 0x20    thirty-two entries
 *     0x03988  ds:0x1FE4 = ds:0x1FF0        THE OBJECTS' graphics segment,
 *                                           which is not the terrain's - the
 *                                           blit probe reads 0x02900 here
 *                                           against 0x07272 for a terrain
 *                                           piece, and that is what tells
 *                                           the two apart in one line
 *     0x0398E  ax = [di]<<8 | [di+1]        x, big-endian
 *     0x03993  and ax,ax / je               a zero x SKIPS this entry - it
 *                                           goes to image 0x039C5, the step,
 *                                           not out of the loop
 *     0x0399A  bx = [di+2]<<8 | [di+3]      y
 *     0x039A0  dx = [di+6]<<8 | [di+7]      the LVL's modifier bytes
 *     0x039A6  bp = dx & 0xFFFE             AND THEY ARE THE BLIT'S FLAGS,
 *                                           and the level files pin the
 *                                           value: e[7] is 0x0F in all 454
 *                                           objects of all eighty levels, so
 *                                           the low byte is always 0x0E and
 *                                           e[6] is 0x00, 0x40, 0x80 or
 *                                           0xC0. An object with e[6] = 0x80
 *                                           therefore blits with bp = 0x800E
 *                                           - which is exactly the value the
 *                                           blit probe caught, so the
 *                                           arithmetic and the observation
 *                                           agree without either being fitted
 *                                           to the other. The 0x0E is bits 3
 *                                           and 2: EVERY object blit asks
 *                                           image 0x08846 for both clips.
 *                                           Bit 15 is e[6] & 0x80, the
 *                                           no-overwrite level_parse reads;
 *                                           the probe caught bp = 0x800E, so
 *                                           bits 1..3 - clipping, image
 *                                           0x08846 - come from the level
 *                                           too. Bit 0 is masked off.
 *     0x039AB  dl = [si+4], dh = 0          the width
 *     0x039B0  cl = [si+5]                  the rows
 *     0x039B3  di = [si+8] + [si+0x17]      the graphic, base plus the
 *                                           frame's own offset
 *     0x039BB  ch = 4                       FOUR planes
 *     0x039BD  the blit
 *     0x039C5  di += 8, si += 0x1C          the next object and the next
 *                                           definition, in step
 *
 * The two pointers advance TOGETHER, so the original walks a prepared array
 * in which each entry already sits beside its definition. The port keeps the
 * LVL's list and the ground set apart and looks the definition up by `id`,
 * which reaches the same pair; where the pairing is made is not read.
 *
 * WHAT THIS SETTLES about the old comment here. It claimed the debt was
 * "pointed at the wrong address" because the original never composes objects
 * into the level bitmap - and that was a fact about ONE INSTANT, image
 * 0x0376A, where tools/prove.sh breaks. These five passes run after it:
 * probe_pvsrc.py dumps the bitmap at image 0x07EB2, the packer's entry, and
 * the port WITH objects differs by 19 pixels where WITHOUT it differs by 815,
 * the 815 beginning exactly where this level's objects are.
 * ====================================================================== */
int render_objects(canvas *c, const level *lv, const ground *g,
                   const uint8_t *sec, uint32_t sec_len,
                   uint8_t frame, const char **err)
{

    /* Image 0x03985: thirty-two, however many the level filled in. */
    for (uint16_t i = 0; i < lv->num_objects && i < LEVEL_MAX_OBJECTS; i++) {
        const level_object *o = &lv->objects[i];
        if (o->id >= GROUND_OBJECTS)
            continue;
        /* Image 0x03993: a zero x skips the entry rather than ending the
         * walk, so a hole in the list is passed over and the ones after it
         * are still drawn. */
        if (o->x == 0)
            continue;
        /* Image 0x039A6's `and bp, 0xfffe` - the level's own modifier bytes
         * ARE the blit's flag word, so the path is the blitter's to choose
         * rather than this routine's to decide. */
        /* Image 0x039A6's flag word is carried on the object (o->bp) and
         * NOT re-decided here: the pixel rule this port applies is
         * object_pixel_combine's, which has image 0x096BA and image 0x0956B
         * against it - the object blit's own paths, not the terrain's. Bit
         * 15 of o->bp is the same no_overwrite the booleans hold, so the two
         * agree by construction; the word is kept because it is what image
         * 0x039BD hands the blitter, and a later reading of image 0x08822's
         * modes will want it. */
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

        /* Image 0x039B3's graphic and the mask beside it at mask_offset, read
         * where they lie rather than decoded into a tile first - the original
         * materialises no tile either. */
        uint32_t stride = ((uint32_t)go->width + 7u) / 8u;
        uint32_t plane  = stride * (uint32_t)go->height;
        if (at + plane * 4u > sec_len
                || at + go->mask_offset + plane > sec_len) {
            if (err) *err = "object bitmap runs past the end of the section";
            return -1;
        }
        const uint8_t *src  = sec + at;
        const uint8_t *mask = sec + at + go->mask_offset;

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
                uint8_t m = blit_mask(mask, stride, sx, src_y);
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
                if (!(o->no_overwrite && !o->only_on_terrain) && !m)
                    continue;

                uint8_t *dst = &c->pix[(uint32_t)dy * c->w + (uint32_t)dx];
                /* THE SAME TEST level_anim_draw uses - see
                 * object_pixel_allowed, which exists because these two loops
                 * had drifted apart twice. */
                if (!object_pixel_allowed(*dst, o))
                    continue;
                /* AND THE SAME VALUE RULE - see object_pixel_combine. */
                *dst = object_pixel_combine(
                           *dst, blit_ink(src, plane, stride, sx, src_y), o);
            }
        }
    }
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
#define LABEL_NONE (-1)       /* image 0x04E84: the bare plate, no label */

/* WHICH LABEL, image 0x04E5A - and it is the SOUND SETTING that picks.
 *
 *     0x04E57  si = 0xEE30                 the default
 *     0x04E5A  al = ds:0x25A7
 *     0x04E5D  test al, 3 / je 0x04E84     both bits clear: a DIFFERENT PLATE
 *     0x04E61  test al, 2 / je 0x04E6F     bit 1 clear: keep 0xEE30
 *     0x04E65  cmp ds:0x2010, 0xFF / jne   and only with a driver
 *     0x04E6C  si = 0xEA50
 *     0x04E6F  ax=0x15F bx=0x92 dx=0x40    (351,146), 64 wide
 *
 * 0xEA50 IS LABEL_BASE, and 0xEE30 is one LABEL_SPRITE past it - so the two
 * offsets the original chooses between are sprites 0 and 1 of the pair this
 * port already had, at the position it already had. What it did not have was
 * the choice: LABEL_FX was 1, so the plate always read "effects only"
 * whatever F3 was set to, and F3 changed nothing anybody could see. */
int menu_label_sprite(void)
{
    if (!sound_enabled(3)) return LABEL_NONE;          /* image 0x04E5D */
    if (sound_enabled(2) && sound_driver_answered())   /* image 0x04E61 */
        return 0;                                      /* image 0x04E6C */
    return 1;                                          /* image 0x04E57 */
}

/* AND THE THIRD STATE IS NO LABEL AT ALL.
 *
 * With both bits of ds:0x25A7 clear, image 0x04E5F goes to image 0x04E84,
 * which blits si=0xB120 masked at si+0xAB9 to (324,120). Every one of those
 * numbers is something this port already has:
 *
 *     0xB120  = 45344       = SIGN_SPRITE[2], the FX plate itself
 *     +0xAB9  = 2745        = SIGN_PLANE * 3, its fourth plane, the mask
 *     (324,120)             = plate 2's own position, 68 + 2 * 128
 *     cx = 0x43D            = 0x400 | 61, and SIGN_H is 61
 *     dx = 0x78             = 120, and SIGN_W is 120
 *
 * So it REDRAWS THE BARE PLATE - the label is not swapped, it is erased. The
 * original has to redraw because it drew a label over the plate earlier; this
 * port composes the screen in one pass, so not drawing the label is the same
 * picture and the plate underneath is already right.
 *
 * Getting this wrong is what made F3 feel like it needed pressing repeatedly:
 * with only two labels, silent and effects-only drew the SAME one, so two of
 * the three presses changed nothing visible. */
#define LABEL_X      351
#define LABEL_Y      146

/* Where the six plates land. NOT a table in the original - image 0x04F31 runs
 * two loops, four from (68,120) and two from (196,197), each `add ax, 0x80`.
 * It is written as the loops rather than as a table of six positions so there
 * is one source of truth. The hotspot matcher that used to need the boxes is
 * gone - image 0x04473 makes a click F1 wherever it lands, see menu_dispatch -
 * and what is left reading this is the drawing.
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

/* IMAGE 0x04967 AND IMAGE 0x04A01'S SET-UP - the four calls both open with.
 *
 *   image 0x04DEF  CLEAR the page. `di = 0`, `cx = 0x6D60` - 28,000 bytes,
 *                  which is 640 x 350 / 8, the whole thing.
 *   image 0x04BF5  DECLARE THE SURFACE. It sets the two page bases -
 *                  ds:0x1F76 = 0 and ds:0x1F78 = 0x6D60 - and the fine
 *                  scroll ds:0x1F7A = 0, then calls image 0x8808 with
 *                  ax = 0x280 (640), bx = 0x15E (350) and dx = the page.
 *
 *                  Image 0x8808 stores ax in ds:0x66F2, bx in ds:0x66F0 and
 *                  ax >> 3 in ds:0x66F4. Those are the BLITTER'S OWN bounds -
 *                  the width it refuses a blit past at image 0x09997, the
 *                  height, and the row in bytes. So the clip limit is
 *                  whatever surface was last declared: 640 here and 352 in a
 *                  level, which is what tools/probe_clip.py measures.
 *   image 0x0531E  THE PALETTE. `al = 0, cx = 0x10, bx = 0x100`, then image
 *                  0x055B7, which branches on ds:0x1F7C - the adapter letter
 *                  - and indexes the table at ds:0x52D4. Sixteen entries.
 *   image 0x05017  THE LEVEL PREVIEW - `xor bx,bx / jmp 0x7E76`. Only the
 *                  briefing reaches it, and draw_preview carries that whole
 *                  chain; it is called from menu_briefing_screen rather than
 *                  here, because here it would run for the code and input
 *                  screens too.
 *
 * TWO OF THE FOUR HAVE NO WORK TO DO IN THIS PORT, and saying which is the
 * whole point of writing the sequence out. This was owed for a long time
 * because the port's decomposition is not the original's and the difference
 * looked like a debt; it is not one, it is two no-ops and a routine that
 * moved.
 */
static void menu_screen_open(menu_screen *m, const uint8_t *sec,
                             int tile_background)
{
    /* IMAGE 0x04DEF - nothing to do. The original clears VIDEO MEMORY it
     * already owns; this port's page is a buffer of its own and the caller
     * has just calloc'd it, which is the same act arrived at differently. */

    /* IMAGE 0x04BF5 - A DELIBERATE NO-OP, and it is the interesting one.
     * ds:0x66F0/2/4 are the blitter's clip bounds, and this port has no
     * blitter to bound: every blit is an array write that carries its own
     * extent, so there is nothing for the numbers to constrain. */

    /* IMAGE 0x0531E - the palette, sixteen entries. */
    for (uint8_t i = 0; i < 16; i++) {
        m->palette[i].r = widen6(MENU_PAL6[i * 3 + 0]);
        m->palette[i].g = widen6(MENU_PAL6[i * 3 + 1]);
        m->palette[i].b = widen6(MENU_PAL6[i * 3 + 2]);
    }

    /* THE PORT'S OWN, and NOT one of the four: the original's background is
     * already in video memory when these screens start - image 0x0445B's loop
     * has been drawing it - so it has nothing to tile. This port composes
     * every screen from nothing each time and has to put it back.
     *
     * `tile_background` is a vertical ORIGIN plus one, so 1 means "tile from
     * row 0" and 0 means "do not tile". The briefing tiles the very same
     * bitmap from row 78, which is the whole of what made it look like a
     * different background for so long. */
    if (tile_background) {
        uint16_t org = (uint16_t)(tile_background - 1);
        for (uint16_t y = org; y < MENU_H; y++)
            for (uint16_t x = 0; x < BG_DRAWN_W; x++)
                m->pix[(size_t)y * MENU_W + x] =
                    planar(sec, BG_PLANE0, BG_PLANE1 - BG_PLANE0, BG_STRIDE,
                           2, x % BG_W, (uint16_t)((y - org) % BG_H));
    }
}

/* THE PORT'S OWN: main.dat opened and its sections found, then image
 * 0x04967's set-up above. The original does no such thing here - the file was
 * read once at start-up through image 0x01D2E and lives in memory - so this
 * is DOS's work, done late because this port loads what a screen needs when
 * the screen needs it. */
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
    menu_screen_open(m, *sec, tile_background);
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
    /* KEEP THE FONT. Image 0x04C7E redraws the ten cells on every pass of
     * its own loop - `call bp` at image 0x04CC9 - so this screen cannot free
     * what it drew with, the way the menu keeps section 4 for its banner. */
    m->gfx = malloc(fontlen);
    if (m->gfx) {
        memcpy(m->gfx, font, fontlen);
        m->gfx_len = fontlen;
    }
    dat_free(&d);
    return 0;
}

/* ======================================================================
 * IMAGE 0x04C7E - TYPING THE CODE, and the port had one line of it.
 *
 * Ten cells at ds:0x4DF2, and the loop is:
 *
 *     0x04CA5  call 0x1550   wait a retrace
 *     0x04CAD  dec ds:0x491E, and every 0x14 of them toggle the caret
 *     0x04CC9  call bp       redraw - the code screen passes image 0x0503B
 *     0x04CCB  call 0x4C20   the page flip
 *     0x04CCE  call 0x4D1A   a key, translated through ds:0x202E
 *     0x04CE0  0x7F -> image 0x04D02, backspace
 *     0x04CE4  0x0D -> image 0x04D10, enter: put the real character back
 *                     under the caret and return
 *     0x04CE8  below 'A' or above 'Z' -> ignored. image 0x04C85 loads
 *              bx = 0x5A41, which is those two letters as a word.
 *
 * THE CARET IS NOT A SEPARATE OVERLAY. `cl` holds the character that
 * belongs in the cell and `ch` is '_', and image 0x04CB8 swaps the cell
 * between them - so the blink WRITES into the field, and enter has to put
 * `cl` back or the code would carry an underscore. That is why image 0x04D16
 * exists and why this cannot be done by drawing a cursor over the top.
 *
 * The port had menu_caret_step, which is image 0x04CAD's four lines, and
 * nothing else: no cells, no keys, no return. Typing did nothing, and a
 * player found it.
 * ====================================================================== */
/* The four ratings, six characters each, exactly as they sit in the
 * executable: "Fun   TrickyTaxingMayhem". Image 0x049AD copies six bytes of
 * it into the code screen's line, and the briefing reads the same table. */
static const char RATINGS[] = "Fun   TrickyTaxingMayhem";  /* image 0x0E691 */


const char *level_code_cells(void) { return g_code_cells; }

/* THE PORT'S OWN, and only for the harness: the original types into ds:0x4DF2
 * a key at a time and has no way to set it. tools/probe_codescreen.py drives
 * the ORIGINAL's own keyboard, so the port has to be put in the same state to
 * be compared with it. */
void level_code_type(const char *cells)
{
    for (uint8_t i = 0; i < CODE_LEN; i++)
        g_code_cells[i] = cells[i] ? cells[i] : '.';
    g_code_cells[CODE_LEN] = '\0';
}

/* One pass of the redraw image 0x04CC9 reaches through bp. */
void code_cells_draw(menu_screen *m)
{
    if (!m->gfx) return;
    for (uint16_t y = CODE_DOTS_Y; y < CODE_DOTS_Y + FONT_H; y++)
        for (uint16_t x = CODE_DOTS_X;
             x < CODE_DOTS_X + (uint16_t)(CODE_LEN * FONT_W); x++)
            if (y < MENU_H && x < BG_DRAWN_W)
                m->pix[(size_t)y * MENU_W + x] = 0;
    draw_at(m->pix, m->gfx, m->gfx_len, g_code_cells,
            CODE_DOTS_X, CODE_DOTS_Y);
}

#define CODE_BLINK_TICKS 0x14        /* image 0x04C95 */

static void menu_code_entry(menu_screen *m)
{
    uint8_t di = 0;                             /* image 0x04C8C: the cell */
    char cl = '.', ch = CODE_CARET;             /* image 0x04C92: 0x5F2E */
    uint8_t blink = CODE_BLINK_TICKS;

    for (;;) {
        video_retrace_wait();                   /* image 0x04CA5 */
        if (--blink == 0) {                     /* image 0x04CAD */
            blink = CODE_BLINK_TICKS;           /* image 0x04CB3 */
            g_code_cells[di] = (g_code_cells[di] == cl) ? ch : cl;
        }
        code_cells_draw(m);                     /* image 0x04CC9 */
        video_page_flip();                      /* image 0x04CCB */

        uint8_t al = key_scan();                      /* image 0x04CCE */
        if (al == 0xFF) continue;               /* image 0x04CD6 */
        if (al == 0x7F) {                       /* image 0x04CE0 */
            g_code_cells[di] = '.';             /* image 0x04D02 */
            cl = '.';
            if (di == 0) continue;              /* image 0x04D07 */
            di--;                               /* image 0x04D0B */
            cl = g_code_cells[di];
            continue;
        }
        if (al == 0x0D) {                       /* image 0x04CE4 */
            g_code_cells[di] = cl;              /* image 0x04D16 */
            return;
        }
        if (al < 'A' || al > 'Z') continue;     /* image 0x04CE8, 0x04CEC */
        if (di == CODE_LEN) continue;           /* image 0x04CF0 */
        g_code_cells[di] = (char)al;            /* image 0x04CF4 */
        di++;
        cl = g_code_cells[di];                  /* image 0x04CF7 */
        if (di == CODE_LEN) {                   /* image 0x04CF9 */
            di--;
            cl = g_code_cells[di];
        }
    }
}

/* ======================================================================
 * IMAGE 0x04967's LOOP - the code screen, once through.
 *
 *     0x0497D  the ten cells to '.'
 *     0x04983  the "Enter Code" prompt
 *     0x04996  call 0x4C7E   type one
 *     0x04999  call 0x51A2   take it back
 *     0x0499C  jne 0x49EC    refused: "Incorrect Code", 0x3C retraces, and
 *                            image 0x049FE goes back to the MENU
 *     0x0499E  accepted: the level number and the rating name, then a wait
 *                        of 0x78 retraces that any button or key cuts short
 *                        (image 0x049D2), and image 0x049E9 goes back to the
 *                        MENU as well
 *
 * Both arms end at image 0x04401. A code does not start a level - it sets
 * ds:0x1F6B and returns, and F1 plays whatever that now names.
 * ====================================================================== */
#define CODE_HOLD_OK    0x78         /* image 0x049CC */
#define CODE_HOLD_BAD   0x3C         /* image 0x049F6 */

/* WHERE THE TWO MESSAGES GO, and they go UNDERNEATH the field.
 *
 * These are not chosen: they are the cell positions in the text records the
 * original hands to image 0x04D4D, whose format is [x+1][y][flag] then the
 * characters to a 0x0D, repeated, ended by a zero length:
 *
 *     ds:0x4DE1   (14, 7) "Enter Code"      (14, 9) ".........."
 *     ds:0x4DFE   (10,11) "Code for Level     "
 *                 (12,13) "Rating Mayhem"
 *     ds:0x4E27   (12,11) "Incorrect Code"
 *
 * The port drew both messages at the FIELD's own position, which is the row
 * the typed code is on - so the answer landed on top of what had just been
 * typed. That was my arrangement and not a reading; a player saw it at once.
 *
 * ds:0x4E10 and ds:0x4E1F are not separate records. They point INTO the one
 * above - at the five spaces after "Code for Level " and at "Mayhem" inside
 * "Rating Mayhem" - so image 0x049A9 and image 0x049C0 patch those strings in
 * place rather than drawing anything of their own. */
#define CODE_CELL       16           /* FONT_W and FONT_H both */
#define CODE_OK_X       (10 * CODE_CELL)
#define CODE_OK_Y       (11 * CODE_CELL)
#define CODE_RATING_X   (12 * CODE_CELL)
#define CODE_RATING_Y   (13 * CODE_CELL)
#define CODE_BAD_X      (12 * CODE_CELL)
#define CODE_BAD_Y      (11 * CODE_CELL)
/* "Code for Level " is fifteen characters; the number is right-aligned into
 * the five that follow - image 0x049A9 writes it through ds:0x4E10. */
#define CODE_NUM_AT     15
#define CODE_NUM_W      5

/* The two messages, drawn where image 0x04D4D's records put them. It is a
 * routine because the LOOP draws them and so does the harness that scores
 * this screen against the original - and a picture composed twice is a
 * picture that will differ, which is the rule level_play_step exists for. */
/* Black the strip a line of text is about to go on.
 *
 * THE RECORDS SAY WHICH LINES GET THIS. Image 0x04D5A reads the third byte of
 * each record and turns it into bp - 2 when the byte is zero, 0 when it is
 * not - and the code screen's records split exactly that way:
 *
 *     (14, 7) flag 0   "Enter Code"        drawn over the background
 *     (14, 9) flag 8   ".........."        the input field
 *     (10,11) flag 8   "Code for Level"
 *     (12,13) flag 8   "Rating Mayhem"
 *     (12,11) flag 8   "Incorrect Code"
 *
 * The port already blacked the field's strip - without it the tile showed
 * between the dots and cost 1,230 pixels - and drew the two messages keyed
 * like the title. Measured against the original's own refused screen that is
 * 665 pixels: every glyph slightly wrong and the spaces exact, which is the
 * signature of the background showing through a glyph's own black rather
 * than of a wrong string or a wrong place. */
/* IMAGE 0x04B68 - three digits, LEFT aligned, leading blanks suppressed.
 *
 * It fills three cells with spaces and then walks hundreds, tens, units,
 * advancing `si` ONLY when it emits a digit - image 0x04B85 puts a space in
 * ah and jumps PAST the `inc si` when the digit is zero. So 13 lands in the
 * first two cells of the field and not the last two.
 *
 * This had been written as right-aligned, on the strength of the field being
 * five spaces, and scored 718 pixels against the original's own screen: the
 * digits three cells too far right and nothing else wrong. Right-aligned into
 * a blank field is the obvious reading and it is not this one. */
static void code_number_write(char *at, uint8_t n)
{
    uint8_t out = 0;
    at[0] = at[1] = at[2] = ' ';                /* image 0x04B68 */
    for (uint8_t div = 100; div; div /= 10) {
        uint8_t d = (uint8_t)(n / div);
        n = (uint8_t)(n % div);
        if (d == 0 && out == 0 && div != 1)
            continue;                           /* image 0x04B85: a space */
        at[out++] = (char)('0' + d);            /* image 0x04B89 */
    }
}

/* A SPACE IS NOT BLACKED, and that is measured. Clearing the whole strip left
 * 46 pixels differing in exactly one cell - the space between "Incorrect" and
 * "Code" - where the original has the background's own ink and this had put
 * black. So the line is not a box: each character's cell is blacked and the
 * glyph drawn into it, and a space is passed over entirely.
 *
 * Which also says what image 0x04D78 does per character rather than per line,
 * and matches the port having scored that cell exactly before any of this. */
static void code_line_clear(menu_screen *m, uint16_t x, uint16_t y,
                            const char *text)
{
    for (uint8_t i = 0; text[i]; i++) {
        if (text[i] == ' ') continue;
        uint16_t cx = (uint16_t)(x + i * FONT_W);
        for (uint16_t yy = y; yy < y + FONT_H && yy < MENU_H; yy++)
            for (uint16_t xx = cx; xx < cx + FONT_W && xx < BG_DRAWN_W; xx++)
                m->pix[(size_t)yy * MENU_W + xx] = 0;
    }
}

/* Image 0x0499E when the code was taken and image 0x049EC when it was not -
 * the two messages, at the cells their records name. See the block above. */
void menu_code_result_draw(menu_screen *m, int accepted)
{
    char line[40];

    if (!m->gfx) return;
    if (!accepted) {
        code_line_clear(m, CODE_BAD_X, CODE_BAD_Y, "Incorrect Code");
        draw_at(m->pix, m->gfx, m->gfx_len, "Incorrect Code",
                CODE_BAD_X, CODE_BAD_Y);        /* image 0x049EC: ds:0x4E27 */
        return;
    }
    /* image 0x0499E: the level it named, and the rating - both BELOW the
     * field, at the record's own cells. */
    memcpy(line, "Code for Level     ", 20);
    code_number_write(line + CODE_NUM_AT, level_number());  /* image 0x049A9 */
    code_line_clear(m, CODE_OK_X, CODE_OK_Y, line);
    draw_at(m->pix, m->gfx, m->gfx_len, line, CODE_OK_X, CODE_OK_Y);
    /* Image 0x049AD: six bytes of ds:0x4C01 indexed by the rating, copied
     * over "Mayhem" in "Rating Mayhem" - so the word is six wide and padded,
     * not trimmed. */
    memcpy(line, "Rating Mayhem", 14);
    memcpy(line + 7, RATINGS + (level_rating() & 3) * 6, 6);
    code_line_clear(m, CODE_RATING_X, CODE_RATING_Y, line);
    draw_at(m->pix, m->gfx, m->gfx_len, line, CODE_RATING_X, CODE_RATING_Y);
}

/* Image 0x04967's own sequence - see the block above it. */
menu_action menu_code_run(menu_screen *m)
{

    /* THE PAGE FLIP HAS TO BE POINTED AT THIS SCREEN. Image 0x04C20 puts the
     * CRTC at whatever was just composed; here the composed screen is a
     * pointer and the menu's was still in it, so the flip went on showing the
     * menu while this one was typed into - "nothing changes when I type". */
    video_screen_set(m->pix, MENU_W, MENU_H, m->palette, NULL, 0);

    for (uint8_t i = 0; i < CODE_LEN; i++)
        g_code_cells[i] = '.';                  /* image 0x0497D */
    g_code_cells[CODE_LEN] = '\0';

    menu_code_entry(m);                         /* image 0x04996 */

    if (!level_code_apply(g_code_cells)) {      /* image 0x04999 */
        menu_code_result_draw(m, 0);            /* image 0x049EC */
        video_page_flip();                      /* image 0x049F3 */
        for (uint16_t n = 0; n < CODE_HOLD_BAD; n++)
            video_retrace_wait();               /* image 0x049F9 */
        return MENU_EXIT;                       /* image 0x049FE: the menu */
    }

    menu_code_result_draw(m, 1);                /* image 0x0499E */
    video_page_flip();                          /* image 0x049C9 */
    /* image 0x049CC: 0x78 retraces, cut short by a button or a key. */
    for (uint16_t n = 0; n < CODE_HOLD_OK; n++) {
        video_retrace_wait();
        if (mouse_buttons() & 7) break;         /* image 0x049D2 */
        if (key_scan() != 0xFF) break;          /* image 0x049DA */
    }
    return MENU_EXIT;                           /* image 0x049E9: the menu */
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
/* image 0x019C9, and it reads the same ds:0x1F6B image 0x0197D just did -
 * the test is on the SAME n, three instructions later in the same routine. */
int level_odd_entry(void)
{
    uint8_t n = LEVEL_ORDER[level_index() % LEVELS_TOTAL];
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
/* WHICH FILE AND WHICH SECTION - image 0x0197D, and it reads ONE byte.
 *
 *     0x0197D  bl = ds:0x1F6B          the level index
 *     0x01983  si = 0x54EE             the running order, image 0x0EF7E
 *     0x01987  cl = [bx+si]            n = LEVEL_ORDER[index]
 *     0x01989  dec cl
 *     0x0198E  cx >>= 1                and image 0x01992's three more shifts
 *     0x01992  ax = (n - 1) >> 4       the FILE
 *     0x01998  cx = (n - 1) >> 1 & 7   the SECTION
 *     0x019A0  call 0x04BC2            the digits into the template
 *
 * This took a rating and a number and multiplied them back together. There is
 * no rating or number to take: ds:0x1F6B is the index, and image 0x04566
 * DERIVES a number from it for the briefing's text and nothing else. */
int level_locate(char *file, size_t file_len, uint32_t *section)
{
    uint8_t n = LEVEL_ORDER[level_index() % LEVELS_TOTAL];  /* image 0x01987 */
    if (n == 0) return -1;
    /* The original writes the digits into a template rather than formatting a
     * name; digits3 is that routine, and using it here keeps the transcription
     * on the path rather than beside it. */
    snprintf(file, file_len, "level000.dat");
    digits3((uint8_t)((n - 1) >> 4), file + 5);
    *section = (uint32_t)(((n - 1) >> 1) & 7);
    return 0;
}

/* WHICH GRAPHICS FILES A LEVEL USES - image 0x0197D's tail, 0x01A00..0x01A7A,
 * and the branch in it that nobody had read.
 *
 *     0x01A00  al = ds:0x0FE6      the graphics set
 *     0x01A03  si = ds:0x2545      where the digit goes in "ground0o.dat"
 *     0x01A07  add al, 0x30        ... as ASCII
 *     0x01A0B  dx = ds:0x253F      and image 0x01A18 loads 0x1000 bytes raw
 *     0x01A1B  the same digit into "vgagr0.dat" at ds:0x2539
 *     0x01A26  al = ds:0x0FE8      THE EXTENDED SET, the header's +0x1C
 *     0x01A2B  je 0x01A30          zero: the ordinary vgagr
 *     0x01A2D  jmp 0x01A5A         non-zero: vgaspec
 *
 * THE TWO PATHS DIFFER IN MORE THAN THE NAME, which is why this returns which
 * one it took. Image 0x01A30 opens the file and reads SECTION 0 into the
 * terrain segment ds:0x1FF4 at +0x10 (image 0x01B79), then SECTION 1 into the
 * sprite segment ds:0x1FF0 (image 0x01BA9). Image 0x01A5A reads section 0
 * alone and closes: a VGASPEC level has no piece set, because its terrain is
 * one whole picture instead of pieces.
 *
 * And the digit arithmetic differs. `add al, 0x30` for the graphics set;
 * `add al, 0x2F` at image 0x01A5E for the extended one, which is 1-based - so
 * extended set 1 is vgaspec0.dat. Four sections in the eighty have one, which
 * is the same four music_choose's by-number table has entries for.
 *
 * The templates are the original's own strings and the digit's place in each
 * is the difference between the two addresses the routine loads, not a count
 * of characters here.
 *
 * Returns 1 for a VGASPEC level, 0 for an ordinary one, -1 if a buffer is too
 * small to hold the name.
 */
static const char T_GROUND[]  = "ground0o.dat";   /* ds:0x253F, image 0x0BFCF */
static const char T_VGAGR[]   = "vgagr0.dat";     /* ds:0x2534, image 0x0BFC4 */
static const char T_VGASPEC[] = "vgaspec0.dat";   /* ds:0x2559, image 0x0BFE9 */
#define GROUND_DIGIT  6   /* ds:0x2545 - ds:0x253F */
#define VGAGR_DIGIT   5   /* ds:0x2539 - ds:0x2534 */
#define VGASPEC_DIGIT 7   /* ds:0x2560 - ds:0x2559 */

int level_graphics_names(const level *lv, char *gname, size_t gn,
                         char *gfx, size_t xn)
{
    if (gn < sizeof(T_GROUND) || xn < sizeof(T_VGASPEC)) return -1;

    uint8_t gs = (uint8_t)(lv->graphics_set & 0xFF);
    memcpy(gname, T_GROUND, sizeof(T_GROUND));
    gname[GROUND_DIGIT] = (char)('0' + gs);         /* image 0x01A07 */

    uint8_t ext = (uint8_t)(lv->extended_graphics_set & 0xFF);
    if (ext == 0) {                                 /* image 0x01A2B */
        memcpy(gfx, T_VGAGR, sizeof(T_VGAGR));
        gfx[VGAGR_DIGIT] = (char)('0' + gs);        /* image 0x01A24 */
        return 0;
    }
    memcpy(gfx, T_VGASPEC, sizeof(T_VGASPEC));
    /* image 0x01A5E adds 0x2F, not 0x30 - the extended set is 1-based. */
    gfx[VGASPEC_DIGIT] = (char)('0' + (ext - 1));
    return 1;
}

/* Image 0x01A1B writes the graphics set's digit into ds:0x2534 before the
 * branch at image 0x01A2B, so the name is the same on both paths - and image
 * 0x01B19 loads it again at the end of a VGASPEC build, for the objects. */
int level_objects_name(const level *lv, char *name, size_t n)
{
    if (n < sizeof(T_VGAGR)) return -1;
    memcpy(name, T_VGAGR, sizeof(T_VGAGR));
    name[VGAGR_DIGIT] = (char)('0' + (uint8_t)(lv->graphics_set & 0xFF));
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
/* ======================================================================
 * THE PREVIEW'S GEOMETRY, and every number here now has an address.
 *
 * All four were MEASURED first - found by matching the port's output against
 * a capture - and image 0x07EB2's packer turns out to state each of them.
 * That is the strongest kind of agreement this project gets: two independent
 * derivations of the same constant, one from pixels and one from the code.
 *
 *   image 0x07EBB  di = ds:0x1F76 + ds:0x1F7A + 0x64F
 *                  The destination. Mode 0x10's logical line is 80 bytes, so
 *                  0x64F = 1615 = 20 * 80 + 15: row 20, byte 15 - which is
 *                  PV_Y = 20 and PV_X = 15 * 8 = 120.
 *   image 0x07EFE  cx = 0x27 = 39 rows          PV_H, "not 40: the top row
 *                                               is not drawn", measured
 *   image 0x07F04  cx = 0x34 = 52 bytes a row   416 pixels, of which the
 *                                               level's 396 are PV_W
 *   image 0x07F28  the bit selection            PV_SAMPLE
 *
 * THE PACKER IS 4:1 AND TAKES THE THIRD PIXEL OF EACH FOUR. Image 0x07F1D
 * reads four source bytes and builds one output byte from eight bits, and
 * the rotate-and-mask pairs say which: `rol al,1` twice with `and 0x80`
 * lands bit 5 at bit 7, `ror al,1` three times with `and 0x40` lands bit 1
 * at bit 6, and so on down. In a byte the leftmost pixel is bit 7, so bits
 * 5 and 1 are pixels 2 and 6 - pixel 2 of each group of four. PV_SAMPLE is
 * 2 and was measured; this is why.
 *
 * The port samples the canvas rather than packing bit-planes, because it has
 * no planes to pack. The GEOMETRY is the original's.
 * ====================================================================== */
#define PV_X      120      /* image 0x07EBB: 0x64F % 80 * 8 */
#define PV_Y       20      /* image 0x07EBB: 0x64F / 80 */
#define PV_W      396      /* LEVEL_W / 4 */
#define PV_H       39      /* image 0x07EFE: cx = 0x27, and not 40 - the top
                            * row is not drawn; see below */
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
#define PV_SAMPLE   2      /* image 0x07F28: bits 5 and 1 of each byte, which
                            * is pixel 2 of each four */
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

/* TRANSCRIBED FROM IMAGE 0x07E76 AND ITS PACKER AT IMAGE 0x07EB2.
 *
 * The original builds the preview in two steps and the port does the same
 * two, by different means:
 *
 *   image 0x07E76  ds:0x1FE4 = ds:0x1FEE, then FOUR passes of two blits -
 *                  ax = 0 then ax = 0x140, both dx = 0x140 wide and
 *                  cx = 0x268, bp = 0x0C - with bx stepping 0x68 = 104 rows
 *                  a pass. Eight blits of 320 columns, which is the level
 *                  laid into the working page. bp = 0x0C asks image 0x08846
 *                  for BOTH clips, horizontal and vertical - see blit_clips.
 *   image 0x07EB2  the packer: 39 rows of 52 bytes, four source bytes to
 *                  one output byte, taking pixel 2 of each four.
 *
 * The port has no page to blit into and no planes to pack, so it composes
 * the level on its canvas - render_terrain and render_objects, both of which
 * this calls - and samples that. What transcribes is the GEOMETRY, and all
 * four of its numbers are above with their addresses.
 *
 * WHAT IS NOT TRANSCRIBED, and it is one thing rather than a routine: image
 * 0x07E76's eight blits go through 1010:8822 into video memory, and reading
 * them properly means reading image 0x08822's modes. The port's sampling
 * reaches the same picture - the briefing scores 0 of 224,000 against the
 * original's own screen - so what is owed here is the ROUTE, not the result.
 */
static void draw_preview(const char *dir, const level *lv, ground *g,
                         uint8_t *dst)
{
    char path[1024];
    dat_file gfx, spec = { NULL, 0 };
    const char *err = NULL;
    char gname[16], xname[16];
    /* Image 0x0394F takes the same branch as the level, image 0x03959 to
     * image 0x01A7B for a VGASPEC one - which also writes its colours into
     * the preview palette, so the caller reads g->preview after this. */
    int special = level_graphics_names(lv, gname, sizeof gname,
                                       xname, sizeof xname);
    if (special < 0) return;
    if (special) {
        snprintf(path, sizeof(path), "%s/%s", dir, xname);
        if (dat_load(path, &spec, &err) != 0) return;
        if (spec.count < 1) { dat_free(&spec); return; }
    }
    level_objects_name(lv, xname, sizeof xname);
    snprintf(path, sizeof(path), "%s/%s", dir, xname);
    if (dat_load(path, &gfx, &err) != 0) {
        dat_free(&spec);
        return;                       /* no preview is better than a wrong one */
    }
    if (gfx.count < 1) { dat_free(&gfx); dat_free(&spec); return; }

    canvas c;
    if (canvas_init(&c) != 0) { dat_free(&gfx); dat_free(&spec); return; }
    int drawn = special
        ? render_vgaspec(&c, g, spec.sections[0].data, spec.sections[0].size,
                         &err)
        : render_terrain(&c, lv, g, gfx.sections[0].data,
                         gfx.sections[0].size, &err);
    dat_free(&spec);
    if (drawn == 0) {
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


/*
 * TRANSCRIBED from image 0x0454C's chain, and every step below carries its
 * address. What is left owed is named at the end - it is one thing, and the
 * same one draw_preview names.
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
 * So the port's measured order is now the original's read order, they agree,
 * and the steps below say which is which.
 *
 * WHAT REMAINS OWED, and it is not this routine's shape:
 *
 *   - the ROUTE of image 0x0394F and image 0x07EB2. Both go through
 *     1010:8822 into video memory, and reading them properly means reading
 *     image 0x08822's five modes. The port composes on a canvas and samples;
 *     see draw_preview, which owes the same thing and says so.
 *   - WHERE IN MAIN.DAT SECTION 3 the background comes from. The blits are
 *     logged - image 0x089C4's three pairs at y = 78, 182 and 286, srcseg
 *     0x587E offset 0 - and the tile at that offset scores 1.65%, so it is
 *     that file and not that offset. Recorded in STATUS.md.
 *
 * The screen itself is exact: 0 of 224,000 against the original's own.
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
    /* Image 0x04DEF: clear the page - di = 0, cx = 0x6D60, which is 28,000
     * bytes, the whole 640 x 350 at eight pixels to the byte. The tiling
     * from BR_BG_Y is this port's, and the blits it stands on are image
     * 0x089C4's three pairs at y = 78, 182 and 286. */
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
        char gname[16], xname[16];
        /* image 0x01A07 names it; the port used to build the same string
         * inline with a mask the original does not apply. */
        if (level_graphics_names(lv, gname, sizeof gname,
                                 xname, sizeof xname) < 0) gname[0] = '\0';
        snprintf(gpath, sizeof(gpath), "%s/%s", dir, gname);
        if (ground_load(gpath, &g, &gerr) == 0) {
            /* Image 0x0394F then image 0x07EB2: the second composition
             * path lays the level down WITH its objects - which is image
             * 0x03981, render_objects - and the packer takes it 4:1 into
             * the visible page. draw_preview carries both. It runs FIRST
             * because a VGASPEC level's image 0x01A7B rewrites the preview
             * eight, and the original's briefing shows the rewritten ones:
             * MEASURED on Fun 22, DAC 56..63 hold vgaspec3.dat's eight
             * unaltered and the whole screen is 0 of 224,000 differing.
             * How they reach the DAC is not read - image 0x045EF's fade
             * starts before image 0x0394F composes, so it is not simply
             * that the fade comes later. */
            draw_preview(dir, lv, &g, m->pix);   /* image 0x07EB2 */
            for (uint8_t i = 8; i < 16; i++)
                m->palette[i] = g.preview[i - 8];
        }
    }

    /* And its lower seven are its own, not the menu's - see BRIEF_PAL6. */
    {
        /* Image 0x05329: the palette, sixteen entries from slot 0x107
         * through image 0x05569 - which is the two-slot selector the palette
         * block above names, with 0x100 read first. */
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
    /* Image 0x0463C onward: the text. A descriptor at ds:0x4B0A through
     * image 0x04D4D - the same record walker the code screen uses, format
     * [x+1][y][flag] then characters to a 0x0D - and then one of three more
     * at ds:0x4BA0, ds:0x4BC3 or ds:0x4BE5 chosen by ds:0x25A6, the input
     * device, so the last line names the mouse, the joystick or the
     * keyboard. Image 0x04660 polls ds:0x14 for the button afterwards. */
    snprintf(line, sizeof(line), "%s", T_LEVEL);   /* image 0x0463C */
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


/* ======================================================================
 * THE MENU'S STATE, and the port's handles on it.
 *
 * These sat in sdl_io.c while the variables they touch were declared here -
 * a getter and a setter over another file's globals, on the wrong side of
 * the boundary. ds:0x21 is the banner's scroll and the animation counters
 * are the game's; none of it is anything the hardware did.
 * ====================================================================== */
/*
 * The port's own accessors. The original keeps this in ds:0x21 and has no
 * function around it.
 */
void menu_set_scroll(uint32_t pixels) { menu_scroll = pixels; }

/* The port's own: the original never sets its animation counters from
 * outside, it only steps them. This is here so a verifier can put the port at
 * the phase a capture was taken at - the same job --scroll does for the
 * banner, and needed for the same reason. */
void menu_set_anim(const int16_t *ctr, uint8_t n)
{
    anim_started = 1;
    for (uint8_t i = 0; i < n && i < MENU_ANIM_N; i++)
        anim_ctr[i] = ctr[i];
}

/* The port's own, all three: the original reads ds:0x21 and its lemming frame
 * where it needs them and has no function around either. */
uint32_t menu_get_scroll(void)        { return menu_scroll; }
void menu_set_lem_frame(uint8_t f)    { menu_lem_frame = f; }
uint8_t menu_get_lem_frame(void)      { return menu_lem_frame; }

/* The menu with the rating it starts on.
 *
 * The port's own.
 */
int menu_build(const char *dir, menu_screen *m, const char **err)
{
    return menu_build_rating(dir, 0, m, err);
}


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


/* THE FX PLATE'S LABEL, which is not part of its plate bitmap - and which is
 * drawn OPAQUELY, unlike everything else on this screen.
 *
 * Its own black is real, not transparent: keying it out leaves the plate's
 * yellow showing where the original has black around the letters, and costs
 * 94.04% against 98.79% over the label's neighbourhood. That it is opaque
 * follows from what it is for - the label swaps between FX and its alternate,
 * so drawing one has to erase the other, and a keyed blit cannot.
 *
 * It is a routine rather than a block inside the set-up because image 0x04444
 * and image 0x04467 both reach it - once on the way in and once a pass
 * thereafter, through the guard in menu_fx_plate. */
static void draw_fx_label(uint8_t *dst, const uint8_t *sec)
{
    const int lab = menu_label_sprite();             /* image 0x04E5A */
    if (lab == LABEL_NONE) {
        /* IMAGE 0x04E84 REDRAWS THE BARE PLATE, and the port has to as well
         * now. It returns here rather than drawing, on the argument - true
         * when it was written - that composing the screen ONCE makes "no
         * label" and "the plate underneath" the same picture.
         *
         * Transcribing image 0x0445B's loop ended that. The FX plate is now
         * redrawn live whenever image 0x04E37's guard fires, over a screen
         * that already has a label on it, so not drawing leaves the PREVIOUS
         * one there - and the third setting showed the second's word. A
         * player found it: "you're missing the third option displayed".
         *
         * The original never had the choice. It draws the label over the
         * plate, so erasing it means putting the plate back, which is what
         * image 0x04E84's blit is: si = 0xB120 masked at si + 0xAB9 to
         * (324,120), 120 wide and 61 high - plate 2 at plate 2's place. */
        draw_plate_row(dst, sec, 2, 1,
                       (uint16_t)(SIGN_ROW[0].x + 2 * SIGN_STRIDE_X),
                       SIGN_ROW[0].y);               /* image 0x04E84 */
        return;
    }
    for (uint16_t sy = 0; sy < LABEL_H; sy++) {
        uint16_t y = (uint16_t)(LABEL_Y + sy);
        if (y >= MENU_H) break;
        for (uint16_t sx = 0; sx < LABEL_W; sx++) {
            uint16_t x = (uint16_t)(LABEL_X + sx);
            if (x >= BG_DRAWN_W) break;
            dst[(size_t)y * MENU_W + x] =
                planar(sec, LABEL_BASE + (uint32_t)lab * LABEL_SPRITE,
                       LABEL_PLANE, LABEL_STRIDE, 4, sx, sy);
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

    /* Image 0x04416 and image 0x0441B, before either plate is drawn: both owe
     * a draw, whatever their guards go on to read. The set-up then draws them
     * once at image 0x04441 and image 0x04444, and the loop's own calls take
     * over from the pass after.
     *
     * The set-up draws the rating it was GIVEN rather than ds:0x1F6E's,
     * because `lemdev --rating` forces one the game does not know about. The
     * two are the same in ./lemmings, and where they are not the loop's first
     * pass redraws from ds:0x1F6E - which is the original's answer, since it
     * has no such parameter to disagree with. */
    menu_plates_arm();
    draw_rating(m->pix, font, rating);               /* image 0x04441 */
    draw_fx_label(m->pix, sec);                      /* image 0x04444 */

    draw_anim(m->pix, font, fontlen);

    /* Keep section 4 so the banner can be redrawn as it scrolls; menu_build
     * composes one frame and the strip moves. Failing to copy it is not fatal
     * - the screen is already complete - so the banner simply stays still. */
    m->gfx = malloc(fontlen);
    if (m->gfx) {
        memcpy(m->gfx, font, fontlen);
        m->gfx_len = fontlen;
    }
    /* And section 0, for the FX plate the loop redraws - see menu_fx_plate. */
    m->menu_sec = malloc(seclen);
    if (m->menu_sec) {
        memcpy(m->menu_sec, sec, seclen);
        m->menu_sec_len = seclen;
    }

    dat_free(&d);
    return 0;
}

/* Image 0x04453: `ax = ds:0x14; test ax, 7; jne` back to itself - the set-up
 * spins until every mouse button is RELEASED before image 0x0445B is entered.
 *
 * It is a debounce, and it has to be: image 0x0446D treats any button as F1,
 * so without this the click that dismissed the previous screen would still be
 * down on the menu's first pass and play a level immediately. The port had no
 * such wait, and did not need one only because its input came off an event
 * queue where a press is an edge. Polling makes it necessary again, and it is
 * the original's own answer to the same problem. */
void menu_wait_release(void)
{
    while (mouse_buttons() & 7)                       /* image 0x04453 */
        video_retrace_wait();
}

/* ======================================================================
 * THE TWO PLATES REDRAW THEMSELVES - image 0x04EA2 and image 0x04E37.
 *
 * Both are called every pass of the loop and both are GUARDED, and the guard
 * is what the port had been reproducing by other means. The rating's:
 *
 *     0x04EA2  al = ds:0x1F6F              what was drawn last
 *     0x04EA5  cmp al, 4 / jne 0x4EB1
 *     0x04EA9  ds:0x1F6F = ds:0x1F6E       the sentinel pass: catch up, draw
 *     0x04EB1  cmp ds:0x1F6E, al / je      unchanged: RETURN without drawing
 *     0x04EB7  ds:0x1F6F = 4               changed: arm one more pass, draw
 *
 * So a change draws on this pass AND the next - 4 is a sentinel meaning "one
 * more", not a rating - and the set-up's 0xFF at image 0x04416 is neither 4
 * nor any rating, so it takes the changed arm and draws.
 *
 * The port rebuilt the WHOLE SCREEN from a key press instead. That agrees on
 * every capture, because the picture is the same; it is a different
 * mechanism, and a mechanism that agrees on every state anyone has compared
 * is exactly what this port has shipped wrong twice.
 * ====================================================================== */

/* ds:0x1F70, AND IT IS NOT THE SETTING. Image 0x044BA flips it with
 * `xor byte [0x1f70], 0xff`, so it holds 0x00 or 0xFF and carries no value
 * at all - it exists only so that image 0x04E46's compare against ds:0x1F72
 * comes out unequal after any F3 press. A "something changed" bit, and the
 * setting itself is ds:0x1F71.
 *
 * Reading it as the setting index is the obvious mistake and it would have
 * worked on two of the three states. */

void menu_plates_arm(void)
{
    g_rating_drawn = 0xFF;              /* image 0x04416 */
    g_fx_drawn     = 0xFF;              /* image 0x0441B */
    g_fx_dirty     = 0;                 /* image 0x04420 */
}

/* Image 0x04EA2. `font` is MAIN.DAT section 4, where the rating sprites are. */
static void menu_rating_plate(uint8_t *pix, const uint8_t *font)
{
    uint8_t al = g_rating_drawn;                /* image 0x04EA2 */
    if (al == 4) {                              /* image 0x04EA5 */
        g_rating_drawn = level_rating();        /* image 0x04EA9 */
    } else if (level_rating() == al) {          /* image 0x04EB1 */
        return;                                 /* image 0x04EB5 */
    } else {
        g_rating_drawn = 4;                     /* image 0x04EB7 */
    }
    draw_rating(pix, font, level_rating());     /* image 0x04EC2 */
}

/* Image 0x04E37, the same shape on ds:0x1F72 against ds:0x1F70 - and its
 * sentinel is 2 rather than 4, because the FX plate has three states where
 * the rating has four. `sec` is MAIN.DAT section 0. */
static void menu_fx_plate(uint8_t *pix, const uint8_t *sec);

/* THE PORT'S OWN, for tests/screens.c: the guarded redraw the loop performs,
 * reachable without a window. See the FX-plate case there for why a test
 * built on fresh composes cannot see the fault this catches. */
void menu_fx_plate_for_test(menu_screen *m)
{
    if (m->menu_sec) menu_fx_plate(m->pix, m->menu_sec);
}

/* Image 0x04E37, the same shape on ds:0x1F72 against ds:0x1F70 - and its
 * sentinel is 2 rather than 4, because the FX plate has three states where
 * the rating has four. `sec` is MAIN.DAT section 0. */
static void menu_fx_plate(uint8_t *pix, const uint8_t *sec)
{
    uint8_t al = g_fx_drawn;                    /* image 0x04E37 */
    if (al == 2) {                              /* image 0x04E3A */
        g_fx_drawn = g_fx_dirty;                /* image 0x04E3E */
    } else if (g_fx_dirty == al) {              /* image 0x04E46 */
        return;                                 /* image 0x04E4A */
    } else {
        g_fx_drawn = 2;                         /* image 0x04E4C */
    }
    draw_fx_label(pix, sec);                    /* image 0x04E51 onward */
}

/* ======================================================================
 * IMAGE 0x0445B - THE MENU LOOP ITSELF.
 *
 * Every call in it had a transcribed counterpart already; what did not was
 * the SEQUENCE, and this port has learnt twice over that a sequence written
 * out a second way is a sequence that will differ. The play loop was the
 * same debt and reading it found the fade, the cues and the music stop.
 *
 * This one carries less: its only state is the two dirty bytes above and
 * ds:0x14. What it does carry is the ORDER - a retrace, then compose, then
 * flip, then read input - and the rate that follows from it. The port's
 * shared window loop paced itself the same 70 Hz by a constant of its own;
 * here it is image 0x01550's retrace, which is where the number comes from.
 * ====================================================================== */
menu_action menu_loop(menu_screen *m, long budget)
{
    /* `budget` is the port's: a loop that only ends on a keypress cannot be
     * driven by a test, and LEM_FRAMES bounds a run. It is HERE rather than
     * in a copy of the loop, which is what sdl_io.c used to hold - four of
     * this loop's seven calls, so a capped run rendered a menu with no
     * animation, no rating plate and no FX plate, and every measurement taken
     * through it was of a screen the game never shows. */
    for (; budget != 0; budget--) {
        video_retrace_wait();                   /* image 0x0445B: call 0x1550 */

        if (m->gfx) {
            menu_frame_step(m, m->pix);         /* image 0x0445E: call 0x7D0D */
            /* Image 0x04461's own call, which menu_frame_step had absorbed.
             * Two lines apart in the original and two routines. */
            menu_anim_step();                   /* image 0x04461: call 0x4F96 */
            draw_anim(m->pix, m->gfx, m->gfx_len);      /* image 0x04FD3 */
            menu_rating_plate(m->pix, m->gfx);  /* image 0x04464: call 0x4EA2 */
        }
        if (m->menu_sec)
            menu_fx_plate(m->pix, m->menu_sec); /* image 0x04467: call 0x4E37 */

        video_page_flip();                      /* image 0x0446A: call 0x4C20 */

        /* Image 0x0446D: ANY button, and it does not once look at where the
         * pointer is - image 0x04473 jumps straight to image 0x0454C, which
         * is key 0x80's own target. A click on this screen is F1. */
        if (mouse_buttons() & 7)
            return MENU_PLAY;

        /* Image 0x04478's call, which scans ds:0x0F78 and translates through
         * ds:0x202E - one routine in the original, and one here. */
        uint8_t ch = key_scan();                /* image 0x04478: call 0x4D1A */
        if (ch == 0xFF) continue;               /* image 0x04D2B: nothing down */
        int32_t r = menu_dispatch(ch);          /* image 0x0447B */

        /* THREE OF THE SEVEN DO NOT LEAVE, and that is what the plates'
         * dirty bytes are for. Image 0x044B3, image 0x044EC and image
         * 0x0451C each do their work and `jmp 0x445B` - straight back to the
         * top of this loop - so the screen is never rebuilt and the plate
         * redraws itself on the next pass from its own guard.
         *
         * The port left the loop on all three, rebuilt the WHOLE menu from
         * main.dat, and re-entered. Same picture; it also meant the guards
         * could never refuse anything, because every press arrived at a
         * freshly-armed screen. */
        switch (r) {
        case MENU_FX:                                  /* image 0x04486 */
            sound_settings_cycle();                    /* image 0x044B3 */
            continue;
        case MENU_RATING_UP:                           /* image 0x04495 */
            level_rating_up();                         /* image 0x044EC */
            continue;
        case MENU_RATING_DOWN:                         /* image 0x0449D */
            level_rating_down();                       /* image 0x0451C */
            continue;
        default:
            break;
        }
        /* And the four that do: image 0x0454C for play or a click, image
         * 0x04967 for the code screen, image 0x04A01 for the input screen
         * and image 0x004D0 for Esc. */
        if (r >= 0) return (menu_action)r;
    }
    return MENU_EXIT;              /* the budget, not the game */
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


/* image 0x04D45..0x04D4B: the scancode it found, through ds:0x202E. */
uint8_t menu_key_code(uint8_t scancode)
{
    return (scancode < 0x53) ? MENU_XLAT[scancode] : 0xFF;
}

/* WHAT THE MENU DOES WITH A CODE - image 0x0447B, the compare chain, in its
 * own order:
 *
 *     0x0447B  cmp al, 0x80  ->  image 0x0454C   F1, play
 *     0x04482  cmp al, 0x81  ->  image 0x044AD   F2
 *     0x04486  cmp al, 0x82  ->  image 0x044B3   F3, the sound settings
 *     0x0448A  cmp al, 0x83  ->  image 0x044B0   F4
 *     0x0448E  cmp al, 0x1B  ->  image 0x004D0   ESC, out to DOS
 *     0x04495  cmp al, 0x8A  ->  image 0x044EC   Up
 *     0x04499  cmp al, 0x60  ->  image 0x044EC   `, the same
 *     0x0449D  cmp al, 0x8B  ->  image 0x0451C   Down
 *     0x044A4  cmp al, 0x2D  ->  image 0x0451C   -, and keypad - via the
 *                                                table, which maps both to
 *                                                0x2D
 *
 * There is no box and no position anywhere in it. Image 0x04473 gets there
 * first: any of ds:0x14's low three bits jumps to image 0x0454C, which is
 * where 0x80 goes - so a click is F1 wherever it lands, which is why
 * "EXIT TO DOS" starts the level when clicked.
 *
 * This replaced a hotspot table with a box per plate. Nine of its ten boxes
 * were zero-sized and could not be hit, and the positions written into them
 * came from image 0x04F31's plate loop through two functions that existed for
 * no other purpose. The original has no cursor to aim and nothing to aim at.
 */
int32_t menu_dispatch(uint8_t code)
{
    switch (code) {
    case 0x80: return MENU_PLAY;        /* image 0x0447B */
    case 0x81: return MENU_NEW_LEVEL;   /* image 0x04482 */
    case 0x82: return MENU_FX;          /* image 0x04486 */
    case 0x83: return MENU_INPUT;        /* image 0x0448A */
    case 0x1B: return MENU_EXIT;        /* image 0x0448E */
    case 0x8A: case 0x60: return MENU_RATING_UP;    /* image 0x04495 */
    case 0x8B: case 0x2D: return MENU_RATING_DOWN;  /* image 0x0449D */
    default:   return -1;               /* image 0x044AB falls through */
    }
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

/* IMAGE 0x07D0D - THE RIBBON, and only the ribbon.
 *
 * It had the walk-on lemmings folded into it, which is a call the LOOP makes:
 * image 0x0445E is 0x7D0D and image 0x04461 is 0x4F96, two lines apart and
 * two different routines. Image 0x07D0D's own body is the pause counter at
 * ds:0x23, the scroll at ds:0x21 stepping four, the 0xFF marker that reloads
 * the pause with 0x64 - a hundred - and the redraw. Nothing else.
 *
 * The RATE is still not transcribed here, but it no longer has to be
 * inferred: menu_loop waits image 0x01550's retrace once a pass, so a step
 * per displayed frame is the structure rather than a reading of it. */
int menu_frame_step(void *ctx, uint8_t *pix)
{
    menu_screen *m = (menu_screen *)ctx;
    static uint16_t pause;
    if (!m->gfx) return 0;
    if (pause) { pause--; return 0; }       /* image 0x07D10: ds:0x23 */
    uint32_t next = menu_get_scroll() + MENU_SCROLL_STEP;   /* image 0x07D23 */
    /* A character boundary just crossed: if the one that entered was preceded
     * by a pause marker, hold. */
    if ((next % BAR_TILE_W) == 0 && scroll_pauses_at(next / BAR_TILE_W))
        pause = BANNER_PAUSE;               /* image 0x07D3C: 0x64 */
    menu_set_scroll(next);                  /* image 0x07D26 */
    draw_banner(pix, m->gfx, m->gfx_len, next,
                (uint8_t)(((((next + BAR_SCROLL_INIT) >> 2)
                            - LEM_FRAME_BIAS)) & (LEM_FRAMES - 1)));
    return 1;
}

/* One frame of a level, in the order image 0x019C runs it: the release at
 * image 0x030A6, the simulation at image 0x01F19, the objects at image
 * 0x07BB8, and the lemmings LAST at image 0x01BC.
 *
 * This used to step the objects and nothing else, so the game you could
 * actually play showed an empty level while every frame-by-frame measurement
 * passed - the measurements go through lemdev, which drives the release and
 * the simulation itself. A harness that exercises what the game does not is
 * the failure this whole method is supposed to prevent, so the two now call
 * the same routines in the same order. */
int level_frame_tick(void *ctx, uint8_t *pix)
{
    struct level_frame *a = (struct level_frame *)ctx;
    uint16_t w = a->from->w, h = a->from->h;

    /* IMAGE 0x001A7 - the page being drawn gets the master back where it
     * was drawn over. It used to get ALL of the master, which is the same
     * picture except where the master changed under a tile nothing marked;
     * see level_page_refresh. */
    level_page_refresh(a->from, pix, a->view_x);    /* image 0x080FD */
    /* IMAGE 0x0019D'S ORDER: STEP, THEN DRAW - image 0x001B3 walks the
     * lemmings and image 0x001BC draws them, on EVERY pass including the
     * first.
     *
     * This used to skip the step on frame 0, and the comment explaining it
     * said the quiet part out loud: "the original's loop steps and then draws,
     * but its FIRST CAPTURED FRAME is the one before any of that". That is a
     * fact about where record_level.py captures - image 0x018F, after the page
     * flip, so the picture it saves was composed by the previous pass - and
     * the port answered it by changing the GAME. A frame of simulation was
     * skipped to make a comparison line up.
     *
     * It is the same shape as a constant chosen to make the observed behaviour
     * come out right: it moved the number and it was not what the program
     * does. An alignment belongs in the tool that aligns. */
    level_play_step(a->st, &a->lv, a->lem, &a->released,
                    a->from->pix, w, h);
    /* Image 0x001E7 and image 0x001FA: everyone released and none active, or
     * the clock run out. Which of the two it was does not decide win or lose -
     * that is the saved count against the level's requirement. Both go to
     * image 0x002B3, which does not end the loop: it arms the fade and lets it
     * run. */
    if (!g_level_over && level_over(&a->lv, a->released,
                                    lemmings_live(a->lem, a->released)))
        level_end_begin();                      /* image 0x002B3 */
    level_play_draw(a->st, &a->lv, a->sec, a->len,
                    a->lem_sec, a->lem_len, a->lem, a->released,
                    pix, w, h, a->view_x);
    return 1;
}


/* THE BRIEFING - IMAGE 0x04566, and image 0x04660's wait at the end of it.
 *
 * The text and the composition are menu_briefing_screen's, transcribed from
 * image 0x04566 through image 0x0465D. What was missing is what happens
 * AFTER, and it is not one answer but two:
 *
 *     0x04660  ax = ds:0x14 / test ax,7 / jne 0x04660   wait for RELEASE, so
 *                              the click that opened this screen is not read
 *                              again as the one that leaves it
 *     0x04668  call 0x046B9                             a retrace
 *     0x04680  call 0x046B9
 *     0x04683  ax = ds:0x14 / test ax,7 / jne 0x046A2   a mouse button: PLAY
 *     0x0468B  ax = ds:0x1FC7
 *     0x0468E  test ax,0x20 / jne 0x0469B               that key: THE MENU
 *     0x04693  test ax,0x40 / je 0x04680                otherwise keep waiting
 *     0x04698  jmp 0x046A2                              PLAY
 *     0x0469B  call 0x046A2 / pop ax / jmp 0x04401      the menu, and the POP
 *                              is what discards the level this briefing was
 *                              for - it does not return to its caller
 *     0x046A2  the fade out, on both answers
 *
 * THE PORT HAD ONE ANSWER. menu_screen_show returned void and its
 * input handler gave Esc the same code as a click, so both played and there
 * was no way back to the menu from a briefing - a screen the original can be
 * left from without playing the level.
 *
 * Returns 0 to play, BRIEF_MENU to go back, -1 on an error. */
int level_show_briefing(const char *dir, const level *lv,
                        uint8_t scale, const char **err)
{
    menu_screen br;
    if (menu_briefing_screen(dir, lv, level_rating(), &br, err) != 0)
        return -1;
    int32_t how = menu_screen_show(&br, "PC Lemmings", scale);
    menu_free(&br);
    /* image 0x0468E's 0x20 is the only one that does not play. */
    return how == BRIEF_MENU ? BRIEF_MENU : 0;
}


/* IMAGE 0x047A9 - THE RESULTS SCREEN'S OWN WAIT.
 *
 *     0x047A9  ax = ds:0x14 / test ax,7 / jne 0x047A9   wait for RELEASE
 *     0x047B1  ax = ds:0x14
 *     0x047B4  test ax,1 / jne 0x047D7    left  -> the next level
 *     0x047B9  test ax,2 / jne 0x047CB    right -> the menu
 *     0x047C9  je 0x047B1                 otherwise keep waiting
 *
 * NOT the menu's hotspot matcher, which is what this used to be. That matcher
 * reports WHERE a click landed and not WHICH button made it, so it gave the
 * same answer for both and a right click advanced instead of going back; and
 * it takes a press at once, where image 0x047A9 waits for every button to be
 * UP first - which is what stops the click that dismissed the level from
 * being read again here.
 *
 * The release wait works because ds:0x14 is a STATE, so a screen entered with
 * nothing held passes it immediately. Waiting for an up EVENT instead made a
 * player click once for nothing, to manufacture the release, before any click
 * counted: there is no event to wait for when the button is already up.
 *
 * ds:0x14 is view_buttons here - see sdl_io.c, which also explains why
 * reading it pumps a queue and costs ten milliseconds. */
int32_t level_results_wait(const uint8_t *pix, const lem_rgb *pal,
                           uint8_t scale)
{
    if (view_open(MENU_W, MENU_H, "PC Lemmings", scale) != 0)
        return RESULTS_MENU;
    video_screen_set(pix, MENU_W, MENU_H, pal, NULL, 0);
    video_page_flip();                          /* image 0x04C20 */

    while (mouse_buttons() & 7) { }          /* image 0x047A9 */
    for (;;) {
        uint16_t b = mouse_buttons();        /* image 0x047B1 */
        if (b & 1) return RESULTS_NEXT;     /* image 0x047B4 */
        if (b & 2) return RESULTS_MENU;     /* image 0x047B9 */
    }                                       /* image 0x047C9 */
}


/* THE RESULTS SCREEN, image 0x046EB.
 *
 * The composition is level_results_draw's; the window and its palette are
 * sdl_io.c's, reached through view_open and view_buttons. What is here is
 * the ROUTINE: which bytes it reads, and image 0x047A9's wait.
 *
 * The part the hardware used to do:
 * a 640x350 mode 0x10 picture, the palette it is shown through, and a loop
 * that waits for a button. That loop is image 0x047A9's, and it polls -
 * there is no retrace wait in it at all, which is the sort of thing worth
 * knowing before writing a probe for the screen.
 *
 * The BACKGROUND is not drawn. Image 0x046EB's set-up calls have not been
 * read, so this puts the text on a cleared screen and the difference is
 * whatever those calls draw - which is the honest state of it, and visible
 * rather than hidden.
 *
 * Returns 1 for the left button (image 0x047B4: on to the next level) and
 * 0 for anything else.
 */
/* THE SCREEN'S I/O, and only that: load the file it draws from, hand the
 * composing to game.c, put it on the display, and read a button. What it
 * SHOWS - the verdict line, the percentages, whether the clock ran out - is
 * image 0x046EB's and lives in game.c; this used to be given all of it as
 * arguments by main.c. */
int level_show_results(const char *dir, uint8_t level_no, const char *code,
                       uint8_t scale, const char **err)
{
    char path[1024];
    dat_file mainf;
    snprintf(path, sizeof(path), "%s/main.dat", dir);
    if (dat_load(path, &mainf, err) != 0) return 0;
    if (mainf.count <= RESULTS_FONT_SECTION) {
        dat_free(&mainf);
        if (err) *err = "main.dat has no section 4 - no results font";
        return 0;
    }

    uint8_t *pix = calloc((size_t)MENU_W * MENU_H, 1);
    if (!pix) { dat_free(&mainf); return 0; }

    const uint8_t *bg = NULL; uint32_t bg_len = 0;
    if (mainf.count > RESULTS_BG_SECTION) {
        bg = mainf.sections[RESULTS_BG_SECTION].data;
        bg_len = mainf.sections[RESULTS_BG_SECTION].size;
    }
    /* image 0x04710 asks the CLOCK for itself, and image 0x0471D and image
     * 0x04720 read ds:0x40 and ds:0x41 - none of it comes from a caller. */
    level_results_draw(pix, MENU_W, MENU_H,
                       mainf.sections[RESULTS_FONT_SECTION].data,
                       mainf.sections[RESULTS_FONT_SECTION].size,
                       bg, bg_len,
                       level_saved_pct(), level_needed_pct(),
                       level_out_of_time(), lem_input_kind,
                       level_no, code);

    /* THE SCREEN'S OWN WAIT, transcribed from image 0x047A9 - not the menu's
     * hotspot matcher, which cannot tell one button from another:
     *
     *     0x047A9  ax = ds:0x14 / test ax,7 / jne 0x47A9   wait for RELEASE
     *     0x047B1  ax = ds:0x14
     *     0x047B4  test ax,1 / jne 0x47D7    left  -> the next level
     *     0x047B9  test ax,2 / jne 0x47CB    right -> the menu
     *     0x047C9  je 0x47B1                 otherwise keep waiting
     *
     * Two things the matcher got wrong. It returned the same answer for
     * either button, so a right click advanced instead of going back to the
     * menu; and it takes a press immediately, where the original waits for
     * every button to be UP first - which is what stops the click that
     * dismissed the level being read again by this screen.
     */
    /* IMAGE 0x047A9's WAIT. The decision is here; the polling is
     * mouse_buttons's, because ds:0x14 was a byte the mouse driver kept up to
     * date and its counterpart is an SDL event queue.
     *
     *     0x047A9  ax = ds:0x14 / test ax,7 / jne 0x047A9   wait for RELEASE
     *     0x047B1  ax = ds:0x14
     *     0x047B4  test ax,1 / jne 0x047D7   left  -> the next level
     *     0x047B9  test ax,2 / jne 0x047CB   right -> the menu
     *     0x047C9  je 0x047B1                otherwise keep waiting
     *
     * Two things the menu's hotspot matcher got wrong before this was read:
     * it gave the same answer for either button, so a right click advanced
     * instead of going back; and it took a press at once, where image 0x047A9
     * waits for every button to be UP first - which is what stops the click
     * that dismissed the level being read again here. */
    lem_rgb pal[16];
    level_results_palette(pal);
    int32_t out = level_results_wait(pix, pal, scale);

    free(pix);
    dat_free(&mainf);
    return out;
}


/* IMAGE 0x036B6 - THE LEVEL RECORD INTO THE RUNNING STATE, THEN THE TERRAIN.
 *
 *     0x036B6  call 0x0368E       the master at 0x4A40 cleared, 0x8880 bytes,
 *                                 and declared as the blitter's surface
 *     0x036BC  [di+0x00] -> ds:0x64 AND ds:0x67   the release rate: the
 *                                 level's own is both the floor and the
 *                                 starting value
 *     0x036C7  [di+0x02] -> ds:0x47   how many lemmings there are
 *     0x036D0  [di+0x04] -> ds:0x44   how many must be saved
 *     0x036D9  [di+0x06] + 0x30 -> ds:0x20A7   the minutes, already the DIGIT
 *                                 the status line prints
 *     0x036E5  [di+0x08] -> ds:0x6A   and seven more three bytes apart, up to
 *      ..0x0372A                  ds:0x7F - the eight skills in header order
 *     0x0372D  [di+0x18] + 0x10 -> ds:0x00   THE SCROLL. The sixteen in it is
 *                                 the offset this port carries as the
 *                                 difference between a record x and a canvas
 *                                 x, which is why view_x is start_x and not
 *                                 start_x + 16
 *     0x03739  ds:0x00 >> 3 -> ds:0x02   its byte column
 *     0x03742  ds:0x0FE8 non-zero -> image 0x01A7B, the VGASPEC picture
 *     0x0374F  otherwise si = ds:0x10EB, four bytes an entry, image 0x07C67
 *                                 for each, ending on 0xFFFF
 *     0x0376A  done - the instruction tools/prove.sh stops at
 *
 * THE UNPACK COMES BEFORE THE COMPOSITION. The port had those the other way
 * round: it composed the terrain and then reset the state from the header.
 * Nothing read that state in between, so it could not show - but the order is
 * the routine's, and the next thing put between them would have inherited
 * whichever one happened to be there.
 *
 * The port's level_status_reset carries image 0x036C7, image 0x036D0 and the
 * skills at image 0x036E5, because it calls level_skills_reset itself. */
static int level_begin(canvas *terrain, struct level_frame *A,
                       ground *g, const uint8_t *sec, uint32_t len,
                       const uint8_t *spec, uint32_t spec_len,
                       const char **err)
{
    level_release_reset(&A->lv);      /* image 0x036BC: ds:0x64 and ds:0x67 */
    level_status_reset(&A->lv);       /* image 0x036C7 .. image 0x0372A */
    A->view_x = A->lv.start_x;        /* image 0x0372D, less the sixteen */
    /* image 0x03742: ds:0x0FE8 sends a VGASPEC level to image 0x01A7B and the
     * rest to image 0x0374F's piece loop; image 0x0368E's clear is the
     * canvas's own, which starts empty. */
    if (spec) {
        if (render_vgaspec(terrain, g, spec, spec_len, err) != 0) return -1;
    } else if (render_terrain(terrain, &A->lv, g, sec, len, err) != 0) {
        return -1;
    }
    minimap_build(terrain->pix, terrain->w, terrain->h);   /* image 0x03779 */
    return 0;
}


/* IMAGE 0x0197D - THE LEVEL'S FILES, LOADED ONCE.
 *
 * The original loads here and nowhere else: image 0x0455D calls this before
 * the briefing at image 0x04560, and the level record it writes to ds:0x0FCB
 * is what BOTH the briefing and the play loop then read. Nothing parses a
 * level twice.
 *
 * The port used to, and its own main.c said so - "Set once for the briefing
 * and once for the level, because each parses". Two parses is two chances to
 * disagree, and it made the ODDTABLE entry something the caller had to
 * remember to hand over twice.
 *
 * Returns 0, or 1 for one of the four VGASPEC levels - see
 * level_graphics_names - or -1 with *err set. */
int level_load(const char *dir, const char *file, uint32_t which,
               level *lv, const char **err)
{
    char path[1024];
    dat_file levels;
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    if (dat_load(path, &levels, err) != 0) return -1;     /* image 0x019B3 */
    if (which < 1 || which > levels.count) {
        dat_free(&levels);
        if (err) *err = "no such level in that container";
        return -1;
    }
    int rc = level_parse(levels.sections[which - 1].data,
                         levels.sections[which - 1].size, lv, err);
    dat_free(&levels);
    if (rc != 0) return -1;
    /* ODDTABLE.DAT, image 0x019F5 - half the plates play their level under
     * another name with other skills. */
    if (level_apply_odd(dir, lv, level_odd_taken(), err) != 0) return -1;
    lv->level_number = level_number();       /* image 0x04566 */
    char gn[16], xn[16];
    return level_graphics_names(lv, gn, sizeof gn, xn, sizeof xn) == 1;
}

/* THE LEVEL ITSELF - IMAGE 0x00455, the level's set-up, and then image
 * 0x0454C's `jmp 0x000FB`, the loop whose frame is written out above
 * level_play_step.
 *
 *     0x00458  clear ss:0x42B2, 0x642 bytes   the trigger map
 *     0x00462  call 0x00FB2   int 10h ax=0x8D - mode 0x0D
 *     0x00465  call 0x052F4   the level palette, branching on ds:0x1F7C
 *     0x00468  call 0x015AD   the PIT divisor into ds:0x1FC9
 *     0x0046B  call 0x036B6   the header into the running state, and the
 *                             terrain composed - it calls image 0x0368E to
 *                             clear the master at 0x4A40 first
 *     0x0046E  if ds:0x1F73 != 0xFF: INT 33h AX=4 to (0x96, 0x64)
 *     0x00480  call 0x0410F   clear ds:0x0EA8, the on-screen column table
 *
 * It takes the level ALREADY LOADED, because image 0x0197D ran before the
 * briefing and its record is what both of them read.
 *
 * WHAT MAPS WHERE. The trigger map is level_triggers_set; the mode and the
 * palette are this file's, being what the card used to do; the divisor is
 * LEVEL_FRAME_HZ, which paces the loop; image 0x036B6's two halves are the
 * level_*_reset calls below and render_terrain. Image 0x0410F's table is
 * rebuilt per frame rather than here - see the note at image 0x07070.
 *
 * The pointer centring is NOT missing. Image 0x0046E skips it when ds:0x1F73
 * is 0xFF, which is the mouse, and this port always uses the mouse - so the
 * original does not do it either in the configuration being reconstructed.
 *
 * AND THE MASTER IS COMPOSED TWICE, once here and once for the briefing's
 * preview at image 0x0394F, which calls the same image 0x0368E. That is worth
 * stating because the opposite was believed here for a while: that the
 * briefing composed it and the play loop only windowed it through image
 * 0x07900. Image 0x036B6's first instruction refutes that in one line, and a
 * restructuring of this file was very nearly done on the strength of it. */
int level_show(const char *dir, const level *lv, uint8_t scale,
               const char **err)
{
    char path[1024];
    dat_file gfx;
    ground g;
    int rc = 0;

    /* image 0x0197D's tail names both files and says which kind this is. */
    char gname[16], xname[16];
    int special = level_graphics_names(lv, gname, sizeof gname,
                                       xname, sizeof xname);
    if (special < 0) { if (err) *err = "level names too long"; return -1; }
    snprintf(path, sizeof(path), "%s/%s", dir, gname);
    if (ground_load(path, &g, err) != 0) return -1;
    /* A VGASPEC level's terrain is section 0 of vgaspecN.dat, image 0x01A5A,
     * and its objects are vgagrN.dat's, which image 0x01B19 loads back once
     * the picture is down - so gfx is vgagrN.dat on both paths, and only an
     * ordinary level draws its section 0. */
    dat_file spec = { NULL, 0 };
    if (special) {
        snprintf(path, sizeof(path), "%s/%s", dir, xname);
        if (dat_load(path, &spec, err) != 0) return -1;
        if (spec.count < 1) { dat_free(&spec);
                              if (err) *err = "no VGASPEC section";
                              return -1; }
        level_objects_name(lv, xname, sizeof xname);    /* image 0x01B19 */
    }
    snprintf(path, sizeof(path), "%s/%s", dir, xname);
    if (dat_load(path, &gfx, err) != 0) { dat_free(&spec); return -1; }
    if (gfx.count < 1) { dat_free(&gfx); if (err) *err = "no terrain section";
                         return -1; }

    /* c and c2 are the two pages, ds:0x1F76 and ds:0x1F78 - see
     * level_page_refresh. */
    canvas c, c2, terrain;
    if (canvas_init(&c) != 0 || canvas_init(&c2) != 0
            || canvas_init(&terrain) != 0) {
        canvas_free(&c); canvas_free(&c2);
        dat_free(&gfx); if (err) *err = "out of memory"; return -1;
    }
    if (gfx.count < 2) {
        canvas_free(&c);
        canvas_free(&c2);
        canvas_free(&terrain);
        dat_free(&gfx);
        if (err) *err = "no sprite section";
        return -1;
    }

    static struct level_frame A;
    static dat_file mainf;
    int have_main = 0;
    A.lv = *lv; A.sec = gfx.sections[1].data; A.len = gfx.sections[1].size;
    A.from = &terrain;
    A.page[0] = c.pix;          /* video memory at 0 */
    A.page[1] = c2.pix;         /* and at 0x2520 */
    A.lem_sec = NULL; A.lem_len = 0; A.released = 0;
    A.pan_sec = NULL; A.pan_len = 0;
    free(A.pan_own); A.pan_own = NULL;
    A.lem = DG.lem;             /* ds:0x0088 */
    A.stepped = 0;
    A.panel = calloc((size_t)PANEL_LINE_W * PANEL_ART_H, 1);

    /* IMAGE 0x00458 - the trigger map, ss:0x42B2 for 0x642 bytes. It is
     * cleared before anything else and image 0x037C2 and image 0x03839 fill
     * it in; level_triggers_set is both. */
    level_triggers_set(&A.lv, &g);

    /* IMAGE 0x00462 through IMAGE 0x00468 are the machine, not the game:
     * `int 10h ax=0x8D` for mode 0x0D, the palette at image 0x052F4 chosen on
     * ds:0x1F7C, and the PIT divisor into ds:0x1FC9. This port sets the mode
     * and the palette when it opens the window and paces on LEVEL_FRAME_HZ,
     * which is that divisor's answer. */

    /* THE WIPE'S SHARE, image 0x04551. The original has already zeroed these
     * by the time it gets here - the lemming array at ds:0x88, the saved
     * count at ds:0x46 - because they sit inside the 0xFCA bytes it erases at
     * image 0x0455B. The port's arrays are per-level rather than a fixed run
     * of DGROUP, so it clears them where they are declared instead. */
    level_saved_reset();
    level_start_reset();
    lemmings_reset(A.lem);

    /* The lemming sprites live in main.dat, not in the level's own
     * graphics - so the level path has to open it too. */
    snprintf(path, sizeof(path), "%s/main.dat", dir);
    {
        const char *e2 = "unknown error";
        if (dat_load(path, &mainf, &e2) == 0
                && mainf.count > LEM_SPRITE_SECTION) {
            A.lem_sec = mainf.sections[LEM_SPRITE_SECTION].data;
            A.lem_len = mainf.sections[LEM_SPRITE_SECTION].size;
            have_main = 1;
            if (mainf.count > PANEL_FONT_SECTION) {
                A.pan_sec = mainf.sections[PANEL_FONT_SECTION].data;
                A.pan_len = mainf.sections[PANEL_FONT_SECTION].size;
                /* IMAGE 0x05976 - THE COMPAT PANEL, and it is TWO things
                 * from one section.
                 *
                 * Image 0x059A1 unpacks section 6 from offset 0 through the
                 * same image 0x057D1 panel_frame is transcribed from - that
                 * is the ARTWORK. Then image 0x059B4 copies 0x720 bytes from
                 * its 0x1900 over the FONT at 0xA0, and 0xA0 + 0x720 = 1984,
                 * which is the font's whole size: image 0x0593C copies 0x3E0
                 * words of it from section 2's own 0x1900. So the glyphs are
                 * replaced and the 0xA0-byte table in front of them is not.
                 *
                 * Section 6 is 8,224 bytes = 6,400 of artwork + 1,824 of
                 * glyphs, against section 2's 6,400 + 1,984: the compat font
                 * carries no table of its own because it borrows section 2's.
                 */
                g_panel_compat_sec = NULL;
                if (lem_machine == LEM_PC_COMPATIBLE
                        && mainf.count > PANEL_COMPAT_SECTION) {
                    const dat_section *cs =
                        &mainf.sections[PANEL_COMPAT_SECTION];
                    if (cs->size >= PANEL_COMPAT_FROM + PANEL_COMPAT_LEN) {
                        g_panel_compat_sec = cs->data;
                        g_panel_compat_len = cs->size;
                        /* The font, in the PORT's coordinates: its glyphs sit
                         * at PANEL_FONT_OFF + 0xA0 of the section, not at
                         * 0xA0 - which is where a first attempt put them, and
                         * it matched nothing. */
                        if (A.pan_len >= PANEL_FONT_OFF + 0xA0
                                         + PANEL_COMPAT_LEN) {
                            A.pan_own = malloc(A.pan_len);
                            if (A.pan_own) {
                                memcpy(A.pan_own, A.pan_sec, A.pan_len);
                                memcpy(A.pan_own + PANEL_FONT_OFF + 0xA0,
                                       cs->data + PANEL_COMPAT_FROM,
                                       PANEL_COMPAT_LEN);
                                A.pan_sec = A.pan_own;
                            }
                        }
                    }
                }
            }
        }
    }

    /* IMAGE 0x0046B - the record into the state, and then the terrain. */
    rc = level_begin(&terrain, &A, &g, gfx.sections[0].data,
                     gfx.sections[0].size,
                     spec.count ? spec.sections[0].data : NULL,
                     spec.count ? spec.sections[0].size : 0, err);
    dat_free(&spec);

    /* IMAGE 0x0046E puts the pointer at (0x96, 0x64) through INT 33h AX=4,
     * and only when ds:0x1F73 is not 0xFF - which is to say only when the
     * input device is NOT the mouse. This port is always the mouse, so the
     * original does not do it here either.
     *
     * IMAGE 0x00480 clears ds:0x0EA8, the on-screen byte-column table. The
     * port rebuilds that per frame where image 0x07070 reads it, so there is
     * nothing to clear once here. */

    if (rc == 0) {
        level_anim_init(A.st, &A.lv, &g);
        level_pages_init(&A);                   /* image 0x0048C */
        /* One step before the first frame. Measured: without it the exit's
         * flames are one animation frame behind the original's at every
         * frame checked. It is image 0x00495's. */
        level_anim_step(A.st, &A.lv);
        /* IMAGE 0x00498 - the objects, once, on the page being drawn - which
         * after image 0x081CC's swap is the one at 0x2520, so the loop's
         * first frame draws on the other. Marked like any other blit, and
         * that is the only trace it leaves: those tiles go back to the
         * master the first time this page is drawn into. */
        level_anim_draw(A.st, &A.lv, A.sec, A.len, level_page_drawn(&A),
                        A.from->w, A.from->h, 0);
        level_anim_mark(A.st, &A.lv, A.view_x);
        /* IMAGE 0x00105 ON THE LOOP'S FIRST PASS. The original's loop swaps
         * at its HEAD, before a frame is composed; this port's swaps at its
         * tail, in level_frame_show, after the sink has the frame. So the
         * head's first swap is made here, and the first frame is composed on
         * page 0 as the original's is. Without it every frame was drawn on
         * the other page - invisible in any picture, because the two pages
         * are symmetric, and plain in tools/verify_dgroup.py, where
         * ds:0x1F76 and ds:0x26 read swapped on all forty frames. */
        level_pages_swap();                     /* image 0x00105 */
        /* image 0x04836 reads the level's extended graphics set. */
        music_set_level(&A.lv);
        /* IMAGE 0x0049E - ONE PANEL UPDATE BEFORE THE LOOP EVER RUNS, and the
         * port had none. The set-up's tail is image 0x0048F's panel INIT, the
         * objects at image 0x00495 and image 0x00498, image 0x0049B's window
         * of the terrain, and then image 0x03AC8 once - so by the end of the
         * loop's first pass the panel has been updated TWICE.
         *
         * That is exactly what the state comparison saw: both dividers are
         * zeroed by the init (image 0x039EE, image 0x03A1B) and the original
         * held 2 in each at the first captured frame where the port held 1 and
         * 0. One missing call, and it is only visible in the segment - the
         * panel is recomposed every frame, so no picture of it can differ. */
        if (A.panel && A.pan_sec)
            level_panel_build(A.panel, A.pan_sec, A.pan_len, &A.lv,
                              A.from->pix, A.from->w, A.from->h,
                              A.lem, A.released, A.view_x,
                              (uint8_t)(level_skill() + 1));   /* image 0x0049E */
    }
    if (rc == 0) {

        /* The level gets its own screen - 320x200, play area over panel -
         * rather than the harness viewer's window of the play area alone. */
        level_run(&A, &c, g.palette, "PC Lemmings", scale);
        /* ds:0x40 through its own routine, image 0x03AF4 - the panel writes
         * it every frame and this is the last one. The port kept a second
         * copy in a global of its own, which main.c then read instead.
         *
         * ds:0x4F needs nothing here any more: image 0x002B3 sets it inside
         * the loop, which is where the original sets it. */
        level_status_in(level_saved_percent(&A.lv));
        free(A.panel); A.panel = NULL;
        free(A.pan_own); A.pan_own = NULL;
        if (have_main) dat_free(&mainf);
    }
    canvas_free(&c);
    canvas_free(&c2);
    canvas_free(&terrain);
    dat_free(&gfx);
    return rc;
}
/* GROUNDxO.DAT - IMAGE 0x01A0B NAMES IT, IMAGE 0x01A18 LOADS IT.
 *
 * THE ORIGINAL NEVER PARSES THIS FILE. Image 0x01A11 hands the loader at
 * image 0x01916 a destination of ds:0x1B4B and a length of 0x1000, and every
 * reader indexes those bytes where they lie. This struct is the port's, and
 * what is transcribed is the LAYOUT - so each field below carries the
 * instruction in the original that reads it, not a line from a file format
 * description.
 *
 * OBJECTS, 28 bytes each from offset 0. Image 0x037D1 is `mov dx, 0x1c / mul
 * dx` on the object id and image 0x037D6 is `lea si, [0x1b4b]`, so the stride
 * and the base are both the original's:
 *
 *     +0x0E  trigger_left     image 0x037E9  add ax, [si + 0x0e]
 *     +0x10  trigger_top      image 0x037F6  add bx, [si + 0x10]
 *     +0x12  trigger_width    image 0x03815  mov dh, [si + 0x12]
 *     +0x13  trigger_height   image 0x03818  mov dl, [si + 0x13]
 *     +0x14  trigger_effect   image 0x03812  or  al, [si + 0x14]
 *
 * TERRAIN, 8 bytes each from offset 0x1C0. Image 0x07C9A is `mov ax, 8 / mul
 * dx` and image 0x07C9F is `add ax, 0x1d0b` - which is 0x1B4B + 0x1C0, so the
 * table's offset within the file is the original's arithmetic and not an
 * assumption:
 *
 *     +0x00  width            image 0x07CA4  mov dl, [bx]
 *     +0x01  height           image 0x07CA8  mov cl, [bx + 1]
 *     +0x02  image_loc        image 0x07CAB  mov si, [bx + 2]
 *     +0x04  mask_loc         image 0x07CAE  mov di, [bx + 4]
 *
 * PALETTES from 0x3C0: image 0x01AB5 is `lea di, [0x1f0b]`, which is
 * 0x1B4B + 0x3C0, and image 0x01AB2 copies 8 bytes.
 *
 * WHAT IS STILL THE PUBLISHED DESCRIPTION, said plainly because the point of
 * the addresses above is that these do not have one: the object fields at
 * +0x00, +0x02, +0x03, +0x04, +0x05, +0x06, +0x08, +0x15, +0x17 and +0x1B.
 * No reader of ds:0x1B4B touches them - the only two sites that index the
 * file are image 0x037D6 and the terrain one, and between them they account
 * for every field above and none of these. They are read from the PREPARED
 * array at ds:0x17CB instead, which image 0x0378A builds and image 0x02FBA
 * indexes with the same 28-byte stride, so the layouts are related and that
 * relation has not been read. Until it is, those ten are inherited knowledge
 * sitting next to measured knowledge, and the difference matters.
 *
 * The file is 1,056 bytes and the original asks for 4,096; DOS returns what
 * is there. */
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
        o->trigger_left    = le16(e + 0x0E);
        o->trigger_top     = le16(e + 0x10);
        o->trigger_width   = e[0x12];
        o->trigger_height  = e[0x13];
        o->trigger_effect  = e[0x14];
        o->frames_base_loc = le16(e + 0x15);
        o->preview_frame   = e[0x17];
        o->trigger_sound   = e[0x1B];
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
    /* THE SCREEN PALETTE AS THE PROGRAM SHIPS IT - ds:0x25B8, image 0x0C048,
     * sixteen 6-bit triples. Entries 0..6 are these; 7 ships as 38 20 08 and
     * 8..15 ship as zero because image 0x053A1 fills them in from the level's
     * own colours at run time - see below.
     *
     * TRANSCRIBED, NOT MEASURED. This was seven literals with a note saying
     * they had been read off a running emulator, and two of them disagreed
     * with the published file description - entry 4 is 2C 2C 00 where the file
     * says 3C 3C 00, and entry 7 is not fixed at all. Both are explained by
     * the buffer being the program's own: the game does not take those from
     * GROUNDxO.DAT, it carries them, and image 0x053AF then overwrites 7. */
    static const uint8_t FIXED[7][3] = {   /* image 0x0C048 */
        { 0x00, 0x00, 0x00 },   /* 0 transparent / background */
        { 0x10, 0x10, 0x38 },   /* 1 blue    - lemming bodies */
        { 0x00, 0x2C, 0x00 },   /* 2 green   - lemming hair   */
        { 0x3C, 0x34, 0x34 },   /* 3 white   - skin           */
        { 0x2C, 0x2C, 0x00 },   /* 4 dark yellow              */
        { 0x3C, 0x08, 0x08 },   /* 5 red                      */
        { 0x20, 0x20, 0x20 },   /* 6 grey                     */
    };
    const uint8_t *vga_custom  = buf + 0x3D8;
    /* 0x3D8 custom, 0x3F0 standard, 0x408 preview - eight triples each. */
    const uint8_t *vga_preview = buf + 0x408;
    for (uint8_t i = 0; i < 8; i++) {
        g->preview[i].r = widen6(vga_preview[i * 3 + 0]);
        g->preview[i].g = widen6(vga_preview[i * 3 + 1]);
        g->preview[i].b = widen6(vga_preview[i * 3 + 2]);
    }

    /* IMAGE 0x053A1 - the level's colours into the screen palette.
     *
     *     0x053A3  si = cs:[0x52E4]         the adapter's pointer table
     *     0x053A7  di = cs:[si + 0x0C]      ds:0x25B8, the screen palette
     *     0x053AB  si = cs:[si + 0x0A]      ds:0x1F23, where image 0x01AB2
     *                                       copied the ground file's custom
     *                                       eight
     *     0x053AF  ax = [si] / [di+0x15] = ax
     *     0x053B4  al = [si+2] / [di+0x17] = al   ENTRY 7 = custom[0]
     *     0x053BA  di += 0x18                     ... which is entry 8
     *     0x053BD  cx = 0x0C / rep movsw           entries 8..15, 24 bytes
     *
     * So entry 7 is not a fixed colour and not the file's seventh: it is a
     * copy of the level's first custom one, which is why it always equals
     * entry 8. The loop below is that routine. */
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
/* IMAGE 0x03150 - A CLICK IN THE PLAY AREA, and it is a routine of its own.
 *
 * The frame loop calls it directly, TWICE a pass - image 0x001AD, before the
 * lemmings step, and image 0x001C5, after image 0x001BF's pointer scan - and
 * image 0x03EF9 never reaches it: that routine leaves at image 0x03F04 for
 * anything above the panel. It polls; it is not handed a press.
 *
 *     0x03150  paused                        -> leave
 *     0x03157  ds:0x50, the walk in progress -> leave (a nuke holds it)
 *     0x03161  ds:0x63, nothing under it     -> leave, the press NOT spent
 *     0x03168  button up                     -> ds:0x61 = 0, leave
 *     0x03170  ds:0x61 already claimed       -> leave
 *     0x03177  ds:0x61 = 0xFF, si = ds:0x58, dispatch on ds:0x82
 *
 * What it acts on is what the LAST scan stored, not a scan of its own. So a
 * press over a lemming that is not yet there passes through the first call
 * and the second takes it, against the scan made after the lemmings moved -
 * which is how the original gave the umbrella to a faller on Fun 10 that the
 * port, acting once at the top of the frame against the previous frame's
 * scan, missed: the lemming fell into the pointer's box that frame.
 *
 * The port used to act on the press EDGE with a fresh scan of its own, once.
 */
void level_play_click(struct level_frame *A)
{
    if (level_paused() == 0xFF) return;          /* image 0x03150 */
    if (g_walking == 0xFF) return;               /* image 0x03157 */
    if (lem_under_count == 0) return;            /* image 0x03161 */
    if (!(lem_buttons & 1)) {                    /* image 0x0316B */
        g_play_latch = 0;                        /* image 0x031BE */
        return;
    }
    if (g_play_latch == 0xFF) return;            /* image 0x03170 */
    g_play_latch = 0xFF;                         /* image 0x03177 */
    /* Image 0x03180..0x031B8 dispatches ds:0x82 to its handler, and each
     * opens by testing its own count - see level_skill_left. Below 2 is no
     * skill at all and image 0x03185 leaves, the press claimed. */
    if (level_skill() < 2 || !level_skill_left(level_skill())) return;
    int16_t si = level_scan_chosen();            /* image 0x0317C: ds:0x58 */
    if (si < 0 || si >= LEMMING_MAX) return;     /* the port's: nothing yet */
    int16_t pl = level_scan_plain();             /* ds:0x5A, when ds:0x5F */
    level_assign(&A->lem[si], pl >= 0 ? &A->lem[pl] : NULL, level_skill());
}

/* IMAGE 0x03F5C - A SKILL ARMED, from a function key at image 0x03EA5 or a
 * click at image 0x03F57. The divider refuses it while the panel is still
 * drawing the last change, and the noise is the selection's alone: a click
 * on the rate, pause or the nuke never reaches here, and the port used to
 * play it for all of them. */
static void skill_store(uint8_t button)
{
    g_press_latch = 0xFF;                           /* image 0x03FE9 */
    if (g_skill_div != 0) return;                   /* image 0x03F5C */
    level_skill_select(button);                     /* image 0x03F66 */
    if (g_snd_driver == 0xFF) {
        if (sound_enabled(3))                       /* image 0x03F70 */
            sound_request(0x61, 4, 0x01);           /* image 0x03F7A */
    } else if (sound_enabled(1)) {                  /* image 0x03F83 */
        sound_request(0x60, 3, 0x13);               /* image 0x03F8D */
    }
}

/* IMAGE 0x03E6F - THE KEYS AND THE PANEL, ONCE A TIMER TICK.
 *
 * Image 0x01874 calls it from image 0x017C7, the INT 8 handler, on EVERY
 * tick - five to a level frame - and that is what this routine's shape is
 * for. There is no edge anywhere in it. A held button comes here five times
 * a frame, and each target decides for itself how often it acts:
 *
 *     the release rate   no latch at all - image 0x03F27 jumps past it - so
 *                        it steps on every tick until ds:0x69, which only
 *                        the panel's redraw turns, says stop
 *     skills 2..9        the latch: image 0x03F38 claims it, and nothing
 *                        more happens until image 0x03FE3 clears it on the
 *                        button's release
 *     pause, the nuke    the latch too, and pause keeps its own two-frame
 *                        stamp (image 0x04021)
 *     the minimap        no latch: the view follows a held pointer
 *
 * The keys come off ds:0x1FD0, which image 0x00194 clears once a frame, so a
 * key reaches every tick between its press and the next frame:
 *
 *     0x3B  F1              image 0x04068   the release rate, down
 *     0x3C  F2              image 0x0403D   the release rate, up
 *     0x3D..0x44  F3..F10   image 0x03F5C   the eight skills, buttons 2..9
 *     0x4E  keypad +        image 0x04018   pause
 *     0x57  F11             image 0x04018   pause
 *     0x53  Del             image 0x03FEF   the nuke
 *     0x58  F12             image 0x03FEF   the nuke
 *
 * NOTE WHERE THE KEYS ENTER. F1 and F2 land past image 0x04032's pause
 * test, and F3..F10 at image 0x03F5C, past image 0x03F52's and past the
 * latch - so the keys arm a skill and move the rate while the game is
 * paused, where a click on the same button does not.
 *
 * `scroll_x` is ds:0x00, which the minimap writes. */
#define TICK_RATE_KEY_DOWN 0x3B   /* image 0x03E72 */
#define TICK_RATE_KEY_UP   0x3C   /* image 0x03E79 */
#define TICK_SKILL_KEY_LO  0x3D   /* image 0x03EA0 */
#define TICK_SKILL_KEY_HI  0x44   /* image 0x03EE6 */

void level_controls_tick(int32_t *scroll_x)
{
    uint8_t al = key_last();                        /* image 0x03E6F */
    switch (al) {
    case TICK_RATE_KEY_DOWN: rate_step(0); return;  /* image 0x03E76 -> 0x04068 */
    case TICK_RATE_KEY_UP:   rate_step(1); return;  /* image 0x03E7D -> 0x0403D */
    case 0x4E: case 0x57:                           /* image 0x03E80, 0x03E87 */
        level_pause_click();                        /* image 0x04018 */
        return;
    case 0x58: case 0x53:                           /* image 0x03E8E, 0x03E95 */
        level_nuke_click();                         /* image 0x03FEF */
        return;
    default: break;
    }
    if (al >= TICK_SKILL_KEY_LO && al <= TICK_SKILL_KEY_HI) {
        skill_store((uint8_t)(2 + al - TICK_SKILL_KEY_LO));  /* image 0x03E9C */
        return;
    }

    if (!(lem_buttons & 1)) {                       /* image 0x03EF1 */
        g_press_latch = 0;                          /* image 0x03FE3 */
        return;
    }
    level_hit h = level_pointer_hit();              /* image 0x03EF9 */
    if (h.what == LEVEL_HIT_PLAY) {                 /* image 0x03F04 */
        g_press_latch = 0xFF;                       /* image 0x03FE9 */
        return;
    }
    if (h.what == LEVEL_HIT_MINIMAP) {
        /* IMAGE 0x03F92. Two tests sit before the store and neither can
         * refuse here. Image 0x03F97 leaves while ds:0x1FCF is set - the
         * ISR's "a tick has happened", which the frame's wait clears - and
         * every tick in this model falls in that wait. Image 0x03FB9 leaves
         * when the pointer is inside the viewport's box AND left of 0xD0,
         * and this branch is only reached at 0xD0 or beyond. */
        g_press_latch = 0xFF;                       /* image 0x03F92 */
        *scroll_x = h.scroll;                       /* image 0x03FD2, 0x03FDB */
        level_pages_redraw_all();                   /* image 0x03FDE -> 0x0410F */
        return;
    }
    if (g_skill_div != 0) {                         /* image 0x03F15 */
        g_press_latch = 0xFF;
        return;
    }
    if (h.button < 2) {                             /* image 0x03F27 */
        level_rate_click(h.button);                 /* image 0x04032 */
        return;
    }
    if (g_press_latch == 0xFF) return;              /* image 0x03F2E */
    g_press_latch = 0xFF;                           /* image 0x03F38 */
    if (h.button == 10) {                           /* image 0x03F3D */
        level_pause_click();                        /* image 0x04018 */
        return;
    }
    if (h.button == 11) {                           /* image 0x03F44 */
        level_nuke_click();                         /* image 0x03FEF */
        return;
    }
    if (h.button == 12) return;                     /* image 0x03FEE */
    if (level_paused() == 0xFF) return;             /* image 0x03F52 */
    skill_store(h.button);                          /* image 0x03F5C */
}

/* IMAGE 0x00164 - THE WAIT, AND THE FIVE TICKS THAT FALL IN IT.
 *
 * Image 0x00186 waits for ds:0x1FCD to pass four, and the ISR counts it at
 * image 0x018A3 right after calling image 0x03E6F. So a frame has five
 * ticks, and each runs image 0x03E6F once - but WHERE in the frame they fall
 * is the machine's speed, not the program's. The frame's own work starts at
 * image 0x0018F as soon as the fifth arrives; on a slow machine the next
 * ticks land in the middle of that work, on a fast one they all land in the
 * wait after it.
 *
 * It is visible. The panel's redraw at image 0x03B86 is what turns the
 * release rate's divider, so a one-frame click on the plus steps once for
 * every tick BEFORE that redraw: measured in the emulator, whose wall-clock
 * timer puts the redraw anywhere from the second tick to the fifth, the
 * same click made +2, +3, +4 or +5 depending on how busy the frame was.
 *
 * The port is the fast machine: all five ticks after the frame's work, here.
 * tools/record_level.py and tools/lockstep.py hold the original's timer back
 * while its frame works, so both games run this same model and stay
 * comparable frame for frame. THE PORT'S OWN, in that the original has no
 * routine that runs five ticks - it has a wait, and an interrupt that keeps
 * arriving; this is the order a fast machine gives them. */
#define LEVEL_TICKS_A_FRAME 5     /* image 0x00189: `cmp al, 4 / jle` */

void level_wait_ticks(int32_t *scroll_x)
{
    harness_script_keys();      /* a recorded key, where the harness put it */
    for (uint8_t t = 0; t < LEVEL_TICKS_A_FRAME; t++)
        level_controls_tick(scroll_x);              /* image 0x01874 */
}

/* IMAGE 0x04660 - A SCREEN THAT WAITS, and the briefing is the one that does.
 *
 *     0x04660  ax = ds:0x14 / test ax,7 / jne 0x04660   wait for RELEASE, so
 *                              the click that opened the screen is not read
 *                              again as the one that leaves it
 *     0x04668  call 0x046B9                             a retrace
 *     0x04680  call 0x046B9                             and one a pass
 *     0x04683  ax = ds:0x14 / test ax,7 / jne 0x046A2   a button: leave
 *     0x0468B  ax = ds:0x1FC7
 *     0x0468E  test ax,0x20 / jne 0x0469B               that key: the MENU
 *     0x04693  test ax,0x40 / je 0x04680                otherwise keep waiting
 *
 * This was view_interact_anim in sdl_io.c - a generic "show a picture until
 * something happens", which is a shape the original does not have. What it
 * does have is this loop, and the fade running inside it: image 0x0031E's
 * cues are armed by the fade at image 0x003C1 and the picture keeps being
 * drawn while the palette moves, which is what a fade IS. `tick` is that
 * fade, and it is game.c's - palette_fade_step carries image 0x0534B.
 *
 * The pump, the page and the pacing are sdl_io.c's, for the reason ds:0x14
 * always is: it was a byte a driver kept current.
 */
int32_t menu_screen_wait(view_tick tick, void *ctx, long budget)
{
    /* IMAGE 0x04660's own first line: wait for every button to be UP, so the
     * click that opened this screen is not read again as the one that leaves
     * it. */
    while (mouse_buttons() & 7) { }              /* image 0x04660 */

    for (;;) {
        /* Image 0x04683 tests ds:0x14's low three bits and jumps without once
         * looking at where the pointer was; image 0x0468E and image 0x04693
         * test ds:0x1FC7's Escape and Enter. All three are polled state, not
         * events - which is why the port needed one handler before it could
         * have one loop. */
        if (mouse_buttons() & 7) return BRIEF_PLAY;      /* image 0x04683 */
        if (key_held(KEY_ESCAPE))  return BRIEF_MENU;    /* image 0x0468E */
        if (key_held(KEY_ENTER))   return BRIEF_PLAY;    /* image 0x04693 */

        if (tick) tick(ctx, NULL);               /* the fade, image 0x0031E */
        video_page_flip();                       /* image 0x04C20 */
        view_screen_pace();                      /* image 0x04680's retrace */
        if (budget && --budget == 0) return VIEW_CLOSED;
    }
}

/* A NOTCH EVERY THIRD PASS, not every pass. Image 0x055B7 waits three retraces
 * between notches - the port stepped once a retrace and faded three times too
 * fast on every screen that fades. menu_screen_wait runs at one pass per
 * retrace, so the counter here is what makes the rate the original's rather
 * than the display's.
 *
 * Non-zero re-uploads the frame, which is what makes a notch visible; once
 * nothing moves the screen is simply held. */
int menu_fade_tick(void *ctx, uint8_t *pix)
{
    static uint8_t wait;
    (void)ctx; (void)pix;
    if (wait) { wait--; return 0; }
    wait = (uint8_t)(palette_fade_retraces() - 1);   /* image 0x055EE */
    return menu_fade_notch();                        /* image 0x05329 */
}

/* ======================================================================
 * IMAGE 0x016D4 - THE KEYBOARD HANDLER, AND THE THREE THINGS IT KEEPS.
 *
 * The port had one of them, and passed a scancode straight from the host's
 * event to whatever wanted it. The original does what games of the era
 * generally do: an ISR takes the byte off port 0x60 and leaves STATE behind,
 * and every reader reads that state rather than an event.
 *
 *   ds:0x0F78[0x53]   ONE BYTE PER SCANCODE. Image 0x01780 indexes it by the
 *                     low seven bits, refuses anything from 0x53 up, and
 *                     writes 0x94 while the key is down and 0 when it comes
 *                     up (image 0x01790's `and cl,0x80 / xor cl,0x80` is the
 *                     release bit inverted, and image 0x0179B ors in 0x14).
 *
 *   ds:0x1FC7         A SIXTEEN-BIT HELD MASK over the keys at ds:0x201F -
 *                     KEY_WATCH in data.c. Image 0x0176E sets the bit on the
 *                     way down and image 0x01776 clears it on the way up.
 *                     This is what image 0x002AB tests to end a level and
 *                     image 0x0468E and image 0x04693 test to leave a
 *                     briefing, and it is a HELD state, not an edge.
 *
 *   ds:0x1FD0         THE LAST SCANCODE PRESSED, and it is cleared by ANY
 *                     release (image 0x01736) as well as by the reader. Image
 *                     0x03E6F takes the function keys off this one.
 *
 * The distinction between the second and the third is not decoration. A held
 * mask answers "is Escape down now", which is what a loop polling once a frame
 * needs; a last-scancode byte answers "what was just pressed", which is what a
 * one-shot like a skill assignment needs. The port drove both from one event
 * and could not have expressed either faithfully.
 *
 * `code` is the byte as it comes off port 0x60: bit 7 set is a release.
 * ====================================================================== */

void keyboard_isr(uint8_t code)
{
    uint8_t sc = (uint8_t)(code & 0x7F);
    uint8_t up = (uint8_t)(code & 0x80);

    if (!up) g_key_last = sc;              /* image 0x01707 */
    else     g_key_last = 0;               /* image 0x01736 */

    /* image 0x016F8's walk: entry i is bit i, and the loop ends when the
     * shift carries out rather than on a terminator. */
    for (uint8_t i = 0; i < 16; i++) {
        uint16_t bit = (uint16_t)(1u << i);
        if (KEY_WATCH[i] == 0xFF) continue;      /* image 0x0175A's hole */
        if (KEY_WATCH[i] != sc) continue;
        if (!up) g_key_held = (uint16_t)(g_key_held | bit);   /* image 0x0176E */
        else {
            g_key_held = (uint16_t)(g_key_held & ~bit);       /* image 0x01776 */
            g_key_last = 0;                                   /* image 0x0177A */
        }
        break;                                                /* image 0x01772 */
    }

    /* image 0x01780's per-key array. */
    if (sc < 0x53) g_key_down[sc] = up ? 0 : 0x94;
}

/* THE READERS, and each one runs the handler first.
 *
 * That is what an interrupt does: by the time the game looks at ds:0x1FC7 the
 * handler has already run, at whatever moment the key arrived. A port that
 * instead drains the host's queue once a frame reads state that is up to a
 * frame stale, and the staleness is invisible - it looks like a key that
 * needed pressing twice. view_input_drain is the one queue, so the mouse is
 * brought up to date on the same pass whoever asked. */
uint8_t key_last(void)          { view_input_drain(); return g_key_last; }
void    key_last_clear(void)    { g_key_last = 0; }
int     key_held(uint16_t mask) { view_input_drain();
                                  return (g_key_held & mask) != 0; }
int     key_is_down(uint8_t sc) { view_input_drain();
                                  return sc < 0x53 && g_key_down[sc] != 0; }

/* IMAGE 0x04D1A - THE MENU'S KEY, AND IT IS A SCAN OF ds:0x0F78 WITH THE
 * GAME'S OWN AUTO-REPEAT IN IT.
 *
 *     0x04D20  cx = 0x53                     every scancode
 *     0x04D23  test [bx+si], 0x80            this one is down
 *     0x04D2B  al = 0xFF                     none of them is
 *     0x04D30  al = [bx+si] & 0x7F, minus 1  the COUNTDOWN in the low seven
 *     0x04D34  je 0x04D42                    it ran out: reload with 0x86
 *     0x04D3C  cmp al, 0x93 / je             the FIRST decrement: report
 *     0x04D40  jmp 0x04D28                   otherwise keep scanning
 *     0x04D47  xlatb through ds:0x202E       the scancode as a CHARACTER
 *
 * So a key reports the instant it goes down (0x94 - 1 is 0x93), then not
 * again until twenty polls have passed, then every six. That is an initial
 * delay and a repeat rate in the game's own units, and it is why holding F3
 * on the original cycles at a pace rather than as fast as the loop runs.
 *
 * The port had the first half of this as a QUEUE in sdl_io.c - a press per
 * pass, repeating at SDL's rate rather than the game's - and said so. The
 * table it needs is ds:0x0F78, which the port did not keep until image
 * 0x016D4 was transcribed; with the array there this is a transcription
 * rather than a substitution.
 *
 * It returns the CHARACTER, because image 0x04D45 and image 0x04D47 are the
 * same routine's tail. menu_key_code is that xlat on its own, for the one
 * caller that already has a scancode.
 */
uint8_t key_scan(void)
{
    view_input_drain();          /* the handler has already run - see above */
    for (uint8_t bx = 0; bx < 0x53; bx++) {          /* image 0x04D20 */
        uint8_t al;
        if (!(g_key_down[bx] & 0x80)) continue;      /* image 0x04D23 */
        al = (uint8_t)((g_key_down[bx] & 0x7F) - 1); /* image 0x04D30 */
        if (al == 0) {                               /* image 0x04D34 */
            g_key_down[bx] = 0x86;                   /* image 0x04D42 */
        } else {
            al = (uint8_t)(al | 0x80);               /* image 0x04D38 */
            g_key_down[bx] = al;
            if (al != 0x93) continue;                /* image 0x04D3C */
        }
        return MENU_XLAT[bx];                        /* image 0x04D47 */
    }
    return 0xFF;                                     /* image 0x04D2B */
}

/* IMAGE 0x04085 - THE LEVEL'S INPUT, called at image 0x001A0 from the frame
 * loop. It is one routine and it does three things:
 *
 *     0x04088  ds:0x1FD0 == 0x2C -> 0x040B5    Z: the skill selection down
 *     0x0408F  ds:0x1FD0 == 0x2D -> 0x040CD    X: and up
 *     0x04096  otherwise the EDGE SCROLL       level_scroll_step, below
 *
 * Z and X are transcribed at level_skill_key and deliberately not reached -
 * image 0x00194 clears ds:0x1FD0 three instructions before image 0x001A0 gets
 * here, so the original cannot see them either, which is what a player found
 * in DOSBox.
 *
 * The function keys are image 0x03E6F's, reached from the same routine that
 * handles a panel click. Escape is image 0x002AB's, which does NOT leave the
 * level: it sets ds:0x4F and the loop ends the way it would have on its own.
 *
 * This was level_input_step in sdl_io.c, with the decisions and the event
 * queue in one function. What is left there is view_level_pump, which brings
 * ds:0x10, ds:0x12, ds:0x14 and ds:0x1FD0 up to date and decides nothing -
 * the relationship the mouse driver and the game actually had.
 *
 * The return is the port's own: under a dummy driver the pointer reads as
 * present and somewhere definite when it is neither, so a scroll that has not
 * been asked for must not start.
 */
int level_input_step(struct level_frame *A, int32_t *scroll_x_p, int32_t max_x)
{
    int32_t scroll_x = *scroll_x_p;
    static int seen_motion;

    /* Image 0x00194: the frame takes the pending key. The five ticks of the
     * wait before it were the key's whole chance - level_wait_ticks. */
    key_last_clear();                               /* image 0x00194 */

    /* The recorded run speaks first - nothing in the original. */
    if (harness_script_step(A, &scroll_x)) seen_motion = 1;

    /* NO PANEL HERE. A held rate button, a skill, pause, the nuke and the
     * minimap are all image 0x03E6F's, which the timer runs five times a
     * frame - level_wait_ticks. This used to step the rate on the press and
     * again from a repeat of its own, two steps on one frame. */

    level_input in = view_level_pump();
    if (in.moved) seen_motion = 1;

    /* ESCAPE ENDS THE LEVEL; IT DOES NOT LEAVE IT. Image 0x002AB tests bit
     * 0x20 of the key word ds:0x1FC7 and jumps to image 0x002B3, which sets
     * ds:0x4F to 0xFF - the same byte image 0x001C8 tests when everyone is out
     * or the clock has run down. So the play loop ends exactly as it would
     * have on its own, image 0x002FB fades, and image 0x00311 goes to the
     * results screen with whatever was rescued.
     *
     * The port took Escape as "close the level", so a player got the menu and
     * no result at all. Bit 0x20 is the same key the results screen tests at
     * image 0x047C1: one key, two screens, both sending it on.
     *
     * AND IT IS THE HELD MASK, not the last scancode. Bit 5 of ds:0x1FC7 is
     * Escape - see KEY_WATCH - and image 0x002AB asks once a pass whether it
     * is down, which is a different question from "was it just pressed" and
     * the reason the handler keeps both. */
    if (key_held(KEY_ESCAPE)) level_end_begin();       /* image 0x002AB */

    /* THE PORT'S OWN, all of it. The original has no way to abandon a level
     * without a result and no keyboard scroll - it has an edge to push the
     * pointer against, which level_scroll_step transcribes. */
    switch (in.port_key) {
    case PORT_KEY_ABANDON: *scroll_x_p = scroll_x; return -1;
    case PORT_KEY_LEFT:  scroll_x -= LEVEL_SCROLL * 2; break;
    case PORT_KEY_RIGHT: scroll_x += LEVEL_SCROLL * 2; break;
    case PORT_KEY_HOME:  scroll_x = 0;                 break;
    case PORT_KEY_END:   scroll_x = max_x;             break;
    default: break;
    }

    *scroll_x_p = scroll_x;
    return seen_motion;
}

/* IMAGE 0x00455's TAIL AND IMAGE 0x0454C - THE LEVEL SET RUNNING.
 *
 * level_show above transcribes image 0x00455 down to image 0x0046B, where the
 * header and the terrain are built. This is the rest of it and the jump out
 * of it, and every line has an address:
 *
 *     0x00462  call 0x00FB2    int 10h ax=0x8D - MODE 0x0D, the window here
 *     0x00465  call 0x052F4    the level palette, branching on ds:0x1F7C
 *     0x00468  call 0x015AD    the PIT divisor into ds:0x1FC9 - a frame's
 *                              length, which is what paces the loop
 *     0x0372D  ds:0x00 = the header's start_x, LESS SIXTEEN
 *     0x040E8  the scroll ceiling, 0x500, which is the GAME's and not the
 *                              canvas's
 *     0x0454C  jmp 0x000FB     the play loop, level_loop below
 *
 * IT WAS level_run IN sdl_io.c, written as a window routine that happened to
 * call the game's loop in the middle - so half of image 0x00455 was
 * transcribed in game.c and the other half implemented next door, which is
 * exactly the split this project's own rule says not to make. What is left in
 * sdl_io.c is five primitives, one per line above, doing what the card and the
 * mouse driver did and deciding nothing.
 *
 * The scale, the title and the frame budget are the port's own: the original
 * owns the machine and takes no arguments about it.
 */
int level_run(struct level_frame *A, const canvas *c,
              const lem_rgb *palette, const char *title, uint8_t scale)
{
    if (scale < 1) scale = 1;
    /* image 0x00462's mode set. */
    if (view_open(LEVEL_SCREEN_W, LEVEL_SCREEN_H, title, scale) != 0)
        return -1;

    /* THE ORIGINAL'S OWN CEILING, not the canvas width: image 0x040E8 stops at
     * 0x500 and this port's view_x is sixteen less. Deriving it from `c->w`
     * was right only while the canvas was the displayed width. */
    int32_t max_x = LEVEL_SCROLL_MAX;
    if (max_x > (int32_t)c->w - LEVEL_SCREEN_W)
        max_x = (int32_t)c->w - LEVEL_SCREEN_W;
    if (max_x < 0) max_x = 0;
    /* IMAGE 0x0372D. The level header's start_x IS the viewport's left edge,
     * not its centre. tools/compare_run.py fits 624 for "Just dig!" and the
     * header says 624; centring on it, which is what the old 320x160 view did,
     * put the play area 160 pixels out and scored 36%. */
    int32_t scroll_x = (int32_t)A->lv.start_x;

    uint8_t *screen = calloc(LEVEL_SCREEN_W * LEVEL_SCREEN_H, 1);
    if (!screen) return -1;
    /* image 0x04C20's page for the level: 320x200, and the palette CHANGES
     * at LEVEL_PLAY_H - the play area keeps the terrain's browns and the
     * panel below gets its own. */
    video_screen_set(screen, LEVEL_SCREEN_W, LEVEL_SCREEN_H,
                     g_lvl_pal_top, g_lvl_pal_bottom, LEVEL_PLAY_H);

    /* IMAGE 0x00465 - TWO PALETTES, and which one a row gets depends on where
     * it is. The game swaps attributes 8..15 part-way down the screen, so the
     * play area keeps the terrain's browns and the panel below gets greens and
     * greys. The lower eight never change.
     *
     * THE SPLIT IS A MACHINE CAPABILITY. A High Performance PC can reprogram
     * the DAC part-way down the frame; a PC compatible cannot - image 0x01200
     * sets ds:0x200C, which is used to turn the screen OFF across a palette
     * write - so the whole frame must live on ONE palette, and that palette has
     * to serve the terrain AND show the skills UI in green.
     *
     * NOT TRANSCRIBED: that single compat palette. Leaving compat on the
     * level's own sixteen is what a player saw as orange status text and orange
     * skill buttons - the terrain's colours running through the panel because
     * nothing replaced them. The gate below is right and the missing half is
     * the palette it should select. tools/probe_dac.py --machine 1 is the
     * instrument: capture what the original uploads in compat and read the
     * sixteen off it, the way PANEL_PAL6 was measured for high.
     *
     * Two earlier readings of this were wrong and are worth not repeating.
     * "--pc selects whether the swap happens" was right about the gate and
     * silent about the palette; "it is the adapter, so always swap" mistook
     * image 0x055B7's ds:0x1F7C - which picks the palette FORMAT, EGA
     * attributes against VGA triples - for the thing that decides the split. */
    lem_rgb top[16], bottom[16];
    for (uint8_t i = 0; i < 16; i++) top[i] = bottom[i] = palette[i];
    if (lem_machine == LEM_PC_HIGH)
        level_panel_palette(&bottom[8]);

    /* NOTHING IN THE ORIGINAL: the frame sink, the state file and the recorded
     * input, all env-gated, all harness.c's. A run being measured is not being
     * displayed, which is the ds:0x200E the palettes are told about. */
    harness_level_begin();
    level_palette_set(top, bottom, !harness_sink_active());

    view_level_grab(1);
    /* IMAGE 0x00468's divisor. A frame is FIVE ticks of the game's own timer -
     * image 0x00186 waits for ds:0x1FCD to exceed 4 - and a tick is a vertical
     * retrace, because image 0x017FE waits for one before reloading the PIT.
     * So the level runs at TIMER_HZ/5, fourteen frames a second on a 70 Hz
     * adapter.
     *
     * It used to pace at 17, on the reading that ds:0x85's 0x11 was the frame
     * rate. It is not: that counter is how many frames the game counts before
     * stepping its DISPLAYED clock, so a level's five minutes takes rather more
     * than five real ones - which is what DOSBox does and what this did not.
     * Before that it slept 16 ms a frame, which was sixty. */
    view_level_period(LEVEL_FRAME_HZ);

    {
        const char *cap = getenv("LEM_FRAMES");   /* the port's own budget */
        level_loop(A, c, screen, &scroll_x, max_x,
                   cap ? (long)atoi(cap) : 0);    /* image 0x0454C */
    }

    /* No fade here. It is image 0x0031E's, it runs INSIDE the loop above, and
     * it was written out a second time in sdl_io.c for as long as the loop
     * exited the moment the level ended - which meant the picture being faded
     * was one nothing was still drawing. See level_end_begin. */
    video_screen_set(NULL, 0, 0, NULL, NULL, 0);
    view_level_grab(0);          /* never leave a going window holding it */
    harness_level_end();
    free(screen);
    return 0;
}

/* IMAGE 0x00105 AND IMAGE 0x00164 - the composed page shown, and the wait
 * before the next one.
 *
 *     0x00105  ds:0x1F76 <-> ds:0x1F78     the two pages exchanged, and the
 *                                          ISR hands the CRTC the new one
 *     0x00164  ds:0x1FCD counted to 5      five of the timer's ticks, which
 *                                          is what makes a level 17 a second
 *
 * The sink between them is the harness's and has no counterpart: the original
 * writes no files. It goes here rather than inside the present because what
 * is being captured is the frame the GAME decided to show. */
void level_frame_show(const uint8_t *screen, struct level_frame *A)
{
    /* IMAGE 0x0018F CLEARS ds:0x1FCD, every frame, and the port never did.
     *
     * The original's loop counts the timer's ticks into it and image 0x0018B
     * waits for five before starting a frame; this port paces on an absolute
     * deadline instead, so timer_frame_due - which IS image 0x0018F's clear -
     * ended up with no callers at all and the byte ran away to 0x20 while the
     * original's sat between 0 and 5.
     *
     * Substituting the WAIT is right: a host deadline is what a retrace-driven
     * count has to become. Leaving the byte behind was not, and it took a
     * comparison against the original's own segment to notice - nothing that
     * looks at pixels ever could. */
    (void)timer_frame_due();                     /* image 0x0018F */

    /* THE SINK CAPTURES BEFORE THE FLIP, so it meets the guest's hook at
     * image 0x00105 - the one instruction both sides share. It used to be
     * after, which put the guest's end-of-frame tests in the gap between the
     * two captures, and those include image 0x00213's `dec ds:0x85`. */
    harness_sink_frame(screen, g_lvl_pal_top, A);
    level_pages_swap();                              /* image 0x00105 */
    video_page_flip();                               /* image 0x00142 */
    /* A run with a sink is being measured, not played, and must not spend
     * wall clock waiting for a display that is not there. */
    if (!harness_sink_active()) view_level_pace();   /* image 0x00164 */
}

/* THE FREES DOS USED TO DO - and they go with the routines that ALLOCATE,
 * which is why they moved here.
 *
 * A free is the counterpart of INT 21h AH=49h and by this port's convention
 * that puts it with the hardware. But dat_load and menu_build_rating are in
 * this file, and a free next door has to know their structures field by field:
 * add one to menu_screen here and the free that does not learn about it leaks
 * silently. That is the same two-spellings-of-one-object hazard this port has
 * already been bitten by twice - the ctypes Lem and the two static g_ticks -
 * and pairing them is what stops it. canvas_init and canvas_free were already
 * together, in sdl_io.c, and stay there for the same reason.
 */
/* THE PORT'S OWN - the original frees through its DOS allocator, and each
 * section is one of this port's mallocs. */
void dat_free(dat_file *d)
{
    if (!d) return;
    for (uint32_t i = 0; i < d->count; i++)
        free(d->sections[i].data);
    free(d->sections);
    d->sections = NULL;
    d->count = 0;
}

/* THE PORT'S OWN. The menu screen's three allocations, and the flip pointer
 * that may be aiming at one of them - video_screen_set is sdl_io.c's. */
void menu_free(menu_screen *m)
{
    if (!m) return;
    /* Never leave the page flip pointing at a buffer that has gone. */
    if (video_screen_is(m->pix)) video_screen_set(NULL, 0, 0, NULL, NULL, 0);
    free(m->pix);
    m->pix = NULL;
    free(m->gfx);
    m->gfx = NULL;
    m->gfx_len = 0;
    free(m->menu_sec);
    m->menu_sec = NULL;
    m->menu_sec_len = 0;
}

/* ======================================================================
 * THE PALETTE SLOTS, ds:0x100 AND ds:0x200 - THE GAME'S OWN MEMORY.
 *
 * These were in sdl_io.c on the reading that a palette is the card's. It is
 * not: the original keeps its slots in DGROUP and image 0x0553D PUSHES one at
 * the DAC, and only that push was ever the hardware's. What sat next door with
 * them were two transcribed routines - image 0x052F4's load and image
 * 0x05329's notch - and a routine lives where the binary puts it.
 *
 * `_cur` is what the screen shows at this moment and `_tgt` is where a fade is
 * heading. The level carries TWO, swapped part-way down the screen, so the
 * play area keeps the terrain's browns and the panel below gets greens and
 * greys; a menu screen carries two only when it is the briefing, which is two
 * pictures stacked.
 *
 * sdl_io.c reads them when it draws and writes none of them.
 * ====================================================================== */
lem_rgb g_lvl_pal_top[16], g_lvl_pal_bottom[16];
static lem_rgb g_lvl_pal_tgt_top[16], g_lvl_pal_tgt_bottom[16];

static lem_rgb g_menu_pal_cur[16], g_menu_pal_tgt[16];
static lem_rgb g_menu_pal_low_cur[16], g_menu_pal_low_tgt[16];
static uint16_t g_menu_split;          /* the row the second slot starts at */

/* IMAGE 0x052F4 - THE LEVEL'S PALETTE LOADED, both slots.
 *
 * With a display - ds:0x200E, which view_level_live has just answered - the
 * pair the screen is drawn through starts at BLACK and image 0x0031E's fade
 * climbs out of it. Without one there is nothing to fade, so it starts AT the
 * target and image 0x0033D's single step clears the bit on the first frame. */
void level_palette_set(const lem_rgb *top, const lem_rgb *bottom, int live)
{
    /* ds:0x200E IS SET HERE AND NOWHERE ELSE, deliberately. It was a separate
     * line in level_run, and when the palette slots moved to game.c that line
     * was dropped: the current pair was blacked ready for the fade, nothing
     * ever answered "there is a display", and the level came up BLACK with the
     * sound playing. One call cannot be half-made. */
    view_level_live(live);                       /* ds:0x200E */
    memcpy(g_lvl_pal_tgt_top, top, sizeof g_lvl_pal_tgt_top);
    memcpy(g_lvl_pal_tgt_bottom, bottom, sizeof g_lvl_pal_tgt_bottom);
    /* Asked rather than trusted: view_level_live answers 0 with no window
     * whatever it was passed, and level_fade_notch asks the same byte. */
    if (video_present()) {
        memset(g_lvl_pal_top, 0, sizeof g_lvl_pal_top);
        memset(g_lvl_pal_bottom, 0, sizeof g_lvl_pal_bottom);
    } else {
        memcpy(g_lvl_pal_top, g_lvl_pal_tgt_top, sizeof g_lvl_pal_top);
        memcpy(g_lvl_pal_bottom, g_lvl_pal_tgt_bottom,
               sizeof g_lvl_pal_bottom);
    }
}

/* IMAGE 0x0534B AND IMAGE 0x05329 - ONE NOTCH of the level's pair. `out`
 * selects the direction: towards the loaded slot, or towards black.
 *
 * Nothing on a display is nothing to fade, so the fade is already over. Image
 * 0x0033D's single step reaches the same place a frame at a time; this reaches
 * it on the first one, which is what a harness with a frame budget needs. */
int level_fade_notch(uint8_t out)
{
    lem_rgb black[16];
    const lem_rgb *tt = g_lvl_pal_tgt_top, *tb = g_lvl_pal_tgt_bottom;
    int moved;
    if (!video_present()) return 0;                      /* ds:0x200E */
    if (out) {
        memset(black, 0, sizeof black);
        tt = tb = black;
    }
    moved  = palette_fade_step(g_lvl_pal_top, tt, 16);   /* image 0x056E2 */
    moved |= palette_fade_step(g_lvl_pal_bottom, tb, 16);
    return moved;
}

/* Image 0x045F2's set-up: both slots aimed at the screen's palettes and the
 * current pair blacked, so the first notch starts from nothing.
 *
 * IT PUTS THE PAGE UP TOO, and that is deliberate rather than convenient. The
 * page has to be pointed at the FADING pair, not at the screen's own - point
 * it at the target and every notch draws the finished picture, so the fade
 * runs and nothing moves on screen. Arming and pointing were two calls for one
 * pass of this file and menu_screen_show already had the first without the
 * second: it would have shown whatever the previous screen left. A call cannot
 * be half-made where two lines can - the same lesson as ds:0x200E. */
void menu_fade_arm(const uint8_t *pix, uint16_t w, uint16_t h,
                   const lem_rgb *pal, const lem_rgb *low, uint16_t split)
{
    g_menu_split = split;
    for (uint8_t i = 0; i < 16; i++) {
        g_menu_pal_tgt[i] = pal[i];
        g_menu_pal_low_tgt[i] = low[i];
        g_menu_pal_cur[i].r = g_menu_pal_cur[i].g = g_menu_pal_cur[i].b = 0;
        g_menu_pal_low_cur[i].r = g_menu_pal_low_cur[i].g =
            g_menu_pal_low_cur[i].b = 0;
    }
    /* image 0x04C20's page, aimed at the pair a notch moves. */
    video_screen_set(pix, w, h, g_menu_pal_cur, g_menu_pal_low_cur, split);
}

/* Image 0x046AB's: both slots aimed at black. */
void menu_fade_to_black(void)
{
    for (uint8_t i = 0; i < 16; i++) {
        g_menu_pal_tgt[i].r = g_menu_pal_tgt[i].g = g_menu_pal_tgt[i].b = 0;
        g_menu_pal_low_tgt[i].r = g_menu_pal_low_tgt[i].g =
            g_menu_pal_low_tgt[i].b = 0;
    }
}

/* THE PORT'S OWN: the original leaves the split where it is and the next
 * screen's image 0x052F4 overwrites it. Here a stale split would send rows of
 * the NEXT screen through a palette nothing is filling. */
void menu_fade_disarm(void) { g_menu_split = 0; }

/* IMAGE 0x05329 - ONE NOTCH OF BOTH SLOTS, which is the call image 0x045F8
 * and image 0x046B1 both make. Non-zero while it still has somewhere to go,
 * which is what image 0x045FB's and image 0x046B4's `jne` test. */
int menu_fade_notch(void)
{
    int moved = palette_fade_step(g_menu_pal_cur, g_menu_pal_tgt, 16);
    if (g_menu_split)
        moved |= palette_fade_step(g_menu_pal_low_cur, g_menu_pal_low_tgt, 16);
    return moved;
}

/* The upper slot as it stands, for the frame a fade puts up each notch. */
const lem_rgb *menu_fade_palette(void) { return g_menu_pal_cur; }

/* WHICH SLOT A ROW TAKES. The briefing is two pictures stacked and the level
 * swaps attributes 8..15 part-way down, so the rows from the split take the
 * second. Zero means one palette for the whole screen, which is every other
 * screen. sdl_io.c asks this per row while it draws - the decision is the
 * game's, the drawing is the card's. */
const lem_rgb *menu_split_palette(uint16_t y, const lem_rgb *upper)
{
    if (g_menu_split && y >= g_menu_split) return g_menu_pal_low_cur;
    return upper;
}

/* ======================================================================
 * IMAGE 0x045EF - UP FROM BLACK, and image 0x046A2 back down. The two halves
 * of image 0x04566's tail, with image 0x04660's wait between them.
 *
 *     0x045EF  call 0x04DEF           the page cleared
 *     0x045F2  cx = 0x10, dx = 0x10   reloaded every pass - image 0x05329's
 *                                     two slot numbers
 *     0x045F8  call 0x05329           one notch of BOTH slots
 *     0x045FB  jne 0x045F2            while it still moved
 *     0x045FD  al = 1
 *     0x04600  call 0x053FE           show slot 1, all sixteen
 *
 * ONE THING IN THAT ORDER DOES NOT CARRY OVER, and it is worth saying rather
 * than quietly matching. The original clears the page BEFORE it fades, and
 * only draws the screen afterwards at image 0x04603 - so what fades up on the
 * original is an EMPTY page. This port is handed a menu_screen whose pixels
 * are already composed, which is the state image 0x0465D leaves the original
 * in, so the clear at image 0x045EF has no counterpart here: its counterpart
 * happened when the screen was built. The fade is of the composed picture.
 *
 * That is a real difference in what a player sees and nothing here has
 * measured which is right, so it is recorded and not fitted.
 * ====================================================================== */
static void menu_screen_fade_in(void)
{
    for (;;) {                                   /* image 0x045F2 */
        int moved = menu_fade_notch();           /* image 0x045F8: call 0x5329 */
        /* The frame put up again, which is the DAC write the original does
         * not need - see sdl_io.c. */
        video_page_flip();                       /* image 0x04C20 */
        view_input_drain();                      /* the close box only */
        view_screen_pace();
        if (!moved) break;                       /* image 0x045FB */
    }
    /* image 0x04600's `al = 1 / call 0x053FE` - the slot shown whole once the
     * notches are done. Here the last notch has already arrived at it. */
}

/* IMAGE 0x046A2 - AND BACK DOWN. A routine of its own: image 0x0469B calls it
 * and image 0x04689 and image 0x04698 jump to it, which is why both answers
 * fade.
 *
 *     0x046A2  call 0x046B9     a retrace
 *     0x046A5  al = 0
 *     0x046A8  call 0x053CA     show slot 0's UPPER EIGHT
 *     0x046AB  cx = 0x10, dx = 0x10
 *     0x046B1  call 0x05329     one notch of both slots
 *     0x046B4  jne 0x046A2      while it still moved
 *     0x046B6  jmp 0x04DEF      and the page cleared behind it
 *
 * This was view_fade_out in sdl_io.c, written as a window loop with a texture
 * upload in the middle of it - a routine the original HAS, living in the file
 * that is meant to hold only what the card did.
 *
 * The original fades the screen away before it draws the next one; cutting
 * straight to the level is the thing that looks wrong even when every pixel
 * is right.
 */
static void menu_screen_fade_out(void)
{
    menu_fade_to_black();                        /* image 0x046AB's targets */
    for (;;) {
        video_retrace_wait();                    /* image 0x046A2: call 0x46B9 */
        int moved = menu_fade_notch();           /* image 0x046B1: call 0x5329 */
        video_page_flip();                       /* image 0x04C20 */
        view_input_drain();
        if (!moved) break;                       /* image 0x046B4 */
    }
    /* image 0x046B6's `jmp 0x04DEF` - the page cleared behind the fade. The
     * port's counterpart is the screen being torn down by the caller. */
}

/* IMAGE 0x04401 - THE MENU, ENTERED. The routine the code screen, the input
 * chooser, the results screen and a briefing's Escape all jump back to, and
 * the port had four of its lines.
 *
 *     0x04401  ds:0x1F76 = 0, ds:0x1F78 = 0x6D60, ds:0x1F7A = 0
 *                                  the page pair - one page here, so nothing
 *     0x04413  call 0x04C32        the CRTC start address
 *     0x04416  ds:0x1F6F = 0xFF    the RATING plate owes a draw
 *     0x0441B  ds:0x1F72 = 0xFF    and so does the FX plate
 *     0x04420  ds:0x1F70 = 0
 *     0x04425  call 0x0531E        the palette to BLACK
 *     0x04428  call 0x04DEF        the page cleared
 *     0x0442B..0x04447             the screen composed - menu_build_rating,
 *                                  which carries this address for that half
 *     0x0444A  call 0x01550        a retrace
 *     0x0444D  call 0x04C32        the start address again
 *     0x04450  call 0x0536C        THE PALETTE UP, sixteen from ds:0x200
 *     0x04453  ds:0x14 & 7         wait for every button to be RELEASED
 *     0x0445B  the loop
 *
 * It was menu_run in sdl_io.c and it was four of those lines: view_open, the
 * buffer pointer, image 0x04453's wait and image 0x0445B's call. So one
 * routine was transcribed in two files that did not name each other -
 * menu_build_rating had the composition and this had the frame around it.
 *
 * WHAT MOVING IT ADDS is the FADE. Image 0x04425 takes the palette to black
 * before anything is drawn and image 0x04450 brings it up, so the menu ARRIVES
 * the way every other screen does; the port cut straight to it. That is the
 * same gap this file records elsewhere: the original does not cut, and a port
 * that does is missing something the original spends real time on.
 *
 * The page pair and the two calls to image 0x04C32 have no counterpart here -
 * one page, and the flip is image 0x04C20's in video_page_flip.
 */
menu_action menu_run(menu_screen *m, uint8_t scale)
{
    const char *cap;

    if (scale < 1) scale = 1;
    if (view_open(MENU_W, MENU_H, "PC Lemmings", scale) != 0) return MENU_EXIT;
    menu_plates_arm();          /* image 0x04416, 0x0441B, 0x04420 */

    /* image 0x04425's black, then image 0x04450's climb out of it. What image
     * 0x04428's clear stood in front of has already happened: the caller
     * composed the screen, which is image 0x0442B..0x04447. */
    menu_fade_arm(m->pix, MENU_W, MENU_H, m->palette, m->palette, 0);
    menu_screen_fade_in();                       /* image 0x04450 */
    menu_fade_disarm();

    menu_wait_release();                         /* image 0x04453 */

    /* THE PORT'S OWN: -1 is unbounded, and the cap goes INTO the loop rather
     * than into a copy of it here. A loop that only ends on a keypress cannot
     * be driven by a test. */
    cap = getenv("LEM_FRAMES");
    return menu_loop(m, cap ? (long)atoi(cap) : -1);   /* image 0x0445B */
}

/* IMAGE 0x04566's TAIL - A SCREEN SHOWN, WAITED ON, AND FADED AWAY.
 *
 * The three stages are the original's own and each is above:
 *
 *     0x045EF  the fade up            menu_screen_fade_in
 *     0x04660  the wait               menu_screen_wait
 *     0x046A2  the fade down          menu_screen_fade_out, on BOTH answers
 *
 * The head of image 0x04566 - image 0x04566..0x045ED, which derives the level
 * number from ds:0x1F6B and ds:0x1F6E, works the percentage out of ds:0x0FD0
 * and ds:0x0FCE, and copies the graphics set's palette - is the BRIEFING's
 * text and lives with the briefing, not here. What is left is the shape every
 * screen that waits shares, which is why the port's other screens reach it
 * too.
 *
 * It was menu_screen_show in sdl_io.c. The window it opens is the
 * card's and stays there; the sequence is the game's.
 */
int32_t menu_screen_show(const menu_screen *m, const char *title, uint8_t scale)
{
    int32_t how;

    if (scale < 1) scale = 1;
    if (view_open(MENU_W, MENU_H, title, scale) != 0) return VIEW_CLOSED;

    /* The briefing is two pictures stacked with different palettes - see
     * menu_screen.split. Every other screen leaves split at zero. */
    menu_fade_arm(m->pix, MENU_W, MENU_H, m->palette, m->palette_low,
                  m->split);
    menu_screen_fade_in();        /* image 0x045EF */

    {
        const char *cap = getenv("LEM_FRAMES");
        how = menu_screen_wait(menu_fade_tick, NULL,
                               cap ? (long)atoi(cap) : 0);
    }

    menu_screen_fade_out();       /* image 0x046A2 */
    menu_fade_disarm();
    return how;                                  /* image 0x0469F against the
                                                  * fall through past 0x046B6 */
}

/* IMAGE 0x04096 - THE EDGE SCROLL, and the two routines it reaches.
 *
 *     0x04096  ds:0x12 + 8 >= 0xC0 -> ret      only in the PLAY AREA
 *     0x040A1  ds:0x10 == 8      -> 0x0414F    the left edge
 *     0x040AC  ds:0x10 == 0x148  -> 0x040E5    the right edge
 *
 *     0x040E5  cmp ds:0x00, 0x500 / je         the ceiling, and stop
 *     0x040ED  add 8                           ONE STEP IS EIGHT
 *     0x040F0  test ds:0x14, 2                 THE RIGHT BUTTON HELD
 *     0x040F8  cmp 0x500 / je                  ... unless that hits it
 *     0x040FD  add 8 AGAIN                     ... doubles the step
 *     0x04103  ds:0x02 = ds:0x00 >> 3          the byte column
 *
 *     0x0414F  cmp ds:0x00, 0x10 / jle         the floor
 *     0x04157  sub 8, and image 0x04167 subtracts a second on the button
 *
 * THREE THINGS THE PORT HAD WRONG, and all three came of writing this from
 * the outside rather than reading it:
 *
 *   - it scrolled while the pointer was over the PANEL. Image 0x0409F refuses
 *     that outright.
 *   - the right button did nothing. Held, it doubles the step - both edges,
 *     and each doubling is separately stopped at the limit.
 *   - it tested a RANGE where the original tests EQUALITY. That is not a
 *     tidier way of saying the same thing by luck: the mouse driver clamps the
 *     pointer to 8..0x148 and lem_mouse_move does the same, so "exactly at the
 *     limit" IS "pushing against the edge", and the range only agreed because
 *     the clamp made it agree.
 *
 * ds:0x00 is this port's view_x plus sixteen - image 0x0372D - so the floor of
 * 0x10 is view_x 0 and the ceiling of 0x500 is LEVEL_SCROLL_MAX.
 *
 * The button is read from lem_buttons, which IS ds:0x14 - image 0x04269
 * stores INT 33h AX=3's BX there - so image 0x040F0's `test ds:0x14, 2` is
 * the same bit here and needs nothing passed in.
 */
static void level_scroll_step(int32_t *sx, int32_t max_x)
{
    if ((int32_t)lem_mouse_y + 8 >= 0xC0) return;   /* image 0x0409F */

    if (lem_mouse_x == 8) {                         /* image 0x040A4 */
        if (*sx <= 0) return;                       /* image 0x04152 */
        *sx -= LEVEL_SCROLL;                        /* image 0x04157 */
        if ((lem_buttons & 2) && *sx > 0)           /* image 0x0415A */
            *sx -= LEVEL_SCROLL;                    /* image 0x04167 */
        level_pages_redraw_all();                   /* image 0x04179 -> 0x0410F */
    } else if (lem_mouse_x == 0x148) {              /* image 0x040AC */
        if (*sx >= max_x) return;                   /* image 0x040E8 */
        *sx += LEVEL_SCROLL;                        /* image 0x040ED */
        if ((lem_buttons & 2) && *sx < max_x)       /* image 0x040F0 */
            *sx += LEVEL_SCROLL;                    /* image 0x040FD */
        level_pages_redraw_all();                   /* image 0x0410F */
    }
}

/* IMAGE 0x000FB - THE LEVEL'S LOOP.
 *
 * The original has ONE loop and every screen jumps into it; image 0x0454C
 * ends `jmp 0x000FB`. Its head flips the page - image 0x00105 swaps ds:0x1F76
 * with ds:0x1F78 and image 0x00142 hands the CRTC registers 0x0C and 0x0D the
 * new start - and image 0x00164 spins until the retrace ISR's ds:0x1FCD
 * reaches five, which is the seventeen frames a second. Then image 0x0018F
 * runs the frame, and image 0x001C8 asks whether it was the last.
 *
 *     level_input_step    ds:0x10, ds:0x12, ds:0x14 and ds:0x1FD0, which a
 *                         driver kept current and an event queue stands in for
 *     level_frame_compose IMAGE 0x0018F's frame, written out above it
 *     level_frame_show     image 0x00105's flip and image 0x00164's wait
 *     level_end_done      IMAGE 0x001C8 and IMAGE 0x002C6, two different
 *                         tests: the first is "has the level ended" and the
 *                         second "has the fade finished". Only the second
 *                         lets the loop out, and between them it keeps
 *                         composing and drawing - which is what the fade is
 *                         a fade OF.
 *     level_end_finish    IMAGE 0x002CD, once it has fallen out
 *
 * `budget` is the port's: a loop that ends only when the level does cannot be
 * driven by a test. So is the -1 from level_input_step, which is the window
 * being closed - the original has no way to abandon a level without a result.
 */
void level_loop(struct level_frame *A, const canvas *c, uint8_t *screen,
                int32_t *scroll_x, int32_t max_x, long budget)
{
    for (;;) {
        int seen = level_input_step(A, scroll_x, max_x);
        if (seen < 0) break;
        level_frame_compose(A, c, screen, scroll_x, max_x, seen);
        level_frame_show(screen, A);             /* image 0x00105 */
        level_wait_ticks(scroll_x);              /* image 0x00164 */
        if (budget && --budget == 0) break;
        if (level_end_done()) break;             /* image 0x001C8, 0x002C6 */
    }
    level_end_finish();                          /* image 0x002CD */
}

/* IMAGE 0x0018F - ONE FRAME OF A LEVEL, COMPOSED.
 *
 * The loop's own order, written out above level_play_step, ends with a page
 * the CRTC is pointed at. This is that page being built, and it used to be
 * spread through sdl_io.c's window loop between the event pump and the
 * texture upload - so the frame order lived in two places, which is the thing
 * this file warns about in three other notes.
 *
 *     image 0x04085  the EDGE SCROLL. Image 0x040A1 tests ds:0x10 against 8
 *                    and 0x148 for the two edges; the RATE is still the
 *                    port's own and the original's has not been read
 *     image 0x001A7  call 0x80FD, which windows the master onto the page -
 *                    and it is a DIRTY-RECTANGLE blitter, not a copy. It
 *                    scans a 20 x 40 array of flags at ss:[ds:0x26] with
 *                    `repne scasb` for a 1, `repe scasb` for the run's
 *                    length, copies only those tiles from ds:0x4A40 to
 *                    ds:0x1F76 + 0x2C2, and clears all 0x320 flags at image
 *                    0x08153. A tile is 8 x 8: image 0x0814C steps the
 *                    source by 0x680, which is eight of the master's
 *                    0xD0-byte rows, and image 0x08148 the page by 0x160.
 *                    So the original's page is maintained INCREMENTALLY and
 *                    keeps whatever no flag asked it to refresh.
 *     image 0x001AA  call 0x7900, which is NOT that window - it copies 13
 *                    bytes by 19 rows from ds:0xD320 to ds:0x1F76 + 0x2174.
 *                    This line used to name it as the terrain window, which
 *                    is the kind of confident wrong comment that stops the
 *                    next reader going to look.
 *
 *                    THE TWO memcpy LOOPS BELOW ARE THE PORT'S OWN. They
 *                    recompose the whole play area from the master every
 *                    frame, which is a different mechanism reaching the same
 *                    picture almost everywhere - and where it does not, the
 *                    original is showing a tile nothing marked dirty.
 *                    tools/probe_page_history.py measures one: canvas
 *                    (799, 81) goes 14 -> 0 in the master on frame 121, the
 *                    port follows it, and the original's page reads 2 from
 *                    frame 122 to 361.
 *
 *                    AND THE REASON IS READ, not guessed. Image 0x09A15
 *                    marks the tiles a rectangle covers - it takes y in ax,
 *                    biases it by 0x10, and walks ss:[si] setting 1s - and
 *                    the code map says it has ONE caller: image 0x0885B,
 *                    inside the blitter, gated on ds:0x2A != 0xFF. So a blit
 *                    marks what it wrote and image 0x080FD carries those
 *                    tiles to the page.
 *
 *                    The digger's clear at image 0x05EBA does not go through
 *                    the blitter. It is a bare stosb run into the master
 *                    with no call in it, so it marks NOTHING, and the page
 *                    keeps whatever it had until some other blit happens to
 *                    dirty that tile. The stale 2 is that, and it is the
 *                    ORIGINAL's behaviour rather than a fault in it.
 *
 *                    Modelling it means the flag array, image 0x09A15 on
 *                    every blit, image 0x080FD's tile copy, the page pair
 *                    and image 0x07178's unmasked 24 x 16 background save.
 *                    It is OWED, and it is now specified rather than
 *                    half-read.
 *     image 0x001B3  the lemmings stepped and image 0x001BC drawn, both
 *                    inside level_frame_tick
 *     image 0x03AC8  the panel, which image 0x001C2 calls
 *     image 0x03DAF  ds:0x1A cleared and image 0x03DE8 set, EVERY PASS, so
 *                    the cursor's box follows the pointer and not a click
 *     image 0x07218  the cursor itself, drawn last over both halves - the
 *                    original saves the bytes under it at image 0x0719C and
 *                    puts them back, which is the same thing as drawing it
 *                    after everything else into a buffer rebuilt each frame
 *
 * `screen` is the 320x200 page; `c` is the master the play area is windowed
 * from. Scrolling is clamped here because image 0x040E8's ceiling is the
 * game's, not the canvas's.
 */
void level_frame_compose(struct level_frame *A, const canvas *c,
                         uint8_t *screen, int32_t *scroll_x, int32_t max_x,
                         int seen_motion)
{
    int32_t sx = *scroll_x;
    /* image 0x04096's edge scroll. seen_motion is the port's: under a dummy
     * driver the pointer reads as present and somewhere definite when it is
     * neither, so a scroll that has not been asked for must not start. */
    if (seen_motion) level_scroll_step(&sx, max_x);
    if (sx < 0) sx = 0;
    if (sx > max_x) sx = max_x;
    A->view_x = (uint16_t)sx;

    /* One frame of the game, then one of the panel. */
    /* IMAGE 0x0019D..0x001B9's share: the sound cues, then the frame. The
     * indirection this used to go through - a function pointer sdl_io.c held
     * - was the window path reaching back for the composer it had just been
     * handed. It is a direct call now, and there is one frame order. */
    uint8_t *page = level_page_drawn(A);         /* ds:0x1F76 */
    level_play_click(A);                         /* image 0x001AD */
    level_frame_tick(A, page);
    if (A->panel && A->pan_sec) {
        memset(A->panel, 0, (size_t)PANEL_LINE_W * PANEL_ART_H);
        level_panel_build(A->panel, A->pan_sec, A->pan_len, &A->lv,
                          A->from->pix, A->from->w, A->from->h,
                          A->lem, A->released, A->view_x,
                          /* IMAGE 0x03B33 DRAWS FROM ds:0x82, not from
                           * ds:0x83 - so the box follows a click at once and
                           * does not wait for image 0x03B46's divider. The
                           * slot is one on from the skill: PANEL_SEL_X0 is 16
                           * where the numbers start at PANEL_ART_X, 32. */
                          (uint8_t)(level_skill() + 1));
    }
    level_play_click(A);                         /* image 0x001C5 */

    for (uint16_t y = 0; y < LEVEL_PLAY_H; y++)
        memcpy(screen + (size_t)y * LEVEL_SCREEN_W,
               page + (size_t)y * c->w + sx, LEVEL_SCREEN_W);
    for (uint16_t y = 0; y < LEVEL_SCREEN_H - LEVEL_PLAY_H; y++) {
        uint8_t *dst = screen + (size_t)(LEVEL_PLAY_H + y) * LEVEL_SCREEN_W;
        if (A->panel && y < PANEL_ART_H)
            memcpy(dst, A->panel + (size_t)y * PANEL_LINE_W + PANEL_ART_X,
                   LEVEL_SCREEN_W);
        else
            memset(dst, 0, LEVEL_SCREEN_W);
    }

    /* The cursor goes on LAST, over the play area and the panel both -
     * the original saves the bytes underneath it (image 0x0719C) and puts
     * them back before the next frame, which is the same thing as drawing
     * it after everything else into a buffer that is rebuilt each frame. */
    /* Image 0x03DAF clears ds:0x1A and image 0x03DE8 sets it, every
     * pass - so the box follows the pointer rather than a click. */
    /* 0xFF, which is what image 0x03DE8 stores - not a C boolean's 1. Both
     * writers of this byte have to agree with the ORIGINAL and not merely with
     * each other: everything that reads ds:0x1A tests it for non-zero, so a 1
     * draws the same box and the difference is invisible until the byte itself
     * is compared. level_pointer_scan already wrote 0xFF; this did not. */
    lem_cursor_box =
        level_lemming_at(A->lem, A->released, A->view_x) >= 0 ? 0xFF : 0;
    level_cursor_draw(screen, LEVEL_SCREEN_W, LEVEL_SCREEN_H, A->view_x);
    *scroll_x = sx;
}

