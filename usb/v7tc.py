#!/usr/bin/env python3
"""Timecode straight into the V7's own outputs: platter -> Serato timecode ->
iso OUT 0x02. Mode A on macOS, where there is no JACK and no loopback cable.

    V7 platter --bulk IN--> v7d.py --> [this] --> iso OUT 0x02 --> RCA --> interface

This is the glue that linux/v7dvs.c wraps around the same C core, redone in
Python for the platform that has no audio server to hand the signal to:
which deck is selected, the pitch fader folded into the speed, silence meaning
stopped, the motor following the selected deck, and the state block zeroed at
start so nothing is inherited. The arithmetic itself stays in C -- encoder.c
and platter.c, built as libv7core by the Makefile and called through ctypes --
so what leaves the RCAs is the same signal check.sh verifies through xwax's
decoder. Both copies of the glue are a known cost; roadmap step 8b pulls it
into src/ once this one has been measured.

No resampler here, on purpose: we synthesize, so the encoder simply renders
as many frames as the V7's rate feedback asks for each millisecond, and the
cycles-per-sample it uses come from that same measured rate. The tone is
1 kHz on the device's clock whatever that clock does.

    V7_TIMECODE=A,B python3 usb/v7d.py     # pair1 <- deck A, pair2 <- deck B
    V7_TIMECODE=A,-                        # pair2 left silent

Tunables come from the environment for now (V7_TC_RANGE, V7_TC_CENTER,
V7_TC_ZERO, V7_TC_LEVEL); the launcher grows a macOS branch when there is
a second thing for it to do here.

    python3 usb/v7tc.py | ./tcdec 44100    # selftest: a synthetic platter at
                                           # nominal, decoded by xwax's own code
"""

import ctypes
import os
import sys
import time

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FRAME_BYTES = 12                     # 4 ch x S24_3LE, the V7's playback frame
PAIR_OFFSET = {"pair1": 0, "pair2": 6}
MICROFRAMES_PER_MS = 8
NOMINAL_FRAMES_PER_MS = 44.1
FEEDBACK_RANGE = range(32, 56)
TC_RESOLUTION = 1000.0
STILL = 0.001                        # below this speed the record is silent
PITCH_MAX = 16383.0

# MIDI, as linux/v7dvs.c has it (measured there; see CLAUDE.md).
CC_PLATTER = (0x00, 0x02)            # position, per deck
PB_CHANNEL = (0, 2)                  # timestamp, per deck
CC_PITCH_MSB, CC_PITCH_LSB = (0x04, 0x05), (0x24, 0x25)
CC_DECK = 0x7D
NOTE_PLAY = (0x11, 0x11 + 0x21)
MOTOR_ON, MOTOR_OFF, MOTOR_STRIDE = 0x41, 0x42, 0x0A
CC_MOTOR_PITCH = (0x49, 0x69)
STATE_BLOCK, STATE_STRIDE = (0x45, 0x46, 0x47), 8
CC_PITCH_LED = 0x37                  # +1 on deck B


class TC(ctypes.Structure):
    _fields_ = [("reg", ctypes.c_uint32), ("cyc", ctypes.c_long),
                ("phase", ctypes.c_double)]


class Tick(ctypes.Structure):
    _fields_ = [("ts", ctypes.c_double), ("pos", ctypes.c_long)]


class Platter(ctypes.Structure):            # mirrors struct platter, platter.h
    _fields_ = [("counts_per_rev", ctypes.c_double), ("tau", ctypes.c_double),
                ("idle", ctypes.c_double), ("pos7", ctypes.c_uint),
                ("raw_ts", ctypes.c_uint), ("have", ctypes.c_int),
                ("pos", ctypes.c_long), ("phys", ctypes.c_long),
                ("ts", ctypes.c_double), ("arrival", ctypes.c_double),
                ("started", ctypes.c_int), ("lead_max", ctypes.c_double),
                ("ring", Tick * 64), ("head", ctypes.c_int), ("n", ctypes.c_int),
                ("raw", ctypes.c_double), ("speed", ctypes.c_double)]


def load_core():
    for name in ("libv7core.dylib", "libv7core.so"):
        path = os.path.join(HERE, name)
        if os.path.exists(path):
            lib = ctypes.CDLL(path)
            break
    else:
        sys.exit("v7tc: libv7core not built -- run make")
    lib.platter_init.argtypes = [ctypes.POINTER(Platter)]
    lib.platter_pos.argtypes = [ctypes.POINTER(Platter), ctypes.c_uint, ctypes.c_double]
    lib.platter_stamp.argtypes = [ctypes.POINTER(Platter), ctypes.c_uint, ctypes.c_double]
    lib.platter_speed.argtypes = [ctypes.POINTER(Platter), ctypes.c_double, ctypes.c_double]
    lib.platter_speed.restype = ctypes.c_double
    lib.tc_seek.argtypes = [ctypes.POINTER(TC), ctypes.c_long]
    lib.tc_render_s24.argtypes = [ctypes.POINTER(TC), ctypes.c_double, ctypes.c_char_p,
                                  ctypes.c_size_t, ctypes.c_size_t, ctypes.c_size_t,
                                  ctypes.c_double, ctypes.POINTER(ctypes.c_short)]
    lib.tc_render_s24.restype = ctypes.c_bool
    return lib


class Deck:
    def __init__(self, lib):
        self.tc = TC()
        lib.tc_seek(self.tc, 0)
        self.platter = Platter()
        lib.platter_init(self.platter)
        self.playing = False
        self.pitch_msb = self.pitch_lsb = 0
        self.pitch_seen = False
        self.led_zero = False


class Timecode:
    """Hands out one millisecond of iso OUT at a time, like v7audio.Sink."""

    def __init__(self, pairs, send, pitch_range=0.08, pitch_center=8192.0,
                 pitch_zero=0.0005, level=1.0, log=print, lib=None):
        self.lib = lib or load_core()
        self.pairs = pairs                      # {"pair1": 0|1|None, "pair2": ...}
        self.send = send                        # bytes -> MIDI OUT to the V7
        self.pitch_range, self.pitch_center = pitch_range, pitch_center
        self.pitch_zero, self.level = pitch_zero, level
        self.log = log
        self.deck = [Deck(self.lib), Deck(self.lib)]
        self.sel = 0
        self.deck_known = False
        self.rate = NOMINAL_FRAMES_PER_MS       # frames per ms, from feedback
        self.acc = 0.0
        self.scratch = (ctypes.c_short * 4096)()
        self.frames = 0
        self.clamped = 0
        self.known_state()

    # --- MIDI from the V7 (whole messages, tapped off the port) ---

    def _cc(self, param, value):
        self.send(bytes([0xB0, param, value]))

    def motor(self, on):
        self._cc((MOTOR_ON if on else MOTOR_OFF) + self.sel * MOTOR_STRIDE, 0x7F)

    def known_state(self):
        """Both decks stopped, both platters forwards, both lamps dark: never
        inherit what the last app left in the device (see CLAUDE.md)."""
        for d in range(2):
            for cc in STATE_BLOCK:
                self._cc(cc + d * STATE_STRIDE, 0)
            for cc in CC_MOTOR_PITCH:
                self._cc(cc + d * MOTOR_STRIDE, 0)
            self._cc(MOTOR_OFF + d * MOTOR_STRIDE, 0x7F)
            self._cc(CC_PITCH_LED + d, 0)
            self.deck[d].led_zero = False

    def seed_selection(self, d):
        if not self.deck_known and self.sel != d:
            self.sel = d
            self.log(f"v7tc: deck {'AB'[d]} is selected (inferred from the "
                     "platter; CC 0x7d will confirm)")

    def on_midi(self, msg):
        """True when the message is ours and must not reach the DJ app: a
        turntable sends no platter MIDI, and the fader is already in the
        timecode -- forwarding it would let a mapped app sum it (CLAUDE.md).
        It also spares a CoreMIDI send per message, 2000/s with the platter
        turning, which on the 2015 Mac was most of the MIDI path's cost."""
        now = time.monotonic()
        status, kind = msg[0], msg[0] & 0xF0
        if kind == 0xB0 and status == 0xB0 and len(msg) == 3:
            param, value = msg[1], msg[2]
            if param in CC_PLATTER:
                d = CC_PLATTER.index(param)
                self.lib.platter_pos(self.deck[d].platter, value, now)
                self.seed_selection(d)
                return True
            elif param in CC_PITCH_MSB:
                deck = self.deck[CC_PITCH_MSB.index(param)]
                deck.pitch_msb, deck.pitch_seen = value, True
                return True
            elif param in CC_PITCH_LSB:
                d = CC_PITCH_LSB.index(param)
                self.deck[d].pitch_lsb, self.deck[d].pitch_seen = value, True
                self.pitch_led(d)              # on the LSB, never the MSB
                return True
            elif param == CC_DECK:
                # The V7 re-announces this after every bulk IN stall recovery
                # (1766 times in one djay session, 2026-09-13). Same deck,
                # nothing to do: no motor command, no log line.
                if value > 1 or (self.deck_known and self.sel == value):
                    return         # garbage after a stall, or nothing new
                self.deck_known = True
                self.sel = int(value != 0)
                self.motor(self.deck[self.sel].playing)
                self.log(f"v7tc: deck {'AB'[self.sel]} selected, "
                         f"{'play' if self.deck[self.sel].playing else 'pause'}")
        elif kind == 0xE0 and len(msg) == 3:
            ch = status & 0x0F
            if ch in PB_CHANNEL:
                raw = msg[1] | (msg[2] << 7)
                self.lib.platter_stamp(self.deck[PB_CHANNEL.index(ch)].platter, raw, now)
                return True
        elif kind == 0x90 and len(msg) == 3 and msg[2] > 0:
            if msg[1] in NOTE_PLAY:
                d = NOTE_PLAY.index(msg[1])
                self.deck[d].playing = not self.deck[d].playing
                if d == self.sel:
                    self.motor(self.deck[d].playing)
                self.log(f"v7tc: deck {'AB'[d]} "
                         f"{'play' if self.deck[d].playing else 'pause'}")
        return False

    # --- speed ---

    def pitch_factor(self, deck):
        if not deck.pitch_seen:
            return 1.0
        raw = (deck.pitch_msb << 7) | deck.pitch_lsb
        span = PITCH_MAX - self.pitch_center if raw >= self.pitch_center \
            else self.pitch_center
        if span < 1.0:
            return 1.0
        return 1.0 + (raw - self.pitch_center) / span * self.pitch_range

    def pitch_led(self, d):
        deck = self.deck[d]
        off = abs(self.pitch_factor(deck) - 1.0)
        slack = abs(self.pitch_range) / 8192.0
        on = deck.pitch_seen and off <= (self.pitch_zero + slack if deck.led_zero
                                         else self.pitch_zero)
        if on != deck.led_zero:
            deck.led_zero = on
            self._cc(CC_PITCH_LED + d, int(on))

    def deck_speed(self, d, now, dt):
        deck = self.deck[d]
        if d == self.sel:
            return self.lib.platter_speed(deck.platter, now, dt) * self.pitch_factor(deck)
        deck.platter.speed = self.pitch_factor(deck) if deck.playing else 0.0
        return deck.platter.speed

    # --- the iso OUT side, same interface as v7audio.Sink ---

    def feedback(self, value):
        if value in FEEDBACK_RANGE:
            self.rate += 0.02 * (value - self.rate)

    def next_ms(self, ms=1):
        """(buffer, per-microframe byte lengths) for the next `ms` milliseconds.

        One buffer, one speed per deck, one render per pair for the whole
        batch. Batching is what made the pump affordable on the Mac (see the
        pump geometry in v7d.py): the per-call overhead here was most of it.
        """
        self.acc += self.rate * ms
        want = int(self.acc)
        self.acc -= want
        body = ctypes.create_string_buffer(want * FRAME_BYTES)
        now = time.monotonic()
        dt = want / (self.rate * 1000.0)
        speed = [self.deck_speed(d, now, dt) for d in range(2)]
        for pair, d in self.pairs.items():
            if d is None or abs(speed[d]) < STILL:
                # A stopped record under a needle is silence, not a frozen
                # phase. A constant level is a dot on the DVS scope, and djay
                # read it as the deck twitching back and forth (2026-09-13).
                continue
            # cycles per sample at the device's real rate: 1000 cycles/s of
            # vinyl at speed 1.0, over rate*1000 samples/s.
            cps = speed[d] * TC_RESOLUTION / (self.rate * 1000.0)
            if not self.lib.tc_render_s24(self.deck[d].tc, cps, body, want,
                                          FRAME_BYTES, PAIR_OFFSET[pair],
                                          self.level, self.scratch):
                self.clamped += 1
        self.frames += want
        n = MICROFRAMES_PER_MS * ms
        lengths = [FRAME_BYTES * ((want * (i + 1)) // n - (want * i) // n)
                   for i in range(n)]
        return body.raw, lengths

    def report(self):
        return (f"{self.frames} frames out, rate {self.rate:.3f} frames/ms, "
                f"deck {'AB'[self.sel]} selected, speed "
                f"A {self.deck[0].platter.speed:+.4f} B {self.deck[1].platter.speed:+.4f}"
                + (f", {self.clamped} clamped blocks" if self.clamped else ""))


def pairs_from_env(spec):
    """'A,B' / 'A,-' / 'B,A' -> {"pair1": deck, "pair2": deck}; '-' is silent."""
    out = {}
    for pair, what in zip(("pair1", "pair2"), (spec.split(",") + ["-"])[:2]):
        what = what.strip().upper()
        out[pair] = None if what in ("", "-", "OUTPUT") else "AB".index(what)
    return out


def selftest():
    """A synthetic platter at nominal for 4 s, rendered as pair1 <- deck A and
    written to stdout as s16 stereo at 44100 -- `./tcdec 44100` must read pitch
    1.0 and a position that advances. No hardware, no libusb, no numpy."""
    sent = []
    tc = Timecode(pairs_from_env("A,-"), sent.append, log=lambda *_: None)
    assert len(sent) == 2 * (3 + 2 + 1 + 1), "known_state zeroes both decks"
    assert sent[0] == bytes([0xB0, 0x45, 0]) and bytes([0xB0, 0x4E, 0]) in sent, \
        "deck B's direction latch is 0x4e (+8), not the motor's +0x0a"
    # 2000.5 counts/s, the measured nominal; timestamp on the 2822400 Hz clock.
    pos, ts, t = 0, 0.0, 0.0
    per_tick = 1.0 / 2000.5
    next_tick = 0.0
    out = bytearray()
    # Drive time ourselves: patch the clock the glue reads.
    clock = [0.0]
    time.monotonic = lambda: clock[0]
    tc.rate = 44.1
    for ms in range(4000):
        clock[0] = ms / 1000.0
        while next_tick <= clock[0]:
            raw_ts = int(next_tick * 2822400.0) % 16384
            tc.on_midi(bytes([0xB0, 0x00, pos % 128]))
            tc.on_midi(bytes([0xE0, raw_ts & 0x7F, raw_ts >> 7]))
            pos += 1
            next_tick += per_tick
        body, lengths = tc.next_ms()
        assert sum(lengths) == len(body)
        out += body
    assert tc.sel == 0 and tc.deck[0].platter.speed > 0.99, tc.report()
    # S24_3LE 4ch -> s16 stereo of pair1 for tcdec: take the top two bytes.
    pcm = bytearray()
    for i in range(0, len(out), FRAME_BYTES):
        pcm += out[i + 1:i + 3] + out[i + 4:i + 6]
    sys.stdout.buffer.write(pcm)
    print(f"v7tc selftest: {tc.report()}", file=sys.stderr)


if __name__ == "__main__":
    selftest()
