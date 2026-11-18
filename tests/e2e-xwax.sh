#!/bin/sh
# End-to-end: bring up the rig, start xwax with both decks, wire the platter to
# it, and let a human scratch. Ctrl-C takes it all down.
#
#     tests/e2e-xwax.sh [--analog | --v7audio] [music-dir]
#
# The two modes are the two ways this project can actually be used, and they are
# mutually exclusive because they compete for the same two RCA pairs:
#
#   --analog   (default) PURE DVS. The timecode leaves the V7's RCAs, goes into
#              a real interface's LINE inputs, and comes back as audio xwax
#              decodes. Monitoring goes out of that same interface. This is the
#              path that also works with Serato, since a certified DVS card
#              feeding it sees exactly what it expects from a turntable. Needs
#              an interface with inputs, and the V7's RCAs patched into it.
#              Runs ./v7d, the C daemon: no JACK, no loopback.
#
#   --v7audio  THE V7 ALONE. The timecode never becomes analog -- xwax reads it
#              straight over JACK -- and the V7's RCAs carry xwax's audio
#              instead, one deck per pair. No second interface anywhere. This is
#              what the macOS HAL plugin will eventually make possible without
#              any cable at all.
#
# This is a TEST, not a feature, and it lives here rather than in launcher.py
# deliberately: v7dvs connects to nothing because the DJ app is meant to be
# interchangeable, and teaching the launcher about xwax would put app-specific
# knowledge back into the product. A test may know about xwax; the product may
# not. It also answers the one question a measurement cannot -- `make check` and
# tcdec prove the signal is correct, only a hand proves the deck is playable.
#
# Both decks are wired, because one physical platter driving two virtual decks
# is the actual instrument: the selected deck follows your hand, the other keeps
# turning at nominal if it was playing.
set -e

MODE=analog
case "$1" in
    --analog)  MODE=analog;  shift ;;
    --v7audio) MODE=v7audio; shift ;;
    --*)       echo "usage: $0 [--analog|--v7audio] [music-dir]" >&2; exit 2 ;;
esac

# xdg-user-dir rather than ~/Music: the directory is localised, and on this very
# machine it is ~/Musica -- a hardcoded English default would have missed it.
MUSIC=${1:-$(xdg-user-dir MUSIC 2>/dev/null || echo "$HOME/Music")}
RATE=44100
ALOOP=alsa_output.platform-snd_aloop.0.analog-surround-40
HERE=$(cd "$(dirname "$0")/.." && pwd)
CONF=${TMPDIR:-/tmp}/v7dvs-e2e-$MODE.json

command -v xwax >/dev/null || {
    echo "xwax is not on PATH. Build it with JACK=1 (see xwax.org)." >&2
    exit 1
}
[ -d "$MUSIC" ] || { echo "no such music directory: $MUSIC" >&2; exit 1; }

pids=""
cleanup() {
    # Kill by pid, never by pattern: pkill -f matches this script's own command
    # line and takes the shell with it (exit 144), which has cost time before.
    for p in $pids; do kill -TERM "$p" 2>/dev/null || true; done
    wait 2>/dev/null || true
    rm -f "$CONF"
    echo "e2e: stopped"
}
trap cleanup EXIT INT TERM

waitfor() {                             # waitfor <seconds> <shell test...>
    _n=0 _max=$(($1 * 5)); shift
    while [ $_n -lt $_max ]; do
        if "$@" >/dev/null 2>&1; then return 0; fi
        _n=$((_n + 1)); sleep 0.2
    done
    return 1
}

has_port() { pw-link -o -i 2>/dev/null | grep -q "$1"; }
graph_is_44100() { [ "$(pw-jack jack_samplerate 2>/dev/null)" = "$RATE" ]; }
node_of() {                             # node_of DEFAULT_AUDIO_SINK|..._SOURCE
    wpctl inspect "@$1@" 2>/dev/null |
        sed -n 's/.*node\.name = "\(.*\)".*/\1/p'
}

# A config of this test's own, so the one you actually use is left alone. The
# rca field is the whole difference between the two modes: in analog the pairs
# carry timecode out to the interface, in v7audio they are free for xwax's
# audio to be pointed at instead.
#
# The level is 0.5 in BOTH modes, and the second case is the surprising one.
# For timecode it keeps the signal off the clipping rail, because a clipped
# timecode still locks pitch while silently destroying absolute position. For
# music the first guess was that the level is xwax's business and this should
# be 1.0 -- measured, that is wrong: at 1.0, 48.5% of samples clipped and the
# crest factor collapsed to 2.0 dB, i.e. a square wave. At 0.5, zero clipped
# samples and a healthy 10.4 dB crest with the peak still at -3.7 dBFS. The
# path to the RCAs has no headroom for a full-scale signal.
if [ "$MODE" = analog ]; then
    RCA='"pair1": "timecode:A", "pair2": "timecode:B"'
else
    RCA='"pair1": "output", "pair2": "output"'
fi
# One variable, written into the config and matched on afterwards. Hardcoding
# the name in the wait below only worked while the config left it out, and
# roadmap 6b is precisely about that key changing.
MIDI_NAME="DVS Controller"
cat > "$CONF" <<EOF
{ "rca": { $RCA },
  "level": 0.5,
  "counts_per_rev": 3600,
  "midi_name": "$MIDI_NAME",
  "pitch": { "range": 0.08, "center": 8192 } }
EOF

echo "e2e: mode $MODE"
echo "e2e: starting the rig"
"$HERE/launcher.py" --config "$CONF" & LAUNCHER=$!; pids="$pids $LAUNCHER"

if [ "$MODE" = analog ]; then
    # Mode A is ./v7d alone: the timecode is analog by the time xwax hears it,
    # so the graph rate is irrelevant and there are no JACK ports to wait for.
    echo "e2e: waiting for the V7's MIDI port"
    waitfor 30 sh -c "aconnect -l | grep -q '$MIDI_NAME'" \
        && kill -0 "$LAUNCHER" 2>/dev/null || {
        echo "e2e: no '$MIDI_NAME' port from the launcher we started." >&2
        echo "     A daemon left over from an earlier run holds the same name" >&2
        echo "     and would look exactly like success, so the launcher's own" >&2
        echo "     liveness is what is checked here -- see its message above." >&2
        exit 1; }
else
    # xwax reads the graph's sample rate once, at startup, and never revisits
    # it. Start it too early and it locks onto 48000 while v7dvs generates at
    # 44100 -- an 8.8% pitch error that looks exactly like an encoder bug. So
    # wait for the rig to have actually pulled the graph down to 44100 first.
    # Measured: without this wait, xwax printed "JACK: 48000Hz" every time.
    echo "e2e: waiting for the graph to settle at $RATE"
    waitfor 30 has_port 'v7dvs:timecode_A_L' || {
        echo "e2e: v7dvs never appeared" >&2; exit 1; }
    waitfor 15 graph_is_44100 || {
        echo "e2e: graph is at $(pw-jack jack_samplerate), not $RATE." >&2
        echo "     xwax would decode at the wrong rate. Refusing to start it." >&2
        exit 1; }
fi

echo "e2e: starting xwax on '$MUSIC'"
# --timecode and --line are xwax's defaults, spelled out on purpose: the
# timecode has to match what encoder.c synthesizes, and PHONO on a line-level
# signal is the classic way to make this look broken.
pw-jack xwax -l "$MUSIC" \
    --timecode serato_2a --line --jack deckA \
    --timecode serato_2a --line --jack deckB & pids="$pids $!"

waitfor 60 has_port 'xwax:deckB_timecode_L' || {
    echo "e2e: xwax's ports never appeared" >&2; exit 1; }

link() { pw-link "$1" "$2" 2>/dev/null && echo "e2e: $1 -> $2"; }

if [ "$MODE" = analog ]; then
    SRC=$(node_of DEFAULT_AUDIO_SOURCE)
    [ -n "$SRC" ] || { echo "e2e: no default source to read timecode from" >&2
                       exit 1; }
    echo "e2e: timecode in from $SRC  (its inputs must be on LINE, not PHONO)"
    # Deck A on the first input pair, deck B on the second -- matching the
    # V7's ch0..ch3 mapping onto the interface, measured diagonal.
    link "$SRC:capture_FL" xwax:deckA_timecode_L
    link "$SRC:capture_FR" xwax:deckA_timecode_R
    link "$SRC:capture_RL" xwax:deckB_timecode_L
    link "$SRC:capture_RR" xwax:deckB_timecode_R

    SINK=$(node_of DEFAULT_AUDIO_SINK)
    for d in A B; do
        link "xwax:deck${d}_playback_L" "$SINK:playback_FL"
        link "xwax:deck${d}_playback_R" "$SINK:playback_FR"
    done
    echo "e2e: monitoring on $SINK"
else
    for d in A B; do
        link "v7dvs:timecode_${d}_L" "xwax:deck${d}_timecode_L"
        link "v7dvs:timecode_${d}_R" "xwax:deck${d}_timecode_R"
    done
    # One deck per RCA pair, which is how v7audio.py's four channels are
    # documented: ch0,ch1 = deck A and ch2,ch3 = deck B.
    link xwax:deckA_playback_L "$ALOOP:playback_FL"
    link xwax:deckA_playback_R "$ALOOP:playback_FR"
    link xwax:deckB_playback_L "$ALOOP:playback_RL"
    link xwax:deckB_playback_R "$ALOOP:playback_RR"
    echo "e2e: audio out of the V7's RCAs, deck A on pair 1, deck B on pair 2"
fi

echo
echo "e2e: load a track on each deck, then spin the platter. Ctrl-C to stop."
wait
