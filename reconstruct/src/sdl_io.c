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

static SDL_Renderer *g_ren;
static SDL_Texture  *g_tex;
static uint16_t      g_w, g_h;
static uint8_t       g_scale;

static void sound_open(void);
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
    /* THE LEVEL'S KEYS, image 0x03E6F's set. F3 is button 2 and each key after
     * it is one more, so the run F3..F10 has to be contiguous - it is, 0x3D to
     * 0x44 - and F11, F12, Del and keypad plus sit apart from it. Without
     * these the pump answered 0xFF for six of the eight skill keys and the
     * nuke, which is a key that does nothing rather than a key that means
     * something else. */
    case SDLK_F5:        return 0x3F;
    case SDLK_F6:        return 0x40;
    case SDLK_F7:        return 0x41;
    case SDLK_F8:        return 0x42;
    case SDLK_F9:        return 0x43;
    case SDLK_F10:       return 0x44;
    case SDLK_F11:       return 0x57;
    case SDLK_F12:       return 0x58;
    case SDLK_DELETE:    return 0x53;
    case SDLK_KP_PLUS:   return 0x4E;
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
/* ds:0x14 - THE BUTTON WORD, and there is one of it.
 *
 * The port had FOUR readers of this: view_buttons with its own state,
 * menu_buttons with g_menu_buttons and its own pump, view_screen_pump with a
 * callback, and lem_buttons. Four pumps on one queue, and only one of them
 * fed the keyboard handler - so a key-up swallowed by any of the other three
 * left ds:0x1FC7's bit SET, and Escape stayed pressed for ever. A player
 * found that as a level ending the moment it started.
 *
 * The original has one byte, kept by image 0x04269 from INT 33h AX=3, and
 * every screen polls it. So does this. */
uint16_t mouse_buttons(void)
{
    view_input_drain();
    return lem_buttons;
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

/* ======================================================================
 * THE LEVEL'S SCREEN AND ITS TWO PALETTES - WHAT THE CARD USED TO HOLD.
 *
 * image 0x055B7 waits three retraces to a notch, image 0x01550 waits a
 * retrace and image 0x0553D pushes the result
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
 * at this moment - the play area's on top, the panel's below. */
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
    if (!g_lvl_live || !g_lvl_ren) return;
    level_screen_upload();
    SDL_RenderClear(g_lvl_ren);
    SDL_RenderTexture(g_lvl_ren, g_lvl_tex, NULL, NULL);
    SDL_RenderPresent(g_lvl_ren);
    /* A fade holds the play loop for the best part of a second, and a window
     * that answers nothing for that long is one the desktop reports as hung.
     *
     * IT MUST GO THROUGH THE HANDLER. This polled the queue itself and acted
     * only on the close box, so every key RELEASE that arrived during a fade
     * was thrown away - and ds:0x1FC7's bit stayed set. Escape ends a level at
     * image 0x002AB and the fade that follows is exactly where its key-up
     * lands, so the next level saw Escape still held and ended the moment it
     * began. A player found it. There is one queue and one handler now. */
    view_input_drain();
}

/* THE WINDOW'S OWN STATE FOR A LEVEL. level_run is game.c's now - it is image
 * 0x00455's tail - and these are what its five primitives here work on:
 * whether the window holds the pointer, and the deadline a frame is paced
 * against. Threading them through the game's signature would have put the
 * window's bookkeeping into a transcribed routine. */
static lem_rgb     g_lv_top[16];
static Uint64      g_lv_due, g_lv_period;
static int         g_lv_grabbed;
static port_key    g_port_key;

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
/* WHAT THE HARDWARE USED TO DO: ONE DRAIN OF THE HOST'S QUEUE, dispatched the
 * way the two interrupts did.
 *
 *   a key      -> keyboard_isr, image 0x016D4, which leaves ds:0x0F78,
 *                 ds:0x1FC7 and ds:0x1FD0 behind
 *   the mouse  -> ds:0x10, ds:0x12 and ds:0x14, which image 0x04269 keeps
 *                 from INT 33h AX=3
 *
 * IT DECIDES NOTHING. Everything that was a decision - which key does what,
 * whether a click assigns, when the level ends - is image 0x04085's and image
 * 0x03E6F's and lives in game.c.
 *
 * IT RUNS WHEN THE STATE IS READ, not once a frame. That is what an interrupt
 * does: by the time the game looks at ds:0x1FC7 the handler has already run,
 * at whatever moment the key arrived. key_last, key_held and key_is_down all
 * come through here first, so a reader can never see a byte that is one drain
 * out of date. There is exactly one queue, so the mouse is brought up to date
 * on the same pass whoever asked.
 *
 * The grab is the port's own and has no counterpart: the original owns the
 * machine and the pointer cannot be anywhere else. */
static uint8_t g_pressed_edge;      /* the left button went down since asked */
static uint8_t g_moved_edge;        /* the pointer has been somewhere definite */

void view_input_drain(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) view_quit_now();
        else if (e.type == SDL_EVENT_MOUSE_MOTION) {
            g_moved_edge = 1;
            if (g_lv_grabbed) {
                /* RELATIVE motion while the window holds the pointer, which
                 * is what the original works in: it asks the driver for
                 * mickeys and sets the mickey-to-pixel ratio itself at image
                 * 0x00F20. An absolute position would also stop at the edge
                 * of the desktop rather than at the edge of the game's own
                 * range. */
                lem_mouse_move((int32_t)lem_mouse_x + (int32_t)e.motion.xrel,
                               (int32_t)lem_mouse_y + (int32_t)e.motion.yrel);
            } else if (g_lvl_ren) {
                int ow = 0, oh = 0;
                SDL_GetRenderOutputSize(g_lvl_ren, &ow, &oh);
                if (ow > 0 && oh > 0)
                    lem_mouse_move(
                        (int32_t)(e.motion.x * LEVEL_SCREEN_W / (float)ow) + 8,
                        (int32_t)(e.motion.y * LEVEL_SCREEN_H / (float)oh) + 8);
            }
        }
        else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            if (!g_lv_grabbed && g_lvl_ren) {
                /* Click to take it back, the way every DOS front end does.
                 * That click is for the window, not the game. */
                view_level_grab(1);
            } else {
                /* THE TWO BUTTONS ARE DIFFERENT BITS. Image 0x04269 stores
                 * INT 33h AX=3's BX in ds:0x14, so bit 0 is the left button
                 * and bit 1 the right, and the game asks for them separately:
                 * image 0x03168 does `and ax, 1` before it will assign
                 * anything, while image 0x03DB7 tests bit 1 and makes the
                 * under-pointer scan pick the PLAIN lemming over the busy
                 * one. That is how the original lets you point past a builder
                 * at the walker behind it. */
                if (e.button.button == SDL_BUTTON_LEFT) {
                    lem_buttons |= 1;
                    g_pressed_edge = 1;
                } else if (e.button.button == SDL_BUTTON_RIGHT) {
                    lem_buttons |= 2;
                }
            }
        }
        else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            if (e.button.button == SDL_BUTTON_LEFT)
                lem_buttons &= (uint8_t)~1u;
            else if (e.button.button == SDL_BUTTON_RIGHT)
                lem_buttons &= (uint8_t)~2u;
        }
        else if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) {
            /* Ctrl+Alt gives the pointer back, which is the gesture DOSBox
             * trained everyone to reach for. Without a release the window
             * owns the mouse and the only way out is to kill the process - so
             * a grab without one is a bug, not a feature. THE PORT'S OWN, and
             * it never reaches the handler. */
            if (e.type == SDL_EVENT_KEY_DOWN
                    && (e.key.mod & SDL_KMOD_CTRL) && (e.key.mod & SDL_KMOD_ALT)) {
                view_level_grab(0);
                continue;
            }
            uint8_t sc = scancode_of(e.key.key);
            if (sc != 0xFF)
                keyboard_isr((uint8_t)(e.type == SDL_EVENT_KEY_UP
                                       ? (sc | 0x80) : sc));   /* image 0x016D4 */
            /* THE PORT'S OWN KEYS, which have no scancode because the
             * original has no such keys - it owns the machine, so there is
             * nothing to abandon a level for and its scroll is the pointer
             * pushed against the edge. Named rather than passed through as
             * SDL's numbering. */
            if (e.type == SDL_EVENT_KEY_DOWN) switch (e.key.key) {
            case SDLK_Q:     g_port_key = PORT_KEY_ABANDON; break;
            case SDLK_LEFT:  g_port_key = PORT_KEY_LEFT;    break;
            case SDLK_RIGHT: g_port_key = PORT_KEY_RIGHT;   break;
            case SDLK_HOME:  g_port_key = PORT_KEY_HOME;    break;
            case SDLK_END:   g_port_key = PORT_KEY_END;     break;
            default: break;
            }
        }
    }
}

/* The mouse edges the drain saw, taken rather than peeked. */
level_input view_level_pump(void)
{
    level_input in;
    view_input_drain();
    in.pressed  = g_pressed_edge; g_pressed_edge = 0;
    in.moved    = g_moved_edge;
    in.port_key = g_port_key;     g_port_key = PORT_KEY_NONE;
    return in;
}

/* WHAT THE HARDWARE USED TO DO once a frame is composed: image 0x00105 hands
 * the CRTC the other page. The SEQUENCE - flip, then wait five ticks - is the
 * play loop's and is level_frame_show in game.c. */
void view_level_present(void)
{
    level_screen_upload();
    SDL_RenderClear(g_lvl_ren);
    SDL_RenderTexture(g_lvl_ren, g_lvl_tex, NULL, NULL);
    SDL_RenderPresent(g_lvl_ren);
}

/* IMAGE 0x00164's wait, which is five of the timer's ticks - see
 * LEVEL_FRAME_HZ.
 * An ABSOLUTE deadline, not a sleep per frame: a sleep is at the mercy of the
 * host scheduler and of how long the frame took to compose, which would make
 * the game's speed a property of the machine. Falling behind resets the
 * deadline rather than building a debt the loop then sprints to repay. */
void view_level_pace(void)
{
    g_lv_due += g_lv_period;
    Uint64 now = SDL_GetTicksNS();
    if (now < g_lv_due) SDL_DelayNS(g_lv_due - now);
    else g_lv_due = now;          /* behind: start again from here */
}

/* The level's upper palette, for the harness's frame sink. */
const lem_rgb *view_level_palette(void) { return g_lv_top; }

/* ======================================================================
 * WHAT THE CARD AND THE DRIVERS DID FOR A LEVEL. The SEQUENCE that calls
 * these is image 0x00455's and lives in game.c as level_run; each of these is
 * one line of it.
 * ====================================================================== */

/* The handles view_open just made, and the page the game composes into.
 *
 * These read `g_lvl_tex = g_lvl_tex;` for a while: a scoped rename turned the
 * two locals into these globals and deleted the lines that had initialised
 * them, so both stayed NULL, level_screen_upload returned at its own guard,
 * and the game showed nothing while the sound played on. */
void view_level_bind(uint8_t *screen)
{
    g_lvl_screen = screen;
    g_lvl_tex = g_tex;
    g_lvl_ren = g_ren;
}

void view_level_unbind(void)
{
    g_lvl_screen = NULL; g_lvl_tex = NULL; g_lvl_ren = NULL;
}

/* IMAGE 0x052F4's load, the level's palette at the card - two of them, because
 * the level swaps attributes 8..15 part-way down the screen.
 *
 * `live` is ds:0x200E. With a display the pair the screen is drawn through
 * starts at BLACK and image 0x0031E's fade climbs out of it; without one there
 * is nothing to fade, so it starts at the target and image 0x0033D's single
 * step clears the bit on the first frame. A capture harness has a frame budget
 * and a fade inside frame 0 would spend a second of wall clock producing
 * nothing, every frame of it carrying a palette climbing out of black. */
void view_level_palettes(const lem_rgb *top, const lem_rgb *bottom, int live)
{
    memcpy(g_lvl_tgt_top, top, sizeof g_lvl_tgt_top);
    memcpy(g_lvl_tgt_bottom, bottom, sizeof g_lvl_tgt_bottom);
    g_lvl_live = (live && g_win) ? 1 : 0;
    if (g_lvl_live) {
        memset(g_lvl_top, 0, sizeof g_lvl_top);
        memset(g_lvl_bottom, 0, sizeof g_lvl_bottom);
    } else {
        memcpy(g_lvl_top, g_lvl_tgt_top, sizeof g_lvl_top);
        memcpy(g_lvl_bottom, g_lvl_tgt_bottom, sizeof g_lvl_bottom);
    }
}

/* IMAGE 0x015AD - the PIT divisor into ds:0x1FC9, which is what sets a frame's
 * length. Here it is the period view_level_pace counts against. */
void view_level_period(uint16_t hz)
{
    g_lv_period = (Uint64)(SDL_NS_PER_SECOND / (hz ? hz : 1));
    g_lv_due = SDL_GetTicksNS();
}

/* THE POINTER IS HELD BY THE WINDOW, and Ctrl+Alt gives it back.
 *
 * The original owns the mouse completely - it hides the driver's pointer at
 * image 0x00F43, sets its own range at images 0x00F2B and 0x00F36 and draws
 * its own cross-hair - and a window that lets the host pointer walk out of it
 * while the game still thinks it is moving does not reproduce that. Held, the
 * two agree; released, the game keeps the last position.
 *
 * THE GLOBAL, not a local of the same name. This read `int g_lv_grabbed = 1;`
 * for a while - a scoped rename turned a local into a shadow of the global the
 * pump reads - so the level grabbed the mouse while the pump still believed it
 * had not, and the FIRST CLICK of every level was eaten to "take the pointer
 * back" from a window that already had it. -Wshadow named the line; I was
 * grepping the build for `error`. */
void view_level_grab(int on)
{
    g_lv_grabbed = on ? 1 : 0;
    if (g_win) SDL_SetWindowRelativeMouseMode(g_win, on ? true : false);
}

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
/* EVERY REQUEST IS TAKEN UNDER THE STREAM'S LOCK. The driver's state is read
 * on SDL's audio thread now, so a request that writes it - and adlib_stop,
 * which walks the channel array - has to be held against a render happening
 * at the same moment. The original needed nothing of the kind: the PIT ISR
 * and the game ran on one processor and DOS turned interrupts off around the
 * parts that mattered. This is that `cli`. */
void sound_request(uint8_t vec, uint8_t ah, uint8_t id)
{
    if (vec != 0x61) return;   /* the fallback device, which this port has not */
    if (!adlib_ready()) return;
    if (g_snd_stream) SDL_LockAudioStream(g_snd_stream);
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
         * and it is IO: the original's OPL is real time, so keying it off is
         * silent in the same instant, where anything this port has already
         * rendered plays on. A player reported that as a level's music
         * running into the next one.
         *
         * It matters far less than it did - the device pulls, so what is
         * outstanding is one device period rather than the fifth of a second
         * the push model kept ahead - but a period is still audible after a
         * stop, and this is the one request that means SILENCE. Doing it on
         * an effect or a tune change would cut whatever is still sounding,
         * and the original does not. */
        if (g_snd_stream) SDL_ClearAudioStream(g_snd_stream);
        break;
    case 3: adlib_music(id);   break;       /* driver +0x0022 */
    case 4: adlib_effect(id);  break;       /* driver +0x002A */
    case 5: adlib_slot5(id);   break;       /* driver +0x0032 */
    default: break;
    }
    if (g_snd_stream) SDL_UnlockAudioStream(g_snd_stream);
}

/* image 0x003BB does this the instant it has asked for a tune. */
void sound_tick_now(void)
{
    if (adlib_ready()) adlib_tick();        /* driver +0x0115 */
}

/* How many samples the callback renders at a time - a working buffer, not
 * a queue depth: SDL asks for an amount and this is the slice it is built
 * in. */
#define SND_CHUNK 1024

/* THE QUEUE IS SDL'S NOW. There was an SND_TARGET_MS here - how far ahead of
 * the game to generate - with sound_target_bytes and sound_fill_bytes to
 * serve it, and a harness probe asking whether the pump kept up at N frames a
 * second. None of that survives a device that PULLS: it asks for what it
 * needs and the callback renders exactly that, so there is no target to miss
 * and no rate to fall behind. They are deleted rather than left passing,
 * because a check that models a mechanism the code no longer has is worse
 * than no check - it keeps agreeing. */

/* Build one chunk: tick the driver where the tick falls, render around it.
 * This is what the audio callback calls, and it is the ISR's counterpart -
 * the driver runs on the DEVICE's clock, the way it ran on the PIT's.
 *
 * It is a separate function so the LIVE path - the one the game actually runs
 * - can be exercised without an audio device, which reconstruct/tests does:
 * everything else measured about the sound is the driver alone
 * (tools/verify_adlib.py), and this is the code between the driver and the
 * card. */
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

/* THE DEVICE ASKS, THE WAY THE PIT DID.
 *
 * The original never pumps anything: image 0x017C7 is an INT 8 handler and
 * the PIT calls it ~96.8 times a second whatever the game is doing, and the
 * OPL sounds continuously in hardware. Nothing in the game loop is involved.
 *
 * This port used to PUSH - the game loop topped a queue up once a frame - and
 * every consequence of that was a bug a player found: a fifth of a second of
 * latency on a click, sound that froze on any screen whose loop forgot to
 * pump, and a residue that resumed on the next screen that remembered. Four
 * scattered pump calls were four chances to forget.
 *
 * SDL calls this from its own audio thread when the device wants more, which
 * is the same relationship the card had with the PIT: sound_build_chunk
 * already ticks image 0x017C7 at the rate the sample count implies, so the
 * driver runs on the device's clock and the queue can be as shallow as SDL
 * likes. */
static void SDLCALL sound_stream_cb(void *userdata, SDL_AudioStream *stream,
                                    int additional, int total)
{
    (void)userdata; (void)total;
    if (!adlib_ready() || additional <= 0) return;

    int16_t buf[SND_CHUNK];
    while (additional > 0) {
        uint32_t frames = SND_CHUNK;
        if ((int)(frames * sizeof(int16_t)) > additional)
            frames = (uint32_t)additional / sizeof(int16_t);
        if (frames == 0) break;
        sound_build_chunk(buf, frames);
        SDL_PutAudioStreamData(stream, buf, (int)(frames * sizeof(int16_t)));
        additional -= (int)(frames * sizeof(int16_t));
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
    /* The callback is handed to SDL_OpenAudioDeviceStream, so the device has
     * a source from the moment it exists: SDL calls it on its own thread when
     * it wants more, and nothing in the game loop touches the queue. */
    g_snd_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                             &want, sound_stream_cb, NULL);
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
/* ======================================================================
 * THE MENU SCREEN'S TWO PALETTE SLOTS - WHAT THE DAC USED TO HOLD.
 *
 * The LOOPS that walk them are the game's and live in game.c, carrying image
 * 0x045EF and image 0x046A2. What is left here is the state a notch moves and
 * the one thing the original did not have to do: re-upload the frame, because
 * this port has no DAC and a palette only becomes visible when the picture is
 * put through it again.
 *
 * The level's fade is split exactly this way already - level_fade_notch here,
 * the loop in level_phase_step - and two fades arranged differently would
 * drift, which is what this file's own notes keep finding.
 * ====================================================================== */

/* Image 0x045F2's set-up: both slots aimed at the screen's palettes and the
 * current pair blacked, so the first notch starts from nothing. */
void menu_fade_arm(const lem_rgb *pal, const lem_rgb *low, uint16_t split)
{
    g_split = split;
    for (uint8_t i = 0; i < 16; i++) {
        g_fade_tgt[i] = pal[i];
        g_fade_low_tgt[i] = low[i];
        g_fade_cur[i].r = g_fade_cur[i].g = g_fade_cur[i].b = 0;
        g_fade_low_cur[i].r = g_fade_low_cur[i].g = g_fade_low_cur[i].b = 0;
    }
    g_split_pal = split ? g_fade_low_cur : NULL;
}

/* Image 0x046AB's: both slots aimed at black. */
void menu_fade_to_black(void)
{
    for (uint8_t i = 0; i < 16; i++) {
        g_fade_tgt[i].r = g_fade_tgt[i].g = g_fade_tgt[i].b = 0;
        g_fade_low_tgt[i].r = g_fade_low_tgt[i].g = g_fade_low_tgt[i].b = 0;
    }
}

void menu_fade_disarm(void) { g_split = 0; g_split_pal = NULL; }

/* IMAGE 0x05329 - ONE NOTCH OF BOTH SLOTS, which is the call image 0x045F8
 * and image 0x046B1 both make. Non-zero while it still has somewhere to go,
 * which is what image 0x045FB's and image 0x046B4's `jne` test. */
int menu_fade_notch(void)
{
    int moved = palette_fade_step(g_fade_cur, g_fade_tgt, 16);
    if (g_split)
        moved |= palette_fade_step(g_fade_low_cur, g_fade_low_tgt, 16);
    return moved;
}

/* The upper slot as it stands, for the frame the loop puts up each notch. */
const lem_rgb *menu_fade_palette(void) { return g_fade_cur; }

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
