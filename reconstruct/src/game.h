/* game.h - PC Lemmings (VGA), reconstructed: shared types and declarations.
 *
 * Reconstructed from vgalemmi.exe (DMA Design / Psygnosis, 1991). The licence
 * of the original is unknown, so nothing here claims one: this file carries a
 * provenance note instead of a licence header, because a licence you do not
 * hold is a claim you cannot support. The project's own tooling is a separate
 * matter and is licensed in the repository.
 *
 * One header for the reconstruction, matching one .c file - see the note at
 * the top of game.c about why the port is not split into modules of its own
 * invention.
 */
#ifndef LEM_GAME_H
#define LEM_GAME_H

#include <stdint.h>
#include <stddef.h>

/* ====================================================================
 * Shared types. stdint only - see CLAUDE.md.
 * ==================================================================== */


/* A level's terrain canvas. The original addresses x from -16 to 1583. */
#define LEVEL_W 1584
#define LEVEL_H 160

/* One 16-colour palette entry, already widened from the file's 6 bits. */
typedef struct {
    uint8_t r, g, b;
} lem_rgb;

/* ====================================================================
 * DAT decompression - the container every Lemmings file uses.
 * ==================================================================== */

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

/* The original's disk check, image 0x0062B - transcribed and deliberately
 * no-op; see game.c. 0 carries on, -1 is the "Disk 1 Not found" exit. */
int disk_check(uint8_t media);

int dat_load(const char *path, dat_file *out, const char **err);
void dat_free(dat_file *d);

/* ====================================================================
 * GROUNDxO.DAT - the per-set bitmap index and palette.
 * ==================================================================== */

#define GROUND_OBJECTS 16
#define GROUND_TERRAIN 64

typedef struct {
    uint8_t  width, height;
    uint16_t image_loc;      /* offset into VGAGRx.DAT section 0 */
    uint16_t mask_loc;
} ground_terrain;

typedef struct {
    uint16_t animation_flags;
    uint8_t  start_frame, end_frame;
    uint8_t  width, height;
    uint16_t frame_data_size;
    uint16_t mask_offset;
    uint16_t frames_base_loc;   /* offset into VGAGRx.DAT section 1 */
    uint8_t  preview_frame;
    /* WHERE the object acts, and how. The rectangle sits immediately before
     * the effect byte in the entry, and the game tests a lemming's position
     * against it - image 0x02F3B is the exit's arm of that test, which sets
     * the whole state word to 0x2000. Without the rectangle the port can
     * draw an exit and never notice a lemming reaching it. */
    uint16_t trigger_left;      /* +0x0E */
    uint16_t trigger_top;       /* +0x10 */
    uint8_t  trigger_width;     /* +0x12 */
    uint8_t  trigger_height;    /* +0x13 */
    uint8_t  trigger_effect;    /* +0x14 */
} ground_object;

/* The effects this port acts on. Image 0x02F1x dispatches them as a chain of
 * `dec al / jne`, and the arm that falls through to image 0x02F3B - the one
 * that sets 0x2000 - is the exit. */
#define TRIGGER_EXIT 1

typedef struct {
    ground_terrain terrain[GROUND_TERRAIN];
    ground_object  objects[GROUND_OBJECTS];
    lem_rgb        palette[16];
    /* The file carries THREE upper-eight palettes at 0x3D8, 0x3F0 and 0x408 -
     * custom, standard and preview. `palette[8..15]` is the custom one, which
     * is what the level itself is drawn in. The PREVIEW one is what the
     * briefing screen uses, and the two differ: for graphics set 0 only in
     * entry 8's red, 0x38 against 0x34. One byte, and it is the difference
     * between the briefing's palette matching the original and not. */
    lem_rgb        preview[8];
} ground;

/* All three live in sdl_io.c - see "READING THE FILES" there. */
uint16_t be16(const uint8_t *p);
uint16_t le16(const uint8_t *p);
int ground_load(const char *path, ground *out, const char **err);

/* ====================================================================
 * LVL - one 2048-byte level.
 * ==================================================================== */

/* Guard deliberately not LEVEL_H: lem.h defines LEVEL_H as the level's pixel
 * height, which would satisfy that guard and skip this header entirely. */


#define LEVEL_MAX_OBJECTS 32
#define LEVEL_MAX_TERRAIN 400
#define LEVEL_MAX_STEEL   32

typedef struct {
    int16_t x, y;
    uint8_t id;
    uint8_t no_overwrite;    /* draw only where nothing is already */
    uint8_t only_on_terrain; /* draw only where terrain already is */
    uint8_t upside_down;
} level_object;

typedef struct {
    int16_t x, y;
    uint8_t id;
    uint8_t no_overwrite;
    uint8_t upside_down;
    uint8_t erase;           /* subtract this shape instead of adding it */
} level_terrain;

typedef struct {
    int16_t x, y;
    uint8_t w, h;            /* already in pixels */
} level_steel;

typedef struct {
    uint16_t release_rate;
    uint16_t num_lemmings;
    uint16_t num_to_rescue;
    uint16_t time_limit;     /* minutes */
    uint16_t skills[8];      /* climber, floater, bomber, blocker,
                                builder, basher, miner, digger */
    uint16_t level_number;   /* the port's own: the briefing prints it, and
                              * the LVL does not carry it */
    uint16_t start_x;
    uint16_t graphics_set;
    uint16_t extended_graphics_set;

    level_object  objects[LEVEL_MAX_OBJECTS];
    uint16_t      num_objects;
    level_terrain terrain[LEVEL_MAX_TERRAIN];
    uint16_t      num_terrain;
    level_steel   steel[LEVEL_MAX_STEEL];
    uint16_t      num_steel;

    char name[33];           /* 32 chars, NUL-terminated here */
} level;

/* Parse one 2048-byte level section. Returns 0 on success. */
int level_parse(const uint8_t *buf, uint32_t len, level *out, const char **err);

/* Copy a sprite row, append a zero byte, and shift it all right by `bits` -
 * the original's own way of drawing at a non-byte-aligned x. Transcribed from
 * image 0x09347; `out` needs len + 1 bytes; returns how many to draw.
 * Checked against the original by tools/verify_shift.py. */
uint8_t shift_row(const uint8_t *src, uint8_t len, uint8_t bits, uint8_t *out);

/* Write `v` as three decimal digits, no terminator - it overwrites the digits
 * in a template such as "level000.dat". Transcribed from image 0x04BC2; the
 * original subtracts 100 until it borrows, so its counter starts one high and
 * it adds 0x2F rather than 0x30. Checked against the original by
 * tools/verify_digits.py, driven over all 256 byte values. */
void digits3(uint8_t v, char *out);

/* Build the eight pre-shifted copies of a sprite, each followed by a row of
 * per-column ORs. Transcribed from image 0x05B11; `out` needs
 * 8 * 3 * cols * (rows + 1) bytes. Checked by tools/verify_preshift.py. */
void preshift_sprite(const uint8_t *src, uint8_t cols, uint8_t rows,
                     uint8_t *out);

/* Where the game's level `number` (1..30) of `rating` (0..3 = Fun, Tricky,
 * Taxing, Mayhem) lives: which levelNNN.dat, and which section inside it.
 * Transcribed from the table at image 0x0EF7E - see game.c. */
int level_locate(uint8_t rating, uint8_t number, char *file, size_t file_len,
                 uint32_t *section);

/* The 15 header fields, flat and in the order the original stores them, for
 * tools/verify_level.py. Transcribed from image 0x036B6. */
int level_header_fields(const uint8_t *buf, uint32_t len,
                        uint16_t *out, uint32_t n_out, const char **err);

/* ====================================================================
 * Composing a level's terrain.
 * ==================================================================== */

typedef struct {
    uint8_t *pix;        /* LEVEL_W * LEVEL_H palette indices; 0 = empty */
    uint16_t w, h;
} canvas;

/* How a terrain pixel combines with what is already on the canvas. The
 * original is not simply "replace": which of these it is, is being settled
 * against its video memory rather than assumed, so they are selectable.
 * See STATUS.md in the development repository. */
typedef enum {
    TERRAIN_REPLACE = 0,   /* the newer pixel wins */
    TERRAIN_OR,            /* colour bits are OR'd together */
    TERRAIN_KEEP,          /* the older pixel wins wherever there is one */
    TERRAIN_NOV_IGNORE,    /* replace, ignoring the no-overwrite flag */
    TERRAIN_NOV_INVERT,    /* no-overwrite means "only where terrain is" */
    TERRAIN_OR_ALL,        /* every piece ORs, no-overwrite included */
    TERRAIN_HW             /* what the original's blitter actually does */
} terrain_mode;

extern terrain_mode g_terrain_mode;

/* Draw the terrain list back to front. The list order is the drawing order in
 * every description of the format; selectable so that can be checked rather
 * than assumed, since it interacts with the no-overwrite flag. */
extern uint8_t g_terrain_reverse;

/* Implemented in sdl_io.c - see "WHAT THE HARDWARE USED TO DO" there. */
uint8_t widen6(uint8_t v);
uint8_t planar(const uint8_t *sec, uint32_t base, uint32_t plane_step,
               uint16_t stride, uint8_t nplanes, uint16_t x, uint16_t y);

int  canvas_init(canvas *c);
void canvas_free(canvas *c);

/* Draw every terrain piece of `lv`, in file order - the order is the
 * drawing order and the "do not overwrite" and "erase" flags depend on it. */
int render_terrain(canvas *c, const level *lv, const ground *g,
                   const uint8_t *terrain_sec, uint32_t terrain_len,
                   const char **err);

/* The level's control panel: its own three-plane font, at the tail of
 * main.dat section 2. See "THE LEVEL'S CONTROL PANEL" in game.c. */
#define PANEL_COLS 44
extern const char PANEL_TEMPLATE[PANEL_COLS + 1];
void panel_char(uint8_t *dst, uint16_t dst_w, uint16_t dst_h,
                const uint8_t *sec, uint32_t sec_len,
                uint8_t ch, uint16_t x, uint16_t y);
/* The panel's artwork - main.dat section 2 from its start, 320 by 40 in four
 * plane-major planes, drawn with its top-left at (0, y0). Transcribed from
 * image 0x057D1, whose set-up at image 0x05914 fixes every one of these:
 * bl = 0x28 bytes a row, cx = 0x28 rows, bh = 4 planes. PANEL_TOP is the
 * screen row it goes at, measured against a recorded frame. */
#define PANEL_FONT_SECTION 2
#define PANEL_ART_W 320
#define PANEL_ART_H 40
#define PANEL_ART_PL 4
#define PANEL_TOP 160
void panel_frame(uint8_t *dst, uint16_t dst_w, uint16_t dst_h,
                 const uint8_t *sec, uint32_t sec_len,
                 uint16_t x0, uint16_t y0);

/* The panel's logical line is 44 cells of eight pixels - 352, not the 320 the
 * screen shows - and the artwork sits in its last forty. So the status line's
 * cell 0 is 32 pixels off the left edge; PANEL_ART_X is where the artwork
 * goes in that line, and the screen is the window from there on. */
#define PANEL_LINE_W  (PANEL_COLS * 8)      /* 352 */
#define PANEL_ART_X   32

/* The numbers on the skill buttons - a four-pixel-wide font of ten digits at
 * the very start of the loaded panel font. Transcribed from image 0x0765F. */
void panel_button_num(uint8_t *dst, uint16_t dst_w, uint16_t dst_h,
                      const uint8_t *sec, uint32_t sec_len,
                      uint8_t value, uint16_t x0, uint16_t y0);
/* ====================================================================
 * THE LEMMINGS - the array at ds:0x88, ds:0x43 records of 0x2D bytes.
 *
 * The fields kept here are the ones that have been read; the record has more.
 * x and y are the ORIGINAL's, not the canvas's: image 0x5DC6 probes the
 * terrain at row `y - 0x10` and byte `x >> 3`, and the port's canvas sits 16
 * pixels along from the original's - the "offset 16" tools/prove.sh prints.
 * Keeping the original's numbers means the transcribed rules read as they do
 * in the disassembly, and the two offsets are applied in one place.
 * ==================================================================== */
#define LEMMING_MAX      16
#define LEMMING_DX       16     /* prove.sh's own offset 16 */
#define LEMMING_DY       16     /* image 0x5DC6: di = [si+2] - 0x10 */
#define LEMMING_DEAD_Y   0xB4   /* image 0x2210 */

typedef struct {
    int16_t  x, y;        /* +0x00, +0x02 */
    int16_t  hot_x, hot_y;/* +0x04, +0x06 */
    uint16_t flags;       /* +0x08 - the state bitmask image 0x1F5E dispatches on */
    uint8_t  fall;        /* +0x23 - the fall counter, +3 a frame */
    uint8_t  alive;       /* +0x26 - 0xFF is dead */
    int8_t   dir;         /* +0x27 */
    uint8_t  anim;        /* +0x28 */
    uint8_t  state;       /* +0x29 - 4 falling, 9 walking */
} lemming;

/* One pass of image 0x1F19 over the array, for the states that have been
 * read. `lvl` is the port's own TERRAIN canvas - the buffer the original
 * probes has no objects in it. */
void lemmings_reset(lemming *lem);
void lemmings_release(lemming *lem, uint8_t n, const level *lv);
/* Not const: a digger removes terrain, which is the buffer the others
 * probe - image 0x05EBA writes into the same planes image 0x05DC6 reads. */
void lemmings_step(lemming *lem, uint8_t n,
                   uint8_t *lvl, uint16_t cw, uint16_t chh);

/* Give this one a digger, image 0x0320D. Non-zero if it took - the original
 * refuses when the lemming is already in a state that forbids it. */
/* WHICH lemming the pointer is over, and how many are under it.
 *
 * Image 0x03CE9 keeps two candidates, not one - a lemming already doing a
 * job (ds:0x60) and a plain one (ds:0x5F) - and image 0x03DB4 picks between
 * them by the mouse buttons. `count` is ds:0x63, the number the panel prints
 * beside the name. */
typedef struct {
    int16_t chosen;      /* ds:0x5E, or -1 */
    int16_t busy;        /* ds:0x60 - one already doing a job */
    int16_t plain;       /* ds:0x5F */
    uint8_t count;       /* ds:0x63 */
} level_scan;

void level_pointer_scan(const lemming *lem, uint8_t n, uint16_t view_x,
                        level_scan *out);
/* The chosen one, or -1 - image 0x03D0A, through the scan above. */
int16_t level_lemming_at(const lemming *lem, uint8_t n, uint16_t view_x);

/* ds:0x14, the mouse buttons. Bit 1 makes image 0x03DB7 prefer the plain
 * lemming to the busy one; nothing sets it in this port yet. */
extern uint8_t lem_buttons;
extern uint8_t lem_under_count;      /* ds:0x63 */

/* What the pointer is over, written into the panel's status line - image
 * 0x03CE9 blanks eleven cells and the ladder at image 0x03DFA fills them. */
void level_status_pointer(const lemming *lem, uint8_t n, uint16_t view_x);

/* Image 0x03180 dispatches the selected skill by BUTTON index: 0 and 1 are
 * the release rate and 2..9 the eight skills, of which 9 - the digger - is
 * the one this port has. */
#define SKILL_DIGGER 9

int lem_dig_start(lemming *a);
uint8_t lemmings_live(const lemming *lem, uint8_t n);

/* One lemming's sprite, from main.dat section 0 - 40 bytes a frame, two
 * planes of ten rows. Transcribed from images 0x0706F, 0x059BB and 0x05B11. */
#define LEM_SPRITE_SECTION 0
void lemming_draw(uint8_t *dst, uint16_t dst_w, uint16_t dst_h,
                  const uint8_t *sec, uint32_t sec_len, const lemming *a);

/* Their dots on the minimap - image 0x06FE2. */
void panel_minimap_dots(uint8_t *dst, uint16_t dst_w, uint16_t dst_h,
                        const lemming *lem, uint8_t n,
                        uint16_t x0, uint16_t y0);

/* The minimap: 104 x 20, a pixel per 16 x 1 span of the level, set when more
 * than eight of those sixteen are terrain. Transcribed from images 0x0796F,
 * 0x07A25 and 0x07A39; where it sits is measured. */
#define MINIMAP_AT_X 241
/* The dots sit ONE pixel left of the map's own origin. Measured: the panel is
 * 12 pixels out with them at MINIMAP_AT_X and exactly 0 out at one less.
 *
 * It is also the loose end recorded against the map itself. The copy that
 * places the map is byte-aligned, so it must start on a multiple of eight -
 * and 241 is not one, while 240 is. So the dots are at the aligned origin and
 * the terrain map's fitted 241 is absorbing a one-pixel shift somewhere in
 * its own decode. Both are exact against the original either way; what is not
 * yet read is why the two differ. */
#define MINIMAP_DOT_X (MINIMAP_AT_X - 1)
/* 19 and not 17: the sampler starts sixteen canvas rows down (image 0x07992)
 * so the picture rides two stash rows higher, and the origin comes down to
 * meet it. Swept: 201, 98, 12, 93, 194 differing pixels at 17..21. */
#define MINIMAP_AT_Y 19
/* The minimap's viewport box - 24 minimap pixels wide at scroll >> 4.
 * Transcribed from image 0x07A5A. */
#define VIEWPORT_AT_Y 18
void panel_viewport(uint8_t *dst, uint16_t dst_w, uint16_t dst_h,
                    uint16_t scroll, uint16_t x0, uint16_t y0);

void panel_minimap(uint8_t *dst, uint16_t dst_w, uint16_t dst_h,
                   const uint8_t *canvas, uint16_t cw, uint16_t chh,
                   uint16_t x0, uint16_t y0);

/* The box round the selected button - 16 wide, 24 tall, colour 3, at
 * 16 + 16*button in the panel's 352-wide line. Transcribed from image
 * 0x0780D; ds:0x83 names the button and image 0x0427 starts it at 3. */
#define PANEL_SEL_BUTTON 3
void panel_highlight(uint8_t *dst, uint16_t dst_w, uint16_t dst_h,
                     uint8_t button);

/* The IN percentage, image 0x03AF8. */
void level_status_in(uint8_t percent);

void panel_buttons(uint8_t *dst, uint16_t dst_w, uint16_t dst_h,
                   const uint8_t *sec, uint32_t sec_len, const level *lv);

/* The status line the panel draws, and the level clock inside it. The time is
 * ASCII in the line itself; there is no binary countdown behind it.
 * Transcribed from image 0x0213, seventeen frames to a second. */
/* Three cells of the status line, left-aligned with leading zeros
 * suppressed. Transcribed from image 0x04B68. */
void    status_put_num(char *dst, uint8_t v);

/* Releasing the lemmings: twenty frames after the entrance opens, then one
 * every ((99 - rate) >> 1) + 4. Transcribed from images 0x30A6 and 0x39D6. */
void    level_release_reset(const level *lv);
void    level_release_step(void);
uint8_t level_released(void);

void        level_status_reset(const level *lv);

/* The skill counts as they are spent - ds:0x6A, three bytes apart, one per
 * button 2..9. The panel draws these and not the level header's, which never
 * change. level_status_reset stocks them from the header. */
void        level_skills_reset(const level *lv);
uint8_t     level_skill_left(uint8_t button);
void        level_skill_spend(uint8_t button);
void        level_timer_step(void);
/* How many got out, and whether the level is over - see level_over in
 * game.c. ds:0x46 counts them and image 0x02335 is what increments it; the
 * two end conditions are at image 0x001E7 and image 0x001FA. */
/* Has this lemming reached an exit, and the state it enters - images
 * 0x02F3B and 0x02F42. The trigger rectangle's units are four pixels,
 * measured rather than assumed; see level_at_exit in game.c. */
void level_triggers_set(const level *lv, const ground *g);
int level_at_exit(const lemming *a, const level *lv, const ground *g);
void lem_exit_start(lemming *a);

/* Win or lose, and which of the nine things the game says about it -
 * image 0x048F6's ladder. */
/* The results screen's text - image 0x048BD. Its COMPOSITION is not read;
 * these are the lines it assembles. */
/* The access code. Image 0x05042 builds one, image 0x051A2 takes one back.
 * The checksum is transcribed (0x05147 and 0x051D4 agree on it) and so is
 * the rotation; the bit table for characters 0..6 is MEASURED by driving the
 * original - see the note in game.c. `out` needs CODE_LEN + 1 bytes. */
char level_code_checksum(const char *first_nine);
int level_code_ok(const char *code);
void level_code_generate(uint8_t level_no, uint8_t other, uint8_t percent,
                         char *out);

const char *level_results_headline(int timed_out);
const char *level_results_prompt(uint8_t input_kind);
void level_results_line(char *line, int needed_line, uint8_t percent);

uint8_t level_verdict(uint8_t saved, uint8_t needed);
const char *level_verdict_text(uint8_t saved, uint8_t needed);
int level_won(uint8_t saved, uint8_t needed);
/* What winning does - image 0x0472E: on to the next level, and the rating
 * recomputed from the index. Non-zero while there is another. */
int level_advance(uint8_t *index, uint8_t *rating);

extern uint8_t level_saved;
void level_saved_reset(void);
void level_lemming_saved(void);
uint8_t level_saved_percent(const level *lv);
int level_over(const level *lv, uint8_t released, uint8_t live);

const char *level_status_line(void);

/* One frame of the control panel, in the order the original draws it - see
 * level_panel_build in game.c. `pp` is PANEL_LINE_W x PANEL_ART_H. */
void level_panel_build(uint8_t *pp, const uint8_t *sec, uint32_t seclen,
                       const level *lv, const uint8_t *terrain,
                       uint16_t tw, uint16_t th,
                       const lemming *lem, uint8_t nlem,
                       uint16_t view_x, uint8_t sel);

void panel_status(uint8_t *dst, uint16_t dst_w, uint16_t dst_h,
                  const uint8_t *sec, uint32_t sec_len,
                  const char *text, uint16_t x, uint16_t y);

/* ====================================================================
 * The level's objects, animated at display time.
 *
 * The original NEVER bakes these into its terrain (tools/prove.sh relies on
 * that: it composes terrain only and matches to the pixel). It keeps a
 * 28-byte state per object at ds:0x17CB, steps them all once a pass at image
 * 0x07C0A, and draws them all at image 0x07BB8 over whatever the terrain
 * left. This mirrors that.
 * ==================================================================== */

typedef struct {
    uint16_t flags;       /* state[0]: bit 1 animating, bit 0 one-shot */
    uint8_t  frame;       /* state[2] */
    uint8_t  end_frame;   /* state[3] */
    uint8_t  w;           /* state[4], in pixels */
    uint8_t  rows;        /* state[5] */
    uint16_t frame_size;  /* state[6] */
    uint16_t mask_delta;  /* state[8], added to the ink pointer */
    uint16_t base;        /* state[0x15] */
    uint16_t ink;         /* state[0x17], recomputed by the stepper */
    uint8_t  live;        /* the level list's x was non-zero */
} level_anim;

#define LEVEL_ANIM_MAX 32          /* image 0x07BC0: mov cx, 0x20 */

/* Build the state array the way the original's set-up does, from the level's
 * own object list and the graphics set's table. */
void level_anim_init(level_anim *st, const level *lv, const ground *g);

/* One pass of image 0x07C0A. */
/* The level's opening sequence, image 0x0361: a frame counter, and the byte
 * at ds:0x55 that holds the trapdoor shut - and the lemmings in it - until
 * play-loop frame 35. Call level_start_step once per frame, before stepping
 * the animations. */
void    level_start_reset(void);
void    level_start_step(void);
uint8_t level_entrance_open(void);
uint8_t level_frame_counter(void);

void level_anim_step(level_anim *st, const level *lv);

/* One pass of the level's frame, in the order image 0x019C..0x01C5 runs the
 * parts of it that have been read. */
void level_frame_step(level_anim *st, const level *lv);

/* One frame of a level, split where image 0x019C splits it: everything up to
 * the drawing, then the drawing. Both the game and the harness call these,
 * which is the point - see level_play_step in game.c. */
/* The eight colours the panel's half of the screen uses for attributes 8..15
 * - image 0x0C080. See level_panel_palette in game.c: the game swaps these in
 * part-way down the screen, which is a thing no single-palette capture can
 * show. `out` takes eight entries. */
void level_panel_palette(lem_rgb *out);

/* The mouse cursor - the game's own cross-hair, and where it is. The
 * coordinates are the original's ds:0x10 and ds:0x12, which are the screen's
 * offset by eight; see level_cursor_draw in game.c. */
/* One step of the original's palette fade - image 0x056E2, two notches a
 * component - and non-zero while anything is still moving. */
/* What the pointer is over, image 0x03EF9 - the play area, one of the
 * panel's sixteen-pixel buttons, or the minimap (whose click carries the
 * scroll it asks for). */
typedef enum { LEVEL_HIT_PLAY = 0, LEVEL_HIT_BUTTON, LEVEL_HIT_MINIMAP }
    level_hit_what;
typedef struct {
    level_hit_what what;
    uint8_t        button;   /* 0,1 the rate; 2..9 the skills; 10.. special */
    uint16_t       scroll;   /* LEVEL_HIT_MINIMAP: where it wants the view */
} level_hit;
level_hit level_pointer_hit(void);

int palette_fade_step(lem_rgb *cur, const lem_rgb *tgt, uint8_t n);

extern uint16_t lem_mouse_x, lem_mouse_y;
/* Is the pointer over a lemming - the original's ds:0x1A, which is what
 * makes the cursor a box (image 0x05854). */
extern uint8_t lem_cursor_box;
void lem_mouse_move(int32_t x, int32_t y);
void level_cursor_draw(uint8_t *pix, uint16_t w, uint16_t h, uint16_t view_x);

/* Which PC the port is pretending to be. The original asks at start-up -
 * "1 - For PC compatibles" against "2 - For High Performance PCs" - and only
 * the high-performance build changes the palette part-way down the screen;
 * the other has to make do with one palette for the whole frame. */
typedef enum { LEM_PC_COMPATIBLE = 0, LEM_PC_HIGH = 1 } lem_pc;
extern lem_pc lem_machine;

/* THE DISPLAY'S refresh, which is what the MENU runs at - and it is a
 * different mechanism from the level's rate, not a different number for the
 * same one.
 *
 * Image 0x01550 is a vertical retrace wait: it reads the status register at
 * ds:0x1FDA + 6 and spins for bit 3 to go high and then low again. The menu
 * loop calls it once a pass (image 0x0445B), so the menu advances one step
 * per retrace and its speed is the adapter's, whatever that is. On the VGA
 * this port is of, the 640x350 and 320x200 modes both refresh at 70 Hz -
 * which is the figure tools/lemmings.py hands the emulator, and the figure
 * the game's own measurement at image 0x1602 arrives at.
 *
 * The LEVEL does not run at this. It waits several retraces a frame and
 * lands on LEVEL_FPS below, which is the game's number and not the
 * display's. Reading one rate off the other is how the level came to run
 * three and a half times too fast. */
#define VIEW_REFRESH_HZ 70

/* THE LEVEL'S FRAME RATE, image 0x0418 and image 0x041D: ds:0x85 and ds:0x86
 * both start at 0x11, and the clock steps a second every time the counter
 * runs out - which is what makes 5,100 frames exactly the five minutes of the
 * expiration timer. It is the game's rate, not the display's: the level runs
 * at seventeen frames a second, not seventy. */
#define LEVEL_FPS 0x11

void level_play_step(level_anim *st, const level *lv, lemming *lem,
                     uint8_t *released, uint8_t *terrain,
                     uint16_t w, uint16_t h);
void level_play_draw(level_anim *st, const level *lv,
                     const uint8_t *obj_sec, uint32_t obj_len,
                     const uint8_t *lem_sec, uint32_t lem_len,
                     const lemming *lem, uint8_t released,
                     uint8_t *pix, uint16_t w, uint16_t h);
void level_status_out(uint8_t live);

/* One pass of image 0x07BB8, onto a 320-wide viewport scrolled `scroll`
 * pixels into the level. */
void level_anim_draw(const level_anim *st, const level *lv,
                     const uint8_t *object_sec, uint32_t object_len,
                     uint8_t *dst, uint16_t dst_w, uint16_t dst_h,
                     uint16_t scroll);

/* Draw the level's interactive objects - the entrance, the exit, traps.
 * Terrain must already be composed: an object's "no overwrite" flag is about
 * the terrain underneath it. `frame` selects an animation frame; 0 is the
 * resting pose the level opens on. */
int render_objects(canvas *c, const level *lv, const ground *g,
                   const uint8_t *object_sec, uint32_t object_len,
                   uint8_t frame, const char **err);

/* ====================================================================
 * VGAGRx.DAT - the 4-bit planar bitmaps.
 * ==================================================================== */

/* Decode one planar bitmap into 8-bit indices, one byte per pixel.
 * `out` must have room for w*h bytes. Pixel 0 means "not drawn": terrain
 * only ever uses colours 8-15, so plane 3 doubles as the shape's mask.
 * Returns 0 on success, -1 if the bitmap would read past `len`. */
/* Which bit of the pixel value each STORED plane contributes. The published
 * description says plane i adds 1<<i; this is selectable so that claim can be
 * tested against the original rather than believed. Default {0,1,2,3}. */
extern uint8_t g_plane_bits[4];

/* Which end of a byte the leftmost pixel is. 1 = the high bit, which is what
 * every published description says; selectable because inside a solid area it
 * makes no difference at all, so believing it is cheap and checking it is
 * cheaper than the bug it hides. */
extern uint8_t g_msb_first;

int vgagr_decode(const uint8_t *sec, uint32_t len, uint32_t loc,
                 uint8_t w, uint8_t h, uint8_t *out);

/* Decode a single monochrome plane - an object's transparency mask, which is
 * stored separately because objects use all sixteen colours and so cannot
 * borrow the fourth bitplane as terrain does. 1 means "draw this pixel". */
int vgagr_decode_mask(const uint8_t *sec, uint32_t len, uint32_t loc,
                      uint8_t w, uint8_t h, uint8_t *out);

/* ====================================================================
 * The window and input. Implemented by sdl_io.c, which is OURS.
 * ==================================================================== */

/* An abstract key, so no header outside view.c has to know about SDL. */
typedef enum {
    VIEW_KEY_NONE = 0,
    VIEW_KEY_F1, VIEW_KEY_F2, VIEW_KEY_F3, VIEW_KEY_F4,
    VIEW_KEY_ESC, VIEW_KEY_UP, VIEW_KEY_DOWN,
    /* The rating plate's alternates. The original's scancode table at
     * ds:0x202E maps ` and - (and keypad -) to the same two handlers the
     * arrows reach, so these are its keys, not the port's convenience. */
    VIEW_KEY_BACKQUOTE, VIEW_KEY_MINUS
} view_key;

/* A thing on the screen that can be clicked, or reached with a key. */
typedef struct {
    uint16_t x, y, w, h;   /* the clickable box, in picture pixels */
    view_key key;          /* VIEW_KEY_NONE for click-only */
    int32_t  id;           /* what view_interact returns when it is chosen */
} view_hotspot;

/* Returned when the window is closed rather than a hotspot chosen. */
#define VIEW_CLOSED (-1)

/* Which hotspot a key, or a point in picture coordinates, selects; VIEW_CLOSED
 * for none. Pure, so the mapping can be tested without opening a window. */
int32_t view_match_key(const view_hotspot *spots, uint8_t n, view_key k);
int32_t view_match_point(const view_hotspot *spots, uint8_t n,
                         float x, float y);

/* Show a picture and wait for the viewer to choose one of `spots`, or close
 * the window. Returns the chosen hotspot's id, or VIEW_CLOSED. */
int32_t view_interact(const uint8_t *pix, uint16_t w, uint16_t h,
                      const lem_rgb *palette, const char *title, uint8_t scale,
                      const view_hotspot *spots, uint8_t nspots);

/* Called once per displayed frame to redraw `pix` in place; return non-zero if
 * anything changed, so the texture is only re-uploaded when it has to be. */
typedef int (*view_tick)(void *ctx, uint8_t *pix);

/* The same, but animated. `pix` is written by `tick` rather than being fixed.
 * The menu needs this and nothing else does: its banner scrolls. */
int32_t view_interact_anim(uint8_t *pix, uint16_t w, uint16_t h,
                           const lem_rgb *palette, const char *title,
                           uint8_t scale, const view_hotspot *spots,
                           uint8_t nspots, view_tick tick, void *ctx);

/* Open a window on an 8-bit indexed buffer and let the viewer scroll it.
 * `view_w`/`view_h` is the viewport onto a `src_w`x`src_h` picture; pass the
 * full size for a screen that is not scrolled. Returns 0 when it is closed. */
int view_show(const uint8_t *pix, uint16_t src_w, uint16_t src_h,
              uint16_t view_w, uint16_t view_h,
              const lem_rgb *palette, const char *title,
              uint16_t start_x, uint8_t scale);

/* Load one level out of `file` and show it, and show its briefing first.
 * THE PORT'S OWN, both of them - they exist so main.c can be the game's flow
 * and nothing else. In sdl_io.c. */
int level_show_briefing(const char *dir, const char *file, uint32_t which,
                        uint8_t rating, uint16_t number, uint8_t scale,
                        const char **err);
int level_show(const char *dir, const char *file, uint32_t which,
               level *lv, uint8_t scale, const char **err);

/* The level, in the original's own 320x160 viewport. */
int view_run(const canvas *c, const lem_rgb *palette, const char *title,
             uint16_t start_x, uint8_t scale);

/* The same, with a per-frame tick so the level's objects can animate over the
 * terrain. `tick` is handed the canvas to redraw into and returns non-zero
 * when it changed something. */
int view_run_anim(canvas *c, const lem_rgb *palette, const char *title,
                  uint16_t start_x, uint8_t scale,
                  view_tick tick, void *ctx);

#endif /* LEM_GAME_H */

/* ====================================================================
 * The main menu screen.
 * ==================================================================== */

#define MENU_W 640
#define MENU_H 350

typedef struct {
    uint8_t *pix;            /* MENU_W * MENU_H, values 0..15 */
    lem_rgb  palette[16];
    /* A SECOND palette, for the rows from `split` down, and the row it
     * starts at - zero meaning the screen has only one.
     *
     * The briefing is two pictures stacked: the level's own preview at the
     * top in the level's colours, and the text below in earth browns and
     * blue shades, with a band of black scanlines between them where the
     * game swaps the palette over. tools/probe_dac.py measures both. A
     * capture holds one of them, so a screen can match the original on every
     * index and still be the wrong colour - which is exactly what this
     * screen did. */
    lem_rgb  palette_low[16];
    uint16_t split;
    /* A copy of MAIN.DAT section 4, kept alive so the banner can be redrawn
     * as it scrolls. The bar's tile and the font both live in it, and
     * menu_build composes only one frame. NULL on the screens that do not
     * animate. */
    uint8_t *gfx;
    uint32_t gfx_len;
} menu_screen;

/* What the player chose. The names are the original's own, read off the signs
 * it draws: the game labels them, so this is evidence rather than a guess.
 * What each one DOES beyond PLAY and EXIT is not reconstructed - see menu.c. */
typedef enum {
    MENU_EXIT = 0,      /* ESC   "EXIT TO DOS", or the window closed */
    MENU_PLAY,          /* F1    "1 PLAYER"  */
    MENU_NEW_LEVEL,     /* F2    "NEW LEVEL" */
    MENU_FX,            /* F3    "FX"        */
    /* The two arrow tabs on the rating plate. Up / ` raise the rating and
     * Down / - / keypad-minus lower it; see MENU_RATING_MAX below. */
    MENU_RATING_UP,
    MENU_RATING_DOWN,
    MENU_HELP           /* F4    the "?" plate */
} menu_action;

/* Compose the menu from `dir`/main.dat. Returns 0 on success. */
int  menu_build(const char *dir, menu_screen *m, const char **err);

/* The same, with the rating name on its plate: 0 Fun, 1 Tricky, 2 Taxing,
 * 3 Mayhem. All four are verified against the original: each rating's
 * name is byte-identical to the sprite the game draws there, and inside
 * the plate's box the original changes 713 pixels between Fun and Mayhem
 * while the port changes the same 713. The plate also carries a 70-pixel
 * residual that is IDENTICAL at every rating - part of the long-standing
 * unexplained plate pixels, nothing to do with the rating. See
 * RATING_BASE in game.c. */
int  menu_build_rating(const char *dir, uint8_t rating, menu_screen *m,
                       const char **err);

/* The two sub-screens the menu can reach, both of them the original's own:
 * F2 shows a level-code entry screen and F4 an input-device chooser. Both are
 * the same tiled background and the same font as the menu. */
int menu_code_screen(const char *dir, menu_screen *m, const char **err);
int menu_input_screen(const char *dir, menu_screen *m, const char **err);

/* The level briefing the original shows between the menu and the level.
 * `rating` indexes "Fun   TrickyTaxingMayhem" (image 0x0E691); this demo is
 * all Fun, and where the game keeps the current rating is not established. */
int menu_briefing_screen(const char *dir, const level *lv, uint8_t rating,
                         menu_screen *m, const char **err);

/* The highest rating, and how many levels each one has. Both are the
 * original's own arithmetic: image 0x044F9 caps the rating at 3, and the
 * handlers at 0x04500 and 0x04530 then compute ds:0x1F6B = rating * 30 by
 * four shift-and-adds - the absolute level number, which is why the level
 * order table falls into four blocks of thirty. */
#define MENU_RATING_MAX  3
#define MENU_RATING_STEP 30

/* How many hotspots the menu has: the six plates, three more for the rating
 * plate's alternate keys (one hotspot carries one key), and one full-screen
 * box for "a click anywhere", which is what the original does with the mouse.
 * See menu_hotspots in game.c. */
#define MENU_HOTSPOTS 10

/* Show a screen that has no choices on it - the briefing, the level-code
 * screen, the input chooser - and wait for the viewer to dismiss it. The
 * briefing says "Press mouse button to continue" and means it, so a click
 * dismisses as well as Esc. Returns when it is dismissed. */
void menu_show_until_dismissed(const menu_screen *m, const char *title,
                               uint8_t scale);

/* Fill in the menu's hotspots. Exposed so the mapping can be tested without
 * opening a window - see tests/menu_input.c. */
void menu_hotspots(view_hotspot *spots);

/* The scrolling banner's position, in pixels, and how far a step moves it.
 * The original shifts the strip 4 pixels a step (image 0x07D54) and takes a
 * new character every 16 (image 0x07D29), so a step is MENU_SCROLL_STEP.
 * The port composes one frame, so the position has to be set before
 * menu_build rather than invented inside it - see draw_banner in game.c. */
#define MENU_SCROLL_STEP 4
/* ds:0x21 and the lemmings' frame. The VARIABLES are the game's - the scroll
 * is the original's own ds:0x21 - and the accessors around them are not, so
 * they live in sdl_io.c. */
extern uint32_t menu_scroll;
extern uint8_t  menu_lem_frame;
/* The code screen's caret: which of the ten cells it is on, and whether it
 * is showing this instant. Image 0x04CB8 swaps that cell between '.' and '_'
 * every twenty retraces, so a capture shows one phase of it and the port has
 * to be told which - the same job --scroll does for the banner. */
extern uint8_t menu_caret, menu_caret_cell;
void menu_caret_step(void);

void     menu_set_scroll(uint32_t pixels);
uint32_t menu_get_scroll(void);

/* The five lemmings that walk on and off the menu, image 0x04F96. Each is on
 * screen for 32 passes out of a period of 400-560, so most of the time there
 * is nothing to draw - which is why no capture of the original shows one.
 * menu_anim_reset puts the counters back to image 0x04FF8's staggered start;
 * menu_anim_step is one pass of the routine. */
void menu_anim_reset(void);
void menu_anim_step(void);
/* Ten of them - image 0x04FFC loads cx with 0xA - though only five are live. */
#define MENU_ANIM_N 10
/* Their counters, the original's ds:0x4960. Set them to score a capture: the
 * five walk on and off, so a composed frame that starts them from the
 * staggered reset shows a different moment from whatever the guest was at. */
extern int16_t anim_ctr[MENU_ANIM_N];
extern int     anim_started;
void menu_set_anim(const int16_t *ctr, uint8_t n);
/* How many of them are on screen right now. Exposed so the counter logic can
 * be checked without a window and without a capture - no capture of the
 * original shows one, so pixels cannot settle this. */
uint8_t menu_anim_visible(void);

/* Which frame the two lemmings holding the banner's ends are on. Sixteen of
 * them, and image 0x07E2F takes the index from ds:0x21 - the SAME word the
 * ribbon runs off - as `(c >> 2) & 15`. This used to say "NOT tied to the
 * scroll", which was wrong and had been contradicted by game.c for as long as
 * both comments existed; the port's table is offset two frames from the
 * original's, which is what made them look independent. See LEM_FRAME_BIAS. */
void     menu_set_lem_frame(uint8_t frame);
uint8_t  menu_get_lem_frame(void);

/* One pass of the original's menu loop, image 0x07D0D - the game's half of
 * what runs while the menu is up. `ctx` is the menu_screen; returns non-zero
 * when the picture changed, which is what view_tick wants. */
int menu_frame_step(void *ctx, uint8_t *pix);

/* Show it and wait for the player to choose. Not const: the banner scrolls,
 * so the picture is redrawn in place while this runs. THE PORT'S OWN, and it
 * lives in sdl_io.c: the original has no such loop. It dispatches from image
 * 0x0447B and never returns a value. */
menu_action menu_run(menu_screen *m, uint8_t scale);

void menu_free(menu_screen *m);
