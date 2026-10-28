#pragma once

#include <math.h>
#include <string.h>

#include "AudioTools.h"
#include "AudioTools/CoreAudio/Analysis/GoerzelStream.h"
#include "MFSKConfig.h"
#include "MFSKEncoder.h"
#include "ModemBase.h"

namespace tinymodem {

/**
 * @brief Multi-tone FSK demodulator: PCM audio data is written to the
 * decoder and the decoded bytes are written to the output. The input can be
 * 8, 16 or 32 bit signed samples with any number of channels (only
 * MFSKConfig::channel is evaluated).
 *
 * The decoder searches for the preamble, determines the symbol timing from
 * the alternating preamble tones and waits for the sync symbols. Then each
 * symbol is analyzed with one Goertzel filter per tone. The timing is
 * tracked with an early/late gate, so small differences of the sample rates
 * are corrected. The end of a transmission is detected when the signal is
 * lost.
 *
 * Status messages (### CARRIER and ### NOCARRIER) are written to the status
 * output.
 * @copyright GPLv3
 */
class MFSKDecoder : public ModemDecoder {
 public:
  MFSKDecoder() = default;
  MFSKDecoder(Print& out) { setOutput(out); }

  /// Provides a default configuration: mfsk16
  MFSKConfig defaultConfig() {
    MFSKConfig result;
    result.setMode("mfsk16");
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
  bool begin(MFSKConfig config) {
    cfg = config;
    return begin();
  }

  bool begin() override {
    is_active = false;
    if (!cfg.isValid()) return false;
    if (!input.begin(cfg.bits_per_sample, cfg.channels, cfg.channel))
      return false;
    ntones = cfg.tones;
    bits_per_symbol = cfg.bitsPerSymbol();
    detectors.resize(ntones);
    magnitudes.resize(ntones);
    tune(1.0f);
    // the frequency estimation is only needed (and reliable) if the clock
    // error can shift a tone by a relevant part of the tone spacing
    estimate_clock = cfg.toneFrequency(ntones - 1) * MAX_CLOCK_ERROR >
                     cfg.spacing() / 4;
    search_detectors.resize(estimate_clock ? 2 * SEARCH_STEPS + 1 : 0);
    for (int k = 0; k < (int)search_detectors.size(); k++) {
      float error = (k - SEARCH_STEPS) * SEARCH_STEP;
      setupDetector(search_detectors[k], cfg.toneFrequency(0) * (1.0f + error));
    }

    // room for the preamble acquisition and a symbol with the timing gate
    float max_nsamples = nsamples * (1.0f + MAX_CLOCK_ERROR);
    size_t size = (ACQUIRE_SYMBOLS + 4) * max_nsamples + 2;
    samples.resize(size);
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

  /// Processes all buffered samples (e.g. at the end of the input)
  void flush() override {
    if (!is_active) return;
    // feed silence, so that the end of the transmission is detected
    size_t n = samples.size();
    for (size_t j = 0; j < n && !is_finished; j++) addSample(0.0f, false);
    if (state == State::DATA) endOfCarrier();
    // with rx_one the decoding stays stopped until begin()
    bool finished = is_finished;
    resetState();
    is_finished = finished;
  }

  /// Provide PCM data to the decoder
  size_t write(const uint8_t* data, size_t len) override {
    if (!is_active) return 0;
    input.write(data, len, [this](float sample) { addSample(sample, true); });
    return len;
  }

  /// true while a transmission is received
  bool isCarrier() override { return state == State::DATA; }

  /// true if the decoding was stopped (rx_one)
  bool isFinished() override { return is_finished; }

  /// The configuration which was provided in begin()
  MFSKConfig& config() { return cfg; }

  operator bool() override { return is_active; }

 protected:
  enum class State { SEARCH, PREAMBLE, DATA };
  /// number of preamble symbols which are used for the timing acquisition
  static constexpr int ACQUIRE_SYMBOLS = 4;
  /// max number of preamble symbols before we give up
  static constexpr int MAX_PREAMBLE = 64;
  /// max difference of the sample rates of sender and receiver
  static constexpr float MAX_CLOCK_ERROR = 0.03f;
  static constexpr float CLOCK_ERROR_STEP = 0.001f;
  /// a different clock must provide this much more energy
  static constexpr float MIN_CLOCK_GAIN = 1.2f;
  /// gains of the timing loop
  static constexpr float TIMING_GAIN = 0.25f;
  static constexpr float DRIFT_GAIN = 0.05f;
  /// a symbol below this part of the tracked amplitude is lost
  static constexpr float LOST_LEVEL = 0.4f;
  /// a preamble symbol above this multiple of the tracked amplitude starts a
  /// new search
  static constexpr float NEW_SIGNAL_LEVEL = 4.0f;
  /// the tone of a preamble or sync symbol must be this much stronger than
  /// any other tone
  static constexpr float CLEAR_RATIO = 1.5f;
  /// the highest preamble tone must have this part of the lowest tone's
  /// amplitude (a window may only partly overlap a symbol)
  static constexpr float HIGH_TONE_LEVEL = 0.25f;
  /// the confidence of a symbol is limited to this value
  static constexpr float MAX_CONFIDENCE = 100.0f;
  /// the search checks the clock errors -3% to +3% in steps of 1%
  static constexpr int SEARCH_STEPS = 3;
  static constexpr float SEARCH_STEP = 0.01f;

  MFSKConfig cfg;
  bool is_active = false;
  bool is_finished = false;
  PcmInput input;
  audio_tools::Vector<audio_tools::GoertzelDetector> detectors;
  audio_tools::Vector<audio_tools::GoertzelDetector> search_detectors;
  audio_tools::Vector<float> magnitudes;
  audio_tools::Vector<float> samples;
  size_t nvalid = 0;
  size_t needed = 0;
  int ntones = 16;
  int bits_per_symbol = 4;
  float nsamples = 0;  // samples per symbol
  int window = 0;      // analysis window
  int delta = 1;       // offset of the early/late gate
  int acquire_step = 1;
  int hop = 1;
  uint32_t noise_seed = 1;
  /// frequency scale which results from the different sample rates
  float scale = 1.0f;
  /// learned timing drift in samples per symbol
  float drift = 0.0f;
  /// true if the clock of the sender is determined from the tones
  bool estimate_clock = true;

  // receiver state
  State state = State::SEARCH;
  float pos = 0;  // start of the next symbol in samples
  float track_amplitude = 0;
  int preamble_count = 0;
  int sync_stage = 0;
  bool has_pending = false;
  int pending_symbol = 0;
  uint32_t bit_buffer = 0;
  int bit_count = 0;
  // statistics
  unsigned ndata = 0;
  unsigned nsymbols = 0;
  float confidence_total = 0;
  float amplitude_total = 0;
  double data_nsamples = 0;

  /// Adapts the tones and the symbol length to the clock of the sender: a
  /// sender with a faster sample rate results in lower frequencies (scale
  /// < 1.0) and longer symbols.
  void tune(float new_scale) {
    scale = new_scale;
    nsamples = cfg.samplesPerSymbol() / scale;
    window = lroundf(nsamples);
    delta = lroundf(nsamples / 8);
    if (delta < 1) delta = 1;
    acquire_step = window / 16;
    if (acquire_step < 1) acquire_step = 1;
    hop = window / 2;
    for (int j = 0; j < ntones; j++)
      setupDetector(detectors[j], cfg.toneFrequency(j) * scale);
  }

  void setupDetector(audio_tools::GoertzelDetector& det, float freq) {
    audio_tools::GoertzelConfig gcfg;
    gcfg.sample_rate = cfg.sample_rate;
    gcfg.channels = 1;
    gcfg.target_frequency = freq;
    gcfg.block_size = window;
    det.begin(gcfg);
  }

  /// Back to the search for the next preamble
  void startSearch() {
    state = State::SEARCH;
    if (scale != 1.0f) tune(1.0f);
  }

  void resetState() {
    nvalid = 0;
    needed = 0;
    startSearch();
    pos = 0;
    is_finished = false;
    input.reset();
  }

  /// uniform random value in the range -0.5 to 0.5
  float noise() {
    noise_seed = noise_seed * 1103515245u + 12345u;
    return ((noise_seed >> 8) & 0xFFFF) / 65535.0f - 0.5f;
  }

  void addSample(float sample, bool add_noise) {
    if (is_finished) return;
    if (add_noise && cfg.rx_noise != 0.0f)
      sample += noise() * cfg.rx_noise * 2.0f;
    if (nvalid == samples.size()) discard(hop);  // never overflow
    samples[nvalid++] = sample;
    if (nvalid >= needed || nvalid == samples.size()) {
      while (!is_finished && step()) {
      }
    }
  }

  /// Removes the processed samples at the start of the buffer
  void discard(size_t n) {
    if (n == 0) return;
    if (n > nvalid) n = nvalid;
    memmove(samples.data(), samples.data() + n,
            (nvalid - n) * sizeof(float));
    nvalid -= n;
    pos -= n;
  }

  /// Magnitude (amplitude) of the tone in the window starting at start
  float magnitude(int tone, int start) {
    return magnitude(detectors[tone], start);
  }

  /// Magnitude (amplitude) of the detector's frequency in the window
  float magnitude(audio_tools::GoertzelDetector& det, int start) {
    det.reset();
    const float* data = samples.data() + start;
    for (int j = 0; j < window; j++) det.processSample(data[j]);
    return det.getMagnitude() * 2.0f / window;
  }

  /// Amplitude of a sine with the same energy as the window
  float windowAmplitude(int start) {
    const float* data = samples.data() + start;
    float sum = 0;
    for (int j = 0; j < window; j++) sum += data[j] * data[j];
    return sqrtf(2.0f * sum / window);
  }

  /// One processing step: returns false if more data is needed
  bool step() {
    return state == State::SEARCH ? searchStep() : symbolStep();
  }

  /// Searches for the preamble at the start of the buffer
  bool searchStep() {
    size_t need = (size_t)((ACQUIRE_SYMBOLS + 1) * nsamples) + window + 1;
    if (nvalid < need) {
      needed = need;
      return false;
    }
    // quick check for silence
    float rms_amplitude = windowAmplitude(0);
    if (rms_amplitude < cfg.min_amplitude) {
      discard(hop);
      return true;
    }
    // look for the lowest preamble tone with all possible clock errors
    float best = 0, best_error = 0, nominal = 0;
    if (!estimate_clock) best = nominal = magnitude(0, 0);
    for (int k = 0; k < (int)search_detectors.size(); k++) {
      float a = magnitude(search_detectors[k], 0);
      if (k == SEARCH_STEPS) nominal = a;
      if (a > best) {
        best = a;
        best_error = (k - SEARCH_STEPS) * SEARCH_STEP;
      }
    }
    // only assume a clock error if this is clearly better
    if (best < MIN_CLOCK_GAIN * nominal) best_error = 0;
    if (best >= cfg.min_amplitude && best > 0.5f * rms_amplitude &&
        hasHighTone(best_error, best) && acquire(best_error)) {
      state = State::PREAMBLE;
      preamble_count = 0;
      sync_stage = 0;
      return true;
    }
    discard(hop);
    return true;
  }

  /// Quick check before the (expensive) acquisition: the preamble alternates
  /// the lowest and the highest tone, so the highest tone must be present in
  /// one of the next symbols. This rejects a steady tone (e.g. a carrier at
  /// the lowest tone), which would otherwise start an acquisition at each
  /// step.
  bool hasHighTone(float clock_error, float low) {
    audio_tools::GoertzelDetector det;
    setupDetector(det, cfg.toneFrequency(ntones - 1) * (1.0f + clock_error));
    for (int k = 0; k <= ACQUIRE_SYMBOLS; k++) {
      if (magnitude(det, lroundf(k * nsamples)) >= HIGH_TONE_LEVEL * low)
        return true;
    }
    return false;
  }

  /// Determines the symbol timing and the clock of the sender from the
  /// alternating preamble tones: the coarse clock error is known from the
  /// search
  bool acquire(float coarse_error) {
    if (coarse_error != 0.0f) tune(1.0f + coarse_error);
    int offset = findTiming();
    if (offset >= 0 && estimate_clock && estimateClock(offset)) {
      // the timing changes with the clock
      offset = findTiming();
    }
    if (offset < 0) {
      startSearch();
      return false;
    }
    // the tones must alternate and be clearly distinguishable
    float amplitude = 0, other = 0;
    int last = -1;
    for (int k = 0; k < ACQUIRE_SYMBOLS; k++) {
      int start = lroundf(offset + k * nsamples);
      float a0 = magnitude(0, start);
      float a1 = magnitude(ntones - 1, start);
      int tone = a0 > a1 ? 0 : 1;
      if (tone == last) {
        startSearch();
        return false;
      }
      last = tone;
      amplitude += a0 > a1 ? a0 : a1;
      other += a0 > a1 ? a1 : a0;
    }
    amplitude /= ACQUIRE_SYMBOLS;
    other /= ACQUIRE_SYMBOLS;
    if (amplitude < cfg.min_amplitude || other > 0.5f * amplitude) {
      startSearch();
      return false;
    }
    pos = offset;
    drift = 0.0f;
    track_amplitude = amplitude;
    return true;
  }

  /// Finds the offset where the preamble tones are best separated: returns
  /// -1 if there is no signal
  int findTiming() {
    int best_offset = -1;
    float best_score = 0;
    for (int offset = 0; offset < (int)nsamples; offset += acquire_step) {
      float score = 0;
      for (int k = 0; k < ACQUIRE_SYMBOLS; k++) {
        int start = lroundf(offset + k * nsamples);
        score += fabsf(magnitude(0, start) - magnitude(ntones - 1, start));
      }
      if (score > best_score) {
        best_score = score;
        best_offset = offset;
      }
    }
    return best_offset;
  }

  /// Determines the exact frequency scale from the highest preamble tone
  /// and adapts the receiver to it: returns true if the receiver was retuned
  bool estimateClock(int offset) {
    // the windows with the highest tone have little energy in the lowest
    int starts[ACQUIRE_SYMBOLS];
    float low[ACQUIRE_SYMBOLS];
    float max_low = 0;
    for (int k = 0; k < ACQUIRE_SYMBOLS; k++) {
      starts[k] = lroundf(offset + k * nsamples);
      low[k] = magnitude(0, starts[k]);
      if (low[k] > max_low) max_low = low[k];
    }
    audio_tools::GoertzelDetector det;
    float high = cfg.toneFrequency(ntones - 1);
    float current_error = scale - 1.0f;
    float best_error = current_error, best_mag = -1, current_mag = 0;
    int steps = lroundf(MAX_CLOCK_ERROR / CLOCK_ERROR_STEP);
    for (int step = -steps; step <= steps + 1; step++) {
      // the last step measures the current setting
      bool is_current = step == steps + 1;
      float error = is_current ? current_error : step * CLOCK_ERROR_STEP;
      setupDetector(det, high * (1.0f + error));
      float sum = 0;
      for (int k = 0; k < ACQUIRE_SYMBOLS; k++)
        if (low[k] < 0.5f * max_low) sum += magnitude(det, starts[k]);
      if (is_current) {
        current_mag = sum;
      } else if (sum > best_mag) {
        best_mag = sum;
        best_error = error;
      }
    }
    // only retune if this is clearly better than the current setting
    if (best_mag < MIN_CLOCK_GAIN * current_mag) return false;
    tune(1.0f + best_error);
    return true;
  }

  /// Analyzes the next symbol
  bool symbolStep() {
    int p = lroundf(pos);
    if (p < delta) p = delta;  // room for the early gate
    size_t need = p + delta + window + 1;
    if (nvalid < need) {
      needed = need;
      return false;
    }

    // find the strongest and the second strongest tone
    int best = 0;
    for (int j = 0; j < ntones; j++) {
      magnitudes[j] = magnitude(j, p);
      if (magnitudes[j] > magnitudes[best]) best = j;
    }
    float second = 0;
    for (int j = 0; j < ntones; j++)
      if (j != best && magnitudes[j] > second) second = magnitudes[j];
    float amplitude = magnitudes[best];
    bool strong = amplitude >= cfg.min_amplitude &&
                  amplitude >= LOST_LEVEL * track_amplitude;

    // early/late gate: if the symbol starts tau samples later than expected,
    // the early window contains window - delta - tau and the late window
    // window - delta + tau samples of the tone, so we can estimate tau. A
    // second order loop corrects a part of tau and learns the drift, which
    // results from different clocks of the sender and the receiver.
    float correction = drift;
    if (strong) {
      float early = magnitude(best, p - delta);
      float late = magnitude(best, p + delta);
      if (early + late > 0) {
        float tau = (window - delta) * (late - early) / (late + early);
        drift += DRIFT_GAIN * tau;
        float max_drift = MAX_CLOCK_ERROR * nsamples;
        if (drift > max_drift) drift = max_drift;
        if (drift < -max_drift) drift = -max_drift;
        correction = drift + TIMING_GAIN * tau;
      }
    }
    if (correction > delta) correction = delta;
    if (correction < -delta) correction = -delta;
    pos += nsamples + correction;

    if (state == State::PREAMBLE)
      preambleSymbol(best, amplitude, strong && amplitude >= CLEAR_RATIO * second);
    else
      dataSymbol(best, amplitude, second, strong, nsamples + correction);

    // keep the samples which are needed for the early gate
    int keep_from = lroundf(pos) - delta;
    if (state == State::SEARCH) keep_from = p;
    if (keep_from > 0) discard(keep_from);
    return true;
  }

  void preambleSymbol(int symbol, float amplitude, bool strong) {
    // a much stronger signal: we were locked to noise, so we search again
    if (amplitude > NEW_SIGNAL_LEVEL * track_amplitude) {
      startSearch();
      return;
    }
    // the preamble and sync symbols must be clear (strong and unambiguous)
    bool ok = strong;
    if (ok && sync_stage == 0 && (symbol == 0 || symbol == ntones - 1)) {
      track_amplitude = (track_amplitude + amplitude) / 2;
      ok = ++preamble_count < MAX_PREAMBLE;
    } else if (ok && symbol == cfg.syncSymbol(sync_stage)) {
      if (++sync_stage == cfg.syncLength()) startOfCarrier();
    } else {
      ok = false;
    }
    if (!ok) startSearch();
  }

  void dataSymbol(int symbol, float amplitude, float second, bool strong,
                  float symbol_nsamples) {
    if (!strong) {
      // the first weak symbol might be a fade: we decide with the next one
      if (has_pending) {
        endOfCarrier();
        return;
      }
      has_pending = true;
      pending_symbol = symbol;
      return;
    }
    if (has_pending) {
      outputSymbol(pending_symbol);
      has_pending = false;
    }
    outputSymbol(symbol);
    track_amplitude = (track_amplitude + amplitude) / 2;
    float confidence = second > 0 ? amplitude / second : MAX_CONFIDENCE;
    confidence_total += confidence < MAX_CONFIDENCE ? confidence : MAX_CONFIDENCE;
    amplitude_total += amplitude;
    nsymbols++;
    data_nsamples += symbol_nsamples;
  }

  void outputSymbol(int symbol) {
    bit_buffer = (bit_buffer << bits_per_symbol) | grayDecode(symbol);
    bit_count += bits_per_symbol;
    while (bit_count >= 8) {
      uint8_t byte = (bit_buffer >> (bit_count - 8)) & 0xFF;
      bit_count -= 8;
      bit_buffer &= (1u << bit_count) - 1;
      if (p_print != nullptr) p_print->write(byte);
      ndata++;
    }
  }

  void startOfCarrier() {
    state = State::DATA;
    has_pending = false;
    bit_buffer = 0;
    bit_count = 0;
    ndata = 0;
    nsymbols = 0;
    confidence_total = 0;
    amplitude_total = 0;
    data_nsamples = 0;
    status("### CARRIER mfsk%d %.2f baud @ %.1f Hz ###\n", ntones,
           (double)cfg.symbol_rate, (double)cfg.base_freq);
  }

  void endOfCarrier() {
    startSearch();
    has_pending = false;
    float n = nsymbols > 0 ? nsymbols : 1;
    double seconds = data_nsamples / cfg.sample_rate;
    double bps = seconds > 0 ? nsymbols * bits_per_symbol / seconds : 0;
    status(
        "\n### NOCARRIER ndata=%u confidence=%.3f ampl=%.3f bps=%.2f ###\n",
        ndata, (double)(confidence_total / n), (double)(amplitude_total / n),
        bps);
    if (cfg.rx_one) is_finished = true;
  }
};

}  // namespace tinymodem
