/* tests/screens.c - build every screen, repeatedly, under the sanitizers.
 *
 * The port's own test. The menu loop goes menu -> briefing -> level -> menu
 * for as long as someone plays, allocating and freeing a screen each time
 * round, and none of that had ever been run in anger: the verifiers each build
 * ONE screen and exit, which is exactly the shape of run that never sees a
 * leak or a double free.
 *
 * So this does the full cycle several times. Built with
 * -fsanitize=address,undefined by `make sanitize`, a leak or an overrun fails
 * the run rather than waiting to be noticed.
 */
#include <stdio.h>
#include <string.h>

#include <stdlib.h>
#include "game.h"

static int fails;

static void ck(const char *what, int ok, const char *err)
{
    printf("  %-34s %s%s%s\n", what, ok ? "ok" : "FAIL",
           ok ? "" : ": ", ok ? "" : (err ? err : "?"));
    if (!ok) fails++;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *dir = argc > 1 ? argv[1] : "../lemming1.pc";
    const char *err = NULL;

    for (int round = 0; round < 3; round++) {
        printf("round %d:\n", round);

        menu_screen m;
        ck("menu_build", menu_build(dir, &m, &err) == 0, err);
        if (!fails) {
            /* The composed menu must not be blank - a screen of zeroes would
             * pass every "did it allocate" check ever written. */
            size_t nz = 0;
            for (size_t i = 0; i < (size_t)MENU_W * MENU_H; i++)
                if (m.pix[i]) nz++;
            ck("menu is not blank", nz > 100000, "too few non-zero pixels");
            menu_free(&m);
            menu_free(&m);          /* free twice: it must be idempotent */
            ck("menu_free is idempotent", m.pix == NULL, "pix not cleared");
        }

        menu_screen c;
        ck("menu_code_screen", menu_code_screen(dir, &c, &err) == 0, err);
        menu_free(&c);

        menu_screen i2;
        ck("menu_input_screen", menu_input_screen(dir, &i2, &err) == 0, err);
        menu_free(&i2);

        dat_file levels;
        char path[512];
        snprintf(path, sizeof(path), "%s/level009.dat", dir);
        if (dat_load(path, &levels, &err) == 0) {
            level lv;
            ck("level_parse", level_parse(levels.sections[1].data,
                                          levels.sections[1].size,
                                          &lv, &err) == 0, err);
            lv.level_number = 2;
            dat_free(&levels);
            dat_free(&levels);      /* also idempotent */

            menu_screen br;
            ck("menu_briefing_screen",
               menu_briefing_screen(dir, &lv, 0, &br, &err) == 0, err);
            menu_free(&br);

            ground g;
            snprintf(path, sizeof(path), "%s/ground0o.dat", dir);
            if (ground_load(path, &g, &err) == 0) {
                canvas cv;
                ck("canvas_init", canvas_init(&cv) == 0, "out of memory");
                canvas_free(&cv);
                canvas_free(&cv);
            }
        } else {
            printf("  (no data files at %s - skipping the level half)\n", dir);
        }
    }

    /* THE BANNER, which is the part of this screen that moves.
     *
     * Both checks exist because both failed in a way nothing else caught.
     *
     * `gfx` is the copy of section 4 the banner is redrawn from. Every screen
     * that animates needs it, and a rating change builds a WHOLE NEW screen -
     * so if only the first one carried it, the credits would roll until you
     * touched the difficulty and then stop dead.
     *
     * And the ribbon has to actually move, and to pause. The pause markers
     * are 0xFF bytes inside the text; they were written into game.c as UTF-8
     * U+00FF - c3 bf, two bytes - instead of the escape, so the string was 12
     * characters too long and contained no marker at all. Nothing failed: the
     * banner scrolled, the score was unchanged, and the credits simply never
     * paused. A test that only asks "did it move" would not have caught that
     * either, so this asks for both. */
    {
        menu_screen a, b;
        const char *e = NULL;
        if (menu_build_rating(dir, 0, &a, &e) == 0) {
            ck("the menu keeps section 4 for the banner", a.gfx != NULL,
               "no gfx: the banner cannot be redrawn, so it would freeze");
            if (menu_build_rating(dir, 3, &b, &e) == 0) {
                ck("and so does the screen a rating change builds",
                   b.gfx != NULL, "no gfx after a rating change");
                menu_free(&b);
            }
            /* The ribbon moves, and the same scroll gives the same picture. */
            size_t band = (size_t)MENU_W * 16;
            uint8_t *at0 = malloc(band), *at8 = malloc(band),
                    *again = malloc(band);
            if (at0 && at8 && again) {
                menu_set_scroll(0);
                menu_screen s0;
                if (menu_build_rating(dir, 0, &s0, &e) == 0) {
                    memcpy(at0, s0.pix + (size_t)334 * MENU_W, band);
                    menu_free(&s0);
                }
                menu_set_scroll(4);
                if (menu_build_rating(dir, 0, &s0, &e) == 0) {
                    memcpy(at8, s0.pix + (size_t)334 * MENU_W, band);
                    menu_free(&s0);
                }
                menu_set_scroll(0);
                if (menu_build_rating(dir, 0, &s0, &e) == 0) {
                    memcpy(again, s0.pix + (size_t)334 * MENU_W, band);
                    menu_free(&s0);
                }
                /* Far enough in that the credits have scrolled into view:
                 * the strip is 35 columns of bare template at start-up, so a
                 * small scroll shows no text at all and a test that only
                 * moved a little would pass on a banner with no text in it. */
                uint8_t *lit = malloc(band);
                menu_set_scroll(35 * 16 + 16 * 12);
                if (lit && menu_build_rating(dir, 0, &s0, &e) == 0) {
                    memcpy(lit, s0.pix + (size_t)334 * MENU_W, band);
                    menu_free(&s0);
                    size_t ink = 0;
                    for (size_t i = 0; i < band; i++)
                        if (lit[i] != 0 && lit[i] != 13) ink++;
                    ck("and the credits scroll into it",
                       ink > 200, "no text in the bar once it should be there");
                }
                free(lit);
                /* The two lemmings holding the ends. They are drawn last and
                 * opaquely, and their sprites carry the bar's end caps - so
                 * if they were missing, x 0..47 would be black. */
                menu_set_scroll(0);
                menu_set_lem_frame(14);
                if (menu_build_rating(dir, 0, &s0, &e) == 0) {
                    size_t ink = 0, cap = 0;
                    for (uint16_t r = 0; r < 16; r++)
                        for (uint16_t x = 0; x < 48; x++) {
                            uint8_t v = s0.pix[(size_t)(334 + r) * MENU_W + x];
                            if (v) ink++;
                            if (x >= 36 && v == 13) cap++;
                        }
                    ck("the left lemming is drawn", ink > 100,
                       "x 0..47 of the banner is empty");
                    ck("and it carries the bar's end cap", cap > 100,
                       "no bar colour at x 36..47");
                    menu_free(&s0);
                }
                menu_set_lem_frame(0);
                menu_set_scroll(0);
                /* 4, not 8: the bar's dash has period EIGHT, so scroll 0 and
                 * scroll 8 are the same picture until text arrives. That is
                 * the tile being what it is, not the scroll failing - and it
                 * is why this pair is chosen deliberately. */
                ck("the banner moves when the scroll does",
                   memcmp(at0, at8, band) != 0, "same picture at 0 and 4");
                ck("and the same scroll gives the same banner",
                   memcmp(at0, again, band) == 0, "not reproducible");
            }
            free(at0); free(at8); free(again);
            menu_free(&a);
        }
    }

    /* A hotspot table the menu never asked for, to catch an out-of-bounds
     * write in menu_hotspots if MENU_HOTSPOTS and the code disagree. */
    view_hotspot spots[MENU_HOTSPOTS];
    memset(spots, 0xAA, sizeof(spots));
    menu_hotspots(spots);
    ck("menu_hotspots fills its array",
       spots[MENU_HOTSPOTS - 1].w == MENU_W, "last hotspot not written");

    printf(fails ? "\n  %d FAILED\n" : "\n  all passed\n", fails);
    return fails ? 1 : 0;
}
