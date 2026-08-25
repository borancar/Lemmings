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
