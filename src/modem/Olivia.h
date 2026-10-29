#pragma once

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "AudioTools.h"
#include "DSPUtils.h"

namespace tinymodem {

/**
 * @brief Configuration of the Olivia and Contestia modems (OliviaEncoder,
 * OliviaDecoder): MFSK with 2 to 256 tones and a strong forward error
 * correction (Walsh-Hadamard codes over blocks of 64 or 32 symbols), as
 * designed by Pawel Jalocha and implemented by fldigi.
 *
 * setMode() selects "olivia-T-B" or "contestia-T-B" with T tones and a
 * bandwidth B of 125, 250, 500, 1000 or 2000 Hz (also written as 1k or 2k),
 * e.g. "olivia-8-250" or "olivia-32-1000". "olivia" is "olivia-32-1000",
 * "contestia" is "contestia-8-250". Olivia sends 7 bit characters (and 8 bit
 * characters escaped with 127 like fldigi), Contestia sends upper case text
 * with 6 bit characters at twice the speed: a line break is sent as '\r'
 * (which fldigi shows as new line), '\r' is ignored.
 *
 * The signal is defined at 8000 Hz: other sample rates are converted.
 * @copyright GPLv3
 */
struct OliviaConfig : public audio_tools::AudioInfo {
  OliviaConfig() {
    sample_rate = 48000;
    channels = 1;
    bits_per_sample = 16;
  }

  /// number of tones (power of 2: 2 - 256)
  int tones = 32;
  /// bandwidth in Hz: 125, 250, 500, 1000 or 2000
  int bandwidth = 1000;
  /// center frequency in Hz
  float center_freq = 1500.0f;
  /// Contestia instead of Olivia
  bool contestia = false;
  /// transmit amplitude (0.0 - 1.0)
  float volume = 1.0f;
  /// transmitter: send the start and stop tones (like fldigi)
  bool start_tones = true;
  /// receiver: search range for the frequency in tone spacings / 2 (fldigi:
  /// tune margin): limited to the available audio band
  int sync_margin = 8;
  /// receiver: integration period of the synchronizer in FEC blocks
  int sync_integration = 4;
  /// receiver: minimum signal to noise ratio of the synchronizer
  float sync_threshold = 3.0f;
  /// receiver: stop after the first transmission
  bool rx_one = false;
  /// receiver: input channel which is used for the decoding
  uint8_t channel = 0;
  /// receiver: (testing only) amplitude of added uniform noise
  float rx_noise = 0.0f;

  /// internal sample rate which defines the signal
  static constexpr float BASE_RATE = 8000.0f;

  /// Selects e.g. "olivia-32-1000", "olivia-8-250" or "contestia-8-250":
  /// returns false if invalid. The other settings are kept.
  bool setMode(const char* mode) {
    int t, b;
    bool c;
    if (!parseMode(mode, t, b, c)) return false;
    tones = t;
    bandwidth = b;
    contestia = c;
    return true;
  }

  /// true if the mode is an Olivia or Contestia mode
  static bool isMode(const char* mode) {
    int t, b;
    bool c;
    return parseMode(mode, t, b, c);
  }

  /// number of bits per symbol (log2 of the tones)
  int bitsPerSymbol() const {
    int bits = 0;
    while ((1 << (bits + 1)) <= tones) bits++;
    return bits;
  }

  /// FFT length (window of a symbol) in samples at 8000 Hz
  int symbolLen() const {
    int bw = 0;
    while ((125 << (bw + 1)) <= bandwidth) bw++;
    return 1 << (bitsPerSymbol() + 7 - bw);
  }

  /// distance of two symbols in samples at 8000 Hz
  int symbolSepar() const { return symbolLen() / 2; }

  /// symbols per FEC block: 64 for Olivia, 32 for Contestia
  int symbolsPerBlock() const { return contestia ? 32 : 64; }

  /// bits per character: 7 for Olivia, 6 for Contestia
  int bitsPerCharacter() const { return contestia ? 6 : 7; }

  /// lowest tone as FFT bin (like fldigi)
  int firstCarrier() const {
    double offset = contestia ? bandwidth / 2.0
                              : bandwidth * (1.0 - 0.5 / tones) / 2.0;
    double multiplier = (center_freq - offset) / 500.0;
    return (int)((symbolLen() / 16) * multiplier) + 1;
  }

  /// frequency of a tone in Hz
  float toneFreq(int tone) const {
    return (firstCarrier() + 2 * tone) * BASE_RATE / symbolLen();
  }

  /// symbols per second
  float symbolRate() const { return BASE_RATE / symbolSepar(); }

  /// characters per second
  float charRate() const {
    return symbolRate() * bitsPerSymbol() / symbolsPerBlock();
  }

  /// Checks the configuration: returns false (and logs the reason) if invalid
  bool isValid() const {
    if (tones < 2 || tones > 256 || (tones & (tones - 1)) != 0) {
      LOGE("Olivia: tones must be a power of 2 (2 - 256)");
      return false;
    }
    if (!(bandwidth == 125 || bandwidth == 250 || bandwidth == 500 ||
          bandwidth == 1000 || bandwidth == 2000)) {
      LOGE("Olivia: bandwidth must be 125, 250, 500, 1000 or 2000");
      return false;
    }
    if (symbolLen() < 16 || symbolLen() > MAX_SYMBOL_LEN) {
      LOGE("Olivia: %d tones with %d Hz are not supported", tones, bandwidth);
      return false;
    }
    if (sync_margin < 1 || sync_integration < 1 || sync_threshold <= 0) {
      LOGE("Olivia: invalid synchronizer settings");
      return false;
    }
    if (firstCarrier() < 1 || toneFreq(tones) >= 0.45f * BASE_RATE) {
      LOGE("Olivia: the center frequency %.1f Hz is out of range",
           (double)center_freq);
      return false;
    }
    if (sample_rate < 4000) {
      LOGE("Olivia: the sample rate must be at least 4000 Hz");
      return false;
    }
    return true;
  }

  /// largest supported FFT length
  static constexpr int MAX_SYMBOL_LEN = 2048;

 protected:
  static bool parseMode(const char* mode, int& tones, int& bandwidth,
                        bool& contestia) {
    if (mode == nullptr) return false;
    const char* rest;
    if (startsWith(mode, "olivia")) {
      contestia = false;
      rest = mode + 6;
      tones = 32;
      bandwidth = 1000;
    } else if (startsWith(mode, "contestia")) {
      contestia = true;
      rest = mode + 9;
      tones = 8;
      bandwidth = 250;
    } else {
      return false;
    }
    if (*rest == 0) return true;
    if (*rest != '-' && *rest != '_') return false;
    char* end;
    tones = strtol(rest + 1, &end, 10);
    if (*end != '-' && *end != '_' && *end != '/') return false;
    bandwidth = strtol(end + 1, &end, 10);
    if (tolower(*end) == 'k') {
      bandwidth *= 1000;
      end++;
    }
    if (*end != 0) return false;
    if (tones < 2 || tones > 256 || (tones & (tones - 1)) != 0) return false;
    return bandwidth == 125 || bandwidth == 250 || bandwidth == 500 ||
           bandwidth == 1000 || bandwidth == 2000;
  }

  static bool startsWith(const char* str, const char* prefix) {
    while (*prefix) {
      if (tolower(*str++) != *prefix++) return false;
    }
    return true;
  }
};

/**
 * @brief The forward error correction of Olivia and Contestia (as in fldigi):
 * each character of a block of bitsPerSymbol() characters is a Walsh-Hadamard
 * code word of 64 (Contestia: 32) bits, which is scrambled and spread
 * diagonally over the 64 (32) symbols of the block.
 * @copyright GPLv3
 */
class OliviaCodec {
 public:
  void begin(int bits_per_symbol, bool is_contestia) {
    bits = bits_per_symbol;
    contestia = is_contestia;
    block_len = contestia ? 32 : 64;
  }

  /// true if the character is not transmitted (Contestia: '\r', which is
  /// replaced by '\n')
  bool isSkipped(uint8_t ch) const { return contestia && ch == '\r'; }

  /// Encodes bits characters into block_len symbols (binary, not Gray coded)
  void encodeBlock(const uint8_t* chars, uint8_t* symbols) {
    int8_t fht[64];
    for (int t = 0; t < block_len; t++) symbols[t] = 0;
    for (int b = 0; b < bits; b++) {
      for (int t = 0; t < block_len; t++) fht[t] = 0;
      uint8_t ch = encodeChar(chars[b]);
      if (ch < block_len)
        fht[ch] = 1;
      else
        fht[ch - block_len] = -1;
      ifht(fht, block_len);
      int code_bit = (b * shift()) & (block_len - 1);
      int rotate = 0;
      for (int t = 0; t < block_len; t++) {
        if ((scramblingCode() >> code_bit) & 1) fht[t] = -fht[t];
        code_bit = (code_bit + 1) & (block_len - 1);
        if (fht[t] < 0) {
          int bit = b + rotate;
          if (bit >= bits) bit -= bits;
          symbols[t] |= 1 << bit;
        }
        if (++rotate >= bits) rotate = 0;
      }
    }
  }

  /// Decodes the character b from the soft bits of a block: soft[t * bits +
  /// k] is bit k of symbol t (positive = 0). Provides the correlation peak
  /// and the energy of the other correlations.
  uint8_t decodeChar(const int8_t* soft, int b, float& peak, float& noise) {
    int32_t fht[64];
    int code_bit = (b * shift()) & (block_len - 1);
    int rotate = b;
    for (int t = 0; t < block_len; t++) {
      int32_t bit = soft[t * bits + rotate];
      if ((scramblingCode() >> code_bit) & 1) bit = -bit;
      fht[t] = bit;
      code_bit = (code_bit + 1) & (block_len - 1);
      if (++rotate >= bits) rotate = 0;
    }
    fhtTransform(fht, block_len);
    int32_t max = 0;
    int pos = 0;
    float sum = 0;
    for (int t = 0; t < block_len; t++) {
      float v = fht[t];
      sum += v * v;
      if (abs(fht[t]) > abs(max)) {
        max = fht[t];
        pos = t;
      }
    }
    uint8_t ch = pos;
    if (max < 0) ch += block_len;
    peak = abs(max);
    noise = (sum - peak * peak) / (block_len - 1);
    return decodeChar(ch);
  }

  int blockLen() const { return block_len; }

  static uint8_t grayCode(uint8_t binary) { return binary ^ (binary >> 1); }

  static uint8_t binaryCode(uint8_t gray) {
    gray ^= gray >> 4;
    gray ^= gray >> 2;
    gray ^= gray >> 1;
    return gray;
  }

 protected:
  int bits = 5;
  int block_len = 64;
  bool contestia = false;

  uint64_t scramblingCode() const {
    return contestia ? 0xEDB88320ull : 0xE257E6D0291574ECull;
  }

  int shift() const { return contestia ? 5 : 13; }

  uint8_t encodeChar(uint8_t ch) const {
    if (!contestia) return ch & 0x7F;
    if (ch >= 'a' && ch <= 'z') ch += 'A' - 'a';
    if (ch == ' ') return 59;
    if (ch == '\n') return 60;  // fldigi shows '\r' as new line
    if (ch == 0) return 0;
    if (ch >= 33 && ch <= 90) return ch - 32;
    if (ch == 8) return 61;
    return '?' - 32;
  }

  uint8_t decodeChar(uint8_t ch) const {
    if (!contestia || ch == 0) return ch;
    if (ch == 59) return ' ';
    if (ch == 60) return '\n';
    if (ch == 61) return 8;
    return ch + 32;
  }

  /// fast Hadamard transform (fldigi FHT)
  template <class T>
  static void fhtTransform(T* data, int len) {
    for (int step = 1; step < len; step *= 2)
      for (int ptr = 0; ptr < len; ptr += 2 * step)
        for (int j = ptr; j < ptr + step; j++) {
          T a = data[j], b = data[j + step];
          data[j] = b + a;
          data[j + step] = b - a;
        }
  }

  /// inverse fast Hadamard transform (fldigi IFHT)
  template <class T>
  static void ifht(T* data, int len) {
    for (int step = len / 2; step; step /= 2)
      for (int ptr = 0; ptr < len; ptr += 2 * step)
        for (int j = ptr; j < ptr + step; j++) {
          T a = data[j], b = data[j + step];
          data[j] = a - b;
          data[j + step] = a + b;
        }
  }
};

}  // namespace tinymodem
