#pragma once

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "AudioTools.h"
#include "DataCodecs.h"
#include "FSKDemodulator.h"
#include "ModemBase.h"
#include "ModemConfig.h"

namespace tinymodem {

/**
 * @brief FSK demodulator: PCM audio data is written to the decoder and the
 * decoded data is written to the output that was defined with setOutput().
 * Supported input formats are 8, 16 or 32 bit signed integer samples with
 * any number of channels (only config.channel is evaluated). The decoding
 * is compatible with minimodem --tx.
 *
 * Status messages (### CARRIER and ### NOCARRIER) are written in the
 * minimodem format to the Print defined with setStatusOutput().
 * @copyright GPLv3
 */
class FSKDecoder : public ModemDecoder {
 public:
  FSKDecoder() = default;
  FSKDecoder(Print& out) { setOutput(out); }

  /// Provides a default configuration for a 1200 baud Bell 202 modem
  ModemConfig defaultConfig() {
    ModemConfig result;
    result.setMode("1200");
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

  /// Starts the processing with the indicated modem configuration
  bool begin(ModemConfig config) {
    cfg = config;
    return begin();
  }

  bool begin() override {
    is_active = false;
    active_cfg = cfg;
    ModemConfig& c = active_cfg;
    if (!c.resolve()) {
      LOGE("FSKDecoder: invalid configuration (max 64 bits per frame)");
      return false;
    }
    if (!input.begin(c.bits_per_sample, c.channels, c.channel)) return false;
    if (!plan.begin(c.sample_rate, c.mark_freq, c.space_freq, c.bandwidth)) {
      LOGE("FSKDecoder: frequencies not supported with sample rate %d",
           (int)c.sample_rate);
      return false;
    }
    setupCodec();

    nsamples_per_bit = c.sample_rate / c.baud_rate;

    // For 8-bit frames with prev/start/stop we need 11 data-bits worth of
    // samples, and we will scan through one bits worth at a time, hence we
    // need a minimum total input buffer size of 12 data-bits.
    unsigned nbits = 1 + c.start_bits + c.data_bits + 1;
    size_t samplebuf_size = ceilf(nsamples_per_bit) * (nbits + 1);
    samplebuf_size *= 2;  // account for the half-buf filling method
    if (c.buffer_divisor > 0 &&
        samplebuf_size < c.sample_rate / (unsigned)c.buffer_divisor)
      samplebuf_size = c.sample_rate / c.buffer_divisor;

    // ensure that we overscan at least a single sample
    nsamples_overscan = nsamples_per_bit * FRAME_OVERSCAN + 0.5f;
    if (nsamples_overscan == 0) nsamples_overscan = 1;

    frame_n_bits = c.frameBits();
    frame_nsamples = nsamples_per_bit * frame_n_bits + 0.5f;

    if (c.expect_bits != nullptr) {
      strncpy(expect_data_string, c.expect_bits, MAX_EXPECT - 1);
      expect_data_string[MAX_EXPECT - 1] = 0;
      expect_n_bits = strlen(expect_data_string);
    } else {
      expect_n_bits = buildExpectBits(expect_data_string, false, 0);
    }
    if (c.rx_sync && c.sync_byte >= 0) {
      buildExpectBits(expect_sync_string, true, c.sync_byte);
    } else {
      strcpy(expect_sync_string, expect_data_string);
    }
    expect_nsamples = nsamples_per_bit * expect_n_bits;

    // we process the data when we have enough samples for the frame search
    unsigned try_max_nsamples = nsamples_per_bit + nsamples_overscan;
    size_t needed = try_max_nsamples + expect_nsamples +
                    ceilf(nsamples_per_bit) + 1;
    process_threshold = samplebuf_size / 2;
    if (process_threshold < needed) process_threshold = needed;
    if (samplebuf_size < process_threshold) samplebuf_size = process_threshold;
    samplebuf.resize(samplebuf_size);

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
    if (!is_finished) {
      while (step(true)) {
      }
      if (carrier) reportNoCarrier();
    }
    // with rx_one the decoding stays stopped until begin()
    bool finished = is_finished;
    resetState();
    is_finished = finished;
  }

  /// Provide PCM data to the decoder
  size_t write(const uint8_t* data, size_t len) override {
    if (!is_active) return 0;
    input.write(data, len, [this](float sample) { addSample(sample); });
    return len;
  }

  /// true while a carrier is detected
  bool isCarrier() override { return carrier; }

  /// true if the decoding was stopped (rx_one)
  bool isFinished() override { return is_finished; }

  /// The configuration which was provided in begin(): you can change it and
  /// call begin() again
  ModemConfig& config() { return cfg; }

  /// The active configuration with all derived values (e.g. frequencies)
  const ModemConfig& activeConfig() const { return active_cfg; }

  /// Actual mark frequency (e.g. after the carrier auto detection)
  float markFrequency() { return plan.markBand() * plan.bandWidth(); }

  operator bool() override { return is_active; }

 protected:
  static constexpr int MAX_EXPECT = 80;
  static constexpr int FSK_ANALYZE_NSTEPS = 3;
  static constexpr int FSK_ANALYZE_NSTEPS_FINE = 8;
  static constexpr unsigned FSK_MAX_NOCONFIDENCE_BITS = 20;
  // Fraction of nsamples_per_bit that we will "overscan"; range (0.0 .. 1.0)
  static constexpr float FRAME_OVERSCAN = 0.5f;

  ModemConfig cfg;
  ModemConfig active_cfg;
  bool is_active = false;
  bool is_finished = false;
  FSKDemodulator plan;
  AsciiCodec ascii;
  BaudotCodec baudot;
  BinaryCodec binary;
  CallerIDCodec callerid;
  UICCodec uic;
  DataCodec* p_codec = &ascii;
  PrintFilter print_filter;

  audio_tools::Vector<float> samplebuf;
  size_t samples_nvalid = 0;
  size_t process_threshold = 0;
  size_t advance = 0;
  size_t skip = 0;
  float nsamples_per_bit = 0;
  unsigned nsamples_overscan = 0;
  float frame_n_bits = 0;
  unsigned frame_nsamples = 0;
  char expect_data_string[MAX_EXPECT];
  char expect_sync_string[MAX_EXPECT];
  unsigned expect_n_bits = 0;
  unsigned expect_nsamples = 0;

  PcmInput input;
  uint32_t noise_seed = 1;

  // receiver state
  bool carrier = false;
  int carrier_band = -1;
  float confidence_total = 0;
  float amplitude_total = 0;
  unsigned nframes_decoded = 0;
  size_t carrier_nsamples = 0;
  unsigned noconfidence = 0;
  float track_amplitude = 0.0f;
  float peak_confidence = 0.0f;

  void setupCodec() {
    switch (active_cfg.format) {
      case DataFormat::BAUDOT:
        baudot.setUSOS(active_cfg.baudot_usos);
        p_codec = &baudot;
        break;
      case DataFormat::BINARY:
        p_codec = &binary;
        break;
      case DataFormat::CALLERID:
        p_codec = &callerid;
        break;
      case DataFormat::UIC_TRAIN:
        uic.setTrainToGround(true);
        p_codec = &uic;
        break;
      case DataFormat::UIC_GROUND:
        uic.setTrainToGround(false);
        p_codec = &uic;
        break;
      default:
        p_codec = &ascii;
        break;
    }
  }

  void resetState() {
    samples_nvalid = 0;
    advance = 0;
    skip = 0;
    input.reset();
    carrier = false;
    carrier_band = -1;
    confidence_total = 0;
    amplitude_total = 0;
    nframes_decoded = 0;
    carrier_nsamples = 0;
    noconfidence = 0;
    track_amplitude = 0.0f;
    peak_confidence = 0.0f;
    is_finished = false;
  }

  /// Builds the expected framing pattern e.g. "10dddddddd1"
  /// (prev_stop, start, data bits, stop)
  int buildExpectBits(char* str, bool use_expect_bits, uint64_t expect_bits) {
    ModemConfig& c = active_cfg;
    char start_bit_value = c.invert_start_stop ? '1' : '0';
    char stop_bit_value = c.invert_start_stop ? '0' : '1';
    int j = 0;
    if (c.stop_bits != 0.0f) str[j++] = stop_bit_value;
    for (int i = 0; i < c.start_bits; i++) str[j++] = start_bit_value;
    for (int i = 0; i < c.data_bits; i++, j++) {
      str[j] = use_expect_bits ? ((expect_bits >> i) & 1) + '0' : 'd';
    }
    if (c.stop_bits != 0.0f) str[j++] = stop_bit_value;
    str[j] = 0;
    return j;
  }

  /// uniform random value in the range -0.5 to 0.5
  float noise() {
    noise_seed = noise_seed * 1103515245u + 12345u;
    return ((noise_seed >> 8) & 0xFFFF) / 65535.0f - 0.5f;
  }

  void addSample(float sample) {
    if (is_finished) return;
    if (skip > 0) {
      skip--;
      return;
    }
    if (active_cfg.rx_noise != 0.0f)
      sample += noise() * active_cfg.rx_noise * 2.0f;
    samplebuf[samples_nvalid++] = sample;
    if (samples_nvalid >= process_threshold) {
      while (step(false)) {
      }
    }
  }

  void applyAdvance() {
    if (advance == 0) return;
    if (advance >= samples_nvalid) {
      skip = advance - samples_nvalid;
      samples_nvalid = 0;
    } else {
      memmove(samplebuf.data(), samplebuf.data() + advance,
              (samples_nvalid - advance) * sizeof(float));
      samples_nvalid -= advance;
    }
    advance = 0;
  }

  /// One iteration of the minimodem receive loop: returns false if more data
  /// is needed or if the processing has ended.
  bool step(bool eof) {
    ModemConfig& c = active_cfg;
    applyAdvance();
    if (eof && skip > 0) return false;
    if (!eof && samples_nvalid < process_threshold) return false;
    if (samples_nvalid == 0) return false;
    float* samples = samplebuf.data();
    if (eof) {
      // pad the incomplete data with silence
      for (size_t j = samples_nvalid; j < samplebuf.size(); j++)
        samples[j] = 0.0f;
    }

    // Auto-detect carrier frequency
    if (c.auto_carrier && carrier_band < 0) {
      unsigned i;
      float nsamples_per_scan = nsamples_per_bit;
      if (nsamples_per_scan > plan.fftSize()) nsamples_per_scan = plan.fftSize();
      for (i = 0; i + nsamples_per_scan <= samples_nvalid;
           i += nsamples_per_scan) {
        carrier_band = plan.detectCarrier(samples + i, nsamples_per_scan,
                                          c.auto_carrier_threshold);
        if (carrier_band >= 0) break;
      }
      advance = i + nsamples_per_scan;
      if (advance > samples_nvalid) advance = samples_nvalid;
      if (carrier_band < 0) return true;

      // default negative shift
      float bw = plan.bandWidth();
      int b_shift = -(float)(c.autodetect_shift + bw / 2.0f) / bw;
      if (c.inverted) b_shift *= -1;
      // only accept a carrier as b_mark if it will not result in a b_space
      // band which is "too low".
      int b_space = carrier_band + b_shift;
      if (b_space < 1 || b_space >= (int)plan.bandCount()) {
        carrier_band = -1;
        return true;
      }
      plan.setTonesByBandShift(carrier_band, b_shift);
    }

    if (samples_nvalid < expect_nsamples) return false;

    // try_max_nsamples
    // 1. avoids finding a non-optimal first frame
    // 2. allows us to track slightly slow signals
    unsigned try_max_nsamples = carrier ? nsamples_per_bit * 0.75f + 0.5f
                                        : nsamples_per_bit;
    try_max_nsamples += nsamples_overscan;

    unsigned try_step_nsamples = try_max_nsamples / FSK_ANALYZE_NSTEPS;
    if (try_step_nsamples == 0) try_step_nsamples = 1;

    float confidence, amplitude;
    uint64_t bits = 0;
    // frame_start_sample is the sample where the prev_stop bit begins
    unsigned frame_start_sample = 0;
    unsigned try_first_sample = carrier ? nsamples_overscan : 0;
    float try_confidence_search_limit = c.search_limit;

    confidence = plan.findFrame(
        samples, expect_nsamples, try_first_sample, try_max_nsamples,
        try_step_nsamples, try_confidence_search_limit,
        carrier ? expect_data_string : expect_sync_string, &bits, &amplitude,
        &frame_start_sample);

    bool do_refine_frame = false;
    if (confidence < peak_confidence * 0.75f) {
      do_refine_frame = true;
      peak_confidence = 0;
    }

    // no-confidence if amplitude drops abruptly to < 25% of the
    // track_amplitude, which follows amplitude with hysteresis
    if (amplitude < track_amplitude * 0.25f) confidence = 0;

    if (confidence <= c.confidence) {
      if (++noconfidence > FSK_MAX_NOCONFIDENCE_BITS) {
        carrier_band = -1;
        if (carrier) {
          reportNoCarrier();
          carrier = false;
          carrier_nsamples = 0;
          confidence_total = 0;
          amplitude_total = 0;
          nframes_decoded = 0;
          track_amplitude = 0.0f;
          if (c.rx_one) {
            is_finished = true;
            return false;
          }
        }
      }
      // continue searching from where we left off this time
      advance = try_max_nsamples;
      return true;
    }

    // Add a frame's worth of samples to the sample count
    carrier_nsamples += frame_nsamples;

    if (carrier) {
      // If we already had carrier, adjust sample count +start -overscan
      carrier_nsamples += frame_start_sample;
      carrier_nsamples -= nsamples_overscan;
    } else {
      // We just acquired carrier.
      reportCarrier();
      carrier = true;
      p_codec->reset();
      do_refine_frame = true;
    }

    if (do_refine_frame && confidence < INFINITY && try_step_nsamples > 1) {
      // Scan again, but try harder to find the best frame.
      try_step_nsamples = try_max_nsamples / FSK_ANALYZE_NSTEPS_FINE;
      if (try_step_nsamples == 0) try_step_nsamples = 1;
      float confidence2, amplitude2;
      uint64_t bits2;
      unsigned frame_start_sample2;
      confidence2 = plan.findFrame(
          samples, expect_nsamples, try_first_sample, try_max_nsamples,
          try_step_nsamples, INFINITY,
          carrier ? expect_data_string : expect_sync_string, &bits2,
          &amplitude2, &frame_start_sample2);
      if (confidence2 > confidence) {
        bits = bits2;
        amplitude = amplitude2;
        frame_start_sample = frame_start_sample2;
      }
    }

    track_amplitude = (track_amplitude + amplitude) / 2;
    if (peak_confidence < confidence) peak_confidence = confidence;
    confidence_total += confidence;
    amplitude_total += amplitude;
    nframes_decoded++;
    noconfidence = 0;

    // Advance past the decoded frame, but a bit less than that to allow for
    // tracking slightly fast signals, hence - nsamples_overscan.
    advance = frame_start_sample + frame_nsamples - nsamples_overscan;

    // chop off the prev_stop bit
    if (c.stop_bits != 0.0f) bits = bits >> 1;
    // chop off framing bits
    bits = bitWindow(bits, c.start_bits, c.data_bits);
    if (c.msb_first) bits = bitReverse(bits, c.data_bits);

    // suppress printing of sync bytes
    if (c.rx_sync && (long long)bits == c.sync_byte) return true;

    if (p_print != nullptr) {
      if (c.print_filter) {
        print_filter.setOutput(p_print);
        p_codec->decode(print_filter, bits, c.data_bits);
      } else {
        p_codec->decode(*p_print, bits, c.data_bits);
      }
    }
    return true;
  }

  void reportCarrier() {
    float rate = active_cfg.baud_rate;
    if (rate >= 100)
      status("### CARRIER %u @ %.1f Hz ###\n", (unsigned)(rate + 0.5f),
             (double)markFrequency());
    else
      status("### CARRIER %.2f @ %.1f Hz ###\n", (double)rate,
             (double)markFrequency());
  }

  void reportNoCarrier() {
    float sample_rate = active_cfg.sample_rate;
    float rate = active_cfg.baud_rate;
    float nbits_decoded = nframes_decoded * frame_n_bits;
    float throughput_rate =
        nbits_decoded * sample_rate / (float)carrier_nsamples;
    status("\n### NOCARRIER ndata=%u confidence=%.3f ampl=%.3f bps=%.2f",
           nframes_decoded, (double)(confidence_total / nframes_decoded),
           (double)(amplitude_total / nframes_decoded),
           (double)throughput_rate);
    if ((unsigned long long)(nbits_decoded * sample_rate + 0.5f) ==
        (unsigned long long)(rate * carrier_nsamples)) {
      status(" (rate perfect) ###\n");
    } else {
      float skew = (throughput_rate - rate) / rate;
      status(" (%.1f%% %s) ###\n", (double)(fabsf(skew) * 100.0f),
             signbit(skew) ? "slow" : "fast");
    }
  }
};

}  // namespace tinymodem
