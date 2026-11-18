/*
 * Whole MIDI messages out of the Numark V7's bulk IN packets. GPLv3.
 *
 * The stream is plain MIDI bytes padded with 0xFD idle, with a trailer byte
 * on every USB packet that is not MIDI (measured 0x00 over 4896/4896 packets),
 * and the V7 uses running status: on repeats it omits the status byte. A
 * parser that forgets that drops every second message of an MSB/LSB pair --
 * it made the 14-bit fader look 7-bit once (see CLAUDE.md, "Decoding a
 * V7D_TRACE needs running status").
 *
 * After a bulk IN stall the stream resumes mid-message, and the bytes that
 * follow would be framed as whatever the last status said: a phantom deck
 * select (b0 7d 6e from 7d 00 6e 00 minus a byte), a phantom pitch fader.
 * midiparse_resync() forgets the message in progress and drops data bytes
 * until the next status byte, which on this stream is at most one message
 * away.
 *
 * No ALSA, no libusb, no printing: pure state machine, tested on its own.
 */

#ifndef MIDIPARSE_H
#define MIDIPARSE_H

#include <stddef.h>
#include <stdint.h>

#define MIDI_IDLE 0xFD
#define MIDIPARSE_SYSEX_MAX 4096

typedef void (*midi_emit_fn)(void *ctx, const uint8_t *msg, size_t n);

struct midiparse {
    midi_emit_fn emit;
    void *ctx;
    uint8_t status;                     /* running status; 0 = none yet */
    uint8_t buf[3];                     /* channel message in progress */
    size_t len;                         /* bytes in buf, 0 = between messages */
    int in_sysex;
    uint8_t sysex[MIDIPARSE_SYSEX_MAX];
    size_t sysex_len;
};

void midiparse_init(struct midiparse *p, midi_emit_fn emit, void *ctx);

/*
 * One bulk IN packet, trailer included (n >= 1). Emits every whole message it
 * completes. Returns 1 if the trailer was not 0x00 -- the caller may note it,
 * once; the parser carries on, the trailer is stripped either way -- and 0
 * otherwise.
 */
int midiparse_packet(struct midiparse *p, const uint8_t *pkt, size_t n);

void midiparse_resync(struct midiparse *p);

/*
 * How many bytes the first MIDI message in `d` occupies, with `n` available.
 * For splitting a buffer that already holds whole messages back to back --
 * which is what CoreMIDI hands us in the app->V7 direction, where the V7 keeps
 * only the first message of each USB transfer. Never returns 0 for n >= 1.
 */
size_t midiparse_msg_len(const uint8_t *d, size_t n);

#endif
