#pragma once

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "AudioTools.h"

namespace tinymodem {

/**
 * @brief Configuration of the multi-tone FSK modem (MFSKEncoder,
 * MFSKDecoder). Each symbol is one of `tones` tones and carries
 * log2(tones) bits. The tones are base_freq + i * tone_spacing.
 *
 * Use setMode() to select a predefined mode: "mfsk4", "mfsk8", "mfsk16",
 * "mfsk32" or "mfsk64". All of them use tones between 1000 and 3000 Hz, so
 * they work with speakers, microphones and telephone lines. More tones are
 * slower but more robust against noise.
 * @copyright GPLv3
 */
struct MFSKConfig : public audio_tools::AudioInfo {
  MFSKConfig() {
    sample_rate = 48000;
    channels = 1;
    bits_per_sample = 16;
  }

  /// number of tones: 4, 8, 16, 32 or 64
  int tones = 16;
  /// symbols per second
  float symbol_rate = 125.0f;
  /// frequency of the lowest tone in Hz
  float base_freq = 1000.0f;
  /// distance between the tones in Hz (0 = symbol_rate)
  float tone_spacing = 0.0f;
  /// transmit amplitude (0.0 - 1.0)
  float volume = 1.0f;
  /// number of preamble symbols (alternating lowest and highest tone)
  int preamble_symbols = 8;
  /// receiver: minimum amplitude of a tone (1.0 = full scale)
  float min_amplitude = 0.005f;
  /// receiver: stop after the first carrier was lost
  bool rx_one = false;
  /// receiver: input channel which is used for the decoding
  uint8_t channel = 0;
  /// receiver: (testing only) amplitude of added uniform noise
  float rx_noise = 0.0f;

  /// Selects a predefined mode ("mfsk4" to "mfsk64"): returns false if
  /// invalid. The audio format and the receiver settings are kept.
  bool setMode(const char* mode) {
    if (mode == nullptr) return false;
    if (!isMode(mode)) return false;
    int n = atoi(mode + 4);
    switch (n) {
      case 4:
        symbol_rate = 500.0f;
        break;
      case 8:
        symbol_rate = 250.0f;
        break;
      case 16:
        symbol_rate = 125.0f;
        break;
      case 32:
        symbol_rate = 62.5f;
        break;
      case 64:
        symbol_rate = 31.25f;
        break;
      default:
        return false;
    }
    tones = n;
    base_freq = 1000.0f;
    tone_spacing = 0.0f;
    return true;
  }

  /// true if the mode name is a MFSK mode
  static bool isMode(const char* mode) {
    if (mode == nullptr) return false;
    const char* prefix = "mfsk";
    for (int j = 0; j < 4; j++)
      if (tolower(mode[j]) != prefix[j]) return false;
    return true;
  }

  /// bits per symbol: log2(tones)
  int bitsPerSymbol() const {
    int bits = 0;
    while ((1 << bits) < tones) bits++;
    return bits;
  }

  /// distance between the tones in Hz
  float spacing() const { return tone_spacing > 0 ? tone_spacing : symbol_rate; }

  /// frequency of the tone with the indicated index
  float toneFrequency(int idx) const { return base_freq + idx * spacing(); }

  /// number of sync symbols after the preamble: they carry at least 8 bits
  int syncLength() const {
    int n = (8 + bitsPerSymbol() - 1) / bitsPerSymbol();
    return n < 2 ? 2 : n;
  }

  /// tone of the sync symbol with the indicated index
  int syncSymbol(int idx) const {
    static const int pattern[4] = {2, 4, 4, 2};  // tones / 2, tones / 4
    return tones / pattern[idx % 4];
  }

  /// data bits per second
  float bitRate() const { return symbol_rate * bitsPerSymbol(); }

  /// number of samples per symbol
  float samplesPerSymbol() const { return sample_rate / symbol_rate; }

  /// Checks the configuration: returns false (and logs the reason) if invalid
  bool isValid() const {
    if (tones < 4 || tones > 64 || (tones & (tones - 1)) != 0) {
      LOGE("MFSK: tones must be 4, 8, 16, 32 or 64, not %d", tones);
      return false;
    }
    if (symbol_rate <= 0 || base_freq <= 0 || tone_spacing < 0) {
      LOGE("MFSK: invalid symbol_rate, base_freq or tone_spacing");
      return false;
    }
    if (toneFrequency(tones - 1) >= sample_rate / 2.0f) {
      LOGE("MFSK: highest tone %.1f Hz must be below %.1f Hz",
           (double)toneFrequency(tones - 1), (double)(sample_rate / 2.0f));
      return false;
    }
    if (samplesPerSymbol() < 8) {
      LOGE("MFSK: at least 8 samples per symbol are needed");
      return false;
    }
    if (preamble_symbols < 4) {
      LOGE("MFSK: at least 4 preamble symbols are needed");
      return false;
    }
    return true;
  }
};

}  // namespace tinymodem
