#pragma once

#include <float.h>
#include <math.h>
#include <string.h>

#include "AudioTools.h"
#include "AudioTools/CoreAudio/Analysis/GoerzelStream.h"

namespace tinymodem {

/**
 * @brief FSK bit and frame analysis (port of minimodem's fsk.c). Instead of
 * a full FFT we use the Goertzel algorithm from the AudioTools to determine
 * the magnitude of the mark and space bands. The bands are defined like the
 * FFT bins of minimodem, so the results are equivalent.
 * @copyright GPLv3
 */
class FSKDemodulator {
 public:
  /// Defines the tones and the filter bandwidth: returns false if invalid
  bool begin(float sample_rate, float f_mark, float f_space, float filter_bw) {
    this->sample_rate = sample_rate;
    this->f_mark = f_mark;
    this->f_space = f_space;
    band_width = filter_bw;
    float half_bw = band_width / 2.0f;
    fftsize = (sample_rate + half_bw) / band_width;
    nbands = fftsize / 2 + 1;
    b_mark = (f_mark + half_bw) / band_width;
    b_space = (f_space + half_bw) / band_width;
    if (fftsize == 0 || b_mark >= nbands || b_space >= nbands) return false;
    detector_nsamples = 0;
    return true;
  }

  /// Analyzes the expected bits starting at samples and returns the
  /// confidence: [0.0 to INFINITY]. Port of fsk_find_frame().
  float findFrame(const float* samples, unsigned frame_nsamples,
                  unsigned try_first_sample, unsigned try_max_nsamples,
                  unsigned try_step_nsamples, float try_confidence_search_limit,
                  const char* expect_bits_string, uint64_t* bits_outp,
                  float* ampl_outp, unsigned* frame_start_outp) {
    int expect_n_bits = strlen(expect_bits_string);
    if (expect_n_bits == 0 || expect_n_bits > MAX_BITS) return 0.0f;
    float samples_per_bit = (float)frame_nsamples / expect_n_bits;

    unsigned best_t = 0;
    float best_c = 0.0f, best_a = 0.0f;
    uint64_t best_bits = 0;

    // Scan the frame positions starting with try_first_sample, alternating
    // between a step above that, a step below that, above, below, and so on,
    // until we've scanned the whole try_max_nsamples range.
    for (int j = 0;; j++) {
      int up = (j % 2) ? 1 : -1;
      int t = try_first_sample + up * ((j + 1) / 2) * try_step_nsamples;
      if (t >= (int)try_max_nsamples) break;
      if (t < 0) continue;

      float ampl_out = 0.0f;
      uint64_t bits_out = 0;
      float c = frameAnalyze(samples + t, samples_per_bit, expect_n_bits,
                             expect_bits_string, &bits_out, &ampl_out);
      if (best_c < c) {
        best_t = t;
        best_c = c;
        best_a = ampl_out;
        best_bits = bits_out;
        // quit searching if confidence > try_confidence_search_limit
        if (best_c >= try_confidence_search_limit) break;
      }
    }
    *bits_outp = best_bits;
    *ampl_outp = best_a;
    *frame_start_outp = best_t;
    return best_c;
  }

  /// Returns the band with the max magnitude or -1. Port of
  /// fsk_detect_carrier()
  int detectCarrier(const float* samples, unsigned nsamples,
                    float min_mag_threshold) {
    float magscalar = 1.0f / ((float)nsamples / 2.0f);
    float max_mag = 0.0f;
    int max_mag_band = -1;
    // start detection at the first non-DC band
    for (unsigned i = 1; i < nbands; i++) {
      float mag = bandMagnitude(samples, nsamples, i) * magscalar;
      if (mag < min_mag_threshold) continue;
      if (max_mag < mag) {
        max_mag = mag;
        max_mag_band = i;
      }
    }
    return max_mag_band;
  }

  /// Redefines the mark and space band
  void setTonesByBandShift(unsigned b_mark, int b_shift) {
    this->b_mark = b_mark;
    this->b_space = b_mark + b_shift;
    f_mark = this->b_mark * band_width;
    f_space = this->b_space * band_width;
    detector_nsamples = 0;
  }

  unsigned fftSize() { return fftsize; }
  unsigned bandCount() { return nbands; }
  float bandWidth() { return band_width; }
  unsigned markBand() { return b_mark; }
  unsigned spaceBand() { return b_space; }

 protected:
  // a frame has max 64 bits + the previous stop bit
  static constexpr int MAX_BITS = 72;
  float sample_rate = 0.0f;
  float f_mark = 0.0f;
  float f_space = 0.0f;
  float band_width = 0.0f;
  unsigned fftsize = 0;
  unsigned nbands = 0;
  unsigned b_mark = 0;
  unsigned b_space = 0;
  audio_tools::GoertzelDetector mark_detector;
  audio_tools::GoertzelDetector space_detector;
  unsigned detector_nsamples = 0;

  /// center frequency of the FFT bin which corresponds to the band
  float bandFrequency(unsigned band) {
    return (float)band * sample_rate / (float)fftsize;
  }

  bool setupDetector(audio_tools::GoertzelDetector& detector, unsigned band,
                     unsigned nsamples) {
    audio_tools::GoertzelConfig gcfg;
    gcfg.sample_rate = sample_rate;
    gcfg.channels = 1;
    gcfg.target_frequency = bandFrequency(band);
    gcfg.block_size = nsamples;
    return detector.begin(gcfg);
  }

  /// unscaled magnitude of the band over n samples
  float magnitude(audio_tools::GoertzelDetector& detector,
                  const float* samples, unsigned nsamples) {
    detector.reset();
    for (unsigned j = 0; j < nsamples; j++) detector.processSample(samples[j]);
    return detector.getMagnitude();
  }

  float bandMagnitude(const float* samples, unsigned nsamples, unsigned band) {
    audio_tools::GoertzelDetector detector;
    if (!setupDetector(detector, band, nsamples)) return 0.0f;
    return magnitude(detector, samples, nsamples);
  }

  void bitAnalyze(const float* samples, unsigned bit_nsamples,
                  unsigned* bit_outp, float* bit_signal_mag_outp,
                  float* bit_noise_mag_outp) {
    if (detector_nsamples != bit_nsamples) {
      setupDetector(mark_detector, b_mark, bit_nsamples);
      setupDetector(space_detector, b_space, bit_nsamples);
      detector_nsamples = bit_nsamples;
    }
    float magscalar = 2.0f / (float)bit_nsamples;
    float mag_mark = magnitude(mark_detector, samples, bit_nsamples) * magscalar;
    float mag_space =
        magnitude(space_detector, samples, bit_nsamples) * magscalar;
    // mark==1, space==0
    if (mag_mark > mag_space) {
      *bit_outp = 1;
      *bit_signal_mag_outp = mag_mark;
      *bit_noise_mag_outp = mag_space;
    } else {
      *bit_outp = 0;
      *bit_signal_mag_outp = mag_space;
      *bit_noise_mag_outp = mag_mark;
    }
  }

  /// returns confidence value [0.0 to INFINITY]
  float frameAnalyze(const float* samples, float samples_per_bit, int n_bits,
                     const char* expect_bits, uint64_t* bits_outp,
                     float* ampl_outp) {
    unsigned bit_nsamples = (float)(samples_per_bit + 0.5f);
    unsigned bit_values[MAX_BITS];
    float bit_sig_mags[MAX_BITS];
    float bit_noise_mags[MAX_BITS];

    // pass #1 - process and check only the "required" (1/0) expect_bits
    for (int bitnum = 0; bitnum < n_bits; bitnum++) {
      if (expect_bits[bitnum] == 'd') continue;
      unsigned bit_begin_sample = (float)(samples_per_bit * bitnum + 0.5f);
      bitAnalyze(samples + bit_begin_sample, bit_nsamples, &bit_values[bitnum],
                 &bit_sig_mags[bitnum], &bit_noise_mags[bitnum]);
      if ((unsigned)(expect_bits[bitnum] - '0') != bit_values[bitnum])
        return 0.0f;  // does not match expected; abort frame analysis.
    }

    // pass #2 - process only the dontcare ('d') expect_bits
    for (int bitnum = 0; bitnum < n_bits; bitnum++) {
      if (expect_bits[bitnum] != 'd') continue;
      unsigned bit_begin_sample = (float)(samples_per_bit * bitnum + 0.5f);
      bitAnalyze(samples + bit_begin_sample, bit_nsamples, &bit_values[bitnum],
                 &bit_sig_mags[bitnum], &bit_noise_mags[bitnum]);
    }

    float total_bit_sig = 0.0f, total_bit_noise = 0.0f;
    float avg_mark_sig = 0.0f, avg_space_sig = 0.0f;
    unsigned n_mark = 0, n_space = 0;
    for (int bitnum = 0; bitnum < n_bits; bitnum++) {
      // Deal with floating point data type quantization noise: if
      // total_bit_noise <= FLT_EPSILON, assume it to be 0.0 (snr==inf).
      total_bit_sig += bit_sig_mags[bitnum];
      if (bit_noise_mags[bitnum] > FLT_EPSILON)
        total_bit_noise += bit_noise_mags[bitnum];
      if (bit_values[bitnum] == 1) {
        avg_mark_sig += bit_sig_mags[bitnum];
        n_mark++;
      } else {
        avg_space_sig += bit_sig_mags[bitnum];
        n_space++;
      }
    }

    // Compute the "frame SNR"
    float snr = total_bit_sig / total_bit_noise;
    float avg_bit_sig = total_bit_sig / n_bits;
    if (n_mark) avg_mark_sig /= n_mark;
    if (n_space) avg_space_sig /= n_space;

    // Compute average "divergence": bit_mag_divergence / other_bits_mag
    float divergence = 0.0f;
    for (int bitnum = 0; bitnum < n_bits; bitnum++) {
      float avg_bit_sig_other = bit_values[bitnum] ? avg_mark_sig : avg_space_sig;
      divergence +=
          fabsf(bit_sig_mags[bitnum] - avg_bit_sig_other) / avg_bit_sig_other;
    }
    divergence *= 2;
    divergence /= n_bits;

    // Frame confidence is the frame ( SNR * consistency )
    float confidence = snr * (1.0f - divergence);
    *ampl_outp = avg_bit_sig;

    // least significant bit first
    *bits_outp = 0;
    for (int bitnum = 0; bitnum < n_bits && bitnum < 64; bitnum++)
      *bits_outp |= (uint64_t)bit_values[bitnum] << bitnum;

    return confidence;
  }
};

}  // namespace tinymodem
