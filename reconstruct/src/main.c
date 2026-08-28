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

    uint8_t rating = 0;             /* 0 Fun, 1 Tricky, 2 Taxing, 3 Mayhem */
    uint32_t number = 1;            /* the GAME's level number, 1..30 */

    menu_screen mm;
    /* sdl_io.c loads adlib.dat when the audio device opens, which is
     * after this point, so it has to be told where the files are. */
    lem_set_data_dir(data);
    /* image 0x0454C: a cold start begins at rotation index 0. */
    music_cold_start();

    if (menu_build_rating(data, rating, &mm, &err) != 0) {
        fprintf(stderr, "PC Lemmings: %s\n"
                        "(looked in \"%s\"; pass the data directory as the "
                        "first argument)\n", err, data);
        return 1;
    }

    uint8_t built = rating;      /* which rating `mm` was built for */

    for (;;) {
        /* The menu screen is built per RATING, and finishing the last level
         * of one moves into the next - image 0x0472E's advance does not stop
         * at a boundary. Rebuild before showing it, or the plate names the
         * rating the player has just left. */
        if (rating != built) {
            menu_screen next;
            if (menu_build_rating(data, rating, &next, &err) == 0) {
                menu_free(&mm);
                mm = next;
                built = rating;
            } else {
                rating = built;   /* keep the one that works */
            }
        }
        menu_action a = menu_run(&mm, DEFAULT_SCALE);
        if (a == MENU_EXIT) break;

        if (a == MENU_PLAY) {
            /* menu -> briefing -> level -> menu. The briefing's own
             * instruction is "Press mouse button to continue" and it means
             * it; the original takes no key off that screen. */
            /* BRIEFING -> LEVEL -> RESULTS -> THE NEXT LEVEL, which is the
             * original's own loop: image 0x047B4 takes the left button on
             * the results screen and jumps to image 0x4551, the next
             * level's briefing, the index having already been advanced by
             * image 0x0472E. The right button goes back to the menu. */
            uint8_t index  = (uint8_t)(rating * 30 + (number - 1));
            uint8_t rat    = rating;
            for (;;) {
                char picked[64];
                uint32_t which = 0;
                uint8_t num = (uint8_t)(index % 30 + 1);
                rat = (uint8_t)(index / 30);
                if (level_locate(rat, num, picked, sizeof(picked),
                                 &which) != 0)
                    break;
                which += 1;         /* level_show takes 1-based sections */
                /* Image 0x019C9: this plate may play its level under
                 * another name with other skills. Set once for the
                 * briefing and once for the level, because each parses. */
                int odd = level_odd_entry(rat, num);
                level lv;
                level_odd_pending(odd);
                if (level_show_briefing(data, picked, which, rat,
                                        num, DEFAULT_SCALE, &err) != 0) {
                    fprintf(stderr, "  %s\n", err);
                    break;
                }
                level_odd_pending(odd);
                if (level_show(data, picked, which, &lv,
                               DEFAULT_SCALE, &err) != 0) {
                    fprintf(stderr, "  %s\n", err);
                    break;
                }
                if (!g_level_over) break;      /* the window was closed */

                /* Image 0x04720 judges it, and image 0x0472E advances the
                 * index only when it was won. */
                /* Image 0x04710 asks the CLOCK, not the caller. Passing a
                 * constant 0 here put "All lemmings accounted for." on a
                 * level that had timed out. */
                int timed_out = level_out_of_time();
                uint8_t saved = g_level_saved;
                /* Image 0x04591 - the header carries a count and image
                 * 0x04720 judges a percentage. */
                uint8_t needed = level_needed_percent(&lv);
                char code[11] = "";
                /* Image 0x04729 sets ds:0x1F6D on a pass and image 0x04778
                 * clears it on a failure, which is what steps the music to
                 * the next tune - or replays the same one with the level. */
                music_level_passed(level_won(saved, needed) ? 1 : 0);
                if (level_won(saved, needed)) {
                    uint8_t next = index, r2 = rat;
                    if (!level_advance(&next, &r2)) break;   /* 0x04745 */
                    /* IMAGE 0x0472E ADVANCES ds:0x1F6B THE MOMENT THE LEVEL
                     * IS JUDGED, before the results screen is drawn, and
                     * image 0x04737 derives the rating from it. That byte is
                     * the MENU's plate as well as the play loop's index -
                     * there is only one of it - so the menu has already
                     * moved on by the time anything can return to it.
                     *
                     * The port kept the menu's rating and number separately
                     * and never advanced them, so a player who finished a
                     * level and went back to the menu found it still
                     * offering the level they had just won. */
                    rating = r2;                       /* image 0x04737 */
                    number = (uint32_t)(next % 30 + 1);
                    level_code_generate(next, 0, saved, code);
                    if (level_show_results(data, saved, needed, timed_out,
                                           (uint8_t)(next + 1), code,
                                           DEFAULT_SCALE, &err)
                            != RESULTS_NEXT)
                        break;                  /* image 0x047BC: the menu */
                    index = next;
                    continue;
                }
                if (level_show_results(data, saved, needed, timed_out, 0, NULL,
                                       DEFAULT_SCALE, &err) != RESULTS_NEXT)
                    break;
                /* Image 0x047B4 on a lost level retries the same one. */
            }
            continue;
        }

        /* Image 0x044B3 cycles the sound settings and goes straight back
         * to the menu loop at image 0x0445B - it opens no screen. */
        if (a == MENU_FX) {
            sound_settings_cycle();
            continue;
        }

        if (a == MENU_NEW_LEVEL || a == MENU_HELP) {
            menu_screen sub;
            int ok = (a == MENU_NEW_LEVEL) ? menu_code_screen(data, &sub, &err)
                                           : menu_input_screen(data, &sub, &err);
            if (ok != 0) continue;
            menu_show_until_dismissed(&sub, "PC Lemmings", DEFAULT_SCALE);
            menu_free(&sub);
            continue;
        }

        if (a == MENU_RATING_UP || a == MENU_RATING_DOWN) {
            /* Image 0x044EC and 0x0451C: cap at MENU_RATING_MAX, floor at 0,
             * and then ds:0x1F6B becomes rating * 30 - the first level of the
             * new rating, so the number restarts with it. */
            uint8_t was = rating;
            if (a == MENU_RATING_UP) { if (rating < MENU_RATING_MAX) rating++; }
            else                     { if (rating > 0) rating--; }
            if (rating == was) continue;
            number = 1;
            menu_screen next;
            if (menu_build_rating(data, rating, &next, &err) != 0) {
                rating = was;
                continue;
            }
            menu_free(&mm);
            mm = next;
            built = rating;
            continue;
        }
        /* F3 "FX" changes nothing on the original's screen in this build -
         * measured with tools/probe_menu_keys.py - and there is no sound here
         * to toggle, so it is a faithful no-op. */
    }

    menu_free(&mm);
    return 0;
}
