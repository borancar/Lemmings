/* adlib.c - the Ad-Lib Music/FX driver, transcribed.
 *
 * PROVENANCE. This is NOT transcribed from vgalemmi.exe. It is transcribed
 * from `adlib.dat`, which the game loads onto INT 61h and calls - a separate
 * program, and one the game did not write: it names itself
 *
 *     " Ad-Lib Music/FX Driver - (c) 1991 Sound Images "
 *
 * Every address in adlib.c is an offset into THAT file's decompressed section
 * 0 (22,125 bytes), written `driver +0x0NNN`, and never an image offset into
 * vgalemmi.exe. The two numbering schemes would otherwise be indistinguishable
 * in a comment, which is exactly the confusion CLAUDE.md's rule about image
 * offsets exists to prevent.
 *
 * It is its own translation unit because it was its own program. The port's
 * files mirror the original's, and this is the clearest case of that there is.
 *
 * WHAT IS HERE and WHAT IS NOT:
 *
 *   here      the sequencer - which registers get written, and when. That is
 *             a program, it is readable, and it is checkable write for write
 *             against the original's own stream (tools/probe_opl.py).
 *
 *   not here  the music. Twenty-one tunes and seventeen effects live in
 *             adlib.dat as data and stay there: this reads them out of the
 *             player's own copy. Nothing of the game is redistributed.
 *
 *   not here  the chip. See opl.h - an OPL2 is hardware, there is no routine
 *             to transcribe, and ymfm does it.
 */
#ifndef ADLIB_H
#define ADLIB_H

#include <stdint.h>

/* Load and decompress adlib.dat from the game directory. Returns 0 on
 * success. Without it every call below is a no-op and the game is silent,
 * which is what a machine with no AdLib did. */
int  adlib_load(const char *data_dir, const char **err);
void adlib_free(void);

/* True once the driver's data is loaded - so a caller can say it had no
 * sound rather than reporting silence as success. */
uint8_t adlib_ready(void);

/* The driver's INT 61h entries, by the AH the game passes.
 *
 *   AH=0  driver +0x0115  the tick. Ends in `iret` and saves every register:
 *                         it is written to BE an interrupt handler, and the
 *                         game calls it from its own timer ISR. An AdLib
 *                         card has no IRQ line, so a music driver physically
 *                         cannot clock itself.
 *   AH=2  driver +0x003A  stop: key every channel off and forget the tune.
 *   AH=3  driver +0x0022  request a tune       (the slot at driver +0x00ED)
 *   AH=4  driver +0x002A  request an effect    (the slot at driver +0x00F0)
 *   AH=5  driver +0x0032  a third slot         (driver +0x00EE)
 */
void adlib_tick(void);
void adlib_stop(void);
void adlib_music(uint8_t id);
void adlib_effect(uint8_t id);
void adlib_slot5(uint8_t id);

/* The rate the tick must be called at is the GAME's, not this file's: the
 * game programs the PIT and calls the driver from IRQ 0. Measured off the
 * original at 97.08 Hz (divisor 12291) - see docs/sound.md. */
#define ADLIB_TICK_HZ 97.08

#endif /* ADLIB_H */
