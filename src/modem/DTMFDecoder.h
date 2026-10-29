#pragma once

#include <math.h>

#include "AudioTools.h"
#include "DTMF.h"
#include "ModemBase.h"

namespace tinymodem {

/**
 * @brief DTMF (touch tone) demodulator: the PCM audio data is written to the
 * decoder and the received keys (0-9, *, #, A-D) are written to the output.
 * The input can be 8, 16 or 32 bit signed samples with any number of
 * channels (only DTMFConfig::channel is evaluated).
 *
 * The 8 tones are measured with Goertzel filters over windows of 25.6 ms
 * (205 samples at 8000 Hz). Four windows overlap, so that a new result is
 * available every 6.4 ms. A key is detected when the strongest row and column
 * tone are clearly above the other tones of their group
 * (DTMFConfig::group_ratio_db), have a similar level (DTMFConfig::max_twist_db)
 * and contain most of the signal energy. A key is reported after 3
 * consecutive detections and released after 3 windows without it. The same
 * key is only reported again if it is at least half as strong as before:
 * this ignores the echo at the end of a tone.
 *
 * A transmission ends after DTMFConfig::rx_timeout_ms without a key: this is
 * reported with ### NOCARRIER.
 * @copyright GPLv3
 */
class DTMFDecoder : public ModemDecoder {
 public:
  DTMFDecoder() = default;
  DTMFDecoder(Print& out) { setOutput(out); }

  /// Provides a default configuration
  DTMFConfig defaultConfig() { return DTMFConfig(); }

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
  bool begin(DTMFConfig config) {
    cfg = config;
    return begin();
  }

  bool begin() override {
    is_active = false;
    if (!cfg.isValid()) return false;
    if (!input.begin(cfg.bits_per_sample, cfg.channels, cfg.channel))
      return false;
    window = lroundf(cfg.sample_rate * WINDOW_SEC);
    step = window / WINDOWS;
    for (int t = 0; t < TONES; t++) {
      float freq = t < 4 ? DTMFKeys::rows()[t] : DTMFKeys::columns()[t - 4];
      coeff[t] = 2.0f * cosf(2.0f * (float)M_PI * freq / cfg.sample_rate);
    }
    timeout = (uint32_t)cfg.rx_timeout_ms * cfg.sample_rate / 1000;
    twist = powf(10.0f, cfg.max_twist_db / 20.0f);         // amplitude ratio
    group_ratio = powf(10.0f, cfg.group_ratio_db / 10.0f);  // power ratio
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
    // feed silence, so that the end of the key is detected
    for (int j = 0; j < 2 * window && !is_finished; j++) addSample(0.0f, false);
    if (carrier) endOfCarrier();
    bool finished = is_finished;
    resetState();
    is_finished = finished;
  }

  /// Provide PCM data to the decoder
  size_t write(const uint8_t* data, size_t len) override {
    if (!is_active) return 0;
    input.write(data, len, [this](float sample) { addSample(sample, true); });
    return len;
  }

  /// true from the first key until the end of the transmission
  bool isCarrier() override { return carrier; }

  /// true if the decoding was stopped (rx_one)
  bool isFinished() override { return is_finished; }

  /// The key which is currently received or 0
  char currentKey() { return held < 0 ? 0 : DTMFKeys::key(held / 4, held % 4); }

  /// The configuration which was provided in begin()
  DTMFConfig& config() { return cfg; }

  operator bool() override { return is_active; }

 protected:
  static constexpr int TONES = 8;
  static constexpr int WINDOWS = 4;
  static constexpr float WINDOW_SEC = 205.0f / 8000.0f;
  static constexpr int HITS = 3;
  static constexpr int MISSES = 3;
  /// the same key with a lower level (relative to the last one) is an echo
  static constexpr float ECHO_LEVEL = 0.5f;
  /// part of the signal energy which must be in the two tones
  static constexpr float ENERGY_RATIO = 0.3f;

  /// Goertzel filters of one window
  struct Window {
    float s1[TONES], s2[TONES];
    float power;
    int count;
  };

  DTMFConfig cfg;
  bool is_active = false;
  bool is_finished = false;
  PcmInput input;
  uint32_t noise_seed = 1;
  int window = 205;
  int step = 51;
  float coeff[TONES];
  Window windows[WINDOWS];
  uint32_t timeout = 0;
  float twist = 6.3f;
  float group_ratio = 4.0f;

  // debouncing
  int candidate = -1;
  int hits = 0;
  int held = -1;
  int misses = 0;
  // the last reported key and its level: detects echoes
  int last_key = -1;
  float last_level = 0;
  float level = 0;  // tone level of the current window
  bool carrier = false;
  uint32_t idle_samples = 0;
  unsigned ndata = 0;

  void resetState() {
    for (int w = 0; w < WINDOWS; w++) {
      resetWindow(windows[w]);
      // staggered start: one window is completed every step samples
      windows[w].count = -w * step;
    }
    candidate = held = last_key = -1;
    hits = misses = 0;
    last_level = 0;
    carrier = false;
    idle_samples = 0;
    is_finished = false;
    input.reset();
  }

  void resetWindow(Window& w) {
    for (int t = 0; t < TONES; t++) w.s1[t] = w.s2[t] = 0;
    w.power = 0;
    w.count = 0;
  }

  /// uniform random value in the range -0.5 to 0.5
  float noise() {
    noise_seed = noise_seed * 1103515245u + 12345u;
    return ((noise_seed >> 8) & 0xFFFF) / 65535.0f - 0.5f;
  }

  void addSample(float x, bool add_noise) {
    if (is_finished) return;
    if (add_noise && cfg.rx_noise != 0.0f) x += noise() * cfg.rx_noise * 2.0f;
    for (int k = 0; k < WINDOWS; k++) {
      Window& w = windows[k];
      if (w.count++ < 0) continue;  // not started yet
      for (int t = 0; t < TONES; t++) {
        float s = x + coeff[t] * w.s1[t] - w.s2[t];
        w.s2[t] = w.s1[t];
        w.s1[t] = s;
      }
      w.power += x * x;
      if (w.count == window) {
        decide(detect(w));
        resetWindow(w);
      }
    }
    if (carrier && held < 0 && ++idle_samples >= timeout) endOfCarrier();
  }

  /// Evaluates a completed window: returns the key index (row * 4 + column)
  /// or -1
  int detect(Window& w) {
    float amp[TONES];
    for (int t = 0; t < TONES; t++) {
      float p = w.s1[t] * w.s1[t] + w.s2[t] * w.s2[t] -
                coeff[t] * w.s1[t] * w.s2[t];
      amp[t] = 2.0f * sqrtf(p > 0 ? p : 0) / window;
    }
    int row = strongest(amp);
    int column = strongest(amp + 4);
    float a_row = amp[row], a_col = amp[4 + column];
    level = a_row + a_col;
    if (a_row < cfg.min_amplitude || a_col < cfg.min_amplitude) return -1;
    if (a_row > twist * a_col || a_col > twist * a_row) return -1;
    for (int t = 0; t < 4; t++) {
      if (t != row && amp[t] * amp[t] * group_ratio > a_row * a_row) return -1;
      if (t != column && amp[4 + t] * amp[4 + t] * group_ratio > a_col * a_col)
        return -1;
    }
    // power of the tones (a^2 / 2) compared to the mean power
    float tone_power = (a_row * a_row + a_col * a_col) / 2.0f;
    if (tone_power < ENERGY_RATIO * w.power / window) return -1;
    return row * 4 + column;
  }

  static int strongest(const float* amp) {
    int result = 0;
    for (int t = 1; t < 4; t++)
      if (amp[t] > amp[result]) result = t;
    return result;
  }

  /// Debouncing of the detected keys
  void decide(int key) {
    if (key >= 0 && key == candidate) {
      hits++;
    } else {
      candidate = key;
      hits = 1;
    }
    if (held >= 0 && key != held) {
      if (++misses >= MISSES) held = -1;
    } else {
      misses = 0;
    }
    if (candidate >= 0 && hits >= HITS && candidate != held) {
      held = candidate;
      misses = 0;
      bool echo = held == last_key && level < ECHO_LEVEL * last_level;
      if (!echo) {
        if (!carrier) startOfCarrier();
        ndata++;
        if (p_print != nullptr) p_print->write((uint8_t)currentKey());
        last_key = held;
        last_level = 0;
      }
    }
    // the level of the reported key
    if (held >= 0 && held == last_key && key == held && level > last_level)
      last_level = level;
    if (held >= 0) idle_samples = 0;
  }

  void startOfCarrier() {
    carrier = true;
    ndata = 0;
    idle_samples = 0;
    status("### CARRIER dtmf ###\n");
  }

  void endOfCarrier() {
    carrier = false;
    status("\n### NOCARRIER ndata=%u ###\n", ndata);
    if (cfg.rx_one) is_finished = true;
  }
};

}  // namespace tinymodem
