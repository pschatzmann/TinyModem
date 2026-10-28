# Modem overview

TinyModem provides five modem types: FSK, MFSK, PSK, OFDM and AX.25. Each type has several modes (e.g. `mfsk4` to `mfsk64`), which you select with `setMode()` or on the command line. All modem types have the same API (an AudioTools encoder and decoder, a `Stream` and a version which receives in a separate task) and the `tinymodem` command line tool supports all of them. Any of them can be combined with [packets](packets.md) for error correction.

All modem types are audio modems: the encoder converts the data into PCM samples of an audio signal, and the decoder converts the samples back into data. The samples can be played and recorded with any audio hardware (DAC and ADC, I2S codec, sound card) or stay digital (WAV file, memory). None of them is a digital line code like a UART. They differ in how the bits are mapped to the audio signal: to the frequency of a tone (FSK, MFSK, AX.25) or to the phase of one or more carriers (PSK, OFDM).

## Comparison

All values were measured with the unit tests and the command line tool in a digital loopback at 8000 Hz (see [testing](testing.md)). "Noise" is the level of uniform noise added in the receiver (`--Xrxnoise`) up to which the test data was decoded without errors: the higher, the more robust. Real audio paths (DAC, cable, speaker and microphone, ADC) are worse than these simulations, so leave a margin.

| | [FSK](fsk.md) 1200 | [MFSK](mfsk.md) 16 | [PSK](psk.md) 125 | [OFDM](ofdm.md) 32 | [AX.25](ax25.md) |
|---|---|---|---|---|---|
| Data rate | 114 bytes/s | 62 bytes/s | 17 bytes/s (text) | 380 bytes/s | 1200 bit/s, framed |
| Bandwidth | 1200 – 2200 Hz | 1000 – 2875 Hz | about 250 Hz | 1000 – 2940 Hz | 1200 – 2200 Hz |
| Noise | 0.1 | 1.0 | 1.0 | 0.1 | 0.3 |
| Different clocks of sender and receiver | about ±2.5% | ±3% | ±1% | ±0.2% | ±1% at 48 kHz, less at 8 kHz |
| Decoder CPU on a PC (× real time) | 1000 | 1100 | 5500 | 1600 | 5600 |
| Interoperable with | minimodem | – | PSK31 programs (untested) | – | direwolf, multimon-ng |
| Error detection | – | – | – | – | CRC (frames with errors are dropped) |

The noise levels of OFDM refer to a quieter signal: OFDM sends at about a third of the level of the other modems, because the sum of its carriers has high peaks.

## Which modem type should I use?

- **Exchange data with minimodem or with standard modems** (Bell 103/202, RTTY, Caller-ID, NOAA SAME): [FSK](fsk.md).
- **Speaker and microphone, noisy environment, cheap hardware with inaccurate clocks**: [MFSK](mfsk.md), together with [packets](packets.md).
- **Highest data rate over a cable or a clean audio path**: [OFDM](ofdm.md) with [packets](packets.md), or FSK with a high baud rate if the audio path has a wide frequency range.
- **Very weak signals in a narrow band, amateur radio keyboard chat**: [PSK](psk.md).
- **APRS position reports and packet radio**: [AX.25](ax25.md).

## FSK (minimodem)

Frequency shift keying with two tones (mark and space) and asynchronous framing with start and stop bits, like the classic Bell 103/202 modems. The receiver is a port of minimodem 0.24, so the library can talk to minimodem in both directions.

**Advantages**

- Compatible with minimodem and with many standard modes: Bell 103 (300 baud), Bell 202 (1200 baud), V.21, RTTY, TTY/TDD, NOAA SAME, and the receive-only modes Caller-ID and UIC-751-3.
- Any baud rate: with a wide audio path (cable) up to 12000 baud (1200 bytes/s) at 48000 Hz.
- Simple and well understood.

**Disadvantages**

- Sensitive to noise: one wrong bit changes a byte, a missed start bit loses it.
- Binary data needs a baud rate which divides the sample rate (e.g. not 1200 baud at 8000 Hz).
- High baud rates need high frequencies, which speakers and microphones can't reproduce.
- No error detection.

## MFSK (multi-tone FSK)

Each symbol is one of 4 to 64 tones and carries 2 to 6 bits. The symbols are much longer than FSK bits at the same data rate. The receiver finds the preamble, measures the clock of the sender and tracks the symbol timing.

**Advantages**

- Very robust against noise: `mfsk64` decodes without errors when the noise is stronger than the signal.
- Tolerates different clocks of sender and receiver of ±3%: the receiver adapts to the sender.
- Long symbols are robust against echoes, so it is the best choice for speakers and microphones.
- All tones are between 1000 and 3000 Hz (telephone band).

**Disadvantages**

- Slower than FSK and OFDM (23 to 125 bytes/s).
- Not compatible with other programs.
- The decoder needs more CPU with more tones (`mfsk64`).

## PSK (PSK31 and faster)

Binary phase shift keying of a single carrier like PSK31: a 0 bit is a phase reversal with a smooth (cosine shaped) amplitude, a 1 bit keeps the phase. Characters are sent with the Varicode, in which frequent characters have short codes. The receiver tracks the frequency and the symbol timing.

**Advantages**

- Very narrow band (about 60 Hz for PSK31), so many signals fit into one audio channel.
- Very robust against noise and very efficient for text.
- The decoder needs little CPU.
- The same format as PSK31 to PSK500 in amateur radio programs like fldigi (not verified with these programs).

**Disadvantages**

- Slow: PSK31 transmits about 4 characters per second.
- Varicode is optimized for text: binary data is slower (bytes above 127 have codes of up to 12 bits).
- The receiver only finds the signal if its frequency is close to the carrier (±10 Hz for PSK31, ±30 Hz for PSK125).
- Phase distortions of the audio path (e.g. echoes) are a problem.

## OFDM

Orthogonal frequency division multiplexing: the data is sent in parallel on 16 to 64 carriers with differential QPSK (2 bits per carrier). Each symbol starts with a cyclic prefix, which absorbs echoes. A frame starts with a sync symbol (Schmidl & Cox) and a reference symbol and contains up to 512 bytes.

**Advantages**

- The fastest modem in the telephone band: 3200 bit/s (about 380 bytes/s) between 1000 and 3000 Hz.
- Echoes shorter than the cyclic prefix (2, 4 or 8 ms) don't matter.
- Each frame has a length header, so the receiver knows exactly where the data ends.

**Disadvantages**

- Needs a good signal: errors start at a lower noise level than with the other modems. Use it with [packets](packets.md).
- Very sensitive to different clocks of sender and receiver: about ±0.2% (±0.1% for `ofdm64`).
- The data is sent in frames: there is a delay until a frame is full or `flush()` is called.
- The signal has high peaks, so it is sent with a lower average level.

## AX.25 (APRS and packet radio)

The packet radio standard of amateur radio: Bell 202 AFSK with 1200 baud, NRZI coding and HDLC framing with bit stuffing and a frame check sequence. The data is a sequence of frames with source, destination and digipeater addresses. The text format (`SRC>DEST,PATH:info`) is the monitor format of direwolf and APRS programs, the binary format is KISS.

**Advantages**

- Compatible with direwolf, multimon-ng, APRS programs and hardware TNCs: tested with direwolf in both directions.
- Each frame has a CRC: corrupted frames are dropped.
- KISS support for packet radio applications.

**Disadvantages**

- Fixed at 1200 baud.
- At 8000 Hz a bit has only 6.7 samples: use 22050 Hz or more, like direwolf.
- Only frames with addresses, no plain byte stream.
- Transmitting on the radio needs an amateur radio license.
- On the frames with increasing noise of direwolf's test, the decoder finds a few frames less than direwolf (68 against 72 of 100).

## Packets

[Packets](packets.md) are not a modem but a layer on top of any of them (or a `Serial`): the data is sent in packets with a CRC and Reed-Solomon error correction. Packets which can't be corrected are dropped, so you only receive correct data. Use them with MFSK, PSK and OFDM if the data must be correct.
