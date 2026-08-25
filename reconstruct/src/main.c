/* SPDX-License-Identifier: GPL-2.0-only
 *
 * PC Lemmings, reconstructed - level loader and terrain renderer.
 *
 *   ./lemmings                       level 1 of the Fun set -> level.bmp
 *   ./lemmings --level 3             the third level in the container
 *   ./lemmings --data ../lemming1.pc --out /tmp/x.bmp
 *
 * It reads the player's own copy of the original data files; nothing of the
 * game is redistributed here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bmp.h"
#include "dat.h"
#include "ground.h"
#include "level.h"
#include "render.h"
#include "vgagr.h"
#include "view.h"

static const char *SKILL_NAMES[8] = {
    "climber", "floater", "bomber", "blocker",
    "builder", "basher", "miner", "digger"
};

static void usage(void)
{
    fputs("usage: lemmings [--data DIR] [--file NAME] [--level N] [--out FILE]\n"
          "  --data DIR    directory holding the original files (default .)\n"
          "  --file NAME   level container (default level009.dat)\n"
          "  --level N     which level inside it, 1-based (default 2)\n"
          "                the default pair is the level the game itself\n"
          "                calls Level 1 - the file order is not the game's\n"
          "  --out FILE    where to write the picture (default level.bmp)\n"
          "  --show        open a window on the level (arrows scroll, Esc quits)\n"
          "  --scale N     window magnification for --show (default 3)\n"
          "  --terrain M   how terrain pixels combine: replace|or|keep\n"
          "  --planebits O which bit each stored plane feeds, e.g. 0123\n"
          "  --raw FILE    also write the raw indices and palette, for\n"
          "                checking against the original: 'LEMRW2', width\n"
          "                and height as u16, 16 RGB triples, then w*h bytes\n",
          stderr);
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
    const char *out  = "level.bmp";
    const char *raw  = NULL;
    uint32_t which = 2;
    uint8_t scale = 3;
    int show = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc)       data = argv[++i];
        else if (!strcmp(argv[i], "--file") && i + 1 < argc)  file = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)   out  = argv[++i];
        else if (!strcmp(argv[i], "--level") && i + 1 < argc) which = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = (uint8_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--show")) show = 1;
        else if (!strcmp(argv[i], "--raw") && i + 1 < argc)   raw  = argv[++i];
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
            else { usage(); return 2; }
        }
        else { usage(); return 2; }
    }

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
    if (gfx.count >= 2) {
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

    if (show) {
        char title[128];
        snprintf(title, sizeof(title), "PC Lemmings - %s", lv.name);
        if (view_run(&c, g.palette, title, lv.start_x, scale) != 0)
            fprintf(stderr, "no display available; the picture is in %s\n", out);
    }
    canvas_free(&c);
    return 0;
}
