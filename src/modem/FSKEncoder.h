#pragma once

#include <math.h>

#include "AudioTools.h"
#include "DataCodecs.h"
#include "ModemBase.h"
#include "ModemConfig.h"

namespace tinymodem {

/**
 * @brief FSK modulator: converts the written characters into 16 bit PCM
 * audio which is written to the defined output. The generated signal is
 * compatible with minimodem --rx.
 *
 * The tones are produced by a SoundGenerator<int16_t>. By default this is a
 * FSKToneGenerator, which reproduces minimodem's signal. You can replace it
 * with setGenerator(): the generator must change the frequency immediately
 * and keep the phase continuous in setFrequency() - e.g. the AudioTools
 * FastIntSineGenerator, which needs no floating point and is a good choice
 * for MCUs without FPU. FloatSineGenerator, FastSineGenerator and
 * SineFromTable are not suited. The generator's amplitude is full scale:
 * ModemConfig::volume is applied by the encoder.
 *
 * A transmission starts with leader (mark) bits, followed by the optional
 * sync bytes and the data frames. Call endTransmission() (or flush()) to send
 * the trailer bits when there is no more data to send.
 * @copyright GPLv3
 */
class FSKEncoder : public ModemEncoder {
 public:
  FSKEncoder() = default;
  FSKEncoder(Print& out) { setOutput(out); }
  FSKEncoder(audio_tools::SoundGenerator<int16_t>& generator) {
    setGenerator(generator);
  }
  FSKEncoder(Print& out, audio_tools::SoundGenerator<int16_t>& generator) {
    setOutput(out);
    setGenerator(generator);
  }

  /// Provides a default configuration for a 1200 baud Bell 202 modem
  ModemConfig defaultConfig() {
    ModemConfig result;
    result.setMode("1200");
    return result;
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

  /// Starts the processing with the indicated modem configuration
  bool begin(ModemConfig config) {
    cfg = config;
    return begin();
  }

  bool begin() override {
    is_active = false;
    active_cfg = cfg;
    if (!active_cfg.resolve()) {
      LOGE("FSKEncoder: invalid configuration (max 64 bits per frame)");
      return false;
    }
    if (!active_cfg.isTxSupported()) {
      LOGE("FSKEncoder: transmit is not supported for this mode");
      return false;
    }
    if (active_cfg.bits_per_sample != 16) {
      LOGE("FSKEncoder: bits_per_sample must be 16, not %d",
           (int)active_cfg.bits_per_sample);
      return false;
    }
    if (active_cfg.channels < 1) {
      LOGE("FSKEncoder: invalid number of channels: %d",
           (int)active_cfg.channels);
      return false;
    }

    if (active_cfg.format == DataFormat::BAUDOT) {
      baudot.setUSOS(active_cfg.baudot_usos);
      p_codec = &baudot;
    } else {
      p_codec = &ascii;
    }

    bit_nsamples = active_cfg.sample_rate / active_cfg.baud_rate + 0.5f;
    if (!tones.begin(active_cfg, active_cfg.volume)) return false;
    transmitting = 0;
    is_active = true;
    return true;
  }

  void end() override {
    endTransmission();
    is_active = false;
  }

  /// Modulates the characters
  size_t write(const uint8_t* data, size_t len) override {
    if (!is_active) return 0;
    for (size_t j = 0; j < len; j++) writeChar(data[j]);
    return len;
  }

  /// Sends the trailer bits (and optional silence) if we are transmitting:
  /// returns true if a transmission was ended.
  bool endTransmission(size_t silence_samples = 0) override {
    if (!is_active || transmitting == 0) return false;
    for (int j = 0; j < active_cfg.trailer_bits; j++)
      tone(active_cfg.mark_freq, bit_nsamples);
    if (silence_samples) tone(0, silence_samples);
    transmitting = 0;
    return true;
  }

  /// Sends the idle (mark) tone for the indicated number of samples
  bool writeIdle(size_t nsamples) override {
    if (!is_active) return false;
    if (transmitting == 0) startTransmission();
    tone(idleFreq(), nsamples);
    return true;
  }

  /// true between the first character and endTransmission()
  bool isTransmitting() override { return transmitting != 0; }

  /// Bits per second
  float symbolRate() override { return active_cfg.baud_rate; }

  /// Number of samples of one bit
  size_t bitSamples() { return bit_nsamples; }

  /// The configuration which was provided in begin(): you can change it and
  /// call begin() again
  ModemConfig& config() { return cfg; }

  /// The active configuration with all derived values (e.g. frequencies)
  const ModemConfig& activeConfig() const { return active_cfg; }

  operator bool() override { return is_active; }

 protected:
  ModemConfig cfg;
  ModemConfig active_cfg;
  AsciiCodec ascii;
  BaudotCodec baudot;
  DataCodec* p_codec = &ascii;
  size_t bit_nsamples = 0;
  // 0: idle, 1: leader sent, 2: sync preamble sent
  int transmitting = 0;
  bool is_active = false;

  float idleFreq() {
    return active_cfg.invert_start_stop ? active_cfg.space_freq
                                        : active_cfg.mark_freq;
  }

  void startTransmission() {
    transmitting = 1;
    // the receiver resets its Baudot shift state with each new carrier, so
    // we start each transmission with a LTRS or FIGS code
    baudot.resetShift();
  }

  void writeChar(uint8_t ch) {
    if (transmitting == 0) {
      startTransmission();
      // emit leader tone (mark)
      for (int j = 0; j < active_cfg.leader_bits; j++)
        tone(idleFreq(), bit_nsamples);
    }
    uint32_t words[2];
    int nwords = p_codec->encode(words, ch);

    if (transmitting < 2) {
      transmitting = 2;
      // emit "preamble" of sync bytes
      for (int j = 0; j < active_cfg.tx_sync_bytes; j++)
        transmitFrame(active_cfg.sync_byte, false);
    }
    // emit data bits
    for (int j = 0; j < nwords; j++)
      transmitFrame(words[j], active_cfg.msb_first);
  }

  void transmitFrame(uint64_t bits, bool msb_first) {
    ModemConfig& c = active_cfg;
    float mark = c.mark_freq;
    float space = c.space_freq;
    if (c.start_bits > 0)
      tone(c.invert_start_stop ? mark : space, bit_nsamples * c.start_bits);
    for (int i = 0; i < c.data_bits; i++) {
      unsigned bit = msb_first ? (bits >> (c.data_bits - i - 1)) & 1
                               : (bits >> i) & 1;
      tone(bit == 1 ? mark : space, bit_nsamples);
    }
    if (c.stop_bits > 0)
      tone(c.invert_start_stop ? space : mark,
           (size_t)(bit_nsamples * c.stop_bits));
  }

  void tone(float freq, size_t nsamples) { tones.tone(freq, nsamples); }
};

}  // namespace tinymodem
