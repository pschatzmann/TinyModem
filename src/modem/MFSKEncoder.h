#pragma once

#include <math.h>

#include "AudioTools.h"
#include "MFSKConfig.h"
#include "ModemBase.h"

namespace tinymodem {

/// Gray code of a value: neighbouring tones differ in a single bit
inline unsigned grayEncode(unsigned value) { return value ^ (value >> 1); }

/// Inverse of grayEncode()
inline unsigned grayDecode(unsigned gray) {
  unsigned value = gray;
  for (unsigned shift = gray >> 1; shift != 0; shift >>= 1) value ^= shift;
  return value;
}

/**
 * @brief Multi-tone FSK modulator: converts the written bytes into 16 bit
 * PCM audio. Each symbol is one of MFSKConfig::tones tones and carries
 * log2(tones) bits (Gray coded, most significant bit first).
 *
 * A transmission starts with a preamble (alternating lowest and highest
 * tone) for the receiver's timing, followed by the sync symbols (see
 * MFSKConfig::syncSymbol()) and the data. Call endTransmission() (or flush()) when the data
 * is complete: the last symbol is padded with 0 bits and 2 symbols of
 * silence mark the end.
 * @copyright GPLv3
 */
class MFSKEncoder : public ModemEncoder {
 public:
  MFSKEncoder() = default;
  MFSKEncoder(Print& out) { setOutput(out); }

  /// Provides a default configuration: mfsk16
  MFSKConfig defaultConfig() {
    MFSKConfig result;
    result.setMode("mfsk16");
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

  /// Starts the processing with the indicated configuration
  bool begin(MFSKConfig config) {
    cfg = config;
    return begin();
  }

  bool begin() override {
    is_active = false;
    if (!cfg.isValid()) return false;
    if (cfg.bits_per_sample != 16) {
      LOGE("MFSKEncoder: bits_per_sample must be 16, not %d",
           (int)cfg.bits_per_sample);
      return false;
    }
    if (cfg.channels < 1) {
      LOGE("MFSKEncoder: invalid number of channels: %d", (int)cfg.channels);
      return false;
    }
    if (!tones.begin(cfg, cfg.volume)) return false;
    bits_per_symbol = cfg.bitsPerSymbol();
    samples_per_symbol = cfg.samplesPerSymbol();
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
      bit_buffer = (bit_buffer << 8) | data[j];
      bit_count += 8;
      while (bit_count >= bits_per_symbol) writeBits();
    }
    return len;
  }

  /// Sends the remaining bits and the end marker (and optional silence):
  /// returns true if a transmission was ended.
  bool endTransmission(size_t silence_samples = 0) override {
    if (!is_active || !transmitting) return false;
    // pad the last symbol with 0 bits
    if (bit_count > 0) {
      bit_buffer <<= bits_per_symbol - bit_count;
      bit_count = bits_per_symbol;
      writeBits();
    }
    for (int j = 0; j < TRAILER_SYMBOLS; j++) writeSymbol(-1);
    if (silence_samples) tones.tone(0, silence_samples);
    transmitting = false;
    return true;
  }

  /// true between the first byte and endTransmission()
  bool isTransmitting() override { return transmitting; }

  /// Symbols per second
  float symbolRate() override { return cfg.symbol_rate; }

  /// The configuration which was provided in begin()
  MFSKConfig& config() { return cfg; }

  operator bool() override { return is_active; }

 protected:
  static constexpr int TRAILER_SYMBOLS = 2;
  MFSKConfig cfg;
  bool is_active = false;
  bool transmitting = false;
  int bits_per_symbol = 4;
  double samples_per_symbol = 0;
  uint32_t symbol_index = 0;
  uint32_t bit_buffer = 0;
  int bit_count = 0;

  void startTransmission() {
    transmitting = true;
    symbol_index = 0;
    bit_buffer = 0;
    bit_count = 0;
    // preamble: alternating lowest and highest tone
    for (int j = 0; j < cfg.preamble_symbols; j++)
      writeSymbol(j % 2 == 0 ? 0 : cfg.tones - 1);
    // sync symbols
    for (int j = 0; j < cfg.syncLength(); j++) writeSymbol(cfg.syncSymbol(j));
  }

  /// Writes the next bits_per_symbol bits from the bit buffer
  void writeBits() {
    unsigned value = (bit_buffer >> (bit_count - bits_per_symbol)) &
                     ((1u << bits_per_symbol) - 1);
    bit_count -= bits_per_symbol;
    bit_buffer &= (1u << bit_count) - 1;
    writeSymbol(grayEncode(value));
  }

  /// Writes the tone with the indicated index (-1 = silence). The symbol
  /// boundaries are rounded from the exact (fractional) symbol length.
  void writeSymbol(int tone) {
    size_t start = llround((double)symbol_index * samples_per_symbol);
    size_t stop = llround((double)(symbol_index + 1) * samples_per_symbol);
    symbol_index++;
    tones.tone(tone < 0 ? 0.0f : cfg.toneFrequency(tone), stop - start);
  }
};

}  // namespace tinymodem
