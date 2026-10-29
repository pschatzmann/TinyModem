# TinyModem

[![Arduino Library](https://img.shields.io/badge/Arduino-Library-blue.svg)](https://www.arduino.cc/reference/en/libraries/)
[![CMake](https://img.shields.io/badge/CMake-Supported-blue.svg)](https://github.com/pschatzmann/TinyModem/blob/main/CMakeLists.txt)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-green.svg)](https://www.gnu.org/licenses/gpl-3.0)

A header-only C++ library of software audio modems: it converts data into an audio signal and back. The audio is a stream of PCM samples, so it can be played by any audio output, such as an I2S DAC or audio codec, a built-in DAC, PWM or a sound card. It can be recorded by any audio input, such as an I2S ADC or microphone, a built-in ADC or a sound card. Between the two, the signal can travel over an audio cable, from a speaker to a microphone, over a telephone line or by radio. It can also stay digital, for example in a WAV file.

TinyModem runs on microcontrollers (ESP32, RP2040) and on the desktop, where the `tinymodem` command line tool is included. All audio and DSP functionality is based on the [arduino-audio-tools](https://github.com/pschatzmann/arduino-audio-tools).

## Modem types

| Modem type | Modulation | Modes | Data rate | Best for | Compatible with |
|---|---|---|---|---|---|
| [FSK](https://github.com/pschatzmann/TinyModem/wiki/FSK) | 2 tones | `1200`, `300`, `rtty`, `tdd`, `same`, `V.21`, any baud rate, `callerid`, `uic` | up to 1200 bytes/s | Exchanging data with [minimodem](https://github.com/kamalmostafa/minimodem) | minimodem |
| [MFSK](https://github.com/pschatzmann/TinyModem/wiki/MFSK) | 4 to 64 tones | `mfsk4` … `mfsk64` | 23 – 125 bytes/s | Speakers and microphones, noise, different clocks | – |
| [PSK](https://github.com/pschatzmann/TinyModem/wiki/PSK) | phase of 1 carrier: BPSK, QPSK with FEC, 8PSK | `psk31` … `psk500`, `qpsk31` … `qpsk500`, `8psk125` … `8psk1000` | 4 – 320 characters/s of text | Narrow band, very weak signals, amateur radio | fldigi, untested |
| [OFDM](https://github.com/pschatzmann/TinyModem/wiki/OFDM) | phase of 16 to 64 carriers | `ofdm16`, `ofdm32`, `ofdm64` | 400 bytes/s | Fast transfers over a cable or a good audio path | – |
| [AX.25](https://github.com/pschatzmann/TinyModem/wiki/AX25) | 2 tones (AFSK), HDLC frames, FX.25 and IL2P with Reed-Solomon | `aprs`, `ax25`, `fx25`, `fx25-32`, `fx25-64`, `il2p` | 1200 bit/s | APRS and packet radio | direwolf, multimon-ng |
| [ggwave](https://github.com/pschatzmann/TinyModem/wiki/ggwave) | 2 – 6 of 96 tones at once, messages of up to 140 bytes with Reed-Solomon | `ggwave-normal`, `ggwave-fast`, `ggwave-fastest`, `ggwave-u-*` (ultrasound), `ggwave-dt-*` (dual tone) | 5 – 47 bytes/s | Exchanging short messages with phones and browsers | [ggwave](https://github.com/ggerganov/ggwave), Waver |
| [Olivia](https://github.com/pschatzmann/TinyModem/wiki/Olivia) | 2 to 256 tones with Walsh-Hadamard FEC | `olivia-8-250`, `olivia-32-1000` (`olivia`), any `olivia-T-B`, `contestia-T-B` | 1 – 5 characters/s of text | Keyboard chat with extremely weak signals | fldigi, untested |
| [CSS](https://github.com/pschatzmann/TinyModem/wiki/CSS) | chirps (LoRa-like), frames with Reed-Solomon | `css5` … `css10`, e.g. `css9-500` | 1 – 20 bytes/s | Very weak signals, speakers and microphones, different clocks | – |
| [DTMF](https://github.com/pschatzmann/TinyModem/wiki/DTMF) | 2 of 8 tones (touch tones) | `dtmf` | 7 keys/s | Telephones, radio remote control | telephones, DTMF decoders |

All modem types transmit an audio signal made of tones, not a digital line signal like a UART. Most modes use tones between about 1000 and 3000 Hz, which pass a telephone line, a radio and most speakers and microphones. Only FSK with high baud rates, `8psk1000` and ggwave (1875 – 6375 Hz, ultrasound 15 – 19.5 kHz) need higher frequencies. Even AX.25, which sends digital frames, transmits the bits with two audio tones.

[Packets](https://github.com/pschatzmann/TinyModem/wiki/Packets) add a CRC and Reed-Solomon error correction to any modem type, and also work over a plain `Serial`. The [modem overview](https://github.com/pschatzmann/TinyModem/wiki/Modem-Types) describes the advantages and disadvantages of each modem type and helps you choose.

## Installation in Arduino

TinyModem needs the [arduino-audio-tools](https://github.com/pschatzmann/arduino-audio-tools). You can download both libraries as zip and call Sketch -> Include Library -> Add .ZIP Library. Or you can git clone both projects into the Arduino libraries folder, e.g. with

```sh
cd ~/Documents/Arduino/libraries
git clone https://github.com/pschatzmann/arduino-audio-tools.git
git clone https://github.com/pschatzmann/TinyModem.git
```

I recommend to use git because you can easily update to the latest version just by executing the `git pull` command in the project folders.

To build the `tinymodem` command line tool and the tests on the desktop, you need CMake (≥ 3.18) and a C++17 compiler. The arduino-audio-tools are taken from the sibling folder `../arduino-audio-tools` if it exists, otherwise they are downloaded. PortAudio (`portaudio19-dev` on Debian/Ubuntu) is optional: it is needed to use the sound card. See [command line](https://github.com/pschatzmann/TinyModem/wiki/Command-Line).

## Arduino

```cpp
#include "AudioTools.h"
#include "TinyModem.h"

I2SStream i2s;
MFSKStream modem(i2s, RXTX_MODE);  // or ModemStream, PSKStream, OFDMStream, AX25Stream, GGWaveStream,
                                   // OliviaStream, CSSStream, DTMFStream

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

See the [examples](examples) and the [Arduino documentation](https://github.com/pschatzmann/TinyModem/wiki/Arduino). The library runs on ESP32-S3, ESP32-P4 and RP2040 (see the [performance](https://github.com/pschatzmann/TinyModem/wiki/Performance); the RP2040 is too slow for Olivia).

## Command line

```sh
cmake -S . -B build && cmake --build build
echo "Hello" | ./build/tinymodem --tx -f hello.wav mfsk16
./build/tinymodem --rx -f hello.wav mfsk16
```

See [command line](https://github.com/pschatzmann/TinyModem/wiki/Command-Line).

## Documentation

The documentation is in the [Wiki](https://github.com/pschatzmann/TinyModem/wiki):

- [Modem overview: which modem type should I use?](https://github.com/pschatzmann/TinyModem/wiki/Modem-Types)
- Modem types: 
  - [FSK](https://github.com/pschatzmann/TinyModem/wiki/FSK), 
  - [MFSK](https://github.com/pschatzmann/TinyModem/wiki/MFSK), 
  - [PSK](https://github.com/pschatzmann/TinyModem/wiki/PSK), 
  - [OFDM](https://github.com/pschatzmann/TinyModem/wiki/OFDM), 
  - [AX.25/APRS](https://github.com/pschatzmann/TinyModem/wiki/AX25),
  - [ggwave](https://github.com/pschatzmann/TinyModem/wiki/ggwave),
  - [Olivia/Contestia](https://github.com/pschatzmann/TinyModem/wiki/Olivia),
  - [Chirp spread spectrum](https://github.com/pschatzmann/TinyModem/wiki/CSS),
  - [DTMF](https://github.com/pschatzmann/TinyModem/wiki/DTMF)
- [Packets with error correction](https://github.com/pschatzmann/TinyModem/wiki/Packets)
- [Arduino API](https://github.com/pschatzmann/TinyModem/wiki/Arduino)
- [Performance on PC and microcontrollers](https://github.com/pschatzmann/TinyModem/wiki/Performance)
- [Class documentation](https://pschatzmann.github.io/TinyModem/annotated.html) (Doxygen)
- [Memory usage](https://github.com/pschatzmann/TinyModem/wiki/Memory)
- [Command line tool](https://github.com/pschatzmann/TinyModem/wiki/Command-Line)
- [Tests](https://github.com/pschatzmann/TinyModem/wiki/Testing)
- [Design](https://github.com/pschatzmann/TinyModem/wiki/Design)

## License

GPLv3 or later. The FSK modem is a rewrite of [minimodem](https://github.com/kamalmostafa/minimodem) by Kamal Mostafa. The ggwave modem implements the protocol of [ggwave](https://github.com/ggerganov/ggwave) by Georgi Gerganov. Olivia/Contestia, QPSK and 8PSK follow [fldigi](https://www.w1hkj.org/) (Olivia by Pawel Jalocha), FX.25 and IL2P follow [direwolf](https://github.com/wb2osz/direwolf) by John Langner, WB2OSZ. See [COPYING](COPYING).
