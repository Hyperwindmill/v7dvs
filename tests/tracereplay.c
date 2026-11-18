/*
 * Replay a `v7d --trace` file through the real parser and the platter model,
 * sampling the speed every 4 ms as the sink does. GPLv3.
 *
 *     ./tracereplay FROM TO DECK < trace.csv
 *
 * FROM and TO are trace seconds (the monotonic clock in column one), DECK is
 * 0 or 1. Prints the speed range, how many samples fell below 0.7 or above
 * 1.3, the mean absolute error from 1.0 and a histogram. This is how the
 * Mac's delivery latency and the V7's stamp jitter were told apart from the
 * model (2026-10-08): a trace is the data, this is the model, and tcdec on
 * the DDI capture is what the app heard.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "midiparse.h"
#include "platter.h"
static struct platter p; static double cur; static int deck;
static void emit(void *ctx, const uint8_t *m, size_t n) {
    (void)ctx; if (n != 3) return;
    if (m[0] == 0xB0 && m[1] == (deck ? 0x02 : 0x00)) platter_pos(&p, m[2], cur);
    else if ((m[0] & 0xF0) == 0xE0 && (m[0] & 0x0F) == (deck ? 2 : 0)) platter_stamp(&p, m[1] | (m[2] << 7), cur);
}
int main(int argc, char **argv) {
    struct midiparse mp; char line[512]; uint8_t pkt[64];
    double lo = atof(argv[1]), hi = atof(argv[2]), next = -1, mn = 9, mx = -9, v = 0; deck = atoi(argv[3]);
    long n = 0, dips = 0, bumps = 0, hist[12] = {0}; double serr = 0;
    midiparse_init(&mp, emit, NULL); platter_init(&p);
    while (fgets(line, sizeof line, stdin)) {
        char *c = strchr(line, ','); if (!c) continue; *c = 0; cur = atof(line);
        if (next < 0) next = cur;
        while (next + 0.004 <= cur) {      /* renders the sink would have done */
            next += 0.004; v = platter_speed(&p, next, 0.004);
            if (next >= lo && next <= hi) {
                int b = (int)floor(v * 5 + 5);

                n++;
                if (v < mn)
                    mn = v;
                if (v > mx)
                    mx = v;
                if (v < 0.7)
                    dips++;
                if (v > 1.3)
                    bumps++;
                serr += fabs(v - 1.0);
                if (b < 0)
                    b = 0;
                if (b > 11)
                    b = 11;
                hist[b]++;
            }
        }
        size_t k = 0; for (char *h = c + 1; h[0] && h[1] && h[0] != '\n'; h += 2) { unsigned x; sscanf(h, "%2x", &x); pkt[k++] = x; }
        midiparse_packet(&mp, pkt, k);
        if (cur > hi) break;
    }
    printf("%ld renders: speed min %.3f max %.3f, below 0.7: %ld, above 1.3: %ld, mean abs err %.4f\n", n, mn, mx, dips, bumps, serr / n);
    printf("  histogram (0.2 wide from -1.0): "); for (int i = 0; i < 12; i++) printf("%ld ", hist[i]); printf("\n");
    return 0;
}
