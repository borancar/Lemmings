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
    if (menu_build_rating(data, rating, &mm, &err) != 0) {
        fprintf(stderr, "PC Lemmings: %s\n"
                        "(looked in \"%s\"; pass the data directory as the "
                        "first argument)\n", err, data);
        return 1;
    }

    for (;;) {
        menu_action a = menu_run(&mm, DEFAULT_SCALE);
        if (a == MENU_EXIT) break;

        if (a == MENU_PLAY) {
            /* menu -> briefing -> level -> menu. The briefing's own
             * instruction is "Press mouse button to continue" and it means
             * it; the original takes no key off that screen. */
            char picked[64];
            uint32_t which = 0;
            if (level_locate(rating, (uint8_t)number, picked, sizeof(picked),
                             &which) != 0)
                continue;
            which += 1;             /* level_show takes 1-based sections */
            level lv;
            if (level_show_briefing(data, picked, which, rating,
                                    (uint16_t)number, DEFAULT_SCALE,
                                    &err) != 0)
                fprintf(stderr, "  %s\n", err);
            else if (level_show(data, picked, which, &lv,
                                DEFAULT_SCALE, &err) != 0)
                fprintf(stderr, "  %s\n", err);
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
            continue;
        }
        /* F3 "FX" changes nothing on the original's screen in this build -
         * measured with tools/probe_menu_keys.py - and there is no sound here
         * to toggle, so it is a faithful no-op. */
    }

    menu_free(&mm);
    return 0;
}
