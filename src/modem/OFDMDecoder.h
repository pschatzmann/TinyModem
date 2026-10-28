#pragma once

#include <math.h>
#include <string.h>

#include "AudioTools.h"
#include "ModemBase.h"
#include "OFDM.h"

namespace tinymodem {

/**
 * @brief OFDM demodulator: the PCM audio data is written to the decoder and
 * the decoded frames are written to the output. The input can be 8, 16 or 32
 * bit signed samples with any number of channels (only OFDMConfig::channel is
 * evaluated).
 *
 * The start of a frame is found with the Schmidl & Cox method: the sync
 * symbol consists of two identical halves, which are detected with a sliding
 * autocorrelation. The reference symbol provides the initial phase of all
 * carriers and an estimate of the frequency offset and timing drift, which
 * are tracked during the frame. The carriers are analyzed with a DFT.
 * @copyright GPLv3
 */
class OFDMDecoder : public ModemDecoder {
 public:
  OFDMDecoder() = default;
  OFDMDecoder(Print& out) { setOutput(out); }

  /// Provides a default configuration: ofdm32
  OFDMConfig defaultConfig() {
    OFDMConfig result;
    result.setMode("ofdm32");
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
  bool begin(OFDMConfig config) {
    cfg = config;
    return begin();
  }

  bool begin() override {
    is_active = false;
    if (!cfg.isValid()) return false;
    if (!input.begin(cfg.bits_per_sample, cfg.channels, cfg.channel))
      return false;
    fft_size = cfg.fftSize();
    prefix_size = cfg.prefixSize();
    symbol_size = fft_size + prefix_size;
    half = fft_size / 2;
    first_bin = cfg.firstBin();
    ncarriers = cfg.carriers;
    table.begin(fft_size);
    z_re.resize(ncarriers);
    z_im.resize(ncarriers);
    prev_re.resize(ncarriers);
    prev_im.resize(ncarriers);
    buffer.resize(4 * symbol_size + 2 * fft_size);
    plateau.resize(2 * fft_size);
    smoothed.resize(2 * fft_size);
    resetState();
    is_active = true;
    return true;
  }

  /// Processes the remaining samples
  void end() override {
    if (!is_active) return;
    flush();
    is_active = false;
  }

  /// Processes the remaining samples (e.g. at the end of the input)
  void flush() override {
    if (!is_active) return;
    // feed silence, so that the last frame is complete
    size_t n = 2 * symbol_size + 2 * fft_size;
    for (size_t j = 0; j < n && !is_finished; j++) addSample(0.0f, false);
    if (state == State::FRAME) endFrame(false);
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

  /// true while a frame is received
  bool isCarrier() override { return state == State::FRAME; }

  /// true if the decoding was stopped (rx_one)
  bool isFinished() override { return is_finished; }

  /// Number of received frames
  size_t frameCount() { return frames; }

  /// The configuration which was provided in begin()
  OFDMConfig& config() { return cfg; }

  operator bool() override { return is_active; }

 protected:
  enum class State { SEARCH, FRAME };
  static constexpr float TRACKING_GAIN = 0.5f;
  static constexpr float MIN_POWER = 1e-6f;

  OFDMConfig cfg;
  bool is_active = false;
  bool is_finished = false;
  PcmInput input;
  OFDMTable table;
  uint32_t noise_seed = 1;
  size_t frames = 0;
  int fft_size = 0, prefix_size = 0, symbol_size = 0, half = 0;
  int first_bin = 0, ncarriers = 0;

  // sample buffer: buffer[0] is the sample with the global index base. The
  // indexes wrap around, so they are only compared by their difference.
  audio_tools::Vector<float> buffer;
  size_t nvalid = 0;
  uint32_t base = 0;
  uint32_t now = 0;     // global index of the next sample
  uint32_t filled = 0;  // number of samples since the reset (saturated)

  // Schmidl & Cox autocorrelation
  float corr = 0, power = 0;
  bool in_plateau = false;
  uint32_t plateau_start = 0;
  audio_tools::Vector<float> plateau;  // metric values of the plateau
  audio_tools::Vector<float> smoothed;
  int plateau_len = 0;

  // frame
  State state = State::SEARCH;
  uint32_t window = 0;  // global index of the DFT window of the next symbol
  int symbol = 0;
  audio_tools::Vector<float> z_re, z_im, prev_re, prev_im;
  float pred_common = 0, pred_slope = 0, slope_adjust = 0;
  float drift = 0;
  float reference_power = 0;
  float header_soft[32];
  int header_symbol = 0;
  int frame_len = -1;
  int ndata = 0;
  uint8_t current = 0;
  int current_dibits = 0;
  float error_total = 0;
  int error_count = 0;

  void resetState() {
    nvalid = 0;
    base = now = filled = 0;
    corr = power = 0;
    in_plateau = false;
    state = State::SEARCH;
    frame_len = -1;
    is_finished = false;
    input.reset();
  }

  /// uniform random value in the range -0.5 to 0.5
  float noise() {
    noise_seed = noise_seed * 1103515245u + 12345u;
    return ((noise_seed >> 8) & 0xFFFF) / 65535.0f - 0.5f;
  }

  float sample(uint32_t idx) { return buffer[idx - base]; }

  void addSample(float x, bool add_noise) {
    if (is_finished) return;
    if (add_noise && cfg.rx_noise != 0.0f)
      x += noise() * cfg.rx_noise * 2.0f;
    if (nvalid == buffer.size()) compact();
    buffer[nvalid++] = x;
    uint32_t t = now++;
    if (filled <= (uint32_t)(2 * half)) filled++;

    // sliding autocorrelation between the two halves of a window of
    // fft_size samples which starts at d = t - fft_size + 1:
    // corr = sum r(m - half) * r(m), power = sum r(m)^2 for the last half
    power += x * x;
    if (filled > (uint32_t)half) {
      float a = sample(t - half);
      corr += a * x;
      power -= a * a;
      if (filled > (uint32_t)(2 * half)) corr -= sample(t - 2 * half) * a;
    }
    if (filled >= (uint32_t)(2 * half)) {
      if (t % 4096 == 0) recalculate(t);
      uint32_t d = t - 2 * half + 1;
      if (state == State::SEARCH) search(d);
    }

    while (state == State::FRAME && isReached(now, window + fft_size)) {
      processSymbol();
    }
  }

  /// true if the global index a is at or after b (with wrap around)
  static bool isReached(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0; }

  void recalculate(uint32_t t) {
    corr = power = 0;
    for (int j = half - 1; j >= 0; j--) {
      uint32_t m = t - j;
      float b = sample(m);
      corr += sample(m - half) * b;
      power += b * b;
    }
  }

  /// Drops the samples which are not needed any more
  void compact() {
    uint32_t keep = now - 2 * half - 1;
    // the window of the sync symbol is in the plateau
    if (in_plateau && !isReached(plateau_start, keep)) keep = plateau_start;
    if (state == State::FRAME && !isReached(window, keep)) keep = window;
    int32_t n = keep - base;
    if (n <= 0) n = 1;  // never overflow
    memmove(buffer.data(), buffer.data() + n, (nvalid - n) * sizeof(float));
    nvalid -= n;
    base += n;
  }

  /// The sync symbol results in a plateau of the normalized correlation
  /// (length = cyclic prefix): we use the middle of the plateau
  void search(uint32_t d) {
    float metric = power > MIN_POWER * half ? corr / power : 0;
    if (metric > cfg.sync_threshold) {
      if (!in_plateau) {
        in_plateau = true;
        plateau_start = d;
        plateau_len = 0;
      }
      // the plateau of a sync symbol is about as long as the cyclic prefix:
      // a much longer one is a periodic signal (e.g. a steady tone), which
      // would prevent that the samples of the plateau are dropped
      if (plateau_len == (int)plateau.size()) {
        in_plateau = false;
        return;
      }
      plateau[plateau_len++] = metric;
      return;
    }
    if (!in_plateau) return;
    in_plateau = false;
    // smooth the metric against noise and use the middle of the range with
    // at least 80% of the peak
    int w = prefix_size / 4;
    if (w < 1) w = 1;
    float peak = 0;
    for (int j = 0; j < plateau_len; j++) {
      float sum = 0;
      int n = 0;
      for (int k = j - w / 2; k <= j + w / 2; k++) {
        if (k < 0 || k >= plateau_len) continue;
        sum += plateau[k];
        n++;
      }
      smoothed[j] = sum / n;
      if (smoothed[j] > peak) peak = smoothed[j];
    }
    int first = -1, last = -1;
    for (int j = 0; j < plateau_len; j++) {
      if (smoothed[j] >= 0.8f * peak) {
        if (first < 0) first = j;
        last = j;
      }
    }
    if (first < 0 || last - first < prefix_size / 4) return;
    window = plateau_start + (first + last) / 2;
    state = State::FRAME;
    symbol = 0;
    drift = 0;
    slope_adjust = 0;
  }

  /// DFT of the window at the carrier frequencies
  void dft() {
    for (int i = 0; i < ncarriers; i++) {
      int bin = first_bin + i;
      float re = 0, im = 0;
      int idx = 0;
      for (int n = 0; n < fft_size; n++) {
        float x = sample(window + n);
        re += x * table.cosine(idx);
        im -= x * table.sine(idx);
        idx += bin;
        if (idx >= fft_size) idx -= fft_size;
      }
      z_re[i] = re;
      z_im[i] = im;
    }
  }

  static float wrap(float angle) {
    while (angle > (float)M_PI) angle -= 2.0f * (float)M_PI;
    while (angle <= -(float)M_PI) angle += 2.0f * (float)M_PI;
    return angle;
  }

  /// Weighted linear fit: angle = common + slope * carrier
  static void fit(const float* angle, const float* weight, int n,
                  float& common, float& slope) {
    float sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int i = 0; i < n; i++) {
      float w = weight[i];
      if (w <= 0) continue;
      sw += w;
      sx += w * i;
      sy += w * angle[i];
      sxx += w * i * i;
      sxy += w * i * angle[i];
    }
    float det = sw * sxx - sx * sx;
    if (sw <= 0 || fabsf(det) < 1e-12f) {
      common = sw > 0 ? sy / sw : 0;
      slope = 0;
      return;
    }
    slope = (sw * sxy - sx * sy) / det;
    common = (sy - slope * sx) / sw;
  }

  void processSymbol() {
    dft();
    if (symbol == 0) {
      // sync symbol: keep it for the reference symbol
      for (int i = 0; i < ncarriers; i++) {
        prev_re[i] = z_re[i];
        prev_im[i] = z_im[i];
      }
    } else if (symbol == 1) {
      referenceSymbol();
    } else {
      dataSymbol();
    }
    if (state != State::FRAME) return;
    symbol++;
    // timing: keep the window in the middle of the cyclic prefix
    window += symbol_size;
    slope_adjust = 0;
    if (drift >= 1.0f) {
      window += 1;
      drift -= 1.0f;
      slope_adjust = 2.0f * (float)M_PI / fft_size;
    } else if (drift <= -1.0f) {
      window -= 1;
      drift += 1.0f;
      slope_adjust = -2.0f * (float)M_PI / fft_size;
    }
  }

  /// Initial estimate of the frequency offset and the timing drift from the
  /// known phases of the even carriers in the sync and reference symbols
  void referenceSymbol() {
    float angle[128], weight[128];
    reference_power = 0;
    for (int i = 0; i < ncarriers; i++) {
      float power_i = z_re[i] * z_re[i] + z_im[i] * z_im[i];
      reference_power += power_i;
      weight[i] = 0;
      angle[i] = 0;
      if ((first_bin + i) & 1) continue;
      // z * conj(prev) rotated by the known phase difference
      float dr = z_re[i] * prev_re[i] + z_im[i] * prev_im[i];
      float di = z_im[i] * prev_re[i] - z_re[i] * prev_im[i];
      int known = OFDMConfig::knownPhase(1, i) - OFDMConfig::knownPhase(0, i);
      angle[i] = wrap(atan2f(di, dr) - known * (float)M_PI / 2);
      weight[i] = sqrtf(dr * dr + di * di);
    }
    reference_power /= ncarriers;
    fit(angle, weight, ncarriers, pred_common, pred_slope);
    drift = 0;
    for (int b = 0; b < 32; b++) header_soft[b] = 0;
    header_symbol = 0;
    frame_len = -1;
    ndata = 0;
    current = 0;
    current_dibits = 0;
    error_total = 0;
    error_count = 0;
    for (int i = 0; i < ncarriers; i++) {
      prev_re[i] = z_re[i];
      prev_im[i] = z_im[i];
    }
  }

  void dataSymbol() {
    float angle[128], weight[128];
    float symbol_power = 0;
    int dibits[128];
    bool is_header = frame_len < 0;
    // header: 0 or 180 degrees, data: 0, 90, 180 or 270 degrees
    float step = is_header ? (float)M_PI : (float)M_PI / 2;
    for (int i = 0; i < ncarriers; i++) {
      symbol_power += z_re[i] * z_re[i] + z_im[i] * z_im[i];
      float dr = z_re[i] * prev_re[i] + z_im[i] * prev_im[i];
      float di = z_im[i] * prev_re[i] - z_re[i] * prev_im[i];
      float a = wrap(atan2f(di, dr) - pred_common -
                     (pred_slope + slope_adjust) * i);
      int q = lroundf(a / step);
      angle[i] = a - q * step;  // residual error
      weight[i] = sqrtf(dr * dr + di * di);
      if (is_header) {
        // soft decision: sum of all copies of the header bit
        header_soft[(header_symbol * ncarriers + i) % 32] += cosf(a);
      } else {
        dibits[i] = phaseToGray(q & 3);
      }
      prev_re[i] = z_re[i];
      prev_im[i] = z_im[i];
      error_total += fabsf(angle[i]);
      error_count++;
    }
    // the signal is lost
    if (symbol_power / ncarriers < 0.1f * reference_power) {
      endFrame(false);
      return;
    }
    // track the frequency offset and the timing drift
    float common, slope;
    fit(angle, weight, ncarriers, common, slope);
    pred_common += TRACKING_GAIN * common;
    pred_slope += TRACKING_GAIN * slope;
    // timing drift in samples per symbol
    drift -= pred_slope * fft_size / (2.0f * (float)M_PI);

    if (is_header) {
      if (++header_symbol == OFDMConfig::headerSymbols(ncarriers))
        checkHeader();
      return;
    }
    for (int i = 0; i < ncarriers && state == State::FRAME; i++)
      dataBits(dibits[i]);
  }

  /// 0 -> 00, 90 -> 01, 180 -> 11, 270 -> 10 degrees
  static int phaseToGray(int phase) {
    static const int dibit[4] = {0, 1, 3, 2};
    return dibit[phase];
  }

  void checkHeader() {
    uint32_t header = 0;
    for (int b = 0; b < 32; b++) header = (header << 1) | (header_soft[b] < 0);
    uint16_t len = header >> 16;
    uint16_t check = header & 0xFFFF;
    if ((uint16_t)~len != check || len == 0) {
      // not a valid frame: search again
      state = State::SEARCH;
      in_plateau = false;
      return;
    }
    frame_len = len;
    status("### CARRIER ofdm%d %.1f-%.1f Hz len=%d ###\n", ncarriers,
           (double)cfg.carrierFrequency(0),
           (double)cfg.carrierFrequency(ncarriers - 1), frame_len);
  }

  void dataBits(int dibit) {
    current = (current << 2) | dibit;
    if (++current_dibits == 4) {
      if (p_print != nullptr) p_print->write(current);
      ndata++;
      current = 0;
      current_dibits = 0;
      if (ndata == frame_len) endFrame(true);
    }
  }

  void endFrame(bool complete) {
    state = State::SEARCH;
    in_plateau = false;
    if (frame_len < 0) return;  // no valid header
    if (complete) frames++;
    float error = error_count > 0 ? error_total / error_count : 0;
    status("\n### NOCARRIER ndata=%d phase_error=%.1f deg%s ###\n", ndata,
           (double)(error * 180.0f / (float)M_PI),
           complete ? "" : " (incomplete)");
    frame_len = -1;
    if (cfg.rx_one) is_finished = true;
  }
};

}  // namespace tinymodem
