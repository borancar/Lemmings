/* tests/menu_input.c - does the menu actually respond?
 *
 * The port's own test, so it carries the project's licence rather than any
 * claim about the original.
 *
 * A screenshot cannot answer that question, and "it built and opened a window"
 * is not evidence that F1 does anything. What CAN be wrong is the mapping: a
 * key on the wrong plate, a hit box off by its own width, a plate that answers
 * to nothing. So this checks the mapping directly, through the pure functions
 * view_match_key and view_match_point.
 *
 * It deliberately does NOT push synthetic SDL events and wait for a window to
 * read them. That was tried first and does not work: under the dummy video
 * driver SDL delivers the window events and then a QUIT, and never delivers a
 * pushed keyboard event - measured, with the event types printed. A test built
 * on that would have been testing SDL's event delivery rather than this port,
 * and it hung instead of failing, which is worse.
 *
 * It still runs with SDL_VIDEODRIVER=dummy (the Makefile sets it), because
 * menu_build touches nothing graphical but the test must never be able to open
 * a window by accident.
 *
 *   make test
 */
#include <stdio.h>
#include <string.h>

#include "game.h"

static int failures;

static void check(const char *what, int32_t got, int32_t want)
{
    if (got == want) {
        printf("  ok    %-36s -> %d\n", what, got);
    } else {
        printf("  FAIL  %-36s -> %d, wanted %d\n", what, got, want);
        failures++;
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    view_hotspot spots[MENU_HOTSPOTS];
    menu_hotspots(spots);

    printf("menu keys - the ones the original prints on its own plates:\n");
    check("F1 starts the game",
          view_match_key(spots, MENU_HOTSPOTS, VIEW_KEY_F1), MENU_PLAY);
    check("F2 is NEW LEVEL",
          view_match_key(spots, MENU_HOTSPOTS, VIEW_KEY_F2), MENU_NEW_LEVEL);
    check("F3 is FX",
          view_match_key(spots, MENU_HOTSPOTS, VIEW_KEY_F3), MENU_FX);
    check("F4 is the ? plate",
          view_match_key(spots, MENU_HOTSPOTS, VIEW_KEY_F4), MENU_HELP);
    check("Esc exits to DOS",
          view_match_key(spots, MENU_HOTSPOTS, VIEW_KEY_ESC), MENU_EXIT);
    check("Up works the rating plate",
          view_match_key(spots, MENU_HOTSPOTS, VIEW_KEY_UP), MENU_RATING);
    check("Down works it too",
          view_match_key(spots, MENU_HOTSPOTS, VIEW_KEY_DOWN), MENU_RATING);

    printf("\nmenu clicks - the middle of each plate:\n");
    check("1 PLAYER",
          view_match_point(spots, MENU_HOTSPOTS, 68 + 60, 120 + 30), MENU_PLAY);
    check("NEW LEVEL",
          view_match_point(spots, MENU_HOTSPOTS, 196 + 60, 120 + 30), MENU_NEW_LEVEL);
    check("FX",
          view_match_point(spots, MENU_HOTSPOTS, 324 + 60, 120 + 30), MENU_FX);
    check("the rating plate",
          view_match_point(spots, MENU_HOTSPOTS, 452 + 60, 120 + 30), MENU_RATING);
    check("EXIT TO DOS",
          view_match_point(spots, MENU_HOTSPOTS, 196 + 60, 197 + 30), MENU_EXIT);
    check("the ? plate",
          view_match_point(spots, MENU_HOTSPOTS, 324 + 60, 197 + 30), MENU_HELP);

    printf("\nand the background is not a button:\n");
    check("top left corner",
          view_match_point(spots, MENU_HOTSPOTS, 4, 4), VIEW_CLOSED);
    check("between the two rows of plates",
          view_match_point(spots, MENU_HOTSPOTS, 320, 188), VIEW_CLOSED);
    check("just left of 1 PLAYER",
          view_match_point(spots, MENU_HOTSPOTS, 67, 150), VIEW_CLOSED);
    check("just right of the ? plate",
          view_match_point(spots, MENU_HOTSPOTS, 324 + 120, 197 + 30), VIEW_CLOSED);
    check("one row below the bottom plates",
          view_match_point(spots, MENU_HOTSPOTS, 324 + 60, 197 + 61), VIEW_CLOSED);

    printf(failures ? "\n  %d FAILED\n" : "\n  all passed\n", failures);
    return failures ? 1 : 0;
}
