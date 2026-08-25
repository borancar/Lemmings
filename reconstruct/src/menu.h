/* SPDX-License-Identifier: GPL-2.0-only
 *
 * The main menu screen: 640x350, 16 colours, from MAIN.DAT.
 */
#ifndef LEM_MENU_H_
#define LEM_MENU_H_

#include "lem.h"

#define MENU_W 640
#define MENU_H 350

typedef struct {
    uint8_t *pix;            /* MENU_W * MENU_H, values 0..15 */
    lem_rgb  palette[16];
} menu_screen;

/* Compose the menu from `dir`/main.dat. Returns 0 on success. */
int  menu_build(const char *dir, menu_screen *m, const char **err);
void menu_free(menu_screen *m);

#endif /* LEM_MENU_H_ */
