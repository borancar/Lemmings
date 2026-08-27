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

#include "game.h"

/* ODDTABLE.DAT's entry for the plate being rendered - image 0x019C9's bit.
 * --file names a file and a section and cannot know which PLATE that is, so
 * a comparison against a recorded run has to be told. -1 is "its own". */
static int g_odd_entry = -1;






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
static const char *const RATING_NAME[4] = { "Fun", "Tricky", "Taxing",
                                            "Mayhem" };

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
            printf("  object effects:");
            for (uint16_t i = 0; i < lv.num_objects; i++) {
                uint8_t id = lv.objects[i].id;
                if (id >= GROUND_OBJECTS) continue;
                printf(" [%u]id%u=%u", i, id, gg.objects[id].trigger_effect);
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
                                g_lem, was, c.pix, c.w, c.h);
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
        if (level_show(data, file, which, &lv2, scale, &e3) != 0)
            fprintf(stderr, "level_show: %s\n", e3);
    }
    free(terrain);
    canvas_free(&c);
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

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc)       data = argv[++i];
        else if (!strcmp(argv[i], "--menu")) menu_forced = 1;
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
        else { usage(); return 2; }
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
    const int forced_file = (file != NULL);   /* --file pins the container */
    if (!file) {
        if (level_locate(rating, (uint8_t)number, picked, sizeof(picked),
                         &which) != 0) {
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
            g_odd_entry = level_odd_entry(rating, (uint8_t)number);
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
            view_show(mm.pix, MENU_W, MENU_H, MENU_W, MENU_H, mm.palette,
                      "PC Lemmings", 0, scale);
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
            /* The rating plate. Up / ` raise it and Down / - lower it, and
             * the original caps and floors rather than wrapping - image
             * 0x044F9 bails when the rating is already 3, image 0x04529 when
             * it is already 0. Then it recomputes the absolute level number
             * as rating * 30, which lands on the FIRST level of the new
             * rating, so the number restarts here too. */
            if (a == MENU_RATING_UP || a == MENU_RATING_DOWN) {
                if (forced_file) {
                    printf("  --file pins the container, so the rating plate "
                           "has nothing to choose\n");
                    continue;
                }
                uint8_t was = rating;
                if (a == MENU_RATING_UP) {
                    if (rating < MENU_RATING_MAX) rating++;
                } else {
                    if (rating > 0) rating--;
                }
                if (rating == was)
                    continue;              /* at the cap or the floor */
                number = 1;                /* rating * 30 + 1, 1-based */
                game_number = number;
                g_odd_entry = level_odd_entry(rating, (uint8_t)number);
                if (level_locate(rating, (uint8_t)number, picked,
                                 sizeof(picked), &which) != 0) {
                    fprintf(stderr, "no such level: rating %u number %u\n",
                            rating, number);
                    rating = was;
                    continue;
                }
                file = picked;
                which += 1;                /* load_level takes 1-based */
                menu_screen next;
                if (menu_build_rating(data, rating, &next, &err) != 0) {
                    fprintf(stderr, "  %s\n", err);
                    rating = was;
                    continue;
                }
                menu_free(&mm);
                mm = next;
                printf("  rating %s, back to level 1 (%s section %u)\n",
                       RATING_NAME[rating], file, which);
                continue;
            }
            /* F3 "FX" changes NOTHING on the original's screen in this build -
             * measured with tools/probe_menu_keys.py, which finds only the
             * scroller moving. There is no sound here to toggle either, so
             * this is a faithful no-op rather than a missing feature. */
            printf("  F3, \"FX\": the original's screen does not change here "
                   "either, and there is no sound yet\n");
        }
        menu_free(&mm);
        return rc;
    }

    level_odd_pending(g_odd_entry);
    return render_level(data, file, which, objects, raw, panel_out, scale, show);
}
