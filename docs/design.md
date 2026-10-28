# Design

TinyModem is header only: `src/TinyModem.h` includes everything, the modems are in `src/modem/`. All audio and DSP functionality is based on the arduino-audio-tools (codecs, streams, sine generators, Goertzel filters, Reed-Solomon, tasks and mutexes).

## Class structure

Each modem consists of a configuration, an encoder and a decoder:

- The encoders are derived from `ModemEncoder`, the decoders from `ModemDecoder` (both in `ModemBase.h`), which are AudioTools `AudioEncoder` and `AudioDecoder`. The base classes define the common API (`endTransmission()`, `writeIdle()`, `isCarrier()`, `isFinished()`, the status output, …).
- Shared helpers: `ToneOutput` writes the tones of a `SoundGenerator` (FSK, MFSK, AX.25), `SampleOutput` writes calculated samples (PSK, OFDM), `PcmInput` assembles the PCM frames and extracts the selected channel, and `ByteQueue` is the receive buffer of the streams.
- The streams are templates: `BasicModemStream<Encoder, Decoder, Config>` provides the Arduino `Stream` API, and `BasicModemStreamRTOS<Base>` adds the receive task (AudioTools `Task` and `Mutex`). `ModemStream`, `MFSKStream`, `PSKStream`, `OFDMStream`, `AX25Stream` and their RTOS versions are the concrete classes.
- `PacketStream` works on any `Stream`, independent of the modems.
- The command line tool works with `ModemEncoder` and `ModemDecoder`, so the same code supports all modems.

All decoders work with push processing: `write()` accepts PCM data in any chunk size, and the decoding is done whenever enough samples are buffered. `flush()` / `end()` pad the input with silence so that the last transmission is completed. The streams call `write()` from `available()` / `read()`, or from a separate task in the RTOS versions. The receive queue grows when needed, so no decoded data is lost: the input is only read while less than `rx_buffer_size` bytes are waiting.

The algorithms of the extensions are described in their documents: [MFSK](mfsk.md#how-it-works), [PSK](psk.md#how-it-works), [OFDM](ofdm.md#how-it-works), [AX.25](ax25.md#how-it-works) and [packets](packets.md#format).

## FSK: rewrite of minimodem

The FSK modem is a C++ rewrite of minimodem 0.24. The modem algorithms are unchanged, and the platform layer is replaced by the arduino-audio-tools.

| minimodem | TinyModem |
|---|---|
| `minimodem.c` option defaults and baud modes | `ModemConfig::setMode()`, `ModemConfig::resolve()` |
| `minimodem.c` transmit (`fsk_transmit_frame`, leader, sync, trailer) | `FSKEncoder` |
| `minimodem.c` receive main loop | `FSKDecoder::step()` |
| `fsk.c` (frame search, confidence, carrier detection) | `FSKDemodulator` |
| `simple-tone-generator.c` | `FSKToneGenerator` (AudioTools `FloatSineGenerator`), replaceable by any phase continuous `SoundGenerator` |
| `databits_*.c`, `baudot.c`, `uic_codes.c` | `DataCodecs.h` |
| `simpleaudio*` (ALSA, PulseAudio, sndio, sndfile) | AudioTools streams. The CLI uses `WAVEncoder`, `WAVDecoder` and `PortAudioStream`. |
| FFTW | AudioTools `GoertzelDetector` |

### Goertzel instead of FFT

minimodem computes a full FFT for every bit, but only reads two bins: mark and space. This rewrite computes exactly those two bins with Goertzel filters. The frequency of band `b` is `b * sample_rate / fftsize`, the same FFT bin center the original uses, and the magnitude is scaled by `2 / nsamples`. The results match the original (see the confidence values in the interop tests), and FFTW isn't needed.

Carrier auto-detection scans every band with a Goertzel filter. That is O(bands × samples) instead of one FFT, so it is slower than the original.

The `GoertzelDetector` of older arduino-audio-tools versions computed double precision `sin()` and `cos()` at the end of each block, which is once per analysed bit here. That is slow on MCUs without a double precision FPU (ESP32, RP2040), so recent versions compute them once in `begin()`.

### From a blocking loop to push processing

The original receive loop blocks in `simpleaudio_read()` until half the sample buffer is filled. `FSKDecoder::write()` runs one iteration of the original loop (`step()`) whenever enough samples for a complete frame search are buffered.

The transmitter's interactive behaviour (end of transmission after one idle bit time, idle carrier) used `SIGALRM` and `select()`. In the library it is an explicit `endTransmission()` / `writeIdle()`. The CLI emulates the original timing with `select()`.

All state that was global in the C code is now kept in class instances, so you can use several encoders and decoders at the same time. This includes the Baudot shift state, the Caller-ID buffer and the tone generator phase.

### Known differences

- The decoder doesn't reach exactly `confidence=inf` for perfect signals (float precision of the Goertzel filter). Decoding results are not affected.
- The generated audio differs from the original by at most 31 (of 32767) per sample: the original uses a 4096-entry sine table, this rewrite uses `sinf()`.
- The AudioTools `WAVDecoder` drops the last incomplete batch of converted 24-bit or float samples in `end()`. The CLI works around this by appending silence.
- RTTY/TDD transmit: each transmission starts with a LTRS or FIGS code, because the receiver resets its shift state with each new carrier. The original only sends a shift code when the character set changes, so a second transmission that continues with figures was decoded as letters. The first transmission is identical to the original.
- Bugs fixed: a possible overflow of the 64-entry bit arrays, the unchecked SDMF Caller-ID message length, and a DC offset instead of noise in `--Xrxnoise`.
