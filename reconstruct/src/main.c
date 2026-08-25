/* main.c - the command line, and the screens it can ask for.
 *
 * THIS FILE IS THE PORT'S OWN: the original had no command line. It exists so
 * a developer can ask for one screen, or for a file to compare against the
 * emulator, without going through the game.
 *
 * Run with no arguments it SHOWS A SCREEN - the main menu. A reconstruction
 * whose default output is a bitmap is a converter; asking for --out or --raw
 * is what turns the window off.
 *
 *   ./lemmings                       the main menu, in a window
 *   ./lemmings --level 3             the third level in the container
 *   ./lemmings --out x.bmp           write a picture instead of showing one
 *
 * It reads the player's own copy of the original data files; nothing of the
 * game is redistributed here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"

/* --- BMP output: the port's own, for looking at and for diffing. --- */



static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

int bmp_write(const char *path, const uint8_t *indices,
              uint16_t w, uint16_t h, const lem_rgb *palette,
              const char **err)
{
    uint32_t row = ((uint32_t)w * 3u + 3u) & ~3u;   /* rows pad to 4 bytes */
    uint32_t pix_bytes = row * h;

    uint8_t hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    put32(hdr + 2, 54u + pix_bytes);
    put32(hdr + 10, 54u);
    put32(hdr + 14, 40u);
    put32(hdr + 18, w);
    put32(hdr + 22, h);
    put16(hdr + 26, 1);
    put16(hdr + 28, 24);
    put32(hdr + 34, pix_bytes);

    FILE *f = fopen(path, "wb");
    if (!f) { if (err) *err = "cannot open output file"; return -1; }
    fwrite(hdr, 1, sizeof(hdr), f);

    uint8_t *line = calloc(row, 1);
    if (!line) { fclose(f); if (err) *err = "out of memory"; return -1; }

    /* BMP rows run bottom to top. */
    for (int32_t y = (int32_t)h - 1; y >= 0; y--) {
        memset(line, 0, row);
        for (uint16_t x = 0; x < w; x++) {
            lem_rgb c = palette[indices[(uint32_t)y * w + x] & 0x0F];
            line[x * 3 + 0] = c.b;
            line[x * 3 + 1] = c.g;
            line[x * 3 + 2] = c.r;
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    return 0;
}




static const char *SKILL_NAMES[8] = {
    "climber", "floater", "bomber", "blocker",
    "builder", "basher", "miner", "digger"
};

static void usage(void)
{
    fputs("usage: lemmings [--data DIR] [--menu|--level N] [--out FILE]\n"
          "  with no arguments at all, shows the main menu in a window\n"
          "  --data DIR    directory holding the original files (default .)\n"
          "  --menu        the main menu (the default screen)\n"
          "  --file NAME   level container (default level009.dat)\n"
          "  --level N     which level inside it, 1-based (default 2)\n"
          "                the default pair is the level the game itself\n"
          "                calls Level 1 - the file order is not the game's\n"
          "  --out FILE    write the picture to FILE instead of showing it\n"
          "  --show        show it as well, when --out or --raw asked for a file\n"
          "  --scale N     window magnification (default 3)\n"
          "  --terrain M   how terrain pixels combine: replace|or|keep\n"
          "  --planebits O which bit each stored plane feeds, e.g. 0123\n"
          "  --bitorder B  leftmost pixel is the byte's msb or lsb\n"
          "  --reverse     draw the terrain list back to front\n"
          "  --no-objects  terrain only, as the original composes it first\n"
          "  --raw FILE    also write the raw indices and palette, for\n"
          "                checking against the original: 'LEMRW2', width\n"
          "                and height as u16, 16 RGB triples, then w*h bytes\n",
          stderr);
}

/* Load a level, compose it, and do whatever the command line asked for
 * with the result. Split out of main() so the menu can start a level and
 * come back, which is what makes the menu a menu rather than a picture. */
static int render_level(const char *data, const char *file, uint32_t which,
                        int objects, const char *out, const char *raw,
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
    if (objects && gfx.count >= 2) {
        if (render_objects(&c, &lv, &g, gfx.sections[1].data,
                           gfx.sections[1].size, 0, &err) != 0) {
            fprintf(stderr, "render objects: %s\n", err);
            canvas_free(&c);
            dat_free(&gfx);
            return 1;
        }
    }
    dat_free(&gfx);

    if (bmp_write(out, c.pix, c.w, c.h, g.palette, &err) != 0) {
        fprintf(stderr, "%s: %s\n", out, err);
        canvas_free(&c);
        return 1;
    }
    printf("wrote %s (%dx%d)\n", out, LEVEL_W, LEVEL_H);

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

    /* Show it unless a file was the whole point of the run. */
    if (show || (!out && !raw)) {
        char title[128];
        snprintf(title, sizeof(title), "PC Lemmings - %s", lv.name);
        view_run(&c, g.palette, title, lv.start_x, scale);
    }
    canvas_free(&c);
    return 0;
}

int main(int argc, char **argv)
{
    /* The level the GAME calls "Level 1" is not the first level in the first
     * file. In this demo build it is section 1 (0-based) of level009.dat -
     * established by running the original under an emulator and reading its
     * own briefing screen: "Just dig!", 10 lemmings, 10% to be saved, release
     * rate 50, 5 minutes, diggers only. Parsing that section gives exactly
     * those numbers. The demo's level order is not its file order, and what
     * maps one to the other has not been traced yet. */
    const char *data = ".";
    const char *file = "level009.dat";
    const char *out  = NULL;
    const char *raw  = NULL;
    uint32_t which = 2;
    uint8_t scale = 3;
    int show = 0;
    /* The menu is the default screen, and anything that names a level asks
     * for the level instead. --out and --raw are file requests, so they turn
     * the window off unless --show says otherwise; that is what keeps
     * tools/prove.sh working unchanged. */
    int want_menu = 1;
    int menu_forced = 0;
    /* The original composes terrain first and draws objects afterwards, so a
     * capture taken at the end of its terrain loop has no objects in it. This
     * lets the port match that, for a comparison that is terrain against
     * terrain and nothing else. */
    int objects = 1;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc)       data = argv[++i];
        else if (!strcmp(argv[i], "--menu")) menu_forced = 1;
        else if (!strcmp(argv[i], "--file") && i + 1 < argc)  { file = argv[++i]; want_menu = 0; }
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)   { out  = argv[++i]; want_menu = 0; }
        else if (!strcmp(argv[i], "--level") && i + 1 < argc) { which = (uint32_t)atoi(argv[++i]); want_menu = 0; }
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = (uint8_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--show")) show = 1;
        else if (!strcmp(argv[i], "--no-objects")) objects = 0;
        else if (!strcmp(argv[i], "--raw") && i + 1 < argc)   { raw  = argv[++i]; want_menu = 0; }
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

    if (want_menu) {
        menu_screen mm;
        if (menu_build(data, &mm, &err) != 0) {
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
        if (out) {
            if (bmp_write(out, mm.pix, MENU_W, MENU_H, mm.palette,
                          &err) != 0) {
                fprintf(stderr, "%s: %s\n", out, err);
                rc = 1;
            } else {
                printf("   wrote %s (%dx%d)\n", out, MENU_W, MENU_H);
            }
        }
        if (out || raw) {          /* a file was the point of the run */
            menu_free(&mm);
            return rc;
        }

        /* The menu loop. F1 starts the level and coming back out of it
         * returns here, which is the whole point of the screen. */
        for (;;) {
            menu_action a = menu_run(&mm, scale);
            if (a == MENU_EXIT) break;
            if (a == MENU_PLAY) {
                render_level(data, file, which, objects, NULL, NULL,
                             scale, 1);
                continue;
            }
            /* The other four plates are drawn and answer to their keys, but
             * what they DO is not reconstructed. Saying so is better than a
             * silent no-op, which reads as a broken button. */
            printf("  %s is not reconstructed yet\n",
                   a == MENU_NEW_LEVEL ? "F2, \"NEW LEVEL\"" :
                   a == MENU_FX        ? "F3, \"FX\"" :
                   a == MENU_RATING    ? "the rating plate" :
                                         "F4, the \"?\" plate");
        }
        menu_free(&mm);
        return rc;
    }

    return render_level(data, file, which, objects, out, raw, scale, show);
}
