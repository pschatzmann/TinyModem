#pragma once

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "AudioTools.h"

namespace tinymodem {

/// Morse code alphabet
enum class MorseAlphabet {
  /// International Morse code (ITU-R M.1677): dots and dashes of 3 units
  INTERNATIONAL,
  /// American Morse code (railroad telegraph): dashes of 2 units, long
  /// dashes and spaces within some characters
  AMERICAN,
};

/**
 * @brief Configuration of the Morse code modem (MorseEncoder, MorseDecoder):
 * a tone which is switched on and off (CW). The speed is defined in words per
 * minute (PARIS): a unit (the length of a dot) is 1.2 / wpm seconds.
 *
 * setMode() selects "morse" or "cw" (International Morse code, 20 wpm),
 * "morse-american" (American Morse code), optionally with the speed, e.g.
 * "morse-25", "morse-american-15" or "cw25".
 * @copyright GPLv3
 */
struct MorseConfig : public audio_tools::AudioInfo {
  MorseConfig() {
    sample_rate = 48000;
    channels = 1;
    bits_per_sample = 16;
  }

  /// International (default) or American Morse code
  MorseAlphabet alphabet = MorseAlphabet::INTERNATIONAL;
  /// speed in words per minute
  float wpm = 20.0f;
  /// frequency of the tone in Hz
  float frequency = 700.0f;
  /// transmit amplitude (0.0 - 1.0)
  float volume = 1.0f;
  /// transmitter: rise and fall time of the tone in ms (avoids clicks)
  float ramp_ms = 5.0f;
  /// receiver: adapt to the speed of the sender
  bool adaptive_speed = true;
  /// receiver: minimum amplitude of the tone (1.0 = full scale)
  float min_amplitude = 0.01f;
  /// receiver: the end of a transmission is reported after this silence in
  /// units
  int rx_timeout_units = 20;
  /// receiver: stop after the first transmission
  bool rx_one = false;
  /// receiver: input channel which is used for the decoding
  uint8_t channel = 0;
  /// receiver: (testing only) amplitude of added uniform noise
  float rx_noise = 0.0f;

  /// Selects e.g. "morse", "morse-25", "morse-american" or "cw20": returns
  /// false if invalid. The other settings are kept.
  bool setMode(const char* mode) {
    MorseAlphabet a;
    float speed;
    if (!parseMode(mode, a, speed)) return false;
    alphabet = a;
    if (speed > 0) wpm = speed;
    return true;
  }

  /// true if the mode is a Morse code mode
  static bool isMode(const char* mode) {
    MorseAlphabet a;
    float speed;
    return parseMode(mode, a, speed);
  }

  /// length of a unit (dot) in seconds
  float unitTime() const { return 1.2f / wpm; }

  /// Checks the configuration: returns false (and logs the reason) if invalid
  bool isValid() const {
    if (wpm < 1 || wpm > 100) {
      LOGE("Morse: the speed must be 1 - 100 wpm");
      return false;
    }
    if (frequency <= 0 || frequency >= 0.45f * sample_rate) {
      LOGE("Morse: invalid frequency %.1f Hz", (double)frequency);
      return false;
    }
    if (ramp_ms < 0 || rx_timeout_units < 8) {
      LOGE("Morse: invalid ramp_ms or rx_timeout_units");
      return false;
    }
    return true;
  }

 protected:
  static bool parseMode(const char* mode, MorseAlphabet& alphabet,
                        float& speed) {
    if (mode == nullptr) return false;
    alphabet = MorseAlphabet::INTERNATIONAL;
    speed = 0;
    const char* rest;
    if (startsWith(mode, "morse")) {
      rest = mode + 5;
    } else if (startsWith(mode, "cw")) {
      rest = mode + 2;
    } else {
      return false;
    }
    if (*rest == '-') rest++;
    if (startsWith(rest, "american")) {
      alphabet = MorseAlphabet::AMERICAN;
      rest += 8;
      if (*rest == '-') rest++;
    } else if (startsWith(rest, "international")) {
      rest += 13;
      if (*rest == '-') rest++;
    }
    if (*rest == 0) return true;
    char* end;
    speed = strtol(rest, &end, 10);
    return end != rest && *end == 0 && speed >= 1 && speed <= 100;
  }

  static bool startsWith(const char* str, const char* prefix) {
    while (*prefix) {
      if (tolower(*str++) != *prefix++) return false;
    }
    return true;
  }
};

/**
 * @brief The Morse code tables and timing. A code is a string of '.' (dot),
 * '-' (dash), '_' (long dash of the American L), '=' (longer dash of the
 * American 0) and ' ' (space within a character of American Morse).
 * @copyright GPLv3
 */
class MorseCode {
 public:
  /// Lengths in units
  struct Timing {
    float dot, dash, long_dash, extra_long_dash;
    float element_gap, inner_gap, letter_gap, word_gap;
  };

  /// Timing of the alphabet: American Morse has shorter dashes, long dashes
  /// and spaces within characters
  static const Timing& timing(MorseAlphabet alphabet) {
    static const Timing international = {1, 3, 3, 3, 1, 1, 3, 7};
    static const Timing american = {1, 2, 4, 5, 1, 2, 3, 6};
    return alphabet == MorseAlphabet::AMERICAN ? american : international;
  }

  /// Code of a character (upper or lower case): returns nullptr if the
  /// character is not in the alphabet
  static const char* encode(MorseAlphabet alphabet, char ch) {
    ch = toupper((unsigned char)ch);
    for (const Entry* e = table(alphabet); e->ch; e++)
      if (e->ch == ch) return e->code;
    return nullptr;
  }

  /// Character of a code: returns 0 if the code is unknown
  static char decode(MorseAlphabet alphabet, const char* code) {
    for (const Entry* e = table(alphabet); e->ch; e++)
      if (strcmp(e->code, code) == 0) return e->ch;
    return 0;
  }

 protected:
  struct Entry {
    char ch;
    const char* code;
  };

  static const Entry* table(MorseAlphabet alphabet) {
    // ITU-R M.1677 (and the common '!'); a line break is sent as AA
    static const Entry international[] = {
        {'A', ".-"},      {'B', "-..."},    {'C', "-.-."},    {'D', "-.."},
        {'E', "."},       {'F', "..-."},    {'G', "--."},     {'H', "...."},
        {'I', ".."},      {'J', ".---"},    {'K', "-.-"},     {'L', ".-.."},
        {'M', "--"},      {'N', "-."},      {'O', "---"},     {'P', ".--."},
        {'Q', "--.-"},    {'R', ".-."},     {'S', "..."},     {'T', "-"},
        {'U', "..-"},     {'V', "...-"},    {'W', ".--"},     {'X', "-..-"},
        {'Y', "-.--"},    {'Z', "--.."},    {'0', "-----"},   {'1', ".----"},
        {'2', "..---"},   {'3', "...--"},   {'4', "....-"},   {'5', "....."},
        {'6', "-...."},   {'7', "--..."},   {'8', "---.."},   {'9', "----."},
        {'.', ".-.-.-"},  {',', "--..--"},  {'?', "..--.."},  {'\'', ".----."},
        {'!', "-.-.--"},  {'/', "-..-."},   {'(', "-.--."},   {')', "-.--.-"},
        {'&', ".-..."},   {':', "---..."},  {';', "-.-.-."},  {'=', "-...-"},
        {'+', ".-.-."},   {'-', "-....-"},  {'_', "..--.-"},  {'"', ".-..-."},
        {'$', "...-..-"}, {'@', ".--.-."},  {'\n', ".-.-"},   {0, nullptr}};
    // American (railroad) Morse code
    static const Entry american[] = {
        {'A', ".-"},     {'B', "-..."},   {'C', ".. ."},   {'D', "-.."},
        {'E', "."},      {'F', ".-."},    {'G', "--."},    {'H', "...."},
        {'I', ".."},     {'J', "-.-."},   {'K', "-.-"},    {'L', "_"},
        {'M', "--"},     {'N', "-."},     {'O', ". ."},    {'P', "....."},
        {'Q', "..-."},   {'R', ". .."},   {'S', "..."},    {'T', "-"},
        {'U', "..-"},    {'V', "...-"},   {'W', ".--"},    {'X', ".-.."},
        {'Y', ".. .."},  {'Z', "... ."},  {'0', "="},      {'1', ".--."},
        {'2', "..-.."},  {'3', "...-."},  {'4', "....-"},  {'5', "---"},
        {'6', "......"}, {'7', "--.."},   {'8', "-...."},  {'9', "-..-"},
        {'.', "..--.."}, {',', ".-.-"},   {'?', "-..-."},  {'!', "---."},
        {'&', ". ..."},  {0, nullptr}};
    return alphabet == MorseAlphabet::AMERICAN ? american : international;
  }
};

}  // namespace tinymodem
