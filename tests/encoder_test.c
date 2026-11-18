/*
 * Tests for the ends of the record: what happens when the timecode runs out.
 * GPLv3.
 *
 * A control record is 11m52s long and then it ends. A real DJ lifts the needle
 * and drops it again; a synthesized record has nobody to lift it, so the deck
 * simply dies -- measured 2026-09-16 on the Mac, where the daemon counted
 * 33330 clamped blocks and Mixxx fell out of vinyl control into its own
 * protection after six minutes.
 *
 * The encoder still clamps rather than walking off the timecode, which is what
 * tcgen and check.sh prove. tc_rewind is the other half: it lifts the needle
 * BEFORE the clamp, so a frozen phase never reaches the wire.
 */

#include <stdio.h>

#include "encoder.h"

static int failures;

static void report(const char *what, long got, long want)
{
    int bad = got != want;

    printf("  %-50s got %8ld  want %8ld  %s\n",
           what, got, want, bad ? "FAIL" : "ok");
    if (bad)
        failures++;
}

int main(void)
{
    struct tc t;
    signed short pcm[2 * 256];
    long blocks, clamped = 0, rewinds = 0, prev;
    int i;

    printf("encoder, the ends of the record:\n");

    /* Near the end, before the clamp: the block is still clean and the needle
     * is lifted on the strength of the position alone. */
    tc_seek(&t, TC_SAFE - TC_GUARD - 5);
    report("a block just short of the guard is clean",
           tc_render(&t, 0.5, pcm, 256), 1);
    tc_rewind(&t);
    report("and the needle is dropped TC_DROP in", t.cyc, TC_DROP);

    /* The same going backwards. */
    tc_seek(&t, TC_GUARD + 5);
    report("a backwards block inside the guard is clean",
           tc_render(&t, -0.5, pcm, 256), 1);
    tc_rewind(&t);
    report("and it is dropped TC_DROP from the end", t.cyc, TC_SAFE - TC_DROP);

    /* Away from the ends a rewind must move nothing, or a deck in the middle
     * of the record would jump for no reason. */
    tc_seek(&t, TC_SAFE / 2);
    report("a block in the middle is clean", tc_render(&t, 0.5, pcm, 16), 1);
    tc_rewind(&t);
    report("and a rewind there moves nothing", t.cyc, TC_SAFE / 2 + 8);

    /* The regression this file exists for: play forward through more than two
     * sides, rewinding after every block the way the backends do, and nothing
     * must ever clamp. Before tc_rewind this died after one side and stayed
     * dead. 0.5 cycles per sample over 256-sample blocks is 128 cycles a
     * block, comfortably inside the guard. */
    tc_seek(&t, TC_DROP);
    prev = t.cyc;
    blocks = 3 * TC_SAFE / 128;
    for (i = 0; i < blocks; i++) {
        if (!tc_render(&t, 0.5, pcm, 256))
            clamped++;
        tc_rewind(&t);
        if (t.cyc < prev)
            rewinds++;                  /* the needle went back */
        prev = t.cyc;
    }
    report("three sides forward, nothing clamped", clamped, 0);
    /* blocks * 128 cycles of travel over sides of TC_SAFE - TC_GUARD - TC_DROP
     * each: three lifts, and the third leaves it partway through a fourth. */
    report("and the needle was lifted once per side",
           rewinds, blocks * 128 / (TC_SAFE - TC_GUARD - TC_DROP));
    report("finishing inside the record", t.cyc > 0 && t.cyc < TC_SAFE, 1);

    /* The drop point has to be worth having: ten seconds of backwards scratch
     * before the needle runs off the other end. */
    report("the drop is seconds in, not minutes", TC_DROP / TC_RESOLUTION, 10);

    if (failures) {
        printf("%d FAILURE(S)\n", failures);
        return 1;
    }
    printf("all ok\n");
    return 0;
}
