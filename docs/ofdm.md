# OFDM

Orthogonal frequency division multiplexing: the data is sent in parallel on 16 to 64 carriers between 1000 and 3000 Hz. Each carrier transmits 2 bits per symbol with differential QPSK (a phase change of 0, 90, 180 or 270 degrees), so the receiver doesn't need to know the phase of the channel. Each symbol starts with a cyclic prefix (a copy of its end), which absorbs echoes. OFDM is an extension of TinyModem: it is not compatible with other programs.

**Advantages**

- The fastest modem in the telephone band: 3200 bit/s (about 380 bytes/s).
- Echoes shorter than the cyclic prefix (2 ms for `ofdm16`, 4 ms for `ofdm32`, 8 ms for `ofdm64`) don't matter.
- The data is sent in frames with a length header: the receiver knows exactly where a frame ends.

**Disadvantages**

- Needs a good signal: errors start at a lower noise level than with the other modems. Use it with [packets](packets.md).
- Very sensitive to different clocks of sender and receiver: about ±0.2% (`ofdm64`: ±0.1%).
- The data is sent in frames of up to 512 bytes: there is a delay until a frame is full or `flush()` is called.
- The sum of the carriers has high peaks, so the signal is sent with a lower average level (0.25 of full scale).
- Not compatible with other programs.

## Modes

| Mode | Carriers | Spacing | Symbol + prefix | Data rate |
|---|---|---|---|---|
| `ofdm16` | 16 | 125 Hz | 8 + 2 ms | 3200 bit/s |
| `ofdm32` | 32 | 62.5 Hz | 16 + 4 ms | 3200 bit/s |
| `ofdm64` | 64 | 31.25 Hz | 32 + 8 ms | 3200 bit/s |

All modes have the same data rate: more carriers result in longer symbols, which are more robust against echoes, but more sensitive to different clocks. `ofdm32` is the default.

## Usage

```cpp
I2SStream i2s;
OFDMStream modem(i2s, RXTX_MODE);
PacketStream<64, 16> packets(modem);  // recommended

void setup() {
  OFDMConfig cfg = modem.defaultConfig();  // ofdm32
  cfg.sample_rate = 22050;
  ...
  modem.begin(cfg);
  packets.begin();
}

void loop() {
  packets.println("Hello");
  packets.flush();  // sends the packet and the OFDM frame
}
```

```sh
tinymodem --tx --packets -f data.wav ofdm32 < file.bin
tinymodem --rx --packets -f data.wav ofdm32 > file.bin
```

`OFDMConfig`, `OFDMEncoder`, `OFDMDecoder`, `OFDMStream` and `OFDMStreamRTOS` are the OFDM classes. The encoder collects the written bytes and sends a frame when 512 bytes were written or when `flush()` (or `endTransmission()`) is called. On the command line, `-R`, `-v`, `-M` (first carrier), `-q`, `--rx-one`, `--packets` and `--Xrxnoise` are supported.

| `OFDMConfig` field | Default | |
|---|---|---|
| `carriers` | 32 | Number of carriers (4 - 128) |
| `carrier_spacing` | 62.5 | Distance of the carriers (Hz) |
| `first_freq` | 1000 | Frequency of the first carrier (Hz) |
| `guard` | 0.25 | Length of the cyclic prefix relative to the symbol |
| `volume` | 1.0 | Transmit amplitude |
| `max_frame` | 512 | Maximum number of bytes per frame |
| `sync_threshold` | 0.5 | Receiver: minimum correlation of the sync symbol |
| `rx_one` | false | Receiver: stop after the first frame |
| `channel` | 0 | Receiver: input channel |
| `sample_rate`, `channels`, `bits_per_sample` | 48000, 1, 16 | Audio format |

## How it works

A frame consists of:

1. A **sync symbol** which only uses the even carriers: its two halves are identical. The receiver finds it with the method of Schmidl & Cox, a sliding autocorrelation between the two halves, and uses the middle of the correlation plateau (the cyclic prefix) as timing.
2. A **reference symbol** with known phases on all carriers. Together with the sync symbol it provides the initial phase of each carrier and an estimate of the frequency offset and the timing drift.
3. The **header**: the length of the frame and its inverse (32 bits). Each carrier transmits one bit (0 or 180 degrees) and the bits are repeated on all carriers of the header symbols, which the receiver adds up. This makes the header more robust than the data.
4. The **data symbols**: 2 bits per carrier and symbol (differential QPSK with Gray code).

The carriers are analysed with a DFT. The receiver tracks the frequency offset and the timing drift with the decisions of each symbol and moves its DFT window when the timing has drifted by one sample.

## Measured performance

Loopback with the unit tests at 8000 Hz. The noise levels refer to the OFDM signal with an average level of 0.25: a noise level of 0.1 corresponds to a signal-to-noise ratio of about 13 dB.

| | `ofdm16` | `ofdm32` | `ofdm64` |
|---|---|---|---|
| Noise without errors | 0.1 | 0.1 | 0.1 |
| Noise 0.15: wrong bytes of 440 | 7 | 3 | 7 |
| Echo of 50% without errors | 2 ms | 4 ms | 4 ms |
| Clock difference without errors | ±0.2% | ±0.2% | −0.2% to +0.1% |

With packets, all 64 packets of a 4 KB file were received with noise 0.12, and 62 of 64 with noise 0.15. On a PC at 8000 Hz, the decoder ran 950 (`ofdm64`) to 2250 (`ofdm16`) times faster than real time.
