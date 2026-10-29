#pragma once

#include <math.h>

#include "AudioTools.h"
#include "ModemBase.h"
#include "Olivia.h"

namespace tinymodem {

/**
 * @brief Olivia and Contestia modulator (compatible with fldigi): the written
 * characters are collected in blocks of bitsPerSymbol() characters, which are
 * FEC encoded into 64 (Contestia: 32) symbols. Each symbol is a tone with a
 * raised cosine envelope over two symbol periods, so consecutive symbols
 * overlap by half.
 *
 * A transmission starts and ends with the fldigi start/stop tones (at the
 * band edges). Call endTransmission() (or flush()) when the data is
 * complete: an incomplete block is filled with 0 (the idle character).
 * Olivia sends 8 bit characters escaped with 127, like fldigi: the character
 * 127 itself can't be sent and is ignored.
 * @copyright GPLv3
 */
class OliviaEncoder : public ModemEncoder {
 public:
  OliviaEncoder() = default;
  OliviaEncoder(Print& out) { setOutput(out); }

  /// Provides a default configuration: olivia-32-1000
  OliviaConfig defaultConfig() { return OliviaConfig(); }

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
  bool begin(OliviaConfig config) {
    cfg = config;
    return begin();
  }

  bool begin() override {
    is_active = false;
    if (!cfg.isValid()) return false;
    if (cfg.bits_per_sample != 16) {
      LOGE("OliviaEncoder: bits_per_sample must be 16, not %d",
           (int)cfg.bits_per_sample);
      return false;
    }
    if (cfg.channels < 1) {
      LOGE("OliviaEncoder: invalid number of channels: %d", (int)cfg.channels);
      return false;
    }
    samples.begin(cfg.channels, cfg.volume);
    if (!tones.begin(cfg, cfg.volume)) return false;
    bits = cfg.bitsPerSymbol();
    codec.begin(bits, cfg.contestia);
    symbol_time = cfg.symbolSepar() / OliviaConfig::BASE_RATE;
    transmitting = false;
    block_len = 0;
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
      uint8_t ch = data[j];
      // 127 is the escape character
      if (codec.isSkipped(ch) || (!cfg.contestia && ch == 127)) continue;
      if (!cfg.contestia && ch > 127) {
        addChar(127);
        ch &= 127;
      }
      addChar(ch);
    }
    samples.flush();
    return len;
  }

  /// Sends the incomplete block and the stop tones (and adds optional
  /// silence): returns true if a transmission was ended.
  bool endTransmission(size_t silence_samples = 0) override {
    if (!is_active || !transmitting) return false;
    if (block_len > 0) {
      while (block_len < bits) block[block_len++] = 0;
      sendBlock();
    }
    sendSymbol(-1);  // the end of the last symbol
    if (cfg.start_tones) sendStartTones();
    for (size_t j = 0; j < silence_samples; j++) samples.write(0.0f);
    samples.flush();
    transmitting = false;
    return true;
  }

  /// Sends idle blocks (characters 0) for the indicated number of samples
  bool writeIdle(size_t nsamples) override {
    if (!is_active) return false;
    size_t block_samples =
        symbol_time * codec.blockLen() * cfg.sample_rate;
    size_t n = nsamples / block_samples + 1;
    for (size_t j = 0; j < n * bits; j++) addChar(0);
    samples.flush();
    return true;
  }

  /// true between the first character and endTransmission()
  bool isTransmitting() override { return transmitting; }

  /// Symbols per second
  float symbolRate() override { return cfg.symbolRate(); }

  /// The configuration which was provided in begin()
  OliviaConfig& config() { return cfg; }

  operator bool() override { return is_active; }

 protected:
  OliviaConfig cfg;
  SampleOutput samples;
  OliviaCodec codec;
  bool is_active = false;
  bool transmitting = false;
  int bits = 5;
  uint8_t block[8];
  int block_len = 0;
  double symbol_time = 0;  // in seconds
  // modulator
  uint64_t symbol_index = 0;
  uint64_t sample_index = 0;
  float prev_freq = 0, prev_phase = 0;  // previous symbol
  float phase = 0;                      // phase of the next symbol
  uint32_t random_seed = 1;

  void startTransmission() {
    transmitting = true;
    if (cfg.start_tones) sendStartTones();
    symbol_index = 0;
    sample_index = 0;
    prev_freq = 0;
    phase = 0;
  }

  void addChar(uint8_t ch) {
    if (!transmitting) startTransmission();
    block[block_len++] = ch;
    if (block_len == bits) sendBlock();
  }

  void sendBlock() {
    uint8_t symbols[64];
    codec.encodeBlock(block, symbols);
    for (int t = 0; t < codec.blockLen(); t++)
      sendSymbol(OliviaCodec::grayCode(symbols[t]));
    block_len = 0;
  }

  /// Writes one symbol period: the second half of the previous symbol and
  /// the first half of this symbol (-1 = none)
  void sendSymbol(int tone) {
    float freq = tone < 0 ? 0 : cfg.toneFreq(tone);
    uint64_t stop = llround((symbol_index + 1) * symbol_time * cfg.sample_rate);
    double start_time = symbol_index * symbol_time;
    for (; sample_index < stop; sample_index++) {
      float tau = sample_index / (double)cfg.sample_rate - start_time;
      // raised cosine over two symbol periods: 0 - 1 in the first half
      float w = 0.5f - 0.5f * cosf((float)M_PI * tau / symbol_time);
      float value = 0;
      if (freq > 0) value += w * cosf(phase + 2.0f * (float)M_PI * freq * tau);
      if (prev_freq > 0)
        value += (1.0f - w) *
                 cosf(prev_phase +
                      2.0f * (float)M_PI * prev_freq * (tau + symbol_time));
      samples.write(value);
    }
    symbol_index++;
    // continuous phase in the middle of the symbol and a random +/- 90 degree
    // step like fldigi
    prev_freq = freq;
    prev_phase = phase;
    random_seed = random_seed * 1103515245u + 12345u;
    float step = ((random_seed >> 16) & 1) ? (float)M_PI / 2 : -(float)M_PI / 2;
    phase = fmodf(phase + 2.0f * (float)M_PI * freq * symbol_time + step,
                  2.0f * (float)M_PI);
  }

  /// The fldigi start and stop tones: the lower and upper band edge twice,
  /// each for 2048 samples at 8000 Hz
  void sendStartTones() {
    size_t len = 2048 * cfg.sample_rate / 8000;
    size_t ramp = len / 8;
    float freqs[2] = {cfg.center_freq - cfg.bandwidth / 2.0f,
                      cfg.center_freq + cfg.bandwidth / 2.0f};
    for (int j = 0; j < 4; j++) {
      float w = 2.0f * (float)M_PI * freqs[j % 2] / cfg.sample_rate;
      for (size_t i = 0; i < len; i++) {
        float gain = 1.0f;
        if (i < ramp) gain = 0.5f - 0.5f * cosf((float)M_PI * i / ramp);
        if (len - 1 - i < ramp)
          gain = 0.5f - 0.5f * cosf((float)M_PI * (len - 1 - i) / ramp);
        samples.write(gain * cosf(w * i));
      }
    }
  }
};

}  // namespace tinymodem
