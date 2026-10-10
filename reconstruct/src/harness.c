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

#define _POSIX_C_SOURCE 200809L         /* nanosleep, for the follow wait */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
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
 *     30 rdown             press the RIGHT button - ds:0x14 bit 1, which
 *     40 rup               image 0x03DB7 reads to pick the plain lemming
 *                          under the pointer, and image 0x040F0 to double
 *                          the edge scroll
 *
 * Frames are this loop's own count, which is the index the frame sink uses,
 * so a script and a captured sequence line up by construction. */
script_act harness_script[SCRIPT_MAX];
uint16_t   harness_script_n;

/* One line of a script into the next free slot. 1 if it was an action. */
static int script_parse_line(const char *line)
{
    char verb[16];
    long fr;
    int32_t x = 0, y = 0;
    if (harness_script_n >= SCRIPT_MAX) return 0;
    if (line[0] == '#' || line[0] == '\n') return 0;
    int got = sscanf(line, "%ld %15s %d %d", &fr, verb, &x, &y);
    if (got < 2) return 0;
    {
        script_act *a = &harness_script[harness_script_n];
        a->frame = fr; a->x = x; a->y = y;
        if (!strcmp(verb, "move") && got == 4) a->what = SCRIPT_MOVE;
        else if (!strcmp(verb, "down")) a->what = SCRIPT_DOWN;
        else if (!strcmp(verb, "up")) a->what = SCRIPT_UP;
        else if (!strcmp(verb, "rdown")) a->what = SCRIPT_RDOWN;
        else if (!strcmp(verb, "rup")) a->what = SCRIPT_RUP;
        /* A KEY IS A SCANCODE, the byte port 0x60 produced - not a character
         * and not SDL's numbering. Replayed through image 0x016D4's own
         * handler, so ds:0x0F78, ds:0x1FC7 and ds:0x1FD0 all end up holding
         * what they would have held. */
        else if (!strcmp(verb, "key")) a->what = SCRIPT_KEY;
        else if (!strcmp(verb, "keyup")) a->what = SCRIPT_KEYUP;
        else return 0;
        harness_script_n++;
    }
    return 1;
}

void harness_script_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "input script %s: cannot open it\n", path);
        return;
    }
    char line[128];
    while (harness_script_n < SCRIPT_MAX && fgets(line, sizeof line, f))
        script_parse_line(line);
    fclose(f);
    fprintf(stderr, "  [script] %u actions from %s\n", harness_script_n, path);
}

/* FOLLOWING A SCRIPT THAT IS STILL BEING WRITTEN - LEM_INPUT_FOLLOW=1.
 *
 * tools/lockstep.py has a person play the ORIGINAL and writes what its frame
 * loop read, frame by frame, into the same format this replays. After the
 * lines for frame N it writes `# frame N`, and that comment is the promise
 * that nothing more is coming for N - so the port, about to run frame N,
 * waits for it rather than running ahead of a player it has not heard from.
 * A comment because every other reader of the format skips comments, so the
 * file the run leaves behind replays like any recording.
 *
 * Read a character at a time and kept across calls, because the writer is
 * another process and the end of the file can fall in the middle of a line. */
static FILE *g_follow;
static long  g_follow_done = -1;            /* the last `# frame N` seen */
static char  g_follow_line[128];
static uint32_t g_follow_len;

int harness_input_follows(void) { return g_follow != NULL; }

static void script_follow_to(long frame)
{
    while (g_follow_done < frame) {
        int c = fgetc(g_follow);
        if (c == EOF) {
            struct timespec ts = { 0, 2000000 };     /* 2 ms */
            clearerr(g_follow);
            nanosleep(&ts, NULL);
            continue;
        }
        if (g_follow_len < sizeof g_follow_line - 1)
            g_follow_line[g_follow_len++] = (char)c;
        if (c != '\n') continue;
        g_follow_line[g_follow_len] = 0;
        g_follow_len = 0;
        long n;
        if (sscanf(g_follow_line, "# frame %ld", &n) == 1) g_follow_done = n;
        else if (g_follow_line[0] == '#' && strstr(g_follow_line, "# end"))
            g_follow_done = 0x7FFFFFFF;          /* the player has gone */
        else script_parse_line(g_follow_line);
    }
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
void dev_show(const uint8_t *pix, uint16_t w, uint16_t h,
                     const lem_rgb *palette, const char *title, uint8_t scale)
{
    if (scale < 1) scale = 1;
    if (view_open(w, h, title, scale) != 0) return;
    video_screen_set(pix, w, h, palette, NULL, 0);
    for (;;) {
        /* The same two the transcribed screens poll - see mouse_buttons and
         * key_held. There is one queue and one handler behind both. */
        if (mouse_buttons() & 7) return;
        if (key_held(KEY_ESCAPE)) return;
        video_page_flip();
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
static FILE       *g_dg_file;
/* The recorded keys waiting for the next wait - see harness_script_keys. */
#define SCRIPT_KEYS_MAX 16
static uint8_t     g_keys_due[SCRIPT_KEYS_MAX];
static uint32_t    g_keys_n;

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
    /* THE DATA SEGMENT, frame by frame - LEM_DG_FILE. What the guest's own DS
     * is compared against: sizeof(dgroup) raw bytes a frame, captured at the
     * same point the guest's hook captures, so tools/verify_dgroup.py can diff
     * every field the port claims to know. Nothing in the original writes a
     * file; this is here for the same reason the frame sink is. */
    {
        const char *dgf = getenv("LEM_DG_FILE");
        g_dg_file = dgf ? fopen(dgf, "wb") : NULL;
    }
    g_sink_dir   = getenv("LEM_SCREEN_DIR");
    g_sink_state = state ? fopen(state, "w") : NULL;
    g_sink_frame = 0;
    g_script_at  = 0;
    g_keys_n     = 0;
    harness_script_n = 0;
    if (g_follow) fclose(g_follow);
    g_follow = NULL;
    g_follow_done = -1;
    g_follow_len = 0;
    if (script && getenv("LEM_INPUT_FOLLOW")) {
        g_follow = fopen(script, "r");
        if (!g_follow)
            fprintf(stderr, "input script %s: cannot open it\n", script);
    } else if (script) harness_script_load(script);
}

void harness_level_end(void)
{
    if (g_sink_state) fclose(g_sink_state);
    g_sink_state = NULL;
    if (g_dg_file) fclose(g_dg_file);
    g_dg_file = NULL;
}

int  harness_sink_active(void) { return g_sink_dir != NULL; }
long harness_frame_no(void)    { return g_sink_frame; }

/* ======================================================================
 * RECORDING A RUN - the mirror of replaying one, and beside it for the reason
 * dat_free sits beside dat_load: the two know one format, and a verb added to
 * one and not the other is a silent hole.
 *
 * WHAT IS RECORDED is what the game READS, not what the host sent: ds:0x10 and
 * ds:0x12 for the pointer, ds:0x14's bit 0 for the button, and a scancode when
 * ds:0x1FD0 changes. So a replay puts the same bytes in the same places rather
 * than reproducing a sequence of SDL events, and the recording is as valid
 * against the ORIGINAL - which has ds:0x14 and no SDL at all - as against the
 * port. That is the whole point of it: one file drives both sides.
 *
 * It is written a FRAME at a time, on the harness's own frame counter, because
 * a wall-clock timestamp would replay differently on a different machine and
 * the thing being compared is what the game did on frame N.
 *
 * Nothing in the original records anything. The flag is devmain.c's.
 * ====================================================================== */
static FILE   *g_rec;
static int32_t g_rec_x = -1, g_rec_y = -1;
static uint8_t g_rec_btn, g_rec_rbtn, g_rec_key;

void harness_record_open(const char *path)
{
    g_rec = fopen(path, "w");
    if (!g_rec) {
        fprintf(stderr, "  [record] %s: cannot open it for writing\n", path);
        return;
    }
    /* WHICH LEVEL IT IS OF, because a recording that does not say can be
     * replayed against a different one - and this project has measured what
     * that looks like: both sides run the same SIMULATION, the panel's OUT, IN
     * and TIME agree to the character, and the terrain is a different graphics
     * set entirely. It scored 0 of 535 frames and read as the port being
     * comprehensively broken.
     *
     * ds:0x1F6B is the game's own running order and ds:0x1F6E the rating, so
     * these are the two numbers both drivers take. */
    fprintf(g_rec, "# recorded by lemdev --record-input\n"
                   "# level %u  rating %u   (ds:0x1F6B, ds:0x1F6E)\n"
                   "# frames are the harness's own count; move is in SCREEN\n"
                   "# pixels, which is ds:0x10 less the eight image 0x0426D\n"
                   "# clamps against; key is a SCANCODE.\n",
            (unsigned)DG.level_index, (unsigned)DG.level_rating);
    g_rec_x = g_rec_y = -1;
    g_rec_btn = g_rec_rbtn = g_rec_key = 0;
}

/* One frame's worth, and only what CHANGED - a script is a list of edges over
 * a polled state, so a frame where nothing moved writes nothing. */
static void harness_record_frame(long frame)
{
    int32_t x, y;
    uint8_t btn, k;
    if (!g_rec) return;

    x = (int32_t)lem_mouse_x - 8;        /* the script is in screen pixels */
    y = (int32_t)lem_mouse_y - 8;
    if (x != g_rec_x || y != g_rec_y) {
        fprintf(g_rec, "%ld move %d %d\n", frame, x, y);
        g_rec_x = x; g_rec_y = y;
    }

    btn = (uint8_t)(lem_buttons & 1);
    if (btn != g_rec_btn) {
        fprintf(g_rec, "%ld %s\n", frame, btn ? "down" : "up");
        g_rec_btn = btn;
    }
    /* AND THE RIGHT BUTTON, bit 1, which this left out. It is the one that
     * makes image 0x03DB7 take the PLAIN lemming under the pointer rather
     * than the busy one, so a recording of someone using it replayed as a
     * different game on both sides. */
    btn = (uint8_t)((lem_buttons >> 1) & 1);
    if (btn != g_rec_rbtn) {
        fprintf(g_rec, "%ld %s\n", frame, btn ? "rdown" : "rup");
        g_rec_rbtn = btn;
    }

    /* ds:0x1FD0 holds the last scancode and image 0x016D4 clears it on any
     * release, so a change to non-zero is a press and a change to zero is the
     * release of whatever was held. */
    k = key_last();
    if (k != g_rec_key) {
        if (k) fprintf(g_rec, "%ld key %u\n", frame, (unsigned)k);
        else   fprintf(g_rec, "%ld keyup %u\n", frame, (unsigned)g_rec_key);
        g_rec_key = k;
    }
    fflush(g_rec);
}

void harness_record_close(void)
{
    if (g_rec) fclose(g_rec);
    g_rec = NULL;
}

/* One frame out: the raw screen with its palette, and a line of state. The
 * container holds ONE palette and the play area is what gets scored, so it is
 * the top one - the split is a fact about the screen, not about this file. */
void harness_sink_frame(const uint8_t *screen, const lem_rgb *top,
                        struct level_frame *A)
{
    /* A DEAD PALETTE IS NOT A QUIET CAPTURE. view_level_palette() returned
     * g_lv_top for a while - an array nothing wrote, left behind when the
     * level's slots moved to game.c - so every frame written here carried
     * sixteen black entries while its INDICES were perfect. Nothing caught it:
     * compare_screen.py and compare_run.py both score indices, which is what
     * makes them independent of a palette sampled at one moment, and this
     * file's own notes say matching indices is not matching colours. It showed
     * up only when a picture was finally looked at. */
    if (g_sink_dir && top) {
        static int said;
        uint8_t lit = 0;
        for (uint8_t i = 0; i < 16; i++)
            if (top[i].r | top[i].g | top[i].b) lit = 1;
        if (!lit && !said) {
            said = 1;
            fprintf(stderr, "  [sink] the palette written with these frames is "
                            "ALL BLACK - the indices may be right and no index "
                            "comparison will tell you\n");
        }
    }
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
    /* The whole segment, padding included - the FIELDS are chosen in the
     * comparison, not here, so a field added to dgroup.h needs no change to
     * this file or to the capture format. */
    if (g_dg_file) fwrite(&DG, 1, sizeof DG, g_dg_file);
    /* BEFORE the counter moves, so a recorded frame number means the same
     * thing to harness_script_step, which compares against g_sink_frame. */
    harness_record_frame(g_sink_frame);
    g_sink_frame++;
}

/* The recorded run, replayed. It speaks BEFORE the host's queue is read, so a
 * scripted frame behaves the same whether or not an event was waiting.
 *
 * A scripted pointer is re-asserted rather than set once: under
 * SDL_VIDEODRIVER=dummy pygame still delivers a MOUSEMOTION and the pointer
 * wanders back, which CLAUDE.md records costing two rounds of reading
 * disassembly for a clamp that does not exist. */
/* A RECORDED KEY WAITS FOR THE TICKS. Image 0x00194 clears ds:0x1FD0 at the
 * top of every frame, and image 0x03E6F reads it on the timer ticks that
 * fall in the wait after the frame's work - so a key handed over with the
 * frame's mouse would be cleared before anything could see it.
 * tools/record_level.py and tools/lockstep.py press theirs at the same
 * moment, image 0x00105, once the frame's work is done. */
void harness_script_keys(void)
{
    for (uint32_t i = 0; i < g_keys_n; i++)
        keyboard_isr(g_keys_due[i]);                /* image 0x016D4 */
    g_keys_n = 0;
}

int harness_script_step(struct level_frame *A, int32_t *scroll_x)
{
    int moved = 0;
    (void)scroll_x;
    if (g_follow) {
        /* Everything already acted on can go, so a live run is not bounded
         * by SCRIPT_MAX the way a loaded file is. */
        if (g_script_at == harness_script_n) g_script_at = harness_script_n = 0;
        script_follow_to(g_sink_frame);
    }
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
            level_play_press(A);                    /* image 0x03150 */
        } else if (act->what == SCRIPT_UP) {
            lem_buttons &= (uint8_t)~1u;
        } else if (act->what == SCRIPT_RDOWN) {
            lem_buttons |= 2;                       /* ds:0x14 bit 1 */
        } else if (act->what == SCRIPT_RUP) {
            lem_buttons &= (uint8_t)~2u;
        } else if ((act->what == SCRIPT_KEY || act->what == SCRIPT_KEYUP)
                   && g_keys_n < SCRIPT_KEYS_MAX) {
            g_keys_due[g_keys_n++] = (uint8_t)((act->x & 0x7F)
                                     | (act->what == SCRIPT_KEYUP ? 0x80 : 0));
        }
    }
    return moved;
}
