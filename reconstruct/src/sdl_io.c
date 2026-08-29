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
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#include "game.h"
#include "data.h"
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
    /* A NOTCH EVERY THIRD PASS, not every pass. Image 0x055B7 waits three
     * retraces between notches - the port stepped once a retrace and faded
     * three times too fast on every screen that fades. The loop above runs
     * at one pass per retrace, so the counter here is what makes the rate
     * the original's rather than the display's. */
    static uint8_t wait;
    (void)ctx; (void)pix;
    if (wait) { wait--; return 0; }
    wait = (uint8_t)(palette_fade_retraces() - 1);   /* image 0x055EE */
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

/* ======================================================================
 * THERE IS ONE WAY OUT, AND IT IS HERE.
 *
 * The original owns the machine: its screens end when the game says so, and
 * there is no window to close and no shell to interrupt. So none of the
 * loops transcribed from it has an escape - image 0x04C7E leaves on Enter
 * AND NOTHING ELSE, and image 0x0445B only on one of seven keys. Faithful,
 * and unusable the moment the host wants the process back.
 *
 * That shipped: pressing F2 entered image 0x04967's screen and the only exit
 * was Enter on a valid code. SDL turns Ctrl+C into an SDL_EVENT_QUIT, and
 * the entry loop dropped it along with every other key outside A-Z, so the
 * process had to be killed.
 *
 * Threading a "has it closed" flag through every transcribed loop is the
 * wrong shape - it puts a host concern inside routines that are supposed to
 * read as the game. The window going away is IO, so it ends the process
 * here, and no loop above needs to know it can happen.
 *
 * The signal handler calls _exit rather than this: almost nothing in SDL is
 * safe to call from a signal, and the compositor reclaims a window from a
 * process that is gone.
 * ====================================================================== */
void view_quit_now(void)
{
    /* Give the pointer back, then GO. Not exit(): view_shutdown is
     * registered with atexit and tears the same window, renderer and
     * SDL down again, which segfaulted on the close box - one teardown
     * running over another's freed handles. _exit skips it, and the
     * compositor reclaims a window from a process that is gone, which is
     * what the signal path has always relied on. */
    if (g_win) SDL_SetWindowRelativeMouseMode(g_win, false);
    _exit(0);
}

static void on_signal(int sig)
{
    (void)sig;
    _exit(130);                        /* 128 + SIGINT, as a shell reports it */
}

/* Returns 0 on success. Reuses what is already open when it fits. */
int view_open(uint16_t w, uint16_t h, const char *title, uint8_t scale)
{
    static int registered;
    static int signals_up;
    if (!signals_up) {
        signals_up = 1;
        signal(SIGINT, on_signal);
        signal(SIGTERM, on_signal);
    }
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
/* A key press is an EVENT; ds:0x1FC7 was a state. Latched here so
 * view_buttons can report it the way the byte would have. */
static uint16_t g_key_button;

/* WHAT THE HARDWARE USED TO DO. The composed buffer the window shows, and the
 * routine that fills it each pass. game.c's level_show aims these at its own
 * frame composer; the original had no such indirection because it wrote
 * straight into the page the CRTC was already pointing at. */
void view_show_hook(view_tick tick, void *ctx, uint8_t *pix)
{
    g_show_tick = tick; g_show_ctx = ctx; g_show_pix = pix;
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
static uint8_t scancode_of(SDL_Keycode k);


/* Exported for the test in tests/screens.c: every letter a player can type
 * has to reach the entry loop as the character the game's own table names,
 * and for a long time none of them did. */
uint8_t view_scancode_of(int32_t keycode)
{
    return scancode_of((SDL_Keycode)keycode);
}

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
    default:             break;
    }

    /* THE LETTERS, BACKSPACE AND ENTER, and they are not listed here.
     *
     * The eleven above are the menu's, and for a long time they were all the
     * port had - so image 0x04C7E's entry loop, which takes A-Z, got 0xFF for
     * every letter typed and correctly ignored it. Nothing appeared on
     * screen, which is what a player reported.
     *
     * Rather than write out a scancode set from memory, this INVERTS the
     * game's own table. MENU_XLAT is ds:0x202E, the xlat image 0x04D4B reads,
     * and it maps scancode -> character; so the scancode for a character is
     * the index that holds it. Derived this way the two cannot disagree, and
     * a wrong entry would show up as a key that does nothing rather than as a
     * key that means something else.
     *
     * Index 0 is skipped: it is not a key, and it holds 'L' - which would
     * otherwise win over the L key at 0x26. */
    {
        uint8_t want;
        if (k >= SDLK_A && k <= SDLK_Z)
            want = (uint8_t)('A' + (k - SDLK_A));
        else if (k == SDLK_BACKSPACE)  want = 0x7F;
        else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) want = 0x0D;
        else return 0xFF;
        for (uint8_t i = 1; i < 0x53; i++)
            if (MENU_XLAT[i] == want)
                return i;
    }
    return 0xFF;   /* past the table: image 0x04D1A's scan would not have
                    * found it */
}

/* A screen with nothing on it to choose takes Esc, and a click. */
/* IMAGE 0x04683's THREE ANSWERS, kept apart. A mouse button and ds:0x1FC7's
 * 0x40 both go to image 0x046A2 and play; its 0x20 goes to image 0x0469B,
 * which pops the stack and jumps to image 0x04401 - the MENU. This used to
 * return 1 for Esc and take 1 as the click id too, so the caller could not
 * tell them apart and every briefing played. */
static int32_t dismiss_input(uint8_t scancode)
{
    return menu_key_code(scancode) == 0x1B ? BRIEF_MENU : -1;
}

/* WHAT THE HARDWARE USED TO DO for a screen that waits: the page, and the
 * event queue behind ds:0x14 and ds:0x1FC7.
 *
 * `hit` comes back as whatever `input` answered for a key, or `click_id` for
 * a button - image 0x04473 tests ds:0x14's low three bits and jumps without
 * once looking at where the pointer was. VIEW_CLOSED means nothing happened.
 *
 * The split palette is the briefing's: two pictures stacked, and the DAC is
 * reprogrammed part-way down. That is the card's, so it is here. */
int32_t view_screen_pump(view_input input, int32_t click_id)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        int32_t hit = VIEW_CLOSED;
        if (e.type == SDL_EVENT_QUIT) view_quit_now();
        else if (e.type == SDL_EVENT_KEY_DOWN) {
            if (input) hit = input(scancode_of(e.key.key));
        } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            hit = click_id;
        }
        if (hit != VIEW_CLOSED) return hit;
    }
    return VIEW_CLOSED;
}

void view_screen_present(const uint8_t *pix, uint16_t w, uint16_t h,
                         const lem_rgb *palette)
{
    void *pixels = NULL;
    int pitch = 0;
    if (SDL_LockTexture(g_tex, NULL, &pixels, &pitch)) {
        for (uint16_t y = 0; y < h; y++) {
            uint32_t *row = (uint32_t *)((uint8_t *)pixels + y * pitch);
            const uint8_t *src = pix + (size_t)y * w;
            const lem_rgb *pl = (g_split && y >= g_split && g_split_pal)
                                    ? g_split_pal : palette;
            for (uint16_t x = 0; x < w; x++) {
                lem_rgb c = pl[src[x]];
                row[x] = ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
            }
        }
        SDL_UnlockTexture(g_tex);
    }
    SDL_RenderClear(g_ren);
    SDL_RenderTexture(g_ren, g_tex, NULL, NULL);
    SDL_RenderPresent(g_ren);
}

/* One pass per retrace, on an ABSOLUTE deadline - a sleep per frame is at the
 * mercy of the host scheduler, and falling behind must reset the deadline
 * rather than build a debt the loop then sprints to repay. */
void view_screen_pace(void)
{
    /* THE AUDIO DEVICE IS FED HERE, on every screen that waits. It used to be
     * fed inside view_show, the scrollable viewer that was deleted as
     * unreachable - and the briefing reached it through view_run_anim, so
     * deleting it left the briefing's loop with nothing topping the queue up.
     * A screen that waits is exactly where a queue drains, so the pump
     * belongs with the pacing rather than in one caller. */
    sound_pump();

    static Uint64 due;
    Uint64 period = 1000000000ull / VIEW_REFRESH_HZ;
    Uint64 now = SDL_GetTicksNS();
    if (due == 0 || now > due + period * 8) due = now;
    due += period;
    if (now < due) SDL_DelayNS(due - now);
    else due = now;
}






/* One press of the left button, wherever the pointer is. A scripted click
 * and a real one go through this same function - written out twice, the two
 * would differ, which is the fault that had the window path showing an empty
 * cave while every harness frame matched. */

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
    /* A MISSING WINDOW IS NOT A QUIET FRAME. This returned silently when the
     * handles were NULL, so a bug that left them unset showed as a game that
     * drew nothing and said nothing - found by playing, not by any check
     * here, because the frame sink writes the composed buffer and never comes
     * through this function at all. Now it says so, once. */
    if (!g_lvl_tex || !g_lvl_screen) {
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "  [video] the level has no window to draw into "
                            "- nothing will be shown\n");
        }
        return;
    }
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
/* IMAGE 0x01550 - and it is what SETS THE RATE for everything that waits on
 * it. The menu loop calls it once a pass (image 0x0445B) and the level's
 * fade calls it once a notch, so both run at the adapter's 70 Hz because
 * that is what the retrace is, not because a constant here says so.
 *
 * Paced on an ABSOLUTE deadline rather than a sleep per pass, for the reason
 * CLAUDE.md gives: a sleep is at the mercy of the host scheduler and of how
 * long the pass took to compose, which would make the game's speed a
 * property of the machine. Falling behind resets the deadline rather than
 * building a debt the loop then sprints to repay.
 *
 * With no window there is nothing to wait for and it returns at once - a
 * harness with a frame budget must not spend wall clock on a retrace that is
 * not happening. */
void video_retrace_wait(void)
{
    static Uint64 due;
    Uint64 now;
    if (!g_win) return;
    now = SDL_GetTicksNS();
    if (due == 0 || now > due + SDL_NS_PER_SECOND) due = now;
    due += SDL_NS_PER_SECOND / VIEW_REFRESH_HZ;
    if (now < due) SDL_DelayNS(due - now);
    else due = now;
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
        if (e.type == SDL_EVENT_QUIT) view_quit_now();
}

/* LEVEL_RUN'S OWN STATE. These were its locals; the loop that used them is
 * the game's and has moved to game.c, and threading a dozen of them through
 * two calls would have put the window's bookkeeping into the game's
 * signature. They sit with the code that is genuinely theirs. */
static const char *g_lv_sink;
static FILE       *g_lv_statef;
static long        g_lv_frame_no;
static lem_rgb     g_lv_top[16];
static Uint64      g_lv_due, g_lv_period;
static int         g_lv_seen_motion, g_lv_grabbed, g_lv_held;
static uint32_t    g_lv_script_at;

/* WHAT THE HARDWARE USED TO DO for a level's frame: ds:0x10, ds:0x12 and
 * ds:0x14, which the mouse driver kept current, and ds:0x1FD0, where the
 * keyboard handler left a scancode. Here they come from an event queue and
 * from a recorded script.
 *
 * The ROUTINES that read them are game.c's - image 0x03EF9 for the panel,
 * image 0x03150 for the play area, image 0x04085 for the keys and the edge.
 *
 * Returns 1 once the pointer has really moved, 0 before that, and -1 when the
 * window asks to stop. That last is the PORT'S OWN exit: the original has no
 * way to abandon a level without a result, and a window does.
 */
int view_level_input(struct level_frame *A, int32_t *scroll_x_p,
                     int32_t max_x)
{
    int32_t scroll_x = *scroll_x_p;
    /* The script speaks first, so a scripted frame behaves the same
     * whether or not the host had an event waiting. */
    while (g_lv_script_at < harness_script_n && harness_script[g_lv_script_at].frame <= g_lv_frame_no) {
        const script_act *act = &harness_script[g_lv_script_at++];
        if (act->what == SCRIPT_MOVE) {
            /* The script is in SCREEN pixels; the game's own pointer sits
             * eight further on, which is where image 0x0426D clamps. */
            lem_mouse_move(act->x + 8, act->y + 8);
            g_lv_seen_motion = 1;
        } else if (act->what == SCRIPT_DOWN) {
            g_lv_held = 1;
            level_panel_click(A, &scroll_x);
        } else if (act->what == SCRIPT_UP) {
            g_lv_held = 0;
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
    if (g_lv_held) {
        level_hit rh = level_pointer_hit();
        if (rh.what == LEVEL_HIT_BUTTON && rh.button < 2)
            level_rate_click(rh.button);    /* image 0x04032 */
    }

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) view_quit_now();
        else if (e.type == SDL_EVENT_MOUSE_MOTION) {
            g_lv_seen_motion = 1;
            if (g_lv_grabbed) {
                /* RELATIVE motion while the pointer is g_lv_held, which is
                 * what the original works in: it asks the driver for
                 * mickeys and sets the mickey-to-pixel ratio itself at
                 * image 0x00F20. An absolute position would also stop at
                 * the edge of the desktop rather than at the edge of the
                 * game's own range. */
                lem_mouse_move((int32_t)lem_mouse_x + (int32_t)e.motion.xrel,
                               (int32_t)lem_mouse_y + (int32_t)e.motion.yrel);
            } else {
                int ow = 0, oh = 0;
                SDL_GetRenderOutputSize(g_lvl_ren, &ow, &oh);
                if (ow > 0 && oh > 0)
                    lem_mouse_move(
                        (int32_t)(e.motion.x * LEVEL_SCREEN_W / (float)ow) + 8,
                        (int32_t)(e.motion.y * LEVEL_SCREEN_H / (float)oh) + 8);
            }
        }
        else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            if (!g_lv_grabbed) {
                /* Click to take it back, the way every DOS front end
                 * does. That click is for the window, not the game. */
                g_lv_grabbed = 1;
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
                    g_lv_held = 1;
                    level_panel_click(A, &scroll_x);      /* image 0x03168 */
                }
            }
        }
        else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            if (e.button.button == SDL_BUTTON_LEFT) {
                lem_buttons &= (uint8_t)~1u;
                g_lv_held = 0;
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
                g_lv_grabbed = 0;
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
            case SDLK_Q:      return -1;                            break;
            case SDLK_LEFT:  scroll_x -= LEVEL_SCROLL * 2;            break;
            case SDLK_RIGHT: scroll_x += LEVEL_SCROLL * 2;            break;
            case SDLK_HOME:  scroll_x = 0;                            break;
            case SDLK_END:   scroll_x = max_x;                        break;
            default: break;
            }
        }
    }
    *scroll_x_p = scroll_x;
    sound_pump();          /* the audio device, not the frame */
    return g_lv_seen_motion;
}

/* WHAT THE HARDWARE USED TO DO once a frame is composed: image 0x00105 hands
 * the CRTC the other page and image 0x00164 spins until the retrace ISR has
 * counted five. Here that is a texture upload and an absolute deadline - a
 * sleep per frame would make the game's speed a property of the machine.
 *
 * The frame g_lv_sink and the state file are the port's, and env-gated: the game
 * has no command line and writes no files.
 */
void view_level_show(const uint8_t *screen, struct level_frame *A)
{
    level_screen_upload();
    if (g_lv_sink) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/f%05ld.raw", g_lv_sink, g_lv_frame_no);
        FILE *f = fopen(path, "wb");
        if (f) {
            uint16_t wh[2] = { LEVEL_SCREEN_W, LEVEL_SCREEN_H };
            fwrite("LEMRW2", 1, 6, f);
            fwrite(wh, sizeof(uint16_t), 2, f);
            /* The TOP palette: the container holds one, and the play
             * area is what gets scored against the original's own
             * frames. The split is in the comment above, not in here. */
            for (uint8_t i = 0; i < 16; i++) {
                fputc(g_lv_top[i].r, f);
                fputc(g_lv_top[i].g, f);
                fputc(g_lv_top[i].b, f);
            }
            fwrite(screen, 1, LEVEL_SCREEN_W * LEVEL_SCREEN_H, f);
            fclose(f);
        }
    }
    if (g_lv_statef) {
        fprintf(g_lv_statef, "{\"frame\":%ld,\"released\":%u,\"live\":%u,"
                "\"saved\":%u,\"over\":%u,\"tick\":%u,\"view_x\":%d,\"lem\":[", g_lv_frame_no,
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
            fprintf(g_lv_statef,
                    "%s{\"x\":%d,\"y\":%d,\"hot_x\":%d,\"hot_y\":%d,"
                    "\"flags\":%u,\"anim\":%u,\"alive\":%u,"
                    "\"fuse\":%u,\"dir\":%d,\"state\":%u}",
                    k ? "," : "", a->x, a->y, a->hot_x, a->hot_y,
                    (unsigned)a->flags, (unsigned)a->anim,
                    (unsigned)a->alive, (unsigned)a->fuse,
                    (int)a->dir, (unsigned)a->state);
        }
        fprintf(g_lv_statef, "]}\n");
    }
    g_lv_frame_no++;

    SDL_RenderClear(g_lvl_ren);
    SDL_RenderTexture(g_lvl_ren, g_lvl_tex, NULL, NULL);
    SDL_RenderPresent(g_lvl_ren);
    if (!g_lv_sink) {
        g_lv_due += g_lv_period;
        Uint64 now = SDL_GetTicksNS();
        if (now < g_lv_due) SDL_DelayNS(g_lv_due - now);
        else g_lv_due = now;          /* behind: start again from here */
    }
}

int level_run(struct level_frame *A, const canvas *c,
                     const lem_rgb *palette, const char *title, uint8_t scale)
{
    if (scale < 1) scale = 1;
    if (view_open(LEVEL_SCREEN_W, LEVEL_SCREEN_H, title, scale) != 0)
        return -1;

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
    /* THE WINDOW'S HANDLES, from the ones view_open just made. These read
     * `g_lvl_tex = g_lvl_tex;` for a while: a scoped rename turned the two
     * locals into these globals and deleted the lines that had initialised
     * them, so both stayed NULL, level_screen_upload returned at its own
     * guard and the game showed nothing while the sound played on. */
    g_lvl_screen = screen;
    g_lvl_tex = g_tex;
    g_lvl_ren = g_ren;

    /* Two palettes, and which one a row gets depends on where it is.
     *
     * The game swaps attributes 8..15 part-way down the screen - see
     * level_panel_palette - so the play area keeps the terrain's browns and
     * the panel below gets greens and greys. On a plain PC it does not, and
     * the panel makes do with the level's palette; that is what lem_machine
     * selects. The lower eight entries are the same either way. */
    lem_rgb bottom[16];
    for (uint8_t i = 0; i < 16; i++) g_lv_top[i] = bottom[i] = palette[i];
    if (lem_machine == LEM_PC_HIGH)
        level_panel_palette(&bottom[8]);
    /* And the fade's copy of them: `g_lv_top` and `bottom` stay the level's own
     * colours - the capture below writes them, and a capture must never
     * hold a mid-fade palette - while the pair the screen is drawn through
     * starts at black and climbs. */
    memcpy(g_lvl_tgt_top, g_lv_top, sizeof g_lv_top);
    memcpy(g_lvl_tgt_bottom, bottom, sizeof bottom);

    /* A frame g_lv_sink, for checking THIS path rather than a second arrangement
     * of the same calls. Every measurement of the level goes through lemdev,
     * which composes the canvas and the panel separately; this writes the
     * screen the game actually puts on the display, in the same 'LEMRW2'
     * container, so tools/compare_frame.py can score it against the
     * original's own 320x200 frames.
     *
     * Env-gated and in the IO file on purpose: the game has no command line
     * and writes no files. Setting LEM_SCREEN_DIR is a developer asking for
     * one, the way LEM_FRAMES already asks for a frame budget. */
    g_lv_sink = SDL_getenv("LEM_SCREEN_DIR");
    /* ds:0x200E. A capture harness is not a display: it has a frame budget,
     * and a fade g_lv_held inside frame 0 would spend a second of wall clock
     * producing no frames while every frame it did produce carried a
     * palette climbing out of black. */
    g_lvl_live = (!g_lv_sink && g_win) ? 1 : 0;
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
    g_lv_statef = state ? fopen(state, "w") : NULL;
    /* A recorded input, so a comparison can cover the parts of the game that
     * need a player - see script_load. */
    const char *script = SDL_getenv("LEM_INPUT_SCRIPT");
    if (script) harness_script_load(script);
    g_lv_script_at = 0;
    g_lv_frame_no = 0;
    /* Edge scrolling waits for a pointer that has actually MOVED. A polled
     * position cannot tell "at the left edge" from "no pointer here": under
     * the dummy video driver it reads (0,0) and reports this window as
     * focused, so the view walked off the left of the level at 8 pixels a
     * frame and every headless frame after 78 was black. */
    g_lv_seen_motion = 0;

    /* The pointer is HELD by the window, and Ctrl+Alt gives it back.
     *
     * The original owns the mouse completely - it hides the driver's pointer
     * at image 0x00F43, sets its own range at images 0x00F2B and 0x00F36 and
     * draws its own cross-hair - and a window that lets the host pointer walk
     * out of it while the game still thinks it is moving does not reproduce
     * that. Held, the two agree; released, the game keeps the last position. */
    /* THE GLOBAL, not a local of the same name. This read `int g_lv_grabbed
     * = 1;` for a while - a scoped rename turned a local into a shadow of the
     * global view_level_input reads - so level_run grabbed the mouse while
     * that function still believed it had not, and the FIRST CLICK of every
     * level was eaten to "take the pointer back" from a window that already
     * had it. -Wshadow named the line; I was grepping the build for `error`. */
    g_lv_grabbed = 1;
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
    g_lv_period = (Uint64)(SDL_NS_PER_SECOND / LEVEL_FRAME_HZ);
    g_lv_due = SDL_GetTicksNS();

    g_lv_held = 0;          /* the button is down - a rate button repeats */
    level_loop(A, c, screen, &scroll_x, max_x, budget);


    /* No fade here. It is image 0x0031E's, it runs INSIDE the loop above,
     * and it was written out a second time in this file for as long as the
     * loop exited the moment the level ended - which meant the picture
     * being faded was one nothing was still drawing. See level_end_begin. */
    g_lvl_screen = NULL; g_lvl_tex = NULL; g_lvl_ren = NULL;

    /* Never leave the pointer captured by a window that is going away. */
    SDL_SetWindowRelativeMouseMode(g_win, false);
    if (g_lv_statef) fclose(g_lv_statef);
    free(screen);
    return 0;
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
    case 2:
        adlib_stop();                       /* driver +0x003A */
        /* AND THROW AWAY WHAT IS ALREADY QUEUED. This is the port's own,
         * and it is IO: the original's OPL is real time, so keying it off
         * is silent in the same instant. This port generates audio ahead of
         * the game and keeps SND_TARGET_MS of it in SDL - a fifth of a
         * second, which at a level's 17 frames a second is over three frames
         * - so without this the driver falls silent and the BUFFER plays on.
         * A player reported it as a level's music running into the next one,
         * and image 0x002FE is the stop that was already being made
         * correctly: the fault was entirely downstream of the driver.
         *
         * Only on AH=2. That is the one request that means SILENCE; doing it
         * on an effect or a tune change would cut whatever is still sounding
         * and the original does not. */
        if (g_snd_stream) SDL_ClearAudioStream(g_snd_stream);
        break;
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
/* HOW FAR AHEAD OF THE GAME THE AUDIO RUNS, and it is a LATENCY: an effect
 * asked for on the frame of a click is rendered behind whatever is already
 * queued, so this number is how late the click sounds. The original has none
 * of it - image 0x0061's driver writes the OPL and the card sounds at once.
 *
 * 200 was safe and audible as a delay; a player heard it on every click in
 * the play area. THE FLOOR IS THE FRAME RATE: sound_pump runs once a level
 * frame, so the queue has to cover 1000/17 = 59ms plus whatever a late frame
 * costs. 120 is two frames - one may arrive a whole frame late without a gap
 * - and halves what the delay was.
 *
 * Going below one frame needs SDL to PULL instead, through an audio stream
 * callback, so the queue is topped up on the device's clock rather than the
 * game's. That is the way to make it inaudible and it is not done here. */
#define SND_TARGET_MS 120
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
    /* Never leave the page flip pointing at a buffer that has gone. */
    if (video_screen_is(m->pix)) video_screen_set(NULL, NULL);
    free(m->pix);
    m->pix = NULL;
    free(m->gfx);
    m->gfx = NULL;
    m->gfx_len = 0;
    free(m->menu_sec);
    m->menu_sec = NULL;
    m->menu_sec_len = 0;
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
            if (e.type == SDL_EVENT_QUIT) view_quit_now();
        int moved = palette_fade_step(g_fade_cur, g_fade_tgt, 16);
        if (g_split)
            moved |= palette_fade_step(g_fade_low_cur, g_fade_low_tgt, 16);
        /* Image 0x055EE: three retraces to a notch, the same as fade_tick. */
        for (uint8_t k = 1; k < palette_fade_retraces(); k++)
            video_retrace_wait();

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

int32_t menu_show_until_dismissed(const menu_screen *m, const char *title,
                                  uint8_t scale)
{
    int32_t how;

    /* The briefing is two pictures stacked with different palettes - see
     * menu_screen.split. Every other screen leaves split at zero. */
    g_split = m->split;

    /* UP FROM BLACK - IMAGE 0x045EF, and it had no address on it until the
     * question was put directly:
     *
     *     0x045EF  call 0x04DEF     the page cleared
     *     0x045F2  cx = 0x10, dx = 0x10   reloaded every pass, they are
     *                              image 0x05329's arguments
     *     0x045F8  call 0x05329     one notch of both palettes
     *     0x045FB  jne 0x045F2      while it still moved
     *     0x045FD  al = 1
     *     0x04600  call 0x053FE     show slot 1, all sixteen
     *
     * palette_fade_step is image 0x05329's notch and carries that address;
     * what follows is the LOOP around it, which is this routine's. */
    for (uint8_t i = 0; i < 16; i++) {
        g_fade_tgt[i] = m->palette[i];
        g_fade_low_tgt[i] = m->palette_low[i];
        g_fade_cur[i].r = g_fade_cur[i].g = g_fade_cur[i].b = 0;
        g_fade_low_cur[i].r = g_fade_low_cur[i].g = g_fade_low_cur[i].b = 0;
    }
    g_split_pal = m->split ? g_fade_low_cur : NULL;
    /* Esc or a click; the briefing says "Press mouse button to continue".
     * The LOOP is image 0x04660's and lives in game.c - see menu_screen_wait;
     * what is left here is opening the window it draws into. */
    if (scale < 1) scale = 1;
    if (view_open(MENU_W, MENU_H, title, scale) != 0) return VIEW_CLOSED;
    {
        const char *cap = SDL_getenv("LEM_FRAMES");
        how = menu_screen_wait(m->pix, MENU_W, MENU_H, g_fade_cur,
                               dismiss_input, BRIEF_PLAY, fade_tick, NULL,
                               cap ? SDL_atoi(cap) : 0);
    }

    /* AND BACK DOWN - IMAGE 0x046A2, the same shape inverted:
     *
     *     0x046A2  call 0x046B9     a retrace
     *     0x046A5  al = 0
     *     0x046A8  call 0x053CA     show slot 0's UPPER EIGHT
     *     0x046AB  cx = 0x10, dx = 0x10
     *     0x046B1  call 0x05329     one notch
     *     0x046B4  jne 0x046A2      while it still moved
     *     0x046B6  jmp 0x04DEF      and the page cleared behind it
     *
     * The original fades the screen
     * away before it draws the next one; cutting straight to the level is
     * the thing that looks wrong even when every pixel is right. */
    for (uint8_t i = 0; i < 16; i++) {
        g_fade_tgt[i].r = g_fade_tgt[i].g = g_fade_tgt[i].b = 0;
        g_fade_low_tgt[i].r = g_fade_low_tgt[i].g = g_fade_low_tgt[i].b = 0;
    }
    view_fade_out(m->pix, MENU_W, MENU_H, title, scale);

    g_split = 0;
    g_split_pal = NULL;
    /* image 0x046A2 fades out on BOTH answers - the one that plays and the
     * one that goes back - so the fade is here and the answer is returned. */
    return how;
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
/* ======================================================================
 * WHAT IMAGE 0x0445B NEEDS FROM THE HARDWARE.
 *
 * The loop itself is game.c's now. These are its four outward calls, and
 * every one of them replaces something a card or a BIOS did.
 *
 * THE KEY QUEUE is the one that is not a straight substitution. The original
 * polls once a pass - image 0x04D1A reads the BIOS buffer, which has held
 * anything typed since the last look - and SDL delivers presses as events
 * whenever it feels like it. Draining the events into a ring and letting the
 * poll take one a pass is what makes those the same thing: a press that
 * arrives between two polls is HELD rather than lost, which is exactly what
 * the BIOS buffer does for the original.
 *
 * It is a ring rather than a single slot on purpose. One slot would drop the
 * second of two presses in a pass, and the loop deliberately takes only one
 * a pass - the plate guards at image 0x044B3 and image 0x044EC refuse a
 * second change in consecutive passes anyway, so a dropped press and a
 * refused one would look identical and only one of them is the game.
 * ====================================================================== */
static uint8_t  *g_menu_pix;
static lem_rgb  *g_menu_pal;

/* WHICH BUFFER THE PAGE FLIP PRESENTS. The original points the CRTC at
 * whatever it has just composed; here that is a pointer, and the code screen
 * is a DIFFERENT one from the menu's. Without this the flip went on showing
 * the menu while the code screen was being typed into - which is exactly
 * "nothing changes on screen when I type". */
void video_screen_set(uint8_t *pix, lem_rgb *pal)
{
    g_menu_pix = pix;
    g_menu_pal = pal;
}

int video_screen_is(const uint8_t *pix) { return g_menu_pix == pix; }
static uint8_t   g_key_ring[32];
static uint8_t   g_key_head, g_key_tail;
static uint16_t  g_menu_buttons;      /* ds:0x14 */

static void menu_pump_events(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) {
            view_quit_now();
        } else if (e.type == SDL_EVENT_KEY_DOWN) {
            uint8_t next = (uint8_t)((g_key_tail + 1) % sizeof g_key_ring);
            if (next != g_key_head) {         /* full: drop, do not wrap */
                g_key_ring[g_key_tail] = scancode_of(e.key.key);
                g_key_tail = next;
            }
        } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            g_menu_buttons |= 1;
        } else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            g_menu_buttons = 0;
        }
    }
}

/* IMAGE 0x04D1A's FIRST HALF - one scancode that is down, or 0xFF.
 *
 * NOT TRANSCRIBED, and here is what is missing rather than a claim that it
 * is only a keyboard read. Image 0x04D1A walks a table of 83 bytes at
 * ds:0x0F78, one per scancode, kept by the game's own INT 9 handler: bit 7
 * says the key is down and the low seven are a COUNTDOWN. Each poll
 * decrements the first key it finds down; reaching 0 fires it and reloads
 * with 0x86, and image 0x04D3C fires again at 0x93. That is an initial delay
 * and a repeat rate, in the game's own units, and it is why holding F3 on
 * the original cycles at a pace rather than as fast as the loop runs.
 *
 * This gives the poll a press per pass out of a queue instead. Holding a key
 * still repeats, because SDL sends repeat events - but at SDL's rate, not
 * the game's, and that difference is real and is not measured. Reading the
 * INT 9 handler and the table is what would settle it.
 *
 * 0xFF for nothing is image 0x04D2B's own answer, not a convention invented
 * here - and it matters, because 0 is a valid scancode index. */
uint8_t menu_key_read(void)
{
    uint8_t k;
    menu_pump_events();
    if (g_key_head == g_key_tail) return 0xFF;   /* image 0x04D2B */
    k = g_key_ring[g_key_head];
    g_key_head = (uint8_t)((g_key_head + 1) % sizeof g_key_ring);
    return k;
}

/* ds:0x14 - the button word image 0x0446D and image 0x04453 both test. */
uint16_t menu_buttons(void)
{
    menu_pump_events();
    return g_menu_buttons;
}

/* Image 0x04C20: point the CRTC at the page just composed and swap
 * ds:0x1F76 with ds:0x1F78. One page here, so this is the present. */
void video_page_flip(void)
{
    void *pixels = NULL;
    int pitch = 0;
    if (!g_ren || !g_tex || !g_menu_pix || !g_menu_pal) return;
    if (SDL_LockTexture(g_tex, NULL, &pixels, &pitch)) {
        for (uint16_t y = 0; y < MENU_H; y++) {
            uint32_t *row = (uint32_t *)((uint8_t *)pixels + y * pitch);
            const uint8_t *src = g_menu_pix + (size_t)y * MENU_W;
            for (uint16_t x = 0; x < MENU_W; x++) {
                lem_rgb c = g_menu_pal[src[x]];
                row[x] = ((uint32_t)c.r << 16)
                       | ((uint32_t)c.g << 8) | c.b;
            }
        }
        SDL_UnlockTexture(g_tex);
    }
    SDL_RenderClear(g_ren);
    SDL_RenderTexture(g_ren, g_tex, NULL, NULL);
    SDL_RenderPresent(g_ren);
}

/* THE WINDOW, and nothing else. The loop is image 0x0445B and lives in
 * game.c; this opens the thing it draws into and hands it over.
 *
 * LEM_FRAMES still bounds a run, because a loop that only ends on a keypress
 * cannot be driven by a test. It arrives as an Esc through the same queue a
 * person's would, so the loop ends the way the original's does rather than
 * by a second exit nothing else uses. */
menu_action menu_run(menu_screen *m, uint8_t scale)
{
    const char *cap;
    if (scale < 1) scale = 1;
    if (view_open(MENU_W, MENU_H, "PC Lemmings", scale) != 0) return MENU_EXIT;
    video_screen_set(m->pix, m->palette);
    g_key_head = g_key_tail = 0;
    g_menu_buttons = 0;
    menu_wait_release();                 /* image 0x04453 */
    cap = SDL_getenv("LEM_FRAMES");
    /* -1 is unbounded. The cap goes INTO the loop rather than into a copy of
     * it here - see menu_loop, and the four-of-seven calls this used to be. */
    return menu_loop(m, cap ? SDL_atoi(cap) : -1);   /* image 0x0445B */
}

/* ======================================================================
 * READING THE FILES.
 *
 * be16 and le16 are the file formats' byte order, not the game's: the
 * original reads these words inline where it needs them and has no routine
 * to transcribe. ground_load used to be here too and is now in game.c, where
 * its LAYOUT carries the address of the instruction that reads each field.
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




/* WHAT THE HARDWARE USED TO DO: ds:0x14, the byte the mouse driver kept
 * current, and the page the picture sits on.
 *
 * The routine that reads them is game.c's level_results_wait - image 0x047A9.
 * Bit 0 is the left button and bit 1 the right, which is what image 0x047B4
 * and image 0x047B9 test. Escape reports bit 1 because that is the answer
 * every other screen here gives it, and image 0x047BE does read ds:0x1FC7 as
 * well so a key does end the screen.
 *
 * READING IT PUMPS THE QUEUE, because a byte a driver maintained has no
 * counterpart that can be read without one, and the delay is here for the
 * same reason: the original owns the machine and may spin, this does not.
 * Closing the window leaves through view_quit_now like everywhere else. */
uint16_t view_buttons(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) view_quit_now();
        else if (e.type == SDL_EVENT_KEY_DOWN) {
            if (e.key.key == SDLK_ESCAPE) g_key_button = 2;
            else                          g_key_button = 1;
        }
        /* ds:0x1FC7 IS A STATE, so a released key stops reading. Latching the
         * press and never clearing it made view_buttons answer for ever, and
         * image 0x047A9's release wait - `while (buttons & 7)` - would then
         * never come out: the screen would hang on the first key it saw. */
        else if (e.type == SDL_EVENT_KEY_UP) g_key_button = 0;
    }
    SDL_Delay(10);
    uint32_t held = SDL_GetMouseState(NULL, NULL);
    uint16_t b = g_key_button;
    if (held & SDL_BUTTON_LMASK) b |= 1;
    if (held & SDL_BUTTON_RMASK) b |= 2;
    return b;
}

/* The picture, uploaded and shown. Nothing on this screen animates - image
 * 0x047A9 polls and draws nothing - so a caller shows it once. */
void view_present_indexed(const uint8_t *pix, uint16_t w, uint16_t h,
                          const lem_rgb *pal)
{
    void *px = NULL; int pitch = 0;
    if (SDL_LockTexture(g_tex, NULL, &px, &pitch)) {
        for (uint16_t y = 0; y < h; y++) {
            uint32_t *row = (uint32_t *)((uint8_t *)px + y * pitch);
            const uint8_t *src = pix + (size_t)y * w;
            for (uint16_t x = 0; x < w; x++) {
                lem_rgb q = pal[src[x] & 15];
                row[x] = ((uint32_t)q.r << 16) | ((uint32_t)q.g << 8)
                       | (uint32_t)q.b;
            }
        }
        SDL_UnlockTexture(g_tex);
    }
    SDL_RenderClear(g_ren);
    SDL_RenderTexture(g_ren, g_tex, NULL, NULL);
    SDL_RenderPresent(g_ren);
}
