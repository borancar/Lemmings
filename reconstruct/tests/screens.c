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

    /* THE FIVE WALK-ON LEMMINGS, image 0x04F96.
     *
     * No capture of the original shows one - each is on screen 32 passes out
     * of a period of 400 to 560, about seven per cent - so pixels cannot
     * settle this and the counter logic is checked directly instead.
     *
     * Zero is a VALID counter value meaning "walk on next pass", so a static
     * array left at its default would put all five on at once on frame one.
     * That is the specific thing the first assertion is here to catch. */
    {
        menu_anim_reset();
        ck("none walks on at the start", menu_anim_visible() == 0,
           "a lemming is on screen before any has been counted in");
        /* Entry 0 starts at 0x28 = 40 and is drawn while its counter is
         * -1..-16, so it is visible on passes 41..56 inclusive. */
        int seen_at = -1, runs = 0, run = 0, maxseen = 0;
        for (int pass = 1; pass <= 700; pass++) {
            menu_anim_step();
            uint8_t n = menu_anim_visible();
            if (n > maxseen) maxseen = n;
            if (n && seen_at < 0) seen_at = pass;
            if (n) run++;
            else if (run) { runs++; run = 0; }
        }
        ck("the first one walks on when its counter says",
           seen_at == 41, "wrong first appearance");
        ck("they walk on and off repeatedly", runs >= 4, "too few walk-ons");
        ck("and not all at once", maxseen < 5,
           "every lemming on screen together");
        menu_anim_reset();
    }

    /* THE LEVEL'S ANIMATED OBJECTS, images 0x07BB8 and 0x07C0A.
     *
     * Checked against what the ORIGINAL actually blits. tools/probe_blits.py,
     * driven into a level, logs the entrance as 48x25 at screen (96,36) with
     * ten distinct sources 750 apart - 0, 750, 1500 ... 6750 - and si=0 seen
     * 179 times against one apiece for the rest. That is a one-shot: it runs
     * its frames once and then sits on frame 0, which is `animation_flags`
     * bit 0 clearing bit 1 at image 0x07C49.
     *
     * So the sequence is the assertion, not a screenshot. */
    {
        const char *e = NULL;
        dat_file levels; ground g; level lv;
        char p2[512];
        snprintf(p2, sizeof(p2), "%s/level009.dat", dir);
        if (dat_load(p2, &levels, &e) == 0) {
            if (level_parse(levels.sections[1].data, levels.sections[1].size,
                            &lv, &e) == 0) {
                snprintf(p2, sizeof(p2), "%s/ground%uo.dat", dir,
                         lv.graphics_set);
                if (ground_load(p2, &g, &e) == 0) {
                    level_anim st[LEVEL_ANIM_MAX];
                    level_anim_init(st, &lv, &g);
                    level_start_reset();
                    /* It does not start straight away. Image 0x0361 sets
                     * ds:0x55 at play-loop frame 0x23, and image 0x07C1E
                     * tests that byte before animating object id 1 - so the
                     * trapdoor is held shut for the first 35 frames. Watching
                     * the running game agrees: the 0xFF arrives from image
                     * 0x394 at frame 35. Until then it sits on its start
                     * frame, which is 1, so ink is 750. */
                    int shut = 1;
                    for (int k = 0; k < 35; k++) {
                        level_start_step();
                        level_anim_step(st, &lv);
                        if (st[0].ink != 750) shut = 0;
                    }
                    ck("the entrance stays shut for 35 frames",
                       shut, "it started animating early");

                    static const uint16_t WANT[10] = {
                        750, 1500, 2250, 3000, 3750, 4500, 5250, 6000, 6750, 0
                    };
                    int ok = (st[0].ink == WANT[0]);
                    for (int k = 1; k < 10; k++) {
                        level_start_step();
                        level_anim_step(st, &lv);
                        if (st[0].ink != WANT[k]) ok = 0;
                    }
                    ck("then runs the original's own frames",
                       ok, "ink sequence differs from the emulator's blits");
                    /* One-shot: after the wrap it must STAY on frame 0. */
                    for (int k = 0; k < 20; k++) {
                        level_start_step();
                        level_anim_step(st, &lv);
                    }
                    ck("and then rests on frame 0, as a one-shot",
                       st[0].ink == 0, "it kept animating");
                }
            }
            dat_free(&levels);
        }
    }

    /* THE LEVEL CLOCK, image 0x0213.
     *
     * Seventeen frames to a second, and the time is ASCII in the status line
     * itself. Checked against the original's own line, read out of the running
     * game: at play-loop frame 40 it says TIME 4:58 and at frame 200 TIME
     * 4:49. Those two are the assertion - not a screenshot, and not a rule
     * re-derived here and checked against itself.
     *
     * The seconds tens wrap to '5', not '9', which is the one place a decimal
     * countdown written digit by digit can go wrong and still look right. */
    {
        level lv;
        memset(&lv, 0, sizeof(lv));
        lv.time_limit = 5;
        level_status_reset(&lv);
        ck("the clock starts at the level's time limit",
           !strncmp(level_status_line() + 40, "5:00", 4),
           level_status_line() + 40);

        for (int k = 0; k < 40; k++) level_timer_step();
        ck("frame 40 reads 4:58, as the original's own line does",
           !strncmp(level_status_line() + 40, "4:58", 4),
           level_status_line() + 40);

        for (int k = 40; k < 200; k++) level_timer_step();
        ck("frame 200 reads 4:49",
           !strncmp(level_status_line() + 40, "4:49", 4),
           level_status_line() + 40);

        /* Run it down past a minute boundary: 4:49 to 3:59 is 50 seconds. */
        for (int k = 0; k < 50 * 17; k++) level_timer_step();
        ck("and the seconds tens wrap to 5, not 9",
           !strncmp(level_status_line() + 40, "3:59", 4),
           level_status_line() + 40);
    }

    /* RELEASING THE LEMMINGS, images 0x30A6 and 0x39D6.
     *
     * The schedule is the assertion, and it came out of the running game:
     * ds:0x43 reads 1 at pass 56 and 2..10 at 84, 112, 140, 168, 196, 224,
     * 252, 280 and 308. Twenty frames after the entrance opens, then one
     * every ((99 - 50) >> 1) + 4 = 28.
     *
     * The releases happen one pass EARLIER than those readings, at 55, 83 and
     * so on, and that is not a fudge to make a test pass. The probe read
     * ds:0x43 at image 0x01A3, and image 0x30A6 - which does the releasing -
     * runs at image 0x01B0, later in the same pass. So a release during pass
     * N is first visible to that probe at pass N+1. The pixels settle it
     * independently: the OUT field is compared against recorded frames 52..58
     * and 82..85, straight across two release boundaries, and differs in zero
     * pixels at every one. */
    {
        level lv;
        memset(&lv, 0, sizeof(lv));
        lv.release_rate = 50;
        lv.num_lemmings = 10;
        level_start_reset();
        level_release_reset(&lv);

        static const uint16_t WHEN[10] = {
            55, 83, 111, 139, 167, 195, 223, 251, 279, 307
        };
        int ok = 1, k = 0;
        uint8_t was = 0;
        for (uint16_t f = 1; f <= 400; f++) {
            level_start_step();
            level_release_step();
            if (level_released() != was) {
                was = level_released();
                if (k >= 10 || f != WHEN[k]) ok = 0;
                k++;
            }
        }
        ck("the lemmings come out on the original's own frames",
           ok && k == 10, "the release schedule differs");
        level_start_reset();
        {
            uint16_t f = 0;
            while (f < 60 && level_entrance_open() != 0xFF) {
                level_start_step();
                f++;
            }
            ck("and the entrance opens on pass 36", f == 36, "not pass 36");
        }
        ck("and stop at the level's count",
           level_released() == 10, "it kept releasing");
    }

    /* THE STATUS LINE'S NUMBERS, image 0x04B68 - left-aligned, leading zeros
     * suppressed, and `si` only advancing when a digit is printed. That last
     * is what makes 5 come out as "5  " and not "  5". */
    {
        char c[4];
        c[3] = 0;
        status_put_num(c, 5);
        ck("a single digit is left-aligned", !strcmp(c, "5  "), c);
        status_put_num(c, 10);
        ck("ten fills two cells", !strcmp(c, "10 "), c);
        status_put_num(c, 105);
        ck("and 105 does not come out as \"1 5\"", !strcmp(c, "105"), c);
        status_put_num(c, 0);
        ck("zero still prints its units digit", !strcmp(c, "0  "), c);
    }

    /* THE SKILL BUTTONS' NUMBERS, image 0x0765F.
     *
     * A four-pixel font of ten digits at the very START of the loaded panel
     * font - main.dat section 2 offsets 6400..6559, the 160 bytes before
     * PANEL_HEAD. Drawn and read back, because that is the point of having
     * found it: "50" must put ink where the original does, and a count of
     * ZERO must come out SOLID, not blank - image 0x076DB writes 0xFF for
     * both nibbles, which is why a level offering only diggers still has ink
     * in all ten button positions.
     *
     * The two colours are measured against a recorded frame: a set font bit
     * is index 3 and a clear one index 0. "50" has 40 set bits between its
     * glyphs and the original shows exactly 40 pixels of index 3 there. */
    {
        const char *e = NULL;
        dat_file d2;
        char p4[512];
        snprintf(p4, sizeof(p4), "%s/main.dat", dir);
        if (dat_load(p4, &d2, &e) == 0 && d2.count > PANEL_FONT_SECTION) {
            const uint8_t *sec = d2.sections[PANEL_FONT_SECTION].data;
            uint32_t len = d2.sections[PANEL_FONT_SECTION].size;
            static uint8_t cell[8 * 8];
            uint16_t ink, bg;

            memset(cell, 0xAA, sizeof(cell));
            panel_button_num(cell, 8, 8, sec, len, 50, 0, 0);
            ink = bg = 0;
            for (uint16_t i = 0; i < 64; i++)
                (cell[i] == 3) ? ink++ : (cell[i] == 0 ? bg++ : (uint16_t)0);
            ck("50 puts 40 pixels of ink on a button",
               ink == 40 && bg == 24, "the digit font is not where it should be");

            memset(cell, 0xAA, sizeof(cell));
            panel_button_num(cell, 8, 8, sec, len, 0, 0, 0);
            ink = 0;
            for (uint16_t i = 0; i < 64; i++)
                if (cell[i] == 3) ink++;
            ck("a count of zero is SOLID, not blank",
               ink == 64, "image 0x076DB writes 0xFF; this did not");

            memset(cell, 0xAA, sizeof(cell));
            panel_button_num(cell, 8, 8, sec, len, 10, 0, 0);
            ink = 0;
            for (uint16_t i = 0; i < 64; i++)
                if (cell[i] == 3) ink++;
            ck("and ten is neither of those",
               ink != 64 && ink != 40, "10 drew the wrong glyphs");
            dat_free(&d2);
        }
    }

    /* THE SELECTED BUTTON'S BOX, image 0x0780D - a 16 x 24 rectangle in
     * colour 3 with only its edges drawn: the top and bottom rows solid and
     * twenty-two middle rows carrying just the leftmost and rightmost pixel,
     * which image 0x07869 does with Bit Mask 0x80 then 0x01. */
    {
        static uint8_t box[128 * 40];
        uint16_t solid = 0, edges = 0, inside = 0;
        /* Button 0 does NOT sit at x 0: image 0x2102 starts the buttons two
         * bytes into the line, so it is at 16. */
        const uint16_t x0 = 16;
        memset(box, 0, sizeof(box));
        panel_highlight(box, 128, 40, 0);
        for (uint16_t x = 0; x < 16; x++) {
            if (box[16 * 128 + x0 + x] == 3) solid++;
            if (box[39 * 128 + x0 + x] == 3) solid++;
        }
        for (uint16_t y = 17; y < 39; y++) {
            if (box[y * 128 + x0 + 0]  == 3) edges++;
            if (box[y * 128 + x0 + 15] == 3) edges++;
            for (uint16_t x = 1; x < 15; x++)
                if (box[y * 128 + x0 + x]) inside++;
        }
        ck("the box has solid top and bottom edges", solid == 32, "not 32");
        ck("and two pixels on each of 22 middle rows", edges == 44, "not 44");
        ck("with nothing inside it", inside == 0, "it filled the button");
    }

    /* THE IN PERCENTAGE, image 0x03AF8. The one thing worth asserting is the
     * trick at image 0x03B17: with no tens digit it loads 0xF0 and adds '0',
     * which wraps a byte to a SPACE. So 0 reads " 0" and not "00", and 100 is
     * special-cased across all three cells. */
    {
        level lv;
        memset(&lv, 0, sizeof(lv));
        lv.time_limit = 5;
        level_status_reset(&lv);
        level_status_in(0);
        ck("0 per cent reads as a space then a zero",
           !strncmp(level_status_line() + 29, "  0%", 4),
           level_status_line() + 29);
        level_status_in(7);
        ck("and 7 keeps the leading space",
           !strncmp(level_status_line() + 29, "  7%", 4),
           level_status_line() + 29);
        level_status_in(42);
        ck("42 uses both cells",
           !strncmp(level_status_line() + 29, " 42%", 4),
           level_status_line() + 29);
        level_status_in(100);
        ck("and 100 fills all three",
           !strncmp(level_status_line() + 29, "100%", 4),
           level_status_line() + 29);
    }

    /* THE PANEL'S FONT, at the tail of main.dat section 2.
     *
     * Checked by drawing and reading back, because the point of finding it was
     * that it renders characters: '0' and 'A' have ink, ' ' clears its cell,
     * and a character the dispatch at image 0x07756 does not map - a lower
     * case letter - draws nothing at all rather than some other glyph. */
    {
        const char *e = NULL;
        dat_file d;
        char p3[512];
        snprintf(p3, sizeof(p3), "%s/main.dat", dir);
        if (dat_load(p3, &d, &e) == 0 && d.count > 2) {
            static uint8_t cell[16 * 8];
            const uint8_t *sec = d.sections[2].data;
            uint32_t len = d.sections[2].size;
            size_t ink0 = 0, inkA = 0, inkSp = 0, inkLc = 0;
            memset(cell, 0xAA, sizeof(cell));
            panel_char(cell, 8, 16, sec, len, '0', 0, 0);
            for (size_t k = 0; k < sizeof(cell); k++) ink0 += (cell[k] != 0);
            memset(cell, 0xAA, sizeof(cell));
            panel_char(cell, 8, 16, sec, len, 'A', 0, 0);
            for (size_t k = 0; k < sizeof(cell); k++) inkA += (cell[k] != 0);
            memset(cell, 0xAA, sizeof(cell));
            panel_char(cell, 8, 16, sec, len, ' ', 0, 0);
            for (size_t k = 0; k < sizeof(cell); k++) inkSp += (cell[k] != 0);
            memset(cell, 0, sizeof(cell));
            panel_char(cell, 8, 16, sec, len, 'a', 0, 0);
            for (size_t k = 0; k < sizeof(cell); k++) inkLc += (cell[k] != 0);
            ck("the panel font draws a digit", ink0 > 10 && ink0 < 90,
               "'0' has no plausible ink");
            ck("and a letter", inkA > 10 && inkA < 90, "'A' has no ink");
            ck("a space clears its cell", inkSp == 0, "space left ink");
            ck("and an unmapped character draws nothing", inkLc == 0,
               "'a' drew something - the dispatch maps it");
            dat_free(&d);
        }
    }

    /* A hotspot table the menu never asked for, to catch an out-of-bounds
     * write in menu_hotspots if MENU_HOTSPOTS and the code disagree. */
    view_hotspot spots[MENU_HOTSPOTS];
    memset(spots, 0xAA, sizeof(spots));
    menu_hotspots(spots);
    /* A SKILL MUST NOT BE SPENT ON A LEMMING THAT CANNOT TAKE IT.
     *
     * Image 0x0321D refuses a digger when the state word has any of 0xF57F
     * set, and 0x10 - digging - is one of them. The port tested the
     * COMPLEMENT of that mask, so clicking a lemming that was already
     * digging assigned it again and spent a second digger. A player found
     * it; nothing here could, because every check compares against a run
     * where the click was made once. */
    {
        lemming a;
        memset(&a, 0, sizeof a);
        ck("a walker takes a digger", lem_dig_start(&a) == 1,
           "image 0x0320D refused a plain lemming");
        ck("and is digging afterwards", (a.flags & 0x0010) != 0,
           "the dig bit is not set");
        ck("a lemming already digging refuses a second",
           lem_dig_start(&a) == 0,
           "image 0x0321D's 0xF57F includes 0x10 - it must refuse");

        /* WHICH bits 0xF57F actually holds is worth asserting, because it
         * is not "anything busy". It is bits 0..6, 8, 10 and 12..15 - so a
         * BLOCKER is refused (0x100 is bit 8) and a BUILDER is not (0x80 is
         * bit 7, and it is clear). A builder can be turned into a digger
         * mid-brick; that is the original's rule, not an oversight here. */
        lemming b;
        memset(&b, 0, sizeof b);
        b.flags = 0x0100;                    /* blocking - bit 8, in the mask */
        ck("a blocker refuses a digger", lem_dig_start(&b) == 0,
           "0x100 is bit 8 and 0xF57F has it");
        memset(&b, 0, sizeof b);
        b.flags = 0x0080;                    /* building - bit 7, NOT in it */
        ck("but a builder takes one, because 0x80 is not in 0xF57F",
           lem_dig_start(&b) == 1,
           "the mask is bits 0..6, 8, 10 and 12..15 - bit 7 is clear");
    }

    /* WHAT THE RESULTS SCREEN IS TOLD, and where the port used to make it up.
     *
     * Both of these were decided in main.c, which no test reaches, and one of
     * them was simply wrong: a constant 0 for "timed out", so a level whose
     * clock had run out was told "All lemmings accounted for." A player found
     * it. They are game.c's now, with the original's addresses on them, which
     * is what lets a test ask. */
    {
        level lv;
        memset(&lv, 0, sizeof lv);
        lv.num_lemmings = 10; lv.num_to_rescue = 1;
        ck("the requirement is a percentage, image 0x04591",
           level_needed_percent(&lv) == 10, "10 of 1 is not 10%");
        lv.num_lemmings = 80; lv.num_to_rescue = 40;
        ck("and it divides rather than assuming tens",
           level_needed_percent(&lv) == 50, "40 of 80 is not 50%");
        lv.num_lemmings = 0;
        ck("a level that releases nobody does not divide by zero",
           level_needed_percent(&lv) == 0, "it divided by zero");

        /* Image 0x04710 asks the panel's own three digits. */
        level lv2;
        memset(&lv2, 0, sizeof lv2);
        lv2.time_limit = 5;
        level_status_reset(&lv2);
        ck("a fresh level has not run out of time",
           !level_out_of_time(), "5:00 read as out of time");
        for (uint32_t i = 0; i < 5u * 60u * LEVEL_FPS + 4 * LEVEL_FPS; i++)
            level_timer_step();
        ck("and one whose clock reads 0:00 has",
           level_out_of_time(), "the clock ran down and it said otherwise");
    }

    /* The access code's checksum - the one part of it two routines agree
     * on: image 0x05147 builds it and image 0x051D4 checks it. */
    {
        char code[11];
        memcpy(code, "AJHLDHBBCJ", 11);
        code[9] = level_code_checksum(code);
        ck("a generated code passes the validator's own rule",
           level_code_ok(code), "images 0x05147 and 0x051D4 disagree");
        code[3]++;
        ck("and one character out fails it",
           !level_code_ok(code), "the checksum does not depend on the code");
    }

    /* The results screen's TEXT, image 0x048BD. */
    {
        char ln[32];
        level_results_line(ln, 0, 10);
        ck("\"You rescued\" carries the percentage",
           strncmp(ln, "You rescued", 11) == 0
           && strstr(ln, "10") != NULL && strlen(ln) == 16,
           "image 0x048BD writes it three from the end");
        level_results_line(ln, 1, 100);
        ck("and \"You needed\" takes three digits",
           strncmp(ln, "You needed", 10) == 0 && strstr(ln, "100") != NULL,
           "100 does not fit where the original puts it");
        ck("the headline says which end it was",
           strcmp(level_results_headline(1), "Your time is up!") == 0
           && strncmp(level_results_headline(0), "All lemmings", 12) == 0,
           "images 0x001FA and 0x001E7");
        ck("and the prompt follows the input type",
           strstr(level_results_prompt(0), "mouse") != NULL
           && strstr(level_results_prompt(1), "button 2") != NULL
           && strstr(level_results_prompt(2), "Return") != NULL,
           "ds:0x25A6 picks among images 0x04C77, 0x04C9D and 0x04CB9");
    }

    /* THE GENERATOR, against the original's own. Every one of these was read
     * out of the guest by tools/probe_code.py, which called image 0x05042
     * with these three inputs and took the ten characters back from
     * ds:0x4D68. This is the difference between a table fitted to my reading
     * of the disassembly and a table the original produced. */
    {
        static const struct {
            uint8_t level, other, percent;
            const char *code;
        } DRIVEN[] = {
        {  0,   0,   0, "BAJHLDHBCL"},
        {  1,   0,   0, "IJHLDHBCCU"},
        {  2,   0,   0, "NHLDHBADCR"},
        {  4,   0,   0, "LDHBAJLFCT"},
        {  8,   0,   0, "BAJHMDHJCU"},
        { 16,   0,   0, "BAJHLFHBDO"},
        { 32,   0,   0, "BAJHLDIBEO"},
        { 64,   0,   0, "FAJHLDHBGT"},
        {128,   0,   0, "JAJHLDHBKL"},
        {  0,   1,   0, "BCJHLDHBCN"},
        {  0,   2,   0, "BAJILDHBCM"},
        {  0,   4,   0, "BAJHLEHBCM"},
        {  0,   8,   0, "BAJHLDJBCN"},
        {  0,  16,   0, "BAJHLDHBCL"},
        {  0,  32,   0, "BAJHLDHBCL"},
        {  0,  64,   0, "BAJHLDHBCL"},
        {  0, 128,   0, "BAJHLDHBCL"},
        {  0,   0,   1, "BEJHLDHBCP"},
        {  0,   0,   2, "BAKHLDHBCM"},
        {  0,   0,   4, "BAJJLDHBCN"},
        {  0,   0,   8, "BAJHNDHBCN"},
        {  0,   0,  16, "BAJHLLHBCT"},
        {  0,   0,  32, "BAJHLDLBCP"},
        {  0,   0,  64, "CAJHLDHBCM"},
        {  0,   0, 128, "BAJHLDHBCL"},
        { 37,   5,  62, "MMBKKNNGET"},
        {119,   3, 100, "GKNOLFMIJK"},
        {  7,   7,   7, "BOOOLEHICN"},
        };
        uint16_t bad = 0;
        char got[11], why[128] = "";
        for (uint16_t i = 0; i < sizeof DRIVEN / sizeof DRIVEN[0]; i++) {
            level_code_generate(DRIVEN[i].level, DRIVEN[i].other,
                                DRIVEN[i].percent, got);
            if (strcmp(got, DRIVEN[i].code) != 0) {
                if (!bad)
                    snprintf(why, sizeof why,
                             "level %u other %u percent %u: the original "
                             "made %s, the port makes %s",
                             DRIVEN[i].level, DRIVEN[i].other,
                             DRIVEN[i].percent, DRIVEN[i].code, got);
                bad++;
            }
        }
        ck("the generator matches the original on all 28 driven cases",
           bad == 0, why);
    }

    /* And the round trip: what the generator makes, the validator takes.
     * The two routines were written apart in the original, so this is a
     * check and not a tautology. */
    {
        uint16_t bad = 0;
        char got[11];
        for (uint16_t lv = 0; lv < 256; lv++)
            for (uint16_t pc = 0; pc <= 100; pc += 5) {
                level_code_generate((uint8_t)lv, 0, (uint8_t)pc, got);
                if (!level_code_ok(got)) bad++;
            }
        ck("every generated code passes the validator", bad == 0,
           "image 0x05042 and image 0x051D4 disagree");
    }

    /* WIN OR LOSE, image 0x048F6's ladder. The rungs are what the original
     * tests, in the order it tests them - and the order matters, because
     * each falls through to the next. Level 1 needs 10%. */
    {
        ck("rescuing none is the bottom", level_verdict(0, 10) == 0,
           "image 0x0490B");
        ck("under half of what was needed", level_verdict(4, 10) == 1,
           "image 0x04912's needed/2");
        ck("one short is its own message", level_verdict(9, 10) == 4,
           "image 0x04933's needed-1");
        ck("exactly what was needed is another", level_verdict(10, 10) == 5,
           "image 0x0493A");
        ck("comfortably over", level_verdict(29, 10) == 6,
           "image 0x04943's needed+20");
        ck("well over", level_verdict(31, 10) == 7, "the fall-through rung");
        ck("and everyone is the best", level_verdict(100, 10) == 8,
           "image 0x04904 is tested FIRST, before anything else");

        /* 100 is checked before the ladder, so it wins even when the level
         * asked for more than it could give. */
        ck("100 outranks every other rung", level_verdict(100, 100) == 8,
           "image 0x04904 must come first");

        ck("won is simply reaching the requirement",
           level_won(10, 10) && !level_won(9, 10),
           "image 0x04720's cmp and image 0x04724's jge");

        /* Image 0x0472E: on to the next, with the rating recomputed as
         * index / 30, and image 0x0473E's cap of 120. */
        uint8_t idx = 29, rat = 0;
        ck("the thirtieth level moves to the next rating",
           level_advance(&idx, &rat) && idx == 30 && rat == 1,
           "image 0x04737's divide by 30");
        idx = 118; rat = 3;
        ck("and there are a hundred and twenty of them",
           level_advance(&idx, &rat) && idx == 119
           && (level_advance(&idx, &rat) == 0),
           "image 0x0473E caps at 0x78");

        ck("and the text follows the rung",
           strstr(level_verdict_text(100, 10), "Superb") != NULL
           && strstr(level_verdict_text(0, 10), "ROCK BOTTOM") != NULL,
           "the nine messages are out of order");
    }

    /* THE EXIT, against where the original actually saved lemmings.
     *
     * tools/probe_exit.py drove the original into digging down and watched
     * for the frame a state became 0x2000. Six lemmings reached it, every
     * one at record (888, 147), with the exit object of "Just dig!" at
     * (848, 116). These are those numbers, not a reading of the format -
     * which is the point, because the trigger rectangle's units are what
     * the format leaves ambiguous. */
    {
        enum { EW = 64, EH = 64 };
        static uint8_t soil[EW * EH];
        char p6[1024], p7[1024];
        dat_file lv6;
        ground g6;
        level L6;
        const char *e6 = "unknown error";
        snprintf(p6, sizeof p6, "%s/level009.dat", dir);
        snprintf(p7, sizeof p7, "%s/ground0o.dat", dir);
        if (dat_load(p6, &lv6, &e6) == 0 && lv6.count > 1
                && level_parse(lv6.sections[1].data, lv6.sections[1].size,
                               &L6, &e6) == 0
                && ground_load(p7, &g6, &e6) == 0) {
            level_triggers_set(&L6, &g6);
            lemming e;
            memset(&e, 0, sizeof e);
            e.x = 888; e.y = 147; e.hot_x = -8; e.hot_y = -10; e.dir = 1;

            ck("a lemming at (888,147) is at the exit",
               level_at_exit(&e, &L6, &g6) == 1,
               "the trigger box is not where the original's is");

            e.x = 888 - 8;
            ck("and eight pixels short of it is not",
               level_at_exit(&e, &L6, &g6) == 0,
               "the box is wider than four pixels");

            e.x = 888; e.flags = 4;          /* falling */
            ck("nor is one falling past it",
               level_at_exit(&e, &L6, &g6) == 0,
               "image 0x02F3B's `test [si+8], 4` is missing");

            /* And what happens next: eight frames, then saved. */
            e.flags = 0;
            lem_exit_start(&e);
            ck("entering sets the state word to 0x2000",
               e.flags == 0x2000 && e.anim == 0, "image 0x02F42");
            level_saved_reset();
            for (uint8_t i = 0; i < 8; i++)
                lemmings_step(&e, 1, soil, EW, EH);
            ck("and eight frames later it is counted and gone",
               level_saved == 1 && e.alive == 0xFF,
               "image 0x02326 did not hand it to the count");
            level_triggers_set(NULL, NULL);
            dat_free(&lv6);
        }
    }

    /* The EXIT's sprite, against the bytes the original was reading. The
     * store frame captured mid-exit begins 00 00 00 00, and its 52 bytes of
     * colour sit at section 0 + 16816 - found by searching main.dat for the
     * exact bytes, not by dividing anything. */
    {
        char p8[1024];
        dat_file d8;
        const char *e8 = "unknown error";
        snprintf(p8, sizeof p8, "%s/main.dat", dir);
        if (dat_load(p8, &d8, &e8) == 0 && d8.count > 0
                && d8.sections[0].size >= 16816 + 52) {
            const uint8_t *x = d8.sections[0].data + 16816;
            /* Row 4 of frame 0 reads .....2222....... - two planes, two
             * bytes a row, thirteen rows. */
            uint8_t v = 0;
            for (uint8_t p = 0; p < 2; p++) {
                uint8_t byte = x[p * 26 + 4 * 2 + (5 >> 3)];
                v = (uint8_t)(v | (((byte >> (7 - (5 & 7))) & 1u) << p));
            }
            ck("the exit sprite is where the guest read it", v == 2,
               "section 0 + 16816 is not the leaving animation");
            dat_free(&d8);
        }
    }

    /* THE BUILDER FACES BOTH WAYS, and the file says so twice over.
     *
     * The port drew the right-facing sixteen whichever way a builder was
     * laying bricks - a player reported it, and the builder's own reference
     * cannot catch it because that one builds to the RIGHT.
     *
     * Two facts stand in for a check that cannot exist without a left-facing
     * recording. Both are about the DATA, not about the reading that produced
     * the fix:
     *
     *   the regions ABUT. BUILD_SPR_BASE and BASH_SPR_BASE were found
     *   separately, by tools/find_sprite.py scoring against the original's
     *   own frames, and 4240 + 32 * 78 comes to exactly 6736. Sixteen frames
     *   would leave 1,248 bytes of nothing between them, in a file where
     *   every other sprite region touches its neighbour.
     *
     *   frame 16 is NOT frame 0. If the left-facing set were absent, the
     *   index would land on whatever follows and the two would differ for a
     *   different reason - so this is weak on its own and only means
     *   something beside the arithmetic above.
     */
    {
        /* The numbers are written out rather than taken from game.c's
         * defines, like the exit's 16816 and the digger's above: this is a
         * fact about MAIN.DAT, and it should still hold if someone changes
         * what the port believes. 4240 is the builder's base and 6736 the
         * basher's, each found separately by tools/find_sprite.py; 78 is
         * sixteen wide by thirteen rows in three planes. */
        ck("the builder's 32 frames end where the basher begins",
           4240 + 32 * 78 == 6736,
           "the builder's region no longer abuts the basher's, so it is not "
           "sixteen frames each way any more");
        char p9[1024];
        dat_file d9;
        const char *e9 = "unknown error";
        snprintf(p9, sizeof p9, "%s/main.dat", dir);
        if (dat_load(p9, &d9, &e9) == 0 && d9.count > 0
                && d9.sections[0].size >= 4240 + 32 * 78) {
            const uint8_t *b0 = d9.sections[0].data + 4240;
            const uint8_t *b16 = b0 + 16 * 78;
            int same = 1;
            for (uint32_t i = 0; i < 78; i++)
                if (b0[i] != b16[i]) { same = 0; break; }
            ck("and frame 16 is a different picture from frame 0", !same,
               "the left-facing set is identical to the right-facing one");
            /* THE SHRUG'S SECOND EIGHT, found the same way: its base is
             * 19440 and eight frames of 40 end at 19760, where the next
             * thing the port knows about is the blast at 20080 - exactly
             * 320 bytes, which is eight more frames. */
            ck("the shrug's 16 frames end where the blast begins",
               19440 + 16 * 40 == 20080,
               "the shrug's region no longer reaches the blast's, so it is "
               "not eight frames each way any more");
            dat_free(&d9);
        }
    }

    /* The digger's SPRITE, against the bytes the original was reading.
     *
     * tools/probe_digger.py caught the guest mid-dig and followed image
     * 0x07112's `[0x1FE6] + bp` to the store; the frame there begins
     * 00 20 00 0a, and searching main.dat for those 84 bytes of colour
     * planes puts them at section 0 offset 720 - immediately after the
     * eighteen walker frames of forty bytes. So this checks the file the
     * port reads against the memory the original read. */
    {
        char p5[1024];
        dat_file d5;
        const char *e5 = "unknown error";
        snprintf(p5, sizeof p5, "%s/main.dat", dir);
        if (dat_load(p5, &d5, &e5) == 0 && d5.count > 0
                && d5.sections[0].size >= 720 + 84) {
            const uint8_t *g = d5.sections[0].data + 720;
            ck("the digger's frame is where the guest read it",
               g[0] == 0x00 && g[1] == 0x20 && g[2] == 0x00 && g[3] == 0x0A,
               "main.dat section 0 + 720 is not the digger");

            /* Row 8 of frame 0 reads ......32223..... - decoded from three
             * planes two bytes wide over fourteen rows. */
            uint8_t v = 0;
            for (uint8_t p = 0; p < 3; p++) {
                uint8_t byte = g[p * 28 + 8 * 2 + (6 >> 3)];
                v = (uint8_t)(v | (((byte >> (7 - (6 & 7))) & 1u) << p));
            }
            ck("and it decodes as 16x14 in three planes", v == 3,
               "row 8 column 6 is not the 3 the store showed");
            dat_free(&d5);
        }
    }

    /* THE DIGGER, against what the original actually did.
     *
     * tools/probe_digger.py drove the original into assigning one and
     * recorded this, so these are not numbers read off a disassembly and
     * hoped for - they are the original's own array, frame by frame:
     *
     *     f103  flags=0x0010  anim=17   assigned, x frozen
     *     f104  flags=0x0010  anim= 0   y 92->93   digs
     *     f105..f111          anim 1..7            does not dig
     *     f112  flags=0x0010  anim= 8   y 93->94   digs
     */
    {
        enum { W = 64, H = 64 };
        static uint8_t ground[W * H];
        for (uint16_t i = 0; i < W * H; i++) ground[i] = 8;   /* all solid */

        lemming a;
        memset(&a, 0, sizeof a);
        a.x = 40; a.y = 40; a.dir = 1; a.state = 9;
        a.hot_x = -8; a.hot_y = -10;

        ck("a walker takes a digger", lem_dig_start(&a) != 0,
           "the assignment was refused");
        ck("which sets bit 0x10 and parks the animation at 17",
           a.flags == 0x0010 && a.anim == 17, "image 0x0320D's fields");
        ck("and drops the hot spot two rows", a.hot_y == -12,
           "image 0x0325x sets hot_y to -12");

        int16_t y0 = a.y;
        lemmings_step(&a, 1, ground, W, H);
        ck("the frame after, it is on anim 0 and has dug",
           a.anim == 0 && a.y == y0 + 1, "the first dig did not happen");
        /* The FIRST frame cuts three rows - image 0x0250C does y-2 and y-1
         * before falling into the ordinary dig at image 0x0240F for the row
         * it is standing on - and the row below is left alone. The column
         * is x-4: 0x0FF8's value bits 3..11 are PIXELS 4..12, because the
         * leftmost pixel is the most significant bit. */
        const int32_t col = a.x - 4 - 16;
        ck("the first frame cuts three rows",
           ground[(y0 - 2 - 16) * W + col] == 0
           && ground[(y0 - 1 - 16) * W + col] == 0
           && ground[(y0 - 16) * W + col] == 0,
           "image 0x0250C's two extra rows are missing");
        ck("and stops there",
           ground[(y0 + 1 - 16) * W + col] == 8
           && ground[(y0 - 3 - 16) * W + col] == 8,
           "the shaft runs past what image 0x0250C cuts");
        ck("the span is x-4 to x+4",
           ground[(y0 - 16) * W + (a.x - 4 - 16)] == 0
           && ground[(y0 - 16) * W + (a.x + 4 - 16)] == 0
           && ground[(y0 - 16) * W + (a.x - 5 - 16)] == 8
           && ground[(y0 - 16) * W + (a.x + 5 - 16)] == 8,
           "the nine-pixel span is not where the mask puts it");

        int moved = 0;
        for (uint8_t i = 1; i <= 7; i++) {
            int16_t before = a.y;
            lemmings_step(&a, 1, ground, W, H);
            if (a.anim != i || a.y != before) moved = 1;
        }
        ck("anims 1..7 animate and do not dig", !moved,
           "it dug on a frame the original does not");

        int16_t y8 = a.y;
        lemmings_step(&a, 1, ground, W, H);
        ck("and anim 8 digs again", a.anim == 8 && a.y == y8 + 1,
           "the second dig is not on anim 8");

        /* Nothing under it any more - image 0x0244E turns it into a fall. */
        lemming b;
        memset(&b, 0, sizeof b);
        b.x = 40; b.y = 40; b.dir = 1; b.state = 9; b.hot_x = -8; b.hot_y = -10;
        static uint8_t empty[W * H];
        memset(empty, 0, sizeof empty);
        lem_dig_start(&b);
        lemmings_step(&b, 1, empty, W, H);
        ck("digging into nothing turns into a fall",
           (b.flags & 0x0010) == 0 && (b.flags & 4) != 0,
           "image 0x0244E did not swap the state");
    }

    /* And which lemming the pointer is over, image 0x03D0A. The same
     * coordinates that a probe got backwards: level y is mouse_y PLUS eight,
     * so to point at a lemming the mouse sits eight rows above it. */
    {
        lemming l[2];
        memset(l, 0, sizeof l);
        l[0].x = 784; l[0].y = 92; l[0].hot_x = -8; l[0].hot_y = -10;
        l[1].x = 900; l[1].y = 92; l[1].hot_x = -8; l[1].hot_y = -10;

        /* Its box is (784 - 8 - 4, 92 - 10 - 1) = (772, 81), twelve square,
         * so it spans 772..784 across and 81..93 down. The pointer is at
         * `view - 16 + mouse_x` and `mouse_y + 8`, so with the view at 640
         * the middle of that box is mouse (154, 79) - which is exactly where
         * tools/probe_digger.py's click landed on the original. */
        /* The PORT's view_x, 624, not the guest's 640 - they differ by the
         * sixteen prove.sh prints as its offset, and writing the guest's
         * number here is what let an off-by-sixteen in the hit test pass. */
        lem_mouse_move(154, 79);
        ck("the pointer finds the lemming under it",
           level_lemming_at(l, 2, 624) == 0, "the 12x12 box missed");

        lem_mouse_move(154, 79 + 16);
        ck("and sixteen rows lower finds nothing",
           level_lemming_at(l, 2, 624) < 0,
           "the box is not where image 0x03D27 puts it");

        lem_mouse_move(154 + (900 - 784), 79);
        ck("the second lemming is found at its own x",
           level_lemming_at(l, 2, 624) == 1, "the scan stopped at the first");

        /* And it must MISS when the pointer is sixteen off, which is the
         * error the old formula made on every click. */
        lem_mouse_move(154 - 16, 79);
        ck("sixteen pixels left of it is a miss",
           level_lemming_at(l, 2, 624) < 0,
           "the box is sixteen pixels wide of where it belongs");
    }

    /* What the pointer is over, image 0x03EF9. The coordinates are the
     * screen's plus eight throughout, which is the thing easiest to get
     * wrong here. */
    {
        level_hit h;
        lem_mouse_move(160, 100);            /* screen (152, 92) */
        h = level_pointer_hit();
        ck("the play area is not the panel", h.what == LEVEL_HIT_PLAY,
           "a pointer in the level hit the panel");

        lem_mouse_move(160, 184);            /* screen row 176, the panel */
        h = level_pointer_hit();
        ck("row 176 is where the panel starts", h.what == LEVEL_HIT_BUTTON,
           "the panel's first row read as play area");
        ck("and the button is the screen x over sixteen", h.button == 9,
           "the button index is not (x - 8) >> 4");

        lem_mouse_move(8 + 0xD0, 190);       /* the minimap's first column */
        h = level_pointer_hit();
        ck("past 0xD0 is the minimap", h.what == LEVEL_HIT_MINIMAP,
           "the minimap read as a button");
        ck("and its left end asks for scroll 0", h.scroll == 0,
           "the low clamp at image 0x03FC1 is missing");

        lem_mouse_move(8 + 0xDA + 0x4F, 190);
        h = level_pointer_hit();
        ck("its right end asks for the last column", h.scroll == 0x4F * 16,
           "the high clamp at image 0x03FC5 is missing");
        ck("which is exactly a 1584-wide level's last view",
           h.scroll == 1584 - 320, "0x4F * 16 is not 1264");

        lem_mouse_move(8 + 0xDA + 0x80, 190);
        h = level_pointer_hit();
        ck("and it does not run past that", h.scroll == 0x4F * 16,
           "a click past the minimap scrolled beyond the level");
    }

    /* "1 Minute", not "1 Minutes" - image 0x045B2 swaps the 's' for a space
     * when the digit is '1', which is why the line keeps its length. Nothing
     * in level009.dat is a one-minute level, so no capture of the briefing
     * can catch this and it is checked directly. */
    {
        char t[32];
        snprintf(t, sizeof t, "%s", "Time   Minutes");
        t[5] = '1'; if (t[5] == '1') t[13] = ' ';
        ck("a one-minute level says Minute", t[13] == ' ' && strlen(t) == 14,
           "the plural was not handled, or the line changed length");
        snprintf(t, sizeof t, "%s", "Time   Minutes");
        t[5] = '5'; if (t[5] == '1') t[13] = ' ';
        ck("and five minutes still says Minutes", t[13] == 's',
           "the 's' went missing");
    }

    /* The palette fade, image 0x0570C. Two notches a component, and it
     * stops ON the target rather than stepping past it - which is what the
     * `jg` after the first step is for. The ramp asserted here is the one
     * tools/probe_dac.py recorded off the running original's DAC on the
     * briefing: c3c300, cbcb00, d3d300, dbdb00, e3e300, ebeb00, f3f300. */
    {
        lem_rgb cur = { 0xc3, 0xc3, 0x00 };
        lem_rgb tgt = { 0xf3, 0xf3, 0x00 };
        static const uint8_t WANT[] = { 0xcb, 0xd3, 0xdb, 0xe3, 0xeb, 0xf3 };
        int ok = 1;
        for (uint8_t i = 0; i < sizeof WANT; i++) {
            if (!palette_fade_step(&cur, &tgt, 1)) { ok = 0; break; }
            if (cur.r != WANT[i] || cur.g != WANT[i] || cur.b != 0x00) {
                ok = 0;
                break;
            }
        }
        ck("the fade walks the DAC the original walked it",
           ok, "the ramp is not the one measured on the briefing");
        ck("and reports itself finished once it arrives",
           palette_fade_step(&cur, &tgt, 1) == 0, "it kept moving");
    }
    {
        /* An ODD distance has to land exactly, not overshoot: one notch
         * short, the second step is the one that must not be taken. */
        lem_rgb cur = { 0x00, 0x00, 0x00 };
        lem_rgb tgt = { 0x04, 0x00, 0x00 };   /* six-bit 1 */
        palette_fade_step(&cur, &tgt, 1);
        ck("an odd distance lands on the target", cur.r == 0x04,
           "the fade stepped past it");
    }
    {
        /* And downwards, which is the fade OUT. */
        lem_rgb cur = { 0xf3, 0x00, 0x00 };
        lem_rgb tgt = { 0x00, 0x00, 0x00 };
        palette_fade_step(&cur, &tgt, 1);
        ck("and it fades down as well as up", cur.r == 0xeb,
           "downwards did not step by two");
    }

    /* The disk check, image 0x0062B. A fixed disk passes because the
     * ORIGINAL passes it - image 0x0062E jumps clear of the protection - and
     * a floppy passes here only because LEM_DISK_CHECK is off. Both are
     * asserted so that turning it on is a visible change and not a silent
     * one, and so the no-op is a decision the tests know about. */
    ck("disk_check passes a fixed disk (image 0x0062E)",
       disk_check(0xF8) == 0, "0xF8 was refused");
    ck("and a floppy too, LEM_DISK_CHECK being off",
       disk_check(0xF9) == 0, "0xF9 was refused with the check disabled");

    ck("menu_hotspots fills its array",
       spots[MENU_HOTSPOTS - 1].w == MENU_W, "last hotspot not written");

    /* THE RATE BUTTONS STEP ONCE PER PRESS AND ONCE PER THREE FRAMES HELD.
     *
     * tools/verify_rate.py drives image 0x04032 itself and agrees on 240 of
     * 240 clicks - but it pokes ds:0x69 to zero before every one, on BOTH
     * sides, so it scores the step arithmetic and its two bounds and says
     * nothing about the PACING. That is where the fault was: the divider was
     * tested in level_rate_held rather than in the click, so a press stepped
     * once as an edge and the very next frame's held path found the divider
     * at rest and stepped again. One click moved the rate by two. A player
     * noticed the amount was wrong.
     *
     * Image 0x0403D and image 0x0406C test ds:0x69 before either bound is
     * looked at, and image 0x03B8C turns it from the panel's redraw - only
     * while the drawn number is stale, which is what makes the FIRST press
     * always take and the next two frames refuse. */
    {
        char pr[1024];
        dat_file lvr;
        level Lr;
        const char *er = "unknown error";
        snprintf(pr, sizeof pr, "%s/level009.dat", dir);
        if (dat_load(pr, &lvr, &er) == 0 && lvr.count > 0
                && level_parse(lvr.sections[0].data, lvr.sections[0].size,
                               &Lr, &er) == 0) {
            level_release_reset(&Lr);
            level_rate_div_reset();
            uint8_t r0 = level_rate();

            /* A FRAME is a click while the button is down, then the
             * panel's redraw - which is the order sdl_io.c runs them in and
             * the order image 0x04032 and image 0x03B8C sit in the loop. */
            ck("a rate press steps once", level_rate_click(1) == 1
               && level_rate() == (uint8_t)(r0 + 1),
               "the press did not take, or moved by the wrong amount");
            level_rate_tick();

            ck("holding it, the next frame does not step",
               level_rate_click(1) == 0 && level_rate() == (uint8_t)(r0 + 1),
               "the divider is not gating the press - one press moves the "
               "rate by two");
            level_rate_tick();

            ck("nor the frame after", level_rate_click(1) == 0,
               "the divider let a press through after two frames");
            level_rate_tick();

            ck("but the third steps again", level_rate_click(1) == 1
               && level_rate() == (uint8_t)(r0 + 2),
               "a held button did not step one frame in three");
            dat_free(&lvr);
        }
    }

    /* THE NUKE IS A DOUBLE CLICK, AND IT HAS TO STAY ONE ACROSS LEVELS.
     *
     * Nothing here tested it, and it broke: the four bytes image 0x00438,
     * image 0x0043B and image 0x0044F's wipe clear at a level start were not
     * reset by the port, so they carried over and a single click nuked. A
     * player found it after playing several levels in a row.
     *
     * The middle case is the one that matters. Image 0x04007 is a SIGNED
     * compare - the signedness is deliberate, so a wrapped counter is let
     * through - which means a STALE `last` gives a negative difference and
     * the first press is accepted. Ticking past 128 with a stale last is
     * exactly the state a second level starts in. */
    {
        level_start_reset();
        for (int i = 0; i < 40; i++) level_tick();
        ck("a first nuke press is refused",
           level_nuke_click() == 0, "one click armed the nuke");
        ck("and a second, straight after, fires it",
           level_nuke_click() == 1, "the double click did not take");
        ck("and the nuke is now running", level_nuking() == 0xFF,
           "ds:0x4B was not set");

        /* A NEW LEVEL CLEARS ds:0x4B. Without this a nuked level left it at
         * 0xFF and the next one began its cascade the moment it started. */
        level_start_reset();
        ck("a new level clears the nuke", level_nuking() == 0,
           "ds:0x4B carried over - the next level nukes itself");

        /* AND THE QUIRK IS ASSERTED, not corrected. Image 0x04007 is a
         * SIGNED compare, so a gap of 128 or more reads as negative and is
         * accepted ON ITS OWN: for half of every fifteen seconds one click
         * nukes the level. The original does this - it lit its first fuse on
         * the first press of the reference run, at the same frame and with
         * the same 79 as the port.
         *
         * It is asserted so that nobody "fixes" it into an unsigned compare
         * later. What was actually wrong was the four bytes above not being
         * cleared, which made this window turn up at random rather than on
         * the original's own schedule. */
        for (int i = 0; i < 200; i++) level_tick();
        ck("one press 200 frames in nukes on its own - the original's quirk",
           level_nuke_click() == 1,
           "image 0x04007's signed compare has been made unsigned");
        level_start_reset();
    }

    /* THE ONE-WAY TILES CARRY ONE COLOUR, AND IT IS NOT THE ONE ON SCREEN.
     *
     * game.c's object_pixel_value says an only-on-terrain object lands as
     * attribute 4 | plane 0, because image 0x096BA sets Set/Reset to 4 and
     * image 0x096C2 sets Enable Set/Reset to 0x0E. A unit test asserting
     * that against object_pixel_value would only repeat the reading the code
     * was written from, which this project has been caught doing before.
     *
     * So this asserts the FILE instead, which no reading of the blitter can
     * change: both one-way tiles in ground2o.dat decode to attribute 1 and
     * nothing else. The original's video memory holds 5 where it draws them.
     * A sprite of one colour cannot produce two, so the colour is the
     * blitter's - and that is true whether or not image 0x096BA is read
     * correctly. If this test ever fails, the premise is gone and
     * object_pixel_value has to be re-argued rather than adjusted. */
    {
        char p8[1024];
        dat_file gr;
        const char *e8 = "unknown error";
        snprintf(p8, sizeof p8, "%s/vgagr2.dat", dir);
        if (dat_load(p8, &gr, &e8) == 0 && gr.count > 1) {
            const uint8_t *sec = gr.sections[1].data;
            uint32_t len = gr.sections[1].size;
            /* id3 at 24600 and id4 at 29080, from ground2o.dat's own object
             * records: 32x32, four ink planes of 128 bytes then a mask. */
            const uint16_t base[2] = { 24600, 29080 };
            uint16_t lit = 0, other = 0;
            for (uint8_t k = 0; k < 2; k++) {
                if ((uint32_t)base[k] + 640 > len) { other = 0xFFFF; break; }
                for (uint16_t r = 0; r < 32; r++)
                    for (uint16_t c = 0; c < 32; c++) {
                        uint16_t byte = (uint16_t)(r * 4 + (c >> 3));
                        uint8_t bit = (uint8_t)(7 - (c & 7));
                        if (((sec[base[k] + 512 + byte] >> bit) & 1) == 0)
                            continue;
                        uint8_t v = 0;
                        for (uint8_t pl = 0; pl < 4; pl++)
                            if ((sec[base[k] + pl * 128 + byte] >> bit) & 1)
                                v = (uint8_t)(v | (1u << pl));
                        if (v == 1) lit++; else other++;
                    }
            }
            ck("the one-way tiles are attribute 1 and nothing else",
               lit == 400 && other == 0,
               "a one-way tile decoded to some other attribute, so the "
               "screen's 5 might come from the sprite after all");
            dat_free(&gr);
        }
    }

    printf(fails ? "\n  %d FAILED\n" : "\n  all passed\n", fails);
    return fails ? 1 : 0;
}
