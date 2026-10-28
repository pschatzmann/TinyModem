# Command line tool

`tools/tinymodem` is a desktop program which supports all modem types of TinyModem. Data is read from stdin (transmit) or written to stdout (receive); the status messages (`### CARRIER` / `### NOCARRIER`) go to stderr. For the FSK modes, the options and the output are the same as minimodem's, so `tinymodem` can replace minimodem in scripts.

## Build

You need CMake ≥ 3.18 and a C++17 compiler. PortAudio (`portaudio19-dev` on Debian/Ubuntu) is optional: with it you can use the sound card, without it only `--file` works.

```sh
cmake -S . -B build
cmake --build build
sudo cmake --install build   # optional: installs bin/tinymodem
```

The arduino-audio-tools are taken from `../arduino-audio-tools` if that folder exists (the usual Arduino `libraries` layout). Otherwise they are downloaded. Use `-DAUDIO_TOOLS_DIR=/path` to select another copy.

| CMake option | Default | |
|---|---|---|
| `TINYMODEM_BUILD_TOOLS` | ON | Build the command line tool |
| `TINYMODEM_BUILD_TESTS` | ON | Build the tests |
| `TINYMODEM_PORTAUDIO` | ON | Use PortAudio if found |
| `MINIMODEM_REFERENCE` | auto | Original minimodem for the interop tests |
| `DIREWOLF_ATEST`, `DIREWOLF_GEN_PACKETS` | auto | direwolf tools for the AX.25 interop tests |

## Usage

```
tinymodem [--tx|--rx] [options] {mode}
```

```sh
# transmit to a WAV file and decode it again
echo "Hello" | ./build/tinymodem --tx -f hello.wav mfsk16
./build/tinymodem --rx -f hello.wav mfsk16

# sound card (PortAudio)
echo "Hello" | ./build/tinymodem --tx 1200
./build/tinymodem --rx 1200            # Ctrl-C to stop

# reliable file transfer with error correction
./build/tinymodem --tx --packets -f data.wav ofdm32 < file.bin
./build/tinymodem --rx --packets -f data.wav ofdm32 > file.bin

# APRS
echo "N0CALL>APRS:>Hello" | ./build/tinymodem --tx -f aprs.wav aprs
./build/tinymodem --rx -f aprs.wav aprs
```

Without `--file`, transmit is interactive like minimodem: the transmission is ended when stdin pauses for more than one symbol, and `--tx-carrier` keeps the idle signal on (FSK, PSK, AX.25).

## Modes

| Mode | Modem type | |
|---|---|---|
| `1200`, `300`, any number, `rtty`, `tdd`, `same`, `V.21`, `callerid`, `uic-train`, `uic-ground` | [FSK](fsk.md) | compatible with minimodem |
| `mfsk4`, `mfsk8`, `mfsk16`, `mfsk32`, `mfsk64` | [MFSK](mfsk.md) | |
| `psk31`, `psk63`, `psk125`, `psk250`, `psk500` | [PSK](psk.md) | |
| `ofdm16`, `ofdm32`, `ofdm64` | [OFDM](ofdm.md) | |
| `aprs`, `ax25` | [AX.25](ax25.md) | compatible with direwolf |

## Options

| Option | Modem types | |
|---|---|---|
| `-t`, `--tx` / `-r`, `--rx` | all | Transmit / receive (default) |
| `-f`, `--file {file.wav}` | all | WAV file instead of the sound card |
| `-R`, `--samplerate {rate}` | all | Sample rate (default 48000) |
| `-v`, `--volume {amplitude}` | all | Transmit amplitude (`E` = epsilon) |
| `-q`, `--quiet` | all | No status messages |
| `--Xrxnoise {level}` | all | Testing: add uniform noise to the input |
| `--packets` | all except AX.25 | [Packets](packets.md) with CRC and Reed-Solomon error correction |
| `--rx-one` | FSK, MFSK, PSK, OFDM | Stop after the first transmission |
| `-M`, `--mark {freq}` | all | FSK and AX.25: mark tone, MFSK: lowest tone, PSK: carrier, OFDM: first carrier |
| `-S`, `--space {freq}` | FSK, AX.25 | Space tone |
| `--tx-carrier` | FSK, PSK, AX.25 | Keep the idle signal on while stdin pauses |
| `--print-eot` | all | Print `### EOT` at the end of each transmission |
| `--kiss` | AX.25 | KISS frames instead of the monitor text format |
| `--benchmarks` | | Throughput of the FSK and MFSK modems |
| `-A`, `--alsa`, `-s`, `--sndio` | | Use the default PortAudio device |
| `-c`, `-l`, `-a`, `-i`, `-8`, `-7`, `-5`, `-u`, `-b`, `--startbits`, `--stopbits`, `--invert-start-stop`, `--sync-byte`, `--msb-first`, `--binary-output`, `--binary-raw`, `--print-filter` | FSK | See [FSK](fsk.md#modemconfig-fields) |

## Differences to minimodem

- Only WAV files are supported (8, 16, 24 or 32-bit PCM, or 32-bit float, mono or multi-channel, where the first channel is decoded). The original supports all libsndfile formats, like FLAC.
- Transmit always writes 16-bit PCM. `--float-samples` and `--lut` are accepted but have no effect.
- `--alsa` and `--sndio` use the default PortAudio device. A device name is ignored.
- `--Xrxnoise` adds real uniform noise. In the original, an integer division makes it a constant DC offset.
- The modes MFSK, PSK, OFDM, AX.25 and the options `--packets` and `--kiss` are extensions.
