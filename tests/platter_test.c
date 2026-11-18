/*
 * Tests for the platter state machine. GPLv3.
 *
 * Simulates the V7: a message every millisecond carrying a 7-bit position and a
 * 14-bit hardware timestamp, both wrapping, with the arrival time quantised onto
 * the 1 ms USB grid the way the real thing does. Nothing here talks to ALSA or
 * JACK, so a wrong unwrap fails in a second rather than sounding vaguely off.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "platter.h"

#define STEP 0.001                      /* one message per ms, as measured */

static int failures;

static void report(const char *what, double got, double want, double tol)
{
    int bad = fabs(got - want) > tol;

    printf("  %-42s got %+8.4f  want %+8.4f  %s\n",
           what, got, want, bad ? "FAIL" : "ok");
    if (bad)
        failures++;
}

static long mod(long v, long m)
{
    return (v % m + m) % m;
}

/*
 * Spin at `speed` for `secs`, feeding the state machine. `order` swaps which
 * half of each pair is sent first. `jitter` adds whole-millisecond arrival
 * error on top of the grid, to prove the wrap anchor tolerates it.
 */
static double spin(struct platter *p, double speed, double secs, int order,
                   int jitter)
{
    double cps = speed * NOMINAL_RPS * p->counts_per_rev;
    long k, n = secs / STEP;
    double out = 0.0;

    for (k = 0; k < n; k++) {
        double t = k * STEP;
        unsigned pos7 = mod(lround(cps * t), CC_WRAP);
        unsigned raw = mod(llround(t * TS_HZ), TS_WRAP);
        double now = t + (jitter && (k % 7) == 0 ? STEP : 0.0);

        if (order) {
            platter_stamp(p, raw, now);
            platter_pos(p, pos7, now);
        } else {
            platter_pos(p, pos7, now);
            platter_stamp(p, raw, now);
        }
        out = platter_speed(p, now, STEP);
    }

    return out;
}

int main(void)
{
    struct platter p;
    double v;

    /* Nominal, and long enough to cross ~340 timestamp wraps. */
    printf("steady spin (crosses the 5.805 ms timestamp wrap ~340 times/2s):\n");
    platter_init(&p);
    report("forwards 1.0", spin(&p, 1.0, 2.0, 0, 0), 1.0, 0.02);

    platter_init(&p);
    report("forwards 1.0, halves in the other order",
           spin(&p, 1.0, 2.0, 1, 0), 1.0, 0.02);

    platter_init(&p);
    report("forwards 1.0, +1 ms arrival jitter",
           spin(&p, 1.0, 2.0, 0, 1), 1.0, 0.02);

    platter_init(&p);
    report("half speed", spin(&p, 0.5, 2.0, 0, 0), 0.5, 0.02);

    platter_init(&p);
    report("double speed", spin(&p, 2.0, 2.0, 0, 0), 2.0, 0.04);

    printf("backwards (7-bit position wrap the other way):\n");
    platter_init(&p);
    report("backwards -1.0", spin(&p, -1.0, 2.0, 0, 0), -1.0, 0.02);

    printf("silence means stopped, not carry on:\n");
    platter_init(&p);
    v = spin(&p, 1.0, 1.0, 0, 0);
    if (fabs(v - 1.0) > 0.02) {
        printf("  spin-up did not reach nominal (%.4f) — later results are moot\n", v);
        failures++;
    }
    {
        /* Stop sending. The V7 goes silent when the platter is still, so the
         * speed must fall to zero on its own. */
        double now = 1.0, k;

        for (k = 0; k < 0.150; k += STEP) {
            now += STEP;
            v = platter_speed(&p, now, STEP);
        }
        report("decays to 0 after 150 ms of silence", v, 0.0, 0.01);
    }

    printf("a change of speed is followed, not averaged away:\n");
    platter_init(&p);
    spin(&p, 1.0, 0.5, 0, 0);
    {
        /* Continue from where the first spin left off, at triple speed —
         * scratching is exactly this, repeatedly. */
        struct platter q = p;
        double cps = 3.0 * NOMINAL_RPS * q.counts_per_rev;
        double t0 = 0.499;               /* where the 0.5 s spin actually ended */
        long base = q.pos;               /* fixed: q.pos moves as we feed it */
        long k;

        for (k = 1; k <= 60; k++) {      /* 60 ms, 5x the 12 ms time constant */
            double t = t0 + k * STEP;
            platter_pos(&q, mod(base + lround(cps * (t - t0)), CC_WRAP), t);
            platter_stamp(&q, mod(llround(t * TS_HZ), TS_WRAP), t);
            v = platter_speed(&q, t, STEP);
        }
        report("reaches 3.0 within 60 ms", v, 3.0, 0.10);
    }

    printf("delivery latency on the host clock (the Mac under a DJ app):\n");
    {
        /* Ticks every 1 ms on the device clock, delivered to us with a
         * latency that jumps between 0 and 12 ms -- more than two wraps.
         * Measured 2026-10-08: under VirtualDJ on the Mac bulk IN packets
         * sit in the queue for 6-12 ms at a time, and the arrival-anchored
         * wrap pick read 1.0 as 0.14, twice a second. */
        double cps = 1.0 * NOMINAL_RPS * COUNTS_PER_REV, worst = 1.0, now = 0.0;
        long k;

        platter_init(&p);
        for (k = 0; k < 3000; k++) {
            double t = k * STEP;
            double lat = ((k / 37) % 3) * 0.006;      /* 0, 6, 12 ms, in runs */

            if (t + lat > now)              /* packets arrive in order: a drop */
                now = t + lat;              /* in latency is a burst, not time */

            platter_pos(&p, mod(lround(cps * t), CC_WRAP), now);
            platter_stamp(&p, mod(llround(t * TS_HZ), TS_WRAP), now);
            v = platter_speed(&p, now, STEP);
            if (k > 200 && fabs(v - 1.0) > fabs(worst - 1.0))
                worst = v;
        }
        report("1.0 under 0/6/12 ms delivery latency", worst, 1.0, 0.03);
    }
    {
        /* A crawl: one count every 8 ms, past the 5.805 ms wrap, delivered on
         * time. The wrap count has to come from somewhere other than the
         * hardware delta alone. */
        double speed = (1.0 / 0.008) / COUNTS_PER_REV / NOMINAL_RPS;   /* ~0.0625 */
        double worst = speed;
        long k;

        platter_init(&p);
        for (k = 1; k < 250; k++) {
            double t = k * 0.008;

            platter_pos(&p, mod(k, CC_WRAP), t);
            platter_stamp(&p, mod(llround(t * TS_HZ), TS_WRAP), t);
            v = platter_speed(&p, t, 0.008);
            if (k > 60 && fabs(v - speed) > fabs(worst - speed))
                worst = v;
        }
        report("a crawl at one count per 8 ms", worst, speed, speed * 0.1);
    }
    {
        /* Stopped for half a second, then nominal at once, with the first
         * packets delivered 10 ms late: the wrap count after the silence is
         * unknowable, and it must not leave the model stuck a wrap off. */
        double now = 0.0;
        long k;

        platter_init(&p);
        spin(&p, 1.0, 0.3, 0, 0);
        for (k = 1; k <= 80; k++) {
            double t = 0.299 + 0.5 + k * STEP;

            if (t + (k < 20 ? 0.010 : 0.0) > now)
                now = t + (k < 20 ? 0.010 : 0.0);

            platter_pos(&p, mod(p.pos + 2, CC_WRAP), now);
            platter_stamp(&p, mod(llround(t * TS_HZ), TS_WRAP), now);
            v = platter_speed(&p, now, STEP);
        }
        report("nominal again 80 ms after a 0.5 s stop, late packets", v, 1.0, 0.05);
    }

    printf("an impossible tick is ignored, not followed:\n");
    platter_init(&p);
    spin(&p, 1.0, 0.5, 0, 0);
    {
        /* Measured on the Mac (2026-10-08): after the host misses bulk IN for
         * ~10 ms the V7 emits a position jump of +12 counts stamped 0.188 ms
         * after the previous tick -- 32x nominal, which no hand can do -- and
         * then carries on at 1 ms per tick as if nothing happened. Taken
         * literally it is a 100-200 ms slide of the record. */
        struct platter q = p;
        double t0 = 0.499, t = t0 + 0.000188;
        long base = q.pos;
        double worst = 1.0;
        long k;

        platter_pos(&q, mod(base + 12, CC_WRAP), t);
        platter_stamp(&q, mod(llround(t * TS_HZ), TS_WRAP), t);
        base += 12;
        /* The sink renders every 4 ms at its own phase, so it does sample
         * the speed inside the millisecond the bad tick is the latest one. */
        worst = platter_speed(&q, t, 0.004);
        for (k = 1; k <= 40; k++) {
            t += STEP;
            platter_pos(&q, mod(base + 2 * k, CC_WRAP), t);
            platter_stamp(&q, mod(llround(t * TS_HZ), TS_WRAP), t);
            v = platter_speed(&q, t, STEP);
            if (fabs(v - 1.0) > fabs(worst - 1.0))
                worst = v;
        }
        report("+12 counts in 0.188 ms leaves the speed alone", worst, 1.0, 0.02);
    }

    {
        /* The V7 putting its position right after a host read gap: at a
         * steady 1.0, one tick of -30 counts over 2.4 ms, then 2 per ms as
         * before. The slope over the window would carry it for 12 ms. */
        struct platter q;
        double t, worst = 1.0;
        long base, k;

        platter_init(&q);
        spin(&q, 1.0, 0.5, 0, 0);
        t = 0.499 + 0.0024;
        base = q.pos - 30;
        platter_pos(&q, mod(base, CC_WRAP), t);
        platter_stamp(&q, mod(llround(t * TS_HZ), TS_WRAP), t);
        v = platter_speed(&q, t, 0.004);
        if (fabs(v - 1.0) > fabs(worst - 1.0))
            worst = v;
        for (k = 1; k <= 40; k++) {
            t += STEP;
            platter_pos(&q, mod(base + 2 * k, CC_WRAP), t);
            platter_stamp(&q, mod(llround(t * TS_HZ), TS_WRAP), t);
            v = platter_speed(&q, t, STEP);
            if (fabs(v - 1.0) > fabs(worst - 1.0))
                worst = v;
        }
        report("-30 counts in 2.4 ms at 1.0 is not believed", worst, 1.0, 0.05);
    }

    printf(failures ? "FAILURES\n" : "all ok\n");
    return failures ? 1 : 0;
}
