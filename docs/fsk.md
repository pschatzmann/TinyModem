# FSK (minimodem)

Frequency shift keying with two tones: the mark tone is a 1 bit, the space tone a 0 bit. The bits are sent in frames with start and stop bits (e.g. 8-N-1), like the classic Bell 103 and Bell 202 modems. The FSK modem is a rewrite of [minimodem](https://github.com/kamalmostafa/minimodem) 0.24, so the library and the `tinymodem` tool can exchange data with minimodem in both directions.

**Advantages**

- Compatible with minimodem and with many standard modes: Bell 103, Bell 202, V.21, RTTY, TTY/TDD and NOAA SAME. Caller-ID and UIC-751-3 can be received.
- Any baud rate: with a wide audio path up to 12000 baud (1200 bytes/s) at 48000 Hz.
- Simple, well understood and tested against the original.

**Disadvantages**

- Sensitive to noise: a wrong bit changes a byte, a missed start bit loses it.
- Binary data needs a baud rate which divides the sample rate (see [fastest settings](#fastest-settings)).
- High baud rates need high frequencies, which speakers and microphones can't reproduce.
- No error detection: use [packets](packets.md) if the data must be correct.

## Usage

```cpp
ModemConfig cfg;
cfg.setMode("1200");     // Bell 202
cfg.sample_rate = 22050;
ModemStream modem(i2s, RXTX_MODE);
modem.begin(cfg);
```

```sh
echo "Hello" | tinymodem --tx -f hello.wav 1200
tinymodem --rx -f hello.wav 1200
```

`ModemConfig` (FSK), `FSKEncoder`, `FSKDecoder`, `ModemStream` and `ModemStreamRTOS` are the FSK classes. See the [Arduino API](arduino.md) and the [command line tool](command-line.md).

Call `setMode()` first and then change individual fields: `setMode()` resets all mode related fields (baud rate, frequencies, bandwidth, framing, sync and format), so the result doesn't depend on a previous mode. Other settings like `volume`, `inverted` or the audio format are kept. Fields left at `0` (or `-1`) are derived the same way minimodem derives them when the encoder or decoder starts. `config()` returns the configuration you passed to `begin()`; `activeConfig()` returns it with all derived values.

## Modes

`ModemConfig::setMode()` and the command line `{baudmode}` argument accept the following modes. On the command line, `-5`, `-7`, `-8` and `--stopbits` override the defaults of `rtty`, `tdd` and the numeric modes, like in minimodem.

| Mode | Description | Baud | Mark / Space (Hz) | Data | Start / Stop | TX |
|---|---|---|---|---|---|---|
| `N` (any number) | Bell-like | N | see below | 8 ASCII | 1 / 1 | yes |
| `1200` | Bell 202 | 1200 | 1200 / 2200 | 8 ASCII | 1 / 1 | yes |
| `300` | Bell 103 | 300 | 1270 / 1070 | 8 ASCII | 1 / 1 | yes |
| `rtty` | RTTY | 45.45 | 1585 / 1415 | 5 Baudot | 1 / 1.5 | yes |
| `tdd` | TTY/TDD | 45.45 | 1400 / 1800 | 5 Baudot | 1 / 2 | yes |
| `same` | NOAA SAME | 520.83 | 2083.3 / 1562.5 | 8, sync 0xAB | 0 / 0 | yes |
| `V.21` | V.21 | 300 | 980 / 1180 | 8 ASCII | 1 / 1 | yes |
| `callerid` | Bell 202 Caller-ID (SDMF/MDMF) | 1200 | 1200 / 2200 | 8 | 1 / 1 | no |
| `uic-train`, `uic-ground` | UIC-751-3 | 600 | 1300 / 1700 | 39 | 8 / 0 | no |

Default frequencies for a numeric baud rate N:

- N ≥ 400: mark = N/2 + 600, space = mark + 5N/6, bandwidth 200
- 100 ≤ N < 400: mark = 1270, space = mark − 200, bandwidth 50
- N < 100: mark = 1585, space = mark − 170, bandwidth 10

The receive bandwidth is never larger than the baud rate.

## ModemConfig fields

| Field | Command line | Description |
|---|---|---|
| `baud_rate` | `{baudmode}` | Bits per second |
| `mark_freq` / `space_freq` | `-M` / `-S` | Tone frequencies (0 = auto) |
| `bandwidth` | `-b` | Receive filter bandwidth (0 = auto) |
| `data_bits` | `-8`, `-7`, `-5` | Data bits per frame |
| `format` | `-5` / mode | `ASCII`, `BAUDOT`, `BINARY`, `CALLERID`, `UIC_TRAIN`, `UIC_GROUND` |
| `start_bits` / `stop_bits` | `--startbits` / `--stopbits` | Framing (stop bits can be fractional) |
| `invert_start_stop` | `--invert-start-stop` | Swap start and stop bit values |
| `inverted` | `-i` | Swap mark and space |
| `msb_first` | `--msb-first` | Send the most significant bit first |
| `rx_sync`, `tx_sync_bytes`, `sync_byte` | `--sync-byte` | Sync byte preamble (16 bytes) |
| `confidence` | `-c` | Minimum confidence (squelch), default 1.5 |
| `search_limit` | `-l` | Stop the frame search above this confidence, default 2.3 |
| `auto_carrier` | `-a` | Detect the carrier frequency automatically |
| `volume` | `-v` | Transmit amplitude (`E` = epsilon) |
| `rx_one` | `--rx-one` | Stop after the first carrier loss |
| `binary_output` | `--binary-output` | Print the data bits as `0`/`1` |
| `binary_raw_bits` | `--binary-raw` | Print N raw bits without framing |
| `baudot_usos` | `-u` | Baudot unshift on space (default on) |
| `print_filter` | `--print-filter` | Replace non-printable characters with `.` |
| `rx_noise` | `--Xrxnoise` | Testing only: add uniform noise to the input |
| `leader_bits` / `trailer_bits` | – | Mark bits before / after a transmission (2 / 2) |
| `channel` | – | Input channel used for decoding |
| `buffer_divisor` | – | Minimum receive buffer is `sample_rate / buffer_divisor` |
| `sample_rate`, `channels`, `bits_per_sample` | `-R` | Audio format |

## Fastest settings


The data rate is `baud / 10` bytes per second for 8-N-1 frames, so a higher baud rate means faster transfers. For a numeric baud rate N ≥ 400 the tones are mark = N/2 + 600 Hz and space = mark + 5N/6 Hz.

The following results were measured with the command line tool in a digital loopback (WAV files with the same sample rate on both sides, about 4000 random characters per test, one run per setting). The original minimodem decoded exactly the same settings as our receiver.

**Use a baud rate that divides the sample rate**, in particular for binary data: 1200 baud at 8000 Hz (6.67 samples per bit) decodes text, but corrupts bytes with the highest bit set, also with the original minimodem. With a whole number of samples per bit (we tested `sample_rate / n` for n = 4, 5, 6, 8, 10 and 12), every rate was decoded without errors. Other rates often failed even when they were lower, for example 1500 baud at 8000 Hz, 2700 at 16000 Hz, 3000 at 22050 Hz or 4800 at 44100 Hz. The rates above `sample_rate / 4` that we tested (2200 baud at 8000 Hz, 13200 at 48000 Hz) failed.

| Sample rate | Fastest | Mark / Space (Hz) | Fastest with noise (16 dB SNR) | Mark / Space (Hz) |
|---|---|---|---|---|
| 8000 Hz | 2000 baud (200 bytes/s) | 1600 / 3267 | 1600 baud (160 bytes/s) | 1400 / 2733 |
| 16000 Hz | 4000 baud (400 bytes/s) | 2600 / 5933 | 2000 baud (200 bytes/s) | 1600 / 3267 |
| 22050 Hz | 5512.5 baud (551 bytes/s) | 3356 / 7950 | 1837.5 baud (184 bytes/s) | 1519 / 3050 |
| 44100 Hz | 11025 baud (1102 bytes/s) | 6113 / 15300 | 11025 baud (1102 bytes/s) | 6113 / 15300 |
| 48000 Hz | 12000 baud (1200 bytes/s) | 6600 / 16600 | 12000 baud (1200 bytes/s) | 6600 / 16600 |

The noise column adds uniform noise with `--Xrxnoise 0.2`, about 16 dB signal-to-noise ratio. At 22050 Hz, 2756.25 baud still worked at about 22 dB.

Example: the fastest setting at 48000 Hz.

```cpp
ModemConfig cfg;
cfg.setMode("12000");
cfg.sample_rate = 48000;
```

```sh
echo "Hello" | tinymodem --tx -R 48000 12000   # sender
tinymodem --rx -R 48000 12000                  # receiver
```

### Settings for real audio

These results are for a perfect digital channel. A real audio path (DAC, cable or speaker and microphone, ADC) has a limited frequency range and adds distortion, so leave a margin and test with your hardware:

- **Check the space tone**: it must be inside the frequency range of your audio path. 12000 baud needs 16.6 kHz, which many speakers and microphones and all telephone lines can't transmit. For a speaker and a microphone, stay below about 3–4 kHz, for example 1600 baud at 8000 Hz, or the standard 1200 baud (1200 / 2200 Hz). With a cable from line out to line in, the high rates are usually fine.
- **Use the same sample rate on both sides**. If they differ, the baud rate must still be a whole fraction of the receiver's sample rate.
- **Use the standard modes** (`1200`, `300`) if you have to talk to other modems.

### Fastest receiver

The decoder's CPU load depends mostly on the sample rate. Decoding 1200 baud took the following on a PC (multiples of real time; a microcontroller is much slower, check it with the [benchmark](../examples/benchmark) example):

| Sample rate | 1200 baud | Note |
|---|---|---|
| 8000 Hz | 1441× | |
| 16000 Hz | 782× | |
| 22050 Hz | 511× | |
| 48000 Hz | 555× | 12000 baud: 268× |

- Use the lowest sample rate that supports your baud rate: 8000 Hz needs about 2.6 times less CPU than 48000 Hz.
- Don't use `auto_carrier` (`-a`): it costs more CPU and is unreliable. In the tests it lost data at 8000 Hz and at 48000 Hz (where it locked onto 1000 Hz instead of 1200 Hz), with exactly the same output as the original minimodem.
- Lowering `search_limit` (`-l`) made no measurable difference.
- `cfg.buffer_divisor = 0` reduces the memory, not the CPU load.
- On ESP32 and RP2040 use the `ModemStreamRTOS`, so the decoding runs in its own task.
