# MFSK (multi-tone FSK)

Instead of 2 tones, MFSK uses 4 to 64 tones: each symbol is one tone and carries 2 to 6 bits. At the same data rate the symbols are much longer than FSK bits, which makes MFSK very robust against noise, echoes and different clocks of the sender and the receiver. It is the best choice for speakers and microphones. MFSK is an extension of TinyModem: it is not compatible with other programs.

**Advantages**

- Very robust against noise: `mfsk64` decodes without errors when the noise is stronger than the signal.
- The receiver adapts to the clock of the sender: different clocks of ±3% don't matter.
- Long symbols are robust against echoes.
- All tones are between 1000 and 3000 Hz (telephone band).

**Disadvantages**

- Slower than FSK and OFDM: 23 to 125 bytes/s.
- More tones need more CPU in the decoder.
- Not compatible with other programs.

## Modes

| Mode | Tones | Symbols/s | Data rate | Tone spacing |
|---|---|---|---|---|
| `mfsk4` | 4 | 500 | 1000 bit/s (125 bytes/s) | 500 Hz |
| `mfsk8` | 8 | 250 | 750 bit/s (94 bytes/s) | 250 Hz |
| `mfsk16` | 16 | 125 | 500 bit/s (62 bytes/s) | 125 Hz |
| `mfsk32` | 32 | 62.5 | 312.5 bit/s (39 bytes/s) | 62.5 Hz |
| `mfsk64` | 64 | 31.25 | 187.5 bit/s (23 bytes/s) | 31.25 Hz |

Fewer tones are faster, more tones are more robust. `mfsk16` is the default.

## Usage

```cpp
I2SStream i2s;
MFSKStream modem(i2s, RXTX_MODE);

void setup() {
  MFSKConfig cfg = modem.defaultConfig();  // mfsk16
  cfg.sample_rate = 22050;
  ...
  modem.begin(cfg);
}
```

```sh
echo "Hello" | tinymodem --tx -f hello.wav mfsk16
tinymodem --rx -f hello.wav mfsk16
```

`MFSKConfig`, `MFSKEncoder`, `MFSKDecoder`, `MFSKStream` and `MFSKStreamRTOS` are the MFSK classes. See the [mfsk-loopback](../examples/mfsk-loopback) and [packet-modem](../examples/packet-modem) examples. On the command line, `-R`, `-v`, `-M` (lowest tone), `-q`, `--rx-one`, `--packets` and `--Xrxnoise` are supported.

| `MFSKConfig` field | Default | |
|---|---|---|
| `tones` | 16 | 4, 8, 16, 32 or 64 |
| `symbol_rate` | 125 | Symbols per second |
| `base_freq` | 1000 | Frequency of the lowest tone (Hz) |
| `tone_spacing` | 0 | Distance between the tones (Hz), 0 = `symbol_rate` |
| `volume` | 1.0 | Transmit amplitude |
| `preamble_symbols` | 8 | Length of the preamble |
| `min_amplitude` | 0.005 | Receiver: minimum signal level (1.0 = full scale) |
| `rx_one` | false | Receiver: stop after the first transmission |
| `channel` | 0 | Receiver: input channel |
| `sample_rate`, `channels`, `bits_per_sample` | 48000, 1, 16 | Audio format |

Call `setMode("mfsk32")` first and then change individual fields. The highest tone must be below half the sample rate, and a symbol needs at least 8 samples.

## How it works

A transmission consists of:

1. A **preamble** of 8 symbols, alternating between the lowest and the highest tone. The receiver uses it to find the start of the symbols and to measure the clock of the sender.
2. **Sync symbols** which carry at least 8 bits (`tones / 2`, `tones / 4`, …). They mark the start of the data.
3. The **data**: each symbol carries log2(tones) bits, most significant bit first. The tones are Gray coded, so a confusion with a neighbouring tone only changes one bit.
4. Two symbols of **silence**: the receiver detects the end of the transmission when the signal is lost.

The receiver analyses each symbol with one Goertzel filter per tone (AudioTools `GoertzelDetector`). The symbol timing is tracked with an early/late gate and a second order loop, which also learns a constant drift. If the sender's clock differs, all tones are shifted by the same factor: the receiver measures this factor with the highest preamble tone and retunes all tones and the symbol length.

## Measured performance

Loopback with the unit tests at 8000 Hz. Noise is uniform noise which is added in the receiver (`rx_noise`, `--Xrxnoise`): 0.5 corresponds to a signal-to-noise ratio of about 7.8 dB, 1.0 to 1.8 dB and 1.5 to −1.8 dB.

| | Without errors up to |
|---|---|
| `mfsk4` noise | 0.8 |
| `mfsk16` noise | 1.0 |
| `mfsk64` noise | 1.5 |
| Clock difference of sender and receiver | ±3% (all modes) |
| Volume | 0.01 (1% of full scale) |

All modes also decode 50 short transmissions in a row with 2% clock difference and noise 0.5 at 8000 Hz and at 48000 Hz. On a PC at 8000 Hz, the decoder ran about 2500 (`mfsk4`), 1100 (`mfsk16`) and 300 (`mfsk64`) times faster than real time. Check your board with the [benchmark](../examples/benchmark) example.
