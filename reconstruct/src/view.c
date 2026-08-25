/* SPDX-License-Identifier: GPL-2.0-only
 *
 * The window, through SDL3.
 *
 * SDL3 is a hard dependency, not an option. A reconstruction's claim is that
 * it is the same game somewhere the original cannot run, and a display layer
 * tied to one platform quietly takes that back - it makes this a Linux program
 * that happens to contain a reconstruction. There is also no second rendering
 * path here for the same reason: a windowed path beside a file path drifts,
 * and only whichever one the build takes ever gets looked at. Both come from
 * the same composed 8-bit buffer.
 *
 * SDL's job stops at getting that buffer to a window and handing back input.
 * Nothing here scales, filters or blends anything the original decided for
 * itself; `scale` is whole-pixel magnification and nothing more.
 */
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "view.h"

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
