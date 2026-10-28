#pragma once

#include <math.h>

#include "AudioTools.h"
#include "ModemBase.h"
#include "PSK.h"

namespace tinymodem {

/**
 * @brief PSK modulator (BPSK like PSK31): the written bytes are Varicode
 * encoded (characters separated by "00"). A 0 bit is a phase reversal with a
 * cosine shaped amplitude, a 1 bit keeps the phase.
 *
 * A transmission starts with phase reversals (preamble). Call
 * endTransmission() (or flush()) when the data is complete: a steady carrier
 * (postamble) is sent and the signal fades out.
 * @copyright GPLv3
 */
class PSKEncoder : public ModemEncoder {
 public:
  PSKEncoder() = default;
  PSKEncoder(Print& out) { setOutput(out); }

  /// Provides a default configuration: psk31
  PSKConfig defaultConfig() {
    PSKConfig result;
    result.setMode("psk31");
    return result;
  }

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
  bool begin(PSKConfig config) {
    cfg = config;
    return begin();
  }

  bool begin() override {
    is_active = false;
    if (!cfg.isValid()) return false;
    if (cfg.bits_per_sample != 16) {
      LOGE("PSKEncoder: bits_per_sample must be 16, not %d",
           (int)cfg.bits_per_sample);
      return false;
    }
    if (cfg.channels < 1) {
      LOGE("PSKEncoder: invalid number of channels: %d", (int)cfg.channels);
      return false;
    }
    samples.begin(cfg.channels, cfg.volume);
    if (!tones.begin(cfg, cfg.volume)) return false;
    samples_per_symbol = cfg.samplesPerSymbol();
    double w = 2.0 * M_PI * cfg.carrier_freq / cfg.sample_rate;
    step_re = cos(w);
    step_im = sin(w);
    transmitting = false;
    is_active = true;
    return true;
  }

  void end() override {
    endTransmission();
    is_active = false;
  }

  /// Modulates the bytes
  size_t write(const uint8_t* data, size_t len) override {
    if (!is_active) return 0;
    for (size_t j = 0; j < len; j++) {
      if (!transmitting) startTransmission();
      Varicode::Code code = Varicode::encode(data[j]);
      for (int b = code.length - 1; b >= 0; b--) sendBit((code.bits >> b) & 1);
      sendBit(0);
      sendBit(0);
    }
    samples.flush();
    return len;
  }

  /// Sends the postamble and fades out (and adds optional silence): returns
  /// true if a transmission was ended.
  bool endTransmission(size_t silence_samples = 0) override {
    if (!is_active || !transmitting) return false;
    for (int j = 0; j < cfg.postamble_symbols; j++) sendBit(1);
    sendSymbol(Shape::FADE_OUT);
    for (size_t j = 0; j < silence_samples; j++) samples.write(0.0f);
    samples.flush();
    transmitting = false;
    return true;
  }

  /// Sends phase reversals (the PSK31 idle signal) for the indicated number
  /// of samples
  bool writeIdle(size_t nsamples) override {
    if (!is_active) return false;
    if (!transmitting) startTransmission();
    size_t n = nsamples / samples_per_symbol + 1;
    for (size_t j = 0; j < n; j++) sendBit(0);
    samples.flush();
    return true;
  }

  /// true between the first byte and endTransmission()
  bool isTransmitting() override { return transmitting; }

  /// Symbols per second
  float symbolRate() override { return cfg.symbol_rate; }

  /// The configuration which was provided in begin()
  PSKConfig& config() { return cfg; }

  operator bool() override { return is_active; }

 protected:
  enum class Shape { STEADY, REVERSAL, FADE_IN, FADE_OUT };
  PSKConfig cfg;
  SampleOutput samples;
  bool is_active = false;
  bool transmitting = false;
  double samples_per_symbol = 0;
  uint64_t symbol_index = 0;
  float phase_sign = 1.0f;
  // carrier oscillator (rotating phasor)
  float re = 1.0f, im = 0.0f;
  float step_re = 1.0f, step_im = 0.0f;
  uint32_t sample_count = 0;

  void startTransmission() {
    transmitting = true;
    symbol_index = 0;
    phase_sign = 1.0f;
    sendSymbol(Shape::FADE_IN);
    for (int j = 0; j < cfg.preamble_symbols; j++) sendBit(0);
  }

  /// 0: phase reversal, 1: same phase
  void sendBit(int bit) { sendSymbol(bit ? Shape::STEADY : Shape::REVERSAL); }

  void sendSymbol(Shape shape) {
    size_t start = llround(symbol_index * samples_per_symbol);
    size_t stop = llround((symbol_index + 1) * samples_per_symbol);
    symbol_index++;
    size_t n = stop - start;
    for (size_t j = 0; j < n; j++) {
      float t = (float)j / n;  // 0 to 1 within the symbol
      float amplitude;
      switch (shape) {
        case Shape::REVERSAL:
          amplitude = phase_sign * cosf((float)M_PI * t);
          break;
        case Shape::FADE_IN:
          amplitude = phase_sign * sinf((float)M_PI / 2 * t);
          break;
        case Shape::FADE_OUT:
          amplitude = phase_sign * cosf((float)M_PI / 2 * t);
          break;
        default:
          amplitude = phase_sign;
          break;
      }
      samples.write(amplitude * im);
      nextCarrier();
    }
    if (shape == Shape::REVERSAL) phase_sign = -phase_sign;
  }

  void nextCarrier() {
    float r = re * step_re - im * step_im;
    im = re * step_im + im * step_re;
    re = r;
    if (++sample_count % 1024 == 0) {
      float mag = sqrtf(re * re + im * im);
      re /= mag;
      im /= mag;
    }
  }
};

}  // namespace tinymodem
