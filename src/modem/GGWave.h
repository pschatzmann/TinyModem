#pragma once

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "AudioTools.h"
#include "ReedSolomon.h"

namespace tinymodem {

/**
 * @brief Protocol ids of ggwave: the values are identical to ggwave's
 * ggwave_ProtocolId. The mono-tone protocols (MT, 9 - 11) are not supported.
 */
enum GGWaveProtocolId {
  GGWAVE_AUDIBLE_NORMAL = 0,
  GGWAVE_AUDIBLE_FAST = 1,
  GGWAVE_AUDIBLE_FASTEST = 2,
  GGWAVE_ULTRASOUND_NORMAL = 3,
  GGWAVE_ULTRASOUND_FAST = 4,
  GGWAVE_ULTRASOUND_FASTEST = 5,
  GGWAVE_DT_NORMAL = 6,
  GGWAVE_DT_FAST = 7,
  GGWAVE_DT_FASTEST = 8,
};

/**
 * @brief Transmission protocol of ggwave (https://github.com/ggerganov/ggwave):
 * the ids and parameters are identical to the original. The tones are
 * multiples of sample_rate / samplesPerFrame (46.875 Hz at 48 kHz).
 * @copyright GPLv3
 */
struct GGWaveProtocol {
  /// ggwave protocol id
  GGWaveProtocolId id;
  /// TinyModem mode name
  const char* mode;
  /// FFT bin of the lowest tone
  int freq_start;
  /// number of frames per transmitted chunk (symbol)
  int frames_per_tx;
  /// number of bytes per chunk: 2 tones per byte
  int bytes_per_tx;

  /// number of simultaneous tones in a data symbol
  int tones() const { return 2 * bytes_per_tx; }
};

/// Reed-Solomon code of ggwave: the generator roots are alpha^0 to
/// alpha^(ecc-1)
using GGWaveRS = RSCodec;

/**
 * @brief Configuration of the ggwave compatible modem (GGWaveEncoder,
 * GGWaveDecoder). Each message (up to 140 bytes) starts and ends with a marker
 * and is protected by Reed-Solomon codes. Each byte is sent as 2 of 16 tones
 * which are transmitted at the same time.
 *
 * setMode() selects the protocol: "ggwave-normal", "ggwave-fast",
 * "ggwave-fastest" (audible, 1875 - 6375 Hz), "ggwave-u-normal",
 * "ggwave-u-fast", "ggwave-u-fastest" (ultrasound, 15000 - 19500 Hz),
 * "ggwave-dt-normal", "ggwave-dt-fast" and "ggwave-dt-fastest" (dual tone,
 * 1125 - 2625 Hz). "ggwave" is "ggwave-fast", the default of the ggwave tools.
 * The variable length format is used, which is the default of ggwave. The
 * mono-tone (MT) protocols and the fixed length format are not supported.
 *
 * The sample rate must be a multiple of 46.875 Hz * 4, e.g. 48000 or 24000 Hz
 * (44100 Hz is not supported), and high enough for the highest tone.
 * @copyright GPLv3
 */
struct GGWaveConfig : public audio_tools::AudioInfo {
  GGWaveConfig() {
    sample_rate = 48000;
    channels = 1;
    bits_per_sample = 16;
  }

  /// samples per frame (DFT size) at 48000 Hz
  static constexpr int BASE_FRAME = 1024;
  static constexpr int BASE_RATE = 48000;
  /// max payload of a message (variable length format)
  static constexpr int MAX_PAYLOAD = 140;
  /// number of frames of the start and end marker
  static constexpr int MARKER_FRAMES = 16;
  /// number of tones of a marker
  static constexpr int MARKER_TONES = 16;
  /// bytes of the Reed-Solomon coded length at the start of a message
  static constexpr int LENGTH_BYTES = 3;

  /// ggwave protocol (see setMode())
  GGWaveProtocolId protocol = GGWAVE_AUDIBLE_FAST;
  /// transmit amplitude (0.0 - 1.0)
  float volume = 1.0f;
  /// XOR the payload with the ggwave DSS sequence (ggwave option -d)
  bool dss = false;
  /// receiver: accept the normal, fast and fastest variant of the protocol
  bool rx_any_speed = true;
  /// receiver: a marker tone must be this much stronger than its neighbor
  /// (in power)
  float marker_threshold = 3.0f;
  /// receiver: stop after the first message
  bool rx_one = false;
  /// receiver: input channel which is used for the decoding
  uint8_t channel = 0;
  /// receiver: (testing only) amplitude of added uniform noise
  float rx_noise = 0.0f;

  /// Selects the protocol by mode name: returns false if invalid
  bool setMode(const char* mode) {
    const GGWaveProtocol* p = findMode(mode);
    if (p == nullptr) return false;
    protocol = p->id;
    return true;
  }

  /// true if the mode is a ggwave mode
  static bool isMode(const char* mode) { return findMode(mode) != nullptr; }

  /// Provides the selected protocol
  const GGWaveProtocol& protocolInfo() const {
    const GGWaveProtocol* p = findProtocol(protocol);
    return p != nullptr ? *p : protocols()[1];
  }

  /// Number of supported protocols
  static int protocolCount() { return 9; }

  /// Provides the supported protocols
  static const GGWaveProtocol* protocols() {
    static const GGWaveProtocol table[] = {
        {GGWAVE_AUDIBLE_NORMAL, "ggwave-normal", 40, 9, 3},
        {GGWAVE_AUDIBLE_FAST, "ggwave-fast", 40, 6, 3},
        {GGWAVE_AUDIBLE_FASTEST, "ggwave-fastest", 40, 3, 3},
        {GGWAVE_ULTRASOUND_NORMAL, "ggwave-u-normal", 320, 9, 3},
        {GGWAVE_ULTRASOUND_FAST, "ggwave-u-fast", 320, 6, 3},
        {GGWAVE_ULTRASOUND_FASTEST, "ggwave-u-fastest", 320, 3, 3},
        {GGWAVE_DT_NORMAL, "ggwave-dt-normal", 24, 9, 1},
        {GGWAVE_DT_FAST, "ggwave-dt-fast", 24, 6, 1},
        {GGWAVE_DT_FASTEST, "ggwave-dt-fastest", 24, 3, 1},
    };
    return table;
  }

  /// Provides the protocol with the indicated id (or nullptr)
  static const GGWaveProtocol* findProtocol(int id) {
    if (id < 0 || id >= protocolCount()) return nullptr;
    return &protocols()[id];
  }

  /// Samples per frame: the tones are multiples of sample_rate / frameSize()
  int frameSize() const {
    return (int)((int64_t)sample_rate * BASE_FRAME / BASE_RATE);
  }

  /// Frequency of the indicated FFT bin in Hz
  float binFrequency(int bin) const {
    return (float)bin * sample_rate / frameSize();
  }

  /// Number of Reed-Solomon parity bytes for a payload of len bytes
  static int eccBytes(int len) {
    return len < 4 ? 2 : (2 * (len / 5) > 4 ? 2 * (len / 5) : 4);
  }

  /// Total number of encoded bytes of a message (length, payload, parity)
  static int encodedBytes(int len) { return LENGTH_BYTES + len + eccBytes(len); }

  /// Number of data symbols of a message with len bytes
  int dataSymbols(int len) const {
    int bpt = protocolInfo().bytes_per_tx;
    return (encodedBytes(len) + bpt - 1) / bpt;
  }

  /// Number of samples of a message with len bytes
  size_t messageSamples(int len) const {
    int frames = 2 * MARKER_FRAMES +
                 dataSymbols(len) * protocolInfo().frames_per_tx;
    return (size_t)frames * frameSize();
  }

  /// Data symbols per second
  float symbolRate() const {
    return (float)sample_rate / (frameSize() * protocolInfo().frames_per_tx);
  }

  /// Payload bits per second (without markers and error correction)
  float bitRate() const { return symbolRate() * 8 * protocolInfo().bytes_per_tx; }

  /// Checks the configuration: returns false (and logs the reason) if invalid
  bool isValid() const {
    if (findProtocol(protocol) == nullptr) {
      LOGE("ggwave: invalid protocol: %d", protocol);
      return false;
    }
    int n = frameSize();
    if ((int64_t)n * BASE_RATE != (int64_t)sample_rate * BASE_FRAME ||
        n % 4 != 0) {
      LOGE("ggwave: the sample rate %d is not supported (e.g. use 48000)",
           (int)sample_rate);
      return false;
    }
    const GGWaveProtocol& p = protocolInfo();
    int top = p.freq_start + 16 * p.tones();
    if (2 * top >= n) {
      LOGE("ggwave: the sample rate %d is too low for %s", (int)sample_rate,
           p.mode);
      return false;
    }
    if (marker_threshold < 1.0f) {
      LOGE("ggwave: marker_threshold must be >= 1");
      return false;
    }
    return true;
  }

  /// Value of the ggwave DSS sequence for the indicated payload byte
  static uint8_t dssMagic(int i) {
    static const uint8_t magic[64] = {
        0x96, 0x9f, 0xb4, 0xaf, 0x1b, 0x91, 0xde, 0xc5, 0x45, 0x75, 0xe8,
        0x2e, 0x0f, 0x32, 0x4a, 0x5f, 0xb4, 0x56, 0x95, 0xcb, 0x7f, 0x6a,
        0x54, 0x6a, 0x48, 0xf2, 0x0b, 0x7b, 0xcd, 0xfb, 0x93, 0x6d, 0x3c,
        0x77, 0x5e, 0xc3, 0x33, 0x47, 0xc0, 0xf1, 0x71, 0x32, 0x33, 0x27,
        0x35, 0x68, 0x47, 0x1f, 0x4e, 0xac, 0x23, 0x42, 0x5f, 0x00, 0x37,
        0xa4, 0x50, 0x6d, 0x48, 0x24, 0x91, 0x7c, 0xa1, 0x4e,
    };
    return magic[i % 64];
  }

 protected:
  static const GGWaveProtocol* findMode(const char* mode) {
    if (mode == nullptr) return nullptr;
    if (equals(mode, "ggwave")) return &protocols()[1];
    for (int j = 0; j < protocolCount(); j++)
      if (equals(mode, protocols()[j].mode)) return &protocols()[j];
    return nullptr;
  }

  static bool equals(const char* a, const char* b) {
    while (*a && *b) {
      if (tolower(*a) != tolower(*b)) return false;
      a++;
      b++;
    }
    return *a == *b;
  }
};

}  // namespace tinymodem
