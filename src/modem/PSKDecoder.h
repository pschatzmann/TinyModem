#pragma once

#include <math.h>
#include <string.h>

#include "AudioTools.h"
#include "ModemBase.h"
#include "PSK.h"

namespace tinymodem {

/**
 * @brief PSK demodulator (BPSK like PSK31): the PCM audio data is written to
 * the decoder and the decoded bytes are written to the output. The input can
 * be 8, 16 or 32 bit signed samples with any number of channels (only
 * PSKConfig::channel is evaluated).
 *
 * The signal is mixed to the baseband and integrated over one symbol. The
 * symbol timing is determined from the energy distribution over the symbol
 * period, the bits from the phase difference of consecutive symbols. An
 * automatic frequency control tracks frequency offsets of up to a quarter of
 * the symbol rate. The receiver starts with a run of phase reversals
 * (preamble) and stops when the signal is lost or with a long steady carrier
 * (postamble).
 * @copyright GPLv3
 */
class PSKDecoder : public ModemDecoder {
 public:
  PSKDecoder() = default;
  PSKDecoder(Print& out) { setOutput(out); }

  /// Provides a default configuration: psk31
  PSKConfig defaultConfig() {
    PSKConfig result;
    result.setMode("psk31");
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
  bool begin(PSKConfig config) {
    cfg = config;
    return begin();
  }

  bool begin() override {
    is_active = false;
    if (!cfg.isValid()) return false;
    if (!input.begin(cfg.bits_per_sample, cfg.channels, cfg.channel))
      return false;
    samples_per_symbol = cfg.samplesPerSymbol();
    window = lroundf(samples_per_symbol);
    history.resize(window * 3);
    resetState();
    is_active = true;
    return true;
  }

  /// Processes the remaining samples and reports the end of the carrier
  void end() override {
    if (!is_active) return;
    flush();
    is_active = false;
  }

  /// Processes the remaining samples (e.g. at the end of the input)
  void flush() override {
    if (!is_active) return;
    // feed silence, so that the end of the transmission is detected
    size_t n = 4 * window;
    for (size_t j = 0; j < n && !is_finished; j++) addSample(0.0f, false);
    if (carrier) endOfCarrier();
    bool finished = is_finished;
    resetState();
    is_finished = finished;
  }

  /// Provide PCM data to the decoder
  size_t write(const uint8_t* data, size_t len) override {
    if (!is_active) return 0;
    input.write(data, len,
                [this](float sample) { addSample(sample, true); });
    return len;
  }

  /// true while a transmission is received
  bool isCarrier() override { return carrier; }

  /// true if the decoding was stopped (rx_one)
  bool isFinished() override { return is_finished; }

  /// Current frequency offset of the received signal in Hz
  float frequencyOffset() { return freq_offset; }

  /// The configuration which was provided in begin()
  PSKConfig& config() { return cfg; }

  operator bool() override { return is_active; }

 protected:
  static constexpr int BINS = 16;
  static constexpr int MIN_PREAMBLE = 8;
  static constexpr int MAX_ONES = 20;
  /// the signal is lost after MAX_WEAK symbols below LOST_LEVEL
  static constexpr int MAX_WEAK = 3;
  static constexpr float LOST_LEVEL = 0.3f;
  static constexpr float AFC_GAIN = 0.1f;
  static constexpr float TIMING_SMOOTHING = 0.15f;
  static constexpr float TIMING_GAIN = 0.5f;
  static constexpr float MAX_CORRECTION = 0.125f;

  PSKConfig cfg;
  bool is_active = false;
  bool is_finished = false;
  PcmInput input;
  uint32_t noise_seed = 1;
  float samples_per_symbol = 0;
  int window = 0;

  // mixer and integration over one symbol
  float nco_re = 1, nco_im = 0, step_re = 1, step_im = 0;
  float freq_offset = 0;
  audio_tools::Vector<float> history;  // i, q, x^2
  int pos = 0;
  float sum_i = 0, sum_q = 0, sum_power = 0;
  uint32_t sample_count = 0;

  // symbol timing
  float phase = 0;
  float target = 0.5f;
  float countdown = 0;  // samples until the next decision
  float bin_sum[BINS];
  int bin_count[BINS];
  float hist[BINS];

  // decisions
  float prev_i = 0, prev_q = 0;
  float quality = 0;
  bool carrier = false;
  int zeros = 0;
  int ones = 0;
  int weak = 0;
  int last_bit = 1;
  uint16_t code = 0;
  float track_amplitude = 0;
  // statistics
  unsigned ndata = 0;
  unsigned nsymbols = 0;
  float quality_total = 0;
  float amplitude_total = 0;

  void resetState() {
    for (size_t j = 0; j < history.size(); j++) history[j] = 0;
    pos = 0;
    sum_i = sum_q = sum_power = 0;
    for (int b = 0; b < BINS; b++) {
      bin_sum[b] = 0;
      bin_count[b] = 0;
      hist[b] = 0;
    }
    phase = 0;
    target = 0.5f;
    countdown = samples_per_symbol / 2;
    prev_i = prev_q = 0;
    quality = 0;
    carrier = false;
    zeros = ones = weak = 0;
    code = 0;
    is_finished = false;
    setFrequencyOffset(0);
    input.reset();
  }

  void setFrequencyOffset(float offset) {
    freq_offset = offset;
    double w = 2.0 * M_PI * (cfg.carrier_freq + offset) / cfg.sample_rate;
    step_re = cos(w);
    step_im = sin(w);
  }

  /// uniform random value in the range -0.5 to 0.5
  float noise() {
    noise_seed = noise_seed * 1103515245u + 12345u;
    return ((noise_seed >> 8) & 0xFFFF) / 65535.0f - 0.5f;
  }

  void addSample(float x, bool add_noise) {
    if (is_finished) return;
    if (add_noise && cfg.rx_noise != 0.0f)
      x += noise() * cfg.rx_noise * 2.0f;

    // mix to the baseband and integrate over one symbol
    float values[3] = {x * nco_re, -x * nco_im, x * x};
    float* old = history.data() + pos * 3;
    sum_i += values[0] - old[0];
    sum_q += values[1] - old[1];
    sum_power += values[2] - old[2];
    old[0] = values[0];
    old[1] = values[1];
    old[2] = values[2];
    if (++pos == window) pos = 0;
    float r = nco_re * step_re - nco_im * step_im;
    nco_im = nco_re * step_im + nco_im * step_re;
    nco_re = r;
    if (++sample_count % 1024 == 0) recalculate();

    // energy distribution over the symbol period
    float energy = sum_i * sum_i + sum_q * sum_q;
    float prev_phase = phase;
    phase += 1.0f / samples_per_symbol;
    int bin = (int)(prev_phase * BINS);
    if (bin >= BINS) bin = BINS - 1;
    bin_sum[bin] += energy;
    bin_count[bin]++;
    if (phase >= 1.0f) {
      phase -= 1.0f;
      updateTiming();
    }
    // one decision per symbol: the next decision moves towards the time with
    // the highest energy
    if (--countdown <= 0) {
      decide();
      float error = target - phase;  // in symbols
      if (error > 0.5f) error -= 1.0f;
      if (error < -0.5f) error += 1.0f;
      float correction = TIMING_GAIN * error;
      if (correction > MAX_CORRECTION) correction = MAX_CORRECTION;
      if (correction < -MAX_CORRECTION) correction = -MAX_CORRECTION;
      countdown += samples_per_symbol * (1.0f + correction);
    }
  }

  /// Avoids the accumulation of rounding errors
  void recalculate() {
    sum_i = sum_q = sum_power = 0;
    for (int k = 0; k < window; k++) {
      sum_i += history[k * 3];
      sum_q += history[k * 3 + 1];
      sum_power += history[k * 3 + 2];
    }
    float mag = sqrtf(nco_re * nco_re + nco_im * nco_im);
    nco_re /= mag;
    nco_im /= mag;
  }

  /// The decision time is the circular center of the energy distribution
  void updateTiming() {
    float min = 0;
    for (int b = 0; b < BINS; b++) {
      if (bin_count[b] > 0) {
        float value = bin_sum[b] / bin_count[b];
        hist[b] += TIMING_SMOOTHING * (value - hist[b]);
      }
      bin_sum[b] = 0;
      bin_count[b] = 0;
      if (b == 0 || hist[b] < min) min = hist[b];
    }
    float x = 0, y = 0;
    for (int b = 0; b < BINS; b++) {
      float angle = 2.0f * (float)M_PI * (b + 0.5f) / BINS;
      float weight = hist[b] - min;
      x += weight * cosf(angle);
      y += weight * sinf(angle);
    }
    if (x == 0 && y == 0) return;
    float t = atan2f(y, x) / (2.0f * (float)M_PI);
    target = t < 0 ? t + 1.0f : t;
  }

  /// Bit decision from the phase difference to the previous symbol
  void decide() {
    float zi = sum_i, zq = sum_q;
    float dr = zi * prev_i + zq * prev_q;  // z * conj(prev)
    float di = zq * prev_i - zi * prev_q;
    prev_i = zi;
    prev_q = zq;
    float amplitude = 2.0f * sqrtf(zi * zi + zq * zq) / window;
    float rms_amplitude = sqrtf(2.0f * sum_power / window);
    // close to 1 for a carrier, small for noise
    bool present = amplitude >= cfg.min_amplitude &&
                   amplitude > 0.5f * rms_amplitude;
    float mag2 = dr * dr + di * di;
    if (mag2 <= 0) return;
    // cos(2 * phase difference): 1 for a clean BPSK signal
    quality += 0.1f * ((dr * dr - di * di) / mag2 - quality);
    int bit = dr > 0 ? 1 : 0;

    if (present && cfg.afc) {
      // before the carrier is detected we expect the preamble (phase
      // reversals): this doubles the capture range to half the symbol rate
      bool reversal = carrier ? bit == 0 : true;
      float error = reversal ? atan2f(-di, -dr) : atan2f(di, dr);
      // a steady carrier is not a preamble with a frequency offset
      if (!carrier && fabsf(error) > 0.75f * (float)M_PI) error = 0;
      float offset = freq_offset + AFC_GAIN * error * cfg.symbol_rate /
                                       (2.0f * (float)M_PI);
      float max_offset = cfg.symbol_rate / 2;
      if (offset > max_offset) offset = max_offset;
      if (offset < -max_offset) offset = -max_offset;
      setFrequencyOffset(offset);
    }

    if (!carrier) {
      // start with a run of phase reversals
      zeros = (present && bit == 0 && quality > 0.5f) ? zeros + 1 : 0;
      if (zeros >= MIN_PREAMBLE) startOfCarrier(amplitude);
      return;
    }

    // end of the transmission: signal lost or long steady carrier
    bool weak_symbol = amplitude < cfg.min_amplitude ||
                       amplitude < LOST_LEVEL * track_amplitude;
    weak = weak_symbol ? weak + 1 : 0;
    ones = bit ? ones + 1 : 0;
    if (weak >= MAX_WEAK || ones >= MAX_ONES || quality < 0.2f) {
      endOfCarrier();
      return;
    }
    if (!weak_symbol) {
      track_amplitude += 0.1f * (amplitude - track_amplitude);
      quality_total += quality;
      amplitude_total += amplitude;
      nsymbols++;
    }
    varicode(bit);
  }

  /// Collects the bits of a character: characters are separated by "00"
  void varicode(int bit) {
    if (bit == 0 && last_bit == 0) {
      if ((code >> 1) != 0) {
        int ch = Varicode::decode(code >> 1);
        if (ch >= 0 && p_print != nullptr) {
          p_print->write((uint8_t)ch);
          ndata++;
        }
      }
      code = 0;
    } else {
      code = (code << 1) | bit;
      if (code & 0x8000) code = 0;  // too long: no valid character
    }
    last_bit = bit;
  }

  void startOfCarrier(float amplitude) {
    carrier = true;
    zeros = ones = weak = 0;
    code = 0;
    last_bit = 0;
    track_amplitude = amplitude;
    ndata = 0;
    nsymbols = 0;
    quality_total = 0;
    amplitude_total = 0;
    status("### CARRIER psk %.2f baud @ %.1f Hz ###\n",
           (double)cfg.symbol_rate, (double)(cfg.carrier_freq + freq_offset));
  }

  void endOfCarrier() {
    carrier = false;
    zeros = 0;
    float n = nsymbols > 0 ? nsymbols : 1;
    status(
        "\n### NOCARRIER ndata=%u quality=%.3f ampl=%.3f freq=%.1f Hz ###\n",
        ndata, (double)(quality_total / n), (double)(amplitude_total / n),
        (double)(cfg.carrier_freq + freq_offset));
    setFrequencyOffset(0);
    if (cfg.rx_one) is_finished = true;
  }
};

}  // namespace tinymodem
