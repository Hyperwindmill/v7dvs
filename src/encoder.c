/*
 * Streaming Serato timecode encoder. GPLv3.
 *
 * lfsr(), fwd() and rev() are xwax's, from ../xwax/timecoder.c — the waveform
 * shaping below is mktimecode.c's, verified against xwax's own decoder before
 * this file existed (constant rate decodes as pitch 1.000000, position
 * advancing exactly one unit per cycle).
 */

#define _GNU_SOURCE                     /* sincos */
#include <limits.h>
#include <math.h>
#ifdef __APPLE__
/* macOS has no sincos() in libm; its math.h offers the same thing under this
 * name as an always-inline. Found by compiling on the Mac, not by reading. */
#define sincos __sincos
#endif

#include "encoder.h"

static inline uint32_t lfsr(uint32_t code, uint32_t taps)
{
    uint32_t taken;
    int xrs;

    taken = code & taps;
    xrs = 0;
    while (taken != 0x0) {
        xrs += taken & 0x1;
        taken >>= 1;
    }

    return xrs & 0x1;
}

/* New bits are added at the MSB; shift right by one */

static inline uint32_t fwd(uint32_t current)
{
    uint32_t l;

    l = lfsr(current, TC_TAPS | 0x1);
    return (current >> 1) | (l << (TC_BITS - 1));
}

/* New bits are added at the LSB; shift left one and mask */

static inline uint32_t rev(uint32_t current)
{
    uint32_t l, mask;

    mask = (1 << TC_BITS) - 1;
    l = lfsr(current, (TC_TAPS >> 1) | (0x1 << (TC_BITS - 1)));
    return ((current << 1) & mask) | l;
}

void tc_seek(struct tc *t, long cycle)
{
    long n;

    if (cycle < 0)
        cycle = 0;
    if (cycle > TC_SAFE)
        cycle = TC_SAFE;

    t->reg = TC_SEED;
    t->cyc = cycle;
    t->phase = 0.0;

    for (n = 0; n < cycle; n++)
        t->reg = fwd(t->reg);
}

void tc_rewind(struct tc *t)
{
    if (t->cyc >= TC_SAFE - TC_GUARD)
        tc_seek(t, TC_DROP);
    else if (t->cyc <= TC_GUARD)
        tc_seek(t, TC_SAFE - TC_DROP);
}

_Bool tc_render(struct tc *t, double cps, signed short *pcm, size_t n)
{
    _Bool ok = 1;
    size_t i;

    for (i = 0; i < n; i++) {
        double angle, x, y, m;

        /* The bit modulates the amplitude over the cycle; a zero bit dips it
         * by up to half. Both channels get the same envelope, so the
         * quadrature phase — and with it the direction — survives it. */

        angle = t->phase * M_PI * 2;
        sincos(angle, &x, &y);
        m = 1.0 - (-y + 1.0) * 0.25 * ((t->reg & 0x1) == 0);

        pcm[2 * i] = -y * m * SHRT_MAX * 0.5;
        pcm[2 * i + 1] = x * m * SHRT_MAX * 0.5;

        t->phase += cps;

        /* cps can exceed a whole cycle per sample if someone spins absurdly
         * fast, hence while rather than if — above ~48x the wave aliases and
         * xwax will not decode it, but it must not corrupt the LFSR. */

        while (t->phase >= 1.0) {
            if (t->cyc >= TC_SAFE) {
                t->phase = 1.0 - cps;   /* park just short of the limit */
                ok = 0;
                break;
            }
            t->phase -= 1.0;
            t->cyc++;
            t->reg = fwd(t->reg);
        }
        while (t->phase < 0.0) {
            if (t->cyc <= 0) {
                t->phase = 0.0;
                ok = 0;
                break;
            }
            t->phase += 1.0;
            t->cyc--;
            t->reg = rev(t->reg);
        }
    }

    return ok;
}

_Bool tc_render_s24(struct tc *t, double cps, unsigned char *frames, size_t n,
                    size_t frame_bytes, size_t offset, double gain,
                    signed short *scratch)
{
    size_t i;
    _Bool ok = tc_render(t, cps, scratch, n);

    for (i = 0; i < n; i++) {
        unsigned char *f = frames + i * frame_bytes + offset;
        int c;

        for (c = 0; c < 2; c++) {
            long v = (long)(scratch[2 * i + c] * gain) << 8;   /* s16 -> s24 */

            f[3 * c] = v & 0xff;
            f[3 * c + 1] = (v >> 8) & 0xff;
            f[3 * c + 2] = (v >> 16) & 0xff;
        }
    }
    return ok;
}
