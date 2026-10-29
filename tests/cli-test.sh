#!/bin/bash
#
# Command line tests (ported from the minimodem test suite).
#
# usage: cli-test.sh {test-name} {tx-minimodem} {rx-minimodem}
#
# The audio is generated with tx-minimodem and decoded with rx-minimodem:
# use our minimodem for both to test the tool, or the original minimodem
# for one of them to test the interoperability.
#
set -u

[ $# -eq 3 ] || {
    echo "usage: cli-test.sh {test-name} {tx-minimodem} {rx-minimodem}" 1>&2
    exit 2
}
TEST="$1"
TX="$2"
RX="$3"
cd "$(dirname "$0")"

TMPD=$(mktemp -d)
trap 'rm -rf "$TMPD"' EXIT

# self_test [-P] textfile tx_args [-- rx_args]
#   -P: require a "(rate perfect)" reception
self_test() {
    local perfect=0
    [ "$1" = "-P" ] && { perfect=1; shift; }
    local textfile="$1"
    shift
    local tx_args="" rx_args=""
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do tx_args="$tx_args $1"; shift; done
    if [ $# -gt 0 ]; then shift; rx_args="$*"; else rx_args="$tx_args"; fi

    "$TX" --tx --file "$TMPD/test.wav" $tx_args < "$textfile" || {
        echo "FAIL: --tx $tx_args"; return 1; }
    "$RX" --rx --file "$TMPD/test.wav" $rx_args > "$TMPD/test.out" 2> "$TMPD/test.err" || {
        cat "$TMPD/test.err"; echo "FAIL: --rx $rx_args"; return 1; }
    cmp "$textfile" "$TMPD/test.out" || {
        cat "$TMPD/test.err"; echo "FAIL: output differs ($tx_args -- $rx_args)"; return 1; }

    STATS=$(grep "### NOCARRIER" "$TMPD/test.err" | tail -1)
    if [ $perfect -eq 1 ] && ! echo "$STATS" | grep -q "(rate perfect)"; then
        echo "IMPERFECT $STATS"
        return 1
    fi
    echo "OK $tx_args -- $rx_args: $STATS"
    return 0
}

# float comparison: |a - b| <= eps
near() {
    awk -v a="$1" -v b="$2" -v e="$3" 'BEGIN { d = a - b; if (d < 0) d = -d; exit !(d <= e) }'
}

amplitude_test() {
    local fail=0
    for a in 3.50 1.00 0.30 0.01; do
        self_test testdata-ascii.txt --volume $a 1200 -- 1200 || { fail=1; continue; }
        local ampl=${STATS##*ampl=}
        ampl=${ampl%% *}
        local expected=$a
        # int samples are clamped to max ~1.0
        near "$a" 3.50 0.001 && expected=1.0
        near "$ampl" "$expected" 0.011 || {
            echo "FAIL: rx amplitude $ampl does not match tx volume $a"; fail=1; }
    done
    return $fail
}

noise_test() {
    local fail=0
    for noise in 0.00 0.05 0.10 0.50; do
        self_test testdata-ascii.txt "$@" --volume 0.5 -- "$@" --Xrxnoise $noise --rx-one || fail=1
    done
    return $fail
}

rate_slop_test() {
    local fail=0
    for tx_rate in 292 299 300 301 308; do
        self_test testdata-ascii.txt $tx_rate -- 300 || fail=1
    done
    return $fail
}

callerid_test() {
    local bytesfile="$1"
    local textfile="${bytesfile%%.bytes}.txt"
    "$TX" --tx --file "$TMPD/test.wav" 1200 --ascii < "$bytesfile" || return 1
    "$RX" --rx --file "$TMPD/test.wav" callerid > "$TMPD/test.out" 2> "$TMPD/test.err" || {
        cat "$TMPD/test.err"; return 1; }
    cmp "$textfile" "$TMPD/test.out" || return 1
    echo "OK callerid $bytesfile"
}

tx_consistent_test() {
    for j in 1 2 3; do
        "$TX" --tx --file "$TMPD/test$j.wav" 1200 "$@" < testdata-ascii.txt || return 1
    done
    cmp -s "$TMPD/test1.wav" "$TMPD/test2.wav" && cmp -s "$TMPD/test1.wav" "$TMPD/test3.wav" || {
        echo "FAIL: TX-NOT-CONSISTENT"; return 1; }
    echo "OK three runs of '--tx 1200 $*' created identical output"
}

tx_wav_header_test() {
    # the WAV header must contain the correct data size
    "$TX" --tx --file "$TMPD/test.wav" 1200 < testdata-ascii.txt || return 1
    local size
    size=$(stat -c %s "$TMPD/test.wav")
    local riff
    riff=$(od -An -t u4 -j 4 -N 4 "$TMPD/test.wav" | tr -d ' ')
    [ "$riff" -eq $((size - 8)) ] || { echo "FAIL: RIFF size $riff != $((size - 8))"; return 1; }
    echo "OK RIFF size $riff"
}

wav_formats_test() {
    # converts a 16 bit WAV to 8, 24, 32 bit PCM and 32 bit float
    "$TX" --tx --file "$TMPD/s16.wav" --samplerate 22050 1200 < testdata-ascii.txt || return 1
    python3 - "$TMPD" <<'PYEOF' || return 1
import struct, sys
d = sys.argv[1]
raw = open(d + "/s16.wav", "rb").read()
pos = raw.find(b"data")
pcm = raw[pos + 8:]
s = struct.unpack("<%dh" % (len(pcm) // 2), pcm)
def wav(name, fmt, bits, data):
    ba = bits // 8
    open(d + "/" + name + ".wav", "wb").write(
        b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " +
        struct.pack("<IHHIIHH", 16, fmt, 1, 22050, 22050 * ba, ba, bits) +
        b"data" + struct.pack("<I", len(data)) + data)
wav("u8", 1, 8, bytes(((v >> 8) + 128) & 255 for v in s))
wav("s24", 1, 24, b"".join(struct.pack("<i", v << 8)[:3] for v in s))
wav("s32", 1, 32, struct.pack("<%di" % len(s), *[v << 16 for v in s]))
wav("f32", 3, 32, struct.pack("<%df" % len(s), *[v / 32768 for v in s]))
PYEOF
    local fail=0
    for f in u8 s24 s32 f32; do
        "$RX" --rx --file "$TMPD/$f.wav" 1200 > "$TMPD/$f.out" 2> /dev/null
        if cmp -s testdata-ascii.txt "$TMPD/$f.out"; then echo "OK $f"; else echo "FAIL $f"; fail=1; fi
    done
    return $fail
}

# MFSK: the sender's clock is off by 2%: we write the audio with 8160 Hz
# and change the sample rate in the WAV header to 8000 Hz
mfsk_clock_test() {
    local fail=0
    for mode in mfsk4 mfsk16 mfsk64; do
        for tx_rate in 7840 8160; do
            "$TX" --tx --file "$TMPD/test.wav" -R $tx_rate $mode < testdata-ascii.txt || return 1
            python3 - "$TMPD/test.wav" <<'PYEOF'
import struct, sys
f = open(sys.argv[1], "r+b")
f.seek(24)
f.write(struct.pack("<II", 8000, 16000))
PYEOF
            "$RX" --rx --file "$TMPD/test.wav" $mode > "$TMPD/test.out" 2> /dev/null
            if cmp -s testdata-ascii.txt "$TMPD/test.out"; then
                echo "OK $mode sent with $tx_rate Hz"
            else
                echo "FAIL $mode sent with $tx_rate Hz"; fail=1
            fi
        done
    done
    return $fail
}

# binary data (all byte values) with packets
packets_binary_test() {
    python3 -c "import sys; sys.stdout.buffer.write(bytes(range(256)) * 4)" > "$TMPD/binary.dat"
    self_test "$TMPD/binary.dat" --packets "$@"
}

# AX.25: text -> KISS -> text
ax25_kiss_test() {
    "$TX" --tx --file "$TMPD/text.wav" aprs < testdata-aprs.txt || return 1
    "$RX" --rx --kiss -q --file "$TMPD/text.wav" aprs > "$TMPD/frames.kiss" || return 1
    "$TX" --tx --kiss --file "$TMPD/kiss.wav" aprs < "$TMPD/frames.kiss" || return 1
    "$RX" --rx -q --file "$TMPD/kiss.wav" aprs > "$TMPD/test.out" || return 1
    cmp testdata-aprs.txt "$TMPD/test.out" || return 1
    echo "OK text -> KISS -> text"
}

echo "KAMAL" > "$TMPD/kamal.txt"

case "$TEST" in
    self-test-1200)       self_test testdata-ascii.txt 1200 ;;
    self-test-300)        self_test testdata-ascii.txt 300 ;;
    self-test-rtty)       self_test testdata-baudot.txt rtty ;;
    self-test-0.5)        self_test "$TMPD/kamal.txt" 0.5 ;;
    self-test-12000)      self_test testdata-ascii.txt 12000 ;;
    self-test-8000hz)     self_test testdata-ascii.txt 1200 --samplerate 8000 ;;
    self-test-22050hz)    self_test testdata-ascii.txt 300 --samplerate 22050 ;;
    verify-perfect)       self_test -P testdata-ascii.txt 1200 --samplerate 24000 -M 1200 -S 2400 ;;
    verify-tx-consistent) tx_consistent_test ;;
    tx-wav-header)        tx_wav_header_test ;;
    rate-slop)            rate_slop_test ;;
    amplitude)            amplitude_test ;;
    noise)                noise_test 1200 ;;
    noise-purefreqs)      noise_test 1200 -M 1200 -S 2400 ;;
    multibyte)            self_test testdata-multibyte.txt 1200 ;;
    callerid-mdmf)        callerid_test testdata-callerid-mdmf.bytes ;;
    callerid-sdmf)        callerid_test testdata-callerid-sdmf.bytes ;;
    same)                 self_test testdata-ascii.txt SAME ;;
    ascii7)               self_test testdata-ascii.txt -7 1200 ;;
    tdd)                  self_test testdata-baudot.txt tdd ;;
    baudot-300)           self_test testdata-baudot.txt -5 300 ;;
    v21)                  self_test testdata-ascii.txt V.21 ;;
    msb-first)            self_test testdata-ascii.txt 1200 --msb-first ;;
    invert-start-stop)    self_test testdata-ascii.txt 1200 --invert-start-stop ;;
    inverted)             self_test testdata-ascii.txt 1200 -i ;;
    start-stop-bits)      self_test testdata-ascii.txt 1200 --startbits 2 --stopbits 2 ;;
    sync-byte)            self_test testdata-ascii.txt 1200 --sync-byte 0xAB ;;
    auto-carrier)         self_test testdata-ascii.txt 1200 -M 1600 -S 2600 -- 1200 -a ;;
    wav-formats)          wav_formats_test ;;
    # tests of the extensions, which are not supported by the original
    mfsk4)                self_test testdata-ascii.txt mfsk4 ;;
    mfsk8)                self_test testdata-ascii.txt mfsk8 -R 22050 ;;
    mfsk16)               self_test testdata-ascii.txt mfsk16 -R 8000 ;;
    mfsk32)               self_test testdata-multibyte.txt mfsk32 -R 8000 ;;
    mfsk64)               self_test testdata-ascii.txt mfsk64 -R 44100 ;;
    mfsk-noise)           self_test testdata-ascii.txt mfsk16 -R 8000 -- mfsk16 --Xrxnoise 1.0 ;;
    mfsk-clock)           mfsk_clock_test ;;
    mfsk-base-freq)       self_test testdata-ascii.txt mfsk16 -M 500 ;;
    mfsk-tx-carrier)      ! "$TX" --tx --tx-carrier --file "$TMPD/test.wav" mfsk16 < /dev/null ;;
    packets-fsk)          self_test testdata-ascii.txt --packets 1200 -- --packets --Xrxnoise 0.3 1200 ;;
    packets-mfsk)         self_test testdata-ascii.txt --packets -R 8000 mfsk16 -- --packets --Xrxnoise 1.2 mfsk16 ;;
    packets-binary)       packets_binary_test 1200 ;;
    ax25)                 self_test testdata-aprs.txt aprs ;;
    ax25-22050hz)         self_test testdata-aprs.txt aprs -R 22050 ;;
    ax25-noise)           self_test testdata-aprs.txt aprs -- aprs --Xrxnoise 0.5 ;;
    ax25-kiss)            ax25_kiss_test ;;
    psk31)                self_test testdata-ascii.txt psk31 -R 8000 ;;
    psk125)               self_test testdata-multibyte.txt psk125 ;;
    psk500)               self_test testdata-ascii.txt psk500 -R 22050 ;;
    psk-noise)            self_test testdata-ascii.txt psk63 -R 8000 -- psk63 --Xrxnoise 1.0 ;;
    psk-offset)           self_test testdata-ascii.txt psk31 -M 1005 -- psk31 -M 1000 ;;
    packets-psk)          self_test testdata-ascii.txt --packets psk125 -- --packets --Xrxnoise 1.0 psk125 ;;
    ofdm16)               self_test testdata-ascii.txt ofdm16 -R 8000 ;;
    ofdm32)               self_test testdata-multibyte.txt ofdm32 ;;
    ofdm64)               self_test testdata-ascii.txt ofdm64 -R 22050 ;;
    ofdm-noise)           self_test testdata-ascii.txt ofdm32 -R 8000 -- ofdm32 --Xrxnoise 0.1 ;;
    packets-ofdm)         self_test testdata-ascii.txt --packets -R 8000 ofdm32 -- --packets --Xrxnoise 0.12 ofdm32 ;;
    ggwave-fast)          self_test testdata-ascii.txt ggwave-fast ;;
    ggwave-fastest)       self_test testdata-multibyte.txt ggwave-fastest -R 24000 ;;
    ggwave-u-normal)      self_test testdata-multibyte.txt ggwave-u-normal ;;
    ggwave-dt-fast)       self_test testdata-multibyte.txt ggwave-dt-fast -R 24000 ;;
    ggwave-auto-speed)    self_test testdata-multibyte.txt ggwave-fastest -- ggwave-normal ;;
    ggwave-noise)         self_test testdata-ascii.txt ggwave-fast -- ggwave-fast --Xrxnoise 0.5 ;;
    ggwave-44100hz)       ! "$TX" --tx --file "$TMPD/test.wav" -R 44100 ggwave < /dev/null 2> /dev/null ;;
    packets-ggwave)       self_test testdata-ascii.txt --packets ggwave-fast -- --packets --Xrxnoise 0.5 ggwave-fast ;;
    *) echo "unknown test: $TEST" 1>&2; exit 2 ;;
esac
