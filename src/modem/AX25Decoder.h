#pragma once

#include <math.h>
#include <string.h>

#include "AX25.h"
#include "AudioTools.h"
#include "ModemBase.h"

namespace tinymodem {

/**
 * @brief AX.25 packet radio demodulator (APRS, 1200 baud AFSK). The PCM
 * audio data is written to the decoder and the received frames with a
 * correct frame check sequence are written to the output: as monitor text
 * lines (SRC>DEST,DIGI:info) or as KISS frames (see AX25Config::format).
 *
 * The mark and space energy is measured with sliding correlators over one
 * bit, a digital PLL recovers the bit clock, and the bits are NRZI and HDLC
 * decoded. The input can be 8, 16 or 32 bit signed samples with any number of
 * channels (only AX25Config::channel is evaluated).
 * @copyright GPLv3
 */
class AX25Decoder : public ModemDecoder {
 public:
  AX25Decoder() = default;
  AX25Decoder(Print& out) { setOutput(out); }

  /// Provides a default configuration: APRS (1200 baud AFSK)
  AX25Config defaultConfig() {
    AX25Config result;
    result.setMode("aprs");
    return result;
  }

  /// Defines the format of the input audio data
  void setAudioInfo(audio_tools::AudioInfo info) override {
    if (info.sample_rate == cfg.sample_rate && info.channels == cfg.channels &&
        info.bits_per_sample == cfg.bits_per_sample)
      return;
    cfg.sample_rate = info.sample_rate;
    cfg.channels = info.channels;
    cfg.bits_per_sample = info.bits_per_sample;
    if (is_active) begin();
  }

  audio_tools::AudioInfo audioInfo() override { return cfg; }

  using audio_tools::AudioDecoder::begin;

  /// Starts the processing with the indicated configuration
  bool begin(AX25Config config) {
    cfg = config;
    return begin();
  }

  bool begin() override {
    is_active = false;
    if (!cfg.isValid()) return false;
    if (!input.begin(cfg.bits_per_sample, cfg.channels, cfg.channel))
      return false;
    window = lroundf(cfg.sample_rate / cfg.baud_rate);
    products.resize(window * 4);
    mark.begin(cfg.mark_freq, cfg.sample_rate);
    space.begin(cfg.space_freq, cfg.sample_rate);
    pll_step = cfg.baud_rate / cfg.sample_rate;
    resetState();
    frames = 0;
    errors = 0;
    is_active = true;
    return true;
  }

  /// Reports the statistics
  void end() override {
    if (!is_active) return;
    flush();
    status("### AX25 frames=%u fcs_errors=%u ###\n", (unsigned)frames,
           (unsigned)errors);
    is_active = false;
  }

  /// The decoding is done immediately: nothing to do
  void flush() override {}

  /// Provide PCM data to the decoder
  size_t write(const uint8_t* data, size_t len) override {
    if (!is_active) return 0;
    input.write(data, len, [this](float sample) { addSample(sample); });
    return len;
  }

  /// true while a frame is received
  bool isCarrier() override { return in_frame && frame_len > 0; }

  /// The decoder never stops
  bool isFinished() override { return false; }

  /// Number of received frames with a correct frame check sequence
  size_t frameCount() { return frames; }

  /// Number of frames with a wrong frame check sequence
  size_t errorCount() { return errors; }

  /// The configuration which was provided in begin()
  AX25Config& config() { return cfg; }

  operator bool() override { return is_active; }

 protected:
  /// Rotating phasor of a tone: avoids sin() and cos() per sample
  struct Phasor {
    float re = 1, im = 0, c = 1, s = 0;
    void begin(float freq, float sample_rate) {
      double w = 2.0 * M_PI * freq / sample_rate;
      c = cos(w);
      s = sin(w);
      re = 1;
      im = 0;
    }
    void next() {
      float r = re * c - im * s;
      im = re * s + im * c;
      re = r;
    }
    void normalize() {
      float mag = sqrtf(re * re + im * im);
      re /= mag;
      im /= mag;
    }
  };

  AX25Config cfg;
  bool is_active = false;
  PcmInput input;
  uint32_t noise_seed = 1;
  size_t frames = 0;
  size_t errors = 0;

  // correlators: products of the last window samples with the phasors
  int window = 40;
  audio_tools::Vector<float> products;  // mark re/im, space re/im
  int pos = 0;
  float sums[4] = {0, 0, 0, 0};
  Phasor mark, space;
  uint32_t sample_count = 0;

  // bit clock
  float pll_step = 0.025f;
  float pll = 0;
  bool last_tone = false;
  bool bit_tone = false;

  // HDLC
  uint8_t history = 0;
  int ones = 0;
  bool in_frame = false;
  uint8_t frame[AX25Frame::MAX_FRAME + 2];
  int frame_len = 0;
  uint8_t current = 0;
  int current_bits = 0;
  char text[AX25Frame::MAX_FRAME * 2];

  void resetState() {
    for (int j = 0; j < window * 4; j++) products[j] = 0;
    for (int j = 0; j < 4; j++) sums[j] = 0;
    pos = 0;
    pll = 0;
    in_frame = false;
    history = 0;
    ones = 0;
    input.reset();
  }

  /// uniform random value in the range -0.5 to 0.5
  float noise() {
    noise_seed = noise_seed * 1103515245u + 12345u;
    return ((noise_seed >> 8) & 0xFFFF) / 65535.0f - 0.5f;
  }

  void addSample(float x) {
    if (cfg.rx_noise != 0.0f) x += noise() * cfg.rx_noise * 2.0f;
    // sliding correlation over one bit
    float values[4] = {x * mark.re, x * mark.im, x * space.re, x * space.im};
    float* old = products.data() + pos * 4;
    for (int j = 0; j < 4; j++) {
      sums[j] += values[j] - old[j];
      old[j] = values[j];
    }
    if (++pos == window) pos = 0;
    mark.next();
    space.next();
    if (++sample_count % 4096 == 0) recalculate();

    float mark_energy = sums[0] * sums[0] + sums[1] * sums[1];
    float space_energy = sums[2] * sums[2] + sums[3] * sums[3];
    bool tone = mark_energy > space_energy;

    // digital PLL: a tone change should be in the middle between two bit
    // samples
    pll += pll_step;
    if (tone != last_tone) {
      pll -= PLL_GAIN * (pll - 0.5f);
      last_tone = tone;
    }
    if (pll >= 1.0f) {
      pll -= 1.0f;
      // NRZI: no tone change = 1
      int bit = tone == bit_tone ? 1 : 0;
      bit_tone = tone;
      hdlc(bit);
    }
  }

  /// Avoids the accumulation of rounding errors
  void recalculate() {
    for (int j = 0; j < 4; j++) sums[j] = 0;
    for (int k = 0; k < window; k++)
      for (int j = 0; j < 4; j++) sums[j] += products[k * 4 + j];
    mark.normalize();
    space.normalize();
  }

  void hdlc(int bit) {
    history = (history << 1) | bit;
    if (history == 0x7E) {
      // flag: the last 7 bits (0111111) belong to the flag
      if (in_frame && current_bits == 7) processFrame();
      in_frame = true;
      frame_len = 0;
      current = 0;
      current_bits = 0;
      ones = 0;
      return;
    }
    if (!in_frame) return;
    if (bit) {
      if (++ones > 6) {  // abort
        in_frame = false;
        return;
      }
    } else {
      if (ones == 5) {  // stuffed bit
        ones = 0;
        return;
      }
      ones = 0;
    }
    // least significant bit first
    current |= bit << current_bits;
    if (++current_bits == 8) {
      if (frame_len < (int)sizeof(frame)) {
        frame[frame_len++] = current;
      } else {
        in_frame = false;
      }
      current = 0;
      current_bits = 0;
    }
  }

  void processFrame() {
    if (frame_len < AX25Frame::MIN_FRAME + 2) return;
    int len = frame_len - 2;
    uint16_t fcs = frame[len] | (frame[len + 1] << 8);
    if (AX25Frame::fcs(frame, len) != fcs) {
      errors++;
      return;
    }
    frames++;
    if (p_print == nullptr) return;
    if (cfg.format == AX25Format::KISS) {
      AX25Frame::writeKiss(*p_print, frame, len);
    } else {
      int n = AX25Frame::toText(frame, len, text, sizeof(text) - 1);
      if (n > 0) {
        text[n++] = '\n';
        p_print->write((const uint8_t*)text, n);
      }
    }
  }

  static constexpr float PLL_GAIN = 0.15f;
};

}  // namespace tinymodem
