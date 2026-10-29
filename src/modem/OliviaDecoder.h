#pragma once

#include <math.h>
#include <string.h>

#include "AudioTools.h"
#include "ModemBase.h"
#include "Olivia.h"

namespace tinymodem {

/**
 * @brief Olivia and Contestia demodulator (compatible with fldigi): the PCM
 * audio data is written to the decoder and the decoded characters are
 * written to the output. The input can be 8, 16 or 32 bit signed samples
 * with any number of channels (only OliviaConfig::channel is evaluated).
 *
 * The input is converted to 8000 Hz. Twice per symbol the tone energies are
 * measured with an FFT and converted to soft bits for each possible frequency
 * offset (+/- OliviaConfig::sync_margin FFT bins). For each time slice and
 * frequency offset the last FEC block is decoded after every symbol. The
 * synchronizer integrates the signal and noise of the decoded blocks over
 * OliviaConfig::sync_integration blocks and selects the block timing and the
 * frequency offset with the strongest signal. A block is provided if its
 * signal to noise ratio is above OliviaConfig::sync_threshold. The idle
 * character 0 is not provided.
 *
 * Memory: about (2 * sync_margin + 1) * 3 * symbolsPerBlock() * (bits per
 * symbol + 16) bytes plus 20 * symbolLen() bytes, e.g. 30 kB for
 * olivia-32-1000.
 * @copyright GPLv3
 */
class OliviaDecoder : public ModemDecoder {
 public:
  OliviaDecoder() = default;
  OliviaDecoder(Print& out) { setOutput(out); }

  /// Provides a default configuration: olivia-32-1000
  OliviaConfig defaultConfig() { return OliviaConfig(); }

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
  bool begin(OliviaConfig config) {
    cfg = config;
    return begin();
  }

  bool begin() override {
    is_active = false;
    if (!cfg.isValid()) return false;
    if (!input.begin(cfg.bits_per_sample, cfg.channels, cfg.channel))
      return false;
    converter.begin(cfg.sample_rate, OliviaConfig::BASE_RATE);
    bits = cfg.bitsPerSymbol();
    tones = 1 << bits;
    codec.begin(bits, cfg.contestia);
    block_len = codec.blockLen();
    symbol_len = cfg.symbolLen();
    // the search range is limited by the audio band
    margin = cfg.sync_margin;
    int first = cfg.firstCarrier();
    if (margin > first - 1) margin = first - 1;
    int upper = symbol_len / 2 - 1 - (first + 2 * tones);
    if (margin > upper) margin = upper;
    if (margin < 0) margin = 0;
    offsets = 2 * margin + 1;
    phases = SLICES * block_len;
    first_bin = cfg.firstCarrier() - margin;
    width = 2 * tones - 1 + 2 * margin;
    if (!fft.begin(symbol_len)) return false;
    samples.resize(symbol_len);
    window.resize(symbol_len);
    for (int j = 0; j < symbol_len; j++)
      window[j] = (1.0f - cosf(2.0f * (float)M_PI * j / symbol_len)) / symbol_len;
    fft_re.resize(symbol_len);
    fft_im.resize(symbol_len);
    energy.resize(width);
    soft.resize(SLICES * offsets * block_len * bits);
    sync_signal.resize(phases * offsets);
    sync_noise.resize(phases * offsets);
    blocks.resize(phases);
    block_snr.resize(phases);
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
    // feed silence, so that the last block is provided
    int n = (block_len + 4) * symbol_len / 2 + 32;
    for (int j = 0; j < n && !is_finished; j++) addSample8k(0.0f);
    if (carrier) endOfCarrier();
    bool finished = is_finished;
    resetState();
    is_finished = finished;
  }

  /// Provide PCM data to the decoder
  size_t write(const uint8_t* data, size_t len) override {
    if (!is_active) return 0;
    input.write(data, len, [this](float x) {
      if (cfg.rx_noise != 0.0f) x += noise() * cfg.rx_noise * 2.0f;
      converter.write(x, [this](float y) { addSample8k(y); });
    });
    return len;
  }

  /// true while a transmission is received
  bool isCarrier() override { return carrier; }

  /// true if the decoding was stopped (rx_one)
  bool isFinished() override { return is_finished; }

  /// Signal to noise ratio of the synchronizer
  float signalToNoise() { return snr; }

  /// Frequency offset of the received signal in Hz
  float frequencyOffset() {
    return (best_offset - margin) * OliviaConfig::BASE_RATE / symbol_len;
  }

  /// The configuration which was provided in begin()
  OliviaConfig& config() { return cfg; }

  operator bool() override { return is_active; }

 protected:
  static constexpr int SLICES = 2;  // FFTs per symbol
  /// minimum signal to noise ratio of a block relative to the sync threshold
  static constexpr float BLOCK_THRESHOLD = 1.0f;
  OliviaConfig cfg;
  bool is_active = false;
  bool is_finished = false;
  PcmInput input;
  SampleRateConverter converter;
  OliviaCodec codec;
  SimpleFFT fft;
  uint32_t noise_seed = 1;
  int bits = 5, tones = 32, block_len = 64, symbol_len = 512;
  int margin = 8, offsets = 17, phases = 128, first_bin = 0, width = 0;

  // input samples (8000 Hz) and spectrum
  audio_tools::Vector<float> samples, window, fft_re, fft_im, energy;
  int sample_pos = 0;
  int slice_countdown = 0;
  int slice = 0;
  // soft bits of the last block per slice and offset (circular by symbol)
  audio_tools::Vector<int8_t> soft;
  int soft_pos = 0;  // symbol which is overwritten next
  // synchronizer per block phase and offset
  audio_tools::Vector<float> sync_signal, sync_noise;
  audio_tools::Vector<uint64_t> blocks;  // decoded block per phase
  audio_tools::Vector<float> block_snr;  // its signal to noise ratio
  int phase = 0;
  float best_signal = 0, best_noise = 0;
  int best_phase = 0, best_offset = 0;
  float snr = 0;
  // output
  bool carrier = false;
  bool escape = false;
  unsigned ndata = 0;
  unsigned nblocks = 0;
  float snr_total = 0;

  void resetState() {
    for (int j = 0; j < symbol_len; j++) samples[j] = 0;
    for (size_t j = 0; j < soft.size(); j++) soft[j] = 0;
    resetSync();
    sample_pos = 0;
    slice_countdown = symbol_len / 4;
    slice = 0;
    soft_pos = 0;
    phase = 0;
    snr = 0;
    carrier = false;
    escape = false;
    is_finished = false;
    converter.reset();
    input.reset();
  }

  /// Forgets the timing and frequency of the last transmission
  void resetSync() {
    for (size_t j = 0; j < sync_signal.size(); j++)
      sync_signal[j] = sync_noise[j] = 0;
    for (int j = 0; j < phases; j++) {
      blocks[j] = 0;
      block_snr[j] = 0;
    }
    best_signal = best_noise = 0;
    best_phase = best_offset = 0;
  }

  /// uniform random value in the range -0.5 to 0.5
  float noise() {
    noise_seed = noise_seed * 1103515245u + 12345u;
    return ((noise_seed >> 8) & 0xFFFF) / 65535.0f - 0.5f;
  }

  void addSample8k(float x) {
    if (is_finished) return;
    samples[sample_pos] = x;
    if (++sample_pos == symbol_len) sample_pos = 0;
    if (--slice_countdown == 0) {
      slice_countdown = symbol_len / 4;
      processSlice();
    }
  }

  /// FFT over the last symbol_len samples and decoding of all offsets
  void processSlice() {
    for (int j = 0; j < symbol_len; j++) {
      int idx = sample_pos + j;
      if (idx >= symbol_len) idx -= symbol_len;
      fft_re[j] = samples[idx] * window[j];
      fft_im[j] = 0;
    }
    fft.process(fft_re.data(), fft_im.data());
    for (int j = 0; j < width; j++) {
      int bin = first_bin + j;
      energy[j] = fft_re[bin] * fft_re[bin] + fft_im[bin] * fft_im[bin];
    }

    int8_t block_bits[64 * 8];
    float slice_best = 0;
    int slice_best_offset = 0;
    uint64_t slice_block = 0;
    float slice_noise = 0, slice_snr = 0;
    for (int o = 0; o < offsets; o++) {
      int8_t* buffer = soft.data() + (slice * offsets + o) * block_len * bits;
      softDecode(energy.data() + o, buffer + soft_pos * bits);
      // decode the block which ends with this symbol
      linearize(buffer, block_bits);
      float signal = 0, noise_energy = 0;
      uint64_t block = 0;
      for (int b = 0; b < bits; b++) {
        float peak, noise;
        uint8_t ch = codec.decodeChar(block_bits, b, peak, noise);
        block |= (uint64_t)ch << (8 * b);
        signal += peak;
        noise_energy += noise;
      }
      signal /= bits;
      noise_energy /= bits;
      float weight = 1.0f / cfg.sync_integration;
      float& s = sync_signal[phase * offsets + o];
      float& n = sync_noise[phase * offsets + o];
      s += weight * (signal - s);
      n += weight * (noise_energy - n);
      if (s > slice_best) {
        slice_best = s;
        slice_best_offset = o;
        slice_block = block;
        slice_noise = n;
        slice_snr = signal / sqrtf(noise_energy > 0 ? noise_energy : 1e-9f);
      }
    }
    // steady tones (e.g. the start and stop tones) are not a valid block
    int8_t* best_buffer =
        soft.data() + (slice * offsets + slice_best_offset) * block_len * bits;
    linearize(best_buffer, block_bits);
    if (repeatedSymbols(block_bits) > block_len / tones + block_len / 4)
      slice_snr = 0;
    blocks[phase] = slice_block;
    block_snr[phase] = slice_snr;
    if (phase == best_phase || slice_best > best_signal) {
      best_signal = slice_best;
      best_phase = phase;
      best_offset = slice_best_offset;
      best_noise = slice_noise;
    }
    // a block is provided half a block after the best timing
    int dist = phase - best_phase;
    if (dist < 0) dist += phases;
    if (dist == phases / 2) evaluateBlock();

    if (++phase == phases) phase = 0;
    if (++slice == SLICES) {
      slice = 0;
      if (++soft_pos == block_len) soft_pos = 0;
    }
  }

  /// Soft bits of a symbol from the tone energies (like fldigi)
  void softDecode(const float* tone_energy, int8_t* out) {
    float sum[8] = {0};
    float total = 0;
    for (int k = 0; k < tones; k++) {
      uint8_t symbol = OliviaCodec::binaryCode(k);
      float e = tone_energy[2 * k];
      e *= e;
      total += e;
      for (int b = 0; b < bits; b++) sum[b] += (symbol >> b) & 1 ? -e : e;
    }
    for (int b = 0; b < bits; b++)
      out[b] = total > 0 ? (int8_t)lroundf(127.0f * sum[b] / total) : 0;
  }

  /// Number of symbols which are equal to the previous symbol (hard decision)
  int repeatedSymbols(const int8_t* block) {
    int result = 0;
    int prev = -1;
    for (int t = 0; t < block_len; t++) {
      int symbol = 0;
      for (int b = 0; b < bits; b++)
        if (block[t * bits + b] < 0) symbol |= 1 << b;
      if (symbol == prev) result++;
      prev = symbol;
    }
    return result;
  }

  /// Copies the circular soft bit buffer: the oldest symbol first
  void linearize(const int8_t* buffer, int8_t* block) {
    int start = soft_pos + 1;  // oldest symbol
    if (start == block_len) start = 0;
    int n1 = (block_len - start) * bits;
    memcpy(block, buffer + start * bits, n1);
    memcpy(block + n1, buffer, start * bits);
  }

  /// Provides the best block if the signal to noise ratio is sufficient
  void evaluateBlock() {
    snr = best_noise > 0 ? best_signal / sqrtf(best_noise)
                         : (best_signal > 0 ? 100.0f : 0.0f);
    // the integrated timing lags behind a drifting clock: we use the best
    // block of the neighboring time slices
    int use = best_phase;
    for (int d = -1; d <= 1; d += 2) {
      int p = (best_phase + d + phases) % phases;
      if (block_snr[p] > block_snr[use]) use = p;
    }
    // the block itself must be valid: avoids garbage at the end of a
    // transmission, while the integrated signal is still strong
    if (snr < cfg.sync_threshold ||
        block_snr[use] < BLOCK_THRESHOLD * cfg.sync_threshold) {
      if (carrier) endOfCarrier();
      return;
    }
    if (!carrier) startOfCarrier();
    nblocks++;
    snr_total += snr;
    uint64_t block = blocks[use];
    for (int b = 0; b < bits; b++) outputChar((block >> (8 * b)) & 0xFF);
  }

  void outputChar(uint8_t ch) {
    if (!cfg.contestia) {
      if (escape) {
        escape = false;
        ch += 128;
      } else if (ch == 127) {
        escape = true;
        return;
      }
    }
    if (ch == 0) return;  // idle
    ndata++;
    if (p_print != nullptr) p_print->write(ch);
  }

  void startOfCarrier() {
    carrier = true;
    ndata = 0;
    nblocks = 0;
    snr_total = 0;
    escape = false;
    status("### CARRIER %s %d/%d @ %.1f Hz ###\n",
           cfg.contestia ? "contestia" : "olivia", cfg.tones, cfg.bandwidth,
           (double)(cfg.center_freq + frequencyOffset()));
  }

  void endOfCarrier() {
    carrier = false;
    float n = nblocks > 0 ? nblocks : 1;
    status("\n### NOCARRIER ndata=%u blocks=%u snr=%.1f freq=%.1f Hz ###\n",
           ndata, nblocks, (double)(snr_total / n),
           (double)(cfg.center_freq + frequencyOffset()));
    resetSync();
    if (cfg.rx_one) is_finished = true;
  }
};

}  // namespace tinymodem
