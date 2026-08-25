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
    uint8_t  trigger_effect;
} ground_object;

typedef struct {
    ground_terrain terrain[GROUND_TERRAIN];
    ground_object  objects[GROUND_OBJECTS];
    lem_rgb        palette[16];
} ground;

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

int  canvas_init(canvas *c);
void canvas_free(canvas *c);

/* Draw every terrain piece of `lv`, in file order - the order is the
 * drawing order and the "do not overwrite" and "erase" flags depend on it. */
int render_terrain(canvas *c, const level *lv, const ground *g,
                   const uint8_t *terrain_sec, uint32_t terrain_len,
                   const char **err);

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

/* The level, in the original's own 320x160 viewport. */
int view_run(const canvas *c, const lem_rgb *palette, const char *title,
             uint16_t start_x, uint8_t scale);

#endif /* LEM_GAME_H */

/* ====================================================================
 * The main menu screen.
 * ==================================================================== */

#define MENU_W 640
#define MENU_H 350

typedef struct {
    uint8_t *pix;            /* MENU_W * MENU_H, values 0..15 */
    lem_rgb  palette[16];
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
void     menu_set_scroll(uint32_t pixels);
uint32_t menu_get_scroll(void);

/* The five lemmings that walk on and off the menu, image 0x04F96. Each is on
 * screen for 32 passes out of a period of 400-560, so most of the time there
 * is nothing to draw - which is why no capture of the original shows one.
 * menu_anim_reset puts the counters back to image 0x04FF8's staggered start;
 * menu_anim_step is one pass of the routine. */
void menu_anim_reset(void);
void menu_anim_step(void);
/* How many of them are on screen right now. Exposed so the counter logic can
 * be checked without a window and without a capture - no capture of the
 * original shows one, so pixels cannot settle this. */
uint8_t menu_anim_visible(void);

/* Which frame the two lemmings holding the banner's ends are on. Sixteen of
 * them, and NOT tied to the scroll - see draw_end_lemming in game.c. */
void     menu_set_lem_frame(uint8_t frame);
uint8_t  menu_get_lem_frame(void);

/* Show it and wait for the player to choose. Not const: the banner scrolls,
 * so the picture is redrawn in place while this runs. */
menu_action menu_run(menu_screen *m, uint8_t scale);

void menu_free(menu_screen *m);
