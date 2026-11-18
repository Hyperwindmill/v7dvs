/*
 * Numark V7 -> a MIDI port the OS already has, plus mode A: Serato timecode
 * rendered straight into the V7's own outputs. GPLv3.
 *
 * The V7 is not class-compliant and not HID: vendor-specific USB 15e4:0075,
 * Ploytec/XMOS chipset, so the kernel creates no MIDI port. This daemon talks
 * to it with libusb from an ordinary process and publishes:
 *
 *   - a MIDI port: ALSA sequencer on Linux, a CoreMIDI virtual source and
 *     destination on macOS. Default name "DVS Controller"; "Numark V7 MIDI"
 *     (Ploytec's INF name, what VirtualDJ matches on) only via --port, because
 *     a DJ app that recognises the name can also object to it.
 *   - with --timecode, the platter's rotation as Serato timecode on the RCAs:
 *     encoder.c and platter.c render into iso OUT 0x02, following the V7's
 *     rate feedback on 0x81. No audio server anywhere. Mode A.
 *
 * Four things happen in order: the Ploytec vendor handshake on EP0; an
 * isochronous OUT pump on 0x02 -- nothing comes back until the OUT side is
 * clocking, this is a paired OUT->IN state machine, and starving it halts
 * bulk IN; bulk IN 0x83 carrying MIDI bytes padded with 0xFD plus a 0x00
 * trailer per packet; bulk OUT 0x04 taking raw MIDI back (LEDs, motor).
 *
 * Every number in here was measured on the hardware and is written down in
 * CLAUDE.md under "Hardware facts". Do not re-derive them.
 *
 * This is the C successor of usb/v7d.py + usb/v7tc.py (2026-09-16). Mode B
 * (the V7 as a sound card, ALSA loopback -> iso OUT) is still usb/v7d.py with
 * V7_AUDIO=1 on Linux.
 */

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <libusb.h>

#include "encoder.h"
#include "midiparse.h"
#include "platter.h"

#ifndef V7DVS_VERSION
#define V7DVS_VERSION "dev"
#endif

#define VID 0x15E4
#define PID 0x0075
#define ISO_OUT       0x02
#define ISO_IN        0x81
#define BULK_MIDI_IN  0x83
#define BULK_PCM_IN   0x86              /* capture: zeros, the V7 has no inputs */
#define OUT_FRAME 42                    /* bulk OUT packet, as the driver writes it */
#define OUT_TRAILER 0xE0
#define DRAIN_INFLIGHT 4
#define DRAIN_SIZE 65536
#define BULK_MIDI_OUT 0x04
#define ISO_OUT_PKT 156                 /* bytes per microframe, max */
#define FEEDBACK_PKT 64
#define BULK_IN_SIZE 512
#define MAX_ISO_PACKETS 256
#define MAX_INFLIGHT 64

/* MIDI OUT pacing: the V7 takes bytes as fast as USB delivers them and drops
 * what its 31250-baud UART cannot clock out. A token bucket: full burst when
 * idle (VDJ's three SysEx probes, 19 bytes, must leave in one piece), wire
 * speed under sustained load. One MIDI message per USB transfer, never more:
 * merging loses all but the first as far as the V7 is concerned. */
#define MIDI_OUT_BYTES_PER_S 3125.0     /* 31250 baud / 10 bits per byte */
#define MIDI_OUT_BURST 64.0             /* bucket depth, ~20 ms of wire time */
#define MIDI_OUT_INFLIGHT 8
#define MSG_MAX 512

/* The sink's side of the V7's playback: S24_3LE, 4 channels, 12 bytes per
 * frame, 44100 nominal, the two RCA pairs 6 bytes apart in the frame. */
#define FRAME_BYTES 12
#define MICROFRAMES_PER_MS 8
#define NOMINAL_FRAMES_PER_MS 44.1
#define FEEDBACK_MIN 32                 /* plausible per-ms frame counts */
#define FEEDBACK_MAX 55
#define MAX_FRAMES_PER_MS 56
#define STILL 0.001                     /* below this speed the record is silent */
#define PITCH_MAX 16383.0

#define DEFAULT_PORT_NAME "DVS Controller"
/* Only the length matters: "<client> - <port>" has to exceed winealsa's 31-char
 * limit so wine falls back to the bare port name, the one VDJ matches. It must
 * not carry the model, or the name leaks in anonymous mode. */
#define CLIENT_NAME "userspace controller daemon, USB"

/* ---- options ---------------------------------------------------------- */

static struct {
    const char *port;
    const char *timecode;               /* NULL: MIDI only */
    double counts, range, center, zero, level;
    int iso_inflight, iso_packets, bulk_inflight;
    const char *trace, *trace_out;
    int debug, selftest;
} opt = {
    DEFAULT_PORT_NAME, NULL,
    COUNTS_PER_REV, 0.08, 8192.0, 0.0005, 1.0,
    4, 32, 4,                           /* 4 x 32: the measured trade, see CLAUDE.md */
    NULL, NULL, 0, 0
};

/* ---- clock ------------------------------------------------------------ */

static double fake_clock = -1.0;        /* --selftest drives time itself */

static double now(void)
{
    struct timespec ts;

    if (fake_clock >= 0.0)
        return fake_clock;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static double t0;

static void say(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

static void warnf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static void dbg(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "[%8.3f] ", now() - t0);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static void hex(char *out, const uint8_t *b, size_t n)
{
    static const char *d = "0123456789abcdef";
    size_t i;

    for (i = 0; i < n; i++) {
        out[2 * i] = d[b[i] >> 4];
        out[2 * i + 1] = d[b[i] & 15];
    }
    out[2 * n] = 0;
}

/* ---- outbox: MIDI headed for the V7, filled from another thread --------- */

struct msg {
    uint8_t b[MSG_MAX];
    size_t n;
};

#define OUTBOX_CAP 256

static struct {
    struct msg m[OUTBOX_CAP];
    unsigned head, tail;                /* head: next to pop; tail: next to push */
    pthread_mutex_t mu;
    long dropped, oversize;             /* both are reported: see shutdown_usb */
} outbox = { .mu = PTHREAD_MUTEX_INITIALIZER };

static void outbox_push(const uint8_t *b, size_t n)
{
    if (n == 0)
        return;
    pthread_mutex_lock(&outbox.mu);
    if (n > MSG_MAX) {
        /* Counted, never silent: the Python's outbox was an unbounded deque
         * and could not lose anything, so every drop here is new. */
        outbox.oversize++;
        pthread_mutex_unlock(&outbox.mu);
        return;
    }
    if ((outbox.tail + 1) % OUTBOX_CAP == outbox.head) {
        outbox.dropped++;
    } else {
        memcpy(outbox.m[outbox.tail].b, b, n);
        outbox.m[outbox.tail].n = n;
        outbox.tail = (outbox.tail + 1) % OUTBOX_CAP;
    }
    pthread_mutex_unlock(&outbox.mu);
}

static int outbox_pop(struct msg *out)
{
    int got = 0;

    pthread_mutex_lock(&outbox.mu);
    if (outbox.head != outbox.tail) {
        *out = outbox.m[outbox.head];
        outbox.head = (outbox.head + 1) % OUTBOX_CAP;
        got = 1;
    }
    pthread_mutex_unlock(&outbox.mu);
    return got;
}

static int outbox_empty(void)
{
    int e;

    pthread_mutex_lock(&outbox.mu);
    e = outbox.head == outbox.tail;
    pthread_mutex_unlock(&outbox.mu);
    return e;
}

/* Only --selftest looks inside: known_state()'s messages, in order. */
static int outbox_peek(unsigned i, struct msg *out)
{
    unsigned idx = (outbox.head + i) % OUTBOX_CAP;

    if (i >= (outbox.tail + OUTBOX_CAP - outbox.head) % OUTBOX_CAP)
        return 0;
    *out = outbox.m[idx];
    return 1;
}

static unsigned outbox_count(void)
{
    return (outbox.tail + OUTBOX_CAP - outbox.head) % OUTBOX_CAP;
}

/* ---- the MIDI port: the one platform-specific piece ---------------------- */

static long port_events, port_dropped; /* messages emitted / refused downstream */

#ifndef __APPLE__

#include <alsa/asoundlib.h>
#include <sched.h>

static snd_seq_t *seq;
static int seq_client, port_in, port_out;
static snd_midi_event_t *seq_enc, *seq_dec;

static int port_open(const char *name)
{
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) {
        warnf("v7d: snd_seq_open failed");
        return -1;
    }
    snd_seq_set_client_name(seq, CLIENT_NAME);
    port_in = snd_seq_create_simple_port(seq, name,
                SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    port_out = snd_seq_create_simple_port(seq, name,
                SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (port_in < 0 || port_out < 0) {
        warnf("v7d: snd_seq_create_simple_port failed");
        return -1;
    }
    seq_client = snd_seq_client_id(seq);
    if (snd_midi_event_new(4096, &seq_enc) < 0
        || snd_midi_event_new(4096, &seq_dec) < 0) {
        warnf("v7d: snd_midi_event_new failed");
        return -1;
    }
    snd_midi_event_no_status(seq_dec, 1);  /* whole messages out, no running status */
    return 0;
}

static void port_describe(char *out, size_t n)
{
    snprintf(out, n, "%d:%d in / %d:%d out", seq_client, port_in, seq_client, port_out);
}

/* A whole message from the V7 -> one sequencer event to whoever subscribed. */
static void port_send(const uint8_t *msg, size_t n)
{
    snd_seq_event_t ev;

    snd_seq_ev_clear(&ev);
    snd_midi_event_reset_encode(seq_enc);
    if (snd_midi_event_encode(seq_enc, msg, n, &ev) < (long)n
        || ev.type == SND_SEQ_EVENT_NONE) {
        port_dropped++;
        return;
    }
    snd_seq_ev_set_source(&ev, port_in);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    if (snd_seq_event_output_direct(seq, &ev) < 0)
        port_dropped++;                 /* subscriber's input pool is full */
    port_events++;
}

/* Events the app writes to us -> raw MIDI in the outbox. Its own thread:
 * snd_seq_event_input blocks, and the USB loop must keep the pump fed. */
static void *port_read_loop(void *unused)
{
    snd_seq_event_t *ev;
    uint8_t buf[MSG_MAX];
    long n;

    (void)unused;
    for (;;) {
        if (snd_seq_event_input(seq, &ev) < 0)
            continue;
        n = snd_midi_event_decode(seq_dec, buf, sizeof buf, ev);
        if (n > 0) {
            if (opt.debug) {
                char h[2 * 32 + 1];
                hex(h, buf, n < 32 ? (size_t)n : 32);
                dbg("APP->V7 %3ldB %s", n, h);
            }
            outbox_push(buf, (size_t)n);
        }
    }
    return NULL;
}

static void port_close(void)
{
    if (seq)
        snd_seq_close(seq);
}

/* Realtime scheduling, load-bearing: the loop must run every few ms (iso OUT
 * wants a transfer per 4 ms) and a normal-priority process takes hiccups of
 * tens of ms under desktop load. rtprio comes from the audio group. */
static void realtime(void)
{
    struct sched_param sp = { .sched_priority = 10 };

    if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
        warnf("v7d: no realtime priority (%s); audio may gouge under load "
             "(audio group membership needs a reboot on GNOME)", strerror(errno));
}

#else /* __APPLE__ */

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>

static MIDIClientRef midi_client;
static MIDIEndpointRef midi_source, midi_dest;

/*
 * What the app writes to our destination, on CoreMIDI's own thread.
 *
 * One MIDIPacket can hold several complete messages sharing a timestamp --
 * MIDIPacketListAdd coalesces them, and an app refreshing a row of LEDs does
 * exactly that. They have to be split here, because a bulk OUT transfer
 * carries one MIDI message and the V7 drops the rest (see the pacing note
 * above). rtmidi used to deliver one message per callback, so the split is
 * what keeps this port equivalent to the Python's.
 */
static void dest_read(const MIDIPacketList *pl, void *rc, void *sc)
{
    const MIDIPacket *pk = &pl->packet[0];
    UInt32 i;

    (void)rc; (void)sc;
    for (i = 0; i < pl->numPackets; i++) {
        size_t off = 0, len;

        if (opt.debug) {
            char h[2 * 32 + 1];
            hex(h, pk->data, pk->length < 32 ? pk->length : 32);
            dbg("APP->V7 %3dB %s", pk->length, h);
        }
        while (off < pk->length) {
            len = midiparse_msg_len(pk->data + off, pk->length - off);
            if (len == 0)
                break;
            outbox_push(pk->data + off, len);
            off += len;
        }
        pk = MIDIPacketNext(pk);
    }
}

static int port_open(const char *name)
{
    CFStringRef cn = CFStringCreateWithCString(NULL, CLIENT_NAME, kCFStringEncodingUTF8);
    CFStringRef pn = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
    OSStatus st;

    st = MIDIClientCreate(cn, NULL, NULL, &midi_client);
    if (st == noErr)
        st = MIDISourceCreate(midi_client, pn, &midi_source);      /* the app reads this */
    if (st == noErr)
        st = MIDIDestinationCreate(midi_client, pn, dest_read, NULL, &midi_dest); /* the app writes here */
    CFRelease(cn);
    CFRelease(pn);
    if (st != noErr) {
        warnf("v7d: CoreMIDI setup failed (%d)", (int)st);
        return -1;
    }
    return 0;
}

static void port_describe(char *out, size_t n)
{
    snprintf(out, n, "coremidi in / coremidi out");
}

static void port_send(const uint8_t *msg, size_t n)
{
    Byte buf[MSG_MAX + 64];
    MIDIPacketList *pl = (MIDIPacketList *)buf;
    MIDIPacket *pk = MIDIPacketListInit(pl);

    pk = MIDIPacketListAdd(pl, sizeof buf, pk, mach_absolute_time(), n, msg);
    if (pk == NULL || MIDIReceived(midi_source, pl) != noErr)
        port_dropped++;
    port_events++;
}

static void *port_read_loop(void *unused)
{
    (void)unused;
    return NULL;                        /* CoreMIDI delivers by callback */
}

static void port_close(void)
{
    if (midi_dest)
        MIDIEndpointDispose(midi_dest);
    if (midi_source)
        MIDIEndpointDispose(midi_source);
    if (midi_client)
        MIDIClientDispose(midi_client);
}

/* No SCHED_FIFO here. This is what CoreAudio's IO thread runs under: one pump
 * transfer as the period, a quarter of it as the budget, done within the
 * period, preemptible. Units are mach_absolute_time ticks, converted through
 * mach_timebase_info so Apple Silicon (1/24 us ticks) gets the same 4 ms. */
static void realtime(void)
{
    mach_timebase_info_data_t tb;
    thread_time_constraint_policy_data_t pol;
    double ns_per_tick;
    uint32_t period;
    kern_return_t kr;

    mach_timebase_info(&tb);
    ns_per_tick = (double)tb.numer / (double)tb.denom;
    period = (uint32_t)((opt.iso_packets / MICROFRAMES_PER_MS) * 1e6 / ns_per_tick);
    pol.period = period;
    pol.computation = period / 4;
    pol.constraint = period;
    pol.preemptible = 1;
    kr = thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY,
                           (thread_policy_t)&pol, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (kr != KERN_SUCCESS)
        warnf("v7d: no realtime thread policy (kern_return %d); audio may gouge "
             "under load", (int)kr);
}

#endif

/* ---- the timecode sink: platter -> Serato timecode -> iso OUT -------------- */

/* MIDI, as linux/v7dvs.c has it (measured there; see CLAUDE.md). Deck B is
 * the second element; the strides differ per group and there is no single
 * rule -- that is measured too. */
static const unsigned CC_PLATTER[2]   = {0x00, 0x02};      /* 7-bit position */
static const unsigned PB_CHANNEL[2]   = {0, 2};            /* 14-bit timestamp */
static const unsigned CC_PITCH_MSB[2] = {0x04, 0x05};
static const unsigned CC_PITCH_LSB[2] = {0x24, 0x25};
#define CC_DECK 0x7D                                       /* 0 = A, 1 = B */
static const unsigned NOTE_PLAY[2]    = {0x11, 0x11 + 0x21};
#define MOTOR_ON 0x41                                      /* the LOWER number starts */
#define MOTOR_OFF 0x42
#define MOTOR_STRIDE 0x0A
static const unsigned CC_MOTOR_PITCH[2] = {0x49, 0x69};    /* MSB, LSB; +0x0a on B */
static const unsigned STATE_BLOCK[3]  = {0x45, 0x46, 0x47};/* RPM, direction, ? */
#define STATE_STRIDE 8                                     /* NOT the motor's +0x0a */
#define CC_PITCH_LED 0x37                                  /* +1 on deck B */

struct deck {
    struct tc tc;
    struct platter platter;
    int playing;
    unsigned pitch_msb, pitch_lsb;
    int pitch_seen;                     /* pitch is exactly 1.0 until then */
    int led_zero;
    double last_speed;                  /* --debug probe: speed between renders */
    int idle_said;
};

static struct {
    int active;
    int pair[2];                        /* deck feeding pair1, pair2; -1 = silent */
    struct deck deck[2];
    int sel;                            /* selected deck: the platter reports on it */
    int deck_known;                     /* CC 0x7d seen */
    double rate;                        /* frames per ms, from feedback */
    double acc;
    long frames, clamped;
    signed short scratch[2 * MAX_FRAMES_PER_MS * (MAX_ISO_PACKETS / MICROFRAMES_PER_MS)];
} tc;

static void tc_cc(unsigned param, unsigned value)
{
    uint8_t m[3] = {0xB0, (uint8_t)param, (uint8_t)value};

    outbox_push(m, 3);
}

static void tc_motor(int on)
{
    tc_cc((on ? MOTOR_ON : MOTOR_OFF) + tc.sel * MOTOR_STRIDE, 0x7F);
}

/* Both decks stopped, both platters forwards, both lamps dark: never inherit
 * what the last app left in the device. The direction latch survives power
 * cycles and a deck that plays backwards for no reason looks exactly like a
 * sign bug. Deck B's block is at +8 (0x4d..0x4f), not the motor's +0x0a: we
 * once zeroed 0x50 and left 0x4e untouched. */
static void tc_known_state(void)
{
    int d, i;

    for (d = 0; d < 2; d++) {
        for (i = 0; i < 3; i++)
            tc_cc(STATE_BLOCK[i] + d * STATE_STRIDE, 0);
        for (i = 0; i < 2; i++)
            tc_cc(CC_MOTOR_PITCH[i] + d * MOTOR_STRIDE, 0);
        tc_cc(MOTOR_OFF + d * MOTOR_STRIDE, 0x7F);
        tc_cc(CC_PITCH_LED + d, 0);
        tc.deck[d].led_zero = 0;
    }
}

/* "A,B" / "A,-" / "B,A" / "A,output": which deck feeds each RCA pair. */
static int tc_setup(const char *spec)
{
    char buf[32], *s, *tok;
    int p = 0, d;

    tc.pair[0] = tc.pair[1] = -1;
    snprintf(buf, sizeof buf, "%s", spec);
    s = buf;
    while (p < 2 && (tok = strsep(&s, ",")) != NULL) {
        while (*tok == ' ')
            tok++;
        if (*tok == 0 || !strcmp(tok, "-") || !strcasecmp(tok, "output"))
            tc.pair[p] = -1;
        else if (!strcasecmp(tok, "A"))
            tc.pair[p] = 0;
        else if (!strcasecmp(tok, "B"))
            tc.pair[p] = 1;
        else
            return -1;
        p++;
    }
    for (d = 0; d < 2; d++) {
        /* TC_DROP in, which is neither cycle 0 nor the middle -- see
         * encoder.h. Cycle 0 clamps the moment the platter is dragged
         * backwards; the middle, which this and v7dvs.c both used to do, spent
         * half the record on headroom nobody uses and put the first needle
         * lift inside the length of an ordinary track. */
        tc_seek(&tc.deck[d].tc, TC_DROP);
        platter_init(&tc.deck[d].platter);
        tc.deck[d].platter.counts_per_rev = opt.counts;
    }
    tc.sel = 0;
    tc.deck_known = 0;
    tc.rate = NOMINAL_FRAMES_PER_MS;
    tc.acc = 0.0;
    tc.frames = tc.clamped = 0;
    tc.active = 1;
    tc_known_state();
    return 0;
}

static void tc_seed_selection(int d)
{
    if (!tc.deck_known && tc.sel != d) {
        tc.sel = d;
        say("v7tc: deck %c is selected (inferred from the platter; CC 0x7d will confirm)",
            "AB"[d]);
    }
}

static double tc_pitch_factor(const struct deck *k)
{
    double raw, span;

    if (!k->pitch_seen)
        return 1.0;
    raw = (double)((k->pitch_msb << 7) | k->pitch_lsb);
    /* Each side normalised by its own travel: with a calibrated centre the
     * halves are different lengths. */
    span = raw >= opt.center ? PITCH_MAX - opt.center : opt.center;
    if (span < 1.0)
        return 1.0;
    return 1.0 + (raw - opt.center) / span * opt.range;
}

/* The fader has no centre detent; this lamp is how zero is found. One LSB of
 * hysteresis so the +-1 dither does not blink it. */
static void tc_pitch_led(int d)
{
    struct deck *k = &tc.deck[d];
    double off = fabs(tc_pitch_factor(k) - 1.0);
    double slack = fabs(opt.range) / 8192.0;
    int on = k->pitch_seen && off <= (k->led_zero ? opt.zero + slack : opt.zero);

    if (on != k->led_zero) {
        k->led_zero = on;
        tc_cc(CC_PITCH_LED + d, on);
    }
}

/*
 * A whole message from the V7. Returns 1 when it is ours and must not reach
 * the DJ app: a turntable sends no platter MIDI, and the fader is already in
 * the timecode -- forwarding it would let a mapped app sum it. Deck select
 * and PLAY are acted on and then forwarded (return 0), as the Python did.
 */
static int tc_on_midi(const uint8_t *m, size_t n)
{
    double t = now();
    uint8_t status = m[0], kind = m[0] & 0xF0;
    int d;

    if (kind == 0xB0 && status == 0xB0 && n == 3) {
        unsigned param = m[1], value = m[2];

        for (d = 0; d < 2; d++) {
            if (param == CC_PLATTER[d]) {
                platter_pos(&tc.deck[d].platter, value, t);
                tc_seed_selection(d);
                return 1;
            }
            if (param == CC_PITCH_MSB[d]) {
                tc.deck[d].pitch_msb = value;
                tc.deck[d].pitch_seen = 1;
                return 1;
            }
            if (param == CC_PITCH_LSB[d]) {
                tc.deck[d].pitch_lsb = value;
                tc.deck[d].pitch_seen = 1;
                tc_pitch_led(d);        /* on the LSB, never the MSB */
                return 1;
            }
        }
        if (param == CC_DECK) {
            /* The V7 re-announces this after every bulk IN stall recovery.
             * Same deck, nothing to do; a value above 1 is garbage after a
             * stall. */
            if (value > 1 || (tc.deck_known && tc.sel == (int)value))
                return 0;
            tc.deck_known = 1;
            tc.sel = value != 0;
            tc_motor(tc.deck[tc.sel].playing);
            say("v7tc: deck %c selected, %s", "AB"[tc.sel],
                tc.deck[tc.sel].playing ? "play" : "pause");
        }
    } else if (kind == 0xE0 && n == 3) {
        unsigned ch = status & 0x0F;

        for (d = 0; d < 2; d++)
            if (ch == PB_CHANNEL[d]) {
                platter_stamp(&tc.deck[d].platter, m[1] | (m[2] << 7), t);
                return 1;
            }
    } else if (kind == 0x90 && n == 3 && m[2] > 0) {
        for (d = 0; d < 2; d++)
            if (m[1] == NOTE_PLAY[d]) {
                tc.deck[d].playing = !tc.deck[d].playing;
                if (d == tc.sel)
                    tc_motor(tc.deck[d].playing);
                say("v7tc: deck %c %s", "AB"[d], tc.deck[d].playing ? "play" : "pause");
            }
    }
    return 0;
}

/* The selected deck is the real platter times the fader; the other runs at
 * the fader's pitch if it was in play, still if it was stopped. */
static double tc_deck_speed(int d, double t, double dt)
{
    struct deck *k = &tc.deck[d];

    if (d == tc.sel) {
        double s = platter_speed(&k->platter, t, dt) * tc_pitch_factor(k);

        if (opt.debug) {                /* probe: why does the speed move? */
            double since = (t - k->platter.arrival) * 1000.0;

            if (k->platter.started && since > k->platter.idle * 1000.0) {
                if (!k->idle_said)
                    dbg("probe: deck %c idle, last tick %.1f ms ago, now=%.4f",
                        "AB"[d], since, t);
                k->idle_said = 1;
            } else
                k->idle_said = 0;
            if (fabs(s - k->last_speed) > 0.1)
                dbg("probe: deck %c speed %.3f -> %.3f (raw %.3f, tick %.1f ms ago) now=%.4f",
                    "AB"[d], k->last_speed, s, k->platter.raw, since, t);
            k->last_speed = s;
        }
        return s;
    }
    k->platter.speed = k->playing ? tc_pitch_factor(k) : 0.0;
    return k->platter.speed;
}

static void tc_feedback(unsigned value)
{
    if (value >= FEEDBACK_MIN && value <= FEEDBACK_MAX)
        tc.rate += 0.02 * ((double)value - tc.rate);
}

/*
 * Byte counts for the next `ms` milliseconds of iso OUT, one per microframe
 * (8*ms entries), at the rate the device reports on 0x81. Returns the frame
 * count. Used with or without the sink: a MIDI-only daemon still owns the
 * pump, and must feed it at the device's rate, not the endpoint's maximum.
 * Measured 2026-10-08 against Ploytec's own driver (dev/windows-oracle.md):
 * it sends 5 or 6 frames per microframe following feedback, always; a full
 * 156-byte packet every microframe is 2.36 times what the V7 consumes.
 */
static int iso_lengths(int ms, unsigned *lengths)
{
    int want, n, i;

    tc.acc += tc.rate * ms;
    want = (int)tc.acc;
    tc.acc -= want;
    n = MICROFRAMES_PER_MS * ms;
    for (i = 0; i < n; i++)
        lengths[i] = FRAME_BYTES * ((want * (i + 1)) / n - (want * i) / n);
    return want;
}

/*
 * Render the next `ms` milliseconds into `buf` and fill `lengths` (8*ms
 * entries) with the per-microframe byte counts. Returns the frame count. One
 * speed per deck and one render per pair for the whole batch: the per-call
 * overhead is what made the pump expensive on the Mac.
 */
static int tc_next_ms(int ms, unsigned char *buf, unsigned *lengths)
{
    int want, i, p, d;
    double t, dt, speed[2];

    want = iso_lengths(ms, lengths);
    memset(buf, 0, (size_t)want * FRAME_BYTES);   /* silence for skipped pairs */
    t = now();
    dt = want / (tc.rate * 1000.0);
    for (d = 0; d < 2; d++)
        speed[d] = tc_deck_speed(d, t, dt);
    for (p = 0; p < 2; p++) {
        double cps;

        d = tc.pair[p];
        /* A stopped record under a needle is silence, not a frozen phase: a
         * constant level is a dot on the DVS scope, and djay read it as the
         * deck twitching. */
        if (d < 0 || fabs(speed[d]) < STILL)
            continue;
        if (p == 1 && tc.pair[0] == d) {
            /* The same deck on both pairs -- one deck into two interfaces,
             * which the config allows. Copy the block: rendering twice would
             * advance the same record twice, so each pair would get every
             * other block and neither would decode. */
            for (i = 0; i < want; i++)
                memcpy(buf + i * FRAME_BYTES + 6, buf + i * FRAME_BYTES, 6);
            continue;
        }
        /* cycles per sample at the device's real rate: 1000 cycles/s of vinyl
         * at speed 1.0, over rate*1000 samples/s. */
        cps = speed[d] * TC_RESOLUTION / (tc.rate * 1000.0);
        if (!tc_render_s24(&tc.deck[d].tc, cps, buf, (size_t)want, FRAME_BYTES,
                           (size_t)p * 6, opt.level, tc.scratch))
            tc.clamped++;               /* an alarm now, not a statistic: the
                                         * rewind below lifts the needle first */
        tc_rewind(&tc.deck[d].tc);
    }
    tc.frames += want;
    return want;
}

static void tc_report(char *out, size_t n)
{
    int k = snprintf(out, n, "%ld frames out, rate %.3f frames/ms, deck %c selected, "
                     "speed A %+.4f B %+.4f", tc.frames, tc.rate, "AB"[tc.sel],
                     tc.deck[0].platter.speed, tc.deck[1].platter.speed);

    if (tc.clamped && k > 0 && (size_t)k < n)
        snprintf(out + k, n - k, ", %ld clamped blocks", tc.clamped);
}

/* ---- USB ---------------------------------------------------------------- */

static libusb_context *usb;
static libusb_device_handle *dev;
static volatile sig_atomic_t stopping;
static int inflight;                    /* submitted and not yet completed */

static struct libusb_transfer *longlived[3 * MAX_INFLIGHT];
static int n_longlived;
static struct libusb_transfer *stalled[MAX_INFLIGHT];      /* bulk IN to revive */
static int n_stalled;
static struct libusb_transfer *iso_stalled[2 * MAX_INFLIGHT];
static int n_iso_stalled;
static struct libusb_transfer *drain_stalled[DRAIN_INFLIGHT];  /* bulk IN 0x86 to revive */
static int n_drain_stalled;
static int out_halted;                  /* a bulk OUT transfer failed */
static int device_gone;                 /* unplugged: there is nothing to recover */

/* Every transfer completes NO_DEVICE from here on, and clearing a halt on a
 * device that is not on the bus only prints an error per event-loop tick. End
 * the loop instead, which is also how the daemon exits on an unplug. */
static void gone(void)
{
    if (!device_gone) {
        device_gone = 1;
        warnf("v7d: the V7 disappeared from the bus; stopping");
    }
    stopping = 1;
}

static struct { long total, said; double tick; } stall;
static struct {
    long iso_ok, iso_err, in_pkts, in_bytes, max_gap_ms, out_msgs, stall_recover, drain_bytes;
} stats;
static double last_in;
static FILE *trace, *trace_out;
static int trailer_warned;
static struct midiparse parser;

static const char *usberr(int r)
{
    return libusb_strerror((enum libusb_error)r);
}

/* Ploytec vendor sequence on EP0. Without it the device stays mute. */
static int handshake(void)
{
    unsigned char buf[16];
    unsigned char rate44100[3] = {0x44, 0xAC, 0x00};
    /* The rate goes to the capture endpoint, the playback endpoint and the
     * capture endpoint again, then is read back from the capture endpoint:
     * Ploytec's own driver, byte for byte (USBPcap 2026-10-08,
     * dev/windows-oracle.md). The old guess sent it to wIndex 0x0004, the
     * MIDI OUT endpoint, which the driver never touches. */
    uint16_t idx[3] = {0x0086, 0x0002, 0x0086};
    int r, status, i;
    uint16_t confirm;

    if ((r = libusb_control_transfer(dev, 0xC0, 0x56, 0, 0, buf, 15, 2000)) < 0)   /* firmware */
        return r;
    if ((r = libusb_control_transfer(dev, 0xC0, 0x49, 0, 0, buf, 1, 2000)) < 0)    /* status */
        return r;
    if ((r = libusb_control_transfer(dev, 0xA2, 0x81, 0x0100, 0, buf, 3, 2000)) < 0) /* get rate */
        return r;
    for (i = 0; i < 3; i++)
        libusb_control_transfer(dev, 0x22, 0x01, 0x0100, idx[i], rate44100, 3, 2000); /* may fail */
    if ((r = libusb_control_transfer(dev, 0xA2, 0x81, 0x0100, 0x0086, buf, 3, 2000)) < 0)
        return r;
    if ((r = libusb_control_transfer(dev, 0xC0, 0x49, 0, 0, buf, 1, 2000)) < 0)
        return r;
    status = buf[0];
    confirm = (status | 0x20) & 0xFF;   /* set bit 5, write back */
    if (confirm & 0x80)
        confirm = (confirm - 0x100) & 0xFFFF;
    if ((r = libusb_control_transfer(dev, 0x40, 0x49, confirm, 0, NULL, 0, 2000)) < 0)
        return r;
    return status;
}

static int submit(struct libusb_transfer *t)
{
    int r = libusb_submit_transfer(t);

    if (r == 0)
        inflight++;
    return r;
}

/* A whole message off bulk IN: the sink's tap first, then the port. */
static void on_message(void *unused, const uint8_t *m, size_t n)
{
    (void)unused;
    if (tc.active && tc_on_midi(m, n)) {
        port_events++;                  /* consumed: the app never sees it */
        return;
    }
    port_send(m, n);
}

static void bulk_in_cb(struct libusb_transfer *t)
{
    inflight--;
    if (t->status == LIBUSB_TRANSFER_CANCELLED)
        return;                         /* shutting down */
    if (t->status == LIBUSB_TRANSFER_NO_DEVICE) {
        gone();
        return;
    }
    if (t->status == LIBUSB_TRANSFER_COMPLETED && t->actual_length > 0) {
        if (trace) {
            char h[2 * BULK_IN_SIZE + 1];
            hex(h, t->buffer, (size_t)t->actual_length);
            fprintf(trace, "%.6f,%s\n", now(), h);
        }
        if (opt.debug) {
            double t_now = now();
            long gap = (long)((t_now - last_in) * 1000.0);
            last_in = t_now;
            stats.in_pkts++;
            stats.in_bytes += t->actual_length;
            if (gap > stats.max_gap_ms)
                stats.max_gap_ms = gap;
        }
        if (midiparse_packet(&parser, t->buffer, (size_t)t->actual_length)
            && !trailer_warned) {
            trailer_warned = 1;
            warnf("v7d: packet trailer is not 0x00 (VirtualDJ's init turns it "
                 "into a status byte); stripped either way");
        }
    }
    if (stopping)
        return;
    /* A resubmit that fails retires this transfer for good, and there are only
     * four: swallowing the error silently kills MIDI input with nothing on
     * stderr. Hand it to the main loop; clearHalt is a synchronous control
     * transfer and must not run inside a callback.
     *
     * A halted pipe is the same failure wearing the opposite face, and it is
     * the one the V7 actually produces: the transfer COMPLETES with
     * LIBUSB_TRANSFER_STALL and a blind resubmit is ACCEPTED, so the four
     * transfers spin at the event loop's rate with no MIDI, no clearHalt and
     * no parser resync. Park those too -- revive() is what clears the pipe. */
    if (t->status != LIBUSB_TRANSFER_COMPLETED || submit(t) < 0) {
        if (n_stalled < MAX_INFLIGHT)
            stalled[n_stalled++] = t;
    }
}

/* Bulk IN 0x86 is the V7's capture stream: 2.85 MB/s of zeros, which
 * Ploytec's driver drains continuously (measured 2026-10-08: 128 kB URBs every
 * 46 ms, 7 in flight, dev/windows-oracle.md) and nobody here ever read.
 * Drained like the driver. On its own it did not stop the motor cutouts (the
 * OUT framing did), and it stays because it is what the device is used to. */
static void drain_cb(struct libusb_transfer *t)
{
    inflight--;
    if (t->status == LIBUSB_TRANSFER_CANCELLED)
        return;
    if (t->status == LIBUSB_TRANSFER_NO_DEVICE) {
        gone();
        return;
    }
    if (stopping)
        return;
    if (opt.debug)
        stats.drain_bytes += t->actual_length;
    /* Same rule as bulk IN 0x83: a halted pipe completes with STALL and a
     * blind resubmit is accepted, so park it for the main loop to clear. It
     * carries nothing, but losing it puts the device back in the state the
     * driver never leaves it in. Lost once on the Mac, first session. */
    if (t->status != LIBUSB_TRANSFER_COMPLETED || submit(t) < 0) {
        if (n_drain_stalled < DRAIN_INFLIGHT)
            drain_stalled[n_drain_stalled++] = t;
    }
}

static void iso_out_cb(struct libusb_transfer *t)
{
    int i;

    inflight--;
    if (t->status == LIBUSB_TRANSFER_CANCELLED)
        return;
    if (t->status == LIBUSB_TRANSFER_NO_DEVICE) {
        gone();
        return;
    }
    if (stopping)
        return;
    if (opt.debug) {
        for (i = 0; i < t->num_iso_packets; i++)
            if (t->iso_packet_desc[i].status == LIBUSB_TRANSFER_COMPLETED)
                stats.iso_ok++;
            else
                stats.iso_err++;
    }
    {
        unsigned lengths[MAX_ISO_PACKETS];
        int ms = opt.iso_packets / MICROFRAMES_PER_MS;

        if (tc.active)
            tc_next_ms(ms, t->buffer, lengths);
        else
            iso_lengths(ms, lengths);   /* zeros, at the device's rate */
        for (i = 0; i < t->num_iso_packets; i++)
            t->iso_packet_desc[i].length = lengths[i];
    }
    if (submit(t) < 0) {
        warnf("iso: resubmit failed; reviving from the main loop");
        if (n_iso_stalled < 2 * MAX_INFLIGHT)
            iso_stalled[n_iso_stalled++] = t;
    }
}

/* iso IN 0x81: byte 0 of each microframe is how many frames the V7 consumed. */
static void feedback_cb(struct libusb_transfer *t)
{
    int i;

    inflight--;
    if (t->status == LIBUSB_TRANSFER_CANCELLED)
        return;
    if (t->status == LIBUSB_TRANSFER_NO_DEVICE) {
        gone();
        return;
    }
    if (stopping)
        return;
    for (i = 0; i < t->num_iso_packets; i++)
        if (t->iso_packet_desc[i].status == LIBUSB_TRANSFER_COMPLETED
            && t->iso_packet_desc[i].actual_length > 0)
            tc_feedback(libusb_get_iso_packet_buffer_simple(t, i)[0]);
    if (submit(t) < 0) {
        warnf("iso in: resubmit failed; reviving from the main loop");
        if (n_iso_stalled < 2 * MAX_INFLIGHT)
            iso_stalled[n_iso_stalled++] = t;
    }
}

/* MIDI OUT: a pool of transfers, one message each, reaped by the callback. */
static struct {
    struct libusb_transfer *t[MIDI_OUT_INFLIGHT];
    unsigned char buf[MIDI_OUT_INFLIGHT][MSG_MAX];
    int busy[MIDI_OUT_INFLIGHT];
    double credit, stamp;
} midi_out;

static void midi_out_cb(struct libusb_transfer *t)
{
    int i;

    inflight--;
    for (i = 0; i < MIDI_OUT_INFLIGHT; i++)
        if (midi_out.t[i] == t)
            midi_out.busy[i] = 0;
    if (t->status == LIBUSB_TRANSFER_NO_DEVICE)
        gone();
    else if (t->status != LIBUSB_TRANSFER_COMPLETED
             && t->status != LIBUSB_TRANSFER_CANCELLED)
        out_halted = 1;                 /* the main loop clears the pipe */
}

static void pump_midi_out(void)
{
    double t = now();
    struct msg m;
    int i;

    midi_out.credit += (t - midi_out.stamp) * MIDI_OUT_BYTES_PER_S;
    if (midi_out.credit > MIDI_OUT_BURST)
        midi_out.credit = MIDI_OUT_BURST;
    midi_out.stamp = t;
    while (midi_out.credit >= 3.0 && !outbox_empty()) {
        for (i = 0; i < MIDI_OUT_INFLIGHT && midi_out.busy[i]; i++)
            ;
        if (i == MIDI_OUT_INFLIGHT)
            return;                     /* all slots in flight */
        if (!outbox_pop(&m))
            return;
        midi_out.credit -= (double)m.n;
        if (trace_out) {
            char h[2 * MSG_MAX + 1];
            hex(h, m.b, m.n);
            fprintf(trace_out, "%.6f,%s\n", t, h);
        }
        if (opt.debug)
            stats.out_msgs++;
        memcpy(midi_out.buf[i], m.b, m.n);
        /* Framed as Ploytec's driver writes it (every one of its 2142 writes in
         * the 2026-10-08 capture): a 42-byte packet, MIDI first, 0xFD padding,
         * 0xE0 as the last byte. Bare messages are accepted too, and that was
         * the bug: under VirtualDJ's LED traffic the V7's motor cut out by
         * itself every few minutes (bare: twice in 138 s; framed: 244 s clean,
         * same night, same gestures). A message longer than the frame goes
         * bare, as before. */
        {
            int len = (int)m.n;

            if (m.n <= OUT_FRAME - 1) {
                memset(midi_out.buf[i] + m.n, MIDI_IDLE, OUT_FRAME - 1 - m.n);
                midi_out.buf[i][OUT_FRAME - 1] = OUT_TRAILER;
                len = OUT_FRAME;
            }
            libusb_fill_bulk_transfer(midi_out.t[i], dev, BULK_MIDI_OUT, midi_out.buf[i],
                                      len, midi_out_cb, NULL, 1000);
        }
        midi_out.busy[i] = 1;
        if (submit(midi_out.t[i]) < 0) {
            midi_out.busy[i] = 0;
            warnf("midi out: submit failed");
            return;
        }
    }
}

static void on_signal(int sig)
{
    (void)sig;
    stopping = 1;
}

static int start_transfers(void)
{
    int i, r;

    if (!tc.active) {                   /* tc_setup did this for mode A */
        tc.rate = NOMINAL_FRAMES_PER_MS;
        tc.acc = 0.0;
    }
    for (i = 0; i < opt.iso_inflight; i++) {
        struct libusb_transfer *t = libusb_alloc_transfer(opt.iso_packets);
        unsigned char *buf = calloc((size_t)opt.iso_packets, ISO_OUT_PKT);

        unsigned lengths[MAX_ISO_PACKETS];
        int j;

        libusb_fill_iso_transfer(t, dev, ISO_OUT, buf, opt.iso_packets * ISO_OUT_PKT,
                                 opt.iso_packets, iso_out_cb, NULL, 0);
        iso_lengths(opt.iso_packets / MICROFRAMES_PER_MS, lengths);  /* zeros, nominal rate */
        for (j = 0; j < opt.iso_packets; j++)
            t->iso_packet_desc[j].length = lengths[j];
        if ((r = submit(t)) < 0) {
            warnf("v7d: iso OUT submit failed: %s", usberr(r));
            return -1;
        }
        longlived[n_longlived++] = t;
    }
    for (i = 0; i < opt.bulk_inflight; i++) {
        struct libusb_transfer *t = libusb_alloc_transfer(0);
        unsigned char *buf = malloc(BULK_IN_SIZE);

        libusb_fill_bulk_transfer(t, dev, BULK_MIDI_IN, buf, BULK_IN_SIZE, bulk_in_cb, NULL, 0);
        if ((r = submit(t)) < 0) {
            warnf("v7d: bulk IN submit failed: %s", usberr(r));
            return -1;
        }
        longlived[n_longlived++] = t;
    }
    for (i = 0; i < DRAIN_INFLIGHT; i++) {
        struct libusb_transfer *t = libusb_alloc_transfer(0);
        unsigned char *buf = malloc(DRAIN_SIZE);

        libusb_fill_bulk_transfer(t, dev, BULK_PCM_IN, buf, DRAIN_SIZE, drain_cb, NULL, 0);
        if ((r = submit(t)) < 0) {
            warnf("v7d: pcm IN submit failed: %s", usberr(r));
            return -1;
        }
        longlived[n_longlived++] = t;
    }
    for (i = 0; i < opt.iso_inflight; i++) {   /* 0x81 is followed in every mode */
        struct libusb_transfer *t = libusb_alloc_transfer(opt.iso_packets);
        unsigned char *buf = calloc((size_t)opt.iso_packets, FEEDBACK_PKT);

        libusb_fill_iso_transfer(t, dev, ISO_IN, buf, opt.iso_packets * FEEDBACK_PKT,
                                 opt.iso_packets, feedback_cb, NULL, 0);
        libusb_set_iso_packet_lengths(t, FEEDBACK_PKT);
        if ((r = submit(t)) < 0) {
            warnf("v7d: iso IN submit failed: %s", usberr(r));
            return -1;
        }
        longlived[n_longlived++] = t;
    }
    for (i = 0; i < MIDI_OUT_INFLIGHT; i++)
        midi_out.t[i] = libusb_alloc_transfer(0);
    midi_out.credit = MIDI_OUT_BURST;
    midi_out.stamp = now();
    return 0;
}

/* The pipe halted. Clear it once, resync the parser (the stream resumes
 * mid-message), put the transfers back. Report the first time always: this
 * failing invisibly is what made a stalled pipe look like dead hardware. */
static void revive(void)
{
    int i, r, revived = 0;

    if (device_gone)
        return;                         /* nothing to clear and nothing to put back */
    if (n_stalled) {
        if ((r = libusb_clear_halt(dev, BULK_MIDI_IN)) < 0)
            warnf("bulk in: clearHalt failed: %s", usberr(r));
        midiparse_resync(&parser);
        for (i = 0; i < n_stalled; ) {
            if (submit(stalled[i]) == 0) {
                stalled[i] = stalled[--n_stalled];
                revived++;
            } else {
                i++;                    /* try again next tick */
            }
        }
        if (revived && !stall.total)
            warnf("bulk in: pipe stalled, %d transfer(s) revived. A gap over 20 ms "
                 "freezes the deck, so this is worth watching.", revived);
        stall.total += revived;
        stats.stall_recover += revived;
    }
    if (n_drain_stalled) {
        if ((r = libusb_clear_halt(dev, BULK_PCM_IN)) < 0)
            warnf("pcm in: clearHalt failed: %s", usberr(r));
        for (i = 0; i < n_drain_stalled; ) {
            if (submit(drain_stalled[i]) == 0) {
                drain_stalled[i] = drain_stalled[--n_drain_stalled];
                warnf("pcm in: capture drain revived");
            } else {
                i++;
            }
        }
    }
    for (i = 0; i < n_iso_stalled; ) {  /* no clearHalt: iso endpoints do not halt */
        if (submit(iso_stalled[i]) == 0) {
            iso_stalled[i] = iso_stalled[--n_iso_stalled];
            warnf("iso: transfer revived");
        } else {
            i++;
        }
    }
    if (out_halted) {
        out_halted = 0;
        if ((r = libusb_clear_halt(dev, BULK_MIDI_OUT)) < 0)
            warnf("midi out: clearHalt failed: %s", usberr(r));
    }
    /* Keep saying so, but only while it is still happening and only twice a
     * minute. A stall that recurs during a set is the thing to catch. */
    if (stall.total != stall.said && now() - stall.tick >= 30.0) {
        stall.tick = now();
        warnf("bulk in: %ld stall recoveries so far (+%ld in the last 30 s)",
             stall.total, stall.total - stall.said);
        stall.said = stall.total;
    }
}

static void event_loop(void)
{
    struct timeval tv = {0, 2000};      /* 2 ms: every USB call stays on this thread */
    double tick = now(), report_tick = now();
    int r;

    while (!stopping) {
        r = libusb_handle_events_timeout_completed(usb, &tv, NULL);
        if (r == LIBUSB_ERROR_INTERRUPTED)
            continue;                   /* a signal landed mid-poll */
        if (r < 0) {
            warnf("usb event loop: %s", usberr(r));
            exit(1);
        }
        pump_midi_out();
        revive();
        if (tc.active && now() - report_tick >= 5.0) {
            char rep[256];
            report_tick = now();
            tc_report(rep, sizeof rep);
            say("v7tc: %s", rep);
        }
        if (opt.debug && now() - tick >= 1.0) {
            tick = now();
            if (stats.in_pkts)
                dbg("iso ok=%ld err=%ld rate %.2f | in %ld pkts %ld B, max gap %ld ms | "
                    "ev %ld drop %ld | out %ld | stall %ld | outbox drop %ld/%ld | drain %ld kB",
                    stats.iso_ok, stats.iso_err, tc.rate, stats.in_pkts, stats.in_bytes,
                    stats.max_gap_ms, port_events, port_dropped, stats.out_msgs,
                    stats.stall_recover, outbox.dropped, outbox.oversize,
                    stats.drain_bytes / 1024);
            memset(&stats, 0, sizeof stats);
            port_events = port_dropped = 0;
        }
    }
}

/*
 * A deck left in play keeps the physical platter turning after the process is
 * gone, which is a nasty surprise and is why linux/v7dvs.c stops the motor on
 * its way out. Queue MOTOR_OFF for both decks and give the pump a bounded
 * moment to get it onto the wire, because shutdown_usb cancels transfers and
 * an in-flight MOTOR_OFF cancelled is a MOTOR_OFF the V7 never saw.
 */
static void motors_off(void)
{
    struct timeval tv = {0, 2000};
    double deadline;
    int d, busy;

    if (!tc.active || device_gone)
        return;
    for (d = 0; d < 2; d++)
        tc_cc(MOTOR_OFF + d * MOTOR_STRIDE, 0x7F);
    deadline = now() + 0.25;
    while (now() < deadline) {
        pump_midi_out();
        if (libusb_handle_events_timeout_completed(usb, &tv, NULL) < 0)
            break;
        busy = 0;
        for (d = 0; d < MIDI_OUT_INFLIGHT; d++)
            busy |= midi_out.busy[d];
        if (!busy && outbox_empty())
            break;
    }
}

/* Cancel everything, wait for the cancellations, then close. Not tidiness:
 * on macOS the process cannot exit otherwise, and the next start finds the
 * interfaces still held (LIBUSB_ERROR_ACCESS). */
static void shutdown_usb(void)
{
    struct timeval tv = {0, 50000};
    double deadline;
    int i;

    for (i = 0; i < n_longlived; i++)
        libusb_cancel_transfer(longlived[i]);
    for (i = 0; i < MIDI_OUT_INFLIGHT; i++)
        if (midi_out.busy[i])
            libusb_cancel_transfer(midi_out.t[i]);
    deadline = now() + 1.0;
    while (now() < deadline && inflight > 0)
        if (libusb_handle_events_timeout_completed(usb, &tv, NULL) < 0)
            break;
    for (i = 0; i < 2; i++)
        libusb_release_interface(dev, i);
    libusb_close(dev);
    libusb_exit(usb);
    if (outbox.dropped || outbox.oversize)
        warnf("v7d: %ld MIDI message(s) to the V7 dropped (queue full), "
              "%ld too long", outbox.dropped, outbox.oversize);
    if (inflight > 0)
        say("v7d: stopped (%d transfer(s) would not cancel)", inflight);
    else
        say("v7d: stopped");
}

/* ---- selftest: a synthetic platter at nominal, decoded by xwax ------------- */

/*
 * 4 s of a platter at 2000.5 counts/s (the measured nominal) rendered as
 * pair1 <- deck A and written to stdout as s16 stereo at 44100, so
 * `./v7d --selftest | ./tcdec 44100` must read pitch 1.0 and a position that
 * advances. No hardware, no libusb. Time is driven by hand.
 */
static int selftest(void)
{
    struct msg m;
    unsigned char body[MAX_FRAMES_PER_MS * FRAME_BYTES];
    unsigned lengths[MICROFRAMES_PER_MS];
    const double per_tick = 1.0 / 2000.5;
    double next_tick = 0.0;
    long pos = 0;
    int ms, i, want, found_4e = 0;
    char rep[256];

    fake_clock = 0.0;
    /* Whatever --timecode said, so the pair routing is exercised too; "A,-"
     * is what check.sh runs and what the numbers below were measured on. */
    if (tc_setup(opt.timecode ? opt.timecode : "A,-") < 0)
        return 1;
    if (outbox_count() != 2 * (3 + 2 + 1 + 1)) {
        warnf("selftest: known_state sent %u messages, want 14", outbox_count());
        return 1;
    }
    outbox_peek(0, &m);
    if (!(m.n == 3 && m.b[0] == 0xB0 && m.b[1] == 0x45 && m.b[2] == 0)) {
        warnf("selftest: known_state does not start with B0 45 00");
        return 1;
    }
    for (i = 0; outbox_peek((unsigned)i, &m); i++)
        if (m.n == 3 && m.b[0] == 0xB0 && m.b[1] == 0x4E && m.b[2] == 0)
            found_4e = 1;
    if (!found_4e) {
        warnf("selftest: deck B's direction latch is 0x4e (+8), not the motor's +0x0a");
        return 1;
    }
    tc.rate = NOMINAL_FRAMES_PER_MS;
    for (ms = 0; ms < 4000; ms++) {
        fake_clock = ms / 1000.0;
        while (next_tick <= fake_clock) {
            unsigned raw_ts = (unsigned)((long)(next_tick * TS_HZ) % TS_WRAP);
            uint8_t cc[3] = {0xB0, 0x00, (uint8_t)(pos % 128)};
            uint8_t pb[3] = {0xE0, (uint8_t)(raw_ts & 0x7F), (uint8_t)(raw_ts >> 7)};

            tc_on_midi(cc, 3);
            tc_on_midi(pb, 3);
            pos++;
            next_tick += per_tick;
        }
        want = tc_next_ms(1, body, lengths);
        if (want > 0 && tc.pair[0] >= 0 && tc.pair[0] == tc.pair[1]
            && memcmp(body, body + 6, 6) != 0) {
            warnf("selftest: one deck on both pairs must render once and copy, "
                  "not render twice");
            return 1;
        }
        {
            unsigned sum = 0;
            for (i = 0; i < MICROFRAMES_PER_MS; i++)
                sum += lengths[i];
            if (sum != (unsigned)want * FRAME_BYTES) {
                warnf("selftest: microframe lengths sum to %u, want %d", sum, want * FRAME_BYTES);
                return 1;
            }
        }
        /* S24_3LE 4ch -> s16 stereo of pair1 for tcdec: the top two bytes. */
        for (i = 0; i < want; i++) {
            const unsigned char *f = body + i * FRAME_BYTES;
            fwrite(f + 1, 1, 2, stdout);
            fwrite(f + 4, 1, 2, stdout);
        }
    }
    fflush(stdout);
    if (!(tc.sel == 0 && tc.deck[0].platter.speed > 0.99)) {
        tc_report(rep, sizeof rep);
        warnf("selftest: %s", rep);
        return 1;
    }
    tc_report(rep, sizeof rep);
    warnf("v7d selftest: %s", rep);
    return 0;
}

/* ---- flags and main ---------------------------------------------------- */

static void usage(FILE *to)
{
    fprintf(to,
        "usage: v7d [--port NAME] [--timecode A,B] [--counts 3600] [--range 0.08]\n"
        "           [--center 8192] [--zero 0.0005] [--level 1.0] [--iso 4x32]\n"
        "           [--bulk-in 4] [--trace FILE] [--trace-out FILE] [--debug]\n"
        "           [--selftest] [--version] [--help]\n"
        "\n"
        "  --port NAME      MIDI port name (default \"%s\"; \"Numark V7 MIDI\" is\n"
        "                   what VirtualDJ matches on)\n"
        "  --timecode A,B   mode A: which deck feeds RCA pair 1 and pair 2; '-' or\n"
        "                   'output' leaves a pair silent. Without it: MIDI only.\n"
        "  --counts N       platter counts per revolution (measured: 3600)\n"
        "  --range R        pitch fader range, +-R (0.08 = vinyl; negative inverts)\n"
        "  --center C       fader value at pitch zero, 0..16383 (calibrate it)\n"
        "  --zero Z         |pitch-1| below which the pitch-zero LED lights\n"
        "  --level G        encoder gain (1.0 = -6 dBFS)\n"
        "  --iso NxM        iso OUT transfers in flight x microframes each\n"
        "  --bulk-in N      bulk IN transfers in flight\n"
        "  --trace FILE     append \"monotonic,hex\" per bulk IN packet\n"
        "  --trace-out FILE the same for bulk OUT\n"
        "  --debug          per-second counters on stderr, MIDI from the app logged\n"
        "  --selftest       synthetic platter to stdout as s16 stereo 44100\n",
        DEFAULT_PORT_NAME);
}

static int parse_args(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;

        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(stdout);
            exit(0);
        } else if (!strcmp(a, "--version")) {
            printf("v7d %s\n", V7DVS_VERSION);
            exit(0);
        } else if (!strcmp(a, "--debug")) {
            opt.debug = 1;
        } else if (!strcmp(a, "--selftest")) {
            opt.selftest = 1;           /* run it after the flags, not here:
                                         * --selftest may precede --timecode */
        } else if (v == NULL) {
            warnf("v7d: %s needs a value", a);
            return -1;
        } else if (!strcmp(a, "--port")) {
            opt.port = v; i++;
        } else if (!strcmp(a, "--timecode")) {
            opt.timecode = v; i++;
        } else if (!strcmp(a, "--counts")) {
            opt.counts = atof(v); i++;
        } else if (!strcmp(a, "--range")) {
            opt.range = atof(v); i++;
        } else if (!strcmp(a, "--center")) {
            opt.center = atof(v); i++;
        } else if (!strcmp(a, "--zero")) {
            opt.zero = atof(v); i++;
        } else if (!strcmp(a, "--level")) {
            opt.level = atof(v); i++;
        } else if (!strcmp(a, "--iso")) {
            if (sscanf(v, "%dx%d", &opt.iso_inflight, &opt.iso_packets) != 2
                || opt.iso_inflight < 1 || opt.iso_inflight > MAX_INFLIGHT
                || opt.iso_packets < 8 || opt.iso_packets > MAX_ISO_PACKETS
                || opt.iso_packets % 8) {
                warnf("v7d: --iso wants NxM with M a multiple of 8, e.g. 4x32");
                return -1;
            }
            i++;
        } else if (!strcmp(a, "--bulk-in")) {
            opt.bulk_inflight = atoi(v); i++;
            if (opt.bulk_inflight < 1 || opt.bulk_inflight > MAX_INFLIGHT) {
                warnf("v7d: --bulk-in out of range");
                return -1;
            }
        } else if (!strcmp(a, "--trace")) {
            opt.trace = v; i++;
        } else if (!strcmp(a, "--trace-out")) {
            opt.trace_out = v; i++;
        } else {
            warnf("v7d: unknown flag %s", a);
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    pthread_t reader;
    char desc[128];
    int i, r, status;

    t0 = now();
    if (parse_args(argc, argv) < 0) {
        usage(stderr);
        return 2;
    }
    if (opt.selftest)
        return selftest();
    if (opt.timecode && tc_setup(opt.timecode) < 0) {
        warnf("v7d: --timecode wants A, B, '-' or 'output' per pair, e.g. A,B");
        return 2;
    }
    if (opt.trace && (trace = fopen(opt.trace, "a")) != NULL)
        setvbuf(trace, NULL, _IOLBF, 0);
    if (opt.trace_out && (trace_out = fopen(opt.trace_out, "a")) != NULL)
        setvbuf(trace_out, NULL, _IOLBF, 0);

    realtime();

    if ((r = libusb_init(&usb)) < 0) {
        warnf("v7d: libusb_init: %s", usberr(r));
        return 1;
    }
    dev = libusb_open_device_with_vid_pid(usb, VID, PID);
    if (dev == NULL) {
        warnf("Numark V7 not found");
        return 1;
    }
    /* SET_CONFIGURATION 0 then 1: what Ploytec's driver does at every open,
     * before anything else (dev/windows-oracle.md). A soft reset of the
     * device's USB state, which this daemon never did. Best effort: on a
     * platform that refuses it the rest still works as before. */
    if ((r = libusb_set_configuration(dev, 0)) < 0)
        warnf("v7d: set configuration 0: %s", usberr(r));
    if ((r = libusb_set_configuration(dev, 1)) < 0)
        warnf("v7d: set configuration 1: %s", usberr(r));
    for (i = 0; i < 2; i++) {
        if (libusb_kernel_driver_active(dev, i) == 1)
            libusb_detach_kernel_driver(dev, i);
        if ((r = libusb_claim_interface(dev, i)) < 0) {
            warnf("v7d: claim interface %d: %s", i, usberr(r));
            return 1;
        }
        if ((r = libusb_set_interface_alt_setting(dev, i, 1)) < 0) {
            warnf("v7d: alt setting on interface %d: %s", i, usberr(r));
            return 1;
        }
    }
    /* A previous run that died mid-stall leaves the bulk pipes halted in the
     * device, and the first submit then fails with LIBUSB_ERROR_PIPE. A clear
     * on a healthy pipe is a legal no-op, so always do it. */
    if ((r = libusb_clear_halt(dev, BULK_MIDI_IN)) < 0)
        warnf("v7d: clearHalt 0x83 at startup failed: %s", usberr(r));
    if ((r = libusb_clear_halt(dev, BULK_MIDI_OUT)) < 0)
        warnf("v7d: clearHalt 0x04 at startup failed: %s", usberr(r));
    if ((r = libusb_clear_halt(dev, BULK_PCM_IN)) < 0)   /* the driver resets this pipe at every open */
        warnf("v7d: clearHalt 0x86 at startup failed: %s", usberr(r));

    if ((status = handshake()) < 0) {
        warnf("v7d: handshake failed: %s", usberr(status));
        return 1;
    }
    if (port_open(opt.port) < 0)
        return 1;
    pthread_create(&reader, NULL, port_read_loop, NULL);
    port_describe(desc, sizeof desc);
    say("%s '%s'  (handshake status=0x%02X)%s", desc, opt.port, status,
        tc.active ? "  mode A: timecode on the RCAs" : "");

    midiparse_init(&parser, on_message, NULL);
    if (start_transfers() < 0)
        return 1;

    last_in = now();                    /* or the first --debug gap is the
                                         * whole uptime of the machine */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;          /* no SA_RESTART: poll must return EINTR */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    event_loop();
    motors_off();
    shutdown_usb();
    port_close();
    return 0;
}
