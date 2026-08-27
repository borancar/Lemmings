/* The OPL2 behind opl.h, checked against the chip's own arithmetic.
 *
 * This does NOT check the port against the original - it cannot. The chip is
 * hardware and the original has no routine for it (see src/opl.h), so there
 * is nothing to transcribe and nothing to differ from. What the original DOES
 * settle is the register stream, and that is `tools/probe_opl.py`'s job and
 * a different claim entirely.
 *
 * So what is checkable here is that the thing behaves like a YM3812: that a
 * documented register sequence produces a tone at the documented frequency,
 * that key-off silences it, and that the level control attenuates. A wrongly
 * wired chip passes none of those, and every one of them is a fact about the
 * datasheet rather than about our reading of the game.
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "opl.h"

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-52s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) failures++;
}

/* A deliberately PLAIN voice: additive connection, no feedback, sine
 * waveform, the modulator attenuated to silence. That leaves one operator
 * making one sine, which is the only patch whose pitch can be read off a
 * zero-crossing count. The feedback-heavy patch that was here first counted
 * four times the fundamental and looked like a bug in the chip. */
static void plain_voice(void)
{
    opl_reset();
    opl_write(0x01, 0x20);        /* waveform select enable */
    /* Bit 5 is the EG-type, and it is the difference between a note that
     * holds and one that does not exist. Clear, the envelope is PERCUSSIVE:
     * it decays straight past the sustain level to silence. With the fastest
     * decay rate below, that is a note lasting a couple of milliseconds - it
     * measured 4 Hz and read exactly like a broken chip. Set, the envelope
     * SUSTAINS, which is what a held note needs. */
    opl_write(0x20, 0x21);        /* modulator: multiple 1, sustaining */
    opl_write(0x23, 0x21);        /* carrier:   multiple 1, sustaining */
    opl_write(0x40, 0x3F);        /* modulator silent (max attenuation) */
    opl_write(0x43, 0x00);        /* carrier at full volume */
    opl_write(0x60, 0xFF);        /* instant attack, fastest decay */
    opl_write(0x63, 0xFF);
    opl_write(0x80, 0x0F);        /* full sustain, fast release */
    opl_write(0x83, 0x0F);
    opl_write(0xE0, 0x00);        /* sine */
    opl_write(0xE3, 0x00);
    opl_write(0xC0, 0x01);        /* additive, no feedback */
}

static double measure_hz(int16_t *buf, uint32_t n)
{
    uint32_t cross = 0;
    for (uint32_t i = 1; i < n; i++)
        if ((buf[i - 1] < 0) != (buf[i] < 0)) cross++;
    return cross / 2.0 / ((double)n / OPL_SAMPLE_RATE);
}

static int32_t peak_of(int16_t *buf, uint32_t n)
{
    int32_t p = 0;
    for (uint32_t i = 0; i < n; i++) {
        int32_t a = buf[i] < 0 ? -buf[i] : buf[i];
        if (a > p) p = a;
    }
    return p;
}

int main(void)
{
    uint32_t n = OPL_SAMPLE_RATE / 4;          /* a quarter second */
    int16_t *buf = malloc(n * sizeof *buf);
    if (!buf) return 2;

    check(OPL_SAMPLE_RATE == 49716,
          "the chip's rate is 3579545/72, rounded: 49716 Hz");

    /* The datasheet's frequency formula:
     *     f = FNum * 49716 / 2^(20 - Block)
     * FNum 408 in block 4 is 408 * 49716 / 65536 = 309.5 Hz. Reaching it
     * needs the f-number split correctly across two registers - the low
     * eight bits at 0xA0 and the top two in 0xB0's bits 0-1 - which is the
     * part a wrong wiring gets wrong. */
    plain_voice();
    opl_write(0xA0, 0x98);
    opl_write(0xB0, 0x31);                     /* key on, block 4, f-num hi 1 */
    opl_render(buf, n);
    double want = 408.0 * OPL_SAMPLE_RATE / 65536.0;
    double got  = measure_hz(buf, n);
    printf("  f-number 408, block 4: want %.1f Hz, measured %.1f Hz\n",
           want, got);
    check(fabs(got - want) < want * 0.02, "the tone is within 2% of the formula");
    int32_t loud = peak_of(buf, n);
    check(loud > 1000, "a keyed-on voice is audible");

    /* An octave is one block, and nothing else changes. */
    plain_voice();
    opl_write(0xA0, 0x98);
    opl_write(0xB0, 0x35);                     /* same f-number, block 5 */
    opl_render(buf, n);
    double oct = measure_hz(buf, n);
    printf("  one block higher:      want %.1f Hz, measured %.1f Hz\n",
           want * 2.0, oct);
    check(fabs(oct - want * 2.0) < want * 0.04, "block + 1 doubles the pitch");

    /* Key-off releases. Measuring the peak of the buffer that STARTS at the
     * key-off is measuring the note still being there: the release begins at
     * full amplitude and falls, so the peak of that window is the note's, not
     * the silence's. Let the release run first, then look. */
    opl_write(0xB0, 0x15);                     /* same note, KEY BIT CLEAR */
    opl_render(buf, n);                        /* the release itself */
    opl_render(buf, n);                        /* and what is left after it */
    check(peak_of(buf, n) < loud / 100, "key-off silences the voice");

    /* Total level is attenuation in 0.75 dB steps, so a bigger number is
     * QUIETER - the one place in the register map where up means down. */
    plain_voice();
    opl_write(0x43, 0x20);                     /* 32 steps = -24 dB */
    opl_write(0xA0, 0x98);
    opl_write(0xB0, 0x31);
    opl_render(buf, n);
    int32_t quiet = peak_of(buf, n);
    printf("  total level 0 -> peak %d, total level 32 -> peak %d\n",
           loud, quiet);
    check(quiet > 0 && quiet < loud / 4, "total level attenuates");

    check(opl_writes() > 0, "the chip counted the writes it was given");

    free(buf);
    printf("%s\n", failures ? "FAILED" : "all OPL2 checks passed");
    return failures ? 1 : 0;
}
