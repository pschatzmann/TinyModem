#pragma once

#include <math.h>

#include "AudioTools.h"
#include "ModemBase.h"
#include "Morse.h"

namespace tinymodem {

/**
 * @brief Morse code modulator (CW): each written character is sent as dots
 * and dashes of a tone with the timing of the alphabet (International: dot
 * 1, dash 3, element space 1, letter space 3, word space 7 units). A space
 * (or a character which is not in the alphabet) is a word space. The tone is
 * switched on and off with a raised cosine ramp of MorseConfig::ramp_ms.
 * @copyright GPLv3
 */
class MorseEncoder : public ModemEncoder {
 public:
  MorseEncoder() = default;
  MorseEncoder(Print& out) { setOutput(out); }

  /// Provides a default configuration: International Morse code, 20 wpm
  MorseConfig defaultConfig() { return MorseConfig(); }

  void setOutput(Print& out) override {
    samples.setOutput(out);
    ModemEncoder::setOutput(out);  // writeSilence(), writeTone()
  }

  /// Defines the sample rate and number of channels of the generated audio
  void setAudioInfo(audio_tools::AudioInfo info) override {
    cfg.sample_rate = info.sample_rate;
    cfg.channels = info.channels;
    cfg.bits_per_sample = info.bits_per_sample;
    if (is_active) begin();
  }

  audio_tools::AudioInfo audioInfo() override { return cfg; }

  using audio_tools::AudioEncoder::begin;

  /// Starts the processing with the indicated configuration
  bool begin(MorseConfig config) {
    cfg = config;
    return begin();
  }

  bool begin() override {
    is_active = false;
    if (!cfg.isValid()) return false;
    if (cfg.bits_per_sample != 16) {
      LOGE("MorseEncoder: bits_per_sample must be 16, not %d",
           (int)cfg.bits_per_sample);
      return false;
    }
    if (cfg.channels < 1) {
      LOGE("MorseEncoder: invalid number of channels: %d", (int)cfg.channels);
      return false;
    }
    samples.begin(cfg.channels, cfg.volume);
    if (!tones.begin(cfg, cfg.volume)) return false;
    unit_samples = cfg.unitTime() * cfg.sample_rate;
    double w = 2.0 * M_PI * cfg.frequency / cfg.sample_rate;
    step_re = cos(w);
    step_im = sin(w);
    transmitting = false;
    word_pending = false;
    is_active = true;
    return true;
  }

  void end() override {
    endTransmission();
    is_active = false;
  }

  /// Sends the characters
  size_t write(const uint8_t* data, size_t len) override {
    if (!is_active) return 0;
    for (size_t j = 0; j < len; j++) {
      const char* code = MorseCode::encode(cfg.alphabet, data[j]);
      if (code == nullptr) {
        // word space before the next character
        if (transmitting) word_pending = true;
        continue;
      }
      sendCode(code);
    }
    samples.flush();
    return len;
  }

  /// Ends the transmission (and adds optional silence): returns true if a
  /// transmission was ended
  bool endTransmission(size_t silence_samples = 0) override {
    if (!is_active || !transmitting) return false;
    for (size_t j = 0; j < silence_samples; j++) samples.write(0.0f);
    samples.flush();
    transmitting = false;
    word_pending = false;
    return true;
  }

  /// Sends silence for the indicated number of samples
  bool writeIdle(size_t nsamples) override {
    if (!is_active) return false;
    for (size_t j = 0; j < nsamples; j++) samples.write(0.0f);
    samples.flush();
    return true;
  }

  /// true between the first character and endTransmission()
  bool isTransmitting() override { return transmitting; }

  /// Units per second
  float symbolRate() override { return 1.0f / cfg.unitTime(); }

  /// The configuration which was provided in begin()
  MorseConfig& config() { return cfg; }

  operator bool() override { return is_active; }

 protected:
  MorseConfig cfg;
  SampleOutput samples;
  bool is_active = false;
  bool transmitting = false;
  bool word_pending = false;
  double unit_samples = 0;
  double position = 0;  // exact time in samples (avoids rounding errors)
  uint64_t written = 0;
  float re = 1, im = 0, step_re = 1, step_im = 0;
  uint32_t sample_count = 0;

  void sendCode(const char* code) {
    const MorseCode::Timing& t = MorseCode::timing(cfg.alphabet);
    if (!transmitting) {
      transmitting = true;
      position = written;
    } else {
      // the letter space was sent after the previous character
      if (word_pending) silence(t.word_gap - t.letter_gap);
    }
    word_pending = false;
    for (const char* c = code; *c; c++) {
      if (*c == ' ') {
        // space within an American character instead of the element space
        silence(t.inner_gap - t.element_gap);
        continue;
      }
      float len = *c == '.'   ? t.dot
                  : *c == '-' ? t.dash
                  : *c == '_' ? t.long_dash
                              : t.extra_long_dash;
      tone(len);
      silence(c[1] ? t.element_gap : t.letter_gap);
    }
  }

  void silence(float units) {
    position += units * unit_samples;
    for (; written < (uint64_t)llround(position); written++) samples.write(0.0f);
  }

  /// Tone with raised cosine ramps
  void tone(float units) {
    position += units * unit_samples;
    uint64_t start = written, stop = llround(position);
    size_t n = stop - start;
    size_t ramp = cfg.ramp_ms * cfg.sample_rate / 1000;
    if (2 * ramp > n) ramp = n / 2;
    re = 1;
    im = 0;
    for (size_t j = 0; j < n; j++) {
      float gain = 1.0f;
      if (j < ramp) gain = 0.5f - 0.5f * cosf((float)M_PI * j / ramp);
      if (n - j <= ramp) gain = 0.5f - 0.5f * cosf((float)M_PI * (n - j) / ramp);
      samples.write(gain * im);
      float r = re * step_re - im * step_im;
      im = re * step_im + im * step_re;
      re = r;
      if (++sample_count % 1024 == 0) {
        float mag = sqrtf(re * re + im * im);
        re /= mag;
        im /= mag;
      }
    }
    written = stop;
  }
};

}  // namespace tinymodem
