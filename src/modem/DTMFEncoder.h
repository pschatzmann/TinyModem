#pragma once

#include <math.h>

#include "AudioTools.h"
#include "DTMF.h"
#include "ModemBase.h"

namespace tinymodem {

/**
 * @brief DTMF (touch tone) modulator: each written character 0-9, *, #, A-D
 * (also a-d) is sent as a tone pair of DTMFConfig::tone_ms followed by a
 * pause of DTMFConfig::pause_ms. A ',' adds a pause of DTMFConfig::comma_ms,
 * all other characters are ignored. The tones fade in and out within 2 ms to
 * avoid clicks.
 * @copyright GPLv3
 */
class DTMFEncoder : public ModemEncoder {
 public:
  DTMFEncoder() = default;
  DTMFEncoder(Print& out) { setOutput(out); }

  /// Provides a default configuration
  DTMFConfig defaultConfig() { return DTMFConfig(); }

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
  bool begin(DTMFConfig config) {
    cfg = config;
    return begin();
  }

  bool begin() override {
    is_active = false;
    if (!cfg.isValid()) return false;
    if (cfg.bits_per_sample != 16) {
      LOGE("DTMFEncoder: bits_per_sample must be 16, not %d",
           (int)cfg.bits_per_sample);
      return false;
    }
    if (cfg.channels < 1) {
      LOGE("DTMFEncoder: invalid number of channels: %d", (int)cfg.channels);
      return false;
    }
    samples.begin(cfg.channels, cfg.volume);
    if (!tones.begin(cfg, cfg.volume)) return false;
    transmitting = false;
    is_active = true;
    return true;
  }

  void end() override {
    endTransmission();
    is_active = false;
  }

  /// Sends the keys
  size_t write(const uint8_t* data, size_t len) override {
    if (!is_active) return 0;
    for (size_t j = 0; j < len; j++) {
      int row, column;
      if (DTMFKeys::find(data[j], row, column)) {
        transmitting = true;
        sendKey(row, column);
      } else if (data[j] == ',') {
        transmitting = true;
        silence(cfg.comma_ms);
      }
    }
    samples.flush();
    return len;
  }

  /// Sends a single key (e.g. '5' or '#'): returns false if the character
  /// is not a DTMF key
  bool writeKey(char key) {
    int row, column;
    if (!is_active || !DTMFKeys::find(key, row, column)) return false;
    transmitting = true;
    sendKey(row, column);
    samples.flush();
    return true;
  }

  /// Ends the transmission (and adds optional silence): returns true if a
  /// transmission was ended
  bool endTransmission(size_t silence_samples = 0) override {
    if (!is_active || !transmitting) return false;
    for (size_t j = 0; j < silence_samples; j++) samples.write(0.0f);
    samples.flush();
    transmitting = false;
    return true;
  }

  /// Sends silence for the indicated number of samples
  bool writeIdle(size_t nsamples) override {
    if (!is_active) return false;
    for (size_t j = 0; j < nsamples; j++) samples.write(0.0f);
    samples.flush();
    return true;
  }

  /// true between the first key and endTransmission()
  bool isTransmitting() override { return transmitting; }

  /// Keys per second
  float symbolRate() override {
    return 1000.0f / (cfg.tone_ms + cfg.pause_ms);
  }

  /// The configuration which was provided in begin()
  DTMFConfig& config() { return cfg; }

  operator bool() override { return is_active; }

 protected:
  static constexpr float RAMP_MS = 2.0f;
  DTMFConfig cfg;
  SampleOutput samples;
  bool is_active = false;
  bool transmitting = false;

  void sendKey(int row, int column) {
    size_t n = (size_t)cfg.tone_ms * cfg.sample_rate / 1000;
    size_t ramp = RAMP_MS * cfg.sample_rate / 1000;
    if (2 * ramp > n) ramp = n / 2;
    float w1 = 2.0f * (float)M_PI * DTMFKeys::rows()[row] / cfg.sample_rate;
    float w2 = 2.0f * (float)M_PI * DTMFKeys::columns()[column] / cfg.sample_rate;
    for (size_t j = 0; j < n; j++) {
      float gain = 0.5f;
      if (j < ramp) gain *= 0.5f - 0.5f * cosf((float)M_PI * j / ramp);
      if (n - j <= ramp)
        gain *= 0.5f - 0.5f * cosf((float)M_PI * (n - j) / ramp);
      samples.write(gain * (sinf(w1 * j) + sinf(w2 * j)));
    }
    silence(cfg.pause_ms);
  }

  void silence(int ms) {
    size_t n = (size_t)ms * cfg.sample_rate / 1000;
    for (size_t j = 0; j < n; j++) samples.write(0.0f);
  }
};

}  // namespace tinymodem
