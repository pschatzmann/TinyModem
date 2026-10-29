#pragma once

#include <math.h>
#include <string.h>

#include "AudioTools.h"
#include "GGWave.h"
#include "ModemBase.h"
#include "OFDM.h"

namespace tinymodem {

/**
 * @brief ggwave compatible modulator: the written bytes are collected into
 * messages of up to 140 bytes. A message is sent when it is full or when you
 * call endTransmission() (or flush()). The generated audio is identical to
 * the audio of ggwave (variable length format).
 *
 * A message consists of the start marker (16 frames), the Reed-Solomon coded
 * length (3 bytes), the Reed-Solomon coded payload and the end marker (16
 * frames). Each byte is sent as 2 simultaneous tones (one of 16 tones for
 * each nibble) and each symbol lasts GGWaveProtocol::frames_per_tx frames.
 * @copyright GPLv3
 */
class GGWaveEncoder : public ModemEncoder {
 public:
  GGWaveEncoder() = default;
  GGWaveEncoder(Print& out) { setOutput(out); }

  /// Provides a default configuration: ggwave-fast
  GGWaveConfig defaultConfig() {
    GGWaveConfig result;
    result.setMode("ggwave-fast");
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
  bool begin(GGWaveConfig config) {
    cfg = config;
    return begin();
  }

  bool begin() override {
    is_active = false;
    if (!cfg.isValid()) return false;
    if (cfg.bits_per_sample != 16) {
      LOGE("GGWaveEncoder: bits_per_sample must be 16, not %d",
           (int)cfg.bits_per_sample);
      return false;
    }
    if (cfg.channels < 1) {
      LOGE("GGWaveEncoder: invalid number of channels: %d", (int)cfg.channels);
      return false;
    }
    samples.begin(cfg.channels, cfg.volume);
    if (!tones.begin(cfg, cfg.volume)) return false;
    protocol = cfg.protocolInfo();
    frame_size = cfg.frameSize();
    table.begin(frame_size);
    message_len = 0;
    is_active = true;
    return true;
  }

  void end() override {
    endTransmission();
    is_active = false;
  }

  /// Collects the bytes and sends full messages
  size_t write(const uint8_t* data, size_t len) override {
    if (!is_active) return 0;
    for (size_t j = 0; j < len; j++) {
      message[message_len++] = data[j];
      if (message_len == GGWaveConfig::MAX_PAYLOAD) sendMessage();
    }
    return len;
  }

  /// Sends the collected bytes (and optional silence): returns true if a
  /// message was sent
  bool endTransmission(size_t silence_samples = 0) override {
    if (!is_active || message_len == 0) return false;
    sendMessage();
    for (size_t j = 0; j < silence_samples; j++) samples.write(0.0f);
    samples.flush();
    return true;
  }

  /// true if there is data which was not sent yet
  bool isTransmitting() override { return message_len > 0; }

  /// Data symbols per second
  float symbolRate() override { return cfg.symbolRate(); }

  /// The configuration which was provided in begin()
  GGWaveConfig& config() { return cfg; }

  operator bool() override { return is_active; }

 protected:
  /// a tone: FFT bin relative to freq_start and index of the phase offset
  struct Tone {
    int bin;
    int phase;
  };

  GGWaveConfig cfg;
  GGWaveProtocol protocol;
  SampleOutput samples;
  OFDMTable table;
  bool is_active = false;
  int frame_size = 1024;
  uint8_t message[GGWaveConfig::MAX_PAYLOAD];
  int message_len = 0;
  uint8_t encoded[GGWaveConfig::LENGTH_BYTES + GGWaveConfig::MAX_PAYLOAD +
                  2 * (GGWaveConfig::MAX_PAYLOAD / 5) + 3];

  void sendMessage() {
    int len = message_len;
    int ecc = GGWaveConfig::eccBytes(len);
    int bpt = protocol.bytes_per_tx;
    uint8_t plain[1 + GGWaveConfig::MAX_PAYLOAD];
    plain[0] = len;
    for (int i = 0; i < len; i++)
      plain[i + 1] = message[i] ^ (cfg.dss ? GGWaveConfig::dssMagic(i) : 0);
    memset(encoded, 0, sizeof(encoded));
    GGWaveRS::encode(plain, 1, GGWaveConfig::LENGTH_BYTES - 1, encoded);
    GGWaveRS::encode(plain + 1, len, ecc,
                     encoded + GGWaveConfig::LENGTH_BYTES);

    Tone tones[GGWaveConfig::MARKER_TONES];
    // start marker: the even tones use bin 2i, the odd tones bin 2i + 1
    for (int i = 0; i < GGWaveConfig::MARKER_TONES; i++)
      tones[i] = {2 * i + i % 2, i};
    for (int f = 0; f < GGWaveConfig::MARKER_FRAMES; f++)
      sendFrame(tones, GGWaveConfig::MARKER_TONES, f,
                GGWaveConfig::MARKER_FRAMES);

    // data: each nibble selects one of 16 bins
    int nsymbols = cfg.dataSymbols(len);
    for (int s = 0; s < nsymbols; s++) {
      int n = 0;
      for (int j = 0; j < bpt; j++) {
        uint8_t value = encoded[s * bpt + j];
        int k0 = (2 * j) * 16 + (value & 15);
        int k1 = (2 * j + 1) * 16 + (value >> 4);
        tones[n++] = {k0, k0 / 2};
        tones[n++] = {k1, k1 / 2};
      }
      for (int f = 0; f < protocol.frames_per_tx; f++)
        sendFrame(tones, n, f, protocol.frames_per_tx);
    }

    // end marker: inverse of the start marker
    for (int i = 0; i < GGWaveConfig::MARKER_TONES; i++)
      tones[i] = {2 * i + 1 - i % 2, i};
    for (int f = 0; f < GGWaveConfig::MARKER_FRAMES; f++)
      sendFrame(tones, GGWaveConfig::MARKER_TONES, f,
                GGWaveConfig::MARKER_FRAMES);
    samples.flush();
    message_len = 0;
  }

  /// Writes a frame with the indicated tones: the amplitude rises during the
  /// first 15% and falls during the last 15% of a cycle (symbol or marker)
  /// of frames (like ggwave's addAmplitudeSmooth())
  void sendFrame(const Tone* tones, int ntones, int cycle, int frames) {
    const int n = frame_size;
    const int total = frames * n;
    const float ds = 0.15f * total;
    const float ids = 1.0f / ds;
    const int begin = (int)(0.15f * total);
    const int end = (int)((1.0f - 0.15f) * total);
    const float scale = 1.0f / ntones;
    float sin_phase[GGWaveConfig::MARKER_TONES];
    float cos_phase[GGWaveConfig::MARKER_TONES];
    int idx[GGWaveConfig::MARKER_TONES];
    int step[GGWaveConfig::MARKER_TONES];
    for (int t = 0; t < ntones; t++) {
      // ggwave: phase offset = pi * index / data bits per symbol
      double phase = M_PI * tones[t].phase / (8 * protocol.bytes_per_tx);
      sin_phase[t] = sin(phase);
      cos_phase[t] = cos(phase);
      idx[t] = 0;
      step[t] = protocol.freq_start + tones[t].bin;
    }
    for (int i = 0; i < n; i++) {
      float value = 0;
      for (int t = 0; t < ntones; t++) {
        // sin(angle + phase)
        value += table.sine(idx[t]) * cos_phase[t] +
                 table.cosine(idx[t]) * sin_phase[t];
        idx[t] += step[t];
        if (idx[t] >= n) idx[t] -= n;
      }
      int k = cycle * n + i;
      float envelope = 1.0f;
      if (k < begin) {
        envelope = k * ids;
      } else if (k > end) {
        envelope = (total - k) * ids;
      }
      samples.write(value * envelope * scale);
    }
  }
};

}  // namespace tinymodem
