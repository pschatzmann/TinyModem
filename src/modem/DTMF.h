#pragma once

#include <ctype.h>
#include <stdint.h>
#include <string.h>

#include "AudioTools.h"

namespace tinymodem {

/**
 * @brief Configuration of the DTMF (touch tone) modem (DTMFEncoder,
 * DTMFDecoder): each of the characters 0-9, *, #, A, B, C and D is sent as
 * the sum of a row tone (697 - 941 Hz) and a column tone (1209 - 1633 Hz).
 *
 * The receiver needs tones and pauses of at least about 45 ms. The mode name
 * is "dtmf".
 * @copyright GPLv3
 */
struct DTMFConfig : public audio_tools::AudioInfo {
  DTMFConfig() {
    sample_rate = 48000;
    channels = 1;
    bits_per_sample = 16;
  }

  /// transmitter: duration of a tone in ms
  int tone_ms = 70;
  /// transmitter: duration of the pause after a tone in ms
  int pause_ms = 70;
  /// transmitter: duration of the pause for a ',' in ms
  int comma_ms = 500;
  /// transmit amplitude (0.0 - 1.0) of the sum of both tones
  float volume = 1.0f;
  /// receiver: minimum amplitude of each tone (1.0 = full scale)
  float min_amplitude = 0.01f;
  /// receiver: max level difference of the row and column tone in dB (a
  /// small speaker attenuates the row tones: use e.g. 24)
  float max_twist_db = 16.0f;
  /// receiver: the row and column tone must be this much stronger than the
  /// other tones of their group in dB
  float group_ratio_db = 6.0f;
  /// receiver: the end of a transmission is reported after this silence
  int rx_timeout_ms = 250;
  /// receiver: stop after the first transmission
  bool rx_one = false;
  /// receiver: input channel which is used for the decoding
  uint8_t channel = 0;
  /// receiver: (testing only) amplitude of added uniform noise
  float rx_noise = 0.0f;

  /// Selects the mode "dtmf": returns false if invalid. The other settings
  /// are kept.
  bool setMode(const char* mode) { return isMode(mode); }

  /// true if the mode name is "dtmf"
  static bool isMode(const char* mode) {
    if (mode == nullptr) return false;
    const char* name = "dtmf";
    for (int j = 0; j < 5; j++)
      if (tolower(mode[j]) != name[j]) return false;
    return true;
  }

  /// Checks the configuration: returns false (and logs the reason) if invalid
  bool isValid() const {
    if (tone_ms <= 0 || pause_ms <= 0 || comma_ms < 0 || rx_timeout_ms <= 0 ||
        max_twist_db <= 0 || group_ratio_db <= 0) {
      LOGE("DTMF: invalid durations");
      return false;
    }
    if (sample_rate < 4000) {
      LOGE("DTMF: the sample rate must be at least 4000 Hz");
      return false;
    }
    return true;
  }
};

/**
 * @brief The DTMF keypad: the tones and the characters of the 16 keys.
 * @copyright GPLv3
 */
class DTMFKeys {
 public:
  /// row frequencies in Hz
  static const float* rows() {
    static const float freqs[4] = {697.0f, 770.0f, 852.0f, 941.0f};
    return freqs;
  }

  /// column frequencies in Hz
  static const float* columns() {
    static const float freqs[4] = {1209.0f, 1336.0f, 1477.0f, 1633.0f};
    return freqs;
  }

  /// Character of the key in the indicated row and column
  static char key(int row, int column) { return keys()[row * 4 + column]; }

  /// Finds row and column of a character (also a - d): returns false if the
  /// character is not a DTMF key
  static bool find(char ch, int& row, int& column) {
    ch = toupper((unsigned char)ch);
    const char* pos = ch == 0 ? nullptr : strchr(keys(), ch);
    if (pos == nullptr) return false;
    int idx = pos - keys();
    row = idx / 4;
    column = idx % 4;
    return true;
  }

 protected:
  static const char* keys() { return "123A456B789C*0#D"; }
};

}  // namespace tinymodem
