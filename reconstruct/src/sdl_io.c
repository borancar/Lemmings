/* sdl_io.c - the window, input and timing, through SDL3.
 *
 * THIS FILE IS THE PORT'S OWN - what the hardware used to do. Nothing in it is
 * a transcription: the original talked to an EGA card, a PIT, a mouse driver
 * and a BIOS, and this talks to SDL. The boundary between it and game.c is a
 * boundary the PORT chose - what a modern backend has to replace - not one the
 * binary proves. (provenance.py reads that first sentence literally, and every
 * function here is counted as the port's own on the strength of it.)
 *
 * It DOES carry image offsets, and this file used to say it carried none
 * "because there are none to carry". Whole transcribed routines then drifted
 * in behind that sentence - menu_show_until_dismissed, view_level_input,
 * view_level_show and level_run were all image 0x04566's, image 0x04085's,
 * image 0x00105's and image 0x00455's, living here. An offset in this file
 * names what a primitive STANDS IN FOR, so a reader can find it: image
 * 0x01550's retrace, image 0x0553D's palette upload, image 0x00105's flip.
 *
 * A routine with a SEQUENCE in it belongs in game.c however much hardware it
 * touches. The check that the split is honest is that game.c contains no SDL_
 * at all - grep for it after moving anything.
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


/* THE SCANCODE THE KEYBOARD PRODUCED - not what it means.
 *
 * This is the only part of the keyboard that is the HARDWARE's: what byte
 * port 0x60 would have produced. Everything after it is transcribed and lives
 * in game.c - image 0x016D4's handler keeps ds:0x0F78, ds:0x1FC7 and
 * ds:0x1FD0, and image 0x04D1A scans the first of those and translates it
 * through ds:0x202E.
 *
 * This used to be a key ENUM and a table of boxes. The enum was the port's
 * invention standing where the game has scancodes, and the boxes stood where
 * the game has nothing at all.
 */
uint8_t scancode_of(uint32_t k)
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
     * The switch above is the menu's keys and the level's, and for a long
     * time it held only the menu's eleven - so image 0x04C7E's entry loop,
     * which takes A-Z, got 0xFF for every letter typed and correctly
     * ignored it. Nothing appeared on screen, which a player reported.
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


/* ds:0x14 - THE BUTTON WORD, and there is one of it.
 *
 * The port had FOUR readers of this: view_buttons with its own state,
 * menu_buttons with its own pump, view_screen_pump with a callback, and
 * lem_buttons. Four pumps on one queue, and only one of them fed the keyboard
 * handler - so a key-up swallowed by any of the other three left ds:0x1FC7's
 * bit SET, and Escape stayed pressed for ever. A player found that as a level
 * ending the moment it started.
 *
 * The original has one byte, kept by image 0x04269 from INT 33h AX=3, and
 * every screen polls it. So does this. */
uint16_t mouse_buttons(void)
{
    view_input_drain();
    return lem_buttons;
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


/* ds:0x200E - is there a display. The level's SCREEN and its two palettes are
 * not here any more: the screen is whichever page video_screen_set was given
 * and the palettes are game.c's slots, so what is left of "the level's video
 * state" is this one byte. */
static int g_lvl_live;

/* ds:0x200E, which image 0x00325 and image 0x00346 both test. */
int video_present(void) { return g_lvl_live; }

/* ds:0x200E SET, which is the host's answer rather than the game's: is there a
 * window to fade INTO. A capture harness is not a display - it has a frame
 * budget, and a fade run inside frame 0 would spend a second of wall clock
 * producing no frames while every frame it did produce carried a palette
 * climbing out of black.
 *
 * This line went missing when the palette slots moved to game.c, and the level
 * came up BLACK: level_palette_set had blacked the current pair ready for the
 * fade, level_screen_upload returned at its own `!g_lvl_live` guard, and
 * level_fade_notch answered "already arrived" every pass, so nothing ever
 * climbed. A player saw a black level with the sound playing - the same
 * SYMPTOM as the g_lvl_tex self-assignment, from a different cause, which is
 * why that guard says so out loud. */
void view_level_live(int on) { g_lvl_live = (on && g_win) ? 1 : 0; }

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
    if (!g_lvl_live) return;
    video_page_flip();          /* image 0x04C20 - the one upload */
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
static Uint64      g_lv_due, g_lv_period;
/* TWO STATES THE PORT HAD CONFLATED. `g_mouse_owned` is the LEVEL owning the
 * pointer - image 0x00F43 hides the driver's own and image 0x00F2B sets the
 * game's range, and that lasts the whole level. `g_lv_grabbed` is whether the
 * WINDOW is holding it at this moment, which Ctrl+Alt gives back and a click
 * takes again. They were one flag, tested through a level-only renderer
 * handle; with the handles gone there was nothing left to ask. */
static const uint8_t *g_page_pix;
static uint16_t       g_page_w, g_page_h, g_page_split;
static const lem_rgb *g_page_pal, *g_page_low;

static int         g_lv_grabbed, g_mouse_owned;
/* THE BUTTONS, AS THE DRIVER WAS ASKED FOR THEM. Image 0x04252 is called
 * from the timer interrupt and stores INT 33h AX=3's answer in ds:0x14 five
 * times a frame; image 0x03E6F - the panel, the nuke's double click - reads it
 * on those ticks, and image 0x03150 from the frame loop. So while a level owns
 * the mouse the host's button changes are QUEUED in the order they arrived,
 * so a quick release between two presses is seen as a release, as the
 * original's driver would have seen it.
 *
 * ONE CHANGE A FRAME, AT THE FRAME'S START (view_level_pump). Every press and
 * every release is then seen by the frame's work and by all five of its
 * ticks, and a double click puts its two presses two frames apart, inside
 * image 0x04005's five. That much is the port's own - generous where the
 * original's sampling was not.
 *
 * The start, and not a tick, because that is where a recorded script puts
 * a change (harness_script_step) and where tools/record_level.py puts it in
 * the original (its poll_now, at image 0x018F). Applied on a tick, the ticks
 * of the frame BEFORE saw a press that `--record-input` writes against the
 * next one, so every panel click replayed a frame late: the release rate
 * turned a frame later, the lemmings came out a frame apart, and Fun 16 and
 * 17 - the first solutions recorded that way - replayed as lost games on
 * both sides while every frame of the replays agreed.
 *
 * This replaced a release held for a frame, and a press that CANCELLED a held
 * release: a double click with both inside one frame read as one long press,
 * ds:0x62 never let go, and the nuke wanted several double clicks before one
 * registered. A player found it. */
static uint8_t g_pressed_edge;      /* the left button went down since asked */
static int32_t g_mouse_px, g_mouse_py;   /* a position the frame has not taken */
/* AND THE KEYS, for the same reason: image 0x03E6F reads ds:0x1FD0 on the
 * ticks, and a replay presses a recorded key at the top of the wait
 * (harness_script_keys, and record_level.py at image 0x00105). So while a
 * level runs a key is held until then too, and recorded as it is pressed -
 * a key that reached the handler on the third tick would otherwise replay
 * two ticks early, and F1 and F2 step the rate on every tick. */
#define KEY_QUEUE 32
static uint8_t g_key_q[KEY_QUEUE];
static uint8_t g_key_qn;
static uint8_t g_mouse_pend;
typedef struct { uint8_t bit, down; } btn_edge;
#define BTN_QUEUE 16
static btn_edge    g_btn_q[BTN_QUEUE];
static uint8_t     g_btn_qn;

static void btn_queue(uint8_t bit, uint8_t down)
{
    if (!bit) return;
    if (g_btn_qn == BTN_QUEUE) {      /* full: the oldest goes in now */
        if (g_btn_q[0].down) lem_buttons |= g_btn_q[0].bit;
        else lem_buttons &= (uint8_t)~g_btn_q[0].bit;
        memmove(g_btn_q, g_btn_q + 1, sizeof g_btn_q[0] * (BTN_QUEUE - 1));
        g_btn_qn--;
    }
    g_btn_q[g_btn_qn].bit = bit;
    g_btn_q[g_btn_qn].down = down;
    g_btn_qn++;
}

/* One change a call - a frame's worth, see g_btn_q. */
static void btn_apply_one(void)
{
    if (!g_btn_qn) return;
    btn_edge e = g_btn_q[0];
    if (e.down) lem_buttons |= e.bit;
    else lem_buttons &= (uint8_t)~e.bit;
    memmove(g_btn_q, g_btn_q + 1, sizeof g_btn_q[0] * (BTN_QUEUE - 1));
    g_btn_qn--;
}

/* ONE BUTTON CHANGE FROM THE HOST, as the event drain sees it - split out so
 * tests/menu_input.c can hand it a double click without SDL delivering one. */
void view_button_event(uint8_t bit, uint8_t down)
{
    if (down && bit == 1) g_pressed_edge = 1;
    if (g_mouse_owned) btn_queue(bit, down);        /* see g_btn_q */
    else if (down) lem_buttons |= bit;
    else lem_buttons &= (uint8_t)~bit;
}

static void btn_flush(void)
{
    while (g_btn_qn) btn_apply_one();
}
static port_key    g_port_key;

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
static uint8_t g_moved_edge;        /* the pointer has been somewhere definite */

void view_input_drain(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) view_quit_now();
        /* FOLLOWING ANOTHER GAME'S INPUT - tools/lockstep.py. The player's
         * hand is on the original's window, so this one only shows: a
         * pointer passing over it must not move the port's. */
        else if (harness_input_follows()) continue;
        else if (e.type == SDL_EVENT_MOUSE_MOTION) {
            g_moved_edge = 1;
            int32_t bx = g_mouse_pend ? g_mouse_px : (int32_t)lem_mouse_x;
            int32_t by = g_mouse_pend ? g_mouse_py : (int32_t)lem_mouse_y;
            int32_t nx, ny;
            if (g_lv_grabbed) {
                /* RELATIVE motion while the window holds the pointer, which
                 * is what the original works in: it asks the driver for
                 * mickeys and sets the mickey-to-pixel ratio itself at image
                 * 0x00F20. An absolute position would also stop at the edge
                 * of the desktop rather than at the edge of the game's own
                 * range. */
                nx = bx + (int32_t)e.motion.xrel;
                ny = by + (int32_t)e.motion.yrel;
            } else if (g_mouse_owned && g_ren) {
                /* Scaled by the PAGE that is up rather than by a constant -
                 * the level's is 320x200 and it was written out as such. */
                int ow = 0, oh = 0;
                SDL_GetRenderOutputSize(g_ren, &ow, &oh);
                if (!(ow > 0 && oh > 0 && g_page_w && g_page_h)) continue;
                nx = (int32_t)(e.motion.x * g_page_w / (float)ow) + 8;
                ny = (int32_t)(e.motion.y * g_page_h / (float)oh) + 8;
            } else continue;
            if (g_mouse_owned) {
                /* HELD FOR THE FRAME'S START, as a button change is - see
                 * g_btn_q. Clamped as it goes, as the driver's range set at
                 * image 0x00F2B clamped it, so a push past an edge does not
                 * bank distance a move back has to undo. */
                lem_mouse_clamp(&nx, &ny);
                g_mouse_px = nx; g_mouse_py = ny; g_mouse_pend = 1;
            } else lem_mouse_move(nx, ny);
        }
        else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            if (!g_lv_grabbed && g_mouse_owned) {
                /* Click to take it back, the way every DOS front end does.
                 * That click is for the window, not the game. */
                g_lv_grabbed = 1;
                if (g_win) SDL_SetWindowRelativeMouseMode(g_win, true);
            } else {
                /* THE TWO BUTTONS ARE DIFFERENT BITS. Image 0x04269 stores
                 * INT 33h AX=3's BX in ds:0x14, so bit 0 is the left button
                 * and bit 1 the right, and the game asks for them separately:
                 * image 0x03168 does `and ax, 1` before it will assign
                 * anything, while image 0x03DB7 tests bit 1 and makes the
                 * under-pointer scan pick the PLAIN lemming over the busy
                 * one. That is how the original lets you point past a builder
                 * at the walker behind it. */
                view_button_event(
                    e.button.button == SDL_BUTTON_LEFT ? 1
                    : e.button.button == SDL_BUTTON_RIGHT ? 2 : 0, 1);
            }
        }
        else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            view_button_event(
                e.button.button == SDL_BUTTON_LEFT ? 1
                : e.button.button == SDL_BUTTON_RIGHT ? 2 : 0, 0);
        }
        else if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) {
            /* Ctrl+Alt gives the pointer back, which is the gesture DOSBox
             * trained everyone to reach for. Without a release the window
             * owns the mouse and the only way out is to kill the process - so
             * a grab without one is a bug, not a feature. THE PORT'S OWN, and
             * it never reaches the handler. */
            if (e.type == SDL_EVENT_KEY_DOWN
                    && (e.key.mod & SDL_KMOD_CTRL) && (e.key.mod & SDL_KMOD_ALT)) {
                g_lv_grabbed = 0;      /* the window lets go; the LEVEL does
                                        * not stop owning the pointer */
                if (g_win) SDL_SetWindowRelativeMouseMode(g_win, false);
                continue;
            }
            uint8_t sc = scancode_of(e.key.key);
            if (sc != 0xFF) {
                uint8_t code = (uint8_t)(e.type == SDL_EVENT_KEY_UP
                                         ? (sc | 0x80) : sc);
                view_key_event(code);
            }
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

/* ONE SCANCODE FROM THE HOST - split out, as view_button_event is, so
 * tests/menu_input.c can hand one over without SDL delivering it. A level's
 * keys wait for the wait - view_keys_apply. */
void view_key_event(uint8_t code)
{
    if (g_mouse_owned && g_key_qn < KEY_QUEUE) g_key_q[g_key_qn++] = code;
    else keyboard_isr(code);                        /* image 0x016D4 */
}

/* The keys a level's drain held back, pressed at the top of the frame's
 * wait - level_wait_ticks, beside the script's own. */
void view_keys_apply(void)
{
    view_input_drain();
    for (uint8_t i = 0; i < g_key_qn; i++) {
        harness_record_key(g_key_q[i]);
        keyboard_isr(g_key_q[i]);                   /* image 0x016D4 */
    }
    g_key_qn = 0;
}

/* The mouse edges the drain saw, taken rather than peeked. */
/* THE DRIVER ASKED ON A TICK - image 0x04252, from the timer interrupt. */
void view_tick_input(void)
{
    view_input_drain();     /* queued; the frame's start applies it */
}

level_input view_level_pump(void)
{
    level_input in;
    view_input_drain();
    btn_apply_one();        /* one change a frame - see g_btn_q */
    if (g_mouse_pend) {     /* and the pointer where it is now */
        lem_mouse_move(g_mouse_px, g_mouse_py);
        g_mouse_pend = 0;
    }
    in.pressed  = g_pressed_edge; g_pressed_edge = 0;
    in.moved    = g_moved_edge;
    in.port_key = g_port_key;     g_port_key = PORT_KEY_NONE;
    return in;
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

/* ======================================================================
 * WHAT THE CARD AND THE DRIVERS DID FOR A LEVEL. The SEQUENCE that calls
 * these is image 0x00455's and lives in game.c as level_run; each of these is
 * one line of it.
 * ====================================================================== */



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
    g_mouse_owned = on ? 1 : 0;
    g_lv_grabbed  = on ? 1 : 0;
    if (!on) btn_flush();         /* changes queued for a level that is gone */
    if (!on) {
        for (uint8_t i = 0; i < g_key_qn; i++) keyboard_isr(g_key_q[i]);
        g_key_qn = 0;
    }
    if (!on && g_mouse_pend) {
        lem_mouse_move(g_mouse_px, g_mouse_py);
        g_mouse_pend = 0;
    }
    if (harness_input_follows()) on = 0;     /* see view_input_drain */
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


/* ======================================================================
 * WHAT IMAGE 0x0445B NEEDS FROM THE HARDWARE, and it is one thing: the page.
 *
 * The loop is game.c's and so is its entry, image 0x04401. What is left here
 * is image 0x04C20's flip and the pointer telling it WHICH composed buffer is
 * up.
 *
 * There used to be a key QUEUE here as well, with a paragraph explaining that
 * a ring was needed because SDL delivers presses as events where the original
 * polls a BIOS buffer once a pass. That reading was wrong about the original:
 * image 0x04D1A does not read the BIOS buffer at all - it SCANS ds:0x0F78, the
 * 83-byte table the game's own INT 9 handler keeps, and the "held rather than
 * lost" behaviour the ring was built for is the handler's, not a buffer's.
 * With image 0x016D4 transcribed the table is here and image 0x04D1A is a
 * transcription over it, so the ring is gone.
 * ====================================================================== */
/* THE PAGE THAT IS UP: which composed buffer, how big, and how a ROW picks its
 * palette. The original has one of each - ds:0x1F76 and ds:0x1F78 are the two
 * page offsets and image 0x04C20 swaps them - and this port had FIVE routines
 * doing the same upload:
 *
 *     view_present_indexed   one palette, indices masked to 15
 *     video_page_flip        the menu's buffer, at a fixed size
 *     view_screen_present    one palette, with a per-row split
 *     level_screen_upload    the level's, split at LEVEL_PLAY_H, into a
 *                            SECOND set of handles that view_level_bind had
 *                            just assigned from the first
 *     view_level_present     that upload, then the present
 *
 * They differed in the size, in one `& 15`, and in where the palette changes.
 * Five spellings of one thing is how they drift, and this file has the scars:
 * the split arrived in one of them and not the others, and the `& 15` in only
 * one. There is one now.
 *
 * `split` is the row the second palette starts at, 0 for a screen with one -
 * the briefing is two pictures stacked and the level swaps attributes 8..15
 * part-way down. */

void video_screen_set(const uint8_t *pix, uint16_t w, uint16_t h,
                      const lem_rgb *pal, const lem_rgb *low, uint16_t split)
{
    g_page_pix = pix; g_page_w = w; g_page_h = h;
    g_page_pal = pal; g_page_low = low; g_page_split = split;
}

int video_screen_is(const uint8_t *pix) { return g_page_pix == pix; }

/* IMAGE 0x04C20 - POINT THE CRTC AT THE PAGE JUST COMPOSED, and swap ds:0x1F76
 * with ds:0x1F78. One page here, so this is the whole of it: the buffer
 * through its palette and up.
 *
 * A MISSING PAGE IS NOT A QUIET FRAME. This returned silently when the
 * pointers were NULL, so a bug that left them unset showed as a game that drew
 * nothing and said nothing - found by playing, not by any check here, because
 * the frame sink writes the composed buffer and never comes through this
 * function at all. Now it says so, once. */
void video_page_flip(void)
{
    void *pixels = NULL;
    int pitch = 0;
    if (!g_ren || !g_tex) return;
    if (!g_page_pix || !g_page_pal) {
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "  [video] no page is up - nothing will be shown\n");
        }
        return;
    }
    if (SDL_LockTexture(g_tex, NULL, &pixels, &pitch)) {
        for (uint16_t y = 0; y < g_page_h; y++) {
            uint32_t *row = (uint32_t *)((uint8_t *)pixels + y * pitch);
            const uint8_t *src = g_page_pix + (size_t)y * g_page_w;
            const lem_rgb *pal = (g_page_split && y >= g_page_split && g_page_low)
                               ? g_page_low : g_page_pal;
            for (uint16_t x = 0; x < g_page_w; x++) {
                lem_rgb c = pal[src[x] & 15];
                row[x] = ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
            }
        }
        SDL_UnlockTexture(g_tex);
    }
    SDL_RenderClear(g_ren);
    SDL_RenderTexture(g_ren, g_tex, NULL, NULL);
    SDL_RenderPresent(g_ren);
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


