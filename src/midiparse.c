/* See midiparse.h. GPLv3. */

#include <string.h>

#include "midiparse.h"

void midiparse_init(struct midiparse *p, midi_emit_fn emit, void *ctx)
{
    memset(p, 0, sizeof *p);
    p->emit = emit;
    p->ctx = ctx;
}

size_t midiparse_msg_len(const uint8_t *d, size_t n)
{
    uint8_t st;
    unsigned hi;
    size_t i;

    if (n == 0)
        return 0;
    st = d[0];
    hi = st >> 4;
    if (st == 0xF0) {                   /* SysEx runs to its F7 */
        for (i = 1; i < n && d[i] != 0xF7; i++)
            ;
        return i < n ? i + 1 : n;
    }
    if (st < 0x80)
        return n;                       /* no status byte: not ours to split */
    if (st >= 0xF4)                     /* realtime, tune request, a lone F7 */
        return 1;
    if (st == 0xF1 || st == 0xF3 || hi == 0xC || hi == 0xD)
        return 2;
    if (st == 0xF2)
        return 3;                       /* song position */
    return n < 3 ? n : 3;
}

void midiparse_resync(struct midiparse *p)
{
    p->status = 0;
    p->len = 0;
    p->in_sysex = 0;
    p->sysex_len = 0;
}

/* Channel messages are three bytes except program change and channel
 * pressure, which are two. */
static size_t need(uint8_t status)
{
    unsigned hi = status >> 4;

    return (hi == 0xC || hi == 0xD) ? 2 : 3;
}

int midiparse_packet(struct midiparse *p, const uint8_t *pkt, size_t n)
{
    size_t i = 0, body;
    int bad = 0;

    if (n == 0)
        return 0;
    /* The trailer is not MIDI whatever it holds: 0x00 on a bare device, a
     * sweeping status byte on every packet once VirtualDJ has sent the V7
     * its SysEx init (2026-10-08). Report it, never resync on it -- a
     * resync per packet drops every message split across two of them,
     * which is the platter's pitch-bend for 20-35 ms at a time. */
    if (pkt[n - 1] != 0x00)
        bad = 1;
    body = n - 1;

    while (i < body) {
        uint8_t b = pkt[i++];

        /* Fast path: a whole 3-byte channel message sitting in the packet,
         * which is what the platter stream is (B0 00 pp E0 ll mm, the
         * statuses alternating so running status never applies). One
         * iteration per message instead of per byte: the byte loop below
         * was about a third of a core on the 2015 Mac with the platter
         * turning. */
        if (!p->in_sysex && b >= 0x80 && b < 0xF0
            && (b >> 4) != 0xC && (b >> 4) != 0xD
            && i + 1 < body && pkt[i] < 0x80 && pkt[i + 1] < 0x80) {
            p->status = b;
            p->len = 0;
            p->emit(p->ctx, pkt + i - 1, 3);
            i += 2;
            continue;
        }

        if (b == MIDI_IDLE || b >= 0xF8)
            continue;                   /* idle padding, realtime */

        if (p->in_sysex) {
            if (p->sysex_len < MIDIPARSE_SYSEX_MAX)
                p->sysex[p->sysex_len++] = b;
            if (b == 0xF7) {
                p->emit(p->ctx, p->sysex, p->sysex_len);
                p->in_sysex = 0;
                p->sysex_len = 0;
            }
            continue;
        }
        if (b == 0xF0) {
            p->in_sysex = 1;
            p->sysex_len = 0;
            p->sysex[p->sysex_len++] = b;
            continue;
        }

        if (b >= 0xF1 && b <= 0xF7) {
            /* System common outside SysEx -- or the F7 of a SysEx whose F0 a
             * resync ate. It is not a channel message and it cancels running
             * status. Without this, need() calls it three bytes long and the
             * bytes after it are framed into garbage messages: a stall landing
             * inside an Identity Reply produced `F7 7D 00` and `F7 6E 00`,
             * which on macOS reach the app as malformed MIDI. */
            p->status = 0;
            p->len = 0;
            continue;
        }

        if (b & 0x80) {
            p->status = b;
            p->buf[0] = b;
            p->len = 1;
        } else {
            if (!p->status)
                continue;               /* data with no status yet */
            if (p->len == 0) {          /* running status */
                p->buf[0] = p->status;
                p->len = 1;
            }
            if (p->len < 3)
                p->buf[p->len++] = b;
        }
        if (p->len == need(p->status)) {
            p->emit(p->ctx, p->buf, p->len);
            p->len = 0;
        }
    }
    return bad;
}
