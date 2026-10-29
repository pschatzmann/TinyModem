#pragma once

#include <math.h>

#include "AudioTools.h"
#include "CSS.h"
#include "ModemBase.h"

namespace tinymodem {

/**
 * @brief Chirp spread spectrum modulator (LoRa-like): the written bytes are
 * collected and sent as frame when CSSConfig::max_frame bytes are available
 * or when you call endTransmission() (or flush()). A frame consists of the
 * preamble (up chirps), 2 down chirps, the header and the payload. Each
 * symbol is a chirp which starts at the frequency of its value and wraps
 * around at the end of the band. The phase is continuous.
 * @copyright GPLv3
 */
class CSSEncoder : public ModemEncoder {
 public:
  CSSEncoder() = default;
  CSSEncoder(Print& out) { setOutput(out); }

  /// Provides a default configuration: css7 (SF 7, 1000 Hz)
  CSSConfig defaultConfig() { return CSSConfig(); }

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
  bool begin(CSSConfig config) {
    cfg = config;
    return begin();
  }

  bool begin() override {
    is_active = false;
    if (!cfg.isValid()) return false;
    if (cfg.bits_per_sample != 16) {
      LOGE("CSSEncoder: bits_per_sample must be 16, not %d",
           (int)cfg.bits_per_sample);
      return false;
    }
    if (cfg.channels < 1) {
      LOGE("CSSEncoder: invalid number of channels: %d", (int)cfg.channels);
      return false;
    }
    samples.begin(cfg.channels, cfg.volume);
    if (!tones.begin(cfg, cfg.volume)) return false;
    frame_len = 0;
    frames = 0;
    is_active = true;
    return true;
  }

  void end() override {
    endTransmission();
    is_active = false;
  }

  /// Collects the data: a frame is sent when max_frame bytes are available
  size_t write(const uint8_t* data, size_t len) override {
    if (!is_active) return 0;
    for (size_t j = 0; j < len; j++) {
      frame[frame_len++] = data[j];
      if (frame_len == cfg.max_frame) sendFrame();
    }
    return len;
  }

  /// Sends the collected data (and adds optional silence): returns true if
  /// a frame was sent
  bool endTransmission(size_t silence_samples = 0) override {
    if (!is_active || frame_len == 0) return false;
    sendFrame();
    for (size_t j = 0; j < silence_samples; j++) samples.write(0.0f);
    samples.flush();
    return true;
  }

  /// true while data is waiting to be sent
  bool isTransmitting() override { return frame_len > 0; }

  /// Symbols per second
  float symbolRate() override { return cfg.symbolRate(); }

  /// Number of sent frames
  size_t frameCount() { return frames; }

  /// The configuration which was provided in begin()
  CSSConfig& config() { return cfg; }

  operator bool() override { return is_active; }

 protected:
  static constexpr float RAMP_SEC = 0.002f;
  CSSConfig cfg;
  SampleOutput samples;
  bool is_active = false;
  uint8_t frame[CSSConfig::MAX_FRAME];
  int frame_len = 0;
  size_t frames = 0;
  // modulator
  double phase = 0;
  uint64_t symbol_index = 0;
  size_t ramp = 0;
  size_t total = 0, written = 0;  // samples of the frame

  void sendFrame() {
    int sf = cfg.spreading_factor;
    uint8_t header[CSSFrame::HEADER_BYTES];
    CSSFrame::header(frame_len, header);
    int payload_len = CSSFrame::payloadBytes(frame_len, cfg.ecc_bytes);
    uint8_t payload[CSSConfig::MAX_FRAME + 2 + RSCodec::MAX_ECC];
    CSSFrame::payload(frame, frame_len, cfg.ecc_bytes, payload);
    uint16_t header_symbols[CSSFrame::HEADER_BYTES * 8];
    uint16_t payload_symbols[(CSSConfig::MAX_FRAME + 2 + RSCodec::MAX_ECC) * 8 / 5 + 1];
    int nh = CSSFrame::toSymbols(header, sizeof(header), sf, header_symbols);
    int np = CSSFrame::toSymbols(payload, payload_len, sf, payload_symbols);

    symbol_index = 0;
    phase = 0;
    written = 0;
    int nsymbols = cfg.preamble_symbols + 2 + nh + np;
    total = llround(nsymbols * (double)cfg.symbolTime() * cfg.sample_rate);
    ramp = RAMP_SEC * cfg.sample_rate;
    for (int j = 0; j < cfg.preamble_symbols; j++) sendChirp(0, true);
    for (int j = 0; j < 2; j++) sendChirp(0, false);
    for (int j = 0; j < nh; j++) sendChirp(header_symbols[j], true);
    for (int j = 0; j < np; j++) sendChirp(payload_symbols[j], true);
    samples.flush();
    frame_len = 0;
    frames++;
  }

  /// Integral of frac(x): the normalized frequency of a chirp is a saw tooth
  static double sawIntegral(double x) {
    double f = floor(x);
    return f / 2 + (x - f) * (x - f) / 2;
  }

  /// Writes an up chirp (starting at the frequency of the value) or a down
  /// chirp. The phase is calculated exactly: the frequency jump at the end of
  /// the band happens between two samples.
  void sendChirp(int value, bool up) {
    double t_sym = cfg.symbolTime();
    double t_start = symbol_index * t_sym;
    uint64_t start = llround(t_start * cfg.sample_rate);
    uint64_t stop = llround((symbol_index + 1) * t_sym * cfg.sample_rate);
    symbol_index++;
    double offset = (double)value / cfg.chips();
    double cycles_per_unit = cfg.bandwidth * t_sym;  // B * T
    for (uint64_t j = start; j < stop; j++) {
      double t = j / (double)cfg.sample_rate - t_start;
      double u = t / t_sym;  // 0 - 1 within the symbol
      // phase = 2 pi * integral of (f0 + B * frac(...)) dt
      double sweep = up ? sawIntegral(offset + u) - sawIntegral(offset)
                        : -sawIntegral(-u);
      double ph = phase + 2.0 * M_PI * (cfg.lowFreq() * t + cycles_per_unit * sweep);
      float gain = 1.0f;
      if (written < ramp) gain = 0.5f - 0.5f * cosf((float)M_PI * written / ramp);
      if (total - written <= ramp)
        gain = 0.5f - 0.5f * cosf((float)M_PI * (total - written) / ramp);
      samples.write(gain * (float)sin(ph));
      written++;
    }
    // phase at the end of the symbol: the mean frequency is the center
    phase = fmod(phase + 2.0 * M_PI * cfg.center_freq * t_sym, 2.0 * M_PI);
  }
};

}  // namespace tinymodem
