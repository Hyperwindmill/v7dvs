/*
 * Drive the encoder from the command line and write raw stereo s16 to stdout,
 * so it can be piped straight into ./tcdec (or a JACK player). GPLv3.
 *
 *     ./tcgen 1.0 10              10s at nominal speed
 *     ./tcgen -1.0 10             the same, backwards
 *     ./tcgen ramp 0.5 1.5 10     sweep the speed while playing
 *
 * Speed is a multiple of nominal (1.0 = 33 1/3 RPM). An optional last argument
 * is the starting cycle.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "encoder.h"

#define RATE 96000                      /* must match tcdec's */
#define BLOCK 1024

int main(int argc, char *argv[])
{
    struct tc t;
    double from, to, secs;
    long start, total, done;
    signed short pcm[BLOCK * 2];
    _Bool ramp;

    ramp = argc > 1 && !strcmp(argv[1], "ramp");
    if (ramp ? argc < 5 : argc < 3) {
        fprintf(stderr, "usage: %s <speed> <seconds> [start_cycle]\n"
                        "       %s ramp <from> <to> <seconds> [start_cycle]\n",
                argv[0], argv[0]);
        return 2;
    }

    if (ramp) {
        from = atof(argv[2]);
        to = atof(argv[3]);
        secs = atof(argv[4]);
        start = argc > 5 ? atol(argv[5]) : 0;
    } else {
        from = to = atof(argv[1]);
        secs = atof(argv[2]);
        start = argc > 3 ? atol(argv[3]) : 0;
    }

    /* Backwards from cycle 0 has nowhere to go, so start mid-record. */
    if (start == 0 && (from < 0 || to < 0))
        start = TC_SAFE / 2;

    tc_seek(&t, start);
    total = secs * RATE;

    for (done = 0; done < total; done += BLOCK) {
        long n = total - done < BLOCK ? total - done : BLOCK;
        double speed = from + (to - from) * ((double)done / total);

        if (!tc_render(&t, speed * TC_RESOLUTION / RATE, pcm, n)) {
            fprintf(stderr, "tcgen: hit the end of the timecode at cycle %ld\n",
                    t.cyc);
            break;
        }
        if (fwrite(pcm, sizeof(signed short) * 2, n, stdout) != (size_t)n) {
            perror("tcgen");
            return 1;
        }
    }

    fprintf(stderr, "tcgen: %.1fs, ended at cycle %ld\n",
            (double)done / RATE, t.cyc);
    return 0;
}
