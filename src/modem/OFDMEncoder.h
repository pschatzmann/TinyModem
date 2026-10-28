#pragma once

#include <math.h>

#include "AudioTools.h"
#include "ModemBase.h"
#include "OFDM.h"

namespace tinymodem {

/**
 * @brief OFDM modulator: the written bytes are collected into frames (up to
 * OFDMConfig::max_frame bytes) which are transmitted with differential QPSK
 * on all carriers. A frame is sent when it is full or when you call
 * endTransmission() (or flush()).
 *
 * A frame consists of a sync symbol (only even carriers, so the two halves
 * are identical), a reference symbol, a header symbol with the length and
 * the data symbols. Each symbol starts with a cyclic prefix.
 * @copyright GPLv3
 */
class OFDMEncoder : public ModemEncoder {
 public:
  OFDMEncoder() = default;
  OFDMEncoder(Print& out) { setOutput(out); }

  /// Provides a default configuration: ofdm32
  OFDMConfig defaultConfig() {
    OFDMConfig result;
    result.setMode("ofdm32");
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
  bool begin(OFDMConfig config) {
    cfg = config;
    return begin();
  }

  bool begin() override {
    is_active = false;
    if (!cfg.isValid()) return false;
    if (cfg.bits_per_sample != 16) {
      LOGE("OFDMEncoder: bits_per_sample must be 16, not %d",
           (int)cfg.bits_per_sample);
      return false;
    }
    if (cfg.channels < 1) {
      LOGE("OFDMEncoder: invalid number of channels: %d", (int)cfg.channels);
      return false;
    }
    samples.begin(cfg.channels, cfg.volume);
    if (!tones.begin(cfg, cfg.volume)) return false;
    fft_size = cfg.fftSize();
    prefix_size = cfg.prefixSize();
    first_bin = cfg.firstBin();
    table.begin(fft_size);
    phases.resize(cfg.carriers);
    frame.resize(cfg.max_frame);
    frame_len = 0;
    // the rms of the sum of all carriers is about 0.25 * volume
    amplitude = 0.25f * sqrtf(2.0f / cfg.carriers);
    is_active = true;
    return true;
  }

  void end() override {
    endTransmission();
    is_active = false;
  }

  /// Collects the bytes and sends full frames
  size_t write(const uint8_t* data, size_t len) override {
    if (!is_active) return 0;
    for (size_t j = 0; j < len; j++) {
      frame[frame_len++] = data[j];
      if (frame_len == cfg.max_frame) sendFrame();
    }
    return len;
  }

  /// Sends the collected bytes (and optional silence): returns true if data
  /// was sent
  bool endTransmission(size_t silence_samples = 0) override {
    if (!is_active || frame_len == 0) return false;
    sendFrame();
    // silence of one symbol marks the end
    size_t n = fft_size + prefix_size + silence_samples;
    for (size_t j = 0; j < n; j++) samples.write(0.0f);
    samples.flush();
    return true;
  }

  /// true if there is data which was not sent yet
  bool isTransmitting() override { return frame_len > 0; }

  /// OFDM symbols per second
  float symbolRate() override { return cfg.symbolRate(); }

  /// The configuration which was provided in begin()
  OFDMConfig& config() { return cfg; }

  operator bool() override { return is_active; }

 protected:
  OFDMConfig cfg;
  SampleOutput samples;
  OFDMTable table;
  bool is_active = false;
  int fft_size = 0;
  int prefix_size = 0;
  int first_bin = 0;
  float amplitude = 0.1f;
  audio_tools::Vector<uint8_t> phases;  // current phase of each carrier
  audio_tools::Vector<uint8_t> frame;
  int frame_len = 0;

  void sendFrame() {
    int n = cfg.carriers;
    // sync symbol: even carriers with known phases
    for (int i = 0; i < n; i++) phases[i] = OFDMConfig::knownPhase(0, i);
    sendSymbol(true);
    // reference symbol: all carriers with known phases
    for (int i = 0; i < n; i++) phases[i] = OFDMConfig::knownPhase(1, i);
    sendSymbol(false);
    // header: length and inverted length (16 bit each)
    sendHeader((uint32_t)frame_len << 16 | (uint16_t)~frame_len);
    // data
    int bit_count = 0;
    sendBits(frame.data(), frame_len, bit_count);
    if (bit_count > 0) sendSymbol(false);
    samples.flush();
    frame_len = 0;
  }

  /// Adds the bits (2 per carrier) and sends the full symbols. A symbol
  /// changes the phase of each carrier by 0, 90, 180 or 270 degrees. The
  /// carriers of an incomplete last symbol which are not used keep their
  /// phase.
  void sendBits(const uint8_t* data, int len, int& bit_count) {
    int n = cfg.carriers;
    for (int j = 0; j < len; j++) {
      for (int b = 6; b >= 0; b -= 2) {
        int dibit = (data[j] >> b) & 3;
        int carrier = bit_count / 2;
        phases[carrier] = (phases[carrier] + grayToPhase(dibit)) & 3;
        bit_count += 2;
        if (bit_count == 2 * n) {
          sendSymbol(false);
          bit_count = 0;
        }
      }
    }
  }

  /// The header bits change the phase by 0 or 180 degrees (more robust than
  /// QPSK) and are repeated on all carriers of the header symbols
  void sendHeader(uint32_t header) {
    int symbols = OFDMConfig::headerSymbols(cfg.carriers);
    for (int s = 0; s < symbols; s++) {
      for (int i = 0; i < cfg.carriers; i++) {
        int bit = (header >> (31 - (s * cfg.carriers + i) % 32)) & 1;
        if (bit) phases[i] = (phases[i] + 2) & 3;
      }
      sendSymbol(false);
    }
  }

  /// 00 -> 0, 01 -> 90, 11 -> 180, 10 -> 270 degrees
  static int grayToPhase(int dibit) {
    static const int phase[4] = {0, 1, 3, 2};
    return phase[dibit];
  }

  /// Writes the cyclic prefix and the symbol
  void sendSymbol(bool sync) {
    int n = cfg.carriers;
    float a = sync ? amplitude * 1.41421356f : amplitude;
    for (int t = -prefix_size; t < fft_size; t++) {
      int time = t < 0 ? t + fft_size : t;
      float value = 0;
      for (int i = 0; i < n; i++) {
        int bin = first_bin + i;
        if (sync && (bin & 1)) continue;  // only even carriers
        int idx = (int)(((int64_t)bin * time) % fft_size);
        // cos(angle + phase * 90 degrees)
        switch (phases[i]) {
          case 0:
            value += table.cosine(idx);
            break;
          case 1:
            value -= table.sine(idx);
            break;
          case 2:
            value -= table.cosine(idx);
            break;
          default:
            value += table.sine(idx);
            break;
        }
      }
      samples.write(a * value);
    }
  }
};

}  // namespace tinymodem
