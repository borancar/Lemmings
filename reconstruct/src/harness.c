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
