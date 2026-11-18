#!/bin/sh
# Encoder against xwax's own decoder. Fails loudly, exits non-zero.
#
# LC_ALL=C is not cosmetic: under an Italian locale awk reads "0.998" as 0 and
# every assertion below silently passes.
set -e
export LC_ALL=C

fail=0

# Skip the first second: pitch needs ~50ms to lock, absolute position ~220ms.
assert_pitch() {
    ./tcgen $2 $3 $4 2>/dev/null | ./tcdec 2>/dev/null | awk -v want="$1" -v what="$2 $3 $4" '
        $1 > 1 && $3 >= 0 { n++; d = $2 - want; if (d < 0) d = -d; if (d > mx) mx = d }
        END {
            printf "  pitch %-18s want %-6s max|err| %.4f  n=%d  ", what, want, mx, n
            if (n < 20)       { print "FAIL (nothing decoded)"; exit 1 }
            if (mx > 0.01)    { print "FAIL"; exit 1 }
            print "ok"
        }' || fail=1
}

# One line per 0.1s, so nominal speed must move position by exactly 100 cycles.
assert_step() {
    ./tcgen $2 $3 $4 2>/dev/null | ./tcdec 2>/dev/null | awk -v want="$1" '
        $3 >= 0 { if (seen) { if ($3 - p != want) bad++; n++ } p = $3; seen = 1 }
        END {
            printf "  position step want %-5s wrong: %d of %d  ", want, bad+0, n
            if (n < 10) { print "FAIL (nothing decoded)"; exit 1 }
            if (bad)    { print "FAIL"; exit 1 }
            print "ok"
        }' || fail=1
}

echo "platter state machine (no ALSA, no JACK):"
./platter_test || fail=1

echo "launcher (config translation, no processes):"
./launcher.py --selftest || fail=1

echo "MIDI parser (trailer, running status, SysEx, resync):"
./midiparse_test || fail=1

echo "end of the record (the needle is lifted before the clamp):"
./encoder_test || fail=1

echo "audio sink (rate servo and FIFO, no ALSA, no hardware):"
python3 usb/v7audio.py || fail=1

echo "macOS mode A glue (synthetic platter through libv7core, no hardware):"
python3 usb/v7tc.py 2>/dev/null | ./tcdec 44100 2>/dev/null | awk '
    $1 > 1 { n++; if ($3 >= 0) pos++; d = $2 - 1.0; if (d < 0) d = -d; if (d > mx) mx = d }
    END {
        printf "  pitch 1.0 from a synthetic platter  max|err| %.4f  position on %d of %d  ", mx, pos, n
        if (n < 20)    { print "FAIL (nothing decoded)"; exit 1 }
        if (mx > 0.01) { print "FAIL"; exit 1 }
        if (pos < n)   { print "FAIL (position lost)"; exit 1 }
        print "ok"
    }' || fail=1

echo "mode A in C (synthetic platter through ./v7d --selftest, no hardware):"
./v7d --selftest 2>/dev/null | ./tcdec 44100 2>/dev/null | awk '
    $1 > 1 { n++; if ($3 >= 0) pos++; d = $2 - 1.0; if (d < 0) d = -d; if (d > mx) mx = d }
    END {
        printf "  pitch 1.0 from a synthetic platter  max|err| %.4f  position on %d of %d  ", mx, pos, n
        if (n < 20)    { print "FAIL (nothing decoded)"; exit 1 }
        if (mx > 0.01) { print "FAIL"; exit 1 }
        if (pos < n)   { print "FAIL (position lost)"; exit 1 }
        print "ok"
    }' || fail=1

echo "constant rate, forwards:"
assert_pitch 1.0 1.0 5
assert_step 100 1.0 5

echo "constant rate, backwards (exercises rev()):"
assert_pitch -1.0 -1.0 5
assert_step -100 -1.0 5

echo "half speed:"
assert_pitch 0.5 0.5 5
assert_step 50 0.5 5

echo "swept rate — the whole point, and what mktimecode cannot do:"
# Looser bound: xwax smooths its pitch estimate, so it lags a moving target.
./tcgen ramp 0.5 1.5 10 2>/dev/null | ./tcdec 2>/dev/null | awk '
    $1 > 1 && $3 >= 0 { want = 0.5 + 1.0 * ($1 / 10); n++
                        d = $2 - want; if (d < 0) d = -d; if (d > mx) mx = d }
    END {
        printf "  pitch tracks 0.5->1.5  max|err| %.4f  n=%d  ", mx, n
        if (n < 50)    { print "FAIL (nothing decoded)"; exit 1 }
        if (mx > 0.05) { print "FAIL"; exit 1 }
        print "ok"
    }' || fail=1

echo "end of the record is clamped, not walked off:"
./tcgen 1.0 5 707000 2>&1 >/dev/null | grep -q "hit the end" \
    && echo "  clamped at TC_SAFE  ok" || { echo "  FAIL (no clamp)"; fail=1; }

[ $fail -eq 0 ] && echo "all ok" || echo "FAILURES"
exit $fail
