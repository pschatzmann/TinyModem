# TinyModem

[![Arduino Library](https://img.shields.io/badge/Arduino-Library-blue.svg)](https://www.arduino.cc/reference/en/libraries/)
[![CMake](https://img.shields.io/badge/CMake-Supported-blue.svg)](https://github.com/pschatzmann/TinyModem/blob/main/CMakeLists.txt)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-green.svg)](https://www.gnu.org/licenses/gpl-3.0)

A header-only C++ library of software audio modems: it converts data into an audio signal and back. The audio is a stream of PCM samples, so it can be played by any audio output, such as an I2S DAC or audio codec, a built-in DAC, PWM or a sound card. It can be recorded by any audio input, such as an I2S ADC or microphone, a built-in ADC or a sound card. Between the two, the signal can travel over an audio cable, from a speaker to a microphone, over a telephone line or by radio. It can also stay digital, for example in a WAV file.

TinyModem runs on microcontrollers (ESP32, RP2040) and on the desktop, where the `tinymodem` command line tool is included. All audio and DSP functionality is based on the [arduino-audio-tools](https://github.com/pschatzmann/arduino-audio-tools).

## Modem types

| Modem type | Modulation | Modes | Data rate | Best for | Compatible with |
|---|---|---|---|---|---|
| [FSK](docs/fsk.md) | 2 tones | `1200`, `300`, `rtty`, `tdd`, `same`, `V.21`, any baud rate, `callerid`, `uic` | up to 1200 bytes/s | Exchanging data with [minimodem](https://github.com/kamalmostafa/minimodem) | minimodem |
| [MFSK](docs/mfsk.md) | 4 to 64 tones | `mfsk4` … `mfsk64` | 23 – 125 bytes/s | Speakers and microphones, noise, different clocks | – |
| [PSK](docs/psk.md) | phase of 1 carrier | `psk31` … `psk500` | 4 – 68 bytes/s of text | Narrow band, very weak signals, amateur radio | PSK31 (fldigi), untested |
| [OFDM](docs/ofdm.md) | phase of 16 to 64 carriers | `ofdm16`, `ofdm32`, `ofdm64` | 400 bytes/s | Fast transfers over a cable or a good audio path | – |
| [AX.25](docs/ax25.md) | 2 tones (AFSK), HDLC frames | `aprs`, `ax25` | 1200 bit/s | APRS and packet radio | direwolf, multimon-ng |

All modem types transmit an audio signal made of tones, not a digital line signal like a UART. Most modes use tones between about 1000 and 3000 Hz, which pass a telephone line, a radio and most speakers and microphones. Only FSK with high baud rates needs higher frequencies. Even AX.25, which sends digital frames, transmits the bits with two audio tones.

[Packets](docs/packets.md) add a CRC and Reed-Solomon error correction to any modem type, and also work over a plain `Serial`. The [modem overview](docs/modems.md) describes the advantages and disadvantages of each modem type and helps you choose.

## Installation in Arduino

TinyModem needs the [arduino-audio-tools](https://github.com/pschatzmann/arduino-audio-tools). You can download both libraries as zip and call Sketch -> Include Library -> Add .ZIP Library. Or you can git clone both projects into the Arduino libraries folder, e.g. with

```sh
cd ~/Documents/Arduino/libraries
git clone https://github.com/pschatzmann/arduino-audio-tools.git
git clone https://github.com/pschatzmann/TinyModem.git
```

I recommend to use git because you can easily update to the latest version just by executing the `git pull` command in the project folders.

To build the `tinymodem` command line tool and the tests on the desktop, you need CMake (≥ 3.18) and a C++17 compiler. The arduino-audio-tools are taken from the sibling folder `../arduino-audio-tools` if it exists, otherwise they are downloaded. PortAudio (`portaudio19-dev` on Debian/Ubuntu) is optional: it is needed to use the sound card. See [command line](docs/command-line.md).

## Arduino

```cpp
#include "AudioTools.h"
#include "TinyModem.h"

I2SStream i2s;
MFSKStream modem(i2s, RXTX_MODE);  // or ModemStream, PSKStream, OFDMStream, AX25Stream

void setup() {
  Serial.begin(115200);
  MFSKConfig cfg = modem.defaultConfig();  // mfsk16
  cfg.sample_rate = 22050;
  auto i2s_cfg = i2s.defaultConfig(RXTX_MODE);
  i2s_cfg.copyFrom(cfg);
  i2s.begin(i2s_cfg);
  modem.begin(cfg);
}

void loop() {
  modem.println("Hello");  // send
  modem.flush();           // end of the transmission
  while (modem.available()) Serial.write(modem.read());  // receive
}
```

See the [examples](examples) and the [Arduino documentation](docs/arduino.md). The library compiles for ESP32 and RP2040, but it hasn't been tested on real hardware yet.

## Command line

```sh
cmake -S . -B build && cmake --build build
echo "Hello" | ./build/tinymodem --tx -f hello.wav mfsk16
./build/tinymodem --rx -f hello.wav mfsk16
```

See [command line](docs/command-line.md).

## Documentation

- [Modem overview: which modem type should I use?](docs/modems.md)
- Modem types: [FSK](docs/fsk.md), [MFSK](docs/mfsk.md), [PSK](docs/psk.md), [OFDM](docs/ofdm.md), [AX.25/APRS](docs/ax25.md)
- [Packets with error correction](docs/packets.md)
- [Arduino API](docs/arduino.md)
- [Command line tool](docs/command-line.md)
- [Tests](docs/testing.md)
- [Design](docs/design.md)

## License

GPLv3 or later. The FSK modem is a rewrite of [minimodem](https://github.com/kamalmostafa/minimodem) by Kamal Mostafa. See [COPYING](COPYING) and [AUTHORS](AUTHORS).
