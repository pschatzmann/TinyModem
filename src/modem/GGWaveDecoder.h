#pragma once

#include <math.h>
#include <string.h>

#include "AudioTools.h"
#include "GGWave.h"
#include "ModemBase.h"
#include "OFDM.h"

namespace tinymodem {

/**
 * @brief ggwave compatible demodulator: the PCM audio data is written to the
 * decoder and the payload of the received messages is written to the output.
 * The input can be 8, 16 or 32 bit signed samples with any number of
 * channels (only GGWaveConfig::channel is evaluated).
 *
 * All ggwave tones are multiples of the frame rate, so they are periodic in a
 * frame. The decoder calculates a DFT of the used bins for each quarter frame
 * (block) and adds the blocks coherently. For each window of one frame a
 * marker score is calculated: the sum of (on - off) / (on + off) of the 16
 * tone pairs of the marker, which is +16 for the start marker, -16 for the end
 * marker and about 0 for noise and data. When the start marker was detected,
 * the position with the highest sum of the scores over the length of the
 * marker provides the start of the data. Then each symbol is decoded by adding
 * the power of the windows inside the symbol (without the first and last
 * block). The power is not added coherently over the whole symbol (like
 * ggwave), so a different clock of the sender is tolerated. This is done in
 * parallel for the normal, fast and fastest variant (rx_any_speed) and for
 * the timing offsets -1, 0 and +1 block. The first candidate with a valid
 * Reed-Solomon code and the end marker at the expected position wins.
 *
 * Unlike ggwave, which records the audio of a whole message, the memory
 * needed is small: about 16 kBytes for the audible protocols.
 * @copyright GPLv3
 */
class GGWaveDecoder : public ModemDecoder {
 public:
  GGWaveDecoder() = default;
  GGWaveDecoder(Print& out) { setOutput(out); }

  /// Provides a default configuration: ggwave-fast
  GGWaveConfig defaultConfig() {
    GGWaveConfig result;
    result.setMode("ggwave-fast");
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
  bool begin(GGWaveConfig config) {
    cfg = config;
    return begin();
  }

  bool begin() override {
    is_active = false;
    if (!cfg.isValid()) return false;
    if (!input.begin(cfg.bits_per_sample, cfg.channels, cfg.channel))
      return false;
    frame_size = cfg.frameSize();
    block_size = frame_size / 4;
    const GGWaveProtocol& selected = cfg.protocolInfo();
    freq_start = selected.freq_start;
    // all protocols with the same tones (they differ only in the speed)
    nprotocols = 0;
    for (int j = 0; j < GGWaveConfig::protocolCount(); j++) {
      const GGWaveProtocol& p = GGWaveConfig::protocols()[j];
      if (p.freq_start != freq_start) continue;
      if (!cfg.rx_any_speed && p.id != selected.id) continue;
      rx_protocols[nprotocols++] = p;
    }
    data_bins = 16 * selected.tones();
    // the marker test also needs the neighbor of the highest marker bin
    nbins = data_bins > 2 * GGWaveConfig::MARKER_TONES
                ? data_bins
                : 2 * GGWaveConfig::MARKER_TONES + 1;
    table.begin(frame_size);
    table_idx.resize(nbins);
    block_re.resize(nbins);
    block_im.resize(nbins);
    ring_re.resize(WINDOW * nbins);
    ring_im.resize(WINDOW * nbins);
    ring_power.resize(RING * nbins);
    candidates.resize(nprotocols * NOFFSETS);
    for (int j = 0; j < (int)candidates.size(); j++)
      candidates[j].acc.resize(data_bins);
    // score of a marker whose tones are marker_threshold times stronger
    float thr = cfg.marker_threshold;
    detect_score = GGWaveConfig::MARKER_TONES * (thr - 1) / (thr + 1);
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
    // feed silence, so that a pending message is completed
    size_t n = (size_t)(GGWaveConfig::MARKER_FRAMES + 4) * frame_size;
    for (size_t j = 0; j < n && !is_finished; j++) {
      if (!run_active && !isCarrier() && sample_pos == 0) break;
      addSample(0.0f, false);
    }
    if (isCarrier()) failed();
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

  /// true while a message is received
  bool isCarrier() override { return nalive > 0; }

  /// true if the decoding was stopped (rx_one)
  bool isFinished() override { return is_finished; }

  /// Number of received messages
  size_t messageCount() { return messages; }

  /// The configuration which was provided in begin()
  GGWaveConfig& config() { return cfg; }

  operator bool() override { return is_active; }

 protected:
  /// number of blocks of the history of the window power
  static constexpr int RING = 24;
  /// number of blocks of the history of the marker score
  static constexpr int SCORE_RING = 128;
  /// number of blocks of the marker detection window (one frame)
  static constexpr int WINDOW = 4;
  /// blocks per frame
  static constexpr int BLOCKS = 4;
  /// timing offsets of the candidates: -1, 0, +1 block
  static constexpr int NOFFSETS = 3;
  /// the marker is lost after this number of blocks without detection
  static constexpr int MAX_MISSES = 8;
  /// a marker must be detected in this number of blocks (of 64)
  static constexpr int MIN_DETECTIONS = 24;
  /// number of windows which are completely inside a marker
  static constexpr int MARKER_WINDOWS =
      GGWaveConfig::MARKER_FRAMES * BLOCKS - (WINDOW - 1);
  static constexpr int MAX_ENCODED = GGWaveConfig::LENGTH_BYTES +
                                     GGWaveConfig::MAX_PAYLOAD +
                                     2 * (GGWaveConfig::MAX_PAYLOAD / 5) + 3;

  /// decoder for one protocol and timing offset
  struct Candidate {
    bool alive = false;
    GGWaveProtocol protocol;
    uint32_t start = 0;  // first block of the data
    uint32_t next = 0;   // next block to process
    int symbol = 0;
    int nsymbols = 0;  // 0 = length not known yet
    int length = 0;
    bool data_done = false;
    float end_score = 0;
    audio_tools::Vector<float> acc;  // power of the bins
    uint8_t bytes[MAX_ENCODED];
  };

  GGWaveConfig cfg;
  bool is_active = false;
  bool is_finished = false;
  PcmInput input;
  OFDMTable table;
  uint32_t noise_seed = 1;
  size_t messages = 0;
  int frame_size = 1024;
  int block_size = 256;
  int freq_start = 40;
  int data_bins = 96;
  int nbins = 96;
  GGWaveProtocol rx_protocols[3];
  int nprotocols = 0;

  // DFT of the current block
  audio_tools::Vector<int> table_idx;
  audio_tools::Vector<float> block_re, block_im;
  int sample_pos = 0;
  // DFT of the last WINDOW blocks
  audio_tools::Vector<float> ring_re, ring_im;
  // power of the window which ends in a block
  audio_tools::Vector<float> ring_power;
  float scores[SCORE_RING];  // start marker score of each block
  float detect_score = 8;    // min score of a detected marker
  uint32_t block_no = 0;

  // start marker
  bool run_active = false;
  uint32_t run_first = 0;
  int run_detections = 0;
  int run_misses = 0;

  audio_tools::Vector<Candidate> candidates;
  int nalive = 0;

  void resetState() {
    for (int b = 0; b < nbins; b++) {
      table_idx[b] = 0;
      block_re[b] = block_im[b] = 0;
    }
    sample_pos = 0;
    block_no = 0;
    run_active = false;
    clearCandidates();
    is_finished = false;
    input.reset();
  }

  void clearCandidates() {
    for (int j = 0; j < (int)candidates.size(); j++)
      candidates[j].alive = false;
    nalive = 0;
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
    // DFT of the block: the index of the sample is counted modulo the frame
    // size, so the blocks of a frame can be added
    const int n = frame_size;
    for (int b = 0; b < nbins; b++) {
      int idx = table_idx[b];
      block_re[b] += x * table.cosine(idx);
      block_im[b] -= x * table.sine(idx);
      idx += freq_start + b;
      if (idx >= n) idx -= n;
      table_idx[b] = idx;
    }
    if (++sample_pos == block_size) {
      sample_pos = 0;
      endBlock();
    }
  }

  float* ringRe(uint32_t block) { return &ring_re[(block % WINDOW) * nbins]; }
  float* ringIm(uint32_t block) { return &ring_im[(block % WINDOW) * nbins]; }
  float* ringPower(uint32_t block) {
    return &ring_power[(block % RING) * nbins];
  }

  void endBlock() {
    uint32_t t = block_no;
    float* re = ringRe(t);
    float* im = ringIm(t);
    for (int b = 0; b < nbins; b++) {
      re[b] = block_re[b];
      im[b] = block_im[b];
      block_re[b] = block_im[b] = 0;
    }
    // power in the window of the last frame
    float* power = ringPower(t);
    float score = 0;
    if (t + 1 >= (uint32_t)WINDOW) {
      for (int b = 0; b < nbins; b++) {
        float sum_re = 0, sum_im = 0;
        for (int k = 0; k < WINDOW; k++) {
          sum_re += ringRe(t - k)[b];
          sum_im += ringIm(t - k)[b];
        }
        power[b] = sum_re * sum_re + sum_im * sum_im;
      }
      score = markerScore(power, nbins);
    } else {
      for (int b = 0; b < nbins; b++) power[b] = 0;
    }
    scores[t % SCORE_RING] = score;
    trackStartMarker(score >= detect_score);
    block_no++;
    processCandidates(t);
  }

  /// Score of the start marker (-score for the end marker): the start marker
  /// tone i uses bin 2i + i % 2, the end marker tone bin 2i + 1 - i % 2.
  /// Pairs which are 40 dB below the strongest bin are ignored: e.g. rounding
  /// errors next to the loud data tones.
  float markerScore(const float* power, int n) {
    float max_power = 0;
    for (int b = 0; b < n; b++)
      if (power[b] > max_power) max_power = power[b];
    float floor = max_power * 1e-4f;
    if (floor <= 0) return 0;
    float score = 0;
    for (int i = 0; i < GGWaveConfig::MARKER_TONES; i++) {
      float on = power[2 * i + i % 2];
      float off = power[2 * i + 1 - i % 2];
      float sum = on + off;
      if (sum > floor) score += (on - off) / sum;
    }
    return score;
  }

  void trackStartMarker(bool detected) {
    uint32_t t = block_no;
    if (detected) {
      if (!run_active) {
        run_active = true;
        run_first = t;
        run_detections = 0;
      }
      run_detections++;
      run_misses = 0;
      return;
    }
    if (!run_active || ++run_misses <= MAX_MISSES) return;
    run_active = false;
    if (run_detections < MIN_DETECTIONS) return;
    // find the windows which are completely inside the marker: they have
    // the highest sum of the scores
    uint32_t oldest = t + 1 > (uint32_t)SCORE_RING ? t + 1 - SCORE_RING : 0;
    uint32_t from = run_first > oldest + 2 * BLOCKS ? run_first - 2 * BLOCKS
                                                    : oldest;
    if (t + 1 < from + MARKER_WINDOWS) return;
    uint32_t to = t + 1 - MARKER_WINDOWS;
    float sum = 0;
    for (int k = 0; k < MARKER_WINDOWS; k++) sum += scores[(from + k) % SCORE_RING];
    float best = sum;
    uint32_t best_pos = from;
    for (uint32_t pos = from + 1; pos <= to; pos++) {
      sum += scores[(pos + MARKER_WINDOWS - 1) % SCORE_RING] -
             scores[(pos - 1) % SCORE_RING];
      if (sum > best) {
        best = sum;
        best_pos = pos;
      }
    }
    // the first window inside the marker ends in its block WINDOW - 1
    startCandidates(best_pos + MARKER_WINDOWS);
  }

  void startCandidates(uint32_t data_start) {
    if (nalive > 0) failed();
    // the oldest block which is still in the history
    uint32_t oldest = block_no + 1 > (uint32_t)RING ? block_no + 1 - RING : 0;
    int idx = 0;
    for (int j = 0; j < nprotocols; j++) {
      for (int k = 0; k < NOFFSETS; k++) {
        Candidate& c = candidates[idx++];
        uint32_t start = data_start + k - NOFFSETS / 2;
        c.alive = start >= oldest;
        if (!c.alive) {
          LOGW("ggwave: start of the data is not in the history");
          continue;
        }
        c.protocol = rx_protocols[j];
        c.start = c.next = start;
        c.symbol = 0;
        c.nsymbols = 0;
        c.length = 0;
        c.data_done = false;
        c.end_score = 0;
        clearAccumulator(c);
        nalive++;
      }
    }
    if (nalive > 0) {
      status("### CARRIER ggwave %.0f-%.0f Hz ###\n",
             (double)cfg.binFrequency(freq_start),
             (double)cfg.binFrequency(freq_start + data_bins - 1));
    }
  }

  void clearAccumulator(Candidate& c) {
    for (int b = 0; b < data_bins; b++) c.acc[b] = 0;
  }

  /// Provides the blocks up to t to all candidates
  void processCandidates(uint32_t t) {
    for (int j = 0; j < (int)candidates.size() && nalive > 0; j++) {
      Candidate& c = candidates[j];
      while (c.alive && (int32_t)(c.next - t) <= 0) {
        processBlock(c, c.next);
        c.next++;
        if (is_finished || nalive == 0) return;
      }
    }
  }

  void processBlock(Candidate& c, uint32_t t) {
    int symbol_blocks = c.protocol.frames_per_tx * BLOCKS;
    uint32_t rel = t - c.start;
    if (!c.data_done) {
      int within = rel % symbol_blocks;
      // windows inside the symbol without its first and last block
      // (envelope and timing)
      if (within >= WINDOW && within < symbol_blocks - 1) {
        const float* power = ringPower(t);
        for (int b = 0; b < data_bins; b++) c.acc[b] += power[b];
      }
      if (within == symbol_blocks - 1) endSymbol(c);
      return;
    }
    // the end marker must follow the data
    uint32_t end_start = (uint32_t)c.nsymbols * symbol_blocks;
    uint32_t first = end_start + 2 * BLOCKS;
    uint32_t last = end_start + (GGWaveConfig::MARKER_FRAMES - 2) * BLOCKS;
    if (rel >= first && rel < last) c.end_score -= scores[t % SCORE_RING];
    if (rel == last - 1) {
      if (c.end_score >= 0.5f * detect_score * (last - first)) {
        accept(c);
      } else {
        kill(c);
      }
    }
  }

  void endSymbol(Candidate& c) {
    int bpt = c.protocol.bytes_per_tx;
    for (int i = 0; i < 2 * bpt; i++) {
      int best = 0;
      float max_power = -1;
      for (int k = 0; k < 16; k++) {
        int b = 16 * i + k;
        if (c.acc[b] > max_power) {
          max_power = c.acc[b];
          best = k;
        }
      }
      uint8_t& value = c.bytes[c.symbol * bpt + i / 2];
      if (i % 2 == 0) {
        value = best;
      } else {
        value |= best << 4;
      }
    }
    c.symbol++;
    clearAccumulator(c);

    if (c.nsymbols == 0 && c.symbol * bpt >= GGWaveConfig::LENGTH_BYTES) {
      uint8_t tmp[GGWaveConfig::LENGTH_BYTES];
      memcpy(tmp, c.bytes, sizeof(tmp));
      if (!GGWaveRS::decode(tmp, 1, GGWaveConfig::LENGTH_BYTES - 1) ||
          tmp[0] == 0 || tmp[0] > GGWaveConfig::MAX_PAYLOAD) {
        kill(c);
        return;
      }
      c.length = tmp[0];
      int total = GGWaveConfig::encodedBytes(c.length);
      c.nsymbols = (total + bpt - 1) / bpt;
    }
    if (c.nsymbols > 0 && c.symbol == c.nsymbols) {
      uint8_t* data = c.bytes + GGWaveConfig::LENGTH_BYTES;
      if (!GGWaveRS::decode(data, c.length, GGWaveConfig::eccBytes(c.length))) {
        kill(c);
        return;
      }
      c.data_done = true;
    }
  }

  void kill(Candidate& c) {
    c.alive = false;
    if (--nalive == 0) failed();
  }

  void failed() {
    clearCandidates();
    status("### NOCARRIER ndata=0 (no valid message) ###\n");
  }

  void accept(Candidate& c) {
    uint8_t* data = c.bytes + GGWaveConfig::LENGTH_BYTES;
    for (int i = 0; i < c.length; i++) {
      uint8_t value = data[i] ^ (cfg.dss ? GGWaveConfig::dssMagic(i) : 0);
      if (p_print != nullptr) p_print->write(value);
    }
    messages++;
    status("\n### NOCARRIER ndata=%d %s ###\n", c.length, c.protocol.mode);
    clearCandidates();
    if (cfg.rx_one) is_finished = true;
  }
};

}  // namespace tinymodem
