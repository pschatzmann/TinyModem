#pragma once

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "AudioTools.h"

namespace tinymodem {

/**
 * @brief Configuration of the OFDM modem (OFDMEncoder, OFDMDecoder): the data
 * is transmitted in parallel on `carriers` subcarriers with differential QPSK
 * (2 bits per carrier and symbol). Each symbol has a cyclic prefix (guard
 * interval) against echoes.
 *
 * setMode() selects "ofdm16", "ofdm32" or "ofdm64": all use the band from
 * 1000 Hz to about 3000 Hz and transmit 3200 bit/s. More carriers result in
 * longer symbols with a longer guard interval.
 * @copyright GPLv3
 */
struct OFDMConfig : public audio_tools::AudioInfo {
  OFDMConfig() {
    sample_rate = 48000;
    channels = 1;
    bits_per_sample = 16;
  }

  /// number of carriers (4 - 128)
  int carriers = 32;
  /// distance of the carriers in Hz
  float carrier_spacing = 62.5f;
  /// frequency of the first carrier in Hz
  float first_freq = 1000.0f;
  /// length of the cyclic prefix relative to the symbol (0.0 - 0.5)
  float guard = 0.25f;
  /// transmit amplitude (0.0 - 1.0)
  float volume = 1.0f;
  /// transmitter: max number of bytes per frame
  int max_frame = 512;
  /// receiver: minimum normalized correlation of the sync symbol
  float sync_threshold = 0.5f;
  /// receiver: stop after the first frame
  bool rx_one = false;
  /// receiver: input channel which is used for the decoding
  uint8_t channel = 0;
  /// receiver: (testing only) amplitude of added uniform noise
  float rx_noise = 0.0f;

  /// Selects "ofdm16", "ofdm32" or "ofdm64": returns false if invalid
  bool setMode(const char* mode) {
    int n = modeCarriers(mode);
    if (n == 0) return false;
    carriers = n;
    carrier_spacing = 2000.0f / n;
    first_freq = 1000.0f;
    return true;
  }

  /// true if the mode is an OFDM mode
  static bool isMode(const char* mode) { return modeCarriers(mode) > 0; }

  /// length of the useful part of a symbol (DFT size) in samples
  int fftSize() const {
    int n = lroundf(sample_rate / carrier_spacing);
    return n + (n & 1);  // even, for the sync symbol
  }

  /// length of the cyclic prefix in samples
  int prefixSize() const { return lroundf(fftSize() * guard); }

  /// DFT bin of the first carrier: even, for the sync symbol
  int firstBin() const {
    int bin = lroundf(first_freq * fftSize() / sample_rate);
    return bin + (bin & 1);
  }

  /// frequency of the carrier with the indicated index
  float carrierFrequency(int idx) const {
    return (float)(firstBin() + idx) * sample_rate / fftSize();
  }

  /// OFDM symbols per second
  float symbolRate() const {
    return (float)sample_rate / (fftSize() + prefixSize());
  }

  /// data bits per second
  float bitRate() const { return symbolRate() * carriers * 2; }

  /// Checks the configuration: returns false (and logs the reason) if invalid
  bool isValid() const {
    if (carriers < 4 || carriers > 128) {
      LOGE("OFDM: carriers must be between 4 and 128, not %d", carriers);
      return false;
    }
    if (carrier_spacing <= 0 || first_freq <= 0 || guard < 0 || guard > 0.5f) {
      LOGE("OFDM: invalid carrier_spacing, first_freq or guard");
      return false;
    }
    if (2 * (firstBin() + carriers) >= fftSize()) {
      LOGE("OFDM: the highest carrier %.1f Hz must be below %.1f Hz",
           (double)carrierFrequency(carriers - 1),
           (double)(sample_rate / 2.0f));
      return false;
    }
    if (max_frame < 1 || max_frame > 65535) {
      LOGE("OFDM: invalid max_frame: %d", max_frame);
      return false;
    }
    return true;
  }

  /// Number of symbols of the header (32 bits, one bit per carrier)
  static int headerSymbols(int carriers) { return (32 + carriers - 1) / carriers; }

  /// Known phase (0 - 3, in steps of 90 degrees) of a carrier in the sync
  /// (symbol = 0) and reference (symbol = 1) symbols
  static int knownPhase(int symbol, int carrier) {
    uint32_t x = (uint32_t)(carrier + 1) * 2654435761u + symbol * 40503u;
    x ^= x >> 13;
    x *= 0x5bd1e995u;
    x ^= x >> 15;
    return x & 3;
  }

 protected:
  static int modeCarriers(const char* mode) {
    if (mode == nullptr) return 0;
    const char* prefix = "ofdm";
    for (int j = 0; j < 4; j++)
      if (tolower(mode[j]) != prefix[j]) return 0;
    int n = atoi(mode + 4);
    return (n == 16 || n == 32 || n == 64) ? n : 0;
  }
};

/// Sine and cosine of 2 * PI * i / size
class OFDMTable {
 public:
  void begin(int size) {
    this->size = size;
    cos_table.resize(size);
    sin_table.resize(size);
    for (int i = 0; i < size; i++) {
      double angle = 2.0 * M_PI * i / size;
      cos_table[i] = cos(angle);
      sin_table[i] = sin(angle);
    }
  }
  float cosine(int i) const { return cos_table[i]; }
  float sine(int i) const { return sin_table[i]; }

 protected:
  int size = 0;
  audio_tools::Vector<float> cos_table;
  audio_tools::Vector<float> sin_table;
};

}  // namespace tinymodem
