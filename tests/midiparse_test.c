/*
 * Tests for the bulk IN MIDI parser. GPLv3.
 *
 * Every case is a trap that was hit for real on the V7 (see CLAUDE.md): the
 * trailer byte, 0xFD idle, running status on the fader's MSB/LSB pair, SysEx
 * split across USB packets, and the phantom messages a stall leaves behind
 * when the parser is not resynced.
 */

#include <stdio.h>
#include <string.h>

#include "midiparse.h"

static int failures;

struct got {
    uint8_t msg[64][MIDIPARSE_SYSEX_MAX];
    size_t len[64];
    int n;
};

static void collect(void *ctx, const uint8_t *msg, size_t n)
{
    struct got *g = ctx;

    if (g->n < 64) {
        memcpy(g->msg[g->n], msg, n);
        g->len[g->n] = n;
    }
    g->n++;
}

static void check(const char *what, int ok)
{
    printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        failures++;
}

static int same(const struct got *g, int i, const uint8_t *want, size_t n)
{
    return i < g->n && g->len[i] == n && memcmp(g->msg[i], want, n) == 0;
}

#define FEED(p, ...) do { \
        const uint8_t pkt_[] = { __VA_ARGS__ }; \
        midiparse_packet(p, pkt_, sizeof pkt_); } while (0)

int main(void)
{
    struct midiparse p;
    struct got g;

    printf("MIDI parser:\n");

    /* 1. The trailer is stripped, the message before it survives. */
    memset(&g, 0, sizeof g);
    midiparse_init(&p, collect, &g);
    FEED(&p, 0xB0, 0x00, 0x05, 0x00);
    check("trailer stripped, one CC out",
          g.n == 1 && same(&g, 0, (uint8_t[]){0xB0, 0x00, 0x05}, 3));

    /* 2. 0xFD idle padding is not data. */
    memset(&g, 0, sizeof g);
    midiparse_init(&p, collect, &g);
    FEED(&p, 0xB0, 0x00, 0x05, 0xFD, 0xFD, 0xFD, 0x00);
    check("0xFD idle skipped", g.n == 1);

    /* 3. Running status: the fader's MSB then LSB with one status byte. */
    memset(&g, 0, sizeof g);
    midiparse_init(&p, collect, &g);
    FEED(&p, 0xB0, 0x04, 0x10, 0x24, 0x20, 0x00);
    check("running status yields both halves of the 14-bit pair",
          g.n == 2 && same(&g, 0, (uint8_t[]){0xB0, 0x04, 0x10}, 3)
                   && same(&g, 1, (uint8_t[]){0xB0, 0x24, 0x20}, 3));

    /* 4. SysEx across two USB packets (an Identity Reply is longer than one). */
    memset(&g, 0, sizeof g);
    midiparse_init(&p, collect, &g);
    FEED(&p, 0xF0, 0x7E, 0x7F, 0x06, 0x02, 0x00, 0x00);
    check("SysEx not emitted before F7", g.n == 0);
    FEED(&p, 0x00, 0x01, 0xF7, 0xFD, 0x00);
    check("SysEx reassembled across packets",
          g.n == 1 && same(&g, 0, (uint8_t[]){0xF0, 0x7E, 0x7F, 0x06, 0x02,
                                               0x00, 0x00, 0x01, 0xF7}, 9));

    /* 5. Two-byte messages: program change and channel pressure. */
    memset(&g, 0, sizeof g);
    midiparse_init(&p, collect, &g);
    FEED(&p, 0xC0, 0x05, 0xD0, 0x40, 0x00);
    check("Cx and Dx are two bytes",
          g.n == 2 && same(&g, 0, (uint8_t[]){0xC0, 0x05}, 2)
                   && same(&g, 1, (uint8_t[]){0xD0, 0x40}, 2));

    /* 6. Realtime bytes inside a message do not break it. */
    memset(&g, 0, sizeof g);
    midiparse_init(&p, collect, &g);
    FEED(&p, 0xB0, 0xFE, 0x00, 0xF8, 0x05, 0x00);
    check("realtime bytes skipped mid-message",
          g.n == 1 && same(&g, 0, (uint8_t[]){0xB0, 0x00, 0x05}, 3));

    /* 7. Resync after a stall: data bytes are dropped until a status byte.
     * Without it, 7d 00 6e 00 under a remembered B0 is a phantom deck select
     * (B0 7D 00) plus a phantom something (B0 6E 00). */
    memset(&g, 0, sizeof g);
    midiparse_init(&p, collect, &g);
    FEED(&p, 0xB0, 0x00, 0x05, 0x00);          /* status B0 remembered */
    midiparse_resync(&p);
    FEED(&p, 0x7D, 0x00, 0x6E, 0x00, 0xB0, 0x01, 0x02, 0x00);
    check("after resync only the message with a status byte comes out",
          g.n == 2 && same(&g, 1, (uint8_t[]){0xB0, 0x01, 0x02}, 3));

    /* 8. A message split across two packets with running status in between. */
    memset(&g, 0, sizeof g);
    midiparse_init(&p, collect, &g);
    FEED(&p, 0xB0, 0x04, 0x00);                /* status + first data byte */
    FEED(&p, 0x10, 0x24, 0x20, 0x00);          /* rest, then running-status LSB */
    check("channel message split across packets",
          g.n == 2 && same(&g, 0, (uint8_t[]){0xB0, 0x04, 0x10}, 3)
                   && same(&g, 1, (uint8_t[]){0xB0, 0x24, 0x20}, 3));

    /* 9. A bad trailer is reported, and the body is still parsed. */
    memset(&g, 0, sizeof g);
    midiparse_init(&p, collect, &g);
    {
        const uint8_t bad[] = {0xB0, 0x00, 0x05, 0xFD};
        int r = midiparse_packet(&p, bad, sizeof bad);
        check("trailer != 0x00 returns 1 and the body still parses",
              r == 1 && g.n == 1);
    }

    /* 9b. A non-zero trailer must not resync. Under VirtualDJ the V7 puts a
     * sweeping status byte there on every packet (measured 2026-10-08, Mac),
     * and a resync per packet dropped every message split across two of
     * them: the platter's pitch-bend, for 20-35 ms at a time, read by the
     * sink as a stopped platter. */
    memset(&g, 0, sizeof g);
    midiparse_init(&p, collect, &g);
    FEED(&p, 0xB0, 0x02, 0x4A, 0xE2, 0x28, 0xFD, 0x1D);
    FEED(&p, 0x41, 0xB0, 0x02, 0x4C, 0xE2, 0x2F, 0xFD, 0x37);
    FEED(&p, 0x57, 0xFD, 0x38);
    check("split pitch-bends survive non-zero trailers",
          g.n == 4 && same(&g, 1, (uint8_t[]){0xE2, 0x28, 0x41}, 3)
                   && same(&g, 3, (uint8_t[]){0xE2, 0x2F, 0x57}, 3));

    /* 10. Data with no status yet is dropped, not framed as garbage. */
    memset(&g, 0, sizeof g);
    midiparse_init(&p, collect, &g);
    FEED(&p, 0x00, 0x05, 0x10, 0x00);
    check("data before any status byte is dropped", g.n == 0);

    /* 11. System common outside SysEx is not a channel message and cancels
     * running status. Without this a stall inside an Identity Reply leaves
     * the terminating F7 framing the V7's re-announcement into garbage. */
    memset(&g, 0, sizeof g);
    midiparse_init(&p, collect, &g);
    FEED(&p, 0xF0, 0x7E, 0x7F, 0x06, 0x02, 0x00);
    midiparse_resync(&p);
    FEED(&p, 0x00, 0x01, 0xF7, 0x7D, 0x00, 0x6E, 0x00, 0xB0, 0x01, 0x02, 0x00);
    check("a stray F7 frames nothing after it",
          g.n == 1 && same(&g, 0, (uint8_t[]){0xB0, 0x01, 0x02}, 3));

    /* 12. Splitting whole messages back to back, for CoreMIDI's packets. */
    {
        const uint8_t run[] = {0xB0, 0x37, 0x01, 0xB0, 0x38, 0x00, 0x90, 0x11,
                               0x7F, 0xF0, 0x7E, 0x06, 0xF7, 0xC0, 0x05, 0xF8};
        size_t off = 0, want[] = {3, 3, 3, 4, 2, 1}, k = 0, len;
        int ok = 1;

        while (off < sizeof run) {
            len = midiparse_msg_len(run + off, sizeof run - off);
            if (k >= 6 || len != want[k++])
                ok = 0;
            off += len;
        }
        check("msg_len splits a CoreMIDI packet into whole messages",
              ok && k == 6 && off == sizeof run);
    }

    if (failures) {
        printf("%d FAILURE(S)\n", failures);
        return 1;
    }
    printf("all ok\n");
    return 0;
}
