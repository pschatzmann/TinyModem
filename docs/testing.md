# Tests

```sh
cmake -S . -B build
cmake --build build
cd build && ctest -j8
```

There are four groups of tests (164 tests with all reference programs installed):

| Group | Prefix | What it tests |
|---|---|---|
| Unit tests | `unit-` | The library in memory: encoder → decoder loopback of all modems and modes, sample rates, input formats, chunk sizes, noise, different clocks, frequency offsets, echoes, amplitude, many transmissions in a row, the codecs (Baudot, Caller-ID, UIC, Varicode, AX.25 frames, KISS), the packets, the streams (also with a receive task), and the AudioTools pipeline. Source: [tests/unit/unit-tests.cpp](../tests/unit/unit-tests.cpp) |
| CLI tests | `cli-` | `tinymodem` transmits a WAV file and decodes it again. For FSK, this is the original minimodem test suite ported to [tests/cli-test.sh](../tests/cli-test.sh); the other tests cover MFSK, PSK, OFDM, AX.25, KISS and packets. |
| minimodem interop tests | `interop-tx-`, `interop-rx-` | The FSK CLI tests with the original minimodem on one side: `interop-tx` means we transmit and the original receives, `interop-rx` the reverse. |
| direwolf interop tests | `interop-ax25-` | AX.25 with direwolf: direwolf decodes our frames (`tx`), we decode direwolf's frames (`rx`), and we decode direwolf's 100 frames with increasing noise (`noise`, at least 60 must be decoded). Source: [tests/ax25-interop.sh](../tests/ax25-interop.sh) |

MFSK, PSK, OFDM and the packets are tested with TinyModem on both sides: there is no reference program for them. The PSK Varicode table is generated from fldigi's source, but the interoperability with fldigi is not tested.

Run a single test with `ctest -R unit-noise --output-on-failure`, or call the programs directly:

```sh
./build/tests/unit-tests mfsk-robust
bash tests/cli-test.sh amplitude ./build/tinymodem /usr/bin/minimodem
```

To check the stream classes with a receive task for data races, build the unit tests with ThreadSanitizer. On newer Linux kernels, `setarch -R` is needed to disable address randomization:

```sh
g++ -std=c++17 -g -O1 -fsanitize=thread -DIS_MIN_DESKTOP -DNO_MAIN -Wno-format \
  -DTEST_DATA_DIR=\"$PWD/tests\" -I../arduino-audio-tools/src -Isrc \
  tests/unit/unit-tests.cpp -o unit-tsan -lpthread
setarch -R ./unit-tsan stream-rtos
setarch -R ./unit-tsan mfsk-stream
```

## Interoperability tests

The minimodem interop tests run only if the original minimodem is found in the `PATH` (`sudo apt install minimodem`), or if it is given explicitly:

```sh
cmake -S . -B build -DMINIMODEM_REFERENCE=/path/to/minimodem
```

The original passes the `verify-perfect` test with `confidence=inf`. Our float Goertzel filters leave a tiny amount of noise, so with our receiver that test checks only for `(rate perfect)`.

The direwolf interop tests run if `atest` and `gen_packets` are found (`sudo apt install direwolf`). To use an unpacked direwolf package, define the programs and the location of its shared libraries:

```sh
cmake -S . -B build -DDIREWOLF_ATEST=/path/atest -DDIREWOLF_GEN_PACKETS=/path/gen_packets \
  -DDIREWOLF_LIBRARY_PATH=/path/lib
```

The CI installs both minimodem and direwolf.

## Arduino

The examples are compiled in CI with `arduino-cli` for ESP32 and RP2040. The `modem-stream-rtos` example is compiled for the RP2040 with FreeRTOS (`rp2040:rp2040:rpipico:os=freertos`). To check them locally (the `--library` options aren't needed if both libraries are installed in your Arduino `libraries` folder):

```sh
arduino-cli compile -b esp32:esp32:esp32 --library . --library ../arduino-audio-tools examples/loopback
```

On a board, the [benchmark](../examples/benchmark) example shows whether the modems run fast enough.
