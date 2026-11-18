#!/usr/bin/env python3
"""Numark V7 -> a MIDI port the OS already has, in userspace: ALSA seq on
Linux, CoreMIDI on macOS.

The V7 is not class-compliant and not HID: vendor-specific USB `15e4:0075`,
Ploytec/XMOS chipset, so the kernel creates no MIDI port and no sysfs sweep can
ever see it. Its Windows stack is three WDM
kernel drivers, which wine cannot load. All VDJ actually wants is a MIDI port,
so this daemon publishes one:

    V7 --libusb--> v7d.py --> ALSA seq port --> winealsa --> VDJ

Four things have to happen, in order:

1. the Ploytec vendor handshake on EP0 (firmware, status, sample rate),
2. an isochronous OUT pump on `0x02` — nothing comes back until the OUT side
   is clocking; this is a paired OUT->IN state machine, not a passive stream,
3. bulk IN `0x83`, which carries plain MIDI bytes padded with `0xFD` idle,
4. bulk OUT `0x04`, which takes raw MIDI bytes back (LEDs, motor, display).

Port name: `Numark V7 MIDI`, straight out of `WDM.MIDIHW.szPname` in Ploytec's
`v7_midi.inf` — that field *is* what `midiInGetDevCaps` reports.

The name alone is not enough, which is why the return path is not optional:
VDJ probes a controller with `F0 7E 7F 06 01 F7` (MIDI Universal Device
Inquiry) plus a vendor SysEx and only attaches its built-in definition once the
device answers. Measured 2026-08-02 — see `profiles/numark-v7/notes.md` in the
vdj-linux repo below.

Needs `python3-usb1` (python-libusb1) and a udev rule, which is the whole rule:

    SUBSYSTEM=="usb", ATTR{idVendor}=="15e4", ATTR{idProduct}=="0075", MODE="0666"

in e.g. /etc/udev/rules.d/99-numark-v7.rules. Without it, run as root.

Copied into timecode-midi from **vdj-linux at `fe7ddd4`** (2026-08-04) so this
repo runs on its own: the point of timecode-midi is a thing other people can
try, and nobody else has a `../vdj-linux` next door. Upstream lives at
github.com/Hyperwindmill/vdj-linux and is still in use for VirtualDJ, so **the
two copies are both alive** — a protocol fix on either side has to be carried
to the other by hand. GPLv3, like the rest of this repo.
"""
import collections
import ctypes
import os
import signal
import sys
import threading
import time

import usb1

# V7D_DEBUG=1 logs MIDI in both directions and flags gaps in the input stream
DEBUG = bool(os.environ.get("V7D_DEBUG"))
# V7D_TRACE=file.csv appends "monotonic,hex" per bulk IN packet, for jitter analysis
TRACE = os.environ.get("V7D_TRACE")
# V7D_TRACE_OUT=file.csv does the same for bulk OUT — what a host sends the V7 to
# drive the motor, LEDs and display. Separate file so the IN format stays parseable.
TRACE_OUT = os.environ.get("V7D_TRACE_OUT")
T0 = time.monotonic()


def log(fmt, *args):
    print(f"[{time.monotonic() - T0:8.3f}] " + fmt % args, file=sys.stderr, flush=True)

VID, PID = 0x15E4, 0x0075
ISO_OUT, ISO_IN, BULK_MIDI_IN, BULK_MIDI_OUT = 0x02, 0x81, 0x83, 0x04
ISO_OUT_PKT = 156
RATE_44100 = bytes([0x44, 0xAC, 0x00])
MIDI_IDLE = 0xFD
MIDI_OUT_BYTES_PER_S = 3125.0      # 31250 baud / 10 bits per MIDI byte
MIDI_OUT_BURST = 64                # token bucket depth, ~20 ms of wire time
MIDI_OUT_INFLIGHT = 8              # concurrent bulk OUT transfers

# The iso OUT pump's shape, and it is not a free parameter. Starving this pipe
# is what halts bulk IN -- the V7 is a paired OUT->IN state machine, so the
# platter goes quiet because the pump fell behind, not because the device
# stopped. Measured on a 2015 MacBook (dev/mac/probe-report-2026-08-09.md),
# five seconds each with the platter spinning:
#
#   in flight x packets   queue    stalls
#         2 x 32           8 ms      117
#         4 x 16           8 ms       13
#         8 x  8           8 ms        5
#         4 x 32          16 ms      0..3   <- what this file uses
#         8 x 32          32 ms        0
#         8 x 64          64 ms        0
#
# Two things drive it and they pull opposite ways: more transfers in flight
# helps, and a deeper queue helps, but queue depth *is* latency -- in analog
# mode this pipe carries the timecode, so 64 ms means the app sees the platter
# 64 ms late. 4 x 32 is the best trade.
#
# 8 x 8 was what this file used until 2026-09-13, because sink.next_ms() hands
# out exactly one millisecond. The first DJ-app test on the Mac ended that:
# 1000 Python callbacks a second cost 56% of a thermally limited core with
# the rig alone, and with djay Pro open the pump delivered 44% of the
# microframes the V7 asked for, which the V7 answers by halting bulk IN 44
# times a second. The callback now asks the sink for ISO_PACKETS // 8
# milliseconds, so the per-transfer overhead is paid a quarter as often.
ISO_INFLIGHT = int(os.environ.get("V7_ISO_INFLIGHT", 4))   # iso OUT in flight
ISO_PACKETS = int(os.environ.get("V7_ISO_PACKETS", 32))    # microframes each
BULK_IN_INFLIGHT = int(os.environ.get("V7_BULK_INFLIGHT", 4))
# ponytail: the three env overrides exist to measure shapes against a real
# DJ app without a commit per attempt; the defaults are the answer so far.

# Anonymous by default, and this is a safety property rather than a principle.
# "Numark V7 MIDI" is the name from Ploytec's INF, and announcing it is what
# makes VirtualDJ attach its built-in definition -- which is the whole of the
# passthrough mode. But a DJ app that recognises the name can also *object* to
# it: Serato DJ Pro warns that the V7 is no longer supported, and in every DVS
# mode here Serato has no business seeing a V7 at all -- it is looking at a
# turntable and an unnamed controller. So the real name is opt-in, passed in as
# argv[1] by whoever wants the passthrough, and never the default.
DEFAULT_PORT_NAME = "DVS Controller"
V7_PORT_NAME = "Numark V7 MIDI"    # opt-in: what VirtualDJ matches on
# Only the length matters: "<client> - <port>" has to exceed winealsa's 31-char
# limit, which is what makes wine fall back to the bare port name -- the one the
# app matches. It must not carry the model, or the name leaks in anonymous mode.
CLIENT_NAME = "userspace controller daemon, USB"

SND_SEQ_OPEN_DUPLEX = 3
CAP_READ, CAP_WRITE, CAP_SUBS_READ, CAP_SUBS_WRITE = 1, 2, 32, 64
TYPE_MIDI_GENERIC = 2 | (1 << 20)          # MIDI_GENERIC | APPLICATION
ADDR_SUBSCRIBERS, ADDR_UNKNOWN = 254, 253
QUEUE_DIRECT = 253


class SeqAddr(ctypes.Structure):
    _fields_ = [("client", ctypes.c_ubyte), ("port", ctypes.c_ubyte)]


class SeqEvent(ctypes.Structure):
    """snd_seq_event_t: 16-byte header plus a 12-byte data union."""
    _fields_ = [
        ("type", ctypes.c_ubyte),
        ("flags", ctypes.c_ubyte),
        ("tag", ctypes.c_byte),
        ("queue", ctypes.c_ubyte),
        ("time", ctypes.c_uint * 2),
        ("source", SeqAddr),
        ("dest", SeqAddr),
        ("data", ctypes.c_ubyte * 12),
    ]


def handshake(h):
    """Ploytec vendor sequence on EP0. Without it the device stays mute."""
    h.controlRead(0xC0, 0x56, 0, 0, 15, 2000)          # firmware
    h.controlRead(0xC0, 0x49, 0, 0, 1, 2000)           # status
    h.controlRead(0xA2, 0x81, 0x0100, 0, 3, 2000)      # get sample rate
    for w_index in (0x0086, 0x0004):                   # 0x0004 is V7-specific
        try:
            h.controlWrite(0x22, 0x01, 0x0100, w_index, RATE_44100, 2000)
        except usb1.USBError:
            pass
    h.controlRead(0xA2, 0x81, 0x0100, 0, 3, 2000)
    status = h.controlRead(0xC0, 0x49, 0, 0, 1, 2000)[0]
    confirm = (status | 0x20) & 0xFF                   # set bit 5, write back
    if confirm & 0x80:
        confirm = (confirm - 0x100) & 0xFFFF
    h.controlWrite(0x40, 0x49, confirm, 0, b"", 2000)
    return status


class SeqPort:
    """Publishes the MIDI port and turns raw bytes into sequencer events."""

    def __init__(self, port_name):
        self.alsa = ctypes.CDLL("libasound.so.2")
        self.seq = ctypes.c_void_p()
        if self.alsa.snd_seq_open(ctypes.byref(self.seq), b"default",
                                  SND_SEQ_OPEN_DUPLEX, 0) < 0:
            sys.exit("snd_seq_open failed")
        self.alsa.snd_seq_set_client_name(self.seq, CLIENT_NAME.encode())

        name = port_name.encode()
        self.port_in = self.alsa.snd_seq_create_simple_port(
            self.seq, name, CAP_READ | CAP_SUBS_READ, TYPE_MIDI_GENERIC)
        self.port_out = self.alsa.snd_seq_create_simple_port(
            self.seq, name, CAP_WRITE | CAP_SUBS_WRITE, TYPE_MIDI_GENERIC)
        if self.port_in < 0 or self.port_out < 0:
            sys.exit("snd_seq_create_simple_port failed")

        self.client = self.alsa.snd_seq_client_id(self.seq)
        self.encoder = ctypes.c_void_p()
        self.decoder = ctypes.c_void_p()
        for parser in (self.encoder, self.decoder):
            if self.alsa.snd_midi_event_new(4096, ctypes.byref(parser)) < 0:
                sys.exit("snd_midi_event_new failed")
            self.alsa.snd_midi_event_no_status(parser, 1)
        self.ev = SeqEvent()
        self.outbox = collections.deque()      # raw MIDI bytes headed for the V7
        self.events = 0                        # sequencer events emitted
        self.dropped = 0                       # events the subscriber refused
        self.warned = False
        self.decode_buf = ctypes.create_string_buffer(4096)

    def feed(self, data):
        """Bytes off bulk IN 0x83 -> sequencer events for whoever subscribed.

        Every packet ends with a trailer byte that is not MIDI: measured over
        4896 consecutive 42-byte packets, the last column was 0x00 every single
        time while columns 15..40 were 0xFD idle. Forwarding it corrupts any
        message that straddles a packet boundary, and lands a spurious 0x00
        inside every SysEx reply.
        """
        if data[-1] != 0x00 and not self.warned:
            self.warned = True
            print("v7d: packet trailer is not 0x00 — the framing assumption in "
                  "feed() is wrong, MIDI will be corrupt", file=sys.stderr,
                  flush=True)
        for byte in data[:-1]:
            if byte == MIDI_IDLE:
                continue
            ctypes.memset(ctypes.byref(self.ev), 0, ctypes.sizeof(SeqEvent))
            if self.alsa.snd_midi_event_encode_byte(
                    self.encoder, byte, ctypes.byref(self.ev)) != 1:
                continue                        # message not complete yet
            self.ev.source.client = self.client
            self.ev.source.port = self.port_in
            self.ev.dest.client, self.ev.dest.port = ADDR_SUBSCRIBERS, ADDR_UNKNOWN
            self.ev.queue = QUEUE_DIRECT
            if self.alsa.snd_seq_event_output_direct(
                    self.seq, ctypes.byref(self.ev)) < 0:
                self.dropped += 1              # subscriber's input pool is full
            self.events += 1
        self.alsa.snd_seq_drain_output(self.seq)

    def read_loop(self):
        """Events VDJ writes to us -> raw MIDI bytes in self.outbox.

        Runs in its own thread: snd_seq_event_input blocks, and the USB event
        loop in main() must keep the iso pump fed regardless.
        """
        ev_p = ctypes.POINTER(SeqEvent)()
        while True:
            if self.alsa.snd_seq_event_input(self.seq, ctypes.byref(ev_p)) < 0:
                continue
            n = self.alsa.snd_midi_event_decode(
                self.decoder, self.decode_buf, len(self.decode_buf), ev_p)
            if n > 0:
                data = self.decode_buf.raw[:n]
                if DEBUG:
                    log("VDJ->V7 %3dB %s", n, data[:32].hex(" "))
                self.outbox.append(data)


class CoreMidiPort:
    """The same port on macOS: a CoreMIDI virtual source and destination,
    through python-rtmidi, wearing SeqPort's interface so main() cannot tell.

    CoreMIDI takes whole messages, not bytes, so the running-status stream off
    bulk IN is reassembled here — the V7 omits the status byte on repeats, and
    a parser that forgets that drops every second message of an MSB/LSB pair
    (see CLAUDE.md on decoding traces). What the app writes to the destination
    arrives on CoreMIDI's own thread and is queued for the USB loop to send.
    """

    def __init__(self, port_name):
        import rtmidi
        self.out = rtmidi.MidiOut(rtapi=rtmidi.API_MACOSX_CORE, name=CLIENT_NAME)
        self.out.open_virtual_port(port_name)      # the app reads this
        self.inp = rtmidi.MidiIn(rtapi=rtmidi.API_MACOSX_CORE, name=CLIENT_NAME)
        self.inp.ignore_types(sysex=False, timing=True, active_sense=True)
        self.inp.open_virtual_port(port_name)      # the app writes here
        self.inp.set_callback(self._from_app)
        self.client, self.port_in, self.port_out = "coremidi", port_name, port_name
        self.tap = None                            # v7tc listens here on macOS
        self.outbox = collections.deque()
        self.events = self.dropped = 0
        self.warned = False
        self.status = None                         # running status
        self.buf = bytearray()
        self.sysex = None

    def _from_app(self, event, _):
        message, _delta = event
        if DEBUG:
            log("APP->V7 %3dB %s", len(message), bytes(message[:32]).hex(" "))
        self.outbox.append(bytes(message))

    def _emit(self, message):
        if self.tap is not None and self.tap(message):
            self.events += 1
            return                                 # consumed: the app never sees it
        try:
            self.out.send_message(message)
        except Exception:                          # rtmidi raises on a full queue
            self.dropped += 1
        self.events += 1

    def resync(self):
        """Forget the message in progress. After a bulk IN stall the stream
        resumes mid-message, and the bytes that follow were being framed as
        whatever the last status said: a phantom deck select (`b0 7d 6e`
        from `7d 00 6e 00` minus a byte), a phantom pitch fader. Data bytes
        are dropped until the next status byte, which on this stream is at
        most one message away."""
        self.status = None
        self.buf = bytearray()
        self.sysex = None

    def feed(self, data):
        if data[-1] != 0x00:
            if not self.warned:
                self.warned = True
                print("v7d: packet trailer is not 0x00 — the framing assumption "
                      "in feed() is wrong, MIDI will be corrupt", file=sys.stderr,
                      flush=True)
            self.resync()
        body = data[:-1]
        n = len(body)
        i = 0
        while i < n:
            byte = body[i]
            i += 1
            # Fast path: a whole 3-byte channel message sitting in the packet,
            # which is what the platter stream is (B0 00 pp E0 ll mm, the
            # statuses alternating so running status never applies). One
            # iteration per message instead of per byte: the byte loop below
            # was about a third of a core on the 2015 Mac with the platter
            # turning.
            if (self.sysex is None and 0x80 <= byte < 0xF0
                    and (byte >> 4) not in (0xC, 0xD)
                    and i + 1 < n and body[i] < 0x80 and body[i + 1] < 0x80):
                self.status = byte
                self.buf = bytearray()
                self._emit(bytes(body[i - 1:i + 2]))
                i += 2
                continue
            if byte == MIDI_IDLE or byte >= 0xF8:
                continue
            if self.sysex is not None:
                self.sysex.append(byte)
                if byte == 0xF7:
                    self._emit(bytes(self.sysex))
                    self.sysex = None
                continue
            if byte == 0xF0:
                self.sysex = bytearray([byte])
                continue
            if byte & 0x80:
                self.status = byte
                self.buf = bytearray([byte])
            else:
                if self.status is None:
                    continue                       # data with no status yet
                if not self.buf:
                    self.buf = bytearray([self.status])   # running status
                self.buf.append(byte)
            need = 2 if self.status >> 4 in (0xC, 0xD) else 3
            if len(self.buf) == need:
                self._emit(bytes(self.buf))
                self.buf = bytearray()

    def read_loop(self):
        pass                                       # CoreMIDI delivers by callback


def main(port_name):
    # Realtime scheduling, and with V7_AUDIO it is load-bearing: the event
    # loop below must run every millisecond (iso OUT wants a packet per ms),
    # and a normal-priority Python process takes scheduling hiccups of tens
    # of ms under desktop load. Measured on the audio path: 50-90 ms gouges
    # in the capture FIFO, each one an audible hole. rtprio comes from the
    # audio group (@audio rtprio 95, jackd's limits.d file). Worth carrying
    # to vdj-linux's copy: VDJ's platter timing rides this same loop.
    if sys.platform == "darwin":
        # No SCHED_FIFO here. It became load-bearing with audio, as predicted
        # (2026-09-13, djay Pro open on the 2015 Core M: the pump starved,
        # bulk IN stalled 6764 times, MIDI arrived late, then iso OUT died).
        # QoS USER_INTERACTIVE was tried first and did not hold: late iso
        # resubmits kept failing with LIBUSB_ERROR_OTHER, which is IOKit's
        # IsoTooOld. This is what CoreAudio's IO thread runs under: one pump
        # transfer as the period, a quarter of it as the budget, done within
        # the period, preemptible. Units are mach_absolute_time ticks, 1 ns
        # on Intel Macs. ponytail: on Apple Silicon the tick is 1/24 us; read
        # mach_timebase_info when that Mac exists.
        libc = ctypes.CDLL(None)
        period = ISO_PACKETS // 8 * 1_000_000
        policy = (ctypes.c_uint32 * 4)(period, period // 4, period, 1)
        kr = libc.thread_policy_set(libc.mach_thread_self(), 2, policy, 4)
        if kr != 0:
            print(f"v7d: no realtime thread policy (kern_return {kr}); "
                  "audio may gouge under load", flush=True)
    else:
        try:
            os.sched_setscheduler(0, os.SCHED_FIFO, os.sched_param(10))
        except OSError as e:
            print(f"v7d: no realtime priority ({e}); audio may gouge under load "
                  "(audio group membership needs a reboot on GNOME)", flush=True)

    ctx = usb1.USBContext()
    h = ctx.openByVendorIDAndProductID(VID, PID)
    if h is None:
        sys.exit("Numark V7 not found")
    for iface in (0, 1):
        try:
            if h.kernelDriverActive(iface):
                h.detachKernelDriver(iface)
        except usb1.USBError:
            pass
        h.claimInterface(iface)
        h.setInterfaceAltSetting(iface, 1)
    # A previous run that died mid-stall leaves the bulk pipes halted in the
    # device, and the first submit then fails with LIBUSB_ERROR_PIPE (seen
    # 2026-09-13 on the Mac, right after the starved-pump crash). A clear on
    # a healthy pipe is a legal no-op, so always do it.
    for ep in (BULK_MIDI_IN, BULK_MIDI_OUT):
        try:
            h.clearHalt(ep)
        except usb1.USBError as err:
            print(f"v7d: clearHalt {ep:#x} at startup failed: {err}",
                  file=sys.stderr, flush=True)

    status = handshake(h)
    port = (CoreMidiPort if sys.platform == "darwin" else SeqPort)(port_name)
    threading.Thread(target=port.read_loop, daemon=True).start()
    print(f"{port.client}:{port.port_in} in / {port.client}:{port.port_out} out "
          f"'{port_name}'  (handshake status=0x{status:02X})", flush=True)

    stats = collections.Counter()

    # V7_AUDIO=1 turns the iso OUT pump into an audio sink instead of a stream
    # of zeros: EP 0x02 is the V7's PCM playback, and 0x81 is its rate
    # feedback. All of that lives in v7audio.py, imported lazily, so this file
    # stays close to vdj-linux's copy -- see the module docstring.
    sink = None
    if os.environ.get("V7_TIMECODE"):
        # Mode A without an audio server: the encoder renders straight into
        # the iso OUT body, fed by the platter messages tapped off the port.
        # Same next_ms()/feedback()/report() face as the audio sink below.
        import v7tc
        env = os.environ.get
        sink = v7tc.Timecode(v7tc.pairs_from_env(env("V7_TIMECODE")),
                             port.outbox.append,
                             pitch_range=float(env("V7_TC_RANGE", "0.08")),
                             pitch_center=float(env("V7_TC_CENTER", "8192")),
                             pitch_zero=float(env("V7_TC_ZERO", "0.0005")),
                             level=float(env("V7_TC_LEVEL", "1.0")),
                             log=lambda msg: print(msg, flush=True))
        if hasattr(port, "tap"):
            port.tap = sink.on_midi
        else:
            sys.exit("v7d: V7_TIMECODE needs the CoreMIDI port; on Linux the "
                     "timecode is ./v7dvs on JACK")
    elif os.environ.get("V7_AUDIO"):
        # Degrade to MIDI only rather than dying. The audio path is the one
        # part of this file with a dependency worth missing -- numpy -- and a
        # controller that still works without its sound card beats a daemon
        # that refuses to start. From vdj-linux, where it was learned first.
        try:
            import v7audio
            sink = v7audio.Sink(log=lambda msg: print(msg, flush=True))
        except Exception as e:
            print(f"v7d: no audio ({e}); MIDI only", flush=True)

    # Set on SIGINT/SIGTERM. Every callback checks it before resubmitting,
    # because a cancelled transfer still completes and would otherwise go
    # straight back in flight -- and on macOS libusb will not let the process
    # exit while anything is in flight (measured 2026-09-13: probes that
    # printed DONE stayed alive for minutes, holding the interfaces).
    stopping = [False]
    transfers = []                     # everything long-lived, for the cancel
    # Iso transfers whose resubmit failed, for the main loop to put back. The
    # same rule as bulk IN below: a failed resubmit must never be swallowed.
    # It happened here first (2026-09-13, djay open on the Mac): the starved
    # pump lost its four transfers one by one and the frame counter froze for
    # good while the process stayed alive and looked healthy.
    iso_stalled = []

    def resubmit(transfer):
        try:
            transfer.submit()
        except usb1.USBError as err:
            print(f"iso: resubmit failed ({err}); reviving from the main loop",
                  file=sys.stderr, flush=True)
            iso_stalled.append(transfer)

    def iso_out_cb(transfer):
        if stopping[0]:
            return
        if DEBUG:
            for setup in transfer.getISOSetupList():
                stats["iso_ok" if setup["status"] == 0 else "iso_err"] += 1
        if sink is not None:
            body, lengths = sink.next_ms(ISO_PACKETS // 8)
            transfer.setIsochronous(ISO_OUT, body, callback=iso_out_cb,
                                    iso_transfer_length_list=lengths)
        resubmit(transfer)

    def feedback_cb(transfer):
        """iso IN 0x81: byte 0 is how many frames the V7 consumed last ms."""
        if stopping[0]:
            return
        data = transfer.getBuffer()
        off = 0
        for setup in transfer.getISOSetupList():
            if setup["status"] == 0 and setup["length"]:
                sink.feedback(data[off])
            off += 64
        resubmit(transfer)

    # MIDI out must never block this thread: the iso pump is what keeps the
    # input stream alive, so a synchronous bulkWrite waiting on the V7's
    # 31250-baud UART stalls the platter. One async transfer in flight, and the
    # queue behind it drained through a token bucket — the V7 takes bytes as
    # fast as USB delivers them and then drops whatever its UART cannot clock
    # out, which is how a 186-byte startup burst loses the byte that engages
    # the motor. A plain rate limit is not enough: VDJ decides whether to
    # attach its built-in definition about 5 ms after writing its three SysEx
    # probes, so those 19 bytes have to leave in one piece, immediately. Hence
    # the bucket — full burst when idle, wire speed only under sustained load.
    midi_out = {"inflight": [], "credit": float(MIDI_OUT_BURST),
                "stamp": time.monotonic()}

    def midi_out_cb(transfer):
        try:
            midi_out["inflight"].remove(transfer)
        except ValueError:
            pass
        if transfer.getStatus() != usb1.TRANSFER_COMPLETED:
            try:
                h.clearHalt(BULK_MIDI_OUT)
            except usb1.USBError:
                pass

    def pump_midi_out():
        now = time.monotonic()
        midi_out["credit"] = min(
            float(MIDI_OUT_BURST),
            midi_out["credit"] + (now - midi_out["stamp"]) * MIDI_OUT_BYTES_PER_S)
        midi_out["stamp"] = now
        # One MIDI message per USB transfer: merging several into one transfer
        # loses all but the first as far as the V7 is concerned, which is what
        # kept the motor from engaging. Several may be in flight at once —
        # bulk transfers on one endpoint complete in submission order — because
        # one per 2 ms event-loop tick is too slow for VDJ's probe burst.
        while (port.outbox and midi_out["credit"] >= 3
               and len(midi_out["inflight"]) < MIDI_OUT_INFLIGHT):
            data = port.outbox.popleft()
            midi_out["credit"] -= len(data)
            if trace_out:
                trace_out.write(f"{time.monotonic():.6f},{bytes(data).hex()}\n")
            if DEBUG:
                stats["out_msgs"] += 1
            transfer = h.getTransfer()
            transfer.setBulk(BULK_MIDI_OUT, data, callback=midi_out_cb,
                             timeout=1000)
            midi_out["inflight"].append(transfer)   # keep alive until reaped
            try:
                transfer.submit()
            except usb1.USBError as err:
                midi_out["inflight"].remove(transfer)
                print(f"midi out: {err}", file=sys.stderr, flush=True)
                return

    last_in = [0.0]
    stalled = []                       # bulk IN transfers the main loop must revive
    # Never cleared, unlike stats, which is wiped once a second. "said" and
    # "tick" keep the reporting quiet when nothing is wrong and specific when
    # something is: silence means healthy, any line at all means the pipe is
    # halting, and the count says how badly.
    stall = {"total": 0, "said": 0, "tick": 0.0}
    trace = open(TRACE, "a", buffering=1) if TRACE else None
    trace_out = open(TRACE_OUT, "a", buffering=1) if TRACE_OUT else None

    def bulk_in_cb(transfer):
        if (transfer.getStatus() == usb1.TRANSFER_COMPLETED
                and transfer.getActualLength()):
            if trace:
                data = transfer.getBuffer()[:transfer.getActualLength()]
                trace.write(f"{time.monotonic():.6f},{data.hex()}\n")
            if DEBUG:
                now = time.monotonic()
                gap = now - last_in[0]
                last_in[0] = now
                stats["in_pkts"] += 1
                stats["in_bytes"] += transfer.getActualLength()
                stats["max_gap_ms"] = max(stats["max_gap_ms"], int(gap * 1000))
            port.feed(transfer.getBuffer()[:transfer.getActualLength()])
        if stopping[0]:
            return
        try:
            transfer.submit()
        except usb1.USBError:
            # A resubmit that fails retires this transfer for good, and there
            # are only four: swallowing the error silently kills MIDI input
            # with nothing on stderr. Measured on macOS 2026-08-09 -- the pipe
            # halted, all four transfers died one by one, and the device looked
            # stone dead for three rounds of misdiagnosis. Hand it to the main
            # loop instead; clearHalt is a synchronous control transfer and must
            # not be called from inside a callback, which is already running
            # under handleEvents.
            stalled.append(transfer)

    for _ in range(ISO_INFLIGHT):
        packets = ISO_PACKETS
        transfer = h.getTransfer(iso_packets=packets)
        transfer.setIsochronous(
            ISO_OUT, bytes(ISO_OUT_PKT * packets), callback=iso_out_cb,
            iso_transfer_length_list=[ISO_OUT_PKT] * packets)
        transfer.submit()
        transfers.append(transfer)
    for _ in range(BULK_IN_INFLIGHT):
        transfer = h.getTransfer()
        transfer.setBulk(BULK_MIDI_IN, 512, callback=bulk_in_cb, timeout=0)
        transfer.submit()
        transfers.append(transfer)
    if sink is not None:
        for _ in range(ISO_INFLIGHT):
            transfer = h.getTransfer(iso_packets=ISO_PACKETS)
            transfer.setIsochronous(ISO_IN, 64 * ISO_PACKETS, callback=feedback_cb,
                                    iso_transfer_length_list=[64] * ISO_PACKETS)
            transfer.submit()
            transfers.append(transfer)

    def stop(signum, _frame):
        stopping[0] = True
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    # ponytail: polling the outbox every 2 ms keeps every USB call on one
    # thread, which sidesteps libusb's sync-transfer-vs-event-thread hazard.
    tick = time.monotonic()
    audio_tick = [time.monotonic()]
    while not stopping[0]:
        try:
            ctx.handleEventsTimeout(0.002)
            pump_midi_out()
        except usb1.USBErrorInterrupted:
            continue        # a signal landed mid-poll (EINTR); not a fault
        except usb1.USBError as err:
            sys.exit(f"usb event loop: {err}")
        if stalled:
            # The pipe halted. Clear it once, then put the transfers back --
            # there are only four and losing them is losing MIDI input for
            # good. Report the first one always: this failing invisibly is
            # what made a stalled pipe look like dead hardware.
            try:
                h.clearHalt(BULK_MIDI_IN)
            except usb1.USBError as err:
                print(f"bulk in: clearHalt failed: {err}",
                      file=sys.stderr, flush=True)
            if hasattr(port, "resync"):
                port.resync()
            revived = 0
            for transfer in list(stalled):
                try:
                    transfer.submit()
                    stalled.remove(transfer)
                    revived += 1
                except usb1.USBError:
                    pass                       # try again on the next tick
            if revived and not stall["total"]:
                print(f"bulk in: pipe stalled, {revived} transfer(s) revived. "
                      "A gap over 20 ms freezes the deck, so this is worth "
                      "watching.", file=sys.stderr, flush=True)
            stall["total"] += revived
            stats["stall_recover"] += revived
        if iso_stalled:
            # No clearHalt: iso endpoints do not halt. Just put them back.
            for transfer in list(iso_stalled):
                try:
                    transfer.submit()
                    iso_stalled.remove(transfer)
                    print("iso: transfer revived", file=sys.stderr, flush=True)
                except usb1.USBError:
                    pass                       # try again on the next tick
        # Then keep saying so, but only while it is still happening and only
        # twice a minute. A stall that recurs during a set is the thing to
        # catch; one at startup that never returns is noise.
        if (stall["total"] != stall["said"]
                and time.monotonic() - stall["tick"] >= 30.0):
            stall["tick"] = time.monotonic()
            print(f"bulk in: {stall['total']} stall recoveries so far "
                  f"(+{stall['total'] - stall['said']} in the last 30 s)",
                  file=sys.stderr, flush=True)
            stall["said"] = stall["total"]
        if sink is not None and time.monotonic() - audio_tick[0] >= 5.0:
            # Always report, not just under DEBUG: held and dropped frames are
            # how a rate problem announces itself, and it is inaudible until it
            # is bad. Silence here would mean guessing.
            audio_tick[0] = time.monotonic()
            print(f"{type(sink).__module__}: {sink.report()}", flush=True)
        if DEBUG and time.monotonic() - tick >= 1.0:
            tick = time.monotonic()
            if stats["in_pkts"]:
                log("iso ok=%d err=%d | in %d pkts %d B, max gap %d ms | "
                    "ev %d drop %d | out %d | stall %d",
                    stats["iso_ok"], stats["iso_err"], stats["in_pkts"],
                    stats["in_bytes"], stats["max_gap_ms"], port.events,
                    port.dropped, stats["out_msgs"], stats["stall_recover"])
                port.events = port.dropped = 0
            stats.clear()

    # Cancel everything, wait for the cancellations to complete, then close.
    # Not tidiness: on macOS the process cannot exit otherwise, and the next
    # start finds the interfaces still held (LIBUSB_ERROR_ACCESS).
    for transfer in transfers + midi_out["inflight"] + stalled:
        try:
            transfer.cancel()
        except usb1.USBError:
            pass
    deadline = time.monotonic() + 1.0
    while (time.monotonic() < deadline
           and any(t.isSubmitted() for t in transfers + midi_out["inflight"])):
        try:
            ctx.handleEventsTimeout(0.05)
        except usb1.USBError:
            break
    left = sum(t.isSubmitted() for t in transfers + midi_out["inflight"])
    for iface in (0, 1):
        try:
            h.releaseInterface(iface)
        except usb1.USBError:
            pass
    h.close()
    print(f"v7d: stopped" + (f" ({left} transfer(s) would not cancel)" if left else ""),
          flush=True)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else DEFAULT_PORT_NAME)
