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

int32_t view_interact_anim(uint8_t *pix, uint16_t w, uint16_t h,
                           const lem_rgb *palette, const char *title,
                           uint8_t scale, const view_hotspot *spots,
                           uint8_t nspots, view_tick tick, void *ctx)
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
    uint8_t      selected;   /* which button's box is drawn */
    uint8_t      skill;      /* ds:0x82 - the one a click would hand out */
    uint8_t      over;       /* image 0x001C8 said the level has finished */
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
        /* Image 0x03170: a skill is selected, so find what is under the
         * pointer and give it one. */
        /* Image 0x0320D opens by testing the count and image 0x03224
         * spends it. The count lives in game.c now, because the PANEL has
         * to draw the same number the assignment reads - it was drawing the
         * level header's instead, which never moves. */
        if (A->skill == SKILL_DIGGER && level_skill_left(SKILL_DIGGER)) {
            int16_t k = level_lemming_at(A->lem, A->released, A->view_x);
            if (k >= 0 && lem_dig_start(&A->lem[k]))
                level_skill_spend(SKILL_DIGGER);
        }
    }
    else if (h.what == LEVEL_HIT_BUTTON) {
        /* The highlight sits one slot on from the number it belongs to -
         * PANEL_SEL_X0 is 16 where the numbers start at PANEL_ART_X, 32 -
         * so the box for button k is drawn at k + 1. */
        A->selected = (uint8_t)(h.button + 1);
        A->skill = h.button;              /* image 0x03F66: ds:0x82 */
    }
}

static int level_run(struct level_frame *A, const canvas *c,
                     const lem_rgb *palette, const char *title, uint8_t scale)
{
    if (scale < 1) scale = 1;
    if (view_open(LEVEL_SCREEN_W, LEVEL_SCREEN_H, title, scale) != 0)
        return -1;
    SDL_Renderer *ren = g_ren;
    SDL_Texture  *tex = g_tex;

    int32_t max_x = (int32_t)c->w - LEVEL_SCREEN_W;
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
     * The level runs at LEVEL_FPS - seventeen frames a second, image 0x0418's
     * own number, which is what makes 5,100 frames exactly the five minutes
     * of the expiration timer. This loop used to sleep 16 milliseconds a
     * frame, which is sixty, so the game ran three and a half times too fast
     * and the clock visibly fell down its own display.
     *
     * The deadline is absolute rather than a sleep per frame: a sleep is at
     * the mercy of the host scheduler and of however long the frame took to
     * compose, which would make the game's speed a property of the machine
     * rather than of the game. Falling behind resets the deadline instead of
     * building a debt the loop would then sprint to repay. */
    const Uint64 period = SDL_NS_PER_SECOND / LEVEL_FPS;
    Uint64 due = SDL_GetTicksNS();

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
                level_click(A, &scroll_x);
            }
            /* SCRIPT_UP: the port acts on the press, as image 0x03EF9 does. */
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
                    level_click(A, &scroll_x);
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
                switch (e.key.key) {
                case SDLK_ESCAPE: case SDLK_Q: running = 0;               break;
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
        if (g_show_tick && g_show_pix) g_show_tick(g_show_ctx, g_show_pix);
        if (A->panel && A->pan_sec) {
            memset(A->panel, 0, (size_t)PANEL_LINE_W * PANEL_ART_H);
            level_panel_build(A->panel, A->pan_sec, A->pan_len, &A->lv,
                              A->from->pix, A->from->w, A->from->h,
                              A->lem, A->released, A->view_x,
                              A->selected);
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

        void *pixels = NULL;
        int pitch = 0;
        if (SDL_LockTexture(tex, NULL, &pixels, &pitch)) {
            for (uint16_t y = 0; y < LEVEL_SCREEN_H; y++) {
                uint32_t *row = (uint32_t *)((uint8_t *)pixels + y * pitch);
                const uint8_t *src = screen + (size_t)y * LEVEL_SCREEN_W;
                const lem_rgb *pal = (y < LEVEL_PLAY_H) ? top : bottom;
                for (uint16_t x = 0; x < LEVEL_SCREEN_W; x++) {
                    lem_rgb q = pal[src[x]];
                    row[x] = ((uint32_t)q.r << 16) |
                             ((uint32_t)q.g << 8) | q.b;
                }
            }
            SDL_UnlockTexture(tex);
        }
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
            fprintf(statef, "{\"frame\":%ld,\"lem\":[", frame_no);
            for (uint8_t k = 0; k < A->released && k < LEMMING_MAX; k++) {
                const lemming *a = &A->lem[k];
                fprintf(statef,
                        "%s{\"x\":%d,\"y\":%d,\"hot_x\":%d,\"hot_y\":%d,"
                        "\"flags\":%u,\"anim\":%u,\"alive\":%u}",
                        k ? "," : "", a->x, a->y, a->hot_x, a->hot_y,
                        (unsigned)a->flags, (unsigned)a->anim,
                        (unsigned)a->alive);
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
        if (A->over) running = 0;      /* image 0x001C8 */
    }
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
    view_interact_anim(m->pix, MENU_W, MENU_H, g_fade_cur, title, scale,
                       any, 2, fade_tick, NULL);

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

int level_show_briefing(const char *dir, const char *file, uint32_t which,
                        uint8_t rating, uint16_t number, uint8_t scale,
                        const char **err)
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
    lv.level_number = number;

    menu_screen br;
    if (menu_briefing_screen(dir, &lv, rating, &br, err) != 0) return -1;
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
        /* Image 0x001C8: everyone released and none active, or the clock
         * run out. Which of the two it was does not decide win or lose -
         * that is the saved count against the level's requirement. */
        if (level_over(&a->lv, a->released,
                       lemmings_live(a->lem, a->released)))
            a->over = 1;
    }
    a->stepped = 1;
    level_play_draw(a->st, &a->lv, a->sec, a->len,
                    a->lem_sec, a->lem_len, a->lem, a->released,
                    pix, w, h);
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
int level_show_results(const char *dir, uint8_t saved, uint8_t needed,
                       int timed_out, uint8_t level_no, const char *code,
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
    level_results_draw(pix, MENU_W, MENU_H,
                       mainf.sections[RESULTS_FONT_SECTION].data,
                       mainf.sections[RESULTS_FONT_SECTION].size,
                       bg, bg_len,
                       saved, needed, timed_out, lem_input_kind,
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

/* HOW THE LAST LEVEL ENDED, for the caller. The original keeps these in
 * ds:0x40 and ds:0x4F; the port has no DGROUP, so they live here beside the
 * loop that fills them. */
uint8_t g_level_saved;
uint8_t g_level_over;

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
        A.selected = PANEL_SEL_BUTTON;
        A.skill = 0;
        A.over = 0;
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
        level_start_reset();
        level_release_reset(&A.lv);
        level_status_reset(&A.lv);
        lemmings_reset(A.lem);

        /* The level gets its own screen - 320x200, play area over panel -
         * rather than view_run_anim's window of the play area alone. */
        g_show_tick = level_frame_tick; g_show_ctx = &A; g_show_pix = c.pix;
        level_run(&A, &c, g.palette, "PC Lemmings", scale);
        g_show_tick = NULL; g_show_ctx = NULL; g_show_pix = NULL;
        /* How it ended, for the caller: image 0x001C8 sets A.over, and the
         * rescued count against the level's requirement is what image
         * 0x04720 then judges. */
        g_level_saved = level_saved_percent(&A.lv);
        g_level_over  = A.over;
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
