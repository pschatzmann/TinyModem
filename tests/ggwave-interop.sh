#!/bin/bash
#
# ggwave interoperability tests with the ggwave-to-file and ggwave-from-file
# examples of https://github.com/ggerganov/ggwave
#
# usage: ggwave-interop.sh {tx|rx} {minimodem} {ggwave-to-file} {ggwave-from-file}
#
#   tx: we transmit, ggwave-from-file decodes
#   rx: ggwave-to-file transmits, we decode
#
# All protocols which are supported by both (ids 0 - 8) are tested.
#
set -u

[ $# -eq 4 ] || {
    echo "usage: ggwave-interop.sh {tx|rx} {minimodem} {ggwave-to-file} {ggwave-from-file}" 1>&2
    exit 2
}
TEST="$1"
MODEM="$2"
TO_FILE="$3"
FROM_FILE="$4"
cd "$(dirname "$0")"

TMPD=$(mktemp -d)
trap 'rm -rf "$TMPD"' EXIT

MODES=(ggwave-normal ggwave-fast ggwave-fastest ggwave-u-normal ggwave-u-fast
       ggwave-u-fastest ggwave-dt-normal ggwave-dt-fast ggwave-dt-fastest)
MESSAGE="Hello from TinyModem 0123456789"
fail=0

case "$TEST" in
    tx)
        for id in "${!MODES[@]}"; do
            mode=${MODES[$id]}
            printf "%s" "$MESSAGE" | "$MODEM" --tx --file "$TMPD/test.wav" "$mode" || {
                echo "FAIL: --tx $mode"; fail=1; continue; }
            if "$FROM_FILE" "$TMPD/test.wav" 2>/dev/null \
                | grep -qF "Decoded message with length ${#MESSAGE}: '$MESSAGE'"; then
                echo "OK ggwave decoded $mode"
            else
                echo "FAIL: ggwave did not decode $mode"; fail=1
            fi
        done
        ;;
    rx)
        for id in "${!MODES[@]}"; do
            mode=${MODES[$id]}
            echo "$MESSAGE" | "$TO_FILE" -f"$TMPD/test.wav" -p"$id" > /dev/null 2>&1 || {
                echo "FAIL: ggwave-to-file -p$id"; fail=1; continue; }
            # the receiver detects the speed: decode with the normal variant
            out=$("$MODEM" --rx -q --file "$TMPD/test.wav" "$mode")
            if [ "$out" = "$MESSAGE" ]; then
                echo "OK decoded ggwave protocol $id ($mode)"
            else
                echo "FAIL: protocol $id ($mode): '$out'"; fail=1
            fi
        done
        ;;
    *) echo "unknown test: $TEST" 1>&2; exit 2 ;;
esac
exit $fail
