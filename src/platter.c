/*
 * V7 platter -> speed. GPLv3. See platter.h.
 */

#include <math.h>

#include "platter.h"

void platter_init(struct platter *p)
{
    p->counts_per_rev = COUNTS_PER_REV;
    p->tau = 0.012;                     /* ~13 Hz corner: fast enough for a
                                         * scratch, slow enough to swallow the
                                         * +/-0.5 count position quantisation */
    p->idle = 0.035;                    /* a packet may reach us PLATTER_LATENCY
                                         * late and still not mean a stop */
    p->pos7 = p->raw_ts = 0;
    p->have = 0;
    p->pos = p->phys = 0;
    p->ts = 0.0;
    p->arrival = 0.0;
    p->started = 0;
    p->lead_max = 0.0;
    p->head = p->n = 0;
    p->full = p->rejected = 0;
    p->raw = 0.0;
    p->speed = 0.0;
}

/*
 * Both halves of a tick are in: unwrap them and take the difference.
 */

static void remember(struct platter *p)
{
    p->ring[p->head].ts = p->ts;
    p->ring[p->head].pos = p->phys;
    p->head = (p->head + 1) % PLATTER_RING;
    if (p->n < PLATTER_RING)
        p->n++;
}

/* Slope of position over the last PLATTER_WINDOW of device time: the oldest
 * remembered tick still inside the window against the newest, or the oldest
 * of all while the ring is younger than the window. */
static void slope(struct platter *p)
{
    int newest = (p->head + PLATTER_RING - 1) % PLATTER_RING, i, old = newest;

    for (i = 1; i < p->n; i++) {
        int j = (p->head + PLATTER_RING - 1 - i) % PLATTER_RING;

        old = j;
        if (p->ring[newest].ts - p->ring[j].ts >= PLATTER_WINDOW)
            break;
    }
    if (old != newest && p->ring[newest].ts > p->ring[old].ts) {
        p->raw = ((p->ring[newest].pos - p->ring[old].pos) / p->counts_per_rev)
                 / (p->ring[newest].ts - p->ring[old].ts) / NOMINAL_RPS;
        p->full = p->ring[newest].ts - p->ring[old].ts >= PLATTER_WINDOW;
    }
}

static void commit(struct platter *p, unsigned pos7, unsigned raw_ts, double now)
{
    const double W = TS_WRAP / TS_HZ;   /* 5.805 ms */
    long dpos;
    double dhw, gap, dts, lead, raw;
    int k, kmax, best = -1;
    double best_err = 0.0;

    if (!p->started) {
        p->pos = p->phys = pos7;
        p->ts = raw_ts / TS_HZ;
        p->arrival = now;
        p->lead_max = p->ts - now;
        p->started = 1;
        remember(p);
        return;
    }

    /* Position: deltas are 2-3 counts per tick, so a 7-bit wrap is never
     * ambiguous — just take the shortest way round. */

    dpos = (long)pos7 - (p->pos % CC_WRAP + CC_WRAP) % CC_WRAP;
    if (dpos > CC_WRAP / 2)
        dpos -= CC_WRAP;
    else if (dpos < -CC_WRAP / 2)        /* backwards */
        dpos += CC_WRAP;
    p->pos += dpos;

    /* Timestamp: 14 bits at 2.8224 MHz wraps every 5.805 ms. The hardware
     * delta is exact modulo the wrap; how many whole wraps passed is the
     * question. Our arrival clock cannot answer it on its own: a packet can
     * reach us PLATTER_LATENCY late, more than two wraps, and the latency
     * changes from one packet to the next. So the arrival clock bounds the
     * candidates, the device's own best-case offset (lead_max: device time
     * minus ours, at its highest, i.e. the least latency seen) keeps the
     * device timeline from running away, and among what is left the wrap
     * whose interval is nearest to what these counts take at the speed we
     * have wins. Nearest in time, not in speed: the V7's stamps jitter by
     * +/-2 ms against its counts (2 counts over 3 ms, then 6 over 1 ms, at
     * a steady 1.0), which is a factor of three in a one-tick speed and
     * two milliseconds in time -- and the alternative wrap is six away.
     * While there is no speed yet, the least latency wins. */

    dhw = raw_ts / TS_HZ - (p->ts - floor(p->ts / W) * W);
    if (dhw <= 0.0)
        dhw += W;
    gap = now - p->arrival;
    p->lead_max -= PLATTER_LEAD_DECAY * gap;

    if (gap > W + PLATTER_LATENCY) {
        /* A silence: the wrap count is unknowable and the speed has decayed
         * anyway. Re-anchor as if this packet were on time, and take no
         * speed from it. */
        p->ts = raw_ts / TS_HZ;
        p->ts += round((now + p->lead_max - p->ts) / W) * W;
        p->arrival = now;
        lead = p->ts - now;
        if (lead > p->lead_max)
            p->lead_max = lead;
        p->head = p->n = p->full = 0;   /* the window starts again */
        remember(p);
        return;
    }

    kmax = (int)((gap + PLATTER_LATENCY - dhw) / W);
    if (kmax < 0)
        kmax = 0;
    for (k = 0; k <= kmax; k++) {
        double err;

        dts = dhw + k * W;
        lead = p->ts + dts - now;
        if (lead > p->lead_max + W / 2)  /* device time cannot run ahead */
            break;
        if (lead < p->lead_max - PLATTER_LATENCY)
            continue;                   /* later than any packet gets */
        if (p->full && p->raw != 0.0)
            err = fabs(dts - dpos / (p->raw * NOMINAL_RPS * p->counts_per_rev));
        else
            err = fabs(lead - p->lead_max);
        if (best < 0 || err < best_err) {
            best = k;
            best_err = err;
        }
    }
    if (best < 0) {                     /* nothing fits: the least latency */
        best = (int)round((p->lead_max + now - p->ts - dhw) / W);
        if (best < 0)
            best = 0;
        if (best > kmax)
            best = kmax;
    }
    dts = dhw + best * W;
    p->ts += dts;
    p->arrival = now;
    lead = p->ts - now;
    if (lead > p->lead_max)
        p->lead_max = lead;

    raw = (dpos / p->counts_per_rev) / dts / NOMINAL_RPS;
    /* The V7 sends impossible ticks after a host read gap (see
     * PLATTER_MAX_SPEED). The position still integrates them; the speed
     * keeps the last tick that could have been a platter. */
    if (fabs(raw) > PLATTER_MAX_SPEED
        || (p->full && p->rejected < PLATTER_MAX_REJECT
            && fabs(p->raw) > PLATTER_REVERSE_FLOOR && raw * p->raw < 0.0)) {
        /* Not motion. p->pos follows the device's counter; p->phys moves
         * as the platter would have at the speed we trust, so the window
         * sees neither the tick nor a hole where it was. */
        p->rejected++;
        p->phys += lround(p->raw * NOMINAL_RPS * p->counts_per_rev * dts);
        remember(p);
        return;
    }
    p->rejected = 0;
    p->phys += dpos;
    remember(p);
    slope(p);
}

/* Commit on whichever half completes the pair, not on an assumed order. */

static void half(struct platter *p, double now)
{
    if (p->have == 3) {
        commit(p, p->pos7, p->raw_ts, now);
        p->have = 0;
    }
}

void platter_pos(struct platter *p, unsigned cc, double now)
{
    p->pos7 = cc;
    p->have |= 1;
    half(p, now);
}

void platter_stamp(struct platter *p, unsigned raw, double now)
{
    p->raw_ts = raw;
    p->have |= 2;
    half(p, now);
}

double platter_speed(struct platter *p, double now, double dt)
{
    double target, a;

    /* Silence is not "carry on": the V7 sends nothing while the platter is
     * still, so a held speed would run the virtual record away during a pause. */
    target = (p->started && now - p->arrival > p->idle) ? 0.0 : p->raw;

    /* One-pole low pass, dt-correct so it behaves the same at any JACK buffer
     * size. ponytail: exponential, not a sliding window — a window needs a
     * history buffer and sounds no better at 12 ms. */
    a = 1.0 - exp(-dt / p->tau);
    p->speed += (target - p->speed) * a;

    return p->speed;
}
