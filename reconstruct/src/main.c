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
#include "view.h"

static const char *SKILL_NAMES[8] = {
    "climber", "floater", "bomber", "blocker",
    "builder", "basher", "miner", "digger"
};

static void usage(void)
{
    fputs("usage: lemmings [--data DIR] [--file NAME] [--level N] [--out FILE]\n"
          "  --data DIR    directory holding the original files (default .)\n"
          "  --file NAME   level container (default level000.dat)\n"
          "  --level N     which level inside it, 1-based (default 1)\n"
          "  --out FILE    where to write the picture (default level.bmp)\n"
          "  --show        open a window on the level (arrows scroll, Esc quits)\n"
          "  --scale N     window magnification for --show (default 3)\n",
          stderr);
}

int main(int argc, char **argv)
{
    const char *data = ".";
    const char *file = "level000.dat";
    const char *out  = "level.bmp";
    uint32_t which = 1;
    uint8_t scale = 3;
    int show = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc)       data = argv[++i];
        else if (!strcmp(argv[i], "--file") && i + 1 < argc)  file = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)   out  = argv[++i];
        else if (!strcmp(argv[i], "--level") && i + 1 < argc) which = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = (uint8_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--show")) show = 1;
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

    if (show) {
        char title[128];
        snprintf(title, sizeof(title), "PC Lemmings - %s", lv.name);
        if (view_run(&c, g.palette, title, lv.start_x, scale) != 0)
            fprintf(stderr, "no display available; the picture is in %s\n", out);
    }
    canvas_free(&c);
    return 0;
}
