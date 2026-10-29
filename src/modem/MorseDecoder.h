#pragma once

#include <math.h>
#include <string.h>

#include "AudioTools.h"
#include "ModemBase.h"
#include "Morse.h"

namespace tinymodem {

/**
 * @brief Morse code demodulator (CW): the PCM audio data is written to the
 * decoder and the decoded characters (upper case) are written to the output.
 * The input can be 8, 16 or 32 bit signed samples with any number of
 * channels (only MorseConfig::channel is evaluated).
 *
 * The level of the tone is measured with a mixer and a sliding sum over half
 * a dot, every 2 ms. The tone is on when the level is above the middle
 * between the signal and the noise level (with hysteresis), and short
 * changes are ignored. The length of the marks and spaces in units provides
 * the dots and dashes and the spaces between the elements, characters and
 * words. The length of a unit adapts to the speed of the sender. The rise
 * and fall time of the tone shortens the marks and lengthens the spaces by
 * the same amount: this is measured with the dots and the following element
 * spaces (both are 1 unit) and corrected.
 *
 * A transmission ends after MorseConfig::rx_timeout_units without a tone:
 * this is reported with ### NOCARRIER.
 * @copyright GPLv3
 */
class MorseDecoder : public ModemDecoder {
 public:
  MorseDecoder() = default;
  MorseDecoder(Print& out) { setOutput(out); }

  /// Provides a default configuration: International Morse code, 20 wpm
  MorseConfig defaultConfig() { return MorseConfig(); }

  /// Defines the format of the input audio data
  void setAudioInfo(audio_tools::AudioInfo info) override {
    if (info.sample_rate == cfg.sample_rate && info.channels == cfg.channels &&
        info.bits_per_sample == cfg.bits_per_sample)
      return;
    cfg.sample_rate = info.sample_rate;
    cfg.channels = info.channels;
    cfg.bits_per_sample = info.bits_per_sample;
    if (is_active) begin();
  }

  audio_tools::AudioInfo audioInfo() override { return cfg; }

  using audio_tools::AudioDecoder::begin;

  /// Starts the processing with the indicated configuration
  bool begin(MorseConfig config) {
    cfg = config;
    return begin();
  }

  bool begin() override {
    is_active = false;
    if (!cfg.isValid()) return false;
    if (!input.begin(cfg.bits_per_sample, cfg.channels, cfg.channel))
      return false;
    block = lroundf(cfg.sample_rate * BLOCK_SEC);
    if (block < 1) block = 1;
    // window of half a dot (at least one block)
    window = lroundf(cfg.unitTime() * cfg.sample_rate * WINDOW_UNITS);
    if (window < block) window = block;
    history.resize(2 * window);
    double w = 2.0 * M_PI * cfg.frequency / cfg.sample_rate;
    step_re = cos(w);
    step_im = sin(w);
    resetState();
    is_active = true;
    return true;
  }

  /// Processes the remaining samples and reports the end of the carrier
  void end() override {
    if (!is_active) return;
    flush();
    is_active = false;
  }

  /// Processes the remaining samples (e.g. at the end of the input)
  void flush() override {
    if (!is_active) return;
    // feed silence, so that the last character is completed
    size_t n = (size_t)(cfg.rx_timeout_units + 2) * cfg.unitTime() *
               cfg.sample_rate;
    for (size_t j = 0; j < n && !is_finished && carrier; j++)
      addSample(0.0f, false);
    if (carrier) endOfCarrier();
    bool finished = is_finished;
    resetState();
    is_finished = finished;
  }

  /// Provide PCM data to the decoder
  size_t write(const uint8_t* data, size_t len) override {
    if (!is_active) return 0;
    input.write(data, len, [this](float x) { addSample(x, true); });
    return len;
  }

  /// true while a transmission is received
  bool isCarrier() override { return carrier; }

  /// true if the decoding was stopped (rx_one)
  bool isFinished() override { return is_finished; }

  /// Current speed of the sender in words per minute
  float wpm() { return unit > 0 ? 1.2f / unit : cfg.wpm; }

  /// The configuration which was provided in begin()
  MorseConfig& config() { return cfg; }

  operator bool() override { return is_active; }

 protected:
  static constexpr float BLOCK_SEC = 0.002f;
  /// length of the measurement window in units
  static constexpr float WINDOW_UNITS = 0.5f;
  /// changes which are shorter than this (in units) are ignored
  static constexpr float DEBOUNCE_UNITS = 0.3f;
  static constexpr int MAX_CODE = 16;
  /// min ratio of the tone level to the average noise level
  static constexpr float MIN_SNR = 5.0f;

  MorseConfig cfg;
  bool is_active = false;
  bool is_finished = false;
  PcmInput input;
  uint32_t noise_seed = 1;
  // level measurement
  int block = 96, window = 480, count = 0;
  audio_tools::Vector<float> history;  // i, q
  int pos = 0;
  float sum_i = 0, sum_q = 0;
  float re = 1, im = 0, step_re = 1, step_im = 0;
  uint32_t sample_count = 0;
  float signal_level = 0, noise_level = 0;
  float noise_mean = 0;  // average level without tone
  bool first_block = true;
  // on/off detection
  bool tone_on = false;     // debounced state
  int pending = 0;          // blocks with a different raw state
  float duration = 0;       // seconds in the current state
  float unit = 0.06f;       // seconds per unit
  float bias = 0;           // marks are measured shorter by this (seconds)
  float last_mark = 0;      // length of the last mark
  char last_element = 0;
  bool carrier = false;
  bool word_pending = false;
  char code[MAX_CODE + 1];
  int code_len = 0;
  unsigned ndata = 0;

  void resetState() {
    for (size_t j = 0; j < history.size(); j++) history[j] = 0;
    pos = count = 0;
    sum_i = sum_q = 0;
    re = 1;
    im = 0;
    signal_level = noise_level = noise_mean = 0;
    first_block = true;
    tone_on = false;
    pending = 0;
    duration = 0;
    unit = cfg.unitTime();
    bias = 0;
    last_element = 0;
    carrier = false;
    word_pending = false;
    code_len = 0;
    is_finished = false;
    input.reset();
  }

  /// uniform random value in the range -0.5 to 0.5
  float noise() {
    noise_seed = noise_seed * 1103515245u + 12345u;
    return ((noise_seed >> 8) & 0xFFFF) / 65535.0f - 0.5f;
  }

  void addSample(float x, bool add_noise) {
    if (is_finished) return;
    if (add_noise && cfg.rx_noise != 0.0f) x += noise() * cfg.rx_noise * 2.0f;
    // mix to the baseband and sum over the window
    float vi = x * re, vq = -x * im;
    float* old = history.data() + pos * 2;
    sum_i += vi - old[0];
    sum_q += vq - old[1];
    old[0] = vi;
    old[1] = vq;
    if (++pos == window) pos = 0;
    float r = re * step_re - im * step_im;
    im = re * step_im + im * step_re;
    re = r;
    if (++sample_count % 1024 == 0) recalculate();
    if (++count == block) {
      count = 0;
      processBlock(2.0f * sqrtf(sum_i * sum_i + sum_q * sum_q) / window);
    }
  }

  /// Avoids the accumulation of rounding errors
  void recalculate() {
    sum_i = sum_q = 0;
    for (int k = 0; k < window; k++) {
      sum_i += history[k * 2];
      sum_q += history[k * 2 + 1];
    }
    float mag = sqrtf(re * re + im * im);
    re /= mag;
    im /= mag;
  }

  /// On/off decision of one block with the level of the tone
  void processBlock(float level) {
    float block_sec = (float)block / cfg.sample_rate;
    // signal level: fast attack, slow decay; noise level: while off
    if (level > signal_level)
      signal_level += 0.5f * (level - signal_level);
    else
      signal_level += 0.002f * (level - signal_level);
    float threshold = noise_level + 0.5f * (signal_level - noise_level);
    float hysteresis = 0.1f * (signal_level - noise_level);
    // a tone must be clearly above the noise (noise alone has peaks)
    bool raw = tone_on ? level > threshold - hysteresis
                       : level > threshold + hysteresis &&
                             level >= cfg.min_amplitude &&
                             level > MIN_SNR * noise_mean;
    // the noise level follows the levels without tone (also not while a new
    // tone is confirmed): it falls fast and rises slowly, so that the end of
    // a tone hardly raises it
    if (first_block) {
      noise_mean = level;
      first_block = false;
    }
    if (!tone_on && !raw) {
      noise_level += (level < noise_level ? 0.1f : 0.01f) * (level - noise_level);
      // the average excludes the end and the start of a tone: the window
      // must have left the last tone and the level must be close to the
      // noise floor
      if (duration >= WINDOW_UNITS * unit && level < 2.5f * noise_level)
        noise_mean += 0.02f * (level - noise_mean);
    }

    duration += block_sec;
    if (raw != tone_on) {
      // the change must last for a while
      if (++pending * block_sec >= DEBOUNCE_UNITS * unit) {
        float changed = pending * block_sec;
        float length = duration - changed;
        tone_on = raw;
        pending = 0;
        duration = changed;
        if (tone_on)
          endOfSpace(length);
        else
          endOfMark(length);
      }
    } else {
      pending = 0;
    }
    // the end of a character, a word and the transmission is detected
    // during the space
    if (!tone_on && carrier) checkSpace(duration);
  }

  void endOfMark(float raw_length) {
    const MorseCode::Timing& t = MorseCode::timing(cfg.alphabet);
    last_mark = raw_length;
    float length = raw_length + bias;
    float units = length / unit;
    char element;
    float expected;
    if (units < (t.dot + t.dash) / 2) {
      element = '.';
      expected = t.dot;
    } else if (cfg.alphabet == MorseAlphabet::INTERNATIONAL ||
               units < (t.dash + t.long_dash) / 2) {
      element = '-';
      expected = t.dash;
    } else if (units < (t.long_dash + t.extra_long_dash) / 2) {
      element = '_';
      expected = t.long_dash;
    } else {
      element = '=';
      expected = t.extra_long_dash;
    }
    // follow the speed of the sender with the dots and dashes
    if (cfg.adaptive_speed && (element == '.' || element == '-'))
      unit += 0.2f * (length / expected - unit);
    if (code_len < MAX_CODE) code[code_len++] = element;
    last_element = element;
  }

  void endOfSpace(float raw_length) {
    if (!carrier) {
      startOfCarrier();
      return;
    }
    const MorseCode::Timing& t = MorseCode::timing(cfg.alphabet);
    float length = raw_length - bias;
    float units = length / unit;
    // a dot and an element space have the same length: the difference is
    // twice the bias
    if (last_element == '.' && code_len > 0 &&
        units < (t.element_gap + t.inner_gap + 1) / 2) {
      bias += 0.2f * ((raw_length - last_mark) / 2 - bias);
      float max_bias = 0.3f * unit;
      if (bias > max_bias) bias = max_bias;
      if (bias < -max_bias) bias = -max_bias;
    }
    // a space within an American character
    if (cfg.alphabet == MorseAlphabet::AMERICAN && code_len > 0 &&
        units >= (t.element_gap + t.inner_gap) / 2 &&
        units < (t.inner_gap + t.letter_gap) / 2 && code_len < MAX_CODE)
      code[code_len++] = ' ';
    if (word_pending) {
      output(' ');
      word_pending = false;
    }
  }

  /// Ends the character, the word or the transmission during a space
  void checkSpace(float raw_length) {
    const MorseCode::Timing& t = MorseCode::timing(cfg.alphabet);
    float units = (raw_length - bias) / unit;
    float letter = cfg.alphabet == MorseAlphabet::AMERICAN
                       ? (t.inner_gap + t.letter_gap) / 2
                       : (t.element_gap + t.letter_gap) / 2;
    if (code_len > 0 && units >= letter) finishCharacter();
    if (units >= (t.letter_gap + t.word_gap) / 2 && ndata > 0)
      word_pending = true;
    if (units >= cfg.rx_timeout_units) endOfCarrier();
  }

  void finishCharacter() {
    code[code_len] = 0;
    char ch = MorseCode::decode(cfg.alphabet, code);
    code_len = 0;
    output(ch == 0 ? '?' : ch);  // '?' for an unknown code
  }

  void output(char ch) {
    ndata++;
    if (p_print != nullptr) p_print->write((uint8_t)ch);
  }

  void startOfCarrier() {
    carrier = true;
    ndata = 0;
    code_len = 0;
    word_pending = false;
    status("### CARRIER morse %s %.1f wpm @ %.1f Hz ###\n",
           cfg.alphabet == MorseAlphabet::AMERICAN ? "american"
                                                   : "international",
           (double)cfg.wpm, (double)cfg.frequency);
  }

  void endOfCarrier() {
    if (code_len > 0) finishCharacter();
    carrier = false;
    word_pending = false;
    status("\n### NOCARRIER ndata=%u wpm=%.1f ###\n", ndata, (double)wpm());
    if (cfg.rx_one) is_finished = true;
  }
};

}  // namespace tinymodem
