/* An OPL2 (YM3812) - the chip on an AdLib card.
 *
 * NOT TRANSCRIBED, and there is nothing to transcribe it from: an OPL2 is
 * hardware, so no routine in vgalemmi.exe implements one. It sits on the same
 * side of the boundary as SDL and as the bit-plane assembly in sdl_io.c -
 * something the port needs because the machine the original ran on is gone.
 * See "WHAT THE HARDWARE USED TO DO" there.
 *
 * The line this must not cross is into the game. What registers get written,
 * and when, is the DRIVER's decision - `adlib.dat`'s sequencer - and that is
 * transcribable, is checked against the original's own register stream, and
 * belongs in the port proper. This file's job stops at turning register
 * writes into samples.
 *
 * Implemented over ymfm (see ../vendor/README.md), which is C++, so this
 * header is the C boundary and opl_ymfm.cpp is the only C++ in the build.
 */
#ifndef OPL_H
#define OPL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The chip's own sample rate: a YM3812 is clocked at 3.579545 MHz - NTSC
 * colourburst, the cheapest crystal there was - and divides it by 72, which
 * is four clocks for each of its eighteen operator slots. 49716 Hz.
 *
 * The port renders at this rate and lets SDL convert. Resampling here would
 * be a second place for the sound to be wrong. */
#define OPL_CLOCK       3579545u
/* 3579545 / 72 is 49715.9, and the figure everyone quotes is 49716 - so round
 * rather than truncate. Plain integer division gives 49715 and that is a
 * different number appearing in every WAV header this writes. */
#define OPL_SAMPLE_RATE ((OPL_CLOCK + 36u) / 72u)      /* 49716 */

void    opl_reset(void);

/* One register write, exactly as `out 0x388, reg` / `out 0x389, val` did.
 *
 * AND THE CHIP RUNS WHILE IT HAPPENS. The driver reads the port twelve times
 * after the register and about forty after the value (adlib.dat +0x0559),
 * which is the YM3812's settling time - 3.3 us and 23 us - spent on a bus
 * that takes about a microsecond a read.
 *
 * This header used to say those reads "carry no state and have no
 * counterpart here". That was wrong, and it is the difference between music
 * and a hollow version of it. Without them every register write of a driver
 * tick lands at ONE INSTANT and the chip advances only afterwards: notes
 * key off and on again with no time in between, envelopes never get their
 * attack, and the result is thin and half as loud.
 *
 * Measured: DOSBox's own register stream, quantised onto the port's tick
 * grid, falls from an RMS of 1354 to 703 and takes on exactly the hollow,
 * collapsing profile the port had. Spreading the writes again restores it,
 * and 59 us of spacing does as well as 1000 - what matters is that the chip
 * advances AT ALL between writes, not by how much. */
void    opl_write(uint8_t reg, uint8_t val);

/* The settling time one register write costs, in microseconds: the YM3812's
 * 3.3 us after the address and 23 us after the data. */
#define OPL_WRITE_SETTLE_US 26.3

/* Render `frames` mono samples at OPL_SAMPLE_RATE. */
void    opl_render(int16_t *out, uint32_t frames);

/* How many register writes have been made since the last reset - so a check
 * can say it exercised the chip rather than assuming it did. */
uint32_t opl_writes(void);

/* Watch every register write. This is how the port's own stream gets dumped
 * for comparison against the original's, and it is deliberately here rather
 * than inside the driver: what matters is what reaches the CHIP, so a write
 * the driver makes twice, or makes through some path nobody remembered, is
 * still counted. Pass NULL to stop. */
typedef void (*opl_trace_fn)(uint8_t reg, uint8_t val);
void opl_set_trace(opl_trace_fn fn);

#ifdef __cplusplus
}
#endif
#endif /* OPL_H */
