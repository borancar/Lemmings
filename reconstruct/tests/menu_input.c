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
    /* The rating plate. All four keys are the original's own, read off its
     * scancode table at ds:0x202E and its dispatcher at image 0x0447B, and
     * driven against it by tools/probe_rating.py. */
    check("Up raises the rating",
          view_match_key(spots, MENU_HOTSPOTS, VIEW_KEY_UP), MENU_RATING_UP);
    check("and so does `",
          view_match_key(spots, MENU_HOTSPOTS, VIEW_KEY_BACKQUOTE),
          MENU_RATING_UP);
    check("Down lowers it",
          view_match_key(spots, MENU_HOTSPOTS, VIEW_KEY_DOWN),
          MENU_RATING_DOWN);
    check("and so does - (keypad minus arrives as the same key)",
          view_match_key(spots, MENU_HOTSPOTS, VIEW_KEY_MINUS),
          MENU_RATING_DOWN);

    /* Clicks. The original HIDES the cursor on this screen and treats a
     * click anywhere as F1 - clicking "EXIT TO DOS" starts the level rather
     * than exiting, measured with tools/probe_menu_keys.py. So every one of
     * these is MENU_PLAY, including the ones over other plates, and that is
     * the behaviour being pinned down here. */
    printf("\nmenu clicks - the original ignores WHERE, and acts as F1:\n");
    check("over 1 PLAYER",
          view_match_point(spots, MENU_HOTSPOTS, 68 + 60, 120 + 30), MENU_PLAY);
    check("over EXIT TO DOS (does NOT exit)",
          view_match_point(spots, MENU_HOTSPOTS, 196 + 60, 197 + 30), MENU_PLAY);
    check("over the rating plate",
          view_match_point(spots, MENU_HOTSPOTS, 452 + 60, 120 + 30), MENU_PLAY);
    check("on bare background, top left",
          view_match_point(spots, MENU_HOTSPOTS, 4, 4), MENU_PLAY);
    check("on bare background, between the rows",
          view_match_point(spots, MENU_HOTSPOTS, 320, 188), MENU_PLAY);
    check("bottom right corner",
          view_match_point(spots, MENU_HOTSPOTS, 639, 349), MENU_PLAY);
    check("outside the screen entirely",
          view_match_point(spots, MENU_HOTSPOTS, 700, 400), VIEW_CLOSED);

    printf(failures ? "\n  %d FAILED\n" : "\n  all passed\n", failures);
    return failures ? 1 : 0;
}
