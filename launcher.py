#!/usr/bin/env python3
"""
One command for the whole rig, driven by one config file.

    ./launcher.py              start it
    ./launcher.py --print      show what it would do, run nothing
    ./launcher.py --selftest   check the config translation, start nothing
    ./launcher.py --calibrate  learn pitch.center from one sweep of the fader
    ./launcher.py --configure  edit the config in a window, then start or not
    ./launcher.py --config F   drive it from F instead of the usual config
    ./launcher.py --version    print the version and exit

The config lives in ~/.config/v7dvs/config.json and is written as a template on
first run. It is the contract: everything here reads it, nothing here decides
policy of its own, and the day there is a GUI it edits this file and nothing
else changes.

    {
      "rca": { "pair1": "timecode:A", "pair2": "timecode:B" },
      "level": 0.5,
      "counts_per_rev": 3600,
      "midi_name": "DVS Controller"
    }

`rca` says what each of the V7's two RCA pairs carries: "timecode:A",
"timecode:B", or "output" — which means we link nothing, leaving those channels
for whatever you point at the loopback sink as an ordinary output device. Two
timecode pairs is two virtual turntables into one 4-input interface.

`midi_name` is what the DJ app sees on the MIDI port, and it defaults to a name
that identifies nothing on purpose. Set it to "Numark V7 MIDI" and VirtualDJ
attaches its own built-in definition and drives the deck itself — the
passthrough mode, which needs no timecode and is therefore configured by setting
both pairs to "output". Leave it alone and the app sees an unnamed controller
you map by hand.

It is opt-in because a name an app recognises is a name an app can object to:
Serato DJ Pro warns that the Numark V7 is no longer supported, and in a DVS rig
Serato should be looking at a turntable and an anonymous controller, never at a
V7. Setting both keys is deliberate in a way that a default could not be.

`pitch.center` is where the fader physically rests at zero, and `--calibrate`
measures it instead of asking: run the fader end to end once and the midpoint of
the observed travel is the answer. Returning it to centre by eye left it 0.9% of
travel off, which is 0.15% of pitch, which is a beat of drift every eleven
minutes — so this is the difference between beatmatching that holds and
beatmatching that wanders.

`pitch.zero` is how close to that centre lights the V7's pitch-zero LED, in
pitch rather than counts. This fader has no detent, so the lamp is the only way
to find zero by anything other than luck. Wider is easier to hit and less true:
0.028% is the motor's own error, 0.05% is 0.3 mm of travel and a beat of drift in
seventeen minutes, 0.09% is where the light starts claiming a zero the deck does
not hold.

`level` is a calibration knob, not a preference. Measured 2026-08-08: the sink
at 1.0 clips the timecode to 0 dBFS, and a clipped timecode still locks pitch
while silently destroying absolute position, because the LFSR bitstream lives in
the peak amplitudes. 0.5 measures about -5 dBFS with no clipped samples.

Start order is forced, not chosen: v7d must open the aloop cable first, because
an aloop takes its rate from whoever opens it first and that is what pins it at
44100 and makes PipeWire do the 48000->44100 conversion.

With every pair on timecode (mode A) none of that applies: ./v7d, the C daemon,
renders the timecode into the V7's own outputs and the launcher only starts it.
"""

import json
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time

# Safe without a display: configwin imports tkinter inside its functions.
import configwin

def resource_dir():
    """Where v7d, VERSION and configwin live.

    In a checkout or a Linux package: next to this file (realpath, not
    abspath: installed, this file is reached through the /usr/bin/v7dvs-launcher
    symlink and its siblings live where the file is). Inside the Mac app:
    Contents/Resources, which py2app's bootstrap names in RESOURCEPATH while
    sys.frozen is set.
    """
    if getattr(sys, "frozen", False) and os.environ.get("RESOURCEPATH"):
        return os.environ["RESOURCEPATH"]
    return os.path.dirname(os.path.realpath(__file__))


HERE = resource_dir()
# The generator sits next to this file in a checkout and on PATH when
# installed from a package.
V7DVS_BIN = (os.path.join(HERE, "v7dvs") if os.path.exists(os.path.join(HERE, "v7dvs"))
             else "v7dvs")
V7D_BIN = (os.path.join(HERE, "v7d") if os.path.exists(os.path.join(HERE, "v7d"))
           else "v7d")

# One number for the whole repo, in one file, confirmed by the git tag. The C
# side gets it through -DV7DVS_VERSION in the Makefile.
with open(os.path.join(HERE, "VERSION")) as f:
    VERSION = f.read().strip()

# --config lets a test drive the rig from a config of its own without touching
# the one you actually use. tests/e2e-xwax.sh is the reason it exists.
CONFIG = os.path.expanduser("~/.config/v7dvs/config.json")
if "--config" in sys.argv:
    CONFIG = sys.argv[sys.argv.index("--config") + 1]
ALOOP = "alsa_output.platform-snd_aloop.0.analog-surround-40"
RATE = 44100

# PipeWire's own node names, never the localised descriptions: `pw-link` takes
# these, while JACK port names come out translated ("Stereo analogico"), so a
# config written under one locale would name ports that do not exist under
# another. This is the one reason to prefer pw-link over jack_connect here.
PAIR_PORTS = {"pair1": ("playback_FL", "playback_FR"),
              "pair2": ("playback_RL", "playback_RR")}

# Spelled out rather than imported: importing usb/v7d.py would pull in libusb
# for a string. It has to match DEFAULT/V7_PORT_NAME there, and the selftest
# checks that it still does.
V7_PORT_NAME = "Numark V7 MIDI"

# The ALSA sequencer tools exist only on Linux. Without them the MIDI port
# cannot be probed, so alsa_port_up() answers "no" and the mode A start trusts
# a v7d that has stayed alive for a moment instead -- see the start path.
HAVE_ASEQ = shutil.which("aconnect") is not None

# The pitch fader, 14-bit, as CC pairs: MSB, then LSB 0x20 higher, and one
# number per deck because the V7 has one physical fader that reports on the
# selected deck's controller. See linux/v7dvs.c for how it was measured.
PITCH_MSB = (0x04, 0x05)
PITCH_LSB = (0x24, 0x25)
PITCH_MAX = 16383

# The fader's LSB dithers by ±1 while it sits still — it is an analog ADC. So one
# count is the resolution of the instrument, and a "correction" that small is
# noise wearing a measurement's clothes.
PITCH_DITHER = 1

# `midi_name` is the one key that changes what the DJ app is looking at, so it
# defaults to a name that identifies nothing. Set it to "Numark V7 MIDI" and
# VirtualDJ attaches its built-in definition and drives the deck itself, which
# is the passthrough mode; leave it alone and the app sees an unnamed
# controller, which is what every DVS mode here wants. Opt-in, because a name
# an app recognises is a name an app can object to -- Serato DJ Pro warns that
# the V7 is unsupported, and in DVS mode it should not be seeing a V7 at all.
TEMPLATE = {"rca": {"pair1": "timecode:A", "pair2": "timecode:B"},
            "level": 0.5,
            "counts_per_rev": 3600,
            "midi_name": "DVS Controller",
            "pitch": {"range": 0.08, "center": 8192, "zero": 0.0005}}


def load():
    """The config, creating the template on first run."""
    if not os.path.exists(CONFIG):
        os.makedirs(os.path.dirname(CONFIG), exist_ok=True)
        with open(CONFIG, "w") as f:
            json.dump(TEMPLATE, f, indent=2)
            f.write("\n")
        print(f"wrote a template to {CONFIG}")
    with open(CONFIG) as f:
        return merge(json.load(f))


def merge(raw):
    """The file over the template, one level into `pitch` as well.

    A config written before a key existed must still load: `pitch.zero` joined
    in step 4, and a shallow merge left every earlier file without it, which
    surfaced as the settings window refusing to open.
    """
    return {**TEMPLATE, **raw,
            "pitch": {**TEMPLATE["pitch"], **raw.get("pitch", {})}}


def links(cfg):
    """[(source port, destination port)] for the configured RCA pairs.

    Pure translation, no processes touched — which is what --selftest checks.
    Raises on an unknown source, because a typo that silently produced no links
    would look exactly like a dead rig.
    """
    out = []
    for pair, (left, right) in sorted(PAIR_PORTS.items()):
        what = cfg["rca"].get(pair, "output")
        if what in ("output", "none"):
            continue
        if not what.startswith("timecode:") or what[9:] not in ("A", "B"):
            raise ValueError(f"{pair}: expected timecode:A, timecode:B, output "
                             f"or none, got {what!r}")
        deck = what[9:]
        out.append((f"v7dvs:timecode_{deck}_L", f"{ALOOP}:{left}"))
        out.append((f"v7dvs:timecode_{deck}_R", f"{ALOOP}:{right}"))
    return out


def v7dvs_args(cfg):
    """Config -> argv for the C side, which never learns a config file exists."""
    pitch = {**TEMPLATE["pitch"], **cfg.get("pitch", {})}
    # --port is not optional even though the C side has a default: `midi_name`
    # renames v7d's port, and since that name stopped being the V7's own the two
    # sides no longer agree by luck. Without it the generator hunts for a port
    # nobody publishes and the rig comes up with a silent platter.
    return ["--port", cfg.get("midi_name", TEMPLATE["midi_name"]),
            "--counts", str(cfg["counts_per_rev"]),
            "--pitch-range", str(pitch["range"]),
            "--pitch-center", str(pitch["center"]),
            "--pitch-zero", str(pitch["zero"])]


def mode_a(cfg):
    """True when every RCA pair carries timecode: then ./v7d alone is the rig.

    No JACK, no loopback, no graph rate to force. A pair on "output" carries
    the app's audio, which needs the snd-aloop chain, so that config keeps the
    old start order below.
    """
    return all(cfg["rca"].get(p, "output").startswith("timecode:")
               for p in ("pair1", "pair2"))


def mode_c(cfg):
    """True when no RCA pair carries timecode: the passthrough, VirtualDJ's
    mode, where the app drives the V7 itself through the named MIDI port.

    On Linux the RCAs then carry the app's audio through snd-aloop. On macOS
    ./v7d alone publishes the port and the RCAs stay silent until mode B has a
    Mac side -- measured 2026-09-19: VDJ for Mac matches the CoreMIDI port by
    name and drives platter, pitch fader and cues through it.
    """
    return all(cfg["rca"].get(p, "output") == "output"
               for p in ("pair1", "pair2"))


def solo(cfg, platform=sys.platform):
    """True when this platform runs ./v7d alone for this config: no pair on
    output -- timecode and none only, so no app audio into the V7 -- on every
    platform, and on macOS the passthrough too.

    "none" is a pair that carries nothing: both on none is the V7 as a MIDI
    controller with its RCAs silent, which is what VirtualDJ gets on macOS
    already and what Linux had no way to ask for (2026-10-08). It is also the
    controlled experiment: the C daemon under VDJ with no Python and no
    loopback in the picture.
    """
    outputs = [cfg["rca"].get(p, "output") == "output" for p in ("pair1", "pair2")]
    return not any(outputs) or (platform == "darwin" and all(outputs))


def validate(cfg, platform=sys.platform):
    """links(), plus what this platform can run. Raises ValueError like links().

    macOS runs ./v7d alone: every pair on timecode (mode A) or every pair on
    output (mode C, MIDI only). A pair carrying the app's audio beside one
    carrying timecode needs the app's audio into the V7, which is PipeWire and
    snd-aloop here and has no Mac side yet (ROADMAP step 6). Refusing here
    means the window refuses too, since it is handed this function.
    """
    if platform == "darwin" and not solo(cfg, platform):
        raise ValueError("on macOS both RCA pairs must carry timecode, or both be "
                         "output: a pair carrying the app's audio has no Mac side yet")
    values = [cfg["rca"].get(p, "output") for p in ("pair1", "pair2")]
    if "none" in values and "output" in values:
        # The app's audio reaches the V7 as one 4-channel stream through
        # snd-aloop: a pair cannot be left out of it without a mixer.
        raise ValueError("a pair on none beside a pair on output is not "
                         "supported: the app's audio would reach both")
    return links(cfg)


def v7d_args(cfg):
    """Config -> argv for ./v7d, the C daemon, which never learns a config exists."""
    pitch = {**TEMPLATE["pitch"], **cfg.get("pitch", {})}
    pairs = []
    for pair in ("pair1", "pair2"):
        what = cfg["rca"].get(pair, "output")
        pairs.append(what[9:] if what.startswith("timecode:") else "-")
    args = ["--port", cfg.get("midi_name", TEMPLATE["midi_name"])]
    if any(p != "-" for p in pairs):
        args += ["--timecode", ",".join(pairs)]
    return args + ["--counts", str(cfg["counts_per_rev"]),
                   "--range", str(pitch["range"]),
                   "--center", str(pitch["center"]),
                   "--zero", str(pitch["zero"])]


def run(*argv, check=False):
    return subprocess.run(argv, capture_output=True, text=True,
                          check=check).stdout


def node_id(name):
    """PipeWire object id for a node name. None if it is not there."""
    for obj in json.loads(run("pw-dump") or "[]"):
        if obj.get("info", {}).get("props", {}).get("node.name") == name:
            return obj["id"]
    return None


def alsa_port_up(name):
    return HAVE_ASEQ and name in run("aconnect", "-l")


def seq_addr(port_name, listing=None):
    """"client:port" of the readable sequencer port called `port_name`, or None.

    By number, never by name. `aseqdump -p` takes a comma-separated *list* of
    addresses and v7d's client name has a comma in it, so handing over the name
    resolves half of it and calls the rest an invalid port. It does not resolve
    port names at all — only client names or numbers.

    `aconnect -i` lists just the readable ports, which is the side to subscribe
    to. Its first word is localised ("cliente" here), so match on the number.
    """
    client = None
    for line in (run("aconnect", "-i") if listing is None
                 else listing).splitlines():
        head = re.match(r"\S+ (\d+):", line)
        if head:
            client = head.group(1)
            continue
        port = re.match(r"\s+(\d+) '(.*)'", line)
        if port and client and port.group(2).strip() == port_name:
            return f"{client}:{port.group(1)}"
    return None


def wait_for(cond, what, timeout=10.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if cond():
            return True
        time.sleep(0.2)
    print(f"launcher: timed out waiting for {what}", file=sys.stderr)
    return False


def pitch_span(lines):
    """(lo, hi) of the 14-bit fader value over aseqdump lines, None if it never
    spoke.

    Only the LSB completes a value. The V7 sends MSB then LSB, so computing on
    the MSB pairs it with the *previous* LSB and puts a transient of up to 127
    counts into the extremes — 0.8% of travel, which is the same order as the
    error being calibrated away. Reading on the LSB has no such window.
    """
    msb = lo = hi = None
    for line in lines:
        if "Control change" not in line:
            continue
        try:
            ctrl = int(line.split("controller")[1].split(",")[0])
            value = int(line.rsplit("value", 1)[1])
        except (IndexError, ValueError):
            continue
        if ctrl in PITCH_MSB:
            msb = value
        elif ctrl in PITCH_LSB and msb is not None:
            raw = (msb << 7) | value
            lo = raw if lo is None else min(lo, raw)
            hi = raw if hi is None else max(hi, raw)
    return None if lo is None else (lo, hi)


def calibrate(cfg):
    """Learn `pitch.center` from one full sweep of the fader and save it.

    Guided rather than configured: the measurement that made this a knob also
    showed the estimator is trivial — the midpoint of one sweep's travel came
    out 8191 against 8192 theoretical, while placing the fader by eye was off by
    0.9% of travel. Asking a human to discover a number they can only get wrong
    is worse than watching them move the thing once.
    """
    addr = seq_addr(cfg["midi_name"])
    if addr is None:
        print(f"launcher: {cfg['midi_name']!r} is not up — start the rig (or "
              f"usb/v7d.py) first, this reads the fader off its MIDI port",
              file=sys.stderr)
        return 1

    # LC_ALL=C because alsa-utils is localised: `aconnect` prints "cliente" here,
    # so the English strings this parses are not a given.
    dump = subprocess.Popen(["aseqdump", "-p", addr],
                            stdout=subprocess.PIPE, text=True,
                            env={**os.environ, "LC_ALL": "C"})
    lines = []
    threading.Thread(target=lambda: lines.extend(dump.stdout),
                     daemon=True).start()
    try:
        print("Run the pitch fader all the way to one end, then all the way to "
              "the other.\nLeave it at centre when you are done, then press "
              "Enter.")
        input()
    finally:
        dump.terminate()

    # A reader that died must not read as a fader that never moved: it already
    # did once, when aseqdump was handed a name it could not resolve and the
    # only symptom was "the fader never spoke".
    if dump.poll() not in (None, 0, -signal.SIGTERM):
        print(f"launcher: aseqdump exited {dump.poll()} (its error is above); "
              f"nothing was read from {addr}", file=sys.stderr)
        return 1

    span = pitch_span(lines)
    if span is None:
        print("launcher: the fader never spoke. The V7 reports it only when it "
              "moves, so either it did not move or the deck it belongs to is "
              "not selected.", file=sys.stderr)
        return 1

    lo, hi = span
    # A partial sweep produces a centre that is confidently wrong, which is
    # exactly the failure being fixed. Refuse it rather than save it.
    if hi - lo < PITCH_MAX * 0.9:
        print(f"launcher: only {(hi - lo) / PITCH_MAX:.0%} of the travel was "
              f"seen ({lo}..{hi}). That is not a full sweep — nothing saved.",
              file=sys.stderr)
        return 1

    center = (lo + hi) // 2
    pitch = {**TEMPLATE["pitch"], **cfg.get("pitch", {})}
    was = pitch["center"]
    # The offset in units of pitch is the number that matters: it is what drifts.
    drift = (center - was) / (PITCH_MAX / 2) * pitch["range"]
    print(f"travel {lo}..{hi}, centre {center} (was {was}, "
          f"{drift:+.3%} of nominal pitch)")

    # Measured 2026-08-10 on the real fader: travel 0..16382, midpoint 8191
    # against 8192 theoretical. One count, which is the dither, which is nothing
    # — and it could hardly have been otherwise, because the midpoint of the
    # travel is a property of the ADC and an ADC is symmetric by construction.
    # Saving it anyway would plant a number that looks measured-and-different
    # where the truth is measured-and-identical.
    if abs(center - was) <= PITCH_DITHER:
        print(f"launcher: already centred to within {PITCH_DITHER} count, "
              f"which is the fader's own dither. {was} kept, nothing saved.")
        return 0

    # Read the file again rather than writing `cfg` back: cfg is merged with the
    # template, and saving that would silently plant every default in the user's
    # config as if they had chosen it.
    with open(CONFIG) as f:
        raw = json.load(f)
    raw.setdefault("pitch", {})["center"] = center
    with open(CONFIG, "w") as f:
        json.dump(raw, f, indent=2)
        f.write("\n")
    print(f"launcher: pitch.center = {center} saved to {CONFIG}")
    return 0


def configure(cfg):
    """Open the config window and save what comes back. (cfg, start) or None.

    The window shows every key, so writing the merged config back is honest
    here in a way it is not in --calibrate: each default the file gains is one
    the user saw on screen and pressed Save over.
    """
    try:
        import tkinter
    except ImportError as err:
        print(f"launcher: no window, tkinter is not installed ({err}); "
              f"edit {CONFIG} by hand", file=sys.stderr)
        return None
    try:
        got = configwin.edit(cfg, validate)
    except tkinter.TclError as err:
        print(f"launcher: no window, no display ({err}); edit {CONFIG} by hand",
              file=sys.stderr)
        return None
    if got is not None:
        with open(CONFIG, "w") as f:
            json.dump(got[0], f, indent=2)
            f.write("\n")
        print(f"launcher: saved {CONFIG}")
    return got


def main():
    if getattr(sys, "frozen", False):
        # Inside the Mac app there is no terminal: the log is the only place
        # output can go, so redirect before anything can print or fail. With
        # no arguments (Finder gives none) the window is the entry point.
        logs = os.path.expanduser("~/Library/Logs")
        os.makedirs(logs, exist_ok=True)
        sys.stdout = sys.stderr = open(os.path.join(logs, "v7dvs.log"), "a",
                                       buffering=1)
        print(f"v7dvs {VERSION}: started {time.strftime('%Y-%m-%d %H:%M:%S')}")
        if len(sys.argv) == 1:
            sys.argv.append("--configure")
    if "--version" in sys.argv:
        print(f"v7dvs {VERSION}")
        return 0

    cfg = load()
    window = False
    if "--configure" in sys.argv:
        got = configure(cfg)
        if got is None or not got[1]:
            return 0
        cfg = got[0]
        window = True
    try:
        wanted = validate(cfg)
    except ValueError as err:
        print(f"launcher: {err}", file=sys.stderr)
        return 1

    if "--selftest" in sys.argv:
        assert links({"rca": {"pair1": "timecode:A", "pair2": "output"}}) == [
            ("v7dvs:timecode_A_L", f"{ALOOP}:playback_FL"),
            ("v7dvs:timecode_A_R", f"{ALOOP}:playback_FR")], "pair1 only"
        assert links({"rca": {"pair1": "output", "pair2": "output"}}) == [], \
            "output means link nothing"
        assert len(links(TEMPLATE)) == 4, "both pairs is four links"
        # Frozen (py2app): v7d and VERSION live in the bundle's Resources,
        # which the bootstrap names in RESOURCEPATH. Faked here so the check
        # runs on Linux too; restored so nothing after it sees a frozen run.
        assert resource_dir() == os.path.dirname(os.path.realpath(__file__)), \
            "not frozen: resources sit next to this file"
        sys.frozen = "macosx_app"
        os.environ["RESOURCEPATH"] = "/tmp/v7dvs-selftest-resources"
        try:
            assert resource_dir() == "/tmp/v7dvs-selftest-resources", \
                "frozen: resources are where py2app says"
        finally:
            del sys.frozen
            del os.environ["RESOURCEPATH"]
        assert v7dvs_args(TEMPLATE) == [
            "--port", "DVS Controller", "--counts", "3600",
            "--pitch-range", "0.08", "--pitch-center", "8192",
            "--pitch-zero", "0.0005"], "argv from the template"
        # The generator must be told the port the daemon was told to publish, or
        # renaming one renames only one of them.
        assert v7dvs_args({**TEMPLATE, "midi_name": V7_PORT_NAME})[:2] == \
            ["--port", V7_PORT_NAME], "the port name reaches the C side"
        # A config predating the pitch key must still start, on the defaults.
        assert v7dvs_args({"counts_per_rev": 3600}) == v7dvs_args(TEMPLATE), \
            "a missing pitch block falls back rather than crashing"
        # And a *partial* one, which is what every config written before today
        # is: `load()` merges only the top level, so the nested fallback has to
        # happen here or a missing key becomes a KeyError at startup.
        assert v7dvs_args({**TEMPLATE,
                           "pitch": {"range": 0.08, "center": 8191}}) == [
            "--port", "DVS Controller", "--counts", "3600",
            "--pitch-range", "0.08", "--pitch-center", "8191",
            "--pitch-zero", "0.0005"], "a pitch block missing one key"
        try:
            links({"rca": {"pair1": "timecode:C"}})
        except ValueError:
            pass
        else:
            raise AssertionError("a bad deck must not pass silently")

        # The V7's name is opt-in and must stay that way: a DVS rig announcing
        # a Numark V7 makes Serato DJ Pro warn about unsupported hardware it
        # should never have been shown.
        assert TEMPLATE["midi_name"] != V7_PORT_NAME, \
            "the default port name must not identify the device"
        assert "V7" not in TEMPLATE["midi_name"] \
            and "Numark" not in TEMPLATE["midi_name"], \
            "the default port name must not name the model or the vendor"
        # Passthrough is the absence of timecode, not a separate switch.
        assert links({"rca": {"pair1": "output", "pair2": "output"}}) == [], \
            "passthrough links nothing, so the generator is not started"
        v7d = open(f"{HERE}/usb/v7d.py").read()
        assert f'V7_PORT_NAME = "{V7_PORT_NAME}"' in v7d, \
            "V7_PORT_NAME here has drifted from usb/v7d.py"
        assert f'DEFAULT_PORT_NAME = "{TEMPLATE["midi_name"]}"' in v7d, \
            "the default name here has drifted from usb/v7d.py"
        # Addressing the port for --calibrate, against a real `aconnect -i`
        # under an Italian locale — where the leading word is not "client".
        listing = ("cliente 14: 'Midi Through' [tipo=kernel]\n"
                   "    0 'Midi Through Port-0'\n"
                   "cliente 128: 'userspace controller daemon, USB' "
                   "[tipo=utente,pid=201869]\n"
                   "    0 'DVS Controller  '\n")
        assert seq_addr("DVS Controller", listing) == "128:0", \
            "the port is addressed by number, and the padding is stripped"
        assert seq_addr("Nothing Here", listing) is None, "a port that is absent"

        # --calibrate's parse. A full sweep of deck A: the extremes are what the
        # centre is computed from, so they are what these check.
        sweep = ["Source  Event                  Ch  Data",
                 " 20:0   Control change          0, controller 4, value 0",
                 " 20:0   Control change          0, controller 36, value 3",
                 " 20:0   Control change          0, controller 4, value 127",
                 " 20:0   Control change          0, controller 36, value 120",
                 " 20:0   Control change          0, controller 4, value 64",
                 " 20:0   Control change          0, controller 36, value 0"]
        assert pitch_span(sweep) == (3, 16376), "the extremes of a sweep"
        assert pitch_span(sweep[:1]) is None, "a fader that never spoke"
        # Deck B is the same physical fader on the next controller up.
        assert pitch_span([s.replace("controller 4,", "controller 5,")
                            .replace("controller 36,", "controller 37,")
                           for s in sweep]) == (3, 16376), "deck B's fader"
        # An MSB with no LSB after it must not produce a value: pairing it with
        # a stale LSB is a 127-count error in exactly the extremes that matter.
        assert pitch_span(sweep[:3] + [sweep[3]]) == (3, 3), \
            "a value needs its LSB"
        # Every message ignored is a value silently missing from the travel.
        assert pitch_span(["Source  Event   Ch  Data",
                           " 20:0   Note on                 0, note 17, "
                           "velocity 127"]) is None, "notes are not the fader"
        # A config from before a key existed still loads with the key filled
        # in, one level down as well: pitch.zero joined in step 4 and an older
        # file must not stop the window from opening.
        old = merge({"rca": {"pair1": "output", "pair2": "output"},
                     "pitch": {"range": 0.08, "center": 8191}})
        assert old["pitch"] == {"range": 0.08, "center": 8191, "zero": 0.0005}, \
            "an older pitch block gains the new key and keeps its own values"
        assert old["rca"]["pair1"] == "output", "the file wins over the template"
        assert merge({})["pitch"] == TEMPLATE["pitch"], "no pitch block at all"

        # The window's one piece of logic: field text back into the config.
        # Round-trips the template exactly, and a bad number names its field.
        assert configwin.parse(configwin.flatten(TEMPLATE)) == TEMPLATE, \
            "the window round-trips the template"
        assert configwin.parse({**configwin.flatten(TEMPLATE),
                                "pitch.range": "-0.16"})["pitch"]["range"] \
            == -0.16, "an inverted range is a number too"
        try:
            configwin.parse({**configwin.flatten(TEMPLATE), "level": "loud"})
            assert False, "a word is not a level"
        except ValueError as err:
            assert "level" in str(err), "the error names the field"
        assert set(k for k, _, _ in configwin.FIELDS) \
            == set(configwin.flatten(TEMPLATE)), \
            "every contract key has a field, and no field is an invented key"
        assert mode_a(TEMPLATE), "both pairs on timecode is mode A"
        assert not mode_a({**TEMPLATE, "rca": {"pair1": "timecode:A",
                                               "pair2": "output"}}), \
            "a pair carrying the app's audio needs the loopback chain"
        assert not mode_a({**TEMPLATE, "rca": {"pair1": "output",
                                               "pair2": "output"}}), \
            "no timecode at all is the passthrough, not mode A"
        assert v7d_args(TEMPLATE) == [
            "--port", "DVS Controller", "--timecode", "A,B", "--counts", "3600",
            "--range", "0.08", "--center", "8192", "--zero", "0.0005"], \
            "the config reaches ./v7d as flags"
        assert v7d_args({**TEMPLATE, "rca": {"pair1": "timecode:B",
                                             "pair2": "timecode:A"}})[3] == "B,A", \
            "pairs are per RCA pair, not global"
        # macOS runs mode A only: the rest of this file is PipeWire and
        # snd-aloop. The window and main() both go through validate(), so a
        # pair on "output" is refused in the window, not after it closes.
        assert validate(TEMPLATE, platform="darwin") == links(TEMPLATE), \
            "mode A passes validate() on macOS"
        assert validate({**TEMPLATE, "rca": {"pair1": "timecode:A",
                                             "pair2": "output"}},
                        platform="linux") == links(
            {**TEMPLATE, "rca": {"pair1": "timecode:A", "pair2": "output"}}), \
            "validate() is links() on Linux"
        try:
            validate({**TEMPLATE, "rca": {"pair1": "timecode:A",
                                          "pair2": "output"}},
                     platform="darwin")
        except ValueError as err:
            assert "macOS" in str(err), f"the refusal names the platform: {err}"
        else:
            assert False, "a pair on output must be refused on macOS"
        assert isinstance(HAVE_ASEQ, bool), "HAVE_ASEQ is decided at import"
        # Mode C: both pairs on output. On macOS that is ./v7d alone, MIDI
        # only, no --timecode -- VirtualDJ drives the V7 through the port.
        passthrough = {**TEMPLATE, "rca": {"pair1": "output", "pair2": "output"},
                       "midi_name": V7_PORT_NAME}
        mixed = {**TEMPLATE, "rca": {"pair1": "timecode:A", "pair2": "output"}}
        assert mode_c(passthrough) and not mode_c(TEMPLATE) and not mode_c(mixed), \
            "mode C is both pairs on output, nothing else"
        assert validate(passthrough, platform="darwin") == [], \
            "macOS runs the passthrough: v7d alone, MIDI only"
        try:
            validate(mixed, platform="darwin")
        except ValueError as err:
            assert "macOS" in str(err), f"the refusal names the platform: {err}"
        else:
            assert False, "a pair carrying the app's audio is still refused on macOS"
        assert "--timecode" not in v7d_args(passthrough), \
            "no timecode to make, no --timecode flag: v7d is MIDI only"
        assert v7d_args(passthrough)[:2] == ["--port", V7_PORT_NAME]
        assert solo(TEMPLATE, platform="linux") and solo(TEMPLATE, platform="darwin")
        assert solo(passthrough, platform="darwin") \
            and not solo(passthrough, platform="linux"), \
            "on Linux the passthrough still runs through snd-aloop"
        assert not solo(mixed, platform="linux")
        # none: nothing on the pair. Both on none is MIDI only everywhere,
        # none beside timecode is mode A with a silent pair, none beside
        # output is refused (the loopback carries all four channels).
        midi_only = {**TEMPLATE, "rca": {"pair1": "none", "pair2": "none"}}
        half = {**TEMPLATE, "rca": {"pair1": "timecode:A", "pair2": "none"}}
        assert solo(midi_only, platform="linux") and solo(midi_only, platform="darwin"), \
            "both pairs on none is ./v7d alone on every platform"
        assert validate(midi_only, platform="linux") == [] \
            and "--timecode" not in v7d_args(midi_only), \
            "none links nothing and makes no timecode"
        assert solo(half, platform="linux") and v7d_args(half)[3] == "A,-", \
            "none beside timecode is mode A with that pair silent"
        try:
            validate({**TEMPLATE, "rca": {"pair1": "none", "pair2": "output"}},
                     platform="linux")
        except ValueError as err:
            assert "none" in str(err), f"the refusal names the value: {err}"
        else:
            assert False, "none beside output must be refused"
        print("launcher selftest ok")
        return 0

    if "--calibrate" in sys.argv:
        return calibrate(cfg)

    if "--print" in sys.argv:
        print(f"config     {CONFIG}")
        # `level` is the aloop sink's volume, so it belongs to the modes that
        # go through the sink. In mode A nothing does: the timecode leaves at
        # the encoder's own level and v7d's --level is the knob, which at a
        # nominal line level nobody needs. Saying "level 0.5 on <sink>" there
        # would name two actions this path never performs.
        if solo(cfg):
            print(f"level      {cfg['level']} applies to mode B only; the "
                  f"timecode leaves at v7d's own level")
        else:
            print(f"rate       force {RATE}")
            print(f"level      {cfg['level']} on {ALOOP}")
        print(f"midi port  {cfg['midi_name']!r}"
              + ("   (passthrough: VirtualDJ will claim it)"
                 if cfg["midi_name"] == V7_PORT_NAME else "   (anonymous)"))
        if solo(cfg):
            print(f"v7d        {V7D_BIN} " + " ".join(v7d_args(cfg))
                  + ("   (mode A: no JACK, no loopback)" if mode_a(cfg)
                     else "   (mode C on macOS: MIDI only, RCAs silent)"))
            return 0
        print(f"v7d        V7_AUDIO=1 python3 {HERE}/usb/v7d.py "
              f"{cfg['midi_name']!r}"
              + ("   (already up, skipped)"
                 if alsa_port_up(cfg["midi_name"]) else ""))
        if wanted:
            print(f"v7dvs      pw-jack {V7DVS_BIN} "
                  + " ".join(v7dvs_args(cfg)))
        else:
            print("v7dvs      (not started: no pair carries timecode)")
        for src, dst in wanted:
            print(f"link       {src} -> {dst}")
        return 0

    children = []
    if solo(cfg):
        # Mode A in C: the timecode leaves the V7's RCAs straight from v7d,
        # so there is nothing to link and no graph rate to force. Mode C on
        # macOS is the same start without --timecode: the port and nothing
        # else, which is all VirtualDJ needs.
        if alsa_port_up(cfg["midi_name"]):
            print(f"launcher: {cfg['midi_name']!r} is already up; stop it first",
                  file=sys.stderr)
            return 1
        child = subprocess.Popen([V7D_BIN] + v7d_args(cfg))
        children.append(child)
        print(f"launcher: {V7D_BIN} " + " ".join(v7d_args(cfg)))
        # Wait for the port, and notice a v7d that died instead: SIGCHLD does
        # not wake signal.pause(), so without this a daemon that exits at
        # startup (no V7, no udev rule, a flag an older binary rejects) leaves
        # the launcher blocked forever over a dead child and returning 0.
        # ponytail: without aconnect (macOS) "up" is "still alive after 2 s".
        # v7d exits within milliseconds when the V7 is missing, claimed by
        # another process or fails its handshake, so a child alive at 2 s has
        # opened its port. Upgrade path: a ready line on v7d's stdout.
        born = time.monotonic()
        if not wait_for(lambda: child.poll() is not None
                        or alsa_port_up(cfg["midi_name"])
                        or (not HAVE_ASEQ and time.monotonic() - born > 2.0),
                        "the V7's MIDI port"):
            return 1
        if child.poll() is not None:
            print(f"launcher: {V7D_BIN} exited with {child.returncode}",
                  file=sys.stderr)
            return 1
        try:
            if window:
                # Started from the window: keep one on screen with a Stop
                # button, and come back when it closes or v7d dies.
                configwin.running(
                    f"v7dvs {VERSION}",
                    f"{cfg['midi_name']!r} is up"
                    + (", timecode on the RCAs" if mode_a(cfg) else ", MIDI only"),
                    alive=lambda: child.poll() is None)
            else:
                signal.signal(signal.SIGINT, lambda *_: None)
                signal.signal(signal.SIGTERM, lambda *_: None)
                signal.pause()
        finally:
            for p in children:
                p.terminate()
            for p in children:
                try:
                    p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    p.kill()
            print("\nlauncher: stopped")
        if child.returncode is not None and child.returncode > 0:
            # It died under the window rather than being stopped: say so, the
            # log is the only place a .app user can look.
            print(f"launcher: {V7D_BIN} exited with {child.returncode}",
                  file=sys.stderr)
            return 1
        return 0

    run("pw-metadata", "-n", "settings", "0", "clock.force-rate", str(RATE))
    try:
        # v7d first, or the aloop cable does not get pinned at 44100. Skipped
        # when the port is already up: it claims the USB device exclusively, and
        # keeping it in its own terminal is the normal debugging posture.
        if not alsa_port_up(cfg["midi_name"]):
            children.append(subprocess.Popen(
                ["python3", f"{HERE}/usb/v7d.py", cfg["midi_name"]],
                env={**os.environ, "V7_AUDIO": "1"}))
            if not wait_for(lambda: alsa_port_up(cfg["midi_name"]),
                            "the V7's ALSA port"):
                return 1

        # No pair carrying timecode means nothing would listen to the generator,
        # so do not run it. That is the passthrough mode: the app drives the V7
        # over MIDI and the RCAs carry its audio, with no timecode anywhere.
        if wanted:
            children.append(subprocess.Popen(
                ["pw-jack", V7DVS_BIN] + v7dvs_args(cfg)))
        else:
            print("launcher: no timecode configured, generator not started")

        if wanted:
            ports = lambda: run("pw-link", "-o", "-i")
            if not wait_for(lambda: all(s in ports() for s, _ in wanted),
                            "v7dvs's JACK ports"):
                return 1
            for src, dst in wanted:
                run("pw-link", src, dst)
                print(f"launcher: {src} -> {dst}")

        # After the links: PipeWire creates the node on demand, and setting a
        # volume on a node that is not there yet is silently lost.
        nid = node_id(ALOOP)
        if nid is None:
            print(f"launcher: {ALOOP} not found, level not set", file=sys.stderr)
        else:
            run("wpctl", "set-volume", str(nid), str(cfg["level"]))
            print(f"launcher: level {cfg['level']}")

        # SIGTERM as well as SIGINT, and this is not defensive tidiness: the
        # default SIGTERM disposition kills the process outright, so the finally
        # below never runs — leaving the children orphaned and, worse, the whole
        # PipeWire graph still forced to 44100. Measured, not imagined.
        signal.signal(signal.SIGINT, lambda *_: None)
        signal.signal(signal.SIGTERM, lambda *_: None)
        signal.pause()
    finally:
        # Reverse order, and always restore the graph rate: forcing it is a
        # system-wide change and leaving it behind would be rude.
        for p in reversed(children):
            p.terminate()
        for p in reversed(children):
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
        run("pw-metadata", "-n", "settings", "0", "clock.force-rate", "0")
        print("\nlauncher: stopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
