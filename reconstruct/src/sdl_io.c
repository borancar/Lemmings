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
#include "adlib.h"
#include "opl.h"



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

/* A screen whose palette changes part-way down it - the briefing, and the
 * level, which has its own loop. The original swaps the palette across a band
 * of black scanlines, so the rows from `g_split` down take `g_split_pal`.
 * Zero means one palette for the whole screen, which is every other screen. */
static uint16_t      g_split;
static const lem_rgb *g_split_pal;

/* The palette the screen is CURRENTLY showing while it fades, and the one it
 * is heading for. The original does not cut to a screen - image 0x055B7
 * walks the palette there a notch a retrace - so a port that cuts is missing
 * something the original spends real time on. */
static lem_rgb       g_fade_cur[16], g_fade_tgt[16];
static lem_rgb       g_fade_low_cur[16], g_fade_low_tgt[16];

static int fade_tick(void *ctx, uint8_t *pix)
{
    (void)ctx; (void)pix;
    int moved = palette_fade_step(g_fade_cur, g_fade_tgt, 16);
    if (g_split)
        moved |= palette_fade_step(g_fade_low_cur, g_fade_low_tgt, 16);
    /* Non-zero re-uploads the frame, which is what makes the fade visible;
     * once nothing moves the screen is simply held. */
    return moved;
}
static SDL_Renderer *g_ren;
static SDL_Texture  *g_tex;
static uint16_t      g_w, g_h;
static uint8_t       g_scale;

static void sound_open(void);
static void sound_pump(void);
static void sound_close(void);

static void view_shutdown(void)
{
    if (g_tex) SDL_DestroyTexture(g_tex);
    if (g_ren) SDL_DestroyRenderer(g_ren);
    if (g_win) SDL_DestroyWindow(g_win);
    g_tex = NULL; g_ren = NULL; g_win = NULL;
    sound_close();
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
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return -1;
    }
    /* The game asks a loaded driver for its sounds - see sound_request. A
     * machine with no audio device is not an error: the port stays silent and
     * everything else runs, which is what a DOS box with no card did. */
    sound_open();
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
    /* Ask for the keyboard. A window that has just been created - and this
     * one is recreated whenever the logical size changes, menu to level - is
     * not guaranteed focus by the window manager, and without it the level
     * view drew correctly and ignored every arrow key. */
    SDL_RaiseWindow(g_win);
    return 0;
}


static view_tick g_show_tick;
static void     *g_show_ctx;
static uint8_t  *g_show_pix;

int view_show(const uint8_t *pix, uint16_t src_w, uint16_t src_h,
              uint16_t view_w, uint16_t view_h,
              const lem_rgb *palette, const char *title,
              uint16_t start_x, uint8_t scale)
{
    if (scale < 1) scale = 1;
    if (view_w > src_w) view_w = src_w;
    if (view_h > src_h) view_h = src_h;

    /* THE SAME WINDOW the menu uses. This built and destroyed one of its own,
     * so starting a level opened a SECOND window beside the menu's - which is
     * what the persistent window in view_open was added to stop, and this
     * function was simply missed when it was.
     *
     * The viewport is a different size from the menu's, so view_open rebuilds
     * rather than reuses; that is one window changing size, not two. */
    if (view_open(view_w, view_h, title, scale) != 0)
        return -1;
    SDL_Renderer *ren = g_ren;
    SDL_Texture  *tex = g_tex;
    const Uint64 period = SDL_NS_PER_SECOND / VIEW_REFRESH_HZ;
    Uint64 due = SDL_GetTicksNS();

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

        sound_pump();
        if (g_show_tick && g_show_pix)
            g_show_tick(g_show_ctx, g_show_pix);

        void *pixels = NULL;
        int pitch = 0;
        if (SDL_LockTexture(tex, NULL, &pixels, &pitch)) {
            for (uint16_t y = 0; y < view_h; y++) {
                uint32_t *row = (uint32_t *)((uint8_t *)pixels + y * pitch);
                const uint8_t *src =
                    pix + (size_t)(scroll_y + y) * src_w + scroll_x;
                for (uint16_t x = 0; x < view_w; x++) {
                    lem_rgb c = (g_split && y >= g_split && g_split_pal)
                              ? g_split_pal[src[x]] : palette[src[x]];
                    row[x] = ((uint32_t)c.r << 16) |
                             ((uint32_t)c.g << 8) | c.b;
                }
            }
            SDL_UnlockTexture(tex);
        }
        SDL_RenderClear(ren);
        SDL_RenderTexture(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
        /* One pass per retrace - see VIEW_REFRESH_HZ. An absolute
         * deadline, for the reason level_run gives. */
        due += period;
        {
            Uint64 now = SDL_GetTicksNS();
            if (now < due) SDL_DelayNS(due - now);
            else due = now;
        }
    }

    /* The window stays open; it is torn down at exit. */
    return 0;
}


/* THE SCANCODE THE KEYBOARD PRODUCED - not what it means.
 *
 * The original's keyboard handler at image 0x016D4 reads port 0x60 and keeps
 * an 83-entry state table at ds:0x0F78; image 0x04D1A scans it and translates
 * through ds:0x202E. This replaces the first half of that - the hardware -
 * and menu_key_code and menu_dispatch in game.c are the second, transcribed.
 *
 * This used to be a key ENUM and a table of boxes. The enum was the port's
 * invention standing where the game has scancodes, and the boxes stood where
 * the game has nothing at all.
 */
static uint8_t scancode_of(SDL_Keycode k)
{
    switch (k) {
    case SDLK_ESCAPE:    return 0x01;
    case SDLK_MINUS:     return 0x0C;
    case SDLK_GRAVE:     return 0x29;
    case SDLK_F1:        return 0x3B;
    case SDLK_F2:        return 0x3C;
    case SDLK_F3:        return 0x3D;
    case SDLK_F4:        return 0x3E;
    case SDLK_UP:        return 0x48;
    case SDLK_KP_MINUS:  return 0x4A;
    case SDLK_DOWN:      return 0x50;
    default:             return 0xFF;   /* past the table: image 0x04D1A's
                                         * scan would not have found it */
    }
}

/* The menu's own two steps, transcribed in game.c. */
static int32_t menu_input(uint8_t scancode)
{
    return menu_dispatch(menu_key_code(scancode));   /* image 0x0447B */
}

/* A screen with nothing on it to choose takes Esc, and a click. */
static int32_t dismiss_input(uint8_t scancode)
{
    return menu_key_code(scancode) == 0x1B ? 1 : -1;
}

int32_t view_interact(const uint8_t *pix, uint16_t w, uint16_t h,
                      const lem_rgb *palette, const char *title, uint8_t scale,
                      view_input input, int32_t click_id)
{
    /* No tick: the picture is fixed, and the cast is safe because nothing
     * writes through it when `tick` is NULL. */
    return view_interact_anim((uint8_t *)pix, w, h, palette, title, scale,
                              input, click_id, NULL, NULL);
}

int32_t view_interact_anim(uint8_t *pix, uint16_t w, uint16_t h,
                           const lem_rgb *palette, const char *title,
                           uint8_t scale, view_input input, int32_t click_id,
                           view_tick tick, void *ctx)
{
    if (scale < 1) scale = 1;
    if (view_open(w, h, title, scale) != 0) return VIEW_CLOSED;
    SDL_Renderer *ren = g_ren;
    SDL_Texture  *tex = g_tex;
    const Uint64 period = SDL_NS_PER_SECOND / VIEW_REFRESH_HZ;
    Uint64 due = SDL_GetTicksNS();

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
                if (input) hit = input(scancode_of(e.key.key));
            } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                /* WHEREVER IT LANDED. Image 0x04473 tests ds:0x14's low three
                 * bits and jumps to image 0x0454C - key 0x80's own target -
                 * without once looking at the position. */
                hit = click_id;
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
                    const lem_rgb *pl =
                        (g_split && y >= g_split && g_split_pal)
                            ? g_split_pal : palette;
                    for (uint16_t x = 0; x < w; x++) {
                        lem_rgb c = pl[src[x]];
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
        /* One pass per retrace - see VIEW_REFRESH_HZ. An absolute
         * deadline, for the reason level_run gives. */
        due += period;
        {
            Uint64 now = SDL_GetTicksNS();
            if (now < due) SDL_DelayNS(due - now);
            else due = now;
        }
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
    g_show_tick = NULL; g_show_ctx = NULL; g_show_pix = NULL;
    return view_show(c->pix, c->w, c->h, 320, 160, palette, title,
                     start_x, scale);
}

/* One frame of a level: the terrain as composed, then every object over it at
 * whatever frame the stepper has reached. The objects are drawn into the
 * CANVAS rather than the screen, because view_show does the scrolling - so the
 * `scroll` the original applies at image 0x07BCF is zero here and the viewport
 * supplies it instead. */
struct level_frame {
    level        lv;
    level_anim   st[LEVEL_ANIM_MAX];
    const uint8_t *sec;
    uint32_t     len;
    canvas       *from;      /* the terrain each frame starts from - NOT
                              * const, a digger removes some of it */
    const uint8_t *lem_sec;  /* main.dat section 0, the lemming sprites */
    uint32_t     lem_len;
    const uint8_t *pan_sec;  /* main.dat section 2, the panel artwork */
    uint32_t     pan_len;
    uint8_t     *panel;      /* PANEL_LINE_W x PANEL_ART_H, rebuilt a frame */
    lemming      lem[LEMMING_MAX];
    uint8_t      released;   /* how many have come out of the trapdoor */
    uint16_t     view_x;     /* where the play area has scrolled to */
    int          stepped;    /* the first frame is the state before a step */
};

/* THE LEVEL'S SCREEN - mode 0x0D is 320x200 and it is two things stacked:
 * the play area at rows 0..159 and the control panel at 160..199. This port
 * had been showing a 320x160 window of the play area alone, so the panel -
 * every piece of which is transcribed and scores 100.00% against the
 * original - was never on screen in the game.
 *
 * The play area is a WINDOW onto the 1584-wide canvas; the panel is composed
 * fresh each frame because the minimap's dots and its viewport box move.
 *
 * The scrolling is THE PORT'S OWN. The original scrolls from the mouse at
 * the screen edges and from the minimap, and neither has been read, so this
 * offers the arrow keys and the mouse at the edges and says so rather than
 * inventing a rate and calling it transcribed.
 */
#define LEVEL_SCREEN_W 320
#define LEVEL_SCREEN_H 200
#define LEVEL_PLAY_H   160
#define LEVEL_EDGE     8       /* how near the edge the pointer must be */
#define LEVEL_SCROLL   8       /* pixels a step */


/* WHAT THE PLAYER USED TO DO - a recorded input script.
 *
 * This belongs here with the rest of the hardware's work, and it is the same
 * kind of thing: the level's loop asks SDL where the mouse is and whether a
 * button is down, exactly as the original asks INT 33h, and this answers
 * those two questions from a file instead of from a person.
 *
 * It exists because a frame-by-frame comparison of a level nobody touches
 * compares only the parts of the game that need no player. Every skill, the
 * whole of the panel's behaviour and the level's ending are behind an input,
 * and the way to score them against the original is to give BOTH sides the
 * same one at the same frame. tools/record_level.py --script feeds the same
 * file to the guest through the emulator's own mouse.
 *
 * The format is one action a line, frame first:
 *
 *     12 move 160 100      put the pointer at this SCREEN position
 *     14 down              press the left button
 *     20 up                release it
 *
 * Frames are this loop's own count, which is the index the frame sink uses,
 * so a script and a captured sequence line up by construction. */
#define SCRIPT_MAX 256
typedef struct { long frame; uint8_t what; int32_t x, y; } script_act;
enum { SCRIPT_MOVE = 0, SCRIPT_DOWN = 1, SCRIPT_UP = 2 };

static script_act g_script[SCRIPT_MAX];
static uint16_t g_script_n;

static void script_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "input script %s: cannot open it\n", path);
        return;
    }
    char line[128];
    while (g_script_n < SCRIPT_MAX && fgets(line, sizeof line, f)) {
        char verb[16];
        long fr;
        int32_t x = 0, y = 0;
        if (line[0] == '#' || line[0] == '\n') continue;
        int got = sscanf(line, "%ld %15s %d %d", &fr, verb, &x, &y);
        if (got < 2) continue;
        script_act *a = &g_script[g_script_n];
        a->frame = fr; a->x = x; a->y = y;
        if (!strcmp(verb, "move") && got == 4) a->what = SCRIPT_MOVE;
        else if (!strcmp(verb, "down")) a->what = SCRIPT_DOWN;
        else if (!strcmp(verb, "up")) a->what = SCRIPT_UP;
        else continue;
        g_script_n++;
    }
    fclose(f);
    fprintf(stderr, "  [script] %u actions from %s\n", g_script_n, path);
}

/* One press of the left button, wherever the pointer is. A scripted click
 * and a real one go through this same function - written out twice, the two
 * would differ, which is the fault that had the window path showing an empty
 * cave while every harness frame matched. */
static void level_click(struct level_frame *A, int32_t *scroll_x)
{
    /* Image 0x03EF9 decides what was clicked. */
    level_hit h = level_pointer_hit();
    if (h.what == LEVEL_HIT_MINIMAP)
        *scroll_x = h.scroll;
    else if (h.what == LEVEL_HIT_PLAY) {
        /* Image 0x03150 opens on the pause flag: a click on a lemming
         * assigns nothing while the game is stopped. */
        if (level_paused() == 0xFF) return;
        /* Image 0x03170: a skill is selected, so find what is under the
         * pointer and give it one. */
        /* Image 0x0320D opens by testing the count and image 0x03224
         * spends it. The count lives in game.c now, because the PANEL has
         * to draw the same number the assignment reads - it was drawing the
         * level header's instead, which never moves. */
        /* Image 0x03180 dispatches the selected button to its handler, and
         * each opens by testing its own count - see level_skill_left. */
        /* AND AN ASSIGNMENT DOES TOO - image 0x03202 tests the same bits and
         * image 0x03205 asks for sound 0x04, inside image 0x03150. */
        if (level_skill_left(level_skill()) && sound_enabled(3))
            sound_request(0x61, 4, 0x04);       /* image 0x03205 */
        if (level_skill_left(level_skill())) {
            int16_t k = level_lemming_at(A->lem, A->released, A->view_x);
            if (k >= 0) {
                int took = 0;
                if (level_skill() == SKILL_DIGGER)
                    took = lem_dig_start(&A->lem[k]);      /* image 0x0320D */
                else if (level_skill() == SKILL_BUILDER)
                    took = lem_build_start(&A->lem[k]);    /* image 0x032BD */
                else if (level_skill() == SKILL_BASHER)
                    took = lem_bash_start(&A->lem[k]);     /* image 0x034CE */
                else if (level_skill() == SKILL_MINER)
                    took = lem_mine_start(&A->lem[k]);     /* image 0x03598 */
                else if (level_skill() == SKILL_BLOCKER)
                    took = lem_block_start(&A->lem[k]);    /* image 0x03336 */
                else if (level_skill() == SKILL_BOMBER)
                    took = lem_bomb_start(&A->lem[k]);     /* image 0x031C4 */
                else if (level_skill() == SKILL_CLIMBER)
                    took = lem_climb_start(&A->lem[k]);    /* image 0x0329B */
                else if (level_skill() == SKILL_FLOATER)
                    took = lem_float_start(&A->lem[k]);    /* image 0x03557 */
                if (took) level_skill_spend(level_skill());
            }
        }
    }
    else if (h.what == LEVEL_HIT_BUTTON) {
        /* NOT EVERY BUTTON IS A SKILL, and the port had treated all thirteen
         * as one - clicking the nuke selected "skill 11", which nothing
         * dispatches, and quietly left the panel with no skill armed.
         *
         * Image 0x03F27 refuses anything below 2 outright, then image
         * 0x03F3D..0x03F4D sends 10, 11 and 12 away before image 0x03F66
         * stores the rest in ds:0x82:
         *
         *     0, 1   image 0x04032   THE RELEASE RATE - transcribed now
         *     2..9   image 0x03F66   the eight skills
         *     10     image 0x04018   pause - NOT TRANSCRIBED
         *     11     image 0x03FEF   THE NUKE
         *     12     image 0x03FEE   nothing at all, a bare `ret`
         *
         * The two marked not transcribed have their addresses here rather
         * than a guess in their place; pause in particular is not a button
         * on its own but a flag the whole frame loop reads (image 0x01F19
         * returns early on it), so it is a change to the loop and not to
         * this switch. */
        /* BUTTONS 0 AND 1 ARE THE RELEASE RATE, and image 0x03F27 sends
         * them there rather than refusing them: `cmp al,2 / jge 0x3f2e /
         * jmp 0x4032`. This line read that as "refuses anything below 2
         * outright", so the panel's minus and plus did nothing at all. A
         * player found it.
         *
         * They also skip the latch at image 0x03F2E that the skill buttons
         * take, which is what lets a held one repeat in the original. */
        /* A BUTTON MAKES A NOISE whichever one it is - image 0x03F75 tests
         * ds:0x25A7 bits 0-1 and image 0x03F77 asks for sound 0x01 with
         * AH=4, before the dispatch below sorts out which button it was. */
        if (sound_enabled(3))             /* image 0x03F75 */
            sound_request(0x61, 4, 0x01);       /* image 0x03F77 */
        if (h.button < 2) {               /* image 0x03F27 */
            level_rate_click(h.button);   /* image 0x04032: 0 down, 1 up */
            return;
        }
        if (h.button == 12) return;       /* image 0x03FEE: a bare `ret` */
        if (h.button == 10) {             /* image 0x04041 */
            level_pause_click();          /* image 0x04018 */
            return;
        }
        if (h.button == 11) {             /* image 0x03F48 */
            level_nuke_click();           /* image 0x03FEF */
            return;
        }
        /* Image 0x03F52: no skill is selected while paused either. */
        if (level_paused() == 0xFF) return;
        /* The highlight sits one slot on from the number it belongs to -
         * PANEL_SEL_X0 is 16 where the numbers start at PANEL_ART_X, 32 -
         * so the box for button k is drawn at k + 1. */
        level_skill_select(h.button);     /* image 0x03F66: ds:0x82 */
    }
}

/* THE FUNCTION KEYS, image 0x03E6F.
 *
 * The same routine that handles a panel CLICK reads the scancode the
 * keyboard handler left in ds:0x1FD0 (image 0x016D4 stores it, and clears it
 * on the key-up), so a key and a click reach the same places:
 *
 *     0x3B  F1              image 0x04068   the release rate, down
 *     0x3C  F2              image 0x0403D   the release rate, up
 *     0x3D..0x44  F3..F10   image 0x03F5C   the eight skills, buttons 2..9
 *     0x4E  keypad +        image 0x04018   pause
 *     0x57  F11             image 0x04018   pause
 *     0x53  Del             image 0x03FEF   the nuke
 *     0x58  F12             image 0x03FEF   the nuke
 *
 * NOTE WHERE THE SKILLS ENTER. Image 0x03E9C..0x03EEB jump to image 0x03F5C,
 * which is PAST image 0x03F52's pause test and past image 0x03F2E's latch -
 * so a function key arms a skill while the game is paused, where a click on
 * the same button does not, and a held key is not stopped from repeating by
 * the thing that stops a held mouse button. Both are the original's, and
 * neither is what a tidier reading would have produced.
 */
static void level_key_button(SDL_Keycode k)
{
    switch (k) {
    case SDLK_F1: level_rate_click(0); return;   /* image 0x04068 */
    case SDLK_F2: level_rate_click(1); return;   /* image 0x0403D */
    case SDLK_KP_PLUS: case SDLK_F11:
        level_pause_click();           return;   /* image 0x04018 */
    case SDLK_DELETE: case SDLK_F12:
        level_nuke_click();            return;   /* image 0x03FEF */
    default: break;
    }
    /* image 0x03EA0 onwards: F3 is button 2 and each key after it is one
     * more, which is why the original counts with `inc al` rather than a
     * table. */
    int button = -1;
    if (k >= SDLK_F3 && k <= SDLK_F10)
        button = 2 + (int)(k - SDLK_F3);
    if (button < 0) return;

    /* image 0x03F66 stores it, and image 0x03F77 makes the button's noise -
     * the same one a click makes, because the key path joins the click path
     * before it. */
    if (sound_enabled(3))                        /* image 0x03F70 */
        sound_request(0x61, 4, 0x01);                  /* image 0x03F77 */
    level_skill_select((uint8_t)button);         /* image 0x03F66 */
}

/* ======================================================================
 * WHAT THE LEVEL'S FADE NEEDS FROM THE HARDWARE - image 0x0031E's four
 * calls, and nothing else of it.
 *
 * The phase machine and both fade routines are transcribed in game.c. What
 * cannot be is the DAC: image 0x0534B and image 0x05329 walk two palettes a
 * notch, image 0x01550 waits a retrace and image 0x0553D pushes the result
 * at the card. Those are here, and the STEP they are built from - image
 * 0x056E2 - is game.c's palette_fade_step, so what lives in this file is
 * only the pair of palettes and the moment they reach the screen.
 *
 * `g_lvl_top` and `g_lvl_bottom` are the palettes the screen is CURRENTLY
 * shown through - the level carries two, swapped part-way down - and the
 * `_tgt` pair is where the fade in is heading. A fade out heads for black.
 *
 * With no window there is nothing to fade, so video_present answers 0 and
 * the current pair starts AT the target: image 0x0033D's single step then
 * finds the palette already there and clears the bit on the first frame,
 * which is the behaviour the harness had before any of this existed.
 * ====================================================================== */
static uint8_t     *g_lvl_screen;
static lem_rgb      g_lvl_top[16], g_lvl_bottom[16];
static lem_rgb      g_lvl_tgt_top[16], g_lvl_tgt_bottom[16];
static SDL_Texture  *g_lvl_tex;
static SDL_Renderer *g_lvl_ren;
static int           g_lvl_live;        /* ds:0x200E - is there a display */

/* The level's screen into the texture, through whatever the palettes hold
 * now. One copy of this conversion, called by the play loop and by the
 * fade: it was written out twice and the second copy only ever ran on the
 * way out of a level, where nothing looks at it. */
static void level_screen_upload(void)
{
    void *pixels = NULL;
    int pitch = 0;
    if (!g_lvl_tex || !g_lvl_screen) return;
    if (!SDL_LockTexture(g_lvl_tex, NULL, &pixels, &pitch)) return;
    for (uint16_t y = 0; y < LEVEL_SCREEN_H; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)pixels + y * pitch);
        const uint8_t *src = g_lvl_screen + (size_t)y * LEVEL_SCREEN_W;
        const lem_rgb *pal = (y < LEVEL_PLAY_H) ? g_lvl_top : g_lvl_bottom;
        for (uint16_t x = 0; x < LEVEL_SCREEN_W; x++) {
            lem_rgb q = pal[src[x]];
            row[x] = ((uint32_t)q.r << 16) | ((uint32_t)q.g << 8) | q.b;
        }
    }
    SDL_UnlockTexture(g_lvl_tex);
}

/* Image 0x0534B and image 0x05329, one notch. */
int level_fade_notch(uint8_t out)
{
    lem_rgb black[16];
    const lem_rgb *tt = g_lvl_tgt_top, *tb = g_lvl_tgt_bottom;
    int moved;
    /* Nothing on a display is nothing to fade, so the fade is already over.
     * Image 0x0033D's single step reaches the same place a frame at a time;
     * this reaches it on the first one, which is what a harness with a
     * frame budget needs. */
    if (!g_lvl_live) return 0;
    if (out) {
        memset(black, 0, sizeof black);
        tt = tb = black;
    }
    moved  = palette_fade_step(g_lvl_top, tt, 16);
    moved |= palette_fade_step(g_lvl_bottom, tb, 16);
    return moved;
}

/* ds:0x200E, which image 0x00325 and image 0x00346 both test. */
int video_present(void) { return g_lvl_live; }

/* Image 0x01550 - the retrace the fade's loop waits for, once a pass. */
void video_retrace_wait(void)
{
    if (g_lvl_live) SDL_DelayNS(SDL_NS_PER_SECOND / VIEW_REFRESH_HZ);
}

/* Image 0x0553D - the palette at the card, which is what makes a notch of
 * the fade visible. The screen itself has not changed. */
void video_palette_upload(void)
{
    SDL_Event e;
    if (!g_lvl_live || !g_lvl_ren) return;
    level_screen_upload();
    SDL_RenderClear(g_lvl_ren);
    SDL_RenderTexture(g_lvl_ren, g_lvl_tex, NULL, NULL);
    SDL_RenderPresent(g_lvl_ren);
    /* A fade holds the play loop for the best part of a second, and a
     * window that answers nothing for that long is one the desktop reports
     * as hung. Only the close box is acted on. */
    while (SDL_PollEvent(&e))
        if (e.type == SDL_EVENT_QUIT) g_lvl_live = 0;
}

static int level_run(struct level_frame *A, const canvas *c,
                     const lem_rgb *palette, const char *title, uint8_t scale)
{
    if (scale < 1) scale = 1;
    if (view_open(LEVEL_SCREEN_W, LEVEL_SCREEN_H, title, scale) != 0)
        return -1;
    SDL_Renderer *ren = g_ren;
    SDL_Texture  *tex = g_tex;

    /* THE ORIGINAL'S OWN CEILING, not the canvas width: image 0x040E8 stops
     * at 0x500 and this port's view_x is sixteen less. Deriving it from
     * `c->w` was right only while the canvas was the displayed width. */
    int32_t max_x = LEVEL_SCROLL_MAX;
    if (max_x > (int32_t)c->w - LEVEL_SCREEN_W)
        max_x = (int32_t)c->w - LEVEL_SCREEN_W;
    if (max_x < 0) max_x = 0;
    /* The level header's start_x IS the viewport's left edge, not its
     * centre. tools/compare_run.py fits 624 for "Just dig!" and the header
     * says 624; centring on it, which is what the old 320x160 view did, put
     * the play area 160 pixels out and scored 36%. */
    int32_t scroll_x = (int32_t)A->lv.start_x;

    const char *cap = SDL_getenv("LEM_FRAMES");
    long budget = cap ? SDL_atoi(cap) : 0;

    uint8_t *screen = calloc(LEVEL_SCREEN_W * LEVEL_SCREEN_H, 1);
    if (!screen) return -1;
    g_lvl_screen = screen;
    g_lvl_tex = tex;
    g_lvl_ren = ren;

    /* Two palettes, and which one a row gets depends on where it is.
     *
     * The game swaps attributes 8..15 part-way down the screen - see
     * level_panel_palette - so the play area keeps the terrain's browns and
     * the panel below gets greens and greys. On a plain PC it does not, and
     * the panel makes do with the level's palette; that is what lem_machine
     * selects. The lower eight entries are the same either way. */
    lem_rgb top[16], bottom[16];
    for (uint8_t i = 0; i < 16; i++) top[i] = bottom[i] = palette[i];
    if (lem_machine == LEM_PC_HIGH)
        level_panel_palette(&bottom[8]);
    /* And the fade's copy of them: `top` and `bottom` stay the level's own
     * colours - the capture below writes them, and a capture must never
     * hold a mid-fade palette - while the pair the screen is drawn through
     * starts at black and climbs. */
    memcpy(g_lvl_tgt_top, top, sizeof top);
    memcpy(g_lvl_tgt_bottom, bottom, sizeof bottom);

    /* A frame sink, for checking THIS path rather than a second arrangement
     * of the same calls. Every measurement of the level goes through lemdev,
     * which composes the canvas and the panel separately; this writes the
     * screen the game actually puts on the display, in the same 'LEMRW2'
     * container, so tools/compare_frame.py can score it against the
     * original's own 320x200 frames.
     *
     * Env-gated and in the IO file on purpose: the game has no command line
     * and writes no files. Setting LEM_SCREEN_DIR is a developer asking for
     * one, the way LEM_FRAMES already asks for a frame budget. */
    const char *sink = SDL_getenv("LEM_SCREEN_DIR");
    /* ds:0x200E. A capture harness is not a display: it has a frame budget,
     * and a fade held inside frame 0 would spend a second of wall clock
     * producing no frames while every frame it did produce carried a
     * palette climbing out of black. */
    g_lvl_live = (!sink && g_win) ? 1 : 0;
    if (g_lvl_live) {
        memset(g_lvl_top, 0, sizeof g_lvl_top);
        memset(g_lvl_bottom, 0, sizeof g_lvl_bottom);
    } else {
        memcpy(g_lvl_top, g_lvl_tgt_top, sizeof g_lvl_top);
        memcpy(g_lvl_bottom, g_lvl_tgt_bottom, sizeof g_lvl_bottom);
    }
    /* THE PORT'S OWN LEMMING ARRAY, frame by frame.
     *
     * A screen comparison says two lemmings look different; it does not say
     * whether the x, the y, the flags or the animation counter is the one
     * that moved. record_level.py --lemmings writes the ORIGINAL's array in
     * the same shape, and the two together turn a puzzle into a field name.
     *
     * Env-gated and here with the rest of the hardware's work for the same
     * reason as LEM_SCREEN_DIR: the game has no command line and writes no
     * files, and a developer asking for one is not the game doing it. */
    const char *state = SDL_getenv("LEM_STATE_FILE");
    FILE *statef = state ? fopen(state, "w") : NULL;
    /* A recorded input, so a comparison can cover the parts of the game that
     * need a player - see script_load. */
    const char *script = SDL_getenv("LEM_INPUT_SCRIPT");
    if (script) script_load(script);
    uint16_t script_at = 0;
    long frame_no = 0;
    /* Edge scrolling waits for a pointer that has actually MOVED. A polled
     * position cannot tell "at the left edge" from "no pointer here": under
     * the dummy video driver it reads (0,0) and reports this window as
     * focused, so the view walked off the left of the level at 8 pixels a
     * frame and every headless frame after 78 was black. */
    int seen_motion = 0;

    /* The pointer is HELD by the window, and Ctrl+Alt gives it back.
     *
     * The original owns the mouse completely - it hides the driver's pointer
     * at image 0x00F43, sets its own range at images 0x00F2B and 0x00F36 and
     * draws its own cross-hair - and a window that lets the host pointer walk
     * out of it while the game still thinks it is moving does not reproduce
     * that. Held, the two agree; released, the game keeps the last position. */
    int grabbed = 1;
    SDL_SetWindowRelativeMouseMode(g_win, true);

    /* PACE ON THE GAME'S RATE, and on an absolute clock.
     *
     * A frame is FIVE ticks of the game's own timer - image 0x00186 waits
     * for ds:0x1FCD to exceed 4 - and a tick is a vertical retrace, because
     * image 0x017FE waits for one before reloading the PIT. So the level
     * runs at TIMER_HZ/5, fourteen frames a second on a 70 Hz adapter.
     *
     * It used to pace at 17, on the reading that ds:0x85's 0x11 was the frame
     * rate. It is not: that counter is how many frames the game counts before
     * stepping its DISPLAYED clock, so a level's five minutes takes rather
     * more than five real ones - which is what DOSBox does and what this did
     * not. Before that it slept 16 ms a frame, which was sixty.
     *
     * The deadline is absolute rather than a sleep per frame: a sleep is at
     * the mercy of the host scheduler and of however long the frame took to
     * compose, which would make the game's speed a property of the machine
     * rather than of the game. Falling behind resets the deadline instead of
     * building a debt the loop would then sprint to repay. */
    const Uint64 period =
        (Uint64)(SDL_NS_PER_SECOND / LEVEL_FRAME_HZ);
    Uint64 due = SDL_GetTicksNS();

    int held = 0;          /* the button is down - a rate button repeats */
    int running = 1;
    while (running) {
        /* The script speaks first, so a scripted frame behaves the same
         * whether or not the host had an event waiting. */
        while (script_at < g_script_n && g_script[script_at].frame <= frame_no) {
            const script_act *act = &g_script[script_at++];
            if (act->what == SCRIPT_MOVE) {
                /* The script is in SCREEN pixels; the game's own pointer sits
                 * eight further on, which is where image 0x0426D clamps. */
                lem_mouse_move(act->x + 8, act->y + 8);
                seen_motion = 1;
            } else if (act->what == SCRIPT_DOWN) {
                held = 1;
                level_click(A, &scroll_x);
            } else if (act->what == SCRIPT_UP) {
                held = 0;
            }
        }

        /* A HELD RATE BUTTON REPEATS. The original polls the button from the
         * panel's own per-frame update (image 0x03B3F) and divides with
         * ds:0x69, so holding the minus or plus walks the rate one frame in
         * three. The port takes the press as an edge, which is right for the
         * skills - image 0x03170's latch gives them one assignment however
         * long the button is down - but it left the rate buttons doing a
         * single step per click.
         *
         * So only these two repeat, which is also what the original's own
         * split says: buttons from 2 up go through the latch at image
         * 0x03F2E and 0 and 1 never touch it. */
        if (held) {
            level_hit rh = level_pointer_hit();
            if (rh.what == LEVEL_HIT_BUTTON && rh.button < 2)
                level_rate_click(rh.button);    /* image 0x04032 */
        }

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = 0;
            else if (e.type == SDL_EVENT_MOUSE_MOTION) {
                seen_motion = 1;
                if (grabbed) {
                    /* RELATIVE motion while the pointer is held, which is
                     * what the original works in: it asks the driver for
                     * mickeys and sets the mickey-to-pixel ratio itself at
                     * image 0x00F20. An absolute position would also stop at
                     * the edge of the desktop rather than at the edge of the
                     * game's own range. */
                    lem_mouse_move((int32_t)lem_mouse_x + (int32_t)e.motion.xrel,
                                   (int32_t)lem_mouse_y + (int32_t)e.motion.yrel);
                } else {
                    int ow = 0, oh = 0;
                    SDL_GetRenderOutputSize(ren, &ow, &oh);
                    if (ow > 0 && oh > 0)
                        lem_mouse_move(
                            (int32_t)(e.motion.x * LEVEL_SCREEN_W / (float)ow) + 8,
                            (int32_t)(e.motion.y * LEVEL_SCREEN_H / (float)oh) + 8);
                }
            }
            else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (!grabbed) {
                    /* Click to take it back, the way every DOS front end
                     * does. That click is for the window, not the game. */
                    grabbed = 1;
                    SDL_SetWindowRelativeMouseMode(g_win, true);
                } else {
                    /* THE TWO BUTTONS DO DIFFERENT THINGS, and the port
                     * treated any of them as a click. Image 0x04269 stores
                     * INT 33h AX=3's BX in ds:0x14, so bit 0 is the left
                     * button and bit 1 the right, and the game asks for them
                     * separately: image 0x03168 does `and ax, 1` before it
                     * will assign anything, while image 0x03DB7 tests bit 1
                     * and makes the under-pointer scan pick the PLAIN
                     * lemming over the busy one.
                     *
                     * So the right button is how the original lets you point
                     * past a builder at the walker behind it. Here it did
                     * nothing at all - lem_buttons was never written - and
                     * it also assigned, which the original will not do. */
                    if (e.button.button == SDL_BUTTON_LEFT)
                        lem_buttons |= 1;
                    else if (e.button.button == SDL_BUTTON_RIGHT)
                        lem_buttons |= 2;
                    if (e.button.button == SDL_BUTTON_LEFT) {
                        held = 1;
                        level_click(A, &scroll_x);      /* image 0x03168 */
                    }
                }
            }
            else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                if (e.button.button == SDL_BUTTON_LEFT) {
                    lem_buttons &= (uint8_t)~1u;
                    held = 0;
                } else if (e.button.button == SDL_BUTTON_RIGHT) {
                    lem_buttons &= (uint8_t)~2u;
                }
            }
            else if (e.type == SDL_EVENT_KEY_DOWN) {
                /* Ctrl+Alt gives the pointer back, which is the gesture
                 * DOSBox trained everyone to reach for. Without a release
                 * the window owns the mouse and the only way out is to kill
                 * the process - so a grab without one is a bug, not a
                 * feature. */
                if ((e.key.mod & SDL_KMOD_CTRL) && (e.key.mod & SDL_KMOD_ALT)) {
                    grabbed = 0;
                    SDL_SetWindowRelativeMouseMode(g_win, false);
                    break;
                }
                /* image 0x03E6F reads the scancode the keyboard handler
                 * left behind, in the same routine that handles a click. */
                level_key_button(e.key.key);
                switch (e.key.key) {
                /* ESCAPE ENDS THE LEVEL; it does not leave it.
                 *
                 * Image 0x002AB tests bit 0x20 of the key word ds:0x1FC7 and
                 * jumps to image 0x002B3, which sets ds:0x4F to 0xFF - the
                 * same byte image 0x001C8 tests when everyone is out or the
                 * clock has run down. So the play loop ends exactly as it
                 * would have on its own, image 0x002FB fades, and image
                 * 0x00311 goes to the results screen with whatever was
                 * rescued.
                 *
                 * The port took Escape as "close the level", so a player got
                 * the menu and no result at all. Bit 0x20 is the same key the
                 * results screen tests at image 0x047C1, which is the
                 * corroboration: one key, two screens, both sending it on. */
                /* Escape ends the level rather than closing the window,
                 * and it ends it the way anything else does - image
                 * 0x002B3, so it fades out and reaches the results. */
                case SDLK_ESCAPE: level_end_begin();                     break;
                /* THE PORT'S OWN. The original has no way to abandon a level
                 * without a result; a window does. */
                case SDLK_Q:      running = 0;                            break;
                case SDLK_LEFT:  scroll_x -= LEVEL_SCROLL * 2;            break;
                case SDLK_RIGHT: scroll_x += LEVEL_SCROLL * 2;            break;
                case SDLK_HOME:  scroll_x = 0;                            break;
                case SDLK_END:   scroll_x = max_x;                        break;
                default: break;
                }
            }
        }
        /* The GAME's cursor at either edge scrolls - not the host pointer.
         *
         * This used to read SDL's pointer position, which cannot work once
         * the window holds the mouse: the host pointer then never moves, so
         * the edge test could not fire and scrolling by mouse simply did
         * nothing. The game's own cursor is the thing that is at an edge,
         * and its coordinates are the screen's plus eight - see
         * level_pointer_hit.
         *
         * The RATE is still the port's own; the original's has not been
         * read. */
        if (seen_motion) {
            int32_t cx = (int32_t)lem_mouse_x - 8;
            if (cx < LEVEL_EDGE)                        scroll_x -= LEVEL_SCROLL;
            else if (cx > LEVEL_SCREEN_W - LEVEL_EDGE)  scroll_x += LEVEL_SCROLL;
        }
        if (scroll_x < 0) scroll_x = 0;
        if (scroll_x > max_x) scroll_x = max_x;
        A->view_x = (uint16_t)scroll_x;

        /* One frame of the game, then one of the panel. */
        sound_pump();
        if (g_show_tick && g_show_pix) g_show_tick(g_show_ctx, g_show_pix);
        if (A->panel && A->pan_sec) {
            memset(A->panel, 0, (size_t)PANEL_LINE_W * PANEL_ART_H);
            level_panel_build(A->panel, A->pan_sec, A->pan_len, &A->lv,
                              A->from->pix, A->from->w, A->from->h,
                              A->lem, A->released, A->view_x,
                              level_selected());
        }

        for (uint16_t y = 0; y < LEVEL_PLAY_H; y++)
            memcpy(screen + (size_t)y * LEVEL_SCREEN_W,
                   c->pix + (size_t)y * c->w + scroll_x, LEVEL_SCREEN_W);
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
        lem_cursor_box =
            level_lemming_at(A->lem, A->released, A->view_x) >= 0;
        level_cursor_draw(screen, LEVEL_SCREEN_W, LEVEL_SCREEN_H, A->view_x);

        level_screen_upload();
        if (sink) {
            char path[1024];
            snprintf(path, sizeof(path), "%s/f%05ld.raw", sink, frame_no);
            FILE *f = fopen(path, "wb");
            if (f) {
                uint16_t wh[2] = { LEVEL_SCREEN_W, LEVEL_SCREEN_H };
                fwrite("LEMRW2", 1, 6, f);
                fwrite(wh, sizeof(uint16_t), 2, f);
                /* The TOP palette: the container holds one, and the play
                 * area is what gets scored against the original's own
                 * frames. The split is in the comment above, not in here. */
                for (uint8_t i = 0; i < 16; i++) {
                    fputc(top[i].r, f);
                    fputc(top[i].g, f);
                    fputc(top[i].b, f);
                }
                fwrite(screen, 1, LEVEL_SCREEN_W * LEVEL_SCREEN_H, f);
                fclose(f);
            }
        }
        if (statef) {
            fprintf(statef, "{\"frame\":%ld,\"released\":%u,\"live\":%u,"
                    "\"saved\":%u,\"over\":%u,\"tick\":%u,\"view_x\":%d,\"lem\":[", frame_no,
                    (unsigned)A->released,
                    (unsigned)lemmings_live(A->lem, A->released),
                    (unsigned)level_saved, (unsigned)g_level_over,
                    (unsigned)level_tick_now(), (int)A->view_x);
            for (uint8_t k = 0; k < A->released && k < LEMMING_MAX; k++) {
                const lemming *a = &A->lem[k];
                /* The FUSE and the direction are in here because the nuke
                 * is a thing that happens to the fuse and to nothing else
                 * for seventy-nine frames: a comparison of position and
                 * flags alone agrees all the way from the double click to
                 * the first explosion, and would have said the cascade was
                 * right whatever order it lit them in. */
                fprintf(statef,
                        "%s{\"x\":%d,\"y\":%d,\"hot_x\":%d,\"hot_y\":%d,"
                        "\"flags\":%u,\"anim\":%u,\"alive\":%u,"
                        "\"fuse\":%u,\"dir\":%d,\"state\":%u}",
                        k ? "," : "", a->x, a->y, a->hot_x, a->hot_y,
                        (unsigned)a->flags, (unsigned)a->anim,
                        (unsigned)a->alive, (unsigned)a->fuse,
                        (int)a->dir, (unsigned)a->state);
            }
            fprintf(statef, "]}\n");
        }
        frame_no++;

        SDL_RenderClear(ren);
        SDL_RenderTexture(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
        if (!sink) {
            due += period;
            Uint64 now = SDL_GetTicksNS();
            if (now < due) SDL_DelayNS(due - now);
            else due = now;          /* behind: start again from here */
        }
        if (budget && --budget == 0) running = 0;
        /* IMAGE 0x001C8 AND IMAGE 0x002C6, which are two different tests.
         * The first is "has the level ended" and sends the loop to image
         * 0x002C3; the second is "has the fade finished", and only that one
         * lets it out. Between them the loop keeps composing and drawing,
         * which is what the fade is a fade OF. */
        if (level_end_done()) running = 0;
    }
    /* Image 0x002CD, which the loop above has just fallen out of at image
     * 0x002C6: stop the driver before the results screen. */
    level_end_finish();

    /* No fade here. It is image 0x0031E's, it runs INSIDE the loop above,
     * and it was written out a second time in this file for as long as the
     * loop exited the moment the level ended - which meant the picture
     * being faded was one nothing was still drawing. See level_end_begin. */
    g_lvl_screen = NULL; g_lvl_tex = NULL; g_lvl_ren = NULL;

    /* Never leave the pointer captured by a window that is going away. */
    SDL_SetWindowRelativeMouseMode(g_win, false);
    if (statef) fclose(statef);
    free(screen);
    return 0;
}

int view_run_anim(canvas *c, const lem_rgb *palette, const char *title,
                  uint16_t start_x, uint8_t scale,
                  view_tick tick, void *ctx)
{
    g_show_tick = tick; g_show_ctx = ctx; g_show_pix = c->pix;
    int rc = view_show(c->pix, c->w, c->h, 320, 160, palette, title,
                       start_x, scale);
    g_show_tick = NULL; g_show_ctx = NULL; g_show_pix = NULL;
    return rc;
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

/* THE WHOLE OF A FILE, IN MEMORY - which DOS used to do a piece at a time.
 *
 * Image 0x01D2E is the original's loader and it is pure DOS: INT 21h AH=23h
 * for the size through an FCB, the size divided into paragraphs and checked
 * against what is left at ds:0x1F90, then INT 21h AH=3Fh reading into a bump
 * allocator at ds:0x1F8A - which it advances past a 13-byte section header
 * each time (image 0x01DA6's `add dx, 0xd`). It streams the sections one
 * after another into one buffer and never has the whole file at once.
 *
 * The port reads it whole. That is the arrangement dat_load's comment already
 * described, but the READING half was sitting in game.c, and the note at the
 * top of this block says what that means: a routine marked as ours inside the
 * game's file is either IO that belongs here, or game logic nobody has
 * transcribed. This one is the first. Splitting the container and
 * decompressing it stay in game.c, because the format is the game's.
 */
int io_read_file(const char *path, uint8_t **out, long *len, const char **err)
{
    *out = NULL;
    *len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) { if (err) *err = "cannot open file"; return -1; }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f); if (err) *err = "cannot size file"; return -1;
    }
    long n = ftell(f);
    if (n <= 0) { fclose(f); if (err) *err = "empty file"; return -1; }
    rewind(f);
    uint8_t *blob = malloc((size_t)n);
    if (!blob) { fclose(f); if (err) *err = "out of memory"; return -1; }
    if (fread(blob, 1, (size_t)n, f) != (size_t)n) {
        free(blob); fclose(f);
        if (err) *err = "short read";
        return -1;
    }
    fclose(f);
    *out = blob;
    *len = n;
    return 0;
}

/* SOUND.
 *
 * WHAT IS TRANSCRIBED and where it lives:
 *
 *   the asking      game.c, beside the routine that asks - every call site,
 *                   its id, and the two gate bits of ds:0x25A7.
 *   the driver      adlib.c, transcribed from adlib.dat, which is a separate
 *                   program the game loads onto INT 61h and calls. It turns
 *                   an id into OPL2 register writes over time, and it is
 *                   checked against the ORIGINAL's own register stream by
 *                   tools/verify_adlib.py - write for write, no listening.
 *   the music       nowhere here. Twenty-one tunes and seventeen effects are
 *                   data in the player's own adlib.dat and stay there.
 *   the chip        opl.h over ymfm. An OPL2 is hardware; see that header.
 *
 * WHAT THIS FILE DOES is the part SDL owns: open a device, keep it fed, and
 * decide when the driver's tick happens.
 *
 * THE TICK IS NOT A FRAME. The original's driver is called from the game's
 * timer ISR at 97.08 Hz (the divisor it programs, measured - docs/sound.md),
 * and that has nothing to do with the 17 Hz of a level or the 70 Hz of the
 * menu. So the tick is driven off the AUDIO clock: one call every
 * 49716/97.08 samples, wherever the frame boundaries happen to fall. Ticking
 * once a frame would make the music's tempo a property of the display, which
 * is the same mistake as pacing the level off the retrace - CLAUDE.md
 * records that one already.
 *
 * There is no placeholder here any more. There used to be: a table of pitches
 * and envelopes, "chosen to be distinguishable, not measured off anything".
 * It made the game audible and it was not the game's sound.
 */
/* THE PORT'S OWN - see game.h. Defaults to what main.c defaults to, so a
 * caller that never sets it behaves as it always did. */
static const char *g_data_dir = "lemming1.pc";

void lem_set_data_dir(const char *dir) { if (dir) g_data_dir = dir; }
const char *lem_data_dir(void) { return g_data_dir; }

static SDL_AudioStream *g_snd_stream;
static double  g_tick_acc;             /* samples owed to the next tick */

/* The game asks through INT 60h (fn 3) or INT 61h (fn 4) - see docs/sound.md
 * for which call site asks for which, and why the two are alternatives
 * rather than two faces of one driver. */
/* WHICH INTERRUPT MATTERS AS MUCH AS WHICH AH.
 *
 * The game asks for sound two ways and they are not the same device:
 *
 *   INT 61h   the loaded driver - adlib.dat - and AH says what: 3 stores a
 *             TUNE number in its slot at driver +0x00ED, 4 an EFFECT at
 *             +0x00F0
 *   INT 60h   the fallback, for a machine where no driver answered. Every
 *             one of its sites is guarded by ds:0x2010 first - image 0x02366
 *             and image 0x02F7B both `cmp byte [0x2010], 0xff / je` PAST the
 *             call - so it never runs alongside the driver.
 *
 * This took only AH, so AH=3 meant "play a tune" whichever interrupt the
 * site belonged to. Image 0x02374's fallback beep for the bomber's cloud is
 * AH=3, AL=0x0C on INT 60h, and it became adlib_music(12): a whole tune,
 * started by a bomb, on a setting where music is off. */
void sound_request(uint8_t vec, uint8_t ah, uint8_t id)
{
    if (vec != 0x61) return;   /* the fallback device, which this port has not */
    if (!adlib_ready()) return;
    /* THE DRIVER'S OWN DISPATCH, and it is a table of five - adlib.c has an
     * entry transcribed for each. This was an if with two arms, so AH=2
     * fell into the else and asked for TUNE ZERO instead of stopping the
     * driver, and AH=5 did the same.
     *
     * That is why a level's music ran on into the next one. Image 0x002FE
     * stops the driver when a level ends, and the port never made the call
     * at all - so the request that arrives twenty frames into the next
     * level (image 0x003B4) reached a driver already playing something. */
    switch (ah) {
    case 2: adlib_stop();      break;       /* driver +0x003A */
    case 3: adlib_music(id);   break;       /* driver +0x0022 */
    case 4: adlib_effect(id);  break;       /* driver +0x002A */
    case 5: adlib_slot5(id);   break;       /* driver +0x0032 */
    default: break;
    }
}

/* image 0x003BB does this the instant it has asked for a tune. */
void sound_tick_now(void)
{
    if (adlib_ready()) adlib_tick();        /* driver +0x0115 */
}

/* Mix and hand to SDL. Called once a frame from the level and menu loops. */
/* How much audio to keep queued, and how much to build at a time.
 *
 * The device drains at 49,716 samples a second whatever the game is doing.
 * A LEVEL runs at 17 frames a second, so one frame owes 2,924 samples; the
 * menu at 70 owes 710. sound_pump is called once a frame from both, so a
 * pump that produces a FIXED amount is right for at most one of them - and
 * the first version produced 1,024, a third of what a level frame owes. The
 * device ran dry every frame, and that is what chopped noise is.
 *
 * So the target is a DEPTH, not a quantity: top the queue up to a fifth of a
 * second, however many chunks that takes. Deep enough to ride out a slow
 * frame, a level load or a fade; shallow enough that a click's sound is not
 * heard a fifth of a second after the click.
 *
 * Nothing in the frame-by-frame comparisons could have caught this. They
 * score what the driver WRITES, and the writes were already exact - 10,333
 * of 10,333. This is the same shape of gap CLAUDE.md records for the level
 * running at sixty frames a second while every frame matched: a rate is not
 * visible to anything that compares frame N against frame N. */
#define SND_TARGET_MS 200
#define SND_CHUNK     1024

int sound_target_bytes(void)
{
    return (int)((OPL_SAMPLE_RATE * SND_TARGET_MS) / 1000u)
           * (int)sizeof(int16_t);
}

uint32_t sound_fill_bytes(int queued, int target)
{
    const int chunk = SND_CHUNK * (int)sizeof(int16_t);
    uint32_t made = 0;
    while (queued < target) { queued += chunk; made += (uint32_t)chunk; }
    return made;
}

/* Build one chunk: tick the driver where the tick falls, render around it.
 *
 * Split out of sound_pump so the LIVE path - the one the game actually runs -
 * can be exercised without an audio device. Everything measured about the
 * sound so far has been either the driver alone (tools/verify_adlib.py) or
 * the pump's arithmetic (sound_fill_bytes); this is the code between them,
 * and it had never been run by anything but the game. */
void sound_build_chunk(int16_t *buf, uint32_t frames)
{
    /* One tick is one of the game's own INT 8 handlers - see TIMER_HZ. */
    const double per_tick = (double)OPL_SAMPLE_RATE / TIMER_HZ;
    uint32_t done = 0;
    while (done < frames) {
        /* One driver tick buys per_tick samples. The accumulator carries the
         * fraction, so the rate stays 97.08 Hz over time rather than being
         * rounded to 512 and running sharp. */
        if (g_tick_acc < 1.0) {
            timer_isr();                    /* image 0x017C7 */
            g_tick_acc += per_tick;
        }
        uint32_t want = (uint32_t)g_tick_acc;
        if (want > frames - done) want = frames - done;
        opl_render(buf + done, want);
        done += want;
        g_tick_acc -= want;
    }
}

static void sound_pump(void)
{
    if (!g_snd_stream || !adlib_ready()) return;

    const int target = sound_target_bytes();
    int queued = SDL_GetAudioStreamQueued(g_snd_stream);
    if (queued < 0) return;                 /* the device went away */

    int16_t buf[SND_CHUNK];
    while (queued < target) {
        sound_build_chunk(buf, SND_CHUNK);
        SDL_PutAudioStreamData(g_snd_stream, buf, (int)sizeof buf);
        queued += (int)sizeof buf;
    }
}

static void sound_open(void)
{
    /* ONCE, AND NOT PER SCREEN.
     *
     * The original does this in its start-up and never again: image 0x00576
     * points INT 61h at the driver, image 0x00589 loads adlib.dat, image
     * 0x0059D asks it whether there is a card, and image 0x005A9 sets
     * ds:0x25A7 from the answer.
     *
     * view_open calls this every time a window opens - menu, briefing,
     * level, results - and it used to redo the whole of it each time,
     * including sound_driver_present, which writes ds:0x25A7. So F3 set the
     * sound off and the very next screen set it back on, and a player who
     * turned everything off still heard everything. It was also re-reading
     * adlib.dat and opening a second audio stream per screen.
     *
     * view_shutdown is the only thing that closes it, and it runs at exit. */
    static int done;
    if (done) return;
    done = 1;

    const char *err = "unknown error";
    /* The driver's data is the player's own adlib.dat. No card and no file
     * are the same thing to the game: silence, and it carries on. */
    if (adlib_load(lem_data_dir(), &err) != 0)
        fprintf(stderr, "no sound: adlib.dat: %s\n", err);
    /* ds:0x2010, set at image 0x005A4 when the detect answers. Without it
     * image 0x04834 refuses and no tune is ever chosen. */
    sound_driver_present(adlib_ready());

    SDL_AudioSpec want;
    SDL_zero(want);
    /* Ask for the CHIP's rate and let SDL convert. Resampling here would be a
     * second place for the sound to be wrong. */
    want.freq = (int)OPL_SAMPLE_RATE;
    want.format = SDL_AUDIO_S16;
    want.channels = 1;
    g_snd_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                             &want, NULL, NULL);
    if (g_snd_stream)
        SDL_ResumeAudioStreamDevice(g_snd_stream);
}

static void sound_close(void)
{
    if (g_snd_stream) SDL_DestroyAudioStream(g_snd_stream);
    g_snd_stream = NULL;
    adlib_free();
}

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
/* Run the palette down to whatever g_fade_tgt now holds and stop. No input:
 * it is a fade, not a screen - the only way out is for it to finish. */
static void view_fade_out(const uint8_t *pix, uint16_t w, uint16_t h,
                          const char *title, uint8_t scale)
{
    if (view_open(w, h, title, scale) != 0) return;
    SDL_Renderer *ren = g_ren;
    SDL_Texture  *tex = g_tex;
    const Uint64 period = SDL_NS_PER_SECOND / VIEW_REFRESH_HZ;
    Uint64 due = SDL_GetTicksNS();
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_EVENT_QUIT) return;
        int moved = palette_fade_step(g_fade_cur, g_fade_tgt, 16);
        if (g_split)
            moved |= palette_fade_step(g_fade_low_cur, g_fade_low_tgt, 16);

        void *pixels = NULL;
        int pitch = 0;
        if (SDL_LockTexture(tex, NULL, &pixels, &pitch)) {
            for (uint16_t y = 0; y < h; y++) {
                uint32_t *row = (uint32_t *)((uint8_t *)pixels + y * pitch);
                const uint8_t *src = pix + (size_t)y * w;
                const lem_rgb *pl = (g_split && y >= g_split)
                                  ? g_fade_low_cur : g_fade_cur;
                for (uint16_t x = 0; x < w; x++) {
                    lem_rgb c = pl[src[x]];
                    row[x] = ((uint32_t)c.r << 16)
                           | ((uint32_t)c.g << 8) | c.b;
                }
            }
            SDL_UnlockTexture(tex);
        }
        SDL_RenderClear(ren);
        SDL_RenderTexture(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
        if (!moved) return;
        /* One pass per retrace - see VIEW_REFRESH_HZ. An absolute
         * deadline, for the reason level_run gives. */
        due += period;
        {
            Uint64 now = SDL_GetTicksNS();
            if (now < due) SDL_DelayNS(due - now);
            else due = now;
        }
    }
}

void menu_show_until_dismissed(const menu_screen *m, const char *title,
                               uint8_t scale)
{

    /* The briefing is two pictures stacked with different palettes - see
     * menu_screen.split. Every other screen leaves split at zero. */
    g_split = m->split;

    /* Up from black, the way the original arrives at it. */
    for (uint8_t i = 0; i < 16; i++) {
        g_fade_tgt[i] = m->palette[i];
        g_fade_low_tgt[i] = m->palette_low[i];
        g_fade_cur[i].r = g_fade_cur[i].g = g_fade_cur[i].b = 0;
        g_fade_low_cur[i].r = g_fade_low_cur[i].g = g_fade_low_cur[i].b = 0;
    }
    g_split_pal = m->split ? g_fade_low_cur : NULL;
    /* Esc or a click; the briefing says "Press mouse button to continue". */
    view_interact_anim(m->pix, MENU_W, MENU_H, g_fade_cur, title, scale,
                       dismiss_input, 1, fade_tick, NULL);

    /* And back down to black on the way out. The original fades the screen
     * away before it draws the next one; cutting straight to the level is
     * the thing that looks wrong even when every pixel is right. */
    for (uint8_t i = 0; i < 16; i++) {
        g_fade_tgt[i].r = g_fade_tgt[i].g = g_fade_tgt[i].b = 0;
        g_fade_low_tgt[i].r = g_fade_low_tgt[i].g = g_fade_low_tgt[i].b = 0;
    }
    view_fade_out(m->pix, MENU_W, MENU_H, title, scale);

    g_split = 0;
    g_split_pal = NULL;
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
    /* image 0x0447B for a key, image 0x04473 for a click - which is F1. */
    int32_t r = view_interact_anim(m->pix, MENU_W, MENU_H, m->palette,
                                   "PC Lemmings", scale, menu_input,
                                   MENU_PLAY, menu_frame_step, m);
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

/* ======================================================================
 * SHOWING A LEVEL.
 *
 * The port's own: loading a container, composing the terrain and putting it
 * in a window is a sequence the original has no single routine for - it
 * streams the level in as it goes. These exist so main.c can be the game's
 * flow and nothing else, and so devmain.c does not have to be the only place
 * that knows how to open a level.
 * ====================================================================== */

/* The briefing's I/O. Its TEXT is image 0x04566's, which derives the number
 * from ds:0x1F6B and writes the requirement into ds:0x41 - so neither comes
 * from a caller any more. */
int level_show_briefing(const char *dir, const char *file, uint32_t which,
                        uint8_t scale, const char **err)
{
    char path[1024];
    dat_file levels;
    level lv;
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    if (dat_load(path, &levels, err) != 0) return -1;
    if (which < 1 || which > levels.count) {
        dat_free(&levels);
        if (err) *err = "no such level in that container";
        return -1;
    }
    int rc = level_parse(levels.sections[which - 1].data,
                         levels.sections[which - 1].size, &lv, err);
    dat_free(&levels);
    if (rc != 0) return -1;
    /* ODDTABLE.DAT, image 0x019F5 - half the plates play their level under
     * another name with other skills. */
    if (level_apply_odd(dir, &lv, level_odd_taken(), err) != 0) return -1;
    lv.level_number = level_number();       /* image 0x04566 */

    menu_screen br;
    if (menu_briefing_screen(dir, &lv, level_rating(), &br, err) != 0)
        return -1;
    menu_show_until_dismissed(&br, "PC Lemmings", scale);
    menu_free(&br);
    return 0;
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
static int level_frame_tick(void *ctx, uint8_t *pix)
{
    struct level_frame *a = (struct level_frame *)ctx;
    uint16_t w = a->from->w, h = a->from->h;

    memcpy(pix, a->from->pix, (size_t)w * h);
    /* The FIRST frame is the level as it stands before anything moves, and
     * every frame after it is one step on. Stepping before the first draw
     * instead put the whole game one frame ahead of the original: measured,
     * the game's frame N matched the original's N+1 to 28 pixels - the mouse
     * cursor - and its own N to 300. The original's loop at image 0x019C
     * steps and then draws, but its first captured frame at image 0x018F is
     * the one before any of that. */
    if (a->stepped) {
        level_play_step(a->st, &a->lv, a->lem, &a->released,
                        a->from->pix, w, h);
        /* Image 0x001E7 and image 0x001FA: everyone released and none
         * active, or the clock run out. Which of the two it was does not
         * decide win or lose - that is the saved count against the level's
         * requirement. Both go to image 0x002B3, which does not end the
         * loop: it arms the fade and lets it run. */
        if (!g_level_over && level_over(&a->lv, a->released,
                                        lemmings_live(a->lem, a->released)))
            level_end_begin();                  /* image 0x002B3 */
    }
    a->stepped = 1;
    level_play_draw(a->st, &a->lv, a->sec, a->len,
                    a->lem_sec, a->lem_len, a->lem, a->released,
                    pix, w, h, a->view_x);
    return 1;
}


/* THE RESULTS SCREEN, image 0x046EB - the port's window for it.
 *
 * The composition is game.c's; this is the part the hardware used to do:
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
    lem_rgb pal[16];
    level_results_palette(pal);
    if (view_open(MENU_W, MENU_H, "PC Lemmings", scale) != 0) {
        free(pix); dat_free(&mainf); return RESULTS_MENU;
    }

    int32_t out = RESULTS_MENU;
    int released = 0;                     /* image 0x047AF's wait */
    int running = 1, dirty = 1;
    while (running) {
        /* image 0x047A9 reads ds:0x14 and tests it - the button STATE, not
         * an edge - so a screen entered with nothing held passes this at
         * once. Waiting for an UP EVENT instead meant a player had to click
         * once for nothing, to manufacture the release, before any click
         * counted. There is no event to wait for when the button is already
         * up. */
        if (!released) {
            Uint32 held = SDL_GetMouseState(NULL, NULL);
            if (!(held & (SDL_BUTTON_LMASK | SDL_BUTTON_RMASK)))
                released = 1;
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) { out = RESULTS_MENU; running = 0; }
            else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) released = 1;
            else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && released) {
                if (e.button.button == SDL_BUTTON_LEFT) {
                    out = RESULTS_NEXT;   /* image 0x047B4 */
                    running = 0;
                } else if (e.button.button == SDL_BUTTON_RIGHT) {
                    out = RESULTS_MENU;   /* image 0x047B9 */
                    running = 0;
                }
            }
            else if (e.type == SDL_EVENT_KEY_DOWN) {
                /* Image 0x047BE reads ds:0x1FC7 as well, so a key does end
                 * this screen; which keys is not read, and Escape is the one
                 * every other screen here takes. */
                if (e.key.key == SDLK_ESCAPE) { out = RESULTS_MENU; running = 0; }
                else if (released) { out = RESULTS_NEXT; running = 0; }
            }
        }
        /* Nothing on this screen animates - image 0x047A9 polls and draws
         * nothing - so it is uploaded once. */
        if (dirty) {
            void *px = NULL; int pitch = 0;
            if (SDL_LockTexture(g_tex, NULL, &px, &pitch)) {
                for (uint16_t y = 0; y < MENU_H; y++) {
                    uint32_t *row = (uint32_t *)((uint8_t *)px + y * pitch);
                    const uint8_t *src = pix + (size_t)y * MENU_W;
                    for (uint16_t x = 0; x < MENU_W; x++) {
                        lem_rgb q = pal[src[x] & 15];
                        row[x] = ((uint32_t)q.r << 16)
                               | ((uint32_t)q.g << 8) | q.b;
                    }
                }
                SDL_UnlockTexture(g_tex);
            }
            dirty = 0;
        }
        SDL_RenderClear(g_ren);
        SDL_RenderTexture(g_ren, g_tex, NULL, NULL);
        SDL_RenderPresent(g_ren);
        SDL_Delay(10);
    }
    free(pix);
    dat_free(&mainf);
    return out;
}

int level_show(const char *dir, const char *file, uint32_t which,
               level *lv, uint8_t scale, const char **err)
{
    char path[1024];
    dat_file levels, gfx;
    ground g;
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    if (dat_load(path, &levels, err) != 0) return -1;
    if (which < 1 || which > levels.count) {
        dat_free(&levels);
        if (err) *err = "no such level in that container";
        return -1;
    }
    int rc = level_parse(levels.sections[which - 1].data,
                         levels.sections[which - 1].size, lv, err);
    dat_free(&levels);
    if (rc != 0) return -1;
    if (level_apply_odd(dir, lv, level_odd_taken(), err) != 0) return -1;

    snprintf(path, sizeof(path), "%s/ground%uo.dat", dir, lv->graphics_set);
    if (ground_load(path, &g, err) != 0) return -1;
    snprintf(path, sizeof(path), "%s/vgagr%u.dat", dir, lv->graphics_set);
    if (dat_load(path, &gfx, err) != 0) return -1;
    if (gfx.count < 1) { dat_free(&gfx); if (err) *err = "no terrain section";
                         return -1; }

    canvas c, terrain;
    if (canvas_init(&c) != 0 || canvas_init(&terrain) != 0) {
        dat_free(&gfx); if (err) *err = "out of memory"; return -1;
    }
    /* Terrain into a pristine copy. The original never bakes the objects in -
     * it animates them at display time, images 0x07BB8 and 0x07C0A - which is
     * why tools/prove.sh composes terrain alone and matches to the pixel. So
     * the terrain is composed once and each frame starts from it. */
    rc = render_terrain(&terrain, lv, &g, gfx.sections[0].data,
                        gfx.sections[0].size, err);
    if (rc == 0 && gfx.count >= 2) {
        static struct level_frame A;
        static dat_file mainf;
        int have_main = 0;
        A.lv = *lv; A.sec = gfx.sections[1].data; A.len = gfx.sections[1].size;
        A.from = &terrain;
        A.lem_sec = NULL; A.lem_len = 0; A.released = 0;
        A.pan_sec = NULL; A.pan_len = 0;
        A.view_x = lv->start_x;
        A.stepped = 0;
        level_saved_reset();
        level_triggers_set(&A.lv, &g);
        A.panel = calloc((size_t)PANEL_LINE_W * PANEL_ART_H, 1);

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
                }
            }
        }

        level_anim_init(A.st, &A.lv, &g);
        /* One step before the first frame. Measured: without it the exit's
         * flames are one animation frame behind the original's at every
         * frame checked. */
        level_anim_step(A.st, &A.lv);
        /* image 0x04836 reads the level's extended graphics set. */
        music_set_level(&A.lv);
        level_start_reset();
        level_release_reset(&A.lv);
        level_status_reset(&A.lv);
        lemmings_reset(A.lem);

        /* The level gets its own screen - 320x200, play area over panel -
         * rather than view_run_anim's window of the play area alone. */
        g_show_tick = level_frame_tick; g_show_ctx = &A; g_show_pix = c.pix;
        level_run(&A, &c, g.palette, "PC Lemmings", scale);
        g_show_tick = NULL; g_show_ctx = NULL; g_show_pix = NULL;
        /* ds:0x40 through its own routine, image 0x03AF4 - the panel writes
         * it every frame and this is the last one. The port kept a second
         * copy in a global of its own, which main.c then read instead.
         *
         * ds:0x4F needs nothing here any more: image 0x002B3 sets it inside
         * the loop, which is where the original sets it. */
        level_status_in(level_saved_percent(&A.lv));
        free(A.panel); A.panel = NULL;
        if (have_main) dat_free(&mainf);
    } else if (rc == 0) {
        view_run(&terrain, g.palette, "PC Lemmings", lv->start_x, scale);
    }
    canvas_free(&c);
    canvas_free(&terrain);
    dat_free(&gfx);
    return rc;
}
