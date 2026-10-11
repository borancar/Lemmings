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

    /* The two steps the original takes, and nothing else: image 0x04D4B
     * translates a SCANCODE through ds:0x202E, and image 0x0447B compares the
     * result. Both are pure, so both can be checked without a window. */
    printf("menu keys - the scancodes the original's own table translates:\n");
    check("F1 (0x3B) starts the game",
          menu_dispatch(menu_key_code(0x3B)), MENU_PLAY);
    check("F2 (0x3C) is NEW LEVEL",
          menu_dispatch(menu_key_code(0x3C)), MENU_NEW_LEVEL);
    check("F3 (0x3D) is the sound settings",
          menu_dispatch(menu_key_code(0x3D)), MENU_FX);
    check("F4 (0x3E) is the ? plate",
          menu_dispatch(menu_key_code(0x3E)), MENU_INPUT);
    check("Esc (0x01) exits to DOS",
          menu_dispatch(menu_key_code(0x01)), MENU_EXIT);
    /* The rating plate. All four are the original's own, read off its
     * scancode table at ds:0x202E and its dispatcher at image 0x0447B, and
     * driven against it by tools/probe_rating.py. */
    check("Up (0x48) raises the rating",
          menu_dispatch(menu_key_code(0x48)), MENU_RATING_UP);
    check("and so does ` (0x29)",
          menu_dispatch(menu_key_code(0x29)), MENU_RATING_UP);
    check("Down (0x50) lowers it",
          menu_dispatch(menu_key_code(0x50)), MENU_RATING_DOWN);
    check("and so does - (0x0C)",
          menu_dispatch(menu_key_code(0x0C)), MENU_RATING_DOWN);
    check("and keypad - (0x4A), which the table maps to the same 0x2D",
          menu_dispatch(menu_key_code(0x4A)), MENU_RATING_DOWN);
    check("a key the menu does not use is ignored",
          menu_dispatch(menu_key_code(0x1E)), -1);
    check("and a scancode past the table's 83 entries",
          menu_dispatch(menu_key_code(0x60)), -1);

    /* THE CLICK IS NOT TESTED HERE, because there is nothing to test: image
     * 0x04473 takes any of ds:0x14's low three bits straight to image
     * 0x0454C, key 0x80's own target, without looking at the position. What
     * used to be here was seven checks that a click at seven different points
     * all meant MENU_PLAY - which was really seven checks that a
     * full-screen box covers the screen. sdl_io.c passes MENU_PLAY as the
     * click action and there is no matcher left to get wrong. */

    /* A DOUBLE CLICK INSIDE ONE FRAME, which is how a player double-clicks
     * the nuke: down, up, down, up, all before the level reads ds:0x14
     * again. Image 0x03E6F reads the button on the timer's ticks and needs
     * to see it go UP between the two presses - ds:0x62 only lets go on a
     * release - or the second press is the first one still held. The port
     * used to cancel a held-back release when the next press came, and the
     * nuke needed several double clicks before one registered.
     *
     * What is checked is the button as each TICK sees it, which is what the
     * panel reads, and as each FRAME's work sees it, which is what image
     * 0x03150 reads. */
    printf("\nlevel buttons - a double click inside one frame:\n");
    {
        view_level_grab(1);                   /* the level owns the mouse */
        lem_buttons = 0;
        view_button_event(1, 1); view_button_event(1, 0);
        view_button_event(1, 1); view_button_event(1, 0);
        int tick_presses = 0, frame_presses = 0, tick_first = -1;
        uint8_t was_tick = 0, was_frame = 0;
        /* Nothing is pending before the first frame: a tick must not take
         * a change, or the press lands a frame before a recording says. */
        view_tick_input();
        if (lem_buttons & 1) tick_first = -2;
        for (int frame = 0; frame < 4; frame++) {
            (void)view_level_pump();
            uint8_t b = (uint8_t)(lem_buttons & 1);
            if (b && !was_frame) frame_presses++;
            was_frame = b;
            for (int t = 0; t < 5; t++) {
                view_tick_input();
                b = (uint8_t)(lem_buttons & 1);
                if (b && !was_tick) {
                    tick_presses++;
                    if (tick_first == -1) tick_first = frame;
                }
                was_tick = b;
            }
        }
        check("presses the panel's ticks see", tick_presses, 2);
        check("presses the frame's work sees", frame_presses, 2);
        /* harness_script_step and record_level.py's poll_now both apply a
         * recorded press at the frame's start, so the ticks must first see
         * it in the frame whose work does - frame 0 here. */
        check("the ticks first see it with the frame's work", tick_first, 0);
        check("and the button ends up released", lem_buttons & 1, 0);
        view_level_grab(0);
    }

    printf("\nlevel keys - held for the top of the wait:\n");
    {
        view_level_grab(1);
        key_last_clear();
        view_key_event(0x41);                 /* F7, the builder */
        view_tick_input();
        /* A replay presses a recorded key at the top of the wait, so a
         * player's must not reach image 0x016D4 on a tick before then. */
        check("a tick does not take it", key_last(), 0);
        view_keys_apply();
        check("the wait's top presses it", key_last(), 0x41);
        view_key_event(0x41 | 0x80);
        view_level_grab(0);                   /* and leaving lets it go */
        check("a level's end delivers what it held", key_last(), 0);
    }

    printf(failures ? "\n  %d FAILED\n" : "\n  all passed\n", failures);
    return failures ? 1 : 0;
}
