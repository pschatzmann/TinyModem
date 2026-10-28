#!/bin/bash
#
# AX.25 interoperability tests with direwolf.
#
# usage: ax25-interop.sh {tx|rx|noise} {minimodem} {atest} {gen_packets}
#
#   tx:    we transmit, direwolf (atest) decodes
#   rx:    direwolf (gen_packets) transmits, we decode
#   noise: we decode the gen_packets frames with increasing noise
#
set -u

[ $# -eq 4 ] || {
    echo "usage: ax25-interop.sh {tx|rx|noise} {minimodem} {atest} {gen_packets}" 1>&2
    exit 2
}
TEST="$1"
MODEM="$2"
ATEST="$3"
GEN_PACKETS="$4"
cd "$(dirname "$0")"

TMPD=$(mktemp -d)
trap 'rm -rf "$TMPD"' EXIT

case "$TEST" in
    tx)
        "$MODEM" --tx --file "$TMPD/test.wav" aprs < testdata-aprs.txt || exit 1
        "$ATEST" "$TMPD/test.wav" 2>&1 | sed 's/\x1b\[[0-9;]*m//g' \
            | sed -n 's/^\[0\] //p' > "$TMPD/test.out"
        cmp testdata-aprs.txt "$TMPD/test.out" || exit 1
        echo "OK direwolf decoded all frames"
        ;;
    rx)
        "$GEN_PACKETS" -r 48000 -o "$TMPD/test.wav" > /dev/null 2>&1 || exit 1
        n=$("$MODEM" --rx -q --file "$TMPD/test.wav" aprs \
            | grep -c "^WB2OSZ-15>TEST:,The quick brown fox jumps over the lazy dog!  [1-4] of 4$")
        [ "$n" -eq 4 ] || { echo "FAIL: $n of 4 frames"; exit 1; }
        echo "OK all 4 frames of gen_packets decoded"
        ;;
    noise)
        "$GEN_PACKETS" -n 100 -r 44100 -o "$TMPD/test.wav" > /dev/null 2>&1 || exit 1
        ours=$("$MODEM" --rx -q --file "$TMPD/test.wav" aprs | wc -l)
        theirs=$("$ATEST" "$TMPD/test.wav" 2>&1 | grep "packets decoded" | cut -d' ' -f1)
        echo "frames with increasing noise: minimodem $ours, direwolf $theirs of 100"
        [ "$ours" -ge 60 ] || exit 1
        ;;
    *) echo "unknown test: $TEST" 1>&2; exit 2 ;;
esac
