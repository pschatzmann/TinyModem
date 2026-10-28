#pragma once

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "AudioTools.h"

namespace tinymodem {

/**
 * @brief Configuration of the PSK modem (PSKEncoder, PSKDecoder): BPSK with
 * Varicode and cosine shaped phase reversals like PSK31.
 *
 * setMode() selects "psk31", "psk63", "psk125", "psk250" or "psk500" (31.25
 * to 500 baud). The carrier frequency is 1000 Hz by default.
 * @copyright GPLv3
 */
struct PSKConfig : public audio_tools::AudioInfo {
  PSKConfig() {
    sample_rate = 48000;
    channels = 1;
    bits_per_sample = 16;
  }

  /// symbols (bits) per second
  float symbol_rate = 31.25f;
  /// carrier frequency in Hz
  float carrier_freq = 1000.0f;
  /// transmit amplitude (0.0 - 1.0)
  float volume = 1.0f;
  /// transmitter: number of phase reversals before the data
  int preamble_symbols = 32;
  /// transmitter: number of symbols with a steady carrier after the data
  int postamble_symbols = 32;
  /// receiver: frequency tracking (automatic frequency control)
  bool afc = true;
  /// receiver: minimum signal level (1.0 = full scale)
  float min_amplitude = 0.005f;
  /// receiver: stop after the first transmission
  bool rx_one = false;
  /// receiver: input channel which is used for the decoding
  uint8_t channel = 0;
  /// receiver: (testing only) amplitude of added uniform noise
  float rx_noise = 0.0f;

  /// Selects "psk31", "psk63", "psk125", "psk250" or "psk500": returns false
  /// if invalid. The other settings are kept.
  bool setMode(const char* mode) {
    float rate = modeRate(mode);
    if (rate <= 0) return false;
    symbol_rate = rate;
    return true;
  }

  /// true if the mode is a PSK mode
  static bool isMode(const char* mode) { return modeRate(mode) > 0; }

  /// number of samples per symbol
  float samplesPerSymbol() const { return sample_rate / symbol_rate; }

  /// Checks the configuration: returns false (and logs the reason) if invalid
  bool isValid() const {
    if (symbol_rate <= 0 || carrier_freq <= 0) {
      LOGE("PSK: invalid symbol_rate or carrier_freq");
      return false;
    }
    if (carrier_freq + symbol_rate >= sample_rate / 2.0f) {
      LOGE("PSK: the carrier must be below %.1f Hz",
           (double)(sample_rate / 2.0f - symbol_rate));
      return false;
    }
    if (carrier_freq < 2 * symbol_rate) {
      LOGE("PSK: the carrier must be at least twice the symbol rate");
      return false;
    }
    if (samplesPerSymbol() < 8) {
      LOGE("PSK: at least 8 samples per symbol are needed");
      return false;
    }
    return true;
  }

 protected:
  static float modeRate(const char* mode) {
    if (mode == nullptr) return 0;
    const char* prefix = "psk";
    for (int j = 0; j < 3; j++)
      if (tolower(mode[j]) != prefix[j]) return 0;
    int n = atoi(mode + 3);
    switch (n) {
      case 31:
        return 31.25f;
      case 63:
        return 62.5f;
      case 125:
      case 250:
      case 500:
        return n;
      default:
        return 0;
    }
  }
};

/**
 * @brief The PSK31 Varicode (as extended by fldigi to all 256 byte values):
 * frequent characters have short codes. A code starts and ends with 1 and
 * contains no "00": the characters are separated by "00".
 * @copyright GPLv3
 */
class Varicode {
 public:
  struct Code {
    uint16_t bits;   // most significant bit is sent first
    uint8_t length;  // number of bits
  };

  /// Code of the indicated byte
  static Code encode(uint8_t ch) { return table()[ch]; }

  /// Byte of the indicated code: returns -1 if the code is unknown
  static int decode(uint16_t bits) {
    for (int j = 0; j < 256; j++)
      if (table()[j].bits == bits) return j;
    return -1;
  }

 protected:
  static const Code* table() {
    // generated from fldigi's varicodetab1 (src/psk/pskvaricode.cxx)
    static const Code codes[256] = {
      {0x2AB, 10}, {0x2DB, 10}, {0x2ED, 10}, {0x377, 10}, {0x2EB, 10}, {0x35F, 10},
      {0x2EF, 10}, {0x2FD, 10}, {0x2FF, 10}, {0x0EF, 8}, {0x01D, 5}, {0x36F, 10},
      {0x2DD, 10}, {0x01F, 5}, {0x375, 10}, {0x3AB, 10}, {0x2F7, 10}, {0x2F5, 10},
      {0x3AD, 10}, {0x3AF, 10}, {0x35B, 10}, {0x36B, 10}, {0x36D, 10}, {0x357, 10},
      {0x37B, 10}, {0x37D, 10}, {0x3B7, 10}, {0x355, 10}, {0x35D, 10}, {0x3BB, 10},
      {0x2FB, 10}, {0x37F, 10}, {0x001, 1}, {0x1FF, 9}, {0x15F, 9}, {0x1F5, 9},
      {0x1DB, 9}, {0x2D5, 10}, {0x2BB, 10}, {0x17F, 9}, {0x0FB, 8}, {0x0F7, 8},
      {0x16F, 9}, {0x1DF, 9}, {0x075, 7}, {0x035, 6}, {0x057, 7}, {0x1AF, 9},
      {0x0B7, 8}, {0x0BD, 8}, {0x0ED, 8}, {0x0FF, 8}, {0x177, 9}, {0x15B, 9},
      {0x16B, 9}, {0x1AD, 9}, {0x1AB, 9}, {0x1B7, 9}, {0x0F5, 8}, {0x1BD, 9},
      {0x1ED, 9}, {0x055, 7}, {0x1D7, 9}, {0x2AF, 10}, {0x2BD, 10}, {0x07D, 7},
      {0x0EB, 8}, {0x0AD, 8}, {0x0B5, 8}, {0x077, 7}, {0x0DB, 8}, {0x0FD, 8},
      {0x155, 9}, {0x07F, 7}, {0x1FD, 9}, {0x17D, 9}, {0x0D7, 8}, {0x0BB, 8},
      {0x0DD, 8}, {0x0AB, 8}, {0x0D5, 8}, {0x1DD, 9}, {0x0AF, 8}, {0x06F, 7},
      {0x06D, 7}, {0x157, 9}, {0x1B5, 9}, {0x15D, 9}, {0x175, 9}, {0x17B, 9},
      {0x2AD, 10}, {0x1F7, 9}, {0x1EF, 9}, {0x1FB, 9}, {0x2BF, 10}, {0x16D, 9},
      {0x2DF, 10}, {0x00B, 4}, {0x05F, 7}, {0x02F, 6}, {0x02D, 6}, {0x003, 2},
      {0x03D, 6}, {0x05B, 7}, {0x02B, 6}, {0x00D, 4}, {0x1EB, 9}, {0x0BF, 8},
      {0x01B, 5}, {0x03B, 6}, {0x00F, 4}, {0x007, 3}, {0x03F, 6}, {0x1BF, 9},
      {0x015, 5}, {0x017, 5}, {0x005, 3}, {0x037, 6}, {0x07B, 7}, {0x06B, 7},
      {0x0DF, 8}, {0x05D, 7}, {0x1D5, 9}, {0x2B7, 10}, {0x1BB, 9}, {0x2B5, 10},
      {0x2D7, 10}, {0x3B5, 10}, {0x3BD, 10}, {0x3BF, 10}, {0x3D5, 10}, {0x3D7, 10},
      {0x3DB, 10}, {0x3DD, 10}, {0x3DF, 10}, {0x3EB, 10}, {0x3ED, 10}, {0x3EF, 10},
      {0x3F5, 10}, {0x3F7, 10}, {0x3FB, 10}, {0x3FD, 10}, {0x3FF, 10}, {0x555, 11},
      {0x557, 11}, {0x55B, 11}, {0x55D, 11}, {0x55F, 11}, {0x56B, 11}, {0x56D, 11},
      {0x56F, 11}, {0x575, 11}, {0x577, 11}, {0x57B, 11}, {0x57D, 11}, {0x57F, 11},
      {0x5AB, 11}, {0x5AD, 11}, {0x5AF, 11}, {0x5B5, 11}, {0x5B7, 11}, {0x5BB, 11},
      {0x5BD, 11}, {0x5BF, 11}, {0x5D5, 11}, {0x5D7, 11}, {0x5DB, 11}, {0x5DD, 11},
      {0x5DF, 11}, {0x5EB, 11}, {0x5ED, 11}, {0x5EF, 11}, {0x5F5, 11}, {0x5F7, 11},
      {0x5FB, 11}, {0x5FD, 11}, {0x5FF, 11}, {0x6AB, 11}, {0x6AD, 11}, {0x6AF, 11},
      {0x6B5, 11}, {0x6B7, 11}, {0x6BB, 11}, {0x6BD, 11}, {0x6BF, 11}, {0x6D5, 11},
      {0x6D7, 11}, {0x6DB, 11}, {0x6DD, 11}, {0x6DF, 11}, {0x6EB, 11}, {0x6ED, 11},
      {0x6EF, 11}, {0x6F5, 11}, {0x6F7, 11}, {0x6FB, 11}, {0x6FD, 11}, {0x6FF, 11},
      {0x755, 11}, {0x757, 11}, {0x75B, 11}, {0x75D, 11}, {0x75F, 11}, {0x76B, 11},
      {0x76D, 11}, {0x76F, 11}, {0x775, 11}, {0x777, 11}, {0x77B, 11}, {0x77D, 11},
      {0x77F, 11}, {0x7AB, 11}, {0x7AD, 11}, {0x7AF, 11}, {0x7B5, 11}, {0x7B7, 11},
      {0x7BB, 11}, {0x7BD, 11}, {0x7BF, 11}, {0x7D5, 11}, {0x7D7, 11}, {0x7DB, 11},
      {0x7DD, 11}, {0x7DF, 11}, {0x7EB, 11}, {0x7ED, 11}, {0x7EF, 11}, {0x7F5, 11},
      {0x7F7, 11}, {0x7FB, 11}, {0x7FD, 11}, {0x7FF, 11}, {0xAAB, 12}, {0xAAD, 12},
      {0xAAF, 12}, {0xAB5, 12}, {0xAB7, 12}, {0xABB, 12}, {0xABD, 12}, {0xABF, 12},
      {0xAD5, 12}, {0xAD7, 12}, {0xADB, 12}, {0xADD, 12}, {0xADF, 12}, {0xAEB, 12},
      {0xAED, 12}, {0xAEF, 12}, {0xAF5, 12}, {0xAF7, 12}, {0xAFB, 12}, {0xAFD, 12},
      {0xAFF, 12}, {0xB55, 12}, {0xB57, 12}, {0xB5B, 12}
    };
    return codes;
  }
};

}  // namespace tinymodem
