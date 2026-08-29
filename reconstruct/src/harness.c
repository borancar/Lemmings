/*
 * PC Lemmings - reconstructed from VGALEMMI.EXE (lemming1.pc), 1996-12-24.
 * No licence is asserted over this file: it is a transcription of someone
 * else's binary. See CLAUDE.md.
 *
 * ======================================================================
 * THE HARNESS, AND NOTHING ELSE. THIS FILE IS THE PORT'S OWN.
 *
 * game.c holds what the original does. sdl_io.c holds what the hardware used
 * to do. THIS file holds what neither does: the things that exist only so the
 * port can be DRIVEN and MEASURED - a recorded input script, a flat view of a
 * struct for a Python tool, a frame sink, a scancode exposed for a test.
 *
 * It is linked into the core rather than into lemdev, and that is deliberate
 * and worth stating, because the obvious place for harness code is the
 * harness binary and it is the wrong one: reconstruct/tests links game.o
 * data.o sdl_io.o adlib.o and NOT devmain.o, and tools/ reach libgame.so,
 * which is the same set. Anything a test or a tool needs has to be here or in
 * one of those three. Putting it in devmain.c breaks them.
 *
 * WHAT BELONGS HERE is the third answer to "where did this come from". The
 * first two are an image address and "what the card did"; this is "nothing in
 * the original, and no part of the game - it is here to check the game". If a
 * routine has an address it goes to game.c even when it ends in a window; if
 * it replaces a card, a driver or DOS it goes to sdl_io.c; only if it is
 * neither does it come here.
 * ====================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"

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
script_act harness_script[SCRIPT_MAX];
uint16_t   harness_script_n;

void harness_script_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "input script %s: cannot open it\n", path);
        return;
    }
    char line[128];
    while (harness_script_n < SCRIPT_MAX && fgets(line, sizeof line, f)) {
        char verb[16];
        long fr;
        int32_t x = 0, y = 0;
        if (line[0] == '#' || line[0] == '\n') continue;
        int got = sscanf(line, "%ld %15s %d %d", &fr, verb, &x, &y);
        if (got < 2) continue;
        script_act *a = &harness_script[harness_script_n];
        a->frame = fr; a->x = x; a->y = y;
        if (!strcmp(verb, "move") && got == 4) a->what = SCRIPT_MOVE;
        else if (!strcmp(verb, "down")) a->what = SCRIPT_DOWN;
        else if (!strcmp(verb, "up")) a->what = SCRIPT_UP;
        else continue;
        harness_script_n++;
    }
    fclose(f);
    fprintf(stderr, "  [script] %u actions from %s\n", harness_script_n, path);
}

/* THE PORT'S OWN, and it is a CONTROL rather than a convenience.
 *
 * tools/ reaches the lemming array through ctypes, which means a Python
 * declaration of `lemming` sitting beside the C one. Two declarations of one
 * struct is two declarations that will differ, and they did: verify_hit.py's
 * stopped at `state` - sixteen bytes where this is thirty-four - so the C
 * read every lemming after the first at the wrong stride and the tool
 * reported 47 of 256 hits differing. It read exactly like a port bug, in the
 * one routine CLAUDE.md already records as having been sixteen pixels wrong.
 *
 * So the size is exported and lemmings.lem_struct() asserts on it. A tool
 * whose struct has drifted now fails on the assert instead of producing a
 * number about itself. */
uint32_t lemming_record_size(void) { return (uint32_t)sizeof(lemming); }

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

/* THE HARNESS'S VIEWER, and it has no counterpart in the original: lemdev
 * putting a composed screen up so a person can look at it.
 *
 * It lived in sdl_io.c and was a scrollable canvas viewer - arrow keys, a
 * start_x it centred on, clamps at both edges - none of which was reachable,
 * because the only caller is the one below and it passes the source size as
 * the view size. Two wrappers that existed to scroll around a level,
 * view_run and view_run_anim, had no callers at all.
 *
 * It is here rather than there because it is the HARNESS's, and devmain.c is
 * where the harness lives. What it is built from - the window, the queue, the
 * page and the pacing - is sdl_io.c's, and those are the same four the
 * transcribed loops in game.c use.
 */
int32_t dev_dismiss(uint8_t scancode)
{
    return menu_key_code(scancode) == 0x1B ? 1 : -1;
}

void dev_show(const uint8_t *pix, uint16_t w, uint16_t h,
                     const lem_rgb *palette, const char *title, uint8_t scale)
{
    if (scale < 1) scale = 1;
    if (view_open(w, h, title, scale) != 0) return;
    for (;;) {
        if (view_screen_pump(dev_dismiss, 1) != VIEW_CLOSED) return;
        view_screen_present(pix, w, h, palette);
        view_screen_pace();
    }
}

/* ======================================================================
 * THE LEVEL'S FRAME SINK AND ITS INPUT SCRIPT.
 *
 * NOTHING IN THE ORIGINAL. Both exist only so the port can be DRIVEN and
 * MEASURED: the sink writes the 320x200 screen the game actually displays so
 * tools/compare_screen.py can score THAT rather than a second arrangement of
 * the same calls, and the script replays a recorded run so both sides of
 * tools/compare_play.py see one set of clicks.
 *
 * They lived in sdl_io.c among the window's own state, which is the drift
 * this file exists to stop: sdl_io.c is what the CARD did, and a card does
 * not write JSON.
 * ====================================================================== */
static const char *g_sink_dir;
static FILE       *g_sink_state;
static long        g_sink_frame;
static uint32_t    g_script_at;

/* A LEVEL IS ABOUT TO RUN AND SOMETHING WANTS TO WATCH IT. All three are
 * environment variables because the game has no command line and writes no
 * files: setting one is a developer asking, not the game doing it.
 *
 *   LEM_SCREEN_DIR    the 320x200 screen the game actually displays, so
 *                     tools/compare_screen.py can score THAT rather than a
 *                     second arrangement of the same calls
 *   LEM_STATE_FILE    the port's lemming array frame by frame. A screen
 *                     comparison says two lemmings look different; it does
 *                     not say whether the x, the y, the flags or the
 *                     animation counter is the one that moved
 *   LEM_INPUT_SCRIPT  a recorded run, so a comparison can cover the parts of
 *                     the game that need a player
 */
void harness_level_begin(void)
{
    const char *state  = getenv("LEM_STATE_FILE");
    const char *script = getenv("LEM_INPUT_SCRIPT");
    g_sink_dir   = getenv("LEM_SCREEN_DIR");
    g_sink_state = state ? fopen(state, "w") : NULL;
    g_sink_frame = 0;
    g_script_at  = 0;
    if (script) harness_script_load(script);
}

void harness_level_end(void)
{
    if (g_sink_state) fclose(g_sink_state);
    g_sink_state = NULL;
}

int  harness_sink_active(void) { return g_sink_dir != NULL; }
long harness_frame_no(void)    { return g_sink_frame; }

/* One frame out: the raw screen with its palette, and a line of state. The
 * container holds ONE palette and the play area is what gets scored, so it is
 * the top one - the split is a fact about the screen, not about this file. */
void harness_sink_frame(const uint8_t *screen, const lem_rgb *top,
                        struct level_frame *A)
{
    if (g_sink_dir) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/f%05ld.raw", g_sink_dir, g_sink_frame);
        FILE *f = fopen(path, "wb");
        if (f) {
            uint16_t wh[2] = { LEVEL_SCREEN_W, LEVEL_SCREEN_H };
            fwrite("LEMRW2", 1, 6, f);
            fwrite(wh, sizeof(uint16_t), 2, f);
            for (uint8_t i = 0; i < 16; i++) {
                fputc(top[i].r, f); fputc(top[i].g, f); fputc(top[i].b, f);
            }
            fwrite(screen, 1, LEVEL_SCREEN_W * LEVEL_SCREEN_H, f);
            fclose(f);
        }
    }
    if (g_sink_state) {
        fprintf(g_sink_state, "{\"frame\":%ld,\"released\":%u,\"live\":%u,"
                "\"saved\":%u,\"over\":%u,\"tick\":%u,\"view_x\":%d,\"lem\":[",
                g_sink_frame, (unsigned)A->released,
                (unsigned)lemmings_live(A->lem, A->released),
                (unsigned)level_saved, (unsigned)g_level_over,
                (unsigned)level_tick_now(), (int)A->view_x);
        for (uint8_t k = 0; k < A->released && k < LEMMING_MAX; k++) {
            const lemming *a = &A->lem[k];
            /* The FUSE and the direction are in here because the nuke is a
             * thing that happens to the fuse and to nothing else for
             * seventy-nine frames: a comparison of position and flags alone
             * agrees all the way from the double click to the first
             * explosion, and would have said the cascade was right whatever
             * order it lit them in. */
            fprintf(g_sink_state,
                    "%s{\"x\":%d,\"y\":%d,\"hot_x\":%d,\"hot_y\":%d,"
                    "\"flags\":%u,\"anim\":%u,\"alive\":%u,"
                    "\"fuse\":%u,\"dir\":%d,\"state\":%u}",
                    k ? "," : "", a->x, a->y, a->hot_x, a->hot_y,
                    (unsigned)a->flags, (unsigned)a->anim,
                    (unsigned)a->alive, (unsigned)a->fuse,
                    (int)a->dir, (unsigned)a->state);
        }
        fprintf(g_sink_state, "]}\n");
    }
    g_sink_frame++;
}

/* The recorded run, replayed. It speaks BEFORE the host's queue is read, so a
 * scripted frame behaves the same whether or not an event was waiting.
 *
 * A scripted pointer is re-asserted rather than set once: under
 * SDL_VIDEODRIVER=dummy pygame still delivers a MOUSEMOTION and the pointer
 * wanders back, which CLAUDE.md records costing two rounds of reading
 * disassembly for a clamp that does not exist. */
int harness_script_step(struct level_frame *A, int32_t *scroll_x)
{
    int moved = 0;
    while (g_script_at < harness_script_n
            && harness_script[g_script_at].frame <= g_sink_frame) {
        const script_act *act = &harness_script[g_script_at++];
        if (act->what == SCRIPT_MOVE) {
            /* The script is in SCREEN pixels; the game's own pointer sits
             * eight further on, which is where image 0x0426D clamps. */
            lem_mouse_move(act->x + 8, act->y + 8);
            moved = 1;
        } else if (act->what == SCRIPT_DOWN) {
            lem_buttons |= 1;                       /* ds:0x14 bit 0 */
            level_panel_click(A, scroll_x);         /* image 0x03168 */
        } else if (act->what == SCRIPT_UP) {
            lem_buttons &= (uint8_t)~1u;
        }
    }
    return moved;
}
