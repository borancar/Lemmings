/* The Ad-Lib Music/FX driver, transcribed from adlib.dat. See adlib.h for
 * where the addresses in these comments point - they are offsets into that
 * file's decompressed section 0, NOT image offsets into vgalemmi.exe.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adlib.h"
#include "game.h"
#include "opl.h"

/* ------------------------------------------------------------ the blob
 *
 * The driver's code and its music are one 22,125-byte image, and the data
 * half is read straight out of it. These are its offsets. */
#define D_CHANNELS   0x05AC   /* nine structures, 0x14 bytes each */
#define D_VOLUME     0x08A7   /* volume -> total-level, 128 entries */
#define D_FNUM       0x0927   /* f-numbers, SIXTEEN words per semitone */
#define D_BLOCK      0x0AA7   /* 96 notes -> block 0..7 */
#define D_SEMITONE   0x0B07   /* 96 notes -> semitone 0..11 */
#define D_TUNES      0x0B6C   /* the tune table, indexed from 1 */
#define D_BASE       0x0B6E   /* everything relative is relative to this */
#define D_FX_INSTR   0x54E3   /* the effect channels' fixed instrument bank */
#define D_FX_TUNES   0x55E3   /* the effect table, indexed from 1 */

#define CHANNELS 9
#define CH_SIZE  0x14

static uint8_t *g_drv;         /* the decompressed driver */
static uint32_t g_drv_len;

/* THE PORT'S OWN. The original had no such question: DOS had either loaded
 * the driver or refused to start. */
uint8_t adlib_ready(void) { return g_drv != NULL; }

/* Reading the blob. A pointer the data hands us is a 16-bit offset into a
 * 64 KB segment, so anything out of range is corrupt rather than clever -
 * return zero and keep going rather than walking off the allocation. */
/* THE PORT'S OWN. The original addresses its data with a segment register
 * and cannot run off the end of a 64 KB segment; a malloc can. */
static uint8_t d8(uint32_t off)
{
    return (off < g_drv_len) ? g_drv[off] : 0;
}

/* THE PORT'S OWN - see d8. */
static uint16_t d16(uint32_t off)
{
    if (off + 1 >= g_drv_len) return 0;
    return (uint16_t)(g_drv[off] | (g_drv[off + 1] << 8));
}

/* ------------------------------------------------- the driver's own state
 *
 * The channel structures live in the blob at driver +0x05AC and the original
 * works on them in place through `di`. They are mirrored here as a struct
 * because a 20-byte array indexed by hand is how the sixteen-pixel bugs in
 * this port happened; every field carries its original offset so the two can
 * still be argued against each other. */
typedef struct {
    uint8_t  note;        /* +0x00  the note last read from the stream */
    uint8_t  count;       /* +0x01  ticks left on it */
    uint16_t instr;       /* +0x02  -> the 16-byte patch */
    uint8_t  instr_no;    /* +0x04  which patch, 1-based */
    uint8_t  op_mod;      /* +0x05  dl: the modulator's slot offset */
    uint8_t  op_car;      /* +0x06  dh: the carrier's slot offset */
    uint8_t  chan;        /* +0x07  the OPL channel, 0..8 */
    uint16_t keyoff;      /* +0x08  the 0xB0+n write that silences it, packed
                           *        al=register ah=value, so the key bit can
                           *        be added back rather than recomputed */
    uint16_t list;        /* +0x0A  position in the pattern list */
    uint16_t pos;         /* +0x0C  position in the note stream */
    uint8_t  vol_mod;     /* +0x0E  the modulator's volume */
    uint8_t  vol_car;     /* +0x0F  the carrier's, as an offset from it */
    uint8_t  state;       /* +0x10  0 idle, 1 music, 2 effect */
    uint8_t  duration;    /* +0x11  the default set by command 0xE0+ */
    uint8_t  transpose;   /* +0x12  added to every note */
    int8_t   slide;       /* +0x13  0, +1 or -1 per tick */
} channel;

static channel  g_ch[CHANNELS];
static uint8_t  g_req_music;    /* driver +0x00ED */
static uint8_t  g_req_slot5;    /* driver +0x00EE - also "a tune is playing" */
static uint8_t  g_req_pending;  /* driver +0x00EF */
static uint8_t  g_req_effect;   /* driver +0x00F0 */
static uint16_t g_instr_base;   /* driver +0x0B68, the tune's patches */
static uint8_t  g_tempo_reload; /* driver +0x0B6A */
static uint8_t  g_tempo;        /* driver +0x0B6B */
static uint16_t g_music_chans;  /* driver +0x0B6C */
static uint16_t g_tune_word;    /* driver +0x05A8, read and never used here */

/* driver +0x0559. The original writes the register, reads the port twelve
 * times, writes the value and reads it eight more - the YM3812's settling
 * time on an ISA bus. Those reads carry no state; see opl.h. */
static void w(uint16_t packed)
{
    opl_write((uint8_t)(packed & 0xFF), (uint8_t)(packed >> 8));
}

/* THE PORT'S OWN, and only a convenience: the original packs the register
 * and the value into one word because it has AX to do it in. driver +0x0559
 * is the routine both of these stand for. */
static void wrv(uint8_t reg, uint8_t val)
{
    opl_write(reg, val);
}

/* ------------------------------------------------------------ key on
 *
 * driver +0x0349. note + transpose + 4 indexes two 96-entry tables, one
 * giving the block and one the semitone; the semitone indexes the f-number
 * table with a stride of SIXTEEN WORDS, which is why the multiply is by 32.
 *
 * The sign is the interesting part. Semitones 7..11 are stored NEGATIVE -
 * 0xFE05 for semitone 7 - and that is not a f-number, it is a flag: the
 * value has already been halved and belongs one octave up. The original says
 * so by decrementing the block, putting it back if the word was negative,
 * and halving only when the block would have gone below zero. Transcribed as
 * written, in signed 16-bit, because rewriting it "clearly" is how the
 * relation gets lost. */
static void key_on(channel *c)
{
    uint8_t  n   = (uint8_t)(c->note + c->transpose + 4);
    int8_t   blk = (int8_t)d8(D_BLOCK + n);
    uint8_t  sem = d8(D_SEMITONE + n);

    /* driver +0x0361: four `add al,al` in a byte, then one `add ax,ax` */
    uint8_t  lo   = (uint8_t)(sem * 16u);
    uint16_t off  = (uint16_t)(lo * 2u);
    int16_t  fnum = (int16_t)d16(D_FNUM + off);

    blk--;                                   /* driver +0x0371 */
    if (fnum < 0) blk++;                     /* +0x0375: not really negative */
    if (blk < 0) { blk++; fnum = (int16_t)(fnum >> 1); }   /* +0x037B */

    wrv((uint8_t)(0xA0 + c->chan), (uint8_t)(fnum & 0xFF));  /* +0x0384 */

    uint8_t hi = (uint8_t)(((uint16_t)fnum >> 8) & 3);
    hi = (uint8_t)(hi | (uint8_t)((uint8_t)blk << 2));
    c->keyoff = (uint16_t)((0xB0 + c->chan) | (hi << 8));    /* +0x039D */
    wrv((uint8_t)(0xB0 + c->chan), (uint8_t)(hi | 0x20));    /* +0x03A1 */
}

/* driver +0x0339 - the per-tick slide, which falls straight into key_on. */
static void slide_step(channel *c)
{
    if (c->slide == 0) return;               /* +0x033F */
    c->note = (uint8_t)(c->note + (uint8_t)c->slide);
    key_on(c);
}

/* driver +0x03CA - the note's gate.
 *
 * Patch byte +0x0E is when to let go: once the countdown reaches it the
 * voice is keyed off, so a note is shorter than its slot in the stream and
 * the next one has somewhere to start. A 0x82 next in the stream is a tie,
 * and ties are not released. */
static void gate_check(channel *c)
{
    if (d8(c->pos) == 0x82) return;          /* +0x03CA */
    if (d8(c->instr + 0x0E) != c->count) return;  /* +0x03D3 */
    w(c->keyoff);                            /* +0x03DD */
    c->slide = 0;
}

/* driver +0x03AA - stop everything. */
static void stop_all(void)
{
    for (uint16_t i = 0; i < g_music_chans && i < CHANNELS; i++) {
        w(g_ch[i].keyoff);
        g_ch[i].state = 0;
    }
    g_req_music = 0;                          /* +0x03C3 writes a WORD, so */
    g_req_slot5 = 0;                          /* it clears 0xED and 0xEE   */
}

/* driver +0x01F4 - the volume commands, 0xB0..0xBF.
 *
 * The byte after the command is a volume, and it reaches the chip through
 * the table at driver +0x08A7, which runs 0x3F down to 0x00 - total level is
 * ATTENUATION, so a bigger number is quieter. The key-scale bits come from
 * patch byte +0x0C, shifted differently for the two operators. */
static void set_volume(channel *c, uint8_t vol)
{
    uint16_t si = c->instr;
    c->vol_mod = vol;

    uint8_t t = d8(D_VOLUME + (uint8_t)(vol & 0x7F));
    uint8_t k = d8(si + 0x0C);
    uint8_t ksl = (uint8_t)((uint8_t)(k << 2) & 0xC0);
    wrv((uint8_t)(0x40 + c->op_mod), (uint8_t)(t | ksl));

    uint8_t v2 = (uint8_t)((c->vol_car + d8(si + 0x0A)) & 0x7F);
    t = d8(D_VOLUME + v2);
    k = d8(si + 0x0C);
    ksl = (uint8_t)((uint8_t)((k >> 2) | (k << 6)) & 0xC0);   /* ror al,1 x2 */
    wrv((uint8_t)(0x40 + c->op_car), (uint8_t)(t | ksl));
}

/* driver +0x0248 - the instrument commands, 0xC0..0xDF.
 *
 * A patch is sixteen bytes and reaches the chip in the original's own order,
 * which is not the register order: attack/decay, sustain/release, waveform,
 * feedback, then the multiple/EG byte, then the volumes. Channels 4 and up
 * take their patches from a FIXED bank at driver +0x54E3 rather than the
 * tune's, which is what lets an effect keep its own voice while music plays. */
static void set_instrument(channel *c, uint8_t n)
{
    c->instr_no = n;
    uint8_t idx = (uint8_t)(n - 1);
    idx = (uint8_t)(idx * 8u);                       /* three `add al,al` */
    uint16_t byte_off = (uint16_t)(idx * 2u);        /* then `add ax,ax`  */

    uint16_t si = (uint16_t)(g_instr_base + byte_off);
    if (c->chan >= 4)                                 /* di >= 0x5FC */
        si = (uint16_t)(D_FX_INSTR + byte_off);
    c->instr = si;

    uint8_t m = c->op_mod, cr = c->op_car;
    wrv((uint8_t)(0x60 + m),  d8(si + 0));
    wrv((uint8_t)(0x60 + cr), d8(si + 1));
    wrv((uint8_t)(0x80 + m),  d8(si + 2));
    wrv((uint8_t)(0x80 + cr), d8(si + 3));
    wrv((uint8_t)(0xE0 + m),  d8(si + 6));
    wrv((uint8_t)(0xE0 + cr), d8(si + 7));
    wrv((uint8_t)(0xC0 + c->chan), d8(si + 9));
    wrv((uint8_t)(0x20 + m),  d8(si + 4));
    wrv((uint8_t)(0x20 + cr), d8(si + 5));

    c->transpose = d8(si + 8);                  /* +0x02D9 */
    c->vol_mod   = d8(si + 0x0A);
    c->vol_car   = d8(si + 0x0B);

    /* the same two writes set_volume makes, from the patch's own levels */
    uint8_t t = d8(D_VOLUME + (uint8_t)(c->vol_mod & 0x7F));
    uint8_t k = d8(si + 0x0C);
    wrv((uint8_t)(0x40 + m),
        (uint8_t)(t | (uint8_t)((uint8_t)(k << 2) & 0xC0)));

    uint8_t v2 = (uint8_t)((c->vol_car + d8(si + 0x0A)) & 0x7F);
    t = d8(D_VOLUME + v2);
    k = d8(si + 0x0C);
    wrv((uint8_t)(0x40 + cr),
        (uint8_t)(t | (uint8_t)((uint8_t)((k >> 2) | (k << 6)) & 0xC0)));

    c->transpose = d8(si + 8);                  /* +0x032D, again */
}

/* ------------------------------------------------------- the sequencer
 *
 * driver +0x019E. The original is a loop made of jumps back to +0x19E, so
 * the commands that only change state `continue` and the ones that consume a
 * tick `return`. Values below 0x80 are notes; everything above is a command.
 */
static void run_stream(channel *c)
{
    for (;;) {
        uint8_t op = d8(c->pos++);

        if (!(op & 0x80)) {                   /* +0x01A3: a note */
            c->note = op;
            w(c->keyoff);                     /* release what was sounding */
            key_on(c);
            c->pos = c->pos;                  /* +0x01B3 */
            c->count = c->duration;
            return;
        }

        if (op >= 0xE0) {                     /* +0x01D2: note length */
            c->duration = (uint8_t)(op - 0xDF);
            continue;
        }
        if (op >= 0xC0) {                     /* +0x01D8: instrument */
            set_instrument(c, (uint8_t)(op - 0xC0));
            continue;
        }
        if (op >= 0xB0) {                     /* +0x01DE: volume */
            set_volume(c, d8(c->pos++));
            continue;
        }

        switch (op) {                         /* +0x03EA's dispatch table */
        case 0x80: {                          /* -> +0x0415: next pattern */
            uint16_t bx = c->list;
            uint16_t nx = d16(bx);
            bx = (uint16_t)(bx + 2);
            if (nx == 0) {                    /* +0x041D -> +0x03FA: loop */
                bx = (uint16_t)(d16(bx) + D_BASE);
                nx = (uint16_t)(d16(bx) + D_BASE);
                bx = (uint16_t)(bx + 2);
            } else {
                nx = (uint16_t)(nx + D_BASE);
            }
            c->list = bx;
            c->pos  = nx;
            continue;
        }
        case 0x81:                            /* -> +0x01C0: a rest */
            w(c->keyoff);
            c->slide = 0;
            c->count = c->duration;
            return;
        case 0x82:                            /* -> +0x01B3: a tie */
            c->count = c->duration;
            return;
        case 0x83:                            /* -> +0x03AA: end of tune */
            stop_all();
            return;
        case 0x84:                            /* -> +0x0430: transpose */
            c->transpose = d8(c->pos++);
            continue;
        case 0x85:                            /* -> +0x043A: end of part */
            w(c->keyoff);
            c->state = 0;
            return;
        case 0x86:                            /* -> +0x0447: slide up */
            c->slide = 1;
            continue;
        case 0x87:                            /* -> +0x044F: slide down */
            c->slide = -1;
            continue;
        default:
            /* 0x88..0xAF index past the eight-entry table. The original
             * would jump through whatever word followed it; nothing in the
             * shipped music reaches here, and guessing what it "meant" would
             * be inventing behaviour. Stop the part instead, and say so. */
            c->state = 0;
            return;
        }
    }
}

/* driver +0x0177 - one channel's tick.
 *
 * A music channel advances only when the tempo counter has run out; an
 * effect (state 2) advances every tick, which is what makes effects run at a
 * fixed speed whatever the tune's tempo is. */
static void channel_tick(channel *c)
{
    if (c->state == 0) return;                /* +0x0177 */
    if (c->state != 2 && g_tempo != 0) return;/* +0x0182, +0x0189 */
    if (--c->count == 0) {                    /* +0x0190 */
        run_stream(c);
        return;
    }
    slide_step(c);                            /* +0x0196 */
    gate_check(c);                            /* +0x0199 */
}

/* driver +0x0457 - a tune was asked for; start it. */
static void start_music(void)
{
    uint8_t al = g_req_pending;
    if (g_req_slot5 == 0) {                   /* +0x0457 */
        al = g_req_music;
        g_req_pending = al;
    }
    if (g_req_pending == 0) return;           /* +0x0464 */
    al = g_req_pending;
    g_req_music = 0;
    g_req_pending = 0;

    if (al >= 0x16) { stop_all(); return; }   /* +0x0478 */
    g_req_slot5 = al;

    uint32_t si = d16(D_TUNES + (uint32_t)al * 2);   /* +0x0484, absolute */

    for (uint8_t i = 0; i < CHANNELS; i++)    /* +0x0490: all nine idle */
        g_ch[i].state = 0;

    g_tune_word   = d16(si); si += 2;         /* +0x04AB -> driver +0x05A8 */
    g_instr_base  = (uint16_t)(d16(si) + D_BASE); si += 2;
    g_tempo_reload = d8(si); si += 1;
    uint8_t count  = d8(si); si += 1;
    g_music_chans  = count;

    for (uint8_t i = 0; i < count && i < CHANNELS; i++) {   /* +0x04C6 */
        uint16_t bx = (uint16_t)(d16(si) + D_BASE); si += 2;
        uint16_t ax = (uint16_t)(d16(bx) + D_BASE);
        bx = (uint16_t)(bx + 2);
        g_ch[i].list  = bx;
        g_ch[i].pos   = ax;
        g_ch[i].count = 1;                    /* so the next tick reads it */
        g_ch[i].state = 1;
    }

    for (uint8_t i = 0; i < CHANNELS; i++)    /* +0x04F1: silence all nine */
        w(g_ch[i].keyoff);

    wrv(0x01, 0x20);                          /* +0x04FE: waveform select on */
    wrv(0xBD, 0xC0);                          /* +0x0504: AM + vibrato depth */
    wrv(0x08, 0x00);                          /* +0x050A */
    wrv(0x04, 0x21);                          /* +0x0510 */
    g_tempo = 1;                              /* +0x0516 */
}

/* driver +0x051C - an effect was asked for; find it a channel.
 *
 * It searches BACKWARDS from channel 8 for four channels, so effects take
 * 8, 7, 6, 5 in that order and never touch 0..4. If all four are busy the
 * effect is dropped - the original does not queue it, and neither does this. */
static void start_effect(void)
{
    uint8_t id = g_req_effect;
    if (id == 0 || id >= 0x13) return;        /* +0x0520, +0x0524 */

    int8_t found = -1;
    for (int8_t i = 8, n = 0; n < 4; i--, n++) {
        if (g_ch[i].state == 0) { found = i; break; }
    }
    if (found < 0) return;                    /* +0x053C: all busy, dropped */

    channel *c = &g_ch[found];
    g_req_effect = 0;
    c->state = 2;
    c->pos   = d16(D_FX_TUNES + (uint32_t)id * 2);   /* +0x054B, absolute */
    c->slide = 0;
    run_stream(c);                            /* +0x0556 */
}

/* driver +0x0115 - the tick, called from the game's own timer ISR. */
void adlib_tick(void)
{
    if (!g_drv) return;
    start_music();                            /* +0x0121 */
    start_effect();                           /* +0x0124 */
    g_tempo--;                                /* +0x0127 */
    for (uint8_t i = 0; i < CHANNELS; i++)    /* +0x012B.. */
        channel_tick(&g_ch[i]);
    if (g_tempo == 0)                         /* +0x0161 */
        g_tempo = g_tempo_reload;
}

/* driver +0x003A - AH=2. */
void adlib_stop(void) { if (g_drv) stop_all(); }

/* driver +0x0022 - AH=3 stores AL and returns; the tick picks it up. */
void adlib_music(uint8_t id) { g_req_music = id; }

/* driver +0x002A - AH=4. */
void adlib_effect(uint8_t id) { g_req_effect = id; }

/* driver +0x0032 - AH=5, the third slot. */
void adlib_slot5(uint8_t id) { g_req_slot5 = id; }

/* ------------------------------------------------------------- loading
 *
 * THE PORT'S OWN. DOS did this: the game opens adlib.dat, decompresses it
 * into a paragraph-aligned block and points INT 61h at it (image 0x00589 and
 * image 0x0058D). There is no routine in the driver to transcribe, because
 * by the time the driver runs it is already in memory. */
int adlib_load(const char *data_dir, const char **err)
{
    char path[1024];
    dat_file d;

    adlib_free();
    snprintf(path, sizeof path, "%s/adlib.dat", data_dir);
    if (dat_load(path, &d, err) != 0) return -1;
    if (d.count < 1) {
        if (err) *err = "adlib.dat has no sections";
        dat_free(&d);
        return -1;
    }

    g_drv_len = d.sections[0].size;
    g_drv = malloc(g_drv_len);
    if (!g_drv) { dat_free(&d); if (err) *err = "out of memory"; return -1; }
    memcpy(g_drv, d.sections[0].data, g_drv_len);
    dat_free(&d);

    /* The nine channel structures are PRE-INITIALISED in the blob, and the
     * two fields that matter are constants of the chip rather than of the
     * music: the operator slot pair and the channel number. The OPL2's
     * operators are not contiguous - 0,3 / 1,4 / 2,5 / 8,11 / ... - and this
     * reads that mapping out of the driver rather than restating it, so a
     * mistake here is a mistake about the file and not about the datasheet. */
    memset(g_ch, 0, sizeof g_ch);
    for (uint8_t i = 0; i < CHANNELS; i++) {
        uint32_t o = D_CHANNELS + (uint32_t)i * CH_SIZE;
        g_ch[i].op_mod = d8(o + 0x05);
        g_ch[i].op_car = d8(o + 0x06);
        g_ch[i].chan   = d8(o + 0x07);
        g_ch[i].keyoff = d16(o + 0x08);
    }
    g_req_music = g_req_slot5 = g_req_pending = g_req_effect = 0;
    g_tempo = g_tempo_reload = 1;
    g_music_chans = CHANNELS;

    opl_reset();
    return 0;
}

/* THE PORT'S OWN - the free DOS did when the program exited. */
void adlib_free(void)
{
    free(g_drv);
    g_drv = NULL;
    g_drv_len = 0;
}
