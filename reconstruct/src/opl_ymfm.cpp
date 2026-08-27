/* The OPL2 behind opl.h, over ymfm. See opl.h for why this is not
 * transcribed, and ../vendor/README.md for what ymfm is and its licence.
 *
 * This is the ONLY C++ in the reconstruction, and it is here only because
 * ymfm is C++. It holds no game logic and no decisions - every line is
 * plumbing between opl.h's C and ymfm's classes.
 */
#include "opl.h"

#include "../vendor/ymfm/ymfm_opl.h"

namespace {

/* ymfm calls back for timers, IRQs and external memory. An AdLib card has no
 * IRQ line at all - which is exactly why adlib.dat hooks no vector and must
 * be called by the game (docs/sound.md) - and a YM3812 has no external
 * memory, so every default here is the right answer. */
struct adlib_interface : public ymfm::ymfm_interface
{
};

adlib_interface g_intf;
ymfm::ym3812    g_chip(g_intf);
uint32_t        g_writes;

}  /* namespace */

extern "C" void opl_reset(void)
{
    g_chip.reset();
    g_writes = 0;
}

extern "C" void opl_write(uint8_t reg, uint8_t val)
{
    /* offset 0 is the address port (0x388), offset 1 the data port (0x389) */
    g_chip.write(0, reg);
    g_chip.write(1, val);
    g_writes++;
}

extern "C" void opl_render(int16_t *out, uint32_t frames)
{
    ymfm::ym3812::output_data frame;
    for (uint32_t i = 0; i < frames; i++) {
        g_chip.generate(&frame, 1);
        int32_t v = frame.data[0];
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        out[i] = (int16_t)v;
    }
}

extern "C" uint32_t opl_writes(void)
{
    return g_writes;
}
