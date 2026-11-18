/*
 * Streaming Serato timecode encoder. GPLv3 — the LFSR is xwax's.
 *
 * Unlike xwax's mktimecode this runs both directions and at an arbitrary,
 * changing rate: the V7's platter is an incremental encoder with no absolute
 * reference, so absolute position is ours to choose and ours to integrate.
 */

#ifndef ENCODER_H
#define ENCODER_H

#include <stddef.h>
#include <stdint.h>

/* Serato 2nd Ed. side A, copied from ../xwax/timecoder.c:68 */
#define TC_RESOLUTION 1000              /* wave cycles/s at nominal speed */
#define TC_BITS 20
#define TC_SEED 0x59017
#define TC_TAPS 0x361e4
#define TC_LENGTH 712000                /* cycles of "vinyl" — 11m52s */
#define TC_SAFE 707000                  /* past this xwax distrusts position */

/* Where the needle is lifted, and where it is dropped again. A record ends,
 * and a synthesized one has nobody to lift it: a deck that runs into the end
 * stops advancing and dies there — measured 2026-09-16, where a DJ app fell
 * out of vinyl control after six minutes and never came back. The lift happens
 * TC_GUARD short of either end, BEFORE the encoder has to clamp, so a frozen
 * phase never reaches the wire; the drop lands TC_DROP in from the other one.
 *
 * TC_DROP is the backwards headroom a scratch has right after a lift, so it is
 * seconds rather than the half-record it used to be: starting mid-record spent
 * six minutes of timecode buying headroom nobody uses, and put the first jump
 * inside the length of an ordinary track. TC_GUARD only has to exceed what one
 * audio block can travel, which even at absurd scratch speeds is tens of
 * cycles. */
#define TC_GUARD 1000                   /* 1 s from each end */
#define TC_DROP 10000                   /* 10 s in from the other end */

struct tc {
    uint32_t reg;                       /* LFSR state at cycle `cyc` */
    long cyc;                           /* absolute position, in cycles */
    double phase;                       /* position within the cycle, [0,1) */
};

/*
 * Place the virtual needle. O(cycle) but that is at most TC_LENGTH iterations
 * of a shift and a popcount, so tens of microseconds — no lookup table.
 */
void tc_seek(struct tc *t, long cycle);

/*
 * Render n stereo frames advancing by `cps` cycles per sample. Negative cps
 * runs the record backwards. Clamps at the ends of the timecode rather than
 * walking off it, and reports whether it had to.
 *
 * Return: false if the position hit a limit during this block.
 */
_Bool tc_render(struct tc *t, double cps, signed short *pcm, size_t n);

/*
 * Lift the needle and drop it at the other end when the record has nearly run
 * out. A no-op away from the ends, so a backend can call it after every block.
 * Costs one tc_seek, which is O(cycle): under a millisecond, once per side.
 */
void tc_rewind(struct tc *t);

/*
 * The same block, packed as S24_3LE straight into interleaved frames of
 * `frame_bytes` at byte `offset` — the V7's 4-channel, 12-byte playback frame
 * with each RCA pair 6 bytes in. `scratch` holds 2*n shorts. `gain` scales the
 * s16 before widening (1.0 keeps the encoder's own -6 dBFS). This is the macOS
 * mode A path: encoder -> iso OUT with no audio server in between.
 */
_Bool tc_render_s24(struct tc *t, double cps, unsigned char *frames, size_t n,
                    size_t frame_bytes, size_t offset, double gain,
                    signed short *scratch);

#endif
