# Arduino API

Include `AudioTools.h` and then `TinyModem.h`, or `TinyModemRTOS.h` if you want to use the stream classes which receive in a separate task. All classes are in the `tinymodem` namespace, which is imported automatically. Define `TINYMODEM_NO_NAMESPACE` before the include to prevent that.

## Classes

Each modem type has the same set of classes:

| Modem type | Configuration | Encoder | Decoder | Stream | Stream with receive task |
|---|---|---|---|---|---|
| [FSK](fsk.md) | `ModemConfig` | `FSKEncoder` | `FSKDecoder` | `ModemStream` | `ModemStreamRTOS` |
| [MFSK](mfsk.md) | `MFSKConfig` | `MFSKEncoder` | `MFSKDecoder` | `MFSKStream` | `MFSKStreamRTOS` |
| [PSK](psk.md) | `PSKConfig` | `PSKEncoder` | `PSKDecoder` | `PSKStream` | `PSKStreamRTOS` |
| [OFDM](ofdm.md) | `OFDMConfig` | `OFDMEncoder` | `OFDMDecoder` | `OFDMStream` | `OFDMStreamRTOS` |
| [AX.25](ax25.md) | `AX25Config` | `AX25Encoder` | `AX25Decoder` | `AX25Stream` | `AX25StreamRTOS` |

- **Configuration**: extends the AudioTools `AudioInfo` (`sample_rate`, `channels`, `bits_per_sample`). `setMode()` selects a predefined mode (e.g. `"1200"`, `"mfsk16"`, `"psk31"`, `"ofdm32"`, `"aprs"`): call it first and then change individual fields. The default sample rate is 48000 Hz.
- **Encoder**: converts the written data into 16-bit PCM audio. An AudioTools `AudioEncoder` (base class `ModemEncoder`).
- **Decoder**: converts PCM audio (8, 16 or 32 bit, 1 to 8 channels) into data. An AudioTools `AudioDecoder` (base class `ModemDecoder`).
- **Stream**: an Arduino `Stream` with both: `print()` transmits, `read()` receives (template `BasicModemStream`).
- **Stream with receive task**: decodes in a FreeRTOS task (template `BasicModemStreamRTOS`).

In addition:

| Class | Purpose |
|---|---|
| `PacketStream`, `PacketEncoder`, `PacketDecoder` | [Packets](packets.md) with CRC and Reed-Solomon error correction over any `Stream` |
| `AX25Frame` | Conversion between AX.25 frames, the text format and KISS |
| `FSKToneGenerator` | Default tone generator of the FSK, MFSK and AX.25 encoders, see [tone generator](#tone-generator) |
| `Varicode` | The PSK31 Varicode |

## Audio hardware

The modems only produce and consume PCM samples, so you can use any AudioTools stream for the audio input and output:

- `I2SStream`: an I2S DAC, ADC, microphone or audio codec (e.g. `AudioBoardStream` for an AudioKit)
- `AnalogAudioStream`: the built-in DAC and ADC (e.g. of the ESP32)
- `PWMAudioOutput`: PWM output with a low-pass filter
- files, memory or a network connection, if the signal should stay digital

A speaker and a microphone (an acoustic path) are the hardest case: use [MFSK](mfsk.md). With a cable from a DAC to an ADC, all modem types work, as long as the sample rates of the two devices match closely enough (OFDM needs ±0.2%, see the [comparison](modems.md#comparison)). Make sure that the input level doesn't clip and that the signal isn't too weak (see the `ampl=` value of the status messages).

## Streams

The stream classes hide the encoder and decoder behind the Arduino `Stream` API:

```cpp
ModemStream modem(audio_out);            // transmit only
ModemStream modem(audio_out, audio_in);  // transmit and receive
ModemStream modem(i2s, RX_MODE);         // one stream: TX_MODE, RX_MODE or RXTX_MODE
```

- `begin(cfg, rx_buffer_size)` starts the modem. The input is only read while less than `rx_buffer_size` decoded bytes (default 256) are waiting to be read. Decoded data is never dropped.
- `write()` / `print()` modulate the data. `flush()` ends the transmission (OFDM: sends the collected frame).
- `available()`, `read()` and `peek()` read the input stream and decode it in the calling task. Call one of them regularly in `loop()`. The decoding processes a block of audio at a time, which blocks `loop()` for a moment: use the RTOS version if that's a problem.
- `setStatusOutput(Serial)` prints the `### CARRIER` / `### NOCARRIER` messages.
- `getEncoder()` and `getDecoder()` give access to the codecs.

### Receiving in a separate task

The RTOS versions (`ModemStreamRTOS`, `MFSKStreamRTOS`, …) have the same API, but a separate task reads the audio input and decodes it. `loop()` is never blocked by the decoding, and no audio is lost while `loop()` is busy. `available()`, `read()` and `peek()` only access the decoded data and can be called from any task.

```cpp
#include "AudioTools.h"
#include "TinyModemRTOS.h"

I2SStream i2s;
MFSKStreamRTOS modem(i2s, RXTX_MODE);

void setup() {
  ...
  modem.setTaskConfig(8 * 1024, 1, 0);  // optional: stack, priority, core
  modem.begin(cfg);
}
```

- Supported on ESP32, on RP2040 and on the desktop (with `std::thread`). On the RP2040 select *Tools > Operating System > FreeRTOS SMP* and add `#include <FreeRTOS.h>` before `#include "AudioTools.h"`.
- Transmitting (`write()`, `print()`, `flush()`) still happens in the calling task. The status output is written by the receive task.
- `setTaskConfig(stack_size, priority, core)` must be called before `begin()`. The default stack is 8 KB (on ESP32 the size is in bytes, on other FreeRTOS platforms in words). On ESP32, core 0 keeps the decoding away from `loop()`, which runs on core 1.
- `end()` waits until the task has finished the current decoding step, then stops it.

## Using the AudioTools pipeline

The encoders and decoders are regular AudioTools codecs, so you can wrap them in an `EncodedAudioOutput` and combine them with any AudioTools stream (I2S, analog, A2DP, files, …).

### Transmit

```cpp
I2SStream i2s;
MFSKEncoder encoder;
EncodedAudioOutput modem(&i2s, &encoder);  // Print: data in, PCM to i2s
MFSKConfig cfg;

void setup() {
  auto i2s_cfg = i2s.defaultConfig(TX_MODE);
  i2s_cfg.copyFrom(cfg);
  i2s.begin(i2s_cfg);
  encoder.begin(cfg);  // modem settings
  modem.begin(cfg);    // activates the output
}

void loop() {
  modem.println("Hello");
  encoder.endTransmission(cfg.sample_rate);  // end + 1 second of silence
}
```

`endTransmission(silence_samples)` ends a transmission (FSK: trailer bits, MFSK: end marker, PSK: postamble, OFDM: sends the frame, AX.25: tail flags). `writeIdle(n)` sends an idle signal where the modem has one (FSK: mark tone, PSK: phase reversals, AX.25: flags), and `writeSilence(n)` / `writeTone(freq, n)` write raw audio.

### Receive

```cpp
I2SStream i2s;
MFSKDecoder decoder;
EncodedAudioOutput modem(&Serial, &decoder);  // PCM in, data to Serial
StreamCopy copier(modem, i2s);
MFSKConfig cfg;

void setup() {
  ...
  decoder.setStatusOutput(Serial);  // optional: ### CARRIER / ### NOCARRIER
  decoder.begin(cfg);
  modem.begin(cfg);
}

void loop() { copier.copy(); }
```

The decoders accept 8, 16 or 32-bit signed samples with up to 8 channels. Only `cfg.channel` is decoded. If the input reports a new `AudioInfo`, for example from a WAV header, the decoder restarts with the new format. Other methods: `isCarrier()`, `isFinished()` (after the first transmission with `rx_one`: the decoder ignores further input, also after `flush()`, until `begin()` is called again), `setQuiet()`, and `flush()` / `end()` to process the remaining buffered samples.

## Tone generator

The FSK, MFSK and AX.25 encoders produce the tones with an AudioTools `SoundGenerator<int16_t>`. The default `FSKToneGenerator` reproduces minimodem's signal. You can replace it with `setGenerator()` before `begin()`:

```cpp
FastIntSineGenerator<int16_t> sine;  // fixed point: no FPU needed
FSKEncoder encoder;
encoder.setGenerator(sine);
```

With a stream, use `modem.getEncoder().setGenerator(sine)`. The generator must switch the frequency immediately and keep the phase continuous in `setFrequency()`. That's the case for `FastIntSineGenerator` (recommended for MCUs without an FPU, like the RP2040). `FloatSineGenerator` and `FastSineGenerator` reset the phase, and `SineFromTable` only changes the frequency at the end of a cycle, so they don't work. The generator should produce full scale: the encoder applies the volume and writes silence itself. PSK and OFDM generate their signal themselves.

## Memory and CPU

| Modem type | Decoder memory at 8000 Hz |
|---|---|
| FSK | 2.6 KB (1200 baud), 12.4 KB (RTTY) |
| MFSK | 8 symbols of samples: 2 KB (`mfsk16`), 8.5 KB (`mfsk64`) |
| PSK | 3 symbols of samples: 3 KB (`psk31`), 0.2 KB (`psk500`) |
| OFDM | 6.6 KB (`ofdm32`) |
| AX.25 | less than 1 KB |

The memory grows with the sample rate: use the lowest sample rate your hardware and the mode support. The encoders need less than 1 KB (OFDM: 1 KB for the DFT tables and the frame of 512 bytes). The RTOS streams need a task stack (8 KB by default) in addition, and packets about 650 bytes of stack.

Decoding speed on a PC at 8000 Hz (× real time): FSK 1200 1000, `mfsk16` 1100, `mfsk64` 300, `psk125` 5500, `ofdm32` 1600, `aprs` 5600. A microcontroller is much slower: run the [benchmark](../examples/benchmark) example, which measures the modems on your board. Use a recent version of the arduino-audio-tools: older versions compute double precision `sin()`/`cos()` in the Goertzel filter for each block, which is slow on MCUs without a double precision FPU (FSK, MFSK).

The library needs `float` math and `vsnprintf` with `%f` support (for the status messages). It compiles for ESP32 and RP2040, but it hasn't been tested on real hardware yet. AVR boards don't have enough RAM.

## Troubleshooting

- **`begin()` returns false**: the reason is logged as an error. Activate the logger with `AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning)`. Typical causes: the decoders only accept 8, 16 or 32-bit samples (configure 24-bit I2S microphones as 32 bits), the encoders only produce 16 bits, a tone is above half the sample rate, and Caller-ID and UIC can't be transmitted.
- **Nothing is received**: activate the status output with `setStatusOutput(Serial)`. If you never see `### CARRIER`, check the input channel: many I2S microphones deliver the signal on only one channel, so try `cfg.channel = 1`. Check that the sample rate in `cfg` matches the real sample rate of your audio input, and that both sides use the same mode.
- **Wrong or missing characters**: the `ampl=` value of the status messages shows the input level (1.0 = full scale). Values below about 0.01 are too weak, values close to 1.0 may be clipped: adjust the volume on the transmitting side. Use a more robust modem (MFSK) or [packets](packets.md).
- **OFDM doesn't work over the air**: OFDM needs a good signal and closely matching clocks (±0.2%). Try MFSK.
- **Data is lost while receiving**: the decoder must get the audio in real time. Call `available()` or `read()` often enough, or use the RTOS version of the stream. Check the speed with the benchmark example.
- **RP2040 compile error "select Tools > Operating System > FreeRTOS SMP"**: the RTOS streams need FreeRTOS, see above. The other classes don't.

## Examples

- [loopback](../examples/loopback): FSK encoder → decoder in memory. Runs on any board.
- [mfsk-loopback](../examples/mfsk-loopback): the same with MFSK.
- [send-i2s](../examples/send-i2s): FSK transmit via an I2S DAC.
- [receive-i2s](../examples/receive-i2s): FSK receive from an I2S microphone.
- [modem-stream](../examples/modem-stream): serial over audio with `ModemStream`.
- [modem-stream-rtos](../examples/modem-stream-rtos): the same, but decoding in a separate task.
- [packet-modem](../examples/packet-modem): reliable serial over audio with packets over MFSK.
- [aprs-beacon](../examples/aprs-beacon): sends APRS position reports.
- [benchmark](../examples/benchmark): measures the speed of the modems on your board.
