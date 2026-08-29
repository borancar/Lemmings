/* main.c - the game's entry point, and only that.
 *
 * The original DOES take a command line, which this file used to say it did
 * not. LEMMINGS.BAT never runs VGALEMMI.EXE directly: it runs `lemvga.com /v`
 * for VGA and `lemvga.com /e` for EGA, and the parser at image 0x012E0 reads
 * those letters - 'v' stores 4 in ds:0x1F7C at image 0x01312, which is what
 * picks the VGA palette path at image 0x055B7. So a switch that selects
 * between the machines the original itself asks about is not foreign to it.
 *
 * It stays to ONE switch. Everything a developer needs - rendering a screen
 * to a file, forcing a rating or a scroll position, comparing against the
 * emulator - still lives in devmain.c and builds as a separate binary.
 *
 * That separation is worth keeping. When the flags lived here, the file a
 * reader would open expecting "the game's entry point" was four hundred lines
 * of argument parsing with the actual start-up lost in the middle of it.
 *
 * THIS FILE IS THE PORT'S OWN in its shape, though what it runs is the
 * game's. A C entry point with a loop that returns a value is not something
 * vgalemmi.exe has; every piece this calls is transcribed and carries its
 * address.
 *
 * The original's own flow is reached from image 0x0454C -
 * F1 on the menu - and its menu loop dispatches at image 0x0447B and never
 * returns to a caller the way this does. The SHAPE here is the port's; the
 * pieces it calls are transcribed and carry their addresses.
 *
 * It reads the player's own copy of the original data files; nothing of the
 * game is redistributed here.
 */
#include <stdio.h>
#include <string.h>

#include "game.h"

/* The one concession to not being DOS: where the data files are. The original
 * looked in the current directory because that is where DOS put it. */
#define DATA_DIR "lemming1.pc"

/* Whole-pixel magnification. The original ran on a display where 640x350
 * filled the screen; on anything modern it is a postage stamp, so the port
 * enlarges by three and nothing else - no filtering, no stretching, no
 * decision of the original's undone. */
#define DEFAULT_SCALE 3

/* The start-up menu's second question, as a switch.
 *
 *   --pc high   the palette changes part-way down the screen, so the panel
 *               gets its own eight colours - image 0x0C080
 *   --pc compat one palette for the whole frame, which is what a slow PC
 *               got: the panel makes do with the level's colours
 *
 * High by default: it is the one the original's own menu highlights, the one
 * this port is checked against, and the one whose panel is not full of the
 * terrain's browns. */
static int parse_pc(const char *v)
{
    if (!strcmp(v, "high") || !strcmp(v, "hi")) { lem_machine = LEM_PC_HIGH;
                                                  return 0; }
    if (!strcmp(v, "compat") || !strcmp(v, "lo")) {
        lem_machine = LEM_PC_COMPATIBLE;
        return 0;
    }
    return -1;
}

int main(int argc, char **argv)
{
    const char *data = DATA_DIR;
    const char *err = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--pc") && i + 1 < argc) {
            if (parse_pc(argv[++i]) != 0) {
                fprintf(stderr, "PC Lemmings: --pc takes high or compat\n");
                return 2;
            }
        } else if (argv[i][0] != '-') {
            data = argv[i];
        } else {
            fprintf(stderr, "PC Lemmings: unknown option %s\n"
                            "usage: lemmings [--pc high|compat] [DATA_DIR]\n",
                    argv[i]);
            return 2;
        }
    }

    /* WHICH LEVEL IS ds:0x1F6B's, and it lives in game.c. main.c used to
     * keep a rating, a number and an index of its own and move all three by
     * hand; the original has one byte and four routines. */
    menu_screen mm;
    /* sdl_io.c loads adlib.dat when the audio device opens, which is
     * after this point, so it has to be told where the files are. */
    lem_set_data_dir(data);
    /* image 0x0454C's wipe: a cold start begins at the first level with the
     * rotation at index 0. */
    music_cold_start();
    level_index_reset();

    if (menu_build_rating(data, level_rating(), &mm, &err) != 0) {
        fprintf(stderr, "PC Lemmings: %s\n"
                        "(looked in \"%s\"; pass the data directory as the "
                        "first argument)\n", err, data);
        return 1;
    }

    uint8_t built = level_rating();   /* which rating `mm` was built for */

    for (;;) {
        /* The menu screen is built per RATING, and finishing the last level
         * of one moves into the next - image 0x0472E's advance does not stop
         * at a boundary. Rebuild before showing it, or the plate names the
         * rating the player has just left. */
        if (level_rating() != built) {
            menu_screen next;
            if (menu_build_rating(data, level_rating(), &next, &err) == 0) {
                menu_free(&mm);
                mm = next;
                built = level_rating();
            }
        }
        menu_action a = menu_run(&mm, DEFAULT_SCALE);
        if (a == MENU_EXIT) break;

        if (a == MENU_PLAY) {
            /* THE GAME'S OWN LOOP, and its shape is the original's.
             *
             *   image 0x0454C  F1 from the MENU: clear ds:0x1F6D, then fall
             *                  into image 0x04551
             *   image 0x04551  set the level up from ds:0x1F6B and play it
             *   image 0x046EB  the results
             *   image 0x047D7  the LEFT button jumps back to image 0x04551 -
             *                  NOT to 0x0454C, so the flag stays as the
             *                  results screen left it
             *   image 0x047CB  the right button, and any key, go to the menu
             *
             * That difference between the two entries is the whole of how
             * the music steps: arriving from the menu clears the advance
             * flag so the tune is the one this level already had, and
             * arriving from the results leaves it set by image 0x04729 so
             * the rotation moves on. main.c had one entry and no flag, so
             * the track changed whenever it happened to. */
            level_from_menu();              /* image 0x0454C */
            for (;;) {
                char picked[64];
                uint32_t which = 0;
                /* image 0x04551, and it is inside the loop because BOTH
                 * entries run it: the results screen's next level jumps to
                 * 0x04551 rather than to 0x0454C, taking the wipe and
                 * skipping only the advance flag above it. */
                level_state_wipe();
                /* image 0x0197D reads ds:0x1F6B and needs nothing else. */
                if (level_locate(picked, sizeof(picked), &which) != 0)
                    break;
                which += 1;         /* level_show takes 1-based sections */
                /* Image 0x019C9: this plate may play its level under
                 * another name with other skills. Set once for the
                 * briefing and once for the level, because each parses. */
                /* ONE PARSE, which is what image 0x0197D does. The odd
                 * entry is handed over once because there is one load; it
                 * used to be set twice, once per parse. */
                level lv;
                level_odd_pending(level_odd_entry());
                if (level_load(data, picked, which, &lv, &err) < 0) {
                    fprintf(stderr, "  %s\n", err);
                    break;
                }
                /* image 0x0469B: the briefing's other exit pops the level
                 * off and jumps to image 0x04401, the menu. It is not an
                 * error, and the port used to have no way to reach it. */
                int brc = level_show_briefing(data, &lv, DEFAULT_SCALE, &err);
                if (brc < 0) {
                    fprintf(stderr, "  %s\n", err);
                    break;
                }
                if (brc == BRIEF_MENU) break;
                if (level_show(data, &lv, DEFAULT_SCALE, &err) != 0) {
                    fprintf(stderr, "  %s\n", err);
                    break;
                }
                if (!g_level_over) break;      /* the window was closed */

                /* IMAGE 0x046EB JUDGES IT, not this loop. It reads ds:0x40
                 * and ds:0x41 - written by the panel and the briefing - and
                 * advances the level, the rating and the music flag itself.
                 * main.c used to do all of that and hand the answers to a
                 * drawing routine. */
                char code[11] = "";
                int verdict = level_results_judge(code);   /* image 0x046EB */
                if (verdict == RESULTS_OUT_OF_GAME) break; /* image 0x04745 */

                uint8_t shown = (verdict == RESULTS_PASSED)
                                ? (uint8_t)(level_index() + 1) : 0;
                if (level_show_results(data, shown,
                                       verdict == RESULTS_PASSED ? code : NULL,
                                       DEFAULT_SCALE, &err) != RESULTS_NEXT)
                    break;                      /* image 0x047CB: the menu */
                /* image 0x047D7 goes back to 0x04551 either way; on a loss
                 * the index was never advanced, so it is the same level. */
            }
            continue;
        }

        if (a == MENU_NEW_LEVEL) {
            /* Image 0x044AD jumps to image 0x04967, which TYPES a code and
             * takes it back. This used to compose the screen and wait for a
             * dismiss - so typing did nothing, and a player found it. */
            menu_screen sub;
            if (menu_code_screen(data, &sub, &err) != 0) continue;
            menu_code_run(&sub);                /* image 0x04967 */
            menu_free(&sub);
            /* Image 0x049E9 and image 0x049FE both go back to image 0x04401,
             * and an accepted code has moved ds:0x1F6B - so the menu is
             * rebuilt if that changed the rating. */
            if (level_rating() != built) {
                menu_screen next;
                if (menu_build_rating(data, level_rating(), &next, &err) == 0) {
                    menu_free(&mm);
                    mm = next;
                    built = level_rating();
                }
            }
            continue;
        }

        if (a == MENU_INPUT) {
            menu_screen sub;
            if (menu_input_screen(data, &sub, &err) != 0) continue;
            menu_screen_show(&sub, "PC Lemmings", DEFAULT_SCALE);
            menu_free(&sub);
            continue;
        }

        /* F3 AND THE TWO RATING KEYS DO NOT GET HERE ANY MORE, and that is
         * the original's shape rather than a tidy-up. Image 0x044B3, image
         * 0x044EC and image 0x0451C each do their work and `jmp 0x445B` -
         * back into the loop - so the menu is never left and never rebuilt.
         * menu_loop handles all three; see game.c.
         *
         * What used to be here rebuilt the whole screen from main.dat on
         * every one of those presses, which is why the plates' own dirty
         * bytes had nothing to guard: each press met a freshly-armed
         * screen. */
    }

    menu_free(&mm);
    return 0;
}
