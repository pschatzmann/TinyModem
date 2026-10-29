#!/bin/bash
#
# AX.25 interoperability tests with direwolf.
#
# usage: ax25-interop.sh {test} {minimodem} {atest} {gen_packets}
#
#   tx:      we transmit, direwolf (atest) decodes
#   rx:      direwolf (gen_packets) transmits, we decode
#   noise:   we decode the gen_packets frames with increasing noise
#   fx25-tx, il2p-tx: we transmit FX.25 / IL2P, direwolf decodes
#   fx25-rx, il2p-rx: direwolf transmits FX.25 / IL2P, we decode
#
set -u

[ $# -eq 4 ] || {
    echo "usage: ax25-interop.sh {test} {minimodem} {atest} {gen_packets}" 1>&2
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
    fx25-tx|il2p-tx)
        mode=${TEST%-tx}
        tag=$([ "$mode" = fx25 ] && echo "FX.25" || echo "IL2P")
        "$MODEM" --tx --file "$TMPD/test.wav" $mode < testdata-aprs.txt || exit 1
        "$ATEST" "$TMPD/test.wav" 2>&1 | sed 's/\x1b\[[0-9;]*m//g' > "$TMPD/atest.out"
        sed -n 's/^\[0\] //p' "$TMPD/atest.out" > "$TMPD/test.out"
        cmp testdata-aprs.txt "$TMPD/test.out" || exit 1
        n=$(grep -c "$tag" "$TMPD/atest.out")
        [ "$n" -eq "$(wc -l < testdata-aprs.txt)" ] || { echo "FAIL: only $n $tag frames"; exit 1; }
        echo "OK direwolf decoded all $tag frames"
        ;;
    fx25-rx|il2p-rx)
        option=$([ "$TEST" = fx25-rx ] && echo "-X 16" || echo "-I 1")
        "$GEN_PACKETS" $option -r 48000 -o "$TMPD/test.wav" > /dev/null 2>&1 || exit 1
        "$MODEM" --rx --file "$TMPD/test.wav" aprs > "$TMPD/test.out" 2> "$TMPD/test.err"
        n=$(grep -c "^WB2OSZ-15>TEST:,The quick brown fox jumps over the lazy dog!  [1-4] of 4$" "$TMPD/test.out")
        [ "$n" -eq 4 ] || { echo "FAIL: $n of 4 frames"; exit 1; }
        grep -E -q "(fx25|il2p)=4 " "$TMPD/test.err" || { cat "$TMPD/test.err"; exit 1; }
        echo "OK all 4 frames of gen_packets $option decoded"
        ;;
    *) echo "unknown test: $TEST" 1>&2; exit 2 ;;
esac
