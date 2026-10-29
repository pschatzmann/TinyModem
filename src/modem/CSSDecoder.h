#pragma once

#include <math.h>
#include <string.h>

#include "AudioTools.h"
#include "CSS.h"
#include "DSPUtils.h"
#include "ModemBase.h"

namespace tinymodem {

/**
 * @brief Chirp spread spectrum demodulator (LoRa-like): the PCM audio data is
 * written to the decoder and the payload of the received frames with a
 * correct CRC is written to the output. The input can be 8, 16 or 32 bit
 * signed samples with any number of channels (only CSSConfig::channel is
 * evaluated).
 *
 * The signal is mixed to the complex baseband and resampled to the bandwidth,
 * so that a symbol has N = 2^SF samples. A symbol is multiplied with the
 * conjugate chirp (dechirped): the result is a tone, whose FFT bin is the
 * symbol value. The preamble is detected by the same bin in consecutive
 * windows; the bins of the up and down chirps provide the time and frequency
 * offset. During the frame the timing follows a different clock of the
 * sender.
 *
 * The receiver must use the same ecc_bytes as the transmitter.
 * @copyright GPLv3
 */
class CSSDecoder : public ModemDecoder {
 public:
  CSSDecoder() = default;
  CSSDecoder(Print& out) { setOutput(out); }

  /// Provides a default configuration: css7 (SF 7, 1000 Hz)
  CSSConfig defaultConfig() { return CSSConfig(); }

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
  bool begin(CSSConfig config) {
    cfg = config;
    return begin();
  }

  bool begin() override {
    is_active = false;
    if (!cfg.isValid()) return false;
    if (!input.begin(cfg.bits_per_sample, cfg.channels, cfg.channel))
      return false;
    n = cfg.chips();
    m = OVERSAMPLING * n;
    if (!fft.begin(m)) return false;
    // baseband with twice the bandwidth as sample rate
    converter.begin(cfg.sample_rate, OVERSAMPLING * cfg.bandwidth,
                    0.5f / OVERSAMPLING);
    double w = 2.0 * M_PI * cfg.center_freq / cfg.sample_rate;
    mix_step_re = cos(w);
    mix_step_im = -sin(w);
    ring_re.resize(RING * m);
    ring_im.resize(RING * m);
    fft_re.resize(m);
    fft_im.resize(m);
    power.resize(m);
    chirp_re.resize(m);
    chirp_im.resize(m);
    for (int k = 0; k < m; k++) {
      // baseband up chirp from -B/2 to B/2: the phase is the integral of
      // the frequency
      double u = (double)k / m;  // 0 - 1 within the symbol
      double ph = 2.0 * M_PI * n * (0.5 * u * u - 0.5 * u);
      chirp_re[k] = cos(ph);
      chirp_im[k] = sin(ph);
    }
    symbols.resize(CSSFrame::symbols(
        CSSFrame::payloadBytes(CSSConfig::MAX_FRAME, RSCodec::MAX_ECC), 5));
    frames = errors = 0;
    resetState();
    is_active = true;
    return true;
  }

  /// Processes the remaining samples and reports the statistics
  void end() override {
    if (!is_active) return;
    flush();
    status("### CSS frames=%u errors=%u ###\n", (unsigned)frames,
           (unsigned)errors);
    is_active = false;
  }

  /// Processes the remaining samples (e.g. at the end of the input)
  void flush() override {
    if (!is_active) return;
    // silence: the last symbol and the delay of the filters
    size_t len = (size_t)((2.0 * n + 64) / cfg.bandwidth * cfg.sample_rate);
    for (size_t j = 0; j < len && !is_finished; j++) addSample(0.0f);
    if (state >= HEADER) {
      errors++;
      endOfCarrier(false);
    }
    bool finished = is_finished;
    resetState();
    is_finished = finished;
  }

  /// Provide PCM data to the decoder
  size_t write(const uint8_t* data, size_t len) override {
    if (!is_active) return 0;
    input.write(data, len, [this](float x) {
      if (cfg.rx_noise != 0.0f) x += noise() * cfg.rx_noise * 2.0f;
      addSample(x);
    });
    return len;
  }

  /// true while a frame is received
  bool isCarrier() override { return state >= HEADER; }

  /// true if the decoding was stopped (rx_one)
  bool isFinished() override { return is_finished; }

  /// Number of received frames with a correct CRC
  size_t frameCount() { return frames; }

  /// Number of frames which could not be decoded
  size_t errorCount() { return errors; }

  /// Frequency offset of the last frame in Hz
  float frequencyOffset() { return freq_offset * cfg.bandwidth / n; }

  /// The configuration which was provided in begin()
  CSSConfig& config() { return cfg; }

  operator bool() override { return is_active; }

 protected:
  enum State { SEARCH, SYNC, HEADER, PAYLOAD };
  static constexpr int RING = 4;            // buffered symbols
  /// baseband samples per chip: the two parts of a chirp (before and after
  /// the wrap around) are separate tones, which makes the detection robust
  /// against a fractional time offset
  static constexpr int OVERSAMPLING = 2;
  static constexpr int PREAMBLE_HITS = 3;   // windows with the same bin
  /// tracking of the remaining offset per symbol
  static constexpr float TRACKING_GAIN = 0.3f;
  /// tracking of the remaining drift per symbol
  static constexpr float DRIFT_GAIN = 0.02f;
  /// min drift in bins per symbol for the correction of the sample rate
  static constexpr float MIN_DRIFT = 0.08f;
  /// max preamble windows for the drift estimation
  static constexpr int MAX_UPS = 16;

  /// Peak of a spectrum
  struct Peak {
    int bin = 0;
    float pos = 0;    // with fractional part
    float power = 0;  // power of the peak bin
    float ratio = 0;  // peak power / mean power of the other bins
  };

  CSSConfig cfg;
  bool is_active = false;
  bool is_finished = false;
  PcmInput input;
  uint32_t noise_seed = 1;
  int n = 128;  // chips (bins) per symbol
  int m = 256;  // baseband samples per symbol
  SimpleFFT fft;
  SampleRateConverter converter;  // complex baseband
  float mix_re = 1, mix_im = 0, mix_step_re = 1, mix_step_im = 0;
  uint32_t mix_count = 0;
  audio_tools::Vector<float> ring_re, ring_im, fft_re, fft_im, power;
  audio_tools::Vector<float> chirp_re, chirp_im;
  int64_t count = 0;       // number of baseband samples
  int64_t next_start = 0;  // start of the next window
  // receiver state
  State state = SEARCH;
  int hits = 0, candidate = 0;
  int sync_windows = 0;
  float last_up = 0;      // signed peak of the last preamble window
  float ups[MAX_UPS];     // peaks of the preamble windows after the alignment
  int nups = 0;
  float drift = 0;        // change of the peak per symbol (different clock)
  float rate_factor = 1;  // clock of the sender relative to our clock
  float sync_offset = 0;  // frequency offset of the sync in bins
  float freq_offset = 0;  // in bins
  audio_tools::Vector<uint16_t> symbols;
  int nsymbols = 0, needed = 0, frame_len = 0;
  float snr_total = 0;
  size_t frames = 0, errors = 0;

  void resetState() {
    state = SEARCH;
    hits = 0;
    count = 0;
    next_start = 0;
    mix_re = 1;
    mix_im = 0;
    converter.reset();
    is_finished = false;
    input.reset();
  }

  /// uniform random value in the range -0.5 to 0.5
  float noise() {
    noise_seed = noise_seed * 1103515245u + 12345u;
    return ((noise_seed >> 8) & 0xFFFF) / 65535.0f - 0.5f;
  }

  /// Mixes a sample to the baseband and resamples it to the bandwidth
  void addSample(float x) {
    if (is_finished) return;
    float i = x * mix_re, q = x * mix_im;
    float r = mix_re * mix_step_re - mix_im * mix_step_im;
    mix_im = mix_re * mix_step_im + mix_im * mix_step_re;
    mix_re = r;
    if (++mix_count % 1024 == 0) {
      float mag = sqrtf(mix_re * mix_re + mix_im * mix_im);
      mix_re /= mag;
      mix_im /= mag;
    }
    converter.writeComplex(i, q, [this](float re, float im) {
      addBaseband(re, im);
    });
  }

  void addBaseband(float re, float im) {
    int idx = count % (RING * m);
    // factor 2: the mixer provides half of the amplitude
    ring_re[idx] = 2 * re;
    ring_im[idx] = 2 * im;
    count++;
    while (!is_finished && count >= next_start + m) processWindow();
  }

  /// Dechirped spectrum of the window at next_start: up or down chirp,
  /// shifted by the frequency offset (in bins). The power of both parts of
  /// the chirp is added, which is robust against any time offset.
  Peak spectrum(bool up, float offset) {
    double w = -2.0 * M_PI * offset / m;
    float rot_re = cos(w), rot_im = sin(w);
    float c_re = 1, c_im = 0;
    for (int k = 0; k < m; k++) {
      int idx = (next_start + k) % (RING * m);
      float r_re = ring_re[idx], r_im = ring_im[idx];
      // up: multiply with conj(chirp), down: with chirp
      float ch_re = chirp_re[k], ch_im = up ? -chirp_im[k] : chirp_im[k];
      float y_re = r_re * ch_re - r_im * ch_im;
      float y_im = r_re * ch_im + r_im * ch_re;
      fft_re[k] = y_re * c_re - y_im * c_im;
      fft_im[k] = y_re * c_im + y_im * c_re;
      float t = c_re * rot_re - c_im * rot_im;
      c_im = c_re * rot_im + c_im * rot_re;
      c_re = t;
    }
    fft.process(fft_re.data(), fft_im.data());
    for (int k = 0; k < m; k++)
      power[k] = fft_re[k] * fft_re[k] + fft_im[k] * fft_im[k];
    // the part after the wrap around is a tone n bins lower: we add both
    Peak result;
    float total = 0, best = 0;
    for (int k = 0; k < n; k++) {
      float p = power[k] + power[k + n];
      total += p;
      if (p > best) {
        best = p;
        result.bin = k;
      }
    }
    result.power = best;
    float mean = (total - best) / (n - 1);
    result.ratio = mean > 0 ? best / mean : 0;
    // fractional position of the stronger tone (Quinn's first estimator,
    // which is exact for a rectangular window)
    int k = power[result.bin] >= power[result.bin + n] ? result.bin
                                                       : result.bin + n;
    int l = (k + m - 1) % m, r = (k + 1) % m;
    float pk = power[k];
    float delta = 0;
    if (pk > 0) {
      float a1 = (fft_re[l] * fft_re[k] + fft_im[l] * fft_im[k]) / pk;
      float a2 = (fft_re[r] * fft_re[k] + fft_im[r] * fft_im[k]) / pk;
      float d1 = a1 / (1.0f - a1), d2 = -a2 / (1.0f - a2);
      delta = (d1 > 0 && d2 > 0) ? d2 : d1;
      if (delta > 0.5f) delta = 0.5f;
      if (delta < -0.5f) delta = -0.5f;
    }
    result.pos = result.bin + delta;
    return result;
  }

  /// signed value of a bin (-n/2 to n/2)
  float signedBin(float pos) {
    while (pos > n / 2) pos -= n;
    while (pos <= -n / 2) pos += n;
    return pos;
  }

  void processWindow() {
    switch (state) {
      case SEARCH:
        search();
        break;
      case SYNC:
        sync();
        break;
      default:
        data();
        break;
    }
  }

  /// Preamble: the same bin in consecutive windows
  void search() {
    Peak up = spectrum(true, 0);
    if (up.ratio >= cfg.detect_threshold) {
      // the bin may move a little with a different clock of the sender
      int dist = abs((int)signedBin(up.bin - candidate));
      hits = hits > 0 && dist <= 2 ? hits + 1 : 1;
      candidate = up.bin;
    } else {
      hits = 0;
    }
    if (hits >= PREAMBLE_HITS) {
      // the window start moves to the symbol start (plus the frequency
      // offset, which is determined with the down chirps)
      int shift = -lroundf(OVERSAMPLING * signedBin(up.pos));
      next_start += m + shift;
      state = SYNC;
      nups = 0;
      sync_windows = 0;
      last_up = 0;
      hits = 0;
      return;
    }
    next_start += m;
  }

  /// Waits for the down chirps: up peak = tau + offset, down peak = -tau +
  /// offset
  void sync() {
    Peak up = spectrum(true, 0);
    Peak down = spectrum(false, 0);
    if (down.power > up.power && down.ratio >= cfg.detect_threshold) {
      // a different clock of the sender changes the timing by drift per
      // symbol: up = tau + offset, down = -(tau + drift) + offset
      drift = estimateDrift();
      float p_down = signedBin(down.pos);
      float tau = (last_up - p_down - drift) / 2;
      float offset = (last_up + p_down + drift) / 2;
      // timing of the first down chirp
      tau += drift;
      // a clear drift is caused by a different clock: from now on we sample
      // with the clock of the sender, which also corrects the chirp rate. A
      // small drift is followed by the tracking.
      rate_factor = fabsf(drift) > MIN_DRIFT ? 1.0f + drift / n : 1.0f;
      converter.setRateFactor(rate_factor);
      drift = 0;
      // the window moves by whole samples, the rest is part of the frequency
      // offset (a fractional time offset is harmless with the oversampling)
      int shift = lroundf(OVERSAMPLING * tau);
      freq_offset = offset + (OVERSAMPLING * tau - shift) / OVERSAMPLING;
      sync_offset = freq_offset;
      // skip the second down chirp
      next_start += 2 * m - shift;
      nsymbols = 0;
      needed = CSSFrame::symbols(CSSFrame::HEADER_BYTES, cfg.spreading_factor);
      snr_total = 0;
      state = HEADER;
      status("### CARRIER css sf%d %.0f Hz @ %.1f Hz ###\n",
             cfg.spreading_factor, (double)cfg.bandwidth,
             (double)(cfg.center_freq + frequencyOffset()));
      return;
    }
    last_up = signedBin(up.pos);
    if (nups < MAX_UPS) ups[nups++] = last_up;
    next_start += m;
    if (++sync_windows > cfg.preamble_symbols + 4 ||
        up.ratio < cfg.detect_threshold) {
      state = SEARCH;
      hits = 0;
    }
  }

  /// Change of the preamble peaks per symbol (least squares fit)
  float estimateDrift() {
    if (nups < 2) return 0;
    float sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int j = 0; j < nups; j++) {
      sx += j;
      sy += ups[j];
      sxx += (float)j * j;
      sxy += j * ups[j];
    }
    float d = nups * sxx - sx * sx;
    return d > 0 ? (nups * sxy - sx * sy) / d : 0;
  }

  /// Header and payload symbols
  void data() {
    Peak peak = spectrum(true, freq_offset);
    int value = peak.bin;
    snr_total += peak.ratio;
    // second order loop: the remaining offset of the peak (frequency error
    // and the timing drift of a remaining clock difference) is corrected in
    // the frequency domain. With the oversampling a small time offset of the
    // window only costs a little energy.
    float error = signedBin(peak.pos - value);
    freq_offset += TRACKING_GAIN * error + drift;
    drift += DRIFT_GAIN * error;
    // a change of a whole bin since the sync is caused by a timing drift: we
    // move the window by one chip, which moves the peak by one bin
    int shift = 0;
    if (freq_offset - sync_offset > 1.0f) shift = 1;
    if (freq_offset - sync_offset < -1.0f) shift = -1;
    freq_offset -= shift;
    next_start += m - OVERSAMPLING * shift;
    symbols[nsymbols++] = value;
    if (nsymbols < needed) return;

    int sf = cfg.spreading_factor;
    if (state == HEADER) {
      uint8_t header[CSSFrame::HEADER_BYTES];
      CSSFrame::fromSymbols(symbols.data(), nsymbols, sf, header,
                            sizeof(header));
      frame_len = CSSFrame::decodeHeader(header);
      if (frame_len < 0) {
        errors++;
        endOfCarrier(false);
        return;
      }
      state = PAYLOAD;
      nsymbols = 0;
      needed = CSSFrame::symbols(
          CSSFrame::payloadBytes(frame_len, cfg.ecc_bytes), sf);
      return;
    }
    uint8_t payload[CSSConfig::MAX_FRAME + 2 + RSCodec::MAX_ECC];
    int len = CSSFrame::payloadBytes(frame_len, cfg.ecc_bytes);
    CSSFrame::fromSymbols(symbols.data(), nsymbols, sf, payload, len);
    int corrected = CSSFrame::decodePayload(payload, frame_len, cfg.ecc_bytes);
    if (corrected < 0) {
      errors++;
      endOfCarrier(false);
      return;
    }
    frames++;
    if (p_print != nullptr) p_print->write(payload, frame_len);
    endOfCarrier(true, corrected);
  }

  void endOfCarrier(bool ok, int corrected = 0) {
    rate_factor = 1;
    converter.setRateFactor(1);
    int total = nsymbols + (state == PAYLOAD
                                ? CSSFrame::symbols(CSSFrame::HEADER_BYTES,
                                                    cfg.spreading_factor)
                                : 0);
    float snr = total > 0 ? 10 * log10f(snr_total / total) : 0;
    if (ok)
      status("\n### NOCARRIER ndata=%d corrected=%d snr=%.1f dB ###\n",
             frame_len, corrected, (double)snr);
    else
      status("\n### NOCARRIER frame error snr=%.1f dB ###\n", (double)snr);
    state = SEARCH;
    hits = 0;
    if (cfg.rx_one) is_finished = true;
  }
};

}  // namespace tinymodem
