/* devmain.c - the developer harness.
 *
 * THIS FILE IS THE PORT'S OWN, and it is deliberately not main.c. The
 * original had no command line: it starts, shows its menu, and plays. main.c
 * mirrors that. Everything here exists so a screen can be rendered to a file
 * and compared against the emulator, or one piece asked for on its own - and
 * none of it corresponds to anything in vgalemmi.exe.
 *
 * Keeping the two apart matters more than it looks. When the flags lived in
 * main.c, the program that a reader would take for "the game's entry point"
 * was mostly argument parsing, and the actual start-up - the thing the
 * original does - was a few lines lost in the middle of it.
 *
 *   ./lemdev --screen menu --raw out/menu.raw
 *   ./lemdev --rating taxing --number 1 --raw out/level.raw
 *   ./lemdev --screen briefing --show
 *
 * There is no --out. It wrote a BMP and nothing wanted one: --raw carries
 * palette INDICES, which is what a comparison against the original's video
 * memory needs and what a BMP destroys, since two different indices can share
 * a colour. Pictures are tools/diff_png.py's job - it draws three and marks
 * the difference. prove.sh had been passing `--out /dev/null` for years.
 *
 * It reads the player's own copy of the original data files; nothing of the
 * game is redistributed here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "opl.h"
#include "adlib.h"

#include "game.h"

/* ODDTABLE.DAT's entry for the plate being rendered - image 0x019C9's bit.
 * --file names a file and a section and cannot know which PLATE that is, so
 * a comparison against a recorded run has to be told. -1 is "its own". */
static int g_odd_entry = -1;
/* --triggers FILE: the port's trigger map, for tools/verify_triggers.py.
 * 64 rows of 0x1A0, the same shape the original keeps at ds:0x12 with
 * image 0x03874's `mul 0x1a0` for a stride. */
static const char *g_triggers_out = NULL;






static const char *SKILL_NAMES[8] = {
    "climber", "floater", "bomber", "blocker",
    "builder", "basher", "miner", "digger"
};

static void usage(void)
{
    fputs("usage: lemmings [--data DIR] [--menu|--level N] [--raw FILE]\n"
          "  with no arguments at all, shows the main menu in a window\n"
          "  --data DIR    directory holding the original files (default .)\n"
          "  --menu        the main menu (the default screen)\n"
          "  --screen S    menu | briefing | code | input - which screen\n"
          "  --rating R    fun | tricky | taxing | mayhem (default fun)\n"
          "  --scroll N    banner ribbon position in pixels (default 0)\n"
          "  --lem-frame N banner end lemmings, frame 0..15 (default 0)\n"
          "  --obj-frame N the frame number within the level (default 0)\n"
          "  --number N    the GAME's level number, 1-30 (default 1)\n"
          "                resolved through the original's own order table,\n"
          "                so --rating fun --number 1 is \"Just dig!\"\n"
          "  --file NAME   force a container; --level is then a raw section\n"
          "  --level N     section inside --file, 1-based\n"
          "  --show        show it as well, when --raw asked for a file\n"
          "  --scale N     window magnification (default 3)\n"
          "  --terrain M   how terrain pixels combine: replace|or|keep\n"
          "  --planebits O which bit each stored plane feeds, e.g. 0123\n"
          "  --bitorder B  leftmost pixel is the byte's msb or lsb\n"
          "  --reverse     draw the terrain list back to front\n"
          "  --no-objects  terrain only, as the original composes it first\n"
          "  --baked       bake objects into the terrain (NOT what the\n"
          "                original does; for comparing the two)\n"
          "  --panel FILE  write the 320x40 control panel on its own\n"
          "  --raw FILE    also write the raw indices and palette, for\n"
          "                checking against the original: 'LEMRW2', width\n"
          "                and height as u16, 16 RGB triples, then w*h bytes\n",
          stderr);
}

/* Load a level, compose it, and do whatever the command line asked for
 * with the result. Split out of main() so the menu can start a level and
 * come back, which is what makes the menu a menu rather than a picture. */
/* Load and parse one level. Split out so the briefing screen can read the same
 * level the renderer is about to draw, without loading it twice in two places
 * that could drift apart. */
/* Image 0x0E691, four six-character slots: "Fun   TrickyTaxingMayhem". These
 * are for the port's own printf; the plate itself is drawn from the game's
 * copy of the same string. */

static int load_level(const char *data, const char *file, uint32_t which,
                      level *lv, const char **err)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", data, file);
    dat_file levels;
    if (dat_load(path, &levels, err) != 0)
        return -1;
    if (which < 1 || which > levels.count) {
        dat_free(&levels);
        if (err) *err = "no such level in that container";
        return -1;
    }
    int rc = level_parse(levels.sections[which - 1].data,
                         levels.sections[which - 1].size, lv, err);
    dat_free(&levels);
    if (rc == 0 && level_apply_odd(data, lv, level_odd_taken(), err) != 0)
        return -1;
    /* The LVL does not carry its own number; the briefing prints one. This is
     * the container index, which is NOT what the game calls the level - see
     * the note in main() about level009.dat. */
    /* Filled in by the caller when it knows the game's own number; the
     * container index is NOT it, and printing that gave "Level 2" where the
     * original says "Level 1". */
    lv->level_number = 0;
    return rc;
}

/* The frame number within a level. A byte wraps at 256 and the
 * five-minute timer runs to 5,100 frames, so this is a word. */
static uint16_t g_obj_frame;
static lemming g_lem[LEMMING_MAX];
/* Bake the objects into the terrain instead of drawing them as the
 * original does. Only for comparing the two - see render_objects. */
static int g_baked_objects;
/* --rate-probe: the release rate after each of a scripted run of clicks, for
 * tools/verify_rate.py to hold against the original's own image 0x04032. The
 * port's rate is a pair of globals rather than an argument, so it cannot be
 * called from outside with a value - it has to be walked from the level's own
 * rate, which is what the game does too. */
static int g_rate_probe;

static int render_level(const char *data, const char *file, uint32_t which,
                        int objects, const char *raw, const char *panel_out,
                        uint8_t scale, int show)
{
    char path[1024];
    const char *err = "unknown error";
    snprintf(path, sizeof(path), "%s/%s", data, file);
    dat_file levels;
    if (dat_load(path, &levels, &err) != 0) {
        fprintf(stderr, "%s: %s\n", path, err);
        return 1;
    }
    if (which < 1 || which > levels.count) {
        fprintf(stderr, "%s holds %u level(s); asked for %u\n",
                path, levels.count, which);
        dat_free(&levels);
        return 1;
    }

    level lv;
    if (level_parse(levels.sections[which - 1].data,
                    levels.sections[which - 1].size, &lv, &err) != 0) {
        fprintf(stderr, "level %u: %s\n", which, err);
        dat_free(&levels);
        return 1;
    }
    /* The plate's own numbers, if this plate has any - image 0x019F5. */
    if (level_apply_odd(data, &lv, g_odd_entry, &err) != 0) {
        fprintf(stderr, "oddtable: %s\n", err);
        dat_free(&levels);
        return 1;
    }
    dat_free(&levels);

    printf("%s  (level %u of %s)\n", lv.name, which, file);
    printf("  %u lemmings, save %u (%u%%), release rate %u, %u minutes\n",
           lv.num_lemmings, lv.num_to_rescue,
           lv.num_lemmings ? lv.num_to_rescue * 100u / lv.num_lemmings : 0u,
           lv.release_rate, lv.time_limit);
    printf("  skills:");
    for (uint8_t i = 0; i < 8; i++)
        if (lv.skills[i])
            printf(" %s=%u", SKILL_NAMES[i], lv.skills[i]);
    printf("\n  graphics set %u, start x %u\n", lv.graphics_set, lv.start_x);
    printf("  %u terrain pieces, %u objects, %u steel areas\n",
           lv.num_terrain, lv.num_objects, lv.num_steel);
    /* WHAT THE OBJECTS DO, which is not visible from the level's header at
     * all: the effect is a field of the GROUND file's object record, image
     * 0x037E3's [si+0x14]. Three faults reported from playing - a missing
     * death scene, no drowning, no trap - were all effects 4, 5 and 6 of
     * image 0x02F0F's dispatch, and there was no way to ask a level which
     * of them it uses. */
    {
        char gp[256];
        ground gg;
        const char *ge = NULL;
        snprintf(gp, sizeof(gp), "%s/ground%uo.dat", data,
                 (unsigned)(lv.graphics_set & 0x0F));
        if (ground_load(gp, &gg, &ge) == 0) {
            printf("  object effects (N no-overwrite, T only-on-terrain):");
            for (uint16_t i = 0; i < lv.num_objects; i++) {
                uint8_t id = lv.objects[i].id;
                if (id >= GROUND_OBJECTS) continue;
                printf(" [%u]id%u=%u%s%s", i, id,
                       gg.objects[id].trigger_effect,
                       lv.objects[i].no_overwrite ? "N" : "",
                       lv.objects[i].only_on_terrain ? "T" : "");
            }
            printf("\n");
        }
    }

    ground g;
    snprintf(path, sizeof(path), "%s/ground%uo.dat", data, lv.graphics_set);
    if (ground_load(path, &g, &err) != 0) {
        fprintf(stderr, "%s: %s\n", path, err);
        return 1;
    }

    /* THE TRIGGER MAP, before anything draws - the thing image 0x03839's
     * steel pass and image 0x037C2's object pass build between them, and the
     * only place the game answers "what is under this lemming". */
    if (g_triggers_out) {
        level_triggers_build(&lv, &g);
        FILE *tf = fopen(g_triggers_out, "wb");
        if (!tf) { fprintf(stderr, "%s: cannot write\n", g_triggers_out); return 1; }
        for (int32_t row = 0; row < 64; row++)
            for (int32_t col = 0; col < 0x1A0; col++)
                fputc(level_trigger_cell(col, row), tf);
        fclose(tf);
        fprintf(stderr, "  wrote the port's trigger map to %s\n", g_triggers_out);
    }

    snprintf(path, sizeof(path), "%s/vgagr%u.dat", data, lv.graphics_set);
    dat_file gfx;
    if (dat_load(path, &gfx, &err) != 0) {
        fprintf(stderr, "%s: %s\n", path, err);
        return 1;
    }
    if (gfx.count < 1) {
        fprintf(stderr, "%s: no terrain section\n", path);
        dat_free(&gfx);
        return 1;
    }

    canvas c;
    if (canvas_init(&c) != 0) {
        fprintf(stderr, "out of memory\n");
        dat_free(&gfx);
        return 1;
    }
    if (render_terrain(&c, &lv, &g, gfx.sections[0].data,
                       gfx.sections[0].size, &err) != 0) {
        fprintf(stderr, "render: %s\n", err);
        canvas_free(&c);
        dat_free(&gfx);
        return 1;
    }
    /* The lemmings and the minimap both probe the TERRAIN, and the buffer the
     * original probes has no objects in it - they are drawn at display time.
     * So a copy is kept before they go on. */
    if (g_rate_probe) {
        /* The rate walked exactly as a player walks it: from the level's own
         * value up to the ceiling and back down to the floor, printing what
         * the panel would show and the interval the release counts from. */
        level_release_reset(&lv);
        printf("floor %u\n", level_rate_floor());
        /* THE DIVIDER IS CLEARED BEFORE EACH CLICK, exactly as
         * tools/verify_rate.py pokes ds:0x69 to 0 before each call on the
         * guest side. Both sides are then testing the STEP arithmetic and its
         * two bounds with the pacing held out of the way - the same claim on
         * each, which is the whole point of the comparison. The pacing itself
         * is level_rate_tick's, and screens.c tests that. */
        for (int k = 0; k < 120; k++) {
            level_rate_div_reset();
            level_rate_click(1);
            printf("up %u %u\n", level_rate(), level_release_interval());
        }
        for (int k = 0; k < 120; k++) {
            level_rate_div_reset();
            level_rate_click(0);
            printf("down %u %u\n", level_rate(), level_release_interval());
        }
        dat_free(&gfx);
        canvas_free(&c);
        return 0;
    }

    uint8_t *terrain = malloc((size_t)c.w * c.h);
    if (!terrain) {
        fprintf(stderr, "out of memory\n");
        canvas_free(&c);
        dat_free(&gfx);
        return 1;
    }
    memcpy(terrain, c.pix, (size_t)c.w * c.h);

    if (objects && gfx.count >= 2) {
        if (g_baked_objects) {
            /* render_objects bakes the objects into the terrain, which is
             * NOT what the original does and says so where it is defined.
             * Kept reachable so the two can be compared, but it is not what
             * --raw uses any more: against a recorded frame it loses the
             * entrance's torch flames and the colour inside its arch, 91 of
             * the 134 pixels that differed before any lemming had dropped. */
            if (render_objects(&c, &lv, &g, gfx.sections[1].data,
                               gfx.sections[1].size,
                               /* render_objects indexes a frame, not a
                                * frame number; no object has more than
                                * eighteen. */
                               (uint8_t)(g_obj_frame & 0xFF),
                               &err) != 0) {
                fprintf(stderr, "render objects: %s\n", err);
                canvas_free(&c);
                dat_free(&gfx);
                return 1;
            }
        } else {
            /* What the original does: the objects are animated sprites drawn
             * over the composed terrain at display time, image 0x07BB8. The
             * window path has always used this; --raw used the baked one,
             * which is why every frame comparison carried the same residue
             * around the entrance. */
            static level_anim st[LEVEL_ANIM_MAX];
            level_anim_init(st, &lv, &g);
            /* One step before the first frame. Measured: without it the
             * exit's flames are exactly one animation frame behind the
             * original's at every frame checked. */
            level_anim_step(st, &lv);
            level_start_reset();
            level_release_reset(&lv);
            level_status_reset(&lv);
            lemmings_reset(g_lem);
            {
                /* Run to the wanted frame through the GAME's own per-frame
                 * routine, the same one the window path calls - see
                 * level_play_step. This loop used to be a second copy of
                 * that sequence, and the window's copy was missing the half
                 * of it that draws, which is how the playable game came to
                 * show an empty level while every frame here matched.
                 *
                 * The lemmings probe the TERRAIN, which is what the canvas
                 * held before the objects went on it - so a copy is kept. */
                uint8_t was = level_released();
                for (uint16_t k = 0; k < g_obj_frame; k++)
                    level_play_step(st, &lv, g_lem, &was, terrain, c.w, c.h);

                /* main.dat for the lemming sprites, so the draw can be the
                 * game's single call rather than a second arrangement. */
                char mpath2[1024];
                dat_file mainf2;
                const char *e2 = "unknown error";
                const uint8_t *lemsec = NULL;
                uint32_t lemlen = 0;
                int have_lem = 0;
                snprintf(mpath2, sizeof(mpath2), "%s/main.dat", data);
                if (objects && dat_load(mpath2, &mainf2, &e2) == 0
                        && mainf2.count > LEM_SPRITE_SECTION) {
                    lemsec = mainf2.sections[LEM_SPRITE_SECTION].data;
                    lemlen = mainf2.sections[LEM_SPRITE_SECTION].size;
                    have_lem = 1;
                }
                if (getenv("LEM_DEBUG"))
                    for (uint8_t k = 0; k < was && k < LEMMING_MAX; k++)
                        fprintf(stderr, "  lem %u: x=%d y=%d dir=%d anim=%u "
                                "state=%u alive=%u -> canvas (%d,%d)\n", k,
                                g_lem[k].x, g_lem[k].y, g_lem[k].dir,
                                g_lem[k].anim, g_lem[k].state, g_lem[k].alive,
                                g_lem[k].x + g_lem[k].hot_x - LEMMING_DX,
                                g_lem[k].y + g_lem[k].hot_y - LEMMING_DY);

                level_play_draw(st, &lv, gfx.sections[1].data,
                                gfx.sections[1].size, lemsec, lemlen,
                                g_lem, was, c.pix, c.w, c.h, 0);
                if (have_lem) dat_free(&mainf2);
            }
        }
    }
    dat_free(&gfx);

    if (raw) {
        /* Palette indices, one byte per pixel, for a pixel-for-pixel
         * comparison against the original's video memory. A picture cannot
         * carry that check: BMP round-tripping through a palette loses which
         * index a pixel was, and two different indices can share a colour. */
        FILE *rf = fopen(raw, "wb");
        if (!rf) {
            fprintf(stderr, "%s: cannot open for writing\n", raw);
        } else {
            uint16_t wh[2] = { c.w, c.h };
            fwrite("LEMRW2", 1, 6, rf);
            fwrite(wh, sizeof(uint16_t), 2, rf);
            /* The palette too: a checker must compare what the port would
             * actually display, not re-derive a palette from the data files
             * and check its own arithmetic against itself. */
            for (uint8_t i = 0; i < 16; i++) {
                fputc(g.palette[i].r, rf);
                fputc(g.palette[i].g, rf);
                fputc(g.palette[i].b, rf);
            }
            fwrite(c.pix, 1, (size_t)c.w * c.h, rf);
            fclose(rf);
            printf("wrote %s (raw indices)\n", raw);
        }
    }

    /* The control panel is a separate 320x40 image, drawn at screen row 160
     * over the play area rather than inside the 1584-wide canvas, so it is
     * written as its own file rather than being pasted into one corner of
     * the level. Its artwork is main.dat section 2 - see panel_frame. */
    if (panel_out) {
        char mpath[1024];
        dat_file mainf;
        snprintf(mpath, sizeof(mpath), "%s/main.dat", data);
        if (dat_load(mpath, &mainf, &err) != 0) {
            fprintf(stderr, "%s: %s\n", mpath, err);
        } else if (mainf.count <= PANEL_FONT_SECTION) {
            fprintf(stderr, "%s: no section %d\n", mpath, PANEL_FONT_SECTION);
            dat_free(&mainf);
        } else {
            const uint8_t *sec = mainf.sections[PANEL_FONT_SECTION].data;
            uint32_t seclen = mainf.sections[PANEL_FONT_SECTION].size;
            /* The panel is written out at its LOGICAL width, 352, not the
             * 320 the screen shows - the artwork sits 32 pixels into that
             * line and the status text starts at its very first cell. A
             * checker windows it from PANEL_ART_X. */
            uint8_t *pp = calloc((size_t)PANEL_LINE_W * PANEL_ART_H, 1);
            if (!pp) {
                fprintf(stderr, "out of memory\n");
            } else {
                /* The sequence lives in game.c, because it is the game's -
                 * see level_panel_build. This used to be written out here,
                 * and the window path had no copy at all. A static render
                 * does not scroll, so the view is where the level starts. */
                level_panel_build(pp, sec, seclen, &lv, terrain, c.w, c.h,
                                  g_lem, level_released(), lv.start_x,
                                  PANEL_SEL_BUTTON);
                /* The status line is NOT drawn. It belongs sixteen rows down
                 * - screen row 176, which is PANEL_BASE / PANEL_STRIDE - but
                 * what writes the counts into the template has not been read,
                 * and panel_status blanks the cells it is given, so drawing
                 * the bare template puts "230." where the original has real
                 * counts and erases the artwork under the rest. Measured: the
                 * artwork alone scores 89.31% of the panel, the artwork plus
                 * the bare template 65.65%. Drawing a placeholder is worse
                 * than drawing nothing, and it would flatter nobody. */
                FILE *pf = fopen(panel_out, "wb");
                if (!pf) {
                    fprintf(stderr, "%s: cannot open for writing\n", panel_out);
                } else {
                    uint16_t wh[2] = { PANEL_LINE_W, PANEL_ART_H };
                    fwrite("LEMRW2", 1, 6, pf);
                    fwrite(wh, sizeof(uint16_t), 2, pf);
                    for (uint8_t i = 0; i < 16; i++) {
                        fputc(g.palette[i].r, pf);
                        fputc(g.palette[i].g, pf);
                        fputc(g.palette[i].b, pf);
                    }
                    fwrite(pp, 1, (size_t)PANEL_LINE_W * PANEL_ART_H, pf);
                    fclose(pf);
                    printf("wrote %s (panel, raw indices)\n", panel_out);
                }
                free(pp);
            }
            dat_free(&mainf);
        }
    }

    /* Show it unless a file was the whole point of the run - and show it
     * through THE GAME'S OWN level view, not a second one.
     *
     * This used to call view_run, which is a 320x160 window of the static
     * canvas: no panel, no lemmings, no stepping. So `lemdev --show` looked
     * nothing like `./lemmings`, and the harness could not have caught the
     * window path being wrong because it never ran it. level_show is what
     * main.c calls. */
    if (show || !raw) {
        level lv2;
        const char *e3 = "unknown error";
        if (level_load(data, file, which, &lv2, &e3) < 0
                || level_show(data, &lv2, scale, &e3) != 0)
            fprintf(stderr, "level_show: %s\n", e3);
    }
    free(terrain);
    canvas_free(&c);
    return 0;
}


/* Replay a captured OPL register stream to a WAV.  THE PORT'S OWN - a
 * developer tool, not anything the original does.
 *
 * `tools/probe_opl.py` writes the stream the ORIGINAL's driver emits, one
 * write per line, stamped with the guest's PIT tick and the divisor in force:
 *
 *     # tick divisor reg val
 *     0 14552 04 60  timer control
 *
 * A register write only means anything at a MOMENT, so the tick is what makes
 * this a tune rather than the right notes at the wrong times: one tick is
 * divisor/1193182 seconds, and the samples between two writes are rendered at
 * the chip's own 49716 Hz.
 *
 * What this checks is the CHIP, not the port: the stream is the original's,
 * so anything wrong in the result is ymfm's or this file's.  When the port
 * has a transcribed sequencer, the same WAV rendered from ITS stream is what
 * settles whether the transcription is right - and the streams themselves
 * compare exactly, with no listening involved.
 */
static void wav_u32(FILE *f, uint32_t v)
{
    fputc((int)(v & 0xFF), f);        fputc((int)((v >> 8) & 0xFF), f);
    fputc((int)((v >> 16) & 0xFF), f); fputc((int)((v >> 24) & 0xFF), f);
}

static void wav_u16(FILE *f, uint16_t v)
{
    fputc((int)(v & 0xFF), f);        fputc((int)((v >> 8) & 0xFF), f);
}

static int opl_replay(const char *in_path, const char *wav_path)
{
    FILE *in = fopen(in_path, "r");
    if (!in) { fprintf(stderr, "cannot read %s\n", in_path); return 2; }

    int16_t *pcm = NULL;
    uint32_t cap = 0, used = 0;
    uint32_t writes = 0, skipped = 0;
    double   now = 0.0;          /* seconds of audio already rendered */
    double   prev_t = -1.0;      /* the previous line's tick, in seconds */
    char     line[256];

    opl_reset();
    while (fgets(line, sizeof line, in)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        unsigned long tick, div;
        unsigned reg, val;
        if (sscanf(line, "%lu %lu %x %x", &tick, &div, &reg, &val) != 4) {
            /* A line this cannot read is NOT a line to skip quietly. The
             * first version did, and silently replayed 41 lines of a
             * 10,147-line capture written in an older format - which came
             * out as "the chip made no sound" and read like a fault in the
             * chip. Count them, and refuse the file below. */
            skipped++;
            continue;
        }
        if (div == 0) div = 0x10000;
        double t = (double)tick * (double)div / 1193182.0;
        /* Rebase on the FIRST write. A capture's ticks are the guest's own
         * counter and start wherever its run did - the original's music
         * begins around tick 1451, which rendered fifteen seconds of silence
         * in front of it and made two identical streams compare as 99.3%
         * different. The gaps are the claim; the absolute count is not. */
        if (prev_t < 0.0) prev_t = t;
        t -= prev_t;
        if (t < 0.0) t = 0.0;

        /* render the gap since the previous write, then apply this one */
        if (t > now) {
            uint32_t need = (uint32_t)((t - now) * OPL_SAMPLE_RATE);
            if (used + need > cap) {
                cap = (used + need) * 2 + OPL_SAMPLE_RATE;
                pcm = realloc(pcm, cap * sizeof *pcm);
                if (!pcm) { fclose(in); return 2; }
            }
            opl_render(pcm + used, need);
            used += need;
            now = t;
        }
        opl_write((uint8_t)reg, (uint8_t)val);
        writes++;
    }
    fclose(in);

    /* a little tail so the last note's release is audible */
    uint32_t tail = OPL_SAMPLE_RATE / 2;
    if (used + tail > cap) {
        cap = used + tail;
        pcm = realloc(pcm, cap * sizeof *pcm);
        if (!pcm) return 2;
    }
    opl_render(pcm + used, tail);
    used += tail;

    if (!writes) {
        fprintf(stderr, "%s held no register writes - that is a fact about "
                        "the capture, not about the game\n", in_path);
        free(pcm);
        return 1;
    }
    if (skipped) {
        fprintf(stderr, "%s: %u lines did not parse and %u did. A capture "
                        "this tool only partly understands is not a capture "
                        "to render - regenerate it with tools/probe_opl.py\n",
                in_path, skipped, writes);
        free(pcm);
        return 2;
    }

    uint32_t nonzero = 0;
    int32_t  peak = 0;
    for (uint32_t i = 0; i < used; i++) {
        if (pcm[i]) nonzero++;
        int32_t a = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (a > peak) peak = a;
    }

    FILE *w = fopen(wav_path, "wb");
    if (!w) { fprintf(stderr, "cannot write %s\n", wav_path); free(pcm); return 2; }
    uint32_t bytes = used * 2;
    fwrite("RIFF", 1, 4, w); wav_u32(w, 36 + bytes); fwrite("WAVE", 1, 4, w);
    fwrite("fmt ", 1, 4, w); wav_u32(w, 16);
    wav_u16(w, 1); wav_u16(w, 1);
    wav_u32(w, OPL_SAMPLE_RATE);
    wav_u32(w, OPL_SAMPLE_RATE * 2);
    wav_u16(w, 2); wav_u16(w, 16);
    fwrite("data", 1, 4, w); wav_u32(w, bytes);
    fwrite(pcm, 2, used, w);
    fclose(w);
    free(pcm);

    printf("  register writes replayed : %u\n", writes);
    printf("  samples rendered         : %u  (%.2f s at %u Hz)\n",
           used, (double)used / OPL_SAMPLE_RATE, OPL_SAMPLE_RATE);
    printf("  non-silent samples       : %u  (%.1f%%)\n",
           nonzero, 100.0 * nonzero / (used ? used : 1));
    printf("  peak amplitude           : %d of 32767\n", peak);
    printf("  wrote %s\n", wav_path);
    /* Silence is the failure this can actually have: every write applied and
     * nothing came out. Say so rather than reporting a happy sample count. */
    if (!peak) {
        fprintf(stderr, "EVERY SAMPLE IS ZERO - the chip was written to %u "
                        "times and made no sound\n", writes);
        return 1;
    }
    return 0;
}



/* Run the TRANSCRIBED driver and dump the register stream it emits, in the
 * same format tools/probe_opl.py writes for the original. THE PORT'S OWN - a
 * developer entry, not anything the original has.
 *
 * This is the comparison the whole sound port rests on: the original's
 * driver and this one, asked for the same tune, must write the same
 * registers in the same order at the same tick. No listening, no tolerance,
 * and no picture - the register stream is to sound what prove.sh's composed
 * buffer is to the screen.
 */
static FILE *g_adlib_log;
static uint32_t g_adlib_tick;

static int g_adlib_live;

static void adlib_log_write(uint8_t reg, uint8_t val)
{
    if (!g_adlib_log) return;
    /* In the live path the tick advances INSIDE the chunk, so ask the driver
     * rather than use whatever was stamped before it started. */
    uint32_t t = g_adlib_live ? adlib_tick_count() : g_adlib_tick;
    /* The divisor a tick is WORTH, derived from the port's own clock rather
     * than written down. It was hard-coded at 12291 - the figure our
     * emulator happened to measure - so a stream logged after the port's
     * tick rate changed still RENDERED at the old rate, and a tempo fix
     * measured as no change at all. */
    fprintf(g_adlib_log, "%u %u %02X %02X\n",
            t, (unsigned)(1193182.0 / TIMER_HZ + 0.5), reg, val);
}

/* Feed the transcribed driver the same requests the ORIGINAL was given, at
 * the same ticks, and dump what it writes to the chip.
 *
 * This is the input half of the comparison, and it is the half that is easy
 * to leave out: the driver plays what it is asked for, so a capture of the
 * original's register stream is only reproducible if the asking is captured
 * too. tools/probe_opl.py writes the script; verify_adlib.py scores the two
 * streams against each other.
 */
/* Does the pump keep up? THE PORT'S OWN.
 *
 * A SIMULATION, deliberately, and no audio device is involved. The claim is
 * that over any stretch the pump produces at least as many samples as the
 * device drains, and that is arithmetic: drain one frame's worth per frame,
 * fill by whatever sound_fill_bytes says, and watch whether the queue ever
 * reaches zero.
 *
 * Driving the real pump instead was tried first and measured nothing -
 * SDL_AUDIODRIVER=dummy never drains its queue, so three frames a second,
 * which cannot keep up, reported a full buffer and no underruns.
 */
static int audio_probe(const char *data, uint32_t seconds, double fps)
{
    (void)data;
    const int frame_bytes = (int)((double)OPL_SAMPLE_RATE / fps)
                            * (int)sizeof(int16_t);
    const int target = sound_target_bytes();
    uint64_t frames = (uint64_t)(seconds * fps);
    int q = 0, mn = 0x7FFFFFFF, dry = 0;

    for (uint64_t i = 0; i < frames; i++) {
        q += (int)sound_fill_bytes(q, target);
        q -= frame_bytes;
        if (q <= 0) { dry++; q = 0; }
        if (q < mn) mn = q;
    }
    printf("  %.1f fps, %u s: one frame drains %d bytes, the target is %d\n",
           fps, seconds, frame_bytes, target);
    printf("  the queue never fell below %d bytes (%.0f ms)\n", mn,
           1000.0 * mn / 2.0 / OPL_SAMPLE_RATE);
    printf("  frames that found it EMPTY : %d\n", dry);
    if (dry) {
        fprintf(stderr, "THE DEVICE WOULD RUN DRY %d times - that is the "
                        "chopping.\n", dry);
        return 1;
    }
    return 0;
}

/* THE LIVE PATH: the level's own opening sequence, driving the driver
 * through the same sound_build_chunk the game uses, for N seconds of AUDIO
 * time. Everything measured before this was either the driver alone or the
 * pump's arithmetic; this is the code between them.
 */
static int adlib_live(const char *data, uint32_t seconds, const char *out)
{
    const char *err = "unknown error";
    if (adlib_load(data, &err) != 0) {
        fprintf(stderr, "adlib.dat: %s\n", err);
        return 2;
    }
    sound_driver_present(1);
    g_adlib_live = 1;
    level_start_reset();

    g_adlib_log = fopen(out, "w");
    if (!g_adlib_log) { fprintf(stderr, "cannot write %s\n", out); return 2; }
    fprintf(g_adlib_log, "# tick divisor reg val   (PIT_HZ = 1193182)\n");
    opl_set_trace(adlib_log_write);

    /* The level's own 17 frames a second drive level_phase_step; the audio
     * clock drives the driver. Two different rates, which is the whole point
     * of the split - and getting them confused is how the music would end up
     * at the display's speed. */
    enum { CH = 1024 };
    int16_t buf[CH];
    uint32_t total = seconds * OPL_SAMPLE_RATE;
    uint32_t made = 0;
    double frame_acc = 0.0;
    const double per_frame = (double)OPL_SAMPLE_RATE / LEVEL_FRAME_HZ;
    while (made < total) {
        if (frame_acc < 1.0) { level_phase_step(); frame_acc += per_frame; }
        /* The DRIVER's own tick count, not one derived from the sample
         * count. Deriving it quantises every write to the chunk the audio
         * happens to be built in - 1024 samples, about two ticks - and the
         * gaps then disagree with the original for a reason that is entirely
         * this tool's. It read as 93.85% and looked like a port fault. */
        g_adlib_tick = adlib_tick_count();
        sound_build_chunk(buf, CH);
        made += CH;
        frame_acc -= CH;
    }
    opl_set_trace(NULL);
    g_adlib_live = 0;
    uint32_t n = opl_writes();
    fclose(g_adlib_log);
    g_adlib_log = NULL;
    printf("  %u s of the LIVE path -> %u register writes -> %s\n",
           seconds, n, out);
    if (!n) {
        fprintf(stderr, "THE LIVE PATH WROTE NOTHING.\n");
        return 1;
    }
    return 0;
}

static int adlib_script(const char *data, const char *script,
                        uint32_t ticks, const char *out)
{
    const char *err = "unknown error";
    if (adlib_load(data, &err) != 0) {
        fprintf(stderr, "adlib.dat: %s\n", err);
        return 2;
    }
    FILE *in = fopen(script, "r");
    if (!in) { fprintf(stderr, "cannot read %s\n", script); return 2; }

    /* Read the whole script first: it has to be rebased on its own first
     * tick, and a stream driven from tick 0 while the script starts at
     * 1,500 would run in silence and score zero for a reason that has
     * nothing to do with the transcription. */
    typedef struct { uint32_t t; char kind[8]; uint32_t id; } req;
    req *rq = NULL; uint32_t nrq = 0, cap = 0;
    char line[256];
    while (fgets(line, sizeof line, in)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        unsigned long t, d; char k[8]; unsigned id;
        if (sscanf(line, "%lu %lu %7s %u", &t, &d, k, &id) != 4) continue;
        if (nrq == cap) {
            cap = cap ? cap * 2 : 64;
            rq = realloc(rq, cap * sizeof *rq);
            if (!rq) { fclose(in); return 2; }
        }
        rq[nrq].t = (uint32_t)t;
        snprintf(rq[nrq].kind, sizeof rq[nrq].kind, "%s", k);
        rq[nrq].id = id;
        nrq++;
    }
    fclose(in);
    if (!nrq) {
        fprintf(stderr, "%s held no requests - nothing to drive the driver "
                        "with, and a stream of silence is not a result\n",
                script);
        free(rq);
        return 1;
    }
    uint32_t base = rq[0].t;

    g_adlib_log = fopen(out, "w");
    if (!g_adlib_log) { fprintf(stderr, "cannot write %s\n", out); free(rq); return 2; }
    fprintf(g_adlib_log, "# tick divisor reg val   (PIT_HZ = 1193182)\n");

    opl_set_trace(adlib_log_write);
    uint32_t next = 0;
    for (g_adlib_tick = 0; g_adlib_tick < ticks; g_adlib_tick++) {
        /* Requests first, then the tick.
         *
         * Moving the tick ahead of them was tried, on the theory that the
         * game's slot write lands between ticks and is picked up by the
         * next one. It changed NOTHING: every request shifts equally, so
         * the relative timing - which is all the comparison scores - came
         * out identical to the write. The ordering here is therefore not
         * evidence of anything, and a comment claiming it fixed the
         * one-tick residue would have been a confident story about a
         * change that moved no number.
         *
         * The residue itself is the INSTRUMENT. The requests are stamped
         * with the emulator's PIT tick at the moment the guest stores the
         * byte, and whether the driver's own tick for that count has
         * already run is not something a tick counter can say. One tick of
         * ambiguity, at exactly the writes where a request takes effect,
         * is what that looks like. */
        while (next < nrq && rq[next].t - base <= g_adlib_tick) {
            if (!strcmp(rq[next].kind, "effect"))
                adlib_effect((uint8_t)rq[next].id);
            else if (!strcmp(rq[next].kind, "slot5"))
                adlib_slot5((uint8_t)rq[next].id);
            else
                adlib_music((uint8_t)rq[next].id);
            next++;
        }
        adlib_tick();
    }
    opl_set_trace(NULL);
    uint32_t n = opl_writes();
    fclose(g_adlib_log);
    g_adlib_log = NULL;
    free(rq);
    printf("  %u requests over %u ticks -> %u register writes -> %s\n",
           next, ticks, n, out);
    if (next < nrq)
        printf("  NOTE: %u of %u requests were past tick %u and never made\n",
               nrq - next, nrq, ticks);
    if (!n) {
        fprintf(stderr, "THE DRIVER WROTE NOTHING.\n");
        return 1;
    }
    return 0;
}

static int adlib_dump(const char *data, uint8_t id, uint32_t ticks,
                      const char *out)
{
    const char *err = "unknown error";
    if (adlib_load(data, &err) != 0) {
        fprintf(stderr, "adlib.dat: %s\n", err);
        return 2;
    }
    g_adlib_log = fopen(out, "w");
    if (!g_adlib_log) { fprintf(stderr, "cannot write %s\n", out); return 2; }
    fprintf(g_adlib_log, "# tick divisor reg val   (PIT_HZ = 1193182)\n");

    opl_set_trace(adlib_log_write);
    adlib_music(id);
    for (g_adlib_tick = 0; g_adlib_tick < ticks; g_adlib_tick++)
        adlib_tick();
    opl_set_trace(NULL);

    uint32_t n = opl_writes();
    fclose(g_adlib_log);
    g_adlib_log = NULL;
    printf("  tune %u, %u ticks -> %u register writes -> %s\n",
           id, ticks, n, out);
    /* A tune that wrote nothing is a fact about this run, and it must not be
     * reported as a stream. */
    if (!n) {
        fprintf(stderr, "THE DRIVER WROTE NOTHING. Either tune %u does not "
                        "exist or the sequencer never ran.\n", id);
        return 1;
    }
    return 0;
}



int main(int argc, char **argv)
{
    /* The level the GAME calls "Level 1" is not the first level in the first
     * file. In this set it is section 1 (0-based) of level009.dat -
     * established by running the original under an emulator and reading its
     * own briefing screen: "Just dig!", 10 lemmings, 10% to be saved, release
     * rate 50, 5 minutes, diggers only. Parsing that section gives exactly
     * those numbers. The level order is not the file order, and what
     * maps one to the other has not been traced yet. */
    const char *data = ".";
    const char *typed = NULL;       /* --code: the ten cells to score */
    const char *file = NULL;        /* NULL = look it up in the game's table */
    uint8_t rating = 0;             /* 0 Fun, 1 Tricky, 2 Taxing, 3 Mayhem */
    uint32_t number = 1;            /* the GAME's level number, 1..30 */
    uint32_t game_number = 0;       /* what the briefing prints; 0 = unknown */
    const char *raw  = NULL;
    const char *panel_out = NULL;
    uint32_t which = 2;
    uint8_t scale = 3;
    int show = 0;
    /* The menu is the default screen, and anything that names a level asks
     * for the level instead. --raw is a file request, so it turns
     * the window off unless --show says otherwise; that is what keeps
     * tools/prove.sh working unchanged. */
    int want_menu = 1;
    int menu_forced = 0;
    int which_screen = 0;   /* 0 menu, 1 enter-code, 2 input type */
    /* The original composes terrain first and draws objects afterwards, so a
     * capture taken at the end of its terrain loop has no objects in it. This
     * lets the port match that, for a comparison that is terrain against
     * terrain and nothing else. */
    int objects = 1;
    const char *opl_in = NULL, *wav_out = NULL;
    int adlib_tune = -1; uint32_t adlib_ticks = 600;
    const char *adlib_scr = NULL;
    int adlib_live_s = 0;
    int audio_secs = 0; double audio_fps = 17.0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc)       data = argv[++i];
        else if (!strcmp(argv[i], "--menu")) menu_forced = 1;
        else if (!strcmp(argv[i], "--code") && i + 1 < argc) {
            typed = argv[++i];         /* the ten cells, for --screen code */
        }
        else if (!strcmp(argv[i], "--screen") && i + 1 < argc) {
            /* Which menu screen: the menu itself, or one of the two the
             * original reaches from it. For looking at and for diffing. */
            const char *w = argv[++i];
            menu_forced = 1;
            which_screen = !strcmp(w, "code") ? 1 : !strcmp(w, "input") ? 2
                         : !strcmp(w, "briefing") ? 3 : 0;
            if (!which_screen && strcmp(w, "menu")) { usage(); return 2; }
        }
        else if (!strcmp(argv[i], "--file") && i + 1 < argc)  { file = argv[++i]; want_menu = 0; }
        else if (!strcmp(argv[i], "--scroll") && i + 1 < argc) {
            /* Where the banner's ribbon has scrolled to, in pixels. The
             * original is always somewhere in this; a capture cannot be
             * compared without saying where. */
            menu_set_scroll((uint32_t)atoi(argv[++i]));
        }
        else if (!strcmp(argv[i], "--anim") && i + 1 < argc) {
            /* The menu animation counters, comma separated, as read out of
             * the guest's ds:0x4960. Same job as --scroll: five lemmings walk
             * on and off the menu, so a capture shows one phase of them. */
            int16_t c[MENU_ANIM_N];
            uint8_t n = 0;
            const char *q = argv[++i];
            while (n < MENU_ANIM_N && *q) {
                c[n++] = (int16_t)strtol(q, (char **)&q, 10);
                if (*q == ',') q++;
            }
            menu_set_anim(c, n);
        }
        else if (!strcmp(argv[i], "--obj-frame") && i + 1 < argc) {
            g_obj_frame = (uint16_t)atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--lem-frame") && i + 1 < argc) {
            /* Which frame the banner's two lemmings are on, 0..15. Separate
             * from --scroll: in the original they are separate. */
            menu_set_lem_frame((uint8_t)atoi(argv[++i]));
        }
        else if (!strcmp(argv[i], "--rating") && i + 1 < argc) {
            const char *r = argv[++i];
            rating = !strcmp(r, "fun") ? 0 : !strcmp(r, "tricky") ? 1
                   : !strcmp(r, "taxing") ? 2 : !strcmp(r, "mayhem") ? 3 : 4;
            if (rating > 3) { usage(); return 2; }
            want_menu = 0;
        }
        /* The two bytes the menu's FX label is chosen by - image 0x04E5A
         * reads ds:0x25A7 and ds:0x2010. A comparison has to put both sides
         * in the same state; see sound_state_set. */
        else if (!strcmp(argv[i], "--snd") && i + 2 < argc) {
            uint8_t bits = (uint8_t)atoi(argv[++i]);
            uint8_t drv  = (uint8_t)atoi(argv[++i]);
            sound_state_set(bits, drv);
        }
        else if (!strcmp(argv[i], "--number") && i + 1 < argc) {
            number = (uint32_t)atoi(argv[++i]); want_menu = 0;
        }
        else if (!strcmp(argv[i], "--level") && i + 1 < argc) { which = (uint32_t)atoi(argv[++i]); want_menu = 0; }
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = (uint8_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--show")) show = 1;
        /* ODDTABLE.DAT's entry for this plate - image 0x019C9. --file names a
         * file and a section and so cannot know which PLATE it is; the
         * original decides from the level index, and this is how a
         * comparison against a recorded run says which it was. */
        else if (!strcmp(argv[i], "--odd") && i + 1 < argc)
            g_odd_entry = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--caret") && i + 1 < argc) {
            /* Whether the code screen's caret is showing, and on which of
             * the ten cells - the phase a capture happens to have caught.
             * See menu_caret in game.h. */
            int on = 0, cell = 0;
            const char *v = argv[++i];
            if (sscanf(v, "%d,%d", &on, &cell) < 1) on = 0;
            menu_caret = (uint8_t)(on != 0);
            menu_caret_cell = (uint8_t)cell;
        }
        else if (!strcmp(argv[i], "--mouse") && i + 1 < argc) {
            /* Where the pointer is, in the ORIGINAL's coordinates - ds:0x10
             * and ds:0x12, eight more than the screen's. A recorded run has
             * the mouse wherever it was left, so a comparison has to be able
             * to put the port's there. */
            int mx = 150, my = 100;
            if (sscanf(argv[++i], "%d,%d", &mx, &my) == 2)
                lem_mouse_move(mx, my);
        }
        else if (!strcmp(argv[i], "--pc") && i + 1 < argc) {
            /* The same switch the game takes - see main.c. High performance
             * swaps the panel's eight colours in part-way down the screen. */
            const char *v = argv[++i];
            lem_machine = (!strcmp(v, "compat") || !strcmp(v, "lo"))
                        ? LEM_PC_COMPATIBLE : LEM_PC_HIGH;
        }
        else if (!strcmp(argv[i], "--no-objects")) objects = 0;
        else if (!strcmp(argv[i], "--baked")) g_baked_objects = 1;
        else if (!strcmp(argv[i], "--triggers") && i + 1 < argc)
            g_triggers_out = argv[++i];
        else if (!strcmp(argv[i], "--rate-probe")) g_rate_probe = 1;
        else if (!strcmp(argv[i], "--raw") && i + 1 < argc)   { raw  = argv[++i]; want_menu = 0; }
        else if (!strcmp(argv[i], "--panel") && i + 1 < argc) { panel_out = argv[++i]; want_menu = 0; }
        else if (!strcmp(argv[i], "--reverse")) g_terrain_reverse = 1;
        else if (!strcmp(argv[i], "--bitorder") && i + 1 < argc) {
            const char *o = argv[++i];
            if (!strcmp(o, "msb"))      g_msb_first = 1;
            else if (!strcmp(o, "lsb")) g_msb_first = 0;
            else { usage(); return 2; }
        }
        else if (!strcmp(argv[i], "--planebits") && i + 1 < argc) {
            const char *o = argv[++i];
            if (strlen(o) != 4) { usage(); return 2; }
            for (uint8_t k = 0; k < 4; k++)
                g_plane_bits[k] = (uint8_t)(o[k] - '0');
        }
        else if (!strcmp(argv[i], "--terrain") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "replace"))   g_terrain_mode = TERRAIN_REPLACE;
            else if (!strcmp(m, "or"))   g_terrain_mode = TERRAIN_OR;
            else if (!strcmp(m, "keep")) g_terrain_mode = TERRAIN_KEEP;
            else if (!strcmp(m, "nov-ignore")) g_terrain_mode = TERRAIN_NOV_IGNORE;
            else if (!strcmp(m, "nov-invert")) g_terrain_mode = TERRAIN_NOV_INVERT;
            else if (!strcmp(m, "or-all")) g_terrain_mode = TERRAIN_OR_ALL;
            else if (!strcmp(m, "hw")) g_terrain_mode = TERRAIN_HW;
            else { usage(); return 2; }
        }
        else if (!strcmp(argv[i], "--audio-probe") && i + 1 < argc)
            audio_secs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc)
            audio_fps = atof(argv[++i]);
        else if (!strcmp(argv[i], "--adlib-live") && i + 1 < argc)
            adlib_live_s = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--adlib-script") && i + 1 < argc)
            adlib_scr = argv[++i];
        else if (!strcmp(argv[i], "--adlib-tune") && i + 1 < argc)
            adlib_tune = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--adlib-ticks") && i + 1 < argc)
            adlib_ticks = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--opl-replay") && i + 1 < argc)
            opl_in = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc)
            wav_out = argv[++i];
        else { usage(); return 2; }
    }

    /* --opl-replay is a whole job on its own: it needs no data directory and
     * renders no screen, so it answers here and returns. */
    if (opl_in) {
        if (!wav_out) { usage(); return 2; }
        return opl_replay(opl_in, wav_out);
    }
    lem_set_data_dir(data);

    if (audio_secs > 0) return audio_probe(data, (uint32_t)audio_secs, audio_fps);

    if (adlib_live_s > 0) {
        if (!raw) { usage(); return 2; }
        return adlib_live(data, (uint32_t)adlib_live_s, raw);
    }

    if (adlib_scr) {
        if (!raw) { usage(); return 2; }
        return adlib_script(data, adlib_scr, adlib_ticks, raw);
    }
    if (adlib_tune >= 0) {
        if (!raw) { usage(); return 2; }      /* --raw names the output */
        return adlib_dump(data, (uint8_t)adlib_tune, adlib_ticks, raw);
    }

    const char *err = "unknown error";

    /* An explicit --menu wins whatever else is on the line, and whatever
     * order it came in; without it, naming a level or a file is what turns
     * the menu off. */
    if (menu_forced) want_menu = 1;

    /* Turn the GAME's level number into a container and a section, through the
     * original's own table. --file forces a container, in which case --level
     * is a raw section index instead, which is what tools/prove.sh uses. */
    char picked[64];
    if (!file) {
        /* ds:0x1F6B is what image 0x0197D reads; the harness's --rating and
         * --number have to become one. */
        level_index_set((uint8_t)(rating * 30 + (number - 1)));
        if (level_locate(picked, sizeof(picked), &which) != 0) {
            fprintf(stderr, "no such level: rating %u number %u\n",
                    rating, number);
            return 1;
        }
        /* A PLATE KNOWS ITS OWN ODD ENTRY, image 0x019C9 - so work it out
         * here rather than making every caller pass --odd. Only --file needs
         * that flag, because a file and a section is not a plate and cannot
         * say which of the two ratings that reach the level is meant.
         *
         * Without this `lemdev --rating fun --number 9` and `./lemmings`
         * played the same level with different skills, a name apiece and
         * different save requirements - the harness disagreeing with the game
         * it is supposed to be checking. */
        if (g_odd_entry < 0)
            g_odd_entry = level_odd_entry();
        file = picked;
        which += 1;               /* load_level takes 1-based sections */
        game_number = number;
    }

    if (want_menu) {
        menu_screen mm;
        int built;
        if (which_screen == 3) {
            level blv;
            built = load_level(data, file, which, &blv, &err);
            blv.level_number = (uint16_t)game_number;
            if (built == 0)
                built = menu_briefing_screen(data, &blv, rating, &mm, &err);
        } else {
            built = which_screen == 1 ? menu_code_screen(data, &mm, &err)
                  : which_screen == 2 ? menu_input_screen(data, &mm, &err)
                                      : menu_build_rating(data, rating, &mm, &err);
            /* THE CODE SCREEN AFTER ENTER, which is the half verify_menu
             * cannot reach: it scores the screen as it OPENS - a prompt and
             * ten dots - and the state that follows is where the port drew
             * both messages over the input field. --code puts the same ten
             * cells in as tools/probe_codescreen.py typed into the original,
             * runs image 0x051A2 on them, and draws what image 0x0499E or
             * image 0x049EC draws. Same routine the loop calls. */
            if (built == 0 && which_screen == 1 && typed) {
                level_code_type(typed);
                code_cells_draw(&mm);          /* image 0x04CC9 */
                menu_code_result_draw(&mm, level_code_apply(typed));
            }
        }
        if (built != 0) {
            fprintf(stderr, "main menu: %s\n", err);
            return 1;
        }
        printf("PC Lemmings - main menu  (%dx%d, from main.dat)\n",
               MENU_W, MENU_H);
        int rc = 0;
        if (raw) {
            /* Same 'LEMRW2' container the level uses, so one comparison tool
             * reads both: magic, w, h, 16 RGB triples, then w*h indices. */
            FILE *rf = fopen(raw, "wb");
            if (!rf) {
                fprintf(stderr, "%s: cannot open for writing\n", raw);
                rc = 1;
            } else {
                uint16_t wh[2] = { MENU_W, MENU_H };
                fwrite("LEMRW2", 1, 6, rf);
                fwrite(wh, sizeof(uint16_t), 2, rf);
                for (uint8_t i = 0; i < 16; i++) {
                    fputc(mm.palette[i].r, rf);
                    fputc(mm.palette[i].g, rf);
                    fputc(mm.palette[i].b, rf);
                }
                fwrite(mm.pix, 1, (size_t)MENU_W * MENU_H, rf);
                fclose(rf);
                printf("   wrote %s (raw indices)\n", raw);
            }
        }
        if (raw) {                 /* a file was the point of the run */
            menu_free(&mm);
            return rc;
        }

        if (which_screen) {       /* a sub-screen: draw it and stop */
            dev_show(mm.pix, MENU_W, MENU_H, mm.palette,
                     "PC Lemmings", scale);
            menu_free(&mm);
            return rc;
        }

        /* The menu loop. F1 starts the level and coming back out of it
         * returns here, which is the whole point of the screen. */
        for (;;) {
            menu_action a = menu_run(&mm, scale);
            if (a == MENU_EXIT) break;
            if (a == MENU_PLAY) {
                /* The original shows the briefing between the menu and the
                 * level, and so does this: menu -> briefing -> level -> menu.
                 * Its "Press mouse button to continue" means it. */
                level lv;
                if (load_level(data, file, which, &lv, &err) == 0) {
                    lv.level_number = (uint16_t)game_number;
                    menu_screen br;
                    if (menu_briefing_screen(data, &lv, rating, &br, &err) == 0) {
                        menu_show_until_dismissed(&br, "PC Lemmings", scale);
                        menu_free(&br);
                    }
                }
                render_level(data, file, which, objects, NULL, NULL, scale, 1);
                continue;
            }
            /* F2 and F4 reach real screens in the original - a level-code
             * entry and an input-device chooser - and the port draws both.
             * What they DO with the answer is not reconstructed, so they come
             * straight back. */
            if (a == MENU_NEW_LEVEL || a == MENU_HELP) {
                menu_screen sub;
                int ok = (a == MENU_NEW_LEVEL)
                    ? menu_code_screen(data, &sub, &err)
                    : menu_input_screen(data, &sub, &err);
                if (ok != 0) {
                    fprintf(stderr, "  %s\n", err);
                    continue;
                }
                menu_show_until_dismissed(&sub, "PC Lemmings", scale);
                menu_free(&sub);
                continue;
            }
            /* THE RATING PLATE AND F3 NEVER ARRIVE HERE ANY MORE.
             *
             * Image 0x044EC, image 0x0451C and image 0x044B3 each do their
             * work and `jmp 0x445B` - back into the menu loop - so menu_loop
             * handles all three and returns only the four that leave. What
             * stood here changed `rating`, rebuilt the screen from main.dat
             * and printed a line about it; it was a second copy of the
             * game's own arithmetic living in the harness, and it would now
             * be unreachable as well.
             *
             * --rating still works: it goes through level_index_set before
             * the first menu_build_rating, above. What is gone is the
             * harness answering a key the game answers. */
        }
        menu_free(&mm);
        return rc;
    }

    level_odd_pending(g_odd_entry);
    return render_level(data, file, which, objects, raw, panel_out, scale, show);
}
