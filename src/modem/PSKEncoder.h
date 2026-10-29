#pragma once

#include <math.h>

#include "AudioTools.h"
#include "ModemBase.h"
#include "PSK.h"

namespace tinymodem {

/**
 * @brief PSK modulator (compatible with fldigi): the written bytes are
 * Varicode encoded and each symbol is a phase step with a cosine shaped
 * transition from the previous phase.
 *
 * BPSK (PSK31): the characters are separated by "00", a 0 bit is a phase
 * reversal, a 1 bit keeps the phase. QPSK: each bit is encoded by a rate 1/2
 * convolutional code into a phase step of 0, 90, 180 or 270 degrees. 8PSK:
 * the MFSK Varicode bits are sent in groups of 3 as phase steps of 45 degrees.
 *
 * A transmission starts with phase reversals (preamble). Call
 * endTransmission() (or flush()) when the data is complete: the postamble is
 * sent (BPSK and 8PSK: steady carrier, QPSK: encoded 0 bits) and the signal
 * fades out.
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
    code.reset();
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
      sendChar(data[j]);
    }
    samples.flush();
    return len;
  }

  /// Sends the postamble and fades out (and adds optional silence): returns
  /// true if a transmission was ended.
  bool endTransmission(size_t silence_samples = 0) override {
    if (!is_active || !transmitting) return false;
    switch (cfg.modulation) {
      case PSKModulation::QPSK:
        // flushes the convolutional code
        for (int j = 0; j < cfg.postamble_symbols; j++) sendBit(0);
        break;
      case PSKModulation::PSK8:
        for (int j = 0; j < 3; j++) sendChar(0);
        bit_count = 0;
        for (int j = 0; j < cfg.postamble_symbols; j++) sendStep(STEADY);
        break;
      default:
        for (int j = 0; j < cfg.postamble_symbols; j++) sendBit(1);
        break;
    }
    sendSymbol(REVERSAL, Shape::FADE_OUT);
    for (size_t j = 0; j < silence_samples; j++) samples.write(0.0f);
    samples.flush();
    transmitting = false;
    return true;
  }

  /// Sends the idle signal (BPSK and QPSK: 0 bits, 8PSK: the character 0)
  /// for the indicated number of samples
  bool writeIdle(size_t nsamples) override {
    if (!is_active) return false;
    if (!transmitting) startTransmission();
    size_t n = nsamples / samples_per_symbol + 1;
    if (cfg.modulation == PSKModulation::PSK8) {
      // 11 bits per character
      for (size_t j = 0; j < n * 3 / 11 + 1; j++) sendChar(0);
    } else {
      for (size_t j = 0; j < n; j++) sendBit(0);
    }
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
  enum class Shape { TRANSITION, FADE_IN, FADE_OUT };
  /// phase steps as index of fldigi's 16 PSK positions (180 + n * 22.5 deg)
  static constexpr int REVERSAL = 0;
  static constexpr int STEADY = 8;
  PSKConfig cfg;
  SampleOutput samples;
  QPSKCode code;
  bool is_active = false;
  bool transmitting = false;
  double samples_per_symbol = 0;
  uint64_t symbol_index = 0;
  // current constellation point
  float point_re = 1.0f, point_im = 0.0f;
  // 8PSK: collected bits
  int bits = 0, bit_count = 0;
  // carrier oscillator (rotating phasor)
  float re = 1.0f, im = 0.0f;
  float step_re = 1.0f, step_im = 0.0f;
  uint32_t sample_count = 0;

  void startTransmission() {
    transmitting = true;
    symbol_index = 0;
    point_re = 1.0f;
    point_im = 0.0f;
    bit_count = 0;
    sendSymbol(STEADY, Shape::FADE_IN);
    for (int j = 0; j < cfg.preamble_symbols; j++) sendStep(REVERSAL);
    // 8PSK: the character 0 marks the start of the data
    if (cfg.modulation == PSKModulation::PSK8) sendChar(0);
  }

  void sendChar(uint8_t ch) {
    if (cfg.modulation == PSKModulation::PSK8) {
      Varicode::Code c = MFSKVaricode::encode(ch);
      for (int b = c.length - 1; b >= 0; b--) sendBit((c.bits >> b) & 1);
      return;
    }
    Varicode::Code c = Varicode::encode(ch);
    for (int b = c.length - 1; b >= 0; b--) sendBit((c.bits >> b) & 1);
    sendBit(0);
    sendBit(0);
  }

  void sendBit(int bit) {
    switch (cfg.modulation) {
      case PSKModulation::QPSK: {
        int symbol = code.encode(bit);
        sendStep(((4 - symbol) & 3) * 4);
        break;
      }
      case PSKModulation::PSK8:
        // the first bit is the least significant bit of the symbol
        if (bit_count == 0) bits = 0;
        bits |= bit << bit_count;
        if (++bit_count == 3) {
          sendStep(bits * 2);
          bit_count = 0;
        }
        break;
      default:
        // 0: phase reversal, 1: same phase
        sendStep(bit ? STEADY : REVERSAL);
        break;
    }
  }

  void sendStep(int step) { sendSymbol(step, Shape::TRANSITION); }

  /// Moves the constellation point by the phase step (index of 180 + n *
  /// 22.5 degrees)
  void sendSymbol(int step, Shape shape) {
    float prev_re = point_re, prev_im = point_im;
    if (shape == Shape::TRANSITION) {
      float angle = (float)M_PI + step * (float)M_PI / 8;
      float c = cosf(angle), s = sinf(angle);
      // round to the exact values of the 8 main positions
      if (fabsf(c) < 1e-6f) c = 0;
      if (fabsf(s) < 1e-6f) s = 0;
      point_re = prev_re * c - prev_im * s;
      point_im = prev_re * s + prev_im * c;
    }
    size_t start = llround(symbol_index * samples_per_symbol);
    size_t stop = llround((symbol_index + 1) * samples_per_symbol);
    symbol_index++;
    size_t n = stop - start;
    for (size_t j = 0; j < n; j++) {
      float t = (float)j / n;  // 0 to 1 within the symbol
      float i_value, q_value;
      if (shape == Shape::TRANSITION) {
        // cosine shaped transition from the previous point
        float a = 0.5f + 0.5f * cosf((float)M_PI * t);
        i_value = a * prev_re + (1.0f - a) * point_re;
        q_value = a * prev_im + (1.0f - a) * point_im;
      } else {
        float a = shape == Shape::FADE_IN ? sinf((float)M_PI / 2 * t)
                                          : cosf((float)M_PI / 2 * t);
        i_value = a * point_re;
        q_value = a * point_im;
      }
      // like fldigi: I * cos + Q * sin
      samples.write(i_value * re + q_value * im);
      nextCarrier();
    }
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
