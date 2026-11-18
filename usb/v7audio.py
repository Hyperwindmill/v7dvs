#!/usr/bin/env python3
"""Audio sink for the Numark V7: ALSA loopback capture -> iso OUT 0x02.

The V7's playback format is plain interleaved S24_3LE, 4 channels, 12 bytes per
frame -- which is exactly a format ALSA can hand us, so frames read from the
loopback go onto the wire unconverted. There is no codec here on purpose.

    xwax -> PipeWire -> snd-aloop -> [this] -> iso OUT 0x02 -> RCA

Two rates have to be reconciled and neither is ours to pick:

* PipeWire runs at 48000 and the V7 at 44100. That conversion is PipeWire's
  job, not ours: our capture opens the loopback cable first and locks it at
  44100 (an aloop cable takes its params from whoever opens first), so when
  PipeWire opens the playback side it conforms and resamples 48000->44100
  itself. This is one reason the launcher starts v7d before xwax.
* The V7's clock runs ~600 ppm fast -- measured, the servo settles at 44.127
  frames/ms, not 44.100 -- while the loopback ticks on the kernel's clock, so
  the device wants ~27 more frames per second than arrive. **That needs
  resampling, not buffering.** No cushion absorbs a persistent rate difference:
  measured, the FIFO sits at 0-3 ms whatever its target, because we consume
  everything that arrives. Holding whole frames to make up the shortfall put 30
  discontinuities a second into the signal (-25 dB of junk at a perfect pitch).
  Interpolating instead measures -49 dB, against -53 for a tone generated
  in-process, and turns the 600 ppm into a fifth of a cent.

Needs numpy, for the interpolation.

It lives in its own module rather than inside v7d.py because v7d.py is a copy
of vdj-linux's, kept in sync by hand; better one import there than a diff that
has to be carried across every time.

Channel order is ASSUMED to be deck A L, deck A R, deck B L, deck B R -- the
cabling was matched by eye. If a deck comes out swapped, swap the cables.

Kept out of scope deliberately: this is playback only. Bulk IN 0x86 is the
capture pipe and it streams zeros, because the V7 has no audio inputs.
"""
import ctypes
import gc
import os

import numpy as np

# ALSA enums we need (from alsa/pcm.h)
SND_PCM_STREAM_CAPTURE = 1
SND_PCM_FORMAT_S24_3LE = 32
SND_PCM_ACCESS_RW_NONINTERLEAVED = 4
EAGAIN = 11
# snd_pcm_state_t values the refill state machine acts on
PREPARED, RUNNING, XRUN, DRAINING, SUSPENDED = 2, 3, 4, 5, 7

CHANNELS = 4
FRAME_BYTES = CHANNELS * 3          # 12: the V7's frame, unconverted
RATE = 44100
NOMINAL_FRAMES_PER_MS = 44.1
FEEDBACK_RANGE = range(32, 56)      # plausible per-millisecond frame counts
MICROFRAMES_PER_MS = 8

# Raw hw:, NOT plughw:, and it is load-bearing. This kernel's aloop driver
# includes the access type in its cable-compatibility check: a capture side
# opened interleaved against PipeWire's non-interleaved playback side gets
# -EIO from snd_pcm_start(), forever -- measured with a parameter sweep, same
# format/rate/channels, only the access differing. And `plug` cannot get us
# out of it: it picks the slave access itself, always interleaved, with no
# way to steer it. So: raw hw, non-interleaved, and we re-interleave the four
# planes in numpy, which was already on the path.
DEFAULT_PCM = os.environ.get("V7_AUDIO_PCM", "hw:Loopback,1,0")
# The FIFO level the resampler steers toward. It is NOT a jitter buffer that
# fills up: snd-aloop advances only when we read, so the level stays near zero
# no matter what target is set -- what this really provides is the reference
# point for the resampling ratio, plus a starvation signal. It grows on
# starvation and gives the latency back after ten quiet seconds.
START_TARGET_FRAMES = 12 * 44         # 12 ms: below this it audibly holds
MAX_TARGET_FRAMES = 48 * 44                  # ~48 ms, above any sane quantum
MAX_FRAMES = 96 * 44


class Sink:
    """Reads an ALSA capture PCM and hands out one millisecond at a time."""

    def __init__(self, pcm_name=DEFAULT_PCM, log=print):
        self.alsa = ctypes.CDLL("libasound.so.2")
        self.log = log
        # Declare the prototypes. snd_pcm_readn takes an unsigned long frame
        # count and returns a signed long; leaving ctypes to assume int for
        # both is how this silently read nothing at all the first time.
        self.alsa.snd_pcm_readn.argtypes = [ctypes.c_void_p,
                                            ctypes.POINTER(ctypes.c_void_p),
                                            ctypes.c_ulong]
        self.alsa.snd_pcm_readn.restype = ctypes.c_long
        self.alsa.snd_pcm_avail_update.argtypes = [ctypes.c_void_p]
        self.alsa.snd_pcm_avail_update.restype = ctypes.c_long
        self.alsa.snd_pcm_state.argtypes = [ctypes.c_void_p]
        self.alsa.snd_pcm_state.restype = ctypes.c_int
        self.pcm = ctypes.c_void_p()
        err = self.alsa.snd_pcm_open(ctypes.byref(self.pcm), pcm_name.encode(),
                                     SND_PCM_STREAM_CAPTURE, 0)
        if err < 0:
            raise OSError(f"snd_pcm_open({pcm_name}): {self._strerror(err)}")
        # Explicit hw_params, NOT snd_pcm_set_params, and the period size is
        # the whole point: set_params derives the period from the buffer
        # (500 ms -> period 4410 = 100 ms), and **aloop advances hw_ptr one
        # period at a time** -- so our avail arrived in 100 ms bursts with
        # 100 ms of nothing in between, whatever PipeWire did on its side
        # (measured: pw-top showed its node cycling clean at 5.3 ms while our
        # ring's avail_max read ~105 ms). Any FIFO shallower than one period
        # starves in the gap, which put a hard 100 ms floor under the deck's
        # audio latency. Period 256 (5.8 ms) makes the delivery fine-grained;
        # the 500 ms buffer stays as free capacity behind it.
        hp = ctypes.c_void_p()
        self.alsa.snd_pcm_hw_params_malloc(ctypes.byref(hp))
        self.alsa.snd_pcm_hw_params_any(self.pcm, hp)
        if (self.alsa.snd_pcm_hw_params_set_access(
                self.pcm, hp, SND_PCM_ACCESS_RW_NONINTERLEAVED) < 0
            or self.alsa.snd_pcm_hw_params_set_format(
                self.pcm, hp, SND_PCM_FORMAT_S24_3LE) < 0
            or self.alsa.snd_pcm_hw_params_set_channels(
                self.pcm, hp, CHANNELS) < 0):
            raise OSError("hw_params: access/format/channels refused")
        rate = ctypes.c_uint(RATE)
        self.alsa.snd_pcm_hw_params_set_rate_near(
            self.pcm, hp, ctypes.byref(rate), None)
        if rate.value != RATE:
            # The cable is locked to the side that opened first; if that was
            # not us, resampling someone else's rate is not our job -- fail
            # loudly instead of playing at the wrong pitch.
            raise OSError(f"hw_params: cable locked at {rate.value}, "
                          f"need {RATE} (did PipeWire open first?)")
        period = ctypes.c_ulong(256)
        self.alsa.snd_pcm_hw_params_set_period_size_near(
            self.pcm, hp, ctypes.byref(period), None)
        bufsz = ctypes.c_ulong(RATE // 2)              # 500 ms
        self.alsa.snd_pcm_hw_params_set_buffer_size_near(
            self.pcm, hp, ctypes.byref(bufsz))
        err = self.alsa.snd_pcm_hw_params(self.pcm, hp)
        self.alsa.snd_pcm_hw_params_free(hp)
        if err < 0:
            raise OSError(f"snd_pcm_hw_params: {self._strerror(err)}")
        self.log(f"v7audio: period {period.value} buffer {bufsz.value}")
        self.alsa.snd_pcm_prepare(self.pcm)
        self.alsa.snd_pcm_nonblock(self.pcm, 1)

        self.fifo = bytearray()
        self.silence = bytes(FRAME_BYTES)
        self.last = self.silence
        self.rate = NOMINAL_FRAMES_PER_MS      # frames per ms, from feedback
        self.acc = 0.0
        self.target = START_TARGET_FRAMES      # grows if the producer bursts
        self.last_starve = 0
        self.min_level = 1 << 30               # lowest FIFO level this window
        self.min_since = 0                     # frame count at window start
        self.lvl_lo, self.lvl_hi = 1 << 30, 0  # level swing per report window
        self.phase = 0.0
        self.ratio = 1.0
        self.window_out = 0
        self.window_read = 0
        # Non-interleaved read lands in one plane per channel; numpy stacks
        # them back into the V7's interleaved frame in _refill.
        self.plane_bufs = [ctypes.create_string_buffer(MAX_FRAMES * 3)
                           for _ in range(CHANNELS)]
        self.planes = (ctypes.c_void_p * CHANNELS)(
            *[ctypes.cast(b, ctypes.c_void_p) for b in self.plane_bufs])
        self.stats = {"held": 0, "dropped": 0, "starved": 0, "frames": 0,
                      "read": 0, "overrun": 0, "errors": 0, "resampled": 0}
        # This allocates numpy arrays every millisecond, and a generational GC
        # pass landing in the middle is long enough to overrun the loopback's
        # capture buffer -- measured as one -12 dB gash every ~20 s. Reference
        # counting still frees everything on this path; none of it is cyclic.
        gc.disable()
        self.log(f"v7audio: capturing {pcm_name}, {CHANNELS} ch S24_3LE @ {RATE}, "
                 "gc disabled")

    def _strerror(self, err):
        self.alsa.snd_strerror.restype = ctypes.c_char_p
        return self.alsa.snd_strerror(err).decode()

    def feedback(self, value):
        """One byte off iso IN 0x81: frames the device consumed last ms."""
        if value in FEEDBACK_RANGE:
            # slow average: an individual millisecond is 44 or 45, and chasing
            # that alternation would just re-impose a pattern of our own phase
            self.rate += 0.02 * (value - self.rate)

    def _refill(self):
        # A state machine re-evaluated every millisecond, because the other
        # side of the loopback cable belongs to PipeWire, which closes and
        # reopens it whenever the JACK graph changes. The kernel notifies us
        # of that with DRAINING, and start() answers -EIO for as long as the
        # two sides' params are incompatible -- so every state here is one
        # PipeWire event away, and none of them is fatal as long as we keep
        # re-evaluating. One-shot recovery is what kept breaking here: each
        # of these conditions was hit at a moment its one shot could not
        # succeed, and the stream then sat dead with no further errors.
        state = self.alsa.snd_pcm_state(self.pcm)
        if state in (XRUN, DRAINING, SUSPENDED):
            self.stats["overrun"] += 1
            self.alsa.snd_pcm_prepare(self.pcm)    # -> PREPARED, start next ms
            return
        if state == PREPARED:
            self.alsa.snd_pcm_start(self.pcm)      # -EIO just means not yet
            return
        if state != RUNNING:
            self.alsa.snd_pcm_prepare(self.pcm)
            return
        room = MAX_FRAMES - len(self.fifo) // FRAME_BYTES
        if room <= 0:
            return
        # Ask for what is ready, never for room. snd_pcm_readn WAITS for the
        # count you request even on a non-blocking handle once a conversion
        # chain is in the way: requesting 352 frames cost 2 ms per call,
        # capping this at 500 calls/s when the device needs 1000, which
        # showed up as half the frames dropped and a -2250 ppm pitch error.
        avail = self.alsa.snd_pcm_avail_update(self.pcm)
        if avail <= 0:
            if avail < 0 and avail != -EAGAIN:
                self.stats["errors"] += 1
                self.alsa.snd_pcm_prepare(self.pcm)
            return
        n = self.alsa.snd_pcm_readn(self.pcm, self.planes, min(room, avail))
        if n > 0:
            frames = np.stack(
                [np.frombuffer(b, dtype=np.uint8, count=n * 3).reshape(n, 3)
                 for b in self.plane_bufs], axis=1)
            self.fifo += frames.tobytes()
            self.stats["read"] += n
            # No trimming HERE, and it was tried twice: PipeWire delivers in
            # period-sized bursts, and a FIFO cut right after a burst starves
            # in the following gap -- measured, both attempts, as a permanent
            # drop/hold churn eating half the frames. Standing latency is
            # shed in next_ms() instead, from the windowed *minimum* level,
            # which by definition never touches the depth the bursts use.
        elif n < 0 and n != -EAGAIN:
            # never fail silently: an unhandled errno here reads as "the source
            # is quiet", which is indistinguishable from a wiring mistake
            self.stats["errors"] += 1
            if self.stats["errors"] <= 3:
                self.log(f"v7audio: snd_pcm_readn: {self._strerror(n)} "
                         f"(state {self.alsa.snd_pcm_state(self.pcm)})")
            self.alsa.snd_pcm_prepare(self.pcm)

    def _measured_ratio(self, want):
        """How many input frames arrive per output frame, measured.

        NOT derived from the FIFO level. Steering on FIFO level is wrong for
        snd-aloop, which advances only when we read: reading slower does not
        fill our FIFO, it fills the loopback's buffer until it **overruns**.
        Measured that way -- the ratio settled at 0.9989, the loopback overran,
        and each recovery was a -17 dB gash in the signal.

        What is real is the rate the source actually delivers against the rate
        the device actually asks for, so count both over a window and divide.
        The clamp keeps a stalled source (window with no reads) from winding the
        ratio somewhere it cannot come back from.
        """
        self.window_out += want
        if self.window_out < RATE // 2:            # ~half a second
            return self.ratio
        read = self.stats["read"] - self.window_read
        ratio = read / self.window_out if self.window_out else 1.0
        self.window_read = self.stats["read"]
        self.window_out = 0
        return max(0.99, min(1.01, ratio))

    def _resample(self, want, ratio):
        """`want` output frames, reading `want * ratio` input frames.

        Linear interpolation between neighbouring frames. At the ratios this
        runs at -- within 0.2% of unity -- linear is well below audibility, and
        it is what turns a persistent rate difference into a pitch offset
        instead of a stream of discontinuities.
        """
        need = int(self.phase + want * ratio) + 2
        have = len(self.fifo) // FRAME_BYTES
        if have < need:
            # genuinely nothing to interpolate from: emit silence rather than
            # smear the last frame, and let the FIFO refill
            self.stats["starved"] += 1
            self.stats["held"] += want
            self.last_starve = self.stats["frames"]
            if self.stats["frames"] > RATE:
                self.target = min(self.target * 2, MAX_TARGET_FRAMES)
            return self.last * want

        src = np.frombuffer(bytes(self.fifo[:need * FRAME_BYTES]),
                            dtype=np.uint8).reshape(need, CHANNELS, 3)
        # S24_3LE -> int32, sign-extended
        samples = (src[:, :, 0].astype(np.int32)
                   | (src[:, :, 1].astype(np.int32) << 8)
                   | (src[:, :, 2].astype(np.int8).astype(np.int32) << 16))

        pos = self.phase + np.arange(want) * ratio
        idx = pos.astype(np.int32)
        frac = (pos - idx)[:, None]
        out = np.rint(samples[idx] * (1.0 - frac)
                      + samples[idx + 1] * frac).astype(np.int32)

        consumed = int(pos[-1] + 1)
        self.phase = float(pos[-1] + ratio - consumed)
        del self.fifo[:consumed * FRAME_BYTES]
        self.stats["resampled"] += want

        packed = np.empty((want, CHANNELS, 3), dtype=np.uint8)
        packed[:, :, 0] = out & 0xFF
        packed[:, :, 1] = (out >> 8) & 0xFF
        packed[:, :, 2] = (out >> 16) & 0xFF
        body = packed.tobytes()
        self.last = body[-FRAME_BYTES:]
        return body

    def next_ms(self, ms=1):
        """(buffer, per-microframe byte lengths) for the next `ms` milliseconds.

        The frame count comes from the device's own feedback, so this follows
        the V7's clock rather than asserting one. The pump asks for several
        milliseconds per transfer (v7d.py); here that is a loop, because the
        FIFO trim below is per-millisecond logic and works unchanged.
        """
        if ms > 1:
            parts = [self.next_ms() for _ in range(ms)]
            return b"".join(p[0] for p in parts), sum((p[1] for p in parts), [])
        self._refill()
        self.acc += self.rate
        want = int(self.acc)
        self.acc -= want

        # Shed standing latency, and only standing latency. The FIFO settles
        # wherever the writer's opening burst left it (measured: pinned at
        # the 96 ms cap, i.e. ~95 ms the deck's audio then carries forever).
        # The minimum level over a window is depth the bursts never used, so
        # cutting down to `target` can never starve the cushion -- unlike the
        # two blind trims tried before this, both of which churned. Only
        # after ten starve-free seconds, so a struggling stream keeps every
        # frame it has; and `target` steps back down on each shed, undoing
        # its own growth from the startup starvation the launcher order
        # guarantees.
        level = len(self.fifo) // FRAME_BYTES
        if level < self.min_level:
            self.min_level = level
        self.lvl_lo = min(self.lvl_lo, level)   # per-report window, see report()
        self.lvl_hi = max(self.lvl_hi, level)
        if self.stats["frames"] - self.min_since >= 3 * RATE:
            keep = int(self.target)
            if (self.min_level > keep
                    and self.stats["frames"] - self.last_starve > 10 * RATE):
                cut = self.min_level - keep
                del self.fifo[:cut * FRAME_BYTES]
                self.stats["dropped"] += cut
                self.target = max(START_TARGET_FRAMES, self.target // 2)
                if getattr(self, "pcm", None):   # absent in the selftest sink
                    self.log(f"v7audio: SHED cut={cut} "
                             f"level now {len(self.fifo) // FRAME_BYTES} "
                             f"ring {self.alsa.snd_pcm_avail_update(self.pcm)}")
            self.min_level = 1 << 30
            self.min_since = self.stats["frames"]

        # NO pre-roll here, and it is not an oversight. Waiting for the FIFO to
        # reach `target` before emitting deadlocks: **snd-aloop has no clock of
        # its own -- it only advances when the capture side reads**. Stop
        # consuming and the producer blocks on a full buffer, so the level
        # freezes below the target and nothing ever plays. Measured: 0 frames
        # out, buffer stuck at 8 ms, forever.
        #
        # The same property explains why the FIFO sits at 0-3 ms whatever the
        # target: the loopback hands over exactly what we take, never more. So
        # the startup transient (junk climbing -35 -> -49 dB over ~5 s while the
        # ratio settles) cannot be fixed by buffering ahead. It would want the
        # ratio to start at the value it converges to, which is only knowable
        # after the fact -- and it is 5 seconds of slightly-worse-than-perfect
        # at startup, so it stays.

        available = len(self.fifo) // FRAME_BYTES
        # Rate matching is RESAMPLING, not buffering: the device consumes
        # 44.13 frames/ms while the loopback produces 44.10, and no amount of
        # cushion absorbs a persistent difference -- measured, the FIFO sits at
        # 0-3 ms whatever the target, because we consume everything that
        # arrives. Holding whole frames to make up the ~30 frames/s shortfall
        # put 30 discontinuities a second into the signal, worth -25 dB of junk
        # at an otherwise perfect pitch. resample() interpolates instead, so the
        # 600 ppm becomes 1 cent of pitch nobody can hear.
        # The ratio is steered by how full the FIFO is: draining means the
        # source is behind, so read a little slower than we emit, and the other
        # way round. A gentle gain keeps it inaudible -- +-0.2% at the extremes,
        # under 4 cents, and only reached if the FIFO is nearly empty or full.
        #
        # It is then SMOOTHED, and that matters as much as the ratio itself: the
        # FIFO level jitters every millisecond, and recomputing the ratio from it
        # directly makes the ratio jump between 0.998 and 1.002, which is a
        # pitch modulation -- audible as exactly the sort of junk the resampler
        # is here to remove.
        self.ratio += 0.02 * (self._measured_ratio(want) - self.ratio)
        body = self._resample(want, self.ratio)
        self.stats["frames"] += want

        lengths = [FRAME_BYTES * ((want * (i + 1)) // MICROFRAMES_PER_MS
                                  - (want * i) // MICROFRAMES_PER_MS)
                   for i in range(MICROFRAMES_PER_MS)]
        return body, lengths

    def report(self):
        s = self.stats
        lo, hi = self.lvl_lo, self.lvl_hi
        self.lvl_lo, self.lvl_hi = 1 << 30, 0
        return (f"{s['frames']} frames out, {s['read']} read, {s['held']} held, "
                f"{s['dropped']} dropped, {s['starved']} starved ms, "
                f"{s['overrun']} overruns, {s['errors']} errors, "
                f"rate {self.rate:.3f} frames/ms, ratio {self.ratio:.6f}, "
                f"buffer {len(self.fifo) / FRAME_BYTES / NOMINAL_FRAMES_PER_MS:.1f} ms "
                f"[{lo / NOMINAL_FRAMES_PER_MS:.1f}"
                f"-{hi / NOMINAL_FRAMES_PER_MS:.1f}] "
                f"(target {self.target / NOMINAL_FRAMES_PER_MS:.0f} ms)")


def selftest():
    """Exercise the rate/FIFO logic with a fake PCM, no ALSA and no hardware."""
    sink = Sink.__new__(Sink)          # skip __init__: no device here
    sink.fifo = bytearray()
    sink.silence = bytes(FRAME_BYTES)
    sink.last = sink.silence
    sink.rate = NOMINAL_FRAMES_PER_MS
    sink.acc = 0.0
    sink.target = START_TARGET_FRAMES
    sink.last_starve = 0
    sink.min_level = 1 << 30
    sink.min_since = 0
    sink.lvl_lo, sink.lvl_hi = 1 << 30, 0
    sink.phase = 0.0
    sink.ratio = 1.0
    sink.window_out = 0
    sink.window_read = 0
    sink.stats = {"held": 0, "dropped": 0, "starved": 0, "frames": 0,
                  "read": 0, "overrun": 0, "errors": 0, "resampled": 0}
    sink._refill = lambda: None        # no PCM to read

    # starved: still produces a full millisecond, by holding
    body, lengths = sink.next_ms()
    assert sum(lengths) == len(body), (sum(lengths), len(body))
    assert len(lengths) == MICROFRAMES_PER_MS
    assert len(body) % FRAME_BYTES == 0
    assert sink.stats["starved"] == 1

    # fed in excess -- the case the trimming exists for, i.e. a source running
    # faster than the V7 asks for
    for _ in range(1000):
        sink.fifo += bytes(range(12)) * 60
        body, lengths = sink.next_ms()
        assert sum(lengths) == len(body)
        assert all(n % FRAME_BYTES == 0 for n in lengths)
        assert max(lengths) <= 156, lengths      # ISO_OUT_PKT
    assert sink.stats["resampled"] > 0, "never resampled anything"

    # fed exactly: no trimming and no holding, the steady state we want
    before = dict(sink.stats)
    for _ in range(200):
        sink.fifo += bytes(range(12)) * 44
        sink.next_ms()
    assert sink.stats["starved"] == before["starved"], "starved while fed"

    # the servo must move toward the reported rate, not past it
    sink.rate = NOMINAL_FRAMES_PER_MS
    for _ in range(500):
        sink.feedback(45)
    assert 44.9 < sink.rate <= 45.0, sink.rate
    for _ in range(500):
        sink.feedback(44)
    assert 44.0 <= sink.rate < 44.1, sink.rate

    # a millisecond's frames must average 44.1 over time, not 44 or 45
    sink.rate, sink.acc, total = NOMINAL_FRAMES_PER_MS, 0.0, 0
    for _ in range(10000):
        sink.acc += sink.rate
        want = int(sink.acc)
        sink.acc -= want
        total += want
    assert abs(total - 441000) <= 1, total
    # standing latency is shed from the windowed minimum, but never within
    # ten seconds of a starve, and never below target
    sink.fifo = bytearray(bytes(FRAME_BYTES) * 4000)   # ~90 ms of backlog
    sink.min_level = 1 << 30
    sink.min_since = 0
    sink.last_starve = 0
    sink.stats["frames"] = 20 * RATE                   # long past any starve
    sink.min_since = sink.stats["frames"] - 3 * RATE   # window already full
    before_drop = sink.stats["dropped"]
    sink.fifo += bytes(FRAME_BYTES) * 44               # keep level >= min
    sink.next_ms()
    assert sink.stats["dropped"] > before_drop, "never shed the backlog"
    assert len(sink.fifo) // FRAME_BYTES <= sink.target + 60, \
        len(sink.fifo) // FRAME_BYTES
    # ...but not when a starve was recent
    sink.fifo = bytearray(bytes(FRAME_BYTES) * 4000)
    sink.last_starve = sink.stats["frames"]
    sink.min_since = sink.stats["frames"] - 3 * RATE
    sink.min_level = 4000
    before_drop = sink.stats["dropped"]
    sink.fifo += bytes(FRAME_BYTES) * 44
    sink.next_ms()
    assert sink.stats["dropped"] == before_drop, "shed too soon after starve"

    # the resampler must produce a clean tone, which is the whole point of it
    import math
    sink.fifo = bytearray()
    sink.phase = 0.0
    sink.ratio = 1.0
    sink.window_out = 0
    sink.window_read = 0
    sink.stats["resampled"] = 0
    for n in range(RATE // 10):                    # 100 ms of 1 kHz
        v = int(0.3 * 0x7FFFFF * math.sin(2 * math.pi * 1000 * n / RATE)) & 0xFFFFFF
        sink.fifo += bytes((v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF)) * CHANNELS
    out = b"".join(sink._resample(44, 1.0006) for _ in range(90))
    got = np.frombuffer(out, dtype=np.uint8).reshape(-1, CHANNELS, 3)
    sig = (got[:, 0, 0].astype(np.int32) | (got[:, 0, 1].astype(np.int32) << 8)
           | (got[:, 0, 2].astype(np.int8).astype(np.int32) << 16)).astype(float)
    spec = np.abs(np.fft.rfft(sig * np.hanning(len(sig))))
    peak = int(np.argmax(spec[1:]) + 1)
    inband = np.sqrt((spec[peak - 2:peak + 3] ** 2).sum())
    total = np.sqrt((spec[1:] ** 2).sum())
    junk = 20 * math.log10(max(math.sqrt(max(total ** 2 - inband ** 2, 0)), 1e-9)
                           / max(inband, 1e-9))
    assert junk < -40, f"resampler is not clean: {junk:.1f} dB of junk"
    assert sink.stats["starved"] == 1, "resampler starved on a full FIFO"

    print(f"v7audio selftest ok (resampler junk {junk:.0f} dB)")


if __name__ == "__main__":
    selftest()
