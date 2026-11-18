/*
 * Decode a raw stereo s16 stream on stdin as Serato 2nd Ed. side A and print
 * what xwax makes of it: pitch and, unlike xwax's own tests/timecoder, the
 * absolute position off the LFSR bitstream.
 *
 * Position is the half-cycle-resolution offset from the start of the timecode,
 * or -1 while unknown. It is the part that can be wrong silently: a stream with
 * the right waveform but a broken bitstream still reports pitch 1.0.
 *
 * Links xwax's timecoder.c and lut.c, so GPLv3.
 *
 *     cc -O2 -I../xwax -o tcdec tcdec.c ../xwax/timecoder.c ../xwax/lut.c -lm
 *     ./mkserato | ./tcdec
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "timecoder.h"

#define STEREO 2
#define DEF "serato_2a"

/*
 *     ./tcdec [sample_rate] [samples_per_line]
 *
 * The rate must match whatever produced the stream — tcgen's 96000, or JACK's,
 * which is 48000 here. Get it wrong and the pitch is simply scaled by the
 * ratio, which looks exactly like an encoder bug.
 */

int main(int argc, char *argv[])
{
    struct timecoder tc;
    struct timecode_def *def;
    unsigned int s = 0, rate, report;
    signed short sample[STEREO];

    rate = argc > 1 ? atoi(argv[1]) : 96000;
    report = argc > 2 ? atoi(argv[2]) : rate / 10;

    def = timecoder_find_definition(DEF);
    assert(def != NULL);
    timecoder_init(&tc, def, 1.0, rate, false);

    while (fread(sample, sizeof(signed short), STEREO, stdin) == STEREO) {
        double when;
        signed int pos;

        timecoder_submit(&tc, sample, 1);

        if (s % report == 0) {
            pos = timecoder_get_position(&tc, &when);
            printf("%.4f\t%.6f\t%d\n",
                   (double)s / rate, timecoder_get_pitch(&tc), pos);
        }
        s++;
    }

    timecoder_clear(&tc);
    timecoder_free_lookup();
    return 0;
}
