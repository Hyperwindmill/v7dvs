# Changelog

The version lives in `VERSION` and in the annotated git tag, nowhere else.
Both are written by `/bump`, together with the section below — a release is one
operation, so a tag without its entry here cannot happen.

There is deliberately no `Unreleased` section: it is a line to remember at every
commit, which is exactly the kind of line that rots. The section is written at
bump time from `git log <last tag>..HEAD`.

Under 1.0 a closed roadmap step is a minor, everything else is a patch, and a
break in the config contract is a minor with `BREAKING` on top. After 1.0 —
which is when the roadmap ends — that break becomes a major.

## [0.7.0] — 2026-10-08

The Mac app for people who did not build it: one zip, nothing to install,
Intel and Apple Silicon.

### Added

- **`v7dvs-<version>-macos.zip` on the release.** `make app` on a Mac now
  builds a real `.app` with py2app: python.org's Python and its Tk relocated
  inside the bundle, `v7d` as a resource, the app's own name in the Dock,
  signed ad-hoc and zipped. Proven on the author's MacBook with the system's
  Python framework renamed away, then mode C under VirtualDJ and mode A under
  Mixxx from the bundle. Built for Intel; Apple Silicon runs it under Rosetta.
  The shell stub and the hand-written plist of the previous bundle are gone.
- **`README-macos.md`**, inside the bundle and attached to the release: the
  first launch (right-click → Open, or "Open Anyway" on Sequoia, Rosetta on
  Apple Silicon), the two modes macOS has and what each needs, where the log
  is, and what to send with a report.
- The release operation (`/bump`) now ends with the Mac build and its upload,
  so a release without the zip says so instead of silently lacking it.

### Changed

- **`launcher.py` knows when it runs frozen**: it finds `v7d` and `VERSION` in
  the bundle's `Contents/Resources`, sends its output to
  `~/Library/Logs/v7dvs.log` because there is no terminal, and opens the
  settings window when started with no arguments, which is what Finder does.
  On Linux nothing changes; the self-test covers both branches.

## [0.6.0] — 2026-10-08

External timecode on a Mac under a DJ app, which slid or dipped every couple
of seconds on 0.5.0, now decodes like Linux: pitch 0.9968-1.0038 off the
interface with Mixxx running, position steps 99-101 against 95-144 before.
Three causes, all in how the platter stream was read, all invisible on a
host that never delivers a packet late.

### Changed

- **The platter model reads the V7 as it actually behaves.** The V7 stamps
  each tick on a clock that wraps every 5.8 ms, and a loaded Mac hands us
  packets 6-12 ms late with the delay changing from one to the next, so the
  arrival clock alone picked the wrong wrap and read a steady 1.0 as 0.14.
  The arrival clock now only bounds the candidates; among them the wrap
  nearest in time to what the counts take at the current speed wins, with
  the device's own best-case offset keeping its timeline honest. The speed
  is the slope of position over 12 ms of device time rather than the last
  tick's delta, which the sink sampled every 4 ms and so weighed a single
  coalesced tick after a read gap as a 100-200 ms slide. A tick that would
  reverse a platter turning above half speed is not believed (the V7 sends
  one to put its position right after a gap), and silence counts as a stop
  after 35 ms instead of 20, so an allowed delay does not read as one.
  Replayed on the worst trace taken under VirtualDJ: speed 0.37-1.37 with a
  mean error of 0.0098, against 0.10-7.8 and 627 excursions in 22 s before.

### Fixed

- **A non-zero trailer byte no longer resyncs the parser.** Once VirtualDJ
  has sent the V7 its SysEx init, the last byte of every bulk IN packet is
  a sweeping status byte instead of `0x00`; the resync on each of them
  dropped every pitch-bend split across two packets, 20-35 ms without a tick
  at a time, which the sink read as a stopped platter twice a second.
- **A halted capture drain is revived from the main loop**, like the MIDI
  pipe, instead of staying dead for the rest of the run.
- **`make` on macOS skips `linux/v7dvs.c`**, which needs ALSA and JACK.

### Added

- **`make tracereplay`**: replays a `v7d --trace` file through the real
  parser and the platter model, sampling the speed as the sink does, and
  prints the range, the excursions and the mean error. With `tcdec` on an
  interface capture it is how the three causes above were told apart.
- **`--debug` probes the selected deck**: it says when the platter went
  idle and how long since the last tick, and every speed jump between two
  renders with the raw value behind it.
- The roadmap's start half of step 8 is a wizard that asks which software
  and how, writing the same config; `CLAUDE.md` records the V7's coalesced
  ticks, its stamp jitter, the trailer under VirtualDJ and the Mac's
  delivery latency as measured facts.

## [0.5.0] — 2026-10-08

The second half of the VirtualDJ story, found the same night as the first
with the same instrument, and one new value in the config.

### Added

- **`none` for an RCA pair**: that pair carries nothing. Both pairs on `none`
  is the V7 as a MIDI controller alone, `v7d` with no audio path, on any OS.
  On Linux this is new: `output` means the app's audio on the RCAs through
  the loopback, and there was no way to ask for the port without it. `none`
  beside timecode is mode A with that pair silent; `none` beside `output` is
  refused, because the loopback carries all four channels. The settings
  window offers it.

### Fixed

- **The V7's motor cut out by itself.** Under VirtualDJ, after some minutes
  of launching tracks with a hotcue and scratching, the platter died under
  the hand and stayed dead: the deck went silent, VDJ still thought it was
  playing, one play press paused and the second restarted the motor. Every
  measurement of our side was clean, the stream full to the last second, no
  stall, nothing dropped, no motor command from VDJ anywhere near. Numark's
  own driver writes every MIDI message to the V7 as a 42-byte packet, the
  message first, `0xFD` padding, `0xE0` last; we wrote bare messages, which
  the V7 accepts and acts on for minutes. Bisected the same night with
  everything else held equal: bare, two cutouts in 138 s; framed, 244 s
  clean over more tracks than any failing run. `v7d` now frames every
  write, and also does the three other things the driver does that it did
  not: it drains the capture endpoint continuously (2.85 MB/s of zeros the
  driver always reads), cycles the USB configuration at open, and sets the
  sample rate on the endpoints the driver sets it on.

## [0.4.0] — 2026-10-08

The USB layer in C, on Linux and macOS, and the end of a bug that looked like
VirtualDJ's for two months. The config contract is unchanged.

### Added

- **`v7d`, one binary for everything the V7 needs over USB**: the Ploytec
  handshake, the MIDI port (ALSA sequencer on Linux, a CoreMIDI source on
  macOS) and mode A, the Serato timecode rendered straight into the V7's own
  playback stream, following its rate feedback. No JACK, no loopback, no
  audio server: the launcher starts `./v7d` alone for mode A, and the e2e
  test waits on its port instead of on the graph. Measured on Linux at 2-3%
  CPU for 1000 USB packets and 2000 MIDI messages a second, zero stall
  recoveries over six minutes, and the analog loop decoded by xwax's own
  code with a worst pitch error of 0.0025. The packages ship it and depend on
  libusb.
- **macOS**: `v7d` builds and runs on Big Sur with a static libusb, the event
  loop under the Mach time-constraint policy, and the pump at 4 × 32 as
  measured on a 2015 MacBook. `make app` builds `v7dvs.app` with the settings
  window and a Stop window while the rig runs. Mode A was measured off the
  RCAs into a DDI and decoded; Mixxx ran under it.
- **Mode C on macOS**: both RCA pairs on `output` starts `v7d` MIDI-only under
  the port name VirtualDJ matches, and VirtualDJ for Mac drives the platter,
  the pitch fader and the cues natively. The V7's vendor driver does not run
  on a current macOS; this is the missing piece.
- The bulk IN MIDI parser moved into the portable core (`src/midiparse.c`)
  with its own tests, whole channel messages per iteration.
- `dev/windows/`: a decoder for VirtualDJ's `midilog.dat` and two scanners for
  a USBPcap capture, plain Python, no tshark. `dev/windows-oracle.md` is the
  session they were written for.

### Fixed

- **The V7 under VirtualDJ: hotcues that retriggered once a second, a held
  platter that never resumed, decks that froze for 2-3 s.** Every measurement
  of the MIDI path was clean and the symptoms were absent on Windows, so for
  two months this read as VirtualDJ's rule. A USB capture of Numark's own
  driver showed the difference: it feeds the V7's playback stream at the
  rate the device reports, always, while a MIDI-only `v7d` pushed a full
  packet every microframe, 2.36 times what the V7 consumes, and never read
  the feedback. Now every mode sizes the stream from the feedback. Same
  gestures under VirtualDJ the same night: 21 of 21 hotcues, hold resumed,
  nothing stopped in 341 s.
- **The record ran out after 5m53s.** The needle sat where it was dropped
  and the encoder clamped at the end of the timecode, which Mixxx read as
  losing vinyl control. The needle is now lifted short of either end and
  dropped near the other, and playback starts near the beginning, so the
  first position jump lands past eleven minutes.
- A failed transfer resubmit is never swallowed: a halted bulk pipe is
  cleared from the main loop, the parser resyncs after it, and the pipes are
  cleared at startup after a run that died mid-stall. A system common byte
  no longer gets framed as a channel message.
- Timecode mode keeps the platter, its timestamp and the pitch fader off the
  MIDI port: a turntable sends none, the fader is already in the timecode,
  and forwarding them cost 2000 sends a second.
- The launcher notices a `v7d` that died at startup instead of waiting
  forever, and stops the motors on its way out.

### Changed

- `usb/v7tc.py`, mode A in Python, is superseded by `v7d` and awaits
  removal; `usb/v7d.py` stays for mode B on Linux.

## [0.3.0] — 2026-09-12

Roadmap step 8, the install half: a settings window and packages for three
distributions. The config contract is unchanged, and a file written by an
older version now loads with the keys it lacks filled in.

### Added

- **A settings window**, `./launcher.py --configure`: one widget per key of
  the config, and Cancel, Save or Save and start. tkinter, so it is standard
  library on every platform this will run on, and no logic of its own — what
  it saves is validated by the same code the launcher runs. The one piece of
  logic, field text back into numbers, is checked by `--selftest` without a
  display.
- **Packages**: `.deb` for Debian and Ubuntu, `.rpm` for Fedora, `.pkg.tar.zst`
  for Arch, all from one description in `packaging/nfpm.yaml` through nfpm's
  own container (`make pkg`). Each installs the binary and the Python side, the
  udev rule, the `snd-aloop` module load, the WirePlumber period for the
  loopback and a **v7dvs** entry in the application menu that opens the
  settings window. They are attached to this release.
- **`make pkg-test`** installs each package in a fresh container of its
  distribution and runs the self-tests, which is how the dependency names
  were proven rather than guessed. Its first run caught two things: Debian's
  `pipewire-jack` keeps `libjack` off the loader path, so the `.deb` also
  depends on `libjack-jackd2-0 | libjack0`; and a failed `pacman` install
  was printing "ok" over "command not found", because the checks were chained
  with `&&` to a `set -e` that never ran. Install and checks are now one
  script, and a bogus install was shown to fail before the green was trusted.
- Installed from a package, the launcher is `v7dvs-launcher` on the path and
  finds the generator on the path too; from a checkout nothing changes.

### Fixed

- A config from before 0.2.0, whose `pitch` block has no `zero`, refused to
  open the settings window with the message `no window ('zero')`. The launcher
  merged the file over its template one level deep only, and a catch-all
  `except` turned the resulting `KeyError` into a riddle. The merge now fills
  `pitch` too, with a self-test for exactly that file shape, and the `except`
  is narrowed to "no display" so the next unexpected error arrives as a
  traceback.

### Changed

- `/bump` now runs `make pkg-test` and creates the GitHub release with the
  three packages attached, so a release stays one operation. The tray icon is
  dropped to optional in the roadmap: a DVS rig is started and stopped
  deliberately, and a window with Save and start does its job.

## [0.2.0] — 2026-09-12

Roadmap step 4 closed, the config gains two keys and a mode, and the ground for
macOS is measured. The config additions are additive: a 0.1.0 file still loads.

### Added

- **The pitch-zero LED**, `B0 37 01` on deck A and `B0 38 01` on deck B, lit
  when the synthesized pitch is within `pitch.zero` of 1.0. It matters more
  than a lamp should: the V7's fader has no centre detent, so the light is the
  only way to find zero by anything other than luck. The tolerance ships at
  0.0005 (0.05% of pitch, about 0.3 mm of travel), bounded by measurement at
  both ends: below 0.028% the fader contributes less error than the motor's own
  fast running, at 0.09% the lamp would claim a zero the deck does not hold.
- **`./launcher.py --calibrate`** learns `pitch.center` from one full sweep of
  the fader, refuses anything under 90% of travel, and declines to save a
  correction smaller than the fader's own dither. On the hardware it measured
  8191 against 8192 theoretical, which is the useful negative result: the
  centre was never wrong, a hand placing a detentless fader was. The LED fixes
  that; the calibration stays to catch a fader that genuinely drifts.
- **`midi_name`** in the config, defaulting to `"DVS Controller"`. The port used
  to announce `"Numark V7 MIDI"`, the name from Ploytec's INF, which is exactly
  what a DJ app can recognise and object to: Serato DJ Pro warns that the V7 is
  unsupported. A project whose thesis is that the app cannot tell what it is
  talking to now introduces itself by nothing.
- **The passthrough mode**, which is the absence of timecode rather than a
  switch: both RCA pairs on `output` links nothing and no longer starts the
  generator, and with `midi_name` set to `"Numark V7 MIDI"` VirtualDJ attaches
  its own built-in V7 definition and drives the deck natively. Proven on Linux;
  it is roadmap step 6b on macOS.
- **macOS probes** in `dev/mac/`, with their report: the USB layer is proven on
  a 2015 Intel MacBook (device claimed, handshake, identity, MIDI in while the
  platter spins), and the one macOS-specific fault was measured rather than
  patched. Bulk IN halts when iso OUT is starved, and the cure is pump geometry:
  4 transfers of 32 packets, 16 ms of queue, ran zero to three recoverable
  stalls in five seconds where every 8 ms shape stalled, from 5 to 117 times.
  The shape is recorded in
  `ISO_INFLIGHT`/`ISO_PACKETS` and switches when the macOS backend exists and
  can be tuned against the real sink. BlackHole passed the half of its gate that
  could have killed the virtual mode: 16 channels each way and 44100 offered.
- `v7d.py` starts without numpy, degrading to MIDI only instead of refusing;
  an `EINTR` during a poll is retried instead of killing the daemon; and stall
  recovery reports once immediately, then every 30 s only while it recurs.

### Fixed

- **Deck B could inherit "backwards" from whatever ran before us.** The state
  block's deck stride is +8, not the motor's +0x0a: startup was zeroing `0x50`,
  which nothing has identified, and leaving deck B's real direction latch at
  `0x4e` untouched. Measured from the V7's own re-announcement on deck
  selection. Deck A had been protected since 0.1.0; deck B was not.
- The default config brought up a generator hunting for a port nobody
  publishes, because the launcher renamed v7d's port when `midi_name` became
  opt-in and never told the C side. `v7dvs` now takes `--port`, and the
  selftest reads `usb/v7d.py` and fails if the two names drift apart.

### Changed

- The pitch fader was already folded into the timecode in 0.1.0; what is new
  is the understanding that the V7 keeps each deck's pitch itself and applies
  the shared fader as a relative offset, so the takeover a shared fader would
  need is solved in the device and nothing is implemented for it.
- Documentation brought up to where the work is: the README's status, config
  and layout, roadmap step 5 stating what is measured on macOS and what is
  left (a `CoreMIDI` virtual source), a Help wanted section, and the macOS
  probe instructions marked done.

## [0.1.0] — 2026-08-09

First tagged version. Everything below was measured, not assumed.

### Added

- The portable core: `src/encoder.c` synthesizes Serato timecode, `src/platter.c`
  turns the V7's platter messages into a speed, both plain C with no system
  dependencies.
- `linux/v7dvs.c`: ALSA seq in, JACK out, connecting to nothing on purpose — the
  DJ app must not know what it is talking to.
- `usb/v7d.py` and `usb/v7audio.py`: the V7's USB protocol and its sound card,
  the public home of that layer.
- The frozen config contract (`rca` per RCA pair, `level`, `counts_per_rev`) and
  `launcher.py` as its only reader; the C side keeps plain flags.
- The pitch fader folded into the synthesized speed
  (`timecode_speed = platter_speed × (1 + pitch)`), measured at +8.05% / −8.09%
  against ±8% nominal. It is CC `0x04`/`0x24`, not what the upstream notes said.
- `tests/tcgen.c` and `tests/tcdec.c` — the encoder checked against xwax's own
  decoder rather than against our idea of it — plus `check.sh`.
- `tests/e2e-xwax.sh`, both delivery modes (analog and `--v7audio`), refusing to
  start xwax unless JACK is already at 44100.
- `ROADMAP.md`, seven steps each finished by a measurement.
