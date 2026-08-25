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
