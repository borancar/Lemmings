/* sdl_io.c - the window, input and timing, through SDL3.
 *
 * THIS FILE IS THE PORT'S OWN. It is not a reconstruction of anything in
 * vgalemmi.exe and carries no image offsets, because there are none to carry:
 * the original talked to an EGA card and a BIOS, and this talks to SDL. The
 * boundary between this and game.c is a boundary the PORT chose - what a
 * modern backend has to replace - not one the binary proves.
 *
 * SDL3 is a hard dependency, never a platform-specific library and never an
 * optional path beside a file writer: a reconstruction's claim is that it is
 * the same game somewhere the original cannot run, and a display layer tied to
 * one platform takes that back. Two rendering paths also drift, and only the
 * one a given build takes is ever looked at.
 *
 * SDL's job stops at getting the port's own composed 8-bit indexed buffer to a
 * window and handing back input. Nothing here scales, filters or blends
 * anything the original decided for itself; `scale` is whole-pixel
 * magnification and nothing more.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "game.h"



int view_show(const uint8_t *pix, uint16_t src_w, uint16_t src_h,
              uint16_t view_w, uint16_t view_h,
              const lem_rgb *palette, const char *title,
              uint16_t start_x, uint8_t scale)
{
    if (scale < 1) scale = 1;
    if (view_w > src_w) view_w = src_w;
    if (view_h > src_h) view_h = src_h;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return -1;
    }

    SDL_Window *win = NULL;
    SDL_Renderer *ren = NULL;
    if (!SDL_CreateWindowAndRenderer(title, view_w * scale, view_h * scale,
                                     0, &win, &ren)) {
        SDL_Log("SDL_CreateWindowAndRenderer: %s", SDL_GetError());
        SDL_Quit();
        return -1;
    }
    /* Whole pixels. The original had no filtering and neither has this. */
    SDL_SetRenderLogicalPresentation(ren, view_w, view_h,
                                     SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);

    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888,
                                         SDL_TEXTUREACCESS_STREAMING,
                                         view_w, view_h);
    if (!tex) {
        SDL_Log("SDL_CreateTexture: %s", SDL_GetError());
        SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
        return -1;
    }
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);

    int32_t scroll_x = (int32_t)start_x - view_w / 2;
    int32_t max_x = (int32_t)src_w - view_w;
    int32_t max_y = (int32_t)src_h - view_h;
    int32_t scroll_y = 0;
    if (max_x < 0) max_x = 0;
    if (max_y < 0) max_y = 0;

    int running = 1;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT)
                running = 0;
            else if (e.type == SDL_EVENT_KEY_DOWN) {
                switch (e.key.key) {
                case SDLK_ESCAPE: case SDLK_Q: running = 0; break;
                case SDLK_LEFT:   scroll_x -= 16; break;
                case SDLK_RIGHT:  scroll_x += 16; break;
                case SDLK_UP:     scroll_y -= 16; break;
                case SDLK_DOWN:   scroll_y += 16; break;
                case SDLK_HOME:   scroll_x = 0; break;
                case SDLK_END:    scroll_x = max_x; break;
                default: break;
                }
            }
        }
        if (scroll_x < 0) scroll_x = 0;
        if (scroll_x > max_x) scroll_x = max_x;
        if (scroll_y < 0) scroll_y = 0;
        if (scroll_y > max_y) scroll_y = max_y;

        void *pixels = NULL;
        int pitch = 0;
        if (SDL_LockTexture(tex, NULL, &pixels, &pitch)) {
            for (uint16_t y = 0; y < view_h; y++) {
                uint32_t *row = (uint32_t *)((uint8_t *)pixels + y * pitch);
                const uint8_t *src =
                    pix + (size_t)(scroll_y + y) * src_w + scroll_x;
                for (uint16_t x = 0; x < view_w; x++) {
                    lem_rgb c = palette[src[x]];
                    row[x] = ((uint32_t)c.r << 16) |
                             ((uint32_t)c.g << 8) | c.b;
                }
            }
            SDL_UnlockTexture(tex);
        }
        SDL_RenderClear(ren);
        SDL_RenderTexture(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
        SDL_Delay(16);
    }

    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

static view_key from_sdl(SDL_Keycode k)
{
    switch (k) {
    case SDLK_F1:     return VIEW_KEY_F1;
    case SDLK_F2:     return VIEW_KEY_F2;
    case SDLK_F3:     return VIEW_KEY_F3;
    case SDLK_F4:     return VIEW_KEY_F4;
    case SDLK_ESCAPE: return VIEW_KEY_ESC;
    case SDLK_UP:     return VIEW_KEY_UP;
    case SDLK_DOWN:   return VIEW_KEY_DOWN;
    /* The original takes the rating on ` and on - as well as on the arrows,
     * and its scancode table gives keypad minus the same code as -, so both
     * arrive here as the same key. */
    case SDLK_GRAVE:  return VIEW_KEY_BACKQUOTE;
    case SDLK_MINUS: case SDLK_KP_MINUS: return VIEW_KEY_MINUS;
    default:          return VIEW_KEY_NONE;
    }
}

/* Which hotspot a key or a point selects.
 *
 * These are separate from the event loop on purpose: they are the part that
 * can be WRONG - a key mapped to the wrong plate, a box off by its own width -
 * and they can be checked without a window, a display or SDL's event delivery.
 * The loop below is then only plumbing.
 *
 * Both return VIEW_CLOSED for "nothing here", which is also what a closed
 * window returns; the caller cannot tell those apart and does not need to,
 * because neither is a choice.
 */
int32_t view_match_key(const view_hotspot *spots, uint8_t n, view_key k)
{
    if (k == VIEW_KEY_NONE) return VIEW_CLOSED;
    for (uint8_t i = 0; i < n; i++)
        if (spots[i].key == k) return spots[i].id;
    return VIEW_CLOSED;
}

int32_t view_match_point(const view_hotspot *spots, uint8_t n,
                         float x, float y)
{
    for (uint8_t i = 0; i < n; i++) {
        const view_hotspot *s = &spots[i];
        if (x >= s->x && x < s->x + s->w && y >= s->y && y < s->y + s->h)
            return s->id;
    }
    return VIEW_CLOSED;
}

int32_t view_interact(const uint8_t *pix, uint16_t w, uint16_t h,
                      const lem_rgb *palette, const char *title, uint8_t scale,
                      const view_hotspot *spots, uint8_t nspots)
{
    /* No tick: the picture is fixed, and the cast is safe because nothing
     * writes through it when `tick` is NULL. */
    return view_interact_anim((uint8_t *)pix, w, h, palette, title, scale,
                              spots, nspots, NULL, NULL);
}

/* ONE window, kept across calls.
 *
 * This used to create a window and call SDL_Quit on every call, and menu_run
 * is called once per menu action - so choosing anything at all, including
 * changing the difficulty, tore the window down and built a new one. That is
 * visible: the window blinks, moves back to wherever the WM puts a new one,
 * and loses focus. A menu that reopens itself is not a menu.
 *
 * So the window, renderer and texture live here and are reused whenever the
 * size and scale match. SDL is shut down once, at exit. */
static SDL_Window   *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture  *g_tex;
static uint16_t      g_w, g_h;
static uint8_t       g_scale;

static void view_shutdown(void)
{
    if (g_tex) SDL_DestroyTexture(g_tex);
    if (g_ren) SDL_DestroyRenderer(g_ren);
    if (g_win) SDL_DestroyWindow(g_win);
    g_tex = NULL; g_ren = NULL; g_win = NULL;
    SDL_Quit();
}

/* Returns 0 on success. Reuses what is already open when it fits. */
static int view_open(uint16_t w, uint16_t h, const char *title, uint8_t scale)
{
    static int registered;
    if (g_win && g_w == w && g_h == h && g_scale == scale) {
        SDL_SetWindowTitle(g_win, title);
        return 0;
    }
    if (g_win) {                       /* a different size: rebuild */
        if (g_tex) SDL_DestroyTexture(g_tex);
        if (g_ren) SDL_DestroyRenderer(g_ren);
        SDL_DestroyWindow(g_win);
        g_tex = NULL; g_ren = NULL; g_win = NULL;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return -1;
    }
    if (!registered) { atexit(view_shutdown); registered = 1; }
    if (!SDL_CreateWindowAndRenderer(title, w * scale, h * scale, 0,
                                     &g_win, &g_ren)) {
        SDL_Log("SDL_CreateWindowAndRenderer: %s", SDL_GetError());
        return -1;
    }
    SDL_SetRenderLogicalPresentation(g_ren, w, h,
                                     SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_XRGB8888,
                              SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!g_tex) return -1;
    SDL_SetTextureScaleMode(g_tex, SDL_SCALEMODE_NEAREST);
    g_w = w; g_h = h; g_scale = scale;
    return 0;
}

int32_t view_interact_anim(uint8_t *pix, uint16_t w, uint16_t h,
                           const lem_rgb *palette, const char *title,
                           uint8_t scale, const view_hotspot *spots,
                           uint8_t nspots, view_tick tick, void *ctx)
{
    if (scale < 1) scale = 1;
    if (view_open(w, h, title, scale) != 0) return VIEW_CLOSED;
    SDL_Renderer *ren = g_ren;
    SDL_Texture  *tex = g_tex;

    /* A frame budget, for tests: the animation has to be observable without a
     * person watching it, and a loop that only ends on a keypress is not. */
    const char *cap = SDL_getenv("LEM_FRAMES");
    long budget = cap ? SDL_atoi(cap) : 0;

    /* Uploaded once when nothing animates, and again whenever `tick` says the
     * picture changed. */
    int dirty = 1;

    int32_t chosen = VIEW_CLOSED;
    int running = 1;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            int32_t hit = VIEW_CLOSED;
            if (e.type == SDL_EVENT_QUIT) {
                running = 0;
            } else if (e.type == SDL_EVENT_KEY_DOWN) {
                hit = view_match_key(spots, nspots, from_sdl(e.key.key));
            } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                /* The window may be scaled and letterboxed, so ask SDL where
                 * the click landed in PICTURE coordinates rather than doing
                 * the arithmetic here and getting it wrong at every scale but
                 * the one that was tested. */
                float px = 0, py = 0;
                SDL_RenderCoordinatesFromWindow(ren, e.button.x, e.button.y,
                                                &px, &py);
                hit = view_match_point(spots, nspots, px, py);
            }
            if (hit != VIEW_CLOSED) {
                chosen = hit;
                running = 0;
            }
        }
        if (tick && tick(ctx, pix))
            dirty = 1;
        if (dirty) {
            void *pixels = NULL;
            int pitch = 0;
            if (SDL_LockTexture(tex, NULL, &pixels, &pitch)) {
                for (uint16_t y = 0; y < h; y++) {
                    uint32_t *row = (uint32_t *)((uint8_t *)pixels + y * pitch);
                    const uint8_t *src = pix + (size_t)y * w;
                    for (uint16_t x = 0; x < w; x++) {
                        lem_rgb c = palette[src[x]];
                        row[x] = ((uint32_t)c.r << 16)
                               | ((uint32_t)c.g << 8) | c.b;
                    }
                }
                SDL_UnlockTexture(tex);
            }
            dirty = 0;
        }
        SDL_RenderClear(ren);
        SDL_RenderTexture(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
        SDL_Delay(16);
        if (budget && --budget == 0) running = 0;
    }
    /* The window stays open. It is torn down at exit, not here. */
    return chosen;
}

/* The level viewport: the original shows 320x160 of the 1584-wide level, with
 * the control panel below it. The panel is not reconstructed yet, so this is
 * the same viewport and no panel - a way to look at what has been built, not a
 * reimplementation of the game's display loop. That has to be transcribed and
 * checked against the original like anything else. */
int view_run(const canvas *c, const lem_rgb *palette, const char *title,
             uint16_t start_x, uint8_t scale)
{
    return view_show(c->pix, c->w, c->h, 320, 160, palette, title,
                     start_x, scale);
}

/* ======================================================================
 * WHAT THE HARDWARE USED TO DO.
 *
 * Everything below replaces something the original did NOT have code for,
 * because a card or DOS did it:
 *
 *   widen6   the DAC turns a 6-bit component into 8; the original just
 *            writes six bits and the hardware widens them
 *   planar   the EGA assembles a pixel out of four bit-planes; the original
 *            programs the card and never touches a plane as data
 *   *_free   DOS owned the memory. The original allocates through INT 21h
 *            and never frees anything by hand
 *
 * and two that are not the game at all: the window loop a screen with no
 * choices sits in, and a flat view of the parsed level header that exists
 * only so tools/verify_level.py can compare against the original.
 *
 * These lived in game.c and were labelled "the port's own", which is a
 * contradiction: game.c holds the game. If a routine is marked as ours and
 * sits in the game's file, either it is IO and belongs here, or it is game
 * logic that has not been transcribed yet - and the second is the thing this
 * whole method exists to avoid.
 * ====================================================================== */

#define GROUND_SIZE 1056

/* A 6-bit DAC value as 8 bits, the way the hardware does it: the top two bits
 * repeated into the bottom, not a proportional scale. The two agree at 0 and
 * 63 and differ by one in between - 0x20 is 130 here and 129 if you compute
 * v*255/63 - and that one is enough to make every mid-tone pixel of a screen
 * compare as different against another emulator.
 *
 * Not transcribed as a routine - this is what the DAC hardware does, not
 * code the original runs.
 */
uint8_t widen6(uint8_t v)
{
    v = (uint8_t)(v & 0x3F);
    return (uint8_t)((v << 2) | (v >> 4));
}

/* One pixel out of a planar bitmap. `planes` is how far apart consecutive
 * planes are; pass 0 for the two-plane background and only bits 0 and 1 are
 * read. The leftmost pixel of a byte is its most significant bit.
 *
 * Not transcribed: reading one pixel out of a planar bitmap, an idiom. The
 * original never does this - it programs the EGA and lets the card do it.
 */
uint8_t planar(const uint8_t *sec, uint32_t base, uint32_t plane_step,
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

/* The port's own. */
void menu_free(menu_screen *m)
{
    if (!m) return;
    free(m->pix);
    m->pix = NULL;
    free(m->gfx);
    m->gfx = NULL;
    m->gfx_len = 0;
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

/* A flat view of the parsed header, so tools/verify_level.py can compare
 * against the original without mirroring `level`'s layout in Python.
 * The order is the order the original stores them in.
 *
 * The port's own: a flat view for tools/verify_level.py, with no counterpart
 * in the original.
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

/* vgagr_decode and vgagr_decode_mask are the same class as `planar` above and
 * were moved here with it: the original NEVER unpacks these bitmaps. It hands
 * the planar bytes straight to the EGA and the card assembles the pixels -
 * every blit logged by tools/probe_blits.py points `si` into the loaded
 * section itself. Software has to do that work; the original had no code for
 * it, so there is nothing here to transcribe.
 */
/* NOT TRANSCRIBED - a 4-bit planar bitmap out of VGAGRx.DAT. Where the
 * original decodes these is not established.
 *
 * NOT TRANSCRIBED. The original decodes these bitmaps itself; the routine
 * has not been read. Owed.
 */
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

/* NOT TRANSCRIBED - the 1-bit transparency mask beside a bitmap.
 *
 * NOT TRANSCRIBED. As vgagr_decode above. Owed.
 */
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

/* The menu's loop. THE PORT'S OWN, and here rather than in game.c because the
 * original has no such thing: image 0x0447B reads a key, jumps to whichever
 * screen it names, and never returns a value to anyone. A function that shows
 * a picture, waits, and hands back what was chosen is a shape this port needs
 * and the original does not have.
 *
 * What runs each pass IS the game's, and stays there: menu_frame_step is
 * image 0x07D0D. */
menu_action menu_run(menu_screen *m, uint8_t scale)
{
    view_hotspot spots[MENU_HOTSPOTS];
    menu_hotspots(spots);
    int32_t r = view_interact_anim(m->pix, MENU_W, MENU_H, m->palette,
                                   "PC Lemmings", scale, spots,
                                   MENU_HOTSPOTS, menu_frame_step, m);
    if (r == VIEW_CLOSED) return MENU_EXIT;
    return (menu_action)r;
}

/* ======================================================================
 * SCAFFOLDING THE ORIGINAL HAS NO COUNTERPART FOR.
 *
 * game.c holds the game. Anything in it marked "the port's own" was a
 * contradiction, so these came here: a convenience wrapper, accessors over
 * state the original keeps in ds:0x21 with no function around it, a query for
 * where plate i is that only exists because this port needs clickable boxes,
 * and a count exposed purely so a test can see the walk-on lemmings without a
 * window.
 * ====================================================================== */



/*
 * The port's own accessors. The original keeps this in ds:0x21 and has no
 * function around it.
 */
void menu_set_scroll(uint32_t pixels) { menu_scroll = pixels; }

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

/* ======================================================================
 * READING THE FILES.
 *
 * be16 and le16 are the file formats' byte order, not the game's: the
 * original reads these words inline where it needs them and has no routine
 * to transcribe. ground_load is the port's struct over GROUNDxO.DAT, and the
 * original does not parse that file at all - image 0x01A0B names it, 0x01A18
 * loads all 4,096 bytes raw to ds:0x1B4B, and image 0x037D1 indexes the
 * bytes where they lie with `mul 0x1C` and `lea si,[0x1b4b]`.
 * ====================================================================== */
/*
 * Not transcribed: a big-endian word, an idiom rather than a routine.
 *
 * The port's own: the original reads these words inline, with no routine to
 * point at.
 */
uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

/*
 * Not transcribed: a little-endian word, an idiom rather than a routine.
 *
 * The port's own: as be16 above - an idiom, not a routine.
 */
uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* NOT TRANSCRIBED - GROUNDxO.DAT, from the published description, checked
 * against the files. Where the original parses it is not established.
 *
 * The port's own, and there is nothing to transcribe: THE ORIGINAL NEVER
 * PARSES THIS FILE. It loads all 4,096 bytes raw to ds:0x1B4B (image 0x01A0B
 * names it, 0x01A18 calls the loader at image 0x01916) and then indexes the
 * bytes in place - image 0x037D1 does `mul 0x1C` on the object id and
 * `lea si,[0x1b4b]`, reading the trigger box straight out of [si+0x0E] and
 * [si+0x10]. This struct is a convenience the port chose; the file layout it
 * fills in is the transcription, and that is in the field comments.
 */
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
    const uint8_t *vga_custom  = buf + 0x3D8;
    /* 0x3D8 custom, 0x3F0 standard, 0x408 preview - eight triples each. */
    const uint8_t *vga_preview = buf + 0x408;
    for (uint8_t i = 0; i < 8; i++) {
        g->preview[i].r = widen6(vga_preview[i * 3 + 0]);
        g->preview[i].g = widen6(vga_preview[i * 3 + 1]);
        g->preview[i].b = widen6(vga_preview[i * 3 + 2]);
    }

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
