/*
 * V7 platter -> speed, in multiples of nominal (1.0 = 33 1/3 RPM). GPLv3.
 *
 * Each platter tick sends two MIDI messages, paired 1:1: a CC with a 7-bit
 * position counter, and a pitch-bend with a 14-bit timestamp off the V7's
 * 2822400 Hz clock. Feed both in and read the speed out.
 *
 * No ALSA and no JACK in here on purpose: this is the part that can be wrong
 * silently, so it is a pure state machine with tests (see platter_test.c).
 */

#ifndef PLATTER_H
#define PLATTER_H

#define TS_HZ 2822400.0                 /* 44100 * 64, measured to 1.4 ppm */
#define TS_WRAP 16384                   /* 14 bits: wraps every 5.805 ms */
#define CC_WRAP 128                     /* 7 bits */

/* COUNTS_PER_REV is measured, not inherited: with the motor running (B0 41 7F)
 * the platter settles at 2000.5 counts/s, which at 33 1/3 RPM is 3600.99
 * counts/rev. Two unknowns — the count and the motor's accuracy — and one round
 * answer settles both: the count is 3600 (0.1 degrees) and the motor runs 0.028%
 * fast, 33.3425 RPM. That error needs no correction, since we integrate the
 * platter's real counts and xwax should hear the deck exactly as it turns.
 * The V7 notes' 3604 is wrong by 0.11%, enough to drift a beat every ten
 * minutes. Still overridable with --counts, but there is nothing left to trim.
 *
 * Measure it over a long steady window. Short windows read up to 0.5% low, and
 * that is an artefact of slicing a byte stream by arrival time — the first
 * message in a window can be mis-framed, and one bad timestamp poisons the
 * unwrap chain after it. The platter itself starts like a turntable, in well
 * under a second. Nothing in this file has that problem: it is fed already-
 * framed ALSA events and never cuts a stream mid-message. */
#define COUNTS_PER_REV 3600.0
#define NOMINAL_RPS (100.0 / 3.0 / 60.0)  /* 33 1/3 RPM in rev/s */

/* Faster than any hand can turn the platter. A violent scratch flick is a
 * quarter turn in 50 ms, about 9x nominal; the V7 itself, after the host has
 * missed its bulk IN reads for ~10 ms, sends position jumps of +12 counts
 * stamped 0.188 ms apart, 32x nominal, and then carries on as if nothing
 * happened (measured on the Mac, 2026-10-08). A tick past this bound is not
 * motion and must not reach the speed. */
#define PLATTER_MAX_SPEED 16.0

/* How late a packet may reach us after the V7 sent it. On Linux it is a
 * steady 1 ms; on the 2015 MacBook under VirtualDJ bulk IN sits in the queue
 * for 6-12 ms at a time (measured 2026-10-08), more than two timestamp
 * wraps, so the host clock cannot pick the wrap on its own -- see commit()
 * in platter.c. Arrival gaps beyond TS_WRAP plus this are a silence. */
#define PLATTER_LATENCY 0.025
#define PLATTER_LEAD_DECAY 0.005        /* s per s: lets lead_max follow a slow clock,
                                         * and forget a wrap picked wrong within a second */

/* The speed is the slope of position over the last PLATTER_WINDOW of device
 * time, not the last tick's delta: the sink samples the speed every 4 ms
 * and a one-tick delta -- +36 counts in one tick after a host read gap, the
 * V7 coalescing, then -30 to put it right -- would weigh as if it lasted
 * the whole 4 ms. The slope over a window weighs every tick by its own
 * duration, whenever it is sampled. 12 ms is the smoothing the low-pass
 * already had, so a scratch feels the same. */
#define PLATTER_WINDOW 0.012

/* A platter turning faster than this does not reverse between one tick and
 * the next: a hand takes it through zero first, and the ticks say so. What
 * the V7 sends after a host read gap -- a tick of -30 counts in 2 ms putting
 * its over-reported position right, at a platter turning at 1.0 -- is not
 * that. Never refused three ticks in a row, so a change the model has not
 * seen before cannot leave it stuck. */
#define PLATTER_REVERSE_FLOOR 0.5
#define PLATTER_MAX_REJECT 3
#define PLATTER_RING 64                 /* ticks kept: 64 ms at one per ms */

struct platter {
    /* Tunables. */
    double counts_per_rev;
    double tau;                         /* velocity smoothing, seconds */
    double idle;                        /* silence before speed decays to 0 */

    /* Raw halves of the current tick, and which of them have arrived. */
    unsigned pos7, raw_ts;
    int have;                           /* bit 0: position, bit 1: timestamp */

    /* Unwrapped running state. */
    long pos;                           /* counts, monotonic-ish, signed:
                                         * the device's counter, unwrapped */
    long phys;                          /* the same minus impossible ticks */
    double ts;                          /* seconds on the V7's clock */
    double arrival;                      /* our clock, when the tick landed */
    int started;

    double lead_max;                    /* best seen (device time - our time):
                                         * the least-latency delivery */
    struct { double ts; long pos; } ring[PLATTER_RING];
    int head, n;                        /* newest at head-1, n valid */
    int full;                           /* the ring spans PLATTER_WINDOW */
    int rejected;                       /* consecutive ticks not believed */
    double raw;                         /* speed: slope over PLATTER_WINDOW */
    double speed;                       /* smoothed, this is the output */
};

void platter_init(struct platter *p);

/*
 * The two halves arrive adjacently but in either order, so both take the clock
 * and either one may be the call that completes the pair.
 */
void platter_pos(struct platter *p, unsigned cc, double now);      /* 7-bit */
void platter_stamp(struct platter *p, unsigned raw, double now);   /* 14-bit */

/*
 * Advance the smoothing filter by dt seconds and return the speed. `now` is
 * the same clock passed to platter_stamp; silence longer than p->idle pulls the
 * speed to zero, because the V7 sends nothing at all while the platter is still.
 */
double platter_speed(struct platter *p, double now, double dt);

#endif
