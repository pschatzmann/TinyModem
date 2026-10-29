#pragma once

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "AudioTools.h"
#include "PacketStream.h"
#include "ReedSolomon.h"

namespace tinymodem {

/**
 * @brief Configuration of the chirp spread spectrum modem (CSSEncoder,
 * CSSDecoder), which works like LoRa in the audio band: each symbol is a
 * linear chirp over the bandwidth, which starts at one of N = 2^SF
 * frequencies and carries SF bits (SF = spreading factor). The signal can be
 * received far below the noise level and is robust against frequency
 * offsets, different clocks and echoes. It is not compatible with LoRa
 * radios.
 *
 * A frame starts with preamble_symbols up chirps and 2 down chirps, followed
 * by the header (length, Reed-Solomon protected) and the payload (CRC-16 and
 * ecc_bytes Reed-Solomon parity bytes).
 *
 * setMode() selects "css5" to "css10" (SF, 1000 Hz bandwidth) or e.g.
 * "css7-500" or "css9-2000" (SF and bandwidth). "css" is "css7".
 * @copyright GPLv3
 */
struct CSSConfig : public audio_tools::AudioInfo {
  CSSConfig() {
    sample_rate = 48000;
    channels = 1;
    bits_per_sample = 16;
  }

  /// bits per symbol (5 - 10): each step doubles the symbol duration and
  /// improves the sensitivity by about 2.5 dB
  int spreading_factor = 7;
  /// bandwidth of the chirps in Hz
  float bandwidth = 1000.0f;
  /// center frequency in Hz
  float center_freq = 1500.0f;
  /// transmit amplitude (0.0 - 1.0)
  float volume = 1.0f;
  /// transmitter: number of up chirps before the sync symbols
  int preamble_symbols = 8;
  /// transmitter: max number of bytes per frame (1 - 200)
  int max_frame = 64;
  /// Reed-Solomon parity bytes of the payload (2 - 64): corrects ecc_bytes /
  /// 2 wrong bytes
  int ecc_bytes = 8;
  /// receiver: min ratio of the peak power to the mean power of the other
  /// bins to detect the preamble
  float detect_threshold = 8.0f;
  /// receiver: stop after the first frame
  bool rx_one = false;
  /// receiver: input channel which is used for the decoding
  uint8_t channel = 0;
  /// receiver: (testing only) amplitude of added uniform noise
  float rx_noise = 0.0f;

  /// Selects e.g. "css7" or "css9-500": returns false if invalid. The other
  /// settings are kept.
  bool setMode(const char* mode) {
    int sf;
    float bw;
    if (!parseMode(mode, sf, bw)) return false;
    spreading_factor = sf;
    bandwidth = bw;
    return true;
  }

  /// true if the mode is a chirp spread spectrum mode
  static bool isMode(const char* mode) {
    int sf;
    float bw;
    return parseMode(mode, sf, bw);
  }

  /// chips (FFT bins) per symbol
  int chips() const { return 1 << spreading_factor; }

  /// duration of a symbol in seconds
  float symbolTime() const { return chips() / bandwidth; }

  /// symbols per second
  float symbolRate() const { return bandwidth / chips(); }

  /// lowest frequency of the chirps in Hz
  float lowFreq() const { return center_freq - bandwidth / 2; }

  /// Checks the configuration: returns false (and logs the reason) if invalid
  bool isValid() const {
    if (spreading_factor < 5 || spreading_factor > 10) {
      LOGE("CSS: the spreading factor must be 5 - 10");
      return false;
    }
    if (bandwidth <= 0 || lowFreq() <= 0) {
      LOGE("CSS: invalid bandwidth or center frequency");
      return false;
    }
    if (center_freq + bandwidth / 2 >= 0.45f * sample_rate) {
      LOGE("CSS: the highest frequency must be below %.1f Hz",
           (double)(0.45f * sample_rate));
      return false;
    }
    if (preamble_symbols < 4 || max_frame < 1 || max_frame > MAX_FRAME ||
        ecc_bytes < 2 || ecc_bytes > RSCodec::MAX_ECC) {
      LOGE("CSS: invalid preamble_symbols, max_frame or ecc_bytes");
      return false;
    }
    return true;
  }

  /// max payload of a frame
  static constexpr int MAX_FRAME = 200;
  /// header: length byte and Reed-Solomon parity
  static constexpr int HEADER_ECC = 4;

 protected:
  static bool parseMode(const char* mode, int& sf, float& bw) {
    if (mode == nullptr) return false;
    const char* prefix = "css";
    for (int j = 0; j < 3; j++)
      if (tolower(mode[j]) != prefix[j]) return false;
    sf = 7;
    bw = 1000.0f;
    const char* rest = mode + 3;
    if (*rest == 0) return true;
    char* end;
    sf = strtol(rest, &end, 10);
    if (end == rest || sf < 5 || sf > 10) return false;
    if (*end == '-') {
      const char* number = end + 1;
      bw = strtol(number, &end, 10);
      if (end == number || bw < 100) return false;
    }
    return *end == 0;
  }
};

/**
 * @brief Frame format of the chirp spread spectrum modem: the bytes of the
 * header code word (length and parity) and of the payload code word (data,
 * CRC-16 and parity) are sent as SF bits per symbol, most significant bit
 * first. The symbol value is the inverse Gray code of the bits, so that a
 * detection in the neighboring FFT bin causes only 1 wrong bit.
 * @copyright GPLv3
 */
class CSSFrame {
 public:
  static constexpr int HEADER_BYTES = 1 + CSSConfig::HEADER_ECC;

  /// number of symbols for the indicated number of bytes
  static int symbols(int bytes, int sf) { return (bytes * 8 + sf - 1) / sf; }

  /// bytes of the payload code word
  static int payloadBytes(int len, int ecc) { return len + 2 + ecc; }

  /// Header code word: length and parity
  static void header(int len, uint8_t* out) {
    uint8_t msg = len;
    RSCodec::encode(&msg, 1, CSSConfig::HEADER_ECC, out);
  }

  /// Corrects the header in place: returns the length or -1 if invalid
  static int decodeHeader(uint8_t* data) {
    if (RSCodec::correct(data, 1, CSSConfig::HEADER_ECC) < 0) return -1;
    int len = data[0];
    return len >= 1 && len <= CSSConfig::MAX_FRAME ? len : -1;
  }

  /// Payload code word: data, CRC-16 and parity
  static void payload(const uint8_t* data, int len, int ecc, uint8_t* out) {
    uint8_t msg[CSSConfig::MAX_FRAME + 2];
    memcpy(msg, data, len);
    uint16_t crc = crc16(data, len);
    msg[len] = crc >> 8;
    msg[len + 1] = crc & 0xFF;
    RSCodec::encode(msg, len + 2, ecc, out);
  }

  /// Corrects the payload in place and checks the CRC: returns the number
  /// of corrected bytes or -1 if invalid
  static int decodePayload(uint8_t* data, int len, int ecc) {
    int corrected = RSCodec::correct(data, len + 2, ecc);
    if (corrected < 0) return -1;
    uint16_t crc = crc16(data, len);
    if (data[len] != (crc >> 8) || data[len + 1] != (crc & 0xFF)) return -1;
    return corrected;
  }

  /// Converts bytes into symbols
  static int toSymbols(const uint8_t* data, int len, int sf, uint16_t* out) {
    int n = symbols(len, sf);
    for (int s = 0; s < n; s++) {
      int value = 0;
      for (int b = 0; b < sf; b++) {
        int bit = s * sf + b;
        int v = bit < len * 8 ? (data[bit / 8] >> (7 - bit % 8)) & 1 : 0;
        value = (value << 1) | v;
      }
      out[s] = inverseGray(value);
    }
    return n;
  }

  /// Converts symbols into bytes
  static void fromSymbols(const uint16_t* symbols, int n, int sf,
                          uint8_t* data, int len) {
    memset(data, 0, len);
    for (int s = 0; s < n; s++) {
      int value = symbols[s] ^ (symbols[s] >> 1);  // Gray code
      for (int b = 0; b < sf; b++) {
        int bit = s * sf + b;
        if (bit >= len * 8) return;
        if ((value >> (sf - 1 - b)) & 1) data[bit / 8] |= 0x80 >> (bit % 8);
      }
    }
  }

  static int inverseGray(int g) {
    g ^= g >> 1;
    g ^= g >> 2;
    g ^= g >> 4;
    g ^= g >> 8;
    return g;
  }
};

}  // namespace tinymodem
