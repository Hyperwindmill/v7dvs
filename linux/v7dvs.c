/*
 * Turn the V7's platter into a virtual DVS turntable: read platter ticks off
 * ALSA sequencer, synthesize Serato timecode, publish it on JACK. GPLv3.
 *
 *     ./v7dvs                         publish both decks' timecode on JACK
 *     ./v7dvs --speed 1.0             no MIDI, constant speed (the path that
 *                                     verified the JACK side; keep it working)
 *
 * It connects to nothing on purpose. The whole point of this project is that
 * the DJ app sees a turntable and a nameless MIDI controller, so which app
 * consumes `v7dvs:timecode_*` is the rig's business, not ours — wire it with
 * jack_connect/pw-link, or let the launcher do it.
 *
 * Two virtual decks off one physical platter. Every platter and transport
 * message says which deck it belongs to, so routing needs no guessing; CC 0x7d
 * says which deck is *selected*, which is a different question and the one the
 * motor and the idle timeout care about.
 *
 * The unselected deck runs at nominal speed if it was in play, and stays still
 * if it was stopped. It is not silent because it stopped — it is silent because
 * nobody is touching it, so the step-1 decay-to-zero rule applies only to the
 * selected side.
 */

#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <alsa/asoundlib.h>
#include <jack/jack.h>
#include <pthread.h>

#include "encoder.h"
#include "platter.h"

#define V7_PORT "Numark V7 MIDI"

/* Which MIDI messages carry the platter, per deck. The deck switch changes the
 * CC's number but the pitch-bend's channel — asymmetric, but measured. Which
 * side is which was confirmed against the hardware: deck A is the low pair. */
#define CC_CHANNEL 0
#define CC_PARAM_A 0
#define CC_PARAM_B 2
#define PB_CHANNEL_A 0
#define PB_CHANNEL_B 2

/* Which deck is selected, straight from the hardware: value 0 = A, 1 = B. This
 * beats inferring it from which platter pair is arriving, which would only be
 * right while the platter turns — press play immediately after switching, hands
 * off the platter, and the inference would send it to the wrong deck. */
#define CC_DECK 0x7d

/* Motor, deck A. Deck B is +0x0a (B0 4B / B0 4C), following the same pairing as
 * the LED block in VDJ's init. The start is the LOWER of the pair. */
#define MOTOR_ON 0x41
#define MOTOR_OFF 0x42
#define MOTOR_STRIDE 0x0a

/* The platter's LATCHED state, and 0x46 is the one that cost an afternoon:
 * `B0 46 7F` runs the platter backwards and the V7 *keeps* it — across a daemon
 * restart, across a different app, overnight. Nothing in a capture says so, and
 * the symptom is a deck that plays backwards for no reason.
 *
 * VDJ zeroes this whole block at init, which is the only reason VDJ always
 * starts forwards and we did not.
 *
 * The stride is +8 and this was measured on 2026-08-10, from the V7's own mouth:
 * flip the deck selector and it re-announces the newly selected deck's block, as
 * 0x45/0x46/0x47 for A and 0x4d/0x4e/0x4f for B. It had been +MOTOR_STRIDE here
 * on the assumption that the motor's pairing held for everything, which meant we
 * were zeroing 0x50 — a CC nothing has ever identified — and leaving deck B's
 * real direction latch at 0x4e untouched. Deck B could still inherit "backwards"
 * from whatever ran before us, which is the same afternoon waiting to happen on
 * the other half of the device. 0x47 is still unidentified.
 *
 * Note it is a different +8 from the transport's +0x21 and the motor's +0x0a: the
 * V7 has one offset per group of controls and no rule that spans them. */
static const int state_block[] = { 0x45, 0x46, 0x47 };
#define STATE_STRIDE 8

/* The motor's commanded speed, and the V7 turns out to accept one rather than
 * only on/off: nominal × (1 + V/10000) with V = (MSB<<7)|LSB read as signed
 * 14-bit two's complement, so one unit is 0.01% of pitch. Measured 2026-08-10
 * against the platter's own counts, five points from +6.4% to -8.1%, each inside
 * 0.1% of the law. Deck B is +MOTOR_STRIDE (0x53/0x73).
 *
 * Not used to drive anything — the pitch lives in the timecode (see
 * `pitch_factor`) — but zeroed at init with the rest, because an inherited LSB
 * on its own is 1.27% of pitch and it would look exactly like a broken encoder.
 * A "physical pitch" mode that spins the platter at the fader's speed is a real
 * option and an opt-in one: the motor is single and the decks are two, so it
 * would not replace this arithmetic, only add a second path beside it. */
#define CC_MOTOR_PITCH_MSB 0x49
#define CC_MOTOR_PITCH_LSB 0x69

/* Transport buttons, momentary: 0x7f on press, 0x00 on release, so the play
 * state is ours to keep. Deck B is +0x21 — measured, and a fourth distinct
 * per-group offset. */
#define NOTE_PLAY 0x11
#define NOTE_CUE 0x10
#define NOTE_STRIDE 0x21

/* The reverse lever. 0x1d is the position that latches, 0x1c the momentary
 * bleep. It is MIDI only — measured: with the lever engaged the motor still runs
 * forwards, so the V7 expects the *host* to do something about it. We do not, on
 * purpose: reverse would be a two-line negation of the speed, but whether it
 * should also spin the platter backwards is a feel decision nobody has made.
 *
 * Logged, though, because the V7 re-announces a latched lever whenever you
 * switch decks, so an engaged lever turns up in a capture looking like an
 * unrelated message. */
#define NOTE_REV 0x1d
#define NOTE_BLEEP 0x1c

/* The pitch fader, 14-bit: MSB 0x04 + LSB 0x24 on deck A, both +1 on deck B.
 * The LSB follows the standard MIDI +0x20 idiom.
 *
 * Measured here 2026-08-08, and it corrects the upstream notes in
 * ../timecode-midi, which call 0x57/0x77 the pitch fader with a +2 deck stride.
 * They are something else: with the fader physically centred those two are
 * re-announced at 0 and 1, while 0x04/0x24 sweep the full 0..16382 as the
 * fader travels. The capture is unambiguous — with deck B selected the fader
 * spoke on 0x05/0x25 and nothing else moved; after switching to deck A it
 * spoke on 0x04/0x24 and nothing else moved.
 *
 * It is worth knowing why this has to be applied here at all. The V7's fader
 * does NOT drive the motor — measured: 2000.1 counts/s at 3/4 up against
 * 2000.5 centred, i.e. software-only. So the platter keeps turning at nominal
 * whatever the fader says, and unless we fold the fader into the synthesized
 * speed the DJ app cannot know it moved. Mapping it as a plain MIDI CC does not
 * work either: in vinyl mode the deck's pitch comes from the timecode, so a
 * mapped fader either does nothing or sums, app-dependently — which is exactly
 * the app-specific behaviour this project exists to avoid. */
#define CC_PITCH_MSB 0x04
#define CC_PITCH_LSB 0x24
#define PITCH_STRIDE 1
#define PITCH_MAX 16383

/* The pitch-zero LED, and it is the only LED here on purpose: this fader has no
 * centre detent, so the light is the only way a DJ can know the deck is at
 * nominal — you cannot feel for a click that does not exist. `B0 37 01` on deck
 * A, `B0 38 01` on deck B, 0x00 to extinguish; a +1 deck stride, which is yet
 * another per-group offset.
 *
 * Taken from Mixxx's V7 mapping and then lit on the hardware before being
 * believed, which is the only order that works around here: the upstream notes
 * were confidently wrong about the fader's own CC. (The same mapping drives the
 * motor's speed on 0x49/0x69 — measured 2026-08-10 as nominal × (1 + V/10000)
 * over a signed 14-bit V, which is a physical pitch and a different project.)
 *
 * The tolerance is a knob because only a hand at the fader can judge it, and the
 * first hand that tried asked for more room within a minute. It is bounded at
 * both ends by measurements rather than taste, though. The floor is the deck's
 * own accuracy: the motor runs 0.028% fast, so inside that the fader contributes
 * less error than the platter already does and calling it zero is honest. The
 * ceiling is what the light exists to prevent — at 0.09%, which is the
 * uncertainty in `counts_per_rev`, the lamp says zero while the deck walks a beat
 * every nine minutes. 0.05% sits between them: 0.3 mm of fader travel, a beat in
 * seventeen minutes if you leave it there all night.
 *
 * In pitch rather than counts because `pitch_range` is a knob too — at ±50% the
 * same tolerance is a sixth of the counts it is at ±8%. */
#define CC_PITCH_LED 0x37
#define PITCH_LED_STRIDE 1

/*
 * One of these per virtual deck. `playing` is our own transport state, which is
 * also the motor state, because the motor IS the transport.
 *
 * Shared between the MIDI thread and the audio thread. The MIDI thread owns
 * every field except the encoder and platter.speed, which only the audio thread
 * writes — so there is no shared read-modify-write, and the worst a torn read
 * can cost is one block of slightly stale speed. ponytail: that beats a mutex in
 * the audio callback.
 */
struct deck {
    struct tc tc;
    struct platter platter;
    jack_port_t *port[2];
    int playing;

    /* The fader, as last heard. `seen` matters: the V7 sends the fader only
     * when it moves or when its deck becomes selected, so at startup we do not
     * know where it is — and guessing is worse than ignoring it. Until the
     * first message arrives the pitch is exactly 1.0. */
    int pitch_msb, pitch_lsb, pitch_seen;

    /* The pitch-zero LED as the V7 currently has it, so the MIDI OUT path only
     * carries transitions. The fader speaks ~2 messages per movement and this
     * pipe is the one that must never be flooded. */
    int led_zero;
};

static jack_client_t *client;
static struct deck deck[2];
static int sel;                         /* selected deck, per CC_DECK */
static signed short *scratch;
static jack_nframes_t scratch_frames;
static double rate;

static double fixed_speed = -1.0;       /* >= 0 disables MIDI */
static volatile sig_atomic_t stop;

/* Calibration, not preference. ±8% is the vinyl standard, but Serato and VDJ
 * also run ±16 and ±50, so it is a knob. A NEGATIVE range inverts the fader,
 * which is how you fix a deck that speeds up when you expect it to slow down —
 * cheaper than a second option for the direction.
 *
 * The centre is a knob for a physical reason: an analog fader does not return
 * to exactly half, and real turntables carry a quartz-lock button precisely
 * because of it. 0.3% of offset here is a beat of drift in a few minutes. */
static double pitch_range = 0.08;
static double pitch_center = 8192.0;
static double pitch_zero = 0.0005;      /* how close to centre lights the LED */

static snd_seq_t *seq_out;              /* NULL if the V7 is not writable */
static int out_port;

static void motor(int on);
static void send_cc(int param, int value);

/*
 * Which deck is selected, before the V7 has told us.
 *
 * CC 0x7d is the authority, but the V7 only sends it when the selection
 * *changes* — so starting the rig with deck B selected used to leave the
 * platter driving a deck nobody was listening to until you touched the
 * selector. Seed it from the first platter pair that arrives instead, and let
 * the real CC override for good the moment it appears.
 *
 * The known hole is the one that made CC 0x7d worth preferring in the first
 * place: press play immediately after starting, without touching the platter,
 * and there is still nothing to infer from. That is strictly better than
 * always guessing A, and it costs one flag.
 */

static int deck_known;

static void seed_selection(int d)
{
    if (deck_known || sel == d)
        return;
    sel = d;
    fprintf(stderr, "\nv7dvs: deck %c is selected (inferred from the platter; "
                    "CC 0x7d will confirm)\n", "AB"[d]);
}

static void on_signal(int sig)
{
    stop = 1;
}

static double mono(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int on_buffer_size(jack_nframes_t nframes, void *arg)
{
    free(scratch);
    scratch = malloc(sizeof(signed short) * 2 * nframes);
    scratch_frames = scratch ? nframes : 0;
    return scratch ? 0 : -1;
}

/*
 * The one place the feel rule lives. Only the selected deck is driven by the
 * platter — and only it may decay to zero on silence, because only it can be
 * silent *because the platter stopped*.
 */

/*
 * The fader as a multiplier on nominal speed, 1.0 until we have heard from it.
 *
 * Each side is normalised by its own travel rather than by `pitch_center` twice
 * — with a calibrated centre the two halves are not the same length, and using
 * one divisor for both would quietly cost full-scale on the longer side.
 *
 * No deadband for the ±1 LSB dither the fader shows while sitting still: at 14
 * bits over ±8% one count is 0.001%, and nothing here treats "the fader moved"
 * as an event, so the dither has nowhere to do damage. It would need one the
 * moment something reacts to a change rather than to a value.
 */

static double pitch_factor(const struct deck *d)
{
    double raw, span;

    if (!d->pitch_seen)
        return 1.0;

    raw = (d->pitch_msb << 7) | d->pitch_lsb;
    span = raw >= pitch_center ? PITCH_MAX - pitch_center : pitch_center;
    if (span < 1.0)
        return 1.0;                     /* centre pinned at an end; no travel */

    return 1.0 + (raw - pitch_center) / span * pitch_range;
}

/*
 * Light the pitch-zero LED while this deck's fader sits at nominal.
 *
 * The comment above says a deadband would be needed "the moment something reacts
 * to a change rather than to a value". This is that moment: a threshold turns a
 * value into an event, and parked exactly on it the fader's ±1 LSB dither would
 * toggle the light and the MIDI OUT pipe forever. So the light holds on for one
 * extra LSB of travel — measured in pitch, since that is what an LSB is worth
 * and it moves with `pitch_range`.
 *
 * `pitch_seen` is not a detail to skip: `pitch_factor` answers 1.0 for a fader
 * it has never heard from, and lighting a "you are at zero" lamp off that would
 * be inventing a fact about the physical world. Unknown means dark.
 */

static void pitch_led(int d)
{
    double f = pitch_factor(&deck[d]);
    double off = f > 1.0 ? f - 1.0 : 1.0 - f;
    double slack = (pitch_range < 0 ? -pitch_range : pitch_range) / 8192.0;
    int on = deck[d].pitch_seen
             && off <= (deck[d].led_zero ? pitch_zero + slack : pitch_zero);

    if (on == deck[d].led_zero)
        return;
    deck[d].led_zero = on;
    send_cc(CC_PITCH_LED + d * PITCH_LED_STRIDE, on);
}

static double deck_speed(struct deck *d, int selected, double dt)
{
    if (selected)
        return platter_speed(&d->platter, mono(), dt) * pitch_factor(d);

    /* Nobody is touching this platter, so nominal if it was in play. Written
     * through the filter, not around it, so reselecting starts from the speed
     * you were actually hearing instead of a stale scratch velocity — and so
     * `speed` stays owned by this thread alone. No ramp: xwax smooths its own
     * pitch estimate, so the snap glides. */
    return d->platter.speed = d->playing ? pitch_factor(d) : 0.0;
}

static int process(jack_nframes_t nframes, void *arg)
{
    jack_default_audio_sample_t *out[2];
    jack_nframes_t n;
    double speed;
    int c, i;

    if (nframes > scratch_frames)
        return 0;

    for (i = 0; i < 2; i++) {
        for (c = 0; c < 2; c++)
            out[c] = jack_port_get_buffer(deck[i].port[c], nframes);

        speed = fixed_speed >= 0.0
            ? fixed_speed
            : deck_speed(&deck[i], i == sel, nframes / rate);

        /* Render to s16 and widen, rather than a second float path in the
         * encoder: JACK then carries bit-for-bit what check.sh verified through
         * xwax's own decoder. One encoder, one convention, one suspect. */

        tc_render(&deck[i].tc, speed * TC_RESOLUTION / rate, scratch, nframes);
        tc_rewind(&deck[i].tc);         /* the record ends; lift the needle
                                         * rather than let the deck die on it */

        for (n = 0; n < nframes; n++) {
            out[0][n] = scratch[2 * n] / 32768.0f;
            out[1][n] = scratch[2 * n + 1] / 32768.0f;
        }
    }

    return 0;
}

/*
 * Say out loud what the reverse lever is doing. Nothing else — see NOTE_REV: the
 * lever does not touch the motor, so there is nothing to follow.
 */

static void lever(int note, int on)
{
    int d = (note - NOTE_BLEEP) / NOTE_STRIDE;  /* 0 or 1 for either position */
    int n = note - d * NOTE_STRIDE;

    if (d < 0 || d > 1 || (n != NOTE_REV && n != NOTE_BLEEP))
        return;

    fprintf(stderr, "\nv7dvs: deck %c %s lever %s%s\n", "AB"[d],
            n == NOTE_REV ? "reverse" : "bleep", on ? "engaged" : "released",
            on && n == NOTE_REV ? " (MIDI only, the platter keeps turning "
                                  "forwards — we do not implement reverse)" : "");
}

/*
 * ALSA sequencer input. Blocks in snd_seq_event_input, so it gets its own
 * thread; v7d.py must be running to publish the V7's port.
 */

static void *midi_thread(void *arg)
{
    snd_seq_t *seq = arg;
    snd_seq_event_t *ev;

    while (!stop) {
        if (snd_seq_event_input(seq, &ev) < 0)
            continue;

        switch (ev->type) {
        case SND_SEQ_EVENT_CONTROLLER:
            if (ev->data.control.channel != CC_CHANNEL)
                break;
            if (ev->data.control.param == CC_PARAM_A) {
                platter_pos(&deck[0].platter, ev->data.control.value, mono());
                seed_selection(0);
            } else if (ev->data.control.param == CC_PARAM_B) {
                platter_pos(&deck[1].platter, ev->data.control.value, mono());
                seed_selection(1);
            } else if (ev->data.control.param == CC_PITCH_MSB
                       || ev->data.control.param == CC_PITCH_MSB
                                                    + PITCH_STRIDE) {
                int d = ev->data.control.param != CC_PITCH_MSB;
                deck[d].pitch_msb = ev->data.control.value;
                deck[d].pitch_seen = 1;
            } else if (ev->data.control.param == CC_PITCH_LSB
                       || ev->data.control.param == CC_PITCH_LSB
                                                    + PITCH_STRIDE) {
                int d = ev->data.control.param != CC_PITCH_LSB;
                deck[d].pitch_lsb = ev->data.control.value;
                deck[d].pitch_seen = 1;
                /* On the LSB and never on the MSB: the V7 sends MSB first, so
                 * an MSB paired with the previous LSB is off by up to 127 counts
                 * — enough to flash the light on the way past centre. */
                pitch_led(d);
            } else if (ev->data.control.param == CC_DECK) {
                deck_known = 1;
                /* One platter, one motor, so the motor follows the selected
                 * deck — exactly how a single motorised deck behaves under
                 * Serato. Switch to a stopped deck and the platter stops; the
                 * deck you left keeps turning at nominal, in software. */
                sel = ev->data.control.value != 0;
                motor(deck[sel].playing);
                fprintf(stderr, "\nv7dvs: deck %c selected, %s\n",
                        "AB"[sel], deck[sel].playing ? "play" : "pause");
            }
            break;

        case SND_SEQ_EVENT_PITCHBEND:
            /* ALSA hands pitch-bend back centred on zero; the V7's timestamp is
             * the raw unsigned 14-bit value, so undo the centring. */
            if (ev->data.control.channel == PB_CHANNEL_A)
                platter_stamp(&deck[0].platter,
                              ev->data.control.value + 8192, mono());
            else if (ev->data.control.channel == PB_CHANNEL_B)
                platter_stamp(&deck[1].platter,
                              ev->data.control.value + 8192, mono());
            break;

        case SND_SEQ_EVENT_NOTEON:
            /* The motor IS the transport: spin the platter and the timecode
             * advances, so xwax plays. No need to ask xwax anything, and the
             * V7's own start/stop ramp does the turntable glide in hardware.
             *
             * The note says which deck, so this is right even for a deck that
             * is not the selected one — which cannot happen on this hardware,
             * since one set of buttons is remapped by the same switch, but
             * costs nothing and keeps the routing rule uniform. */
            /* The V7 releases with Note On velocity 0, not Note Off, so a
             * release is this same case with the velocity at zero. */
            if (ev->data.note.velocity > 0) {
                int n = ev->data.note.note;
                int d = n == NOTE_PLAY ? 0
                      : n == NOTE_PLAY + NOTE_STRIDE ? 1 : -1;

                if (d >= 0) {
                    deck[d].playing = !deck[d].playing;
                    if (d == sel)
                        motor(deck[d].playing);
                    fprintf(stderr, "\nv7dvs: deck %c %s\n", "AB"[d],
                            deck[d].playing ? "play" : "pause");
                } else
                    lever(n, 1);
            } else
                lever(ev->data.note.note, 0);
            break;

        case SND_SEQ_EVENT_NOTEOFF:
            lever(ev->data.note.note, 0);
            break;
        }
    }

    return NULL;
}

/*
 * Find a readable port by substring, matching either the client or the port
 * name. snd_seq_parse_address only matches the CLIENT name, which is no use
 * here: v7d.py's client is "vdj-linux Numark V7 userspace daemon" while the port
 * is "Numark V7 MIDI" — and the port name carries trailing spaces. On failure,
 * list what is actually there rather than just saying no.
 */

static int find_port(snd_seq_t *seq, const char *want, snd_seq_addr_t *addr,
                     unsigned needed, int complain)
{
    snd_seq_client_info_t *cinfo;
    snd_seq_port_info_t *pinfo;
    int found = 0;

    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);

    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(seq, cinfo) >= 0) {
        int c = snd_seq_client_info_get_client(cinfo);

        snd_seq_port_info_set_client(pinfo, c);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(seq, pinfo) >= 0) {
            const char *pname = snd_seq_port_info_get_name(pinfo);
            const char *cname = snd_seq_client_info_get_name(cinfo);

            if ((snd_seq_port_info_get_capability(pinfo) & needed) != needed)
                continue;
            if (complain) {
                fprintf(stderr, "  %3d:%-2d  %s / %s\n", c,
                        snd_seq_port_info_get_port(pinfo), cname, pname);
                continue;
            }
            if (strcasestr(pname, want) || strcasestr(cname, want)) {
                *addr = *snd_seq_port_info_get_addr(pinfo);
                found = 1;
                return found;
            }
        }
    }

    return found;
}

/*
 * Motor. Only the MIDI thread and the exit path touch this, never the audio
 * thread, so a plain send is fine.
 *
 * Addressed to the selected deck, which is what VDJ does. There is only one
 * motor, so whether the V7 gates on the address or ignores it is unknown;
 * ponytail: send the selected deck's own command, and if deck B turns out not to
 * respond, drop MOTOR_STRIDE and always use deck A's proven pair.
 */

static void send_cc(int param, int value)
{
    snd_seq_event_t ev;

    if (seq_out == NULL)
        return;

    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, out_port);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    snd_seq_ev_set_controller(&ev, CC_CHANNEL, param, value);
    snd_seq_event_output_direct(seq_out, &ev);
    snd_seq_drain_output(seq_out);
}

static void motor(int on)
{
    send_cc((on ? MOTOR_ON : MOTOR_OFF) + sel * MOTOR_STRIDE, 0x7f);
}

/*
 * Both decks stopped, both platters forwards. Inheriting the V7's latched state
 * is what made a deck play backwards with no message to explain it, so start
 * from a state we chose.
 */

static void platter_known_state(void)
{
    size_t i;
    int d;

    for (d = 0; d < 2; d++) {
        for (i = 0; i < sizeof state_block / sizeof *state_block; i++)
            send_cc(state_block[i] + d * STATE_STRIDE, 0);
        send_cc(CC_MOTOR_PITCH_MSB + d * MOTOR_STRIDE, 0);
        send_cc(CC_MOTOR_PITCH_LSB + d * MOTOR_STRIDE, 0);
        send_cc(MOTOR_OFF + d * MOTOR_STRIDE, 0x7f);
        /* Dark, because the fader has not spoken yet and a lamp we did not
         * light is a lamp the last app left on. Same rule as the platter latch:
         * never inherit this device's state. */
        deck[d].led_zero = 0;
        send_cc(CC_PITCH_LED + d * PITCH_LED_STRIDE, 0);
    }
}

static snd_seq_t *midi_open(const char *source)
{
    snd_seq_t *seq;
    snd_seq_addr_t addr;
    int in;

    /* DUPLEX, not INPUT: the motor needs writing back. */
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) {
        fprintf(stderr, "v7dvs: cannot open ALSA sequencer\n");
        return NULL;
    }
    snd_seq_set_client_name(seq, "timecode-midi");

    in = snd_seq_create_simple_port(seq, "platter",
                                    SND_SEQ_PORT_CAP_WRITE
                                    | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                    SND_SEQ_PORT_TYPE_MIDI_GENERIC
                                    | SND_SEQ_PORT_TYPE_APPLICATION);
    out_port = snd_seq_create_simple_port(seq, "motor",
                                          SND_SEQ_PORT_CAP_READ
                                          | SND_SEQ_PORT_CAP_SUBS_READ,
                                          SND_SEQ_PORT_TYPE_MIDI_GENERIC
                                          | SND_SEQ_PORT_TYPE_APPLICATION);
    if (in < 0 || out_port < 0) {
        fprintf(stderr, "v7dvs: cannot create ALSA ports\n");
        return NULL;
    }

    if (!find_port(seq, source, &addr, SND_SEQ_PORT_CAP_READ
                   | SND_SEQ_PORT_CAP_SUBS_READ, 0)
        && snd_seq_parse_address(seq, &addr, source) < 0) {
        fprintf(stderr, "v7dvs: no MIDI port matching \"%s\" "
                        "(is v7d.py running?). Readable ports:\n", source);
        find_port(seq, source, &addr, SND_SEQ_PORT_CAP_READ
                  | SND_SEQ_PORT_CAP_SUBS_READ, 1);
        return NULL;
    }
    if (snd_seq_connect_from(seq, in, addr.client, addr.port) < 0) {
        fprintf(stderr, "v7dvs: cannot subscribe to %s (%d:%d)\n",
                source, addr.client, addr.port);
        return NULL;
    }
    fprintf(stderr, "v7dvs: reading platter from %s (%d:%d)\n",
            source, addr.client, addr.port);

    /* The writable side is a different port on the same client, so search
     * again rather than assuming addr.port + 1. Failing here is not fatal —
     * the platter still works, only the motor is lost. */
    if (find_port(seq, source, &addr, SND_SEQ_PORT_CAP_WRITE
                  | SND_SEQ_PORT_CAP_SUBS_WRITE, 0)
        && snd_seq_connect_to(seq, out_port, addr.client, addr.port) == 0) {
        seq_out = seq;
        fprintf(stderr, "v7dvs: motor on %d:%d, play button toggles it\n",
                addr.client, addr.port);
        platter_known_state();
    } else {
        fprintf(stderr, "v7dvs: no writable port, motor stays manual\n");
    }

    return seq;
}

int main(int argc, char *argv[])
{
    jack_status_t status;
    const char *channel = "LR";
    const char *source = V7_PORT;
    char name[16];
    snd_seq_t *seq = NULL;
    pthread_t thread;
    int c, d, i;

    for (d = 0; d < 2; d++)
        platter_init(&deck[d].platter);

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--speed") && i + 1 < argc)
            fixed_speed = atof(argv[++i]);
        else if (!strcmp(argv[i], "--port") && i + 1 < argc)
            source = argv[++i];
        else if (!strcmp(argv[i], "--counts") && i + 1 < argc)
            /* calibration knob; one platter, so one value for both decks */
            deck[0].platter.counts_per_rev =
                deck[1].platter.counts_per_rev = atof(argv[++i]);
        else if (!strcmp(argv[i], "--pitch-range") && i + 1 < argc)
            pitch_range = atof(argv[++i]);
        else if (!strcmp(argv[i], "--pitch-center") && i + 1 < argc)
            pitch_center = atof(argv[++i]);
        else if (!strcmp(argv[i], "--pitch-zero") && i + 1 < argc)
            pitch_zero = atof(argv[++i]);
        else if (!strcmp(argv[i], "--version")) {
            puts("v7dvs " V7DVS_VERSION);
            return 0;
        }
        else {
            fprintf(stderr, "usage: %s [--speed X] [--port NAME] [--counts N]\n"
                            "       [--pitch-range F] [--pitch-center N]\n"
                            "       [--pitch-zero F] [--version]\n",
                    argv[0]);
            return 2;
        }
    }

    if (fixed_speed < 0.0) {
        seq = midi_open(source);
        if (seq == NULL)
            return 1;
    }

    client = jack_client_open("v7dvs", JackNullOption, &status);
    if (client == NULL) {
        fprintf(stderr, "v7dvs: no JACK server (status 0x%x)\n", status);
        return 1;
    }
    rate = jack_get_sample_rate(client);

    for (d = 0; d < 2; d++)
        for (c = 0; c < 2; c++) {
            snprintf(name, sizeof(name), "timecode_%c_%c", "AB"[d], channel[c]);
            deck[d].port[c] = jack_port_register(client, name,
                                                 JACK_DEFAULT_AUDIO_TYPE,
                                                 JackPortIsOutput, 0);
            if (deck[d].port[c] == NULL) {
                fprintf(stderr, "v7dvs: cannot register %s\n", name);
                return 1;
            }
        }

    jack_set_buffer_size_callback(client, on_buffer_size, NULL);
    jack_set_process_callback(client, process, NULL);

    /* TC_DROP in, with tc_rewind lifting the needle at each end as the record
     * runs out -- see encoder.h. This started mid-record until 2026-09-16,
     * which bought six minutes of backwards headroom nobody uses and then let
     * the deck die at the end of the side. */
    for (d = 0; d < 2; d++)
        tc_seek(&deck[d].tc, TC_DROP);

    if (jack_activate(client) != 0) {
        fprintf(stderr, "v7dvs: cannot activate\n");
        return 1;
    }

    fprintf(stderr, "v7dvs: %.0f Hz, %.0f counts/rev, starting at cycle %ld\n",
            rate, deck[0].platter.counts_per_rev, deck[0].tc.cyc);

    if (seq)
        pthread_create(&thread, NULL, midi_thread, seq);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    while (!stop) {
        sleep(1);
        if (seq)
            /* `counts` is there to calibrate counts_per_rev: spin a known
             * number of revolutions and divide. It needs no known speed. */
            /* The pitch is printed separately because `platter.speed` is the
             * platter alone — the fader is folded in on the way out, so a log
             * without it shows 1.000 while the deck plays at 1.08. */
            fprintf(stderr,
                    "\r%cA %+6.3f x%.4f %ld %ld   %cB %+6.3f x%.4f %ld %ld    ",
                    sel == 0 ? '>' : ' ', deck[0].platter.speed,
                    pitch_factor(&deck[0]), deck[0].tc.cyc, deck[0].platter.pos,
                    sel == 1 ? '>' : ' ', deck[1].platter.speed,
                    pitch_factor(&deck[1]), deck[1].tc.cyc,
                    deck[1].platter.pos);
    }

    /* Leave the platter still: a spinning deck outliving the process that
     * started it is a nasty surprise. */
    motor(0);

    fprintf(stderr, "\nv7dvs: stopped at cycle %ld / %ld\n",
            deck[0].tc.cyc, deck[1].tc.cyc);
    jack_client_close(client);
    free(scratch);
    return 0;
}
