/* data.h - the transcribed DATA, declared.
 *
 * game.c holds the game; these are the tables it reads. They were inside it,
 * and a thousand lines of transcribed bytes between two routines makes the
 * routines harder to find and says nothing while you scroll past them.
 *
 * Everything here is TRANSCRIBED, from the address in its comment in data.c,
 * and none of it is the port's own. A table that had to be computed rather
 * than read would not belong here.
 */
#ifndef DATA_H
#define DATA_H

#include <stdint.h>
#include "game.h"

/* THE SHAPES, which are facts about the tables and not about the code that
 * reads them - so they travel with the data. Each carries the address it
 * was read from, as everything transcribed here does. */
/* THE RESULTS SCREEN'S LAYOUT - transcribed from image 0x04D4D.
 *
 * This was "not read" for a long time and it is data, not code. Image
 * 0x04D4D walks a chain of records and every one carries its own position:
 *
 *     one byte  the column PLUS ONE - image 0x04D54 decrements it, and a
 *               zero is what ends the chain (image 0x04D50)
 *     one byte  the row
 *     one byte  a style; zero gives image 0x04D5E's bp = 2
 *     the text, with 0x0D ending the line (image 0x04D66)
 *
 * Image 0x04D78 then puts each character at (column * 16, row * 16) from a
 * 16x16 glyph, so the screen is a 40 by 21 grid - and it is mode 0x10 at
 * 640x350, NOT the level's 0x0D. That is why tools/record_level.py cannot
 * reach it: it captures at image 0x018F, the level loop's own end-of-frame,
 * and this screen has a loop of its own.
 */
typedef struct {
    uint8_t     col, row, style;
    const char *text;
} results_line;

#define VERDICTS 9

#define MINE_ROWS    13         /* 312 bytes a slice / 8 shifts / 3 = 13 */

#define CRATER_ROWS  22         /* image 0x05E17: cx = 0x16 */

#define CLOUD_SPR_H 32

#define CLOUD_SPR_W 32

#define DEBRIS_ROWS  52

#define DEBRIS_N     80    /* ds:0x4D - image 0x0042C stores 0x50 over the
                            * 0x80 the image ships with, so eighty and not 128 */

#define DEBRIS_GONE  (-128)  /* 0x80: this one has flown off already */

#define CURSOR_H 14

#define CURSOR_W 14

#define CURSOR_BOX_H  14

extern const results_line VERDICT_AT[VERDICTS][2];
extern const lem_rgb RESULTS_PAL[16];
extern const uint32_t BASH_MASK[2][4][10];
extern const uint32_t MINE_MASK[2][2][MINE_ROWS];
extern const uint16_t CRATER[CRATER_ROWS];
extern const uint8_t CLOUD_SPR[CLOUD_SPR_H][CLOUD_SPR_W];
extern const int8_t DEBRIS_AT[DEBRIS_ROWS][DEBRIS_N][2];
extern const uint8_t CURSOR[CURSOR_H][CURSOR_W];
extern const uint8_t CURSOR_MASK[CURSOR_H][CURSOR_W];
extern const uint8_t CURSOR_BOX[CURSOR_BOX_H][CURSOR_W];
extern const uint8_t CURSOR_BOX_MASK[CURSOR_BOX_H][CURSOR_W];
extern const uint8_t LEVEL_ORDER[120];
extern const uint8_t BRIEF_PAL6[7 * 3];
extern const uint8_t MENU_XLAT[0x53];

#endif /* DATA_H */
