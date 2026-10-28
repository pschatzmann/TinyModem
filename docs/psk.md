# PSK (PSK31 to PSK500)

Binary phase shift keying of a single carrier like PSK31, the keyboard chat mode of amateur radio: a 0 bit is a phase reversal, a 1 bit keeps the phase. The amplitude of a phase reversal follows a cosine, so the signal is very narrow. The characters are sent with the Varicode of PSK31, in which frequent characters have short codes; the characters are separated by "00".

**Advantages**

- Very narrow band: about 60 Hz for PSK31, so many signals fit into one audio channel.
- Very robust against noise, efficient for text.
- The decoder needs little CPU.
- The receiver tracks the frequency of the signal (automatic frequency control) and the symbol timing.
- The format of PSK31 to PSK500 in amateur radio programs like fldigi. The Varicode table is taken from fldigi, but the interoperability has not been tested.

**Disadvantages**

- Slow: PSK31 transmits about 4 characters per second.
- The Varicode is optimized for text: binary data is slower, because bytes above 127 have codes of up to 12 bits (fldigi's extension of the Varicode).
- The receiver only finds a signal whose frequency is close to the carrier: ±10 Hz for PSK31 and ±30 Hz for PSK125 were tested, ±14 Hz and ±45 Hz failed.
- Phase distortions of the audio path (e.g. strong echoes) cause errors.

## Modes

| Mode | Symbols/s | Bandwidth | Text |
|---|---|---|---|
| `psk31` | 31.25 | about 60 Hz | about 4 characters/s |
| `psk63` | 62.5 | about 125 Hz | about 8 characters/s |
| `psk125` | 125 | about 250 Hz | about 17 characters/s |
| `psk250` | 250 | about 500 Hz | about 34 characters/s |
| `psk500` | 500 | about 1000 Hz | about 68 characters/s |

The carrier is at 1000 Hz by default.

## Usage

```cpp
I2SStream i2s;
PSKStream modem(i2s, RXTX_MODE);

void setup() {
  PSKConfig cfg = modem.defaultConfig();  // psk31
  cfg.setMode("psk125");
  cfg.sample_rate = 8000;
  ...
  modem.begin(cfg);
}
```

```sh
echo "CQ CQ de N0CALL" | tinymodem --tx -f cq.wav psk31
tinymodem --rx -f cq.wav psk31
```

`PSKConfig`, `PSKEncoder`, `PSKDecoder`, `PSKStream` and `PSKStreamRTOS` are the PSK classes. On the command line, `-R`, `-v`, `-M` (carrier frequency), `-q`, `--rx-one`, `--packets` and `--Xrxnoise` are supported. `--tx-carrier` sends phase reversals, the idle signal of PSK31.

| `PSKConfig` field | Default | |
|---|---|---|
| `symbol_rate` | 31.25 | Symbols (bits) per second |
| `carrier_freq` | 1000 | Carrier frequency (Hz) |
| `volume` | 1.0 | Transmit amplitude |
| `preamble_symbols` | 32 | Phase reversals before the data |
| `postamble_symbols` | 32 | Steady carrier after the data |
| `afc` | true | Receiver: automatic frequency control |
| `min_amplitude` | 0.005 | Receiver: minimum signal level |
| `rx_one` | false | Receiver: stop after the first transmission |
| `channel` | 0 | Receiver: input channel |
| `sample_rate`, `channels`, `bits_per_sample` | 48000, 1, 16 | Audio format |

## How it works

A transmission starts with phase reversals (the PSK31 idle signal), followed by the Varicode characters. It ends with a steady carrier and a fade out.

The receiver mixes the signal to the baseband and integrates it over one symbol. The decision time is the center of the energy distribution over the symbol period (like fldigi), and exactly one decision is made per symbol. The bit is determined from the phase difference to the previous symbol. Before a transmission, the receiver expects phase reversals, which allows a frequency capture range of about ±0.37 × the symbol rate. During a transmission, the frequency is tracked with the decisions. The receiver starts after a run of 8 phase reversals and stops when the signal is lost or after 20 symbols without phase change.

## Measured performance

Loopback with the unit tests at 8000 Hz:

| | Without errors up to |
|---|---|
| Noise (PSK31, PSK125) | 1.0 (about 1.8 dB signal-to-noise ratio) |
| Frequency offset | ±10 Hz (PSK31), ±30 Hz (PSK125) |
| Clock difference of sender and receiver | ±1% |

On a PC at 8000 Hz, the decoder ran 3000 to 7000 times faster than real time.
