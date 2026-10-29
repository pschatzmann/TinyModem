#pragma once

#include <math.h>
#include <stdint.h>

#include "AudioTools.h"

namespace tinymodem {

/**
 * @brief Converts the sample rate of a signal with a windowed sinc low pass
 * filter (by default the cut off is at 45% of the lower sample rate).
 * @copyright GPLv3
 */
class SampleRateConverter {
 public:
  /// Defines the input and output sample rate and the cut off relative to
  /// the lower sample rate
  void begin(float in_rate, float out_rate, float cutoff = 0.45f) {
    this->cutoff = cutoff;
    ratio = base_ratio = in_rate / out_rate;
    bypass = fabsf(ratio - 1.0f) < 1e-6f;
    float low_rate = in_rate < out_rate ? in_rate : out_rate;
    // kernel in input samples
    half_width = HALF_WIDTH * in_rate / low_rate;
    kernel_scale = low_rate / in_rate;
    int len = (int)ceilf(2 * half_width) + 2;
    history.resize(len);
    history_q.resize(len);
    for (int j = 0; j <= TABLE; j++) {
      // u in units of the lower rate: 0 to HALF_WIDTH
      float u = (float)HALF_WIDTH * j / TABLE;
      float x = 2.0f * cutoff * u;
      float sinc = j == 0 ? 1.0f : sinf((float)M_PI * x) / ((float)M_PI * x);
      float window = 0.5f + 0.5f * cosf((float)M_PI * u / HALF_WIDTH);
      table[j] = 2.0f * cutoff * sinc * window;
    }
    reset();
  }

  void reset() {
    for (size_t j = 0; j < history.size(); j++) history[j] = history_q[j] = 0;
    count = 0;
    next = 0;
  }

  /// Changes the output rate by the indicated factor (e.g. 1.001), e.g. to
  /// follow a different clock of the sender
  void setRateFactor(double factor) {
    if (!bypass) ratio = base_ratio / factor;
  }

  /// Adds an input sample and calls out(float) for each output sample
  template <class F>
  void write(float x, F out) {
    if (bypass) {
      out(x);
      return;
    }
    int len = history.size();
    history[count % len] = x;
    count++;
    // the output at position next needs the inputs up to next + half_width
    while (next + half_width < count - 1) {
      int first = (int)ceilf(next - half_width);
      int last = (int)floorf(next + half_width);
      float sum = 0;
      for (int n = first; n <= last; n++) {
        if (n < 0) continue;
        sum += history[n % len] * kernel(fabsf(n - next) * kernel_scale);
      }
      out(sum * kernel_scale);
      next += ratio;
    }
  }

  /// Adds a complex input sample (both parts use the same timing) and calls
  /// out(float re, float im) for each output sample
  template <class F>
  void writeComplex(float re, float im, F out) {
    if (bypass) {
      out(re, im);
      return;
    }
    int len = history.size();
    history[count % len] = re;
    history_q[count % len] = im;
    count++;
    while (next + half_width < count - 1) {
      int first = (int)ceilf(next - half_width);
      int last = (int)floorf(next + half_width);
      float sum_re = 0, sum_im = 0;
      for (int n = first; n <= last; n++) {
        if (n < 0) continue;
        float k = kernel(fabsf(n - next) * kernel_scale);
        sum_re += history[n % len] * k;
        sum_im += history_q[n % len] * k;
      }
      out(sum_re * kernel_scale, sum_im * kernel_scale);
      next += ratio;
    }
  }

 protected:
  static constexpr int HALF_WIDTH = 8;
  static constexpr int TABLE = 512;
  float table[TABLE + 2];
  audio_tools::Vector<float> history, history_q;
  double ratio = 1, base_ratio = 1;
  float cutoff = 0.45f;
  bool bypass = true;
  float half_width = HALF_WIDTH;
  float kernel_scale = 1;
  int64_t count = 0;
  double next = 0;

  /// kernel at the distance u (in samples of the lower rate)
  float kernel(float u) {
    float pos = u * TABLE / HALF_WIDTH;
    int idx = (int)pos;
    if (idx >= TABLE) return 0;
    float frac = pos - idx;
    return table[idx] + frac * (table[idx + 1] - table[idx]);
  }
};

/**
 * @brief Radix 2 complex FFT with a precomputed twiddle table.
 * @copyright GPLv3
 */
class SimpleFFT {
 public:
  bool begin(int size) {
    if (size < 2 || (size & (size - 1)) != 0) return false;
    len = size;
    cos_table.resize(len / 2);
    sin_table.resize(len / 2);
    for (int j = 0; j < len / 2; j++) {
      cos_table[j] = cos(2.0 * M_PI * j / len);
      sin_table[j] = -sin(2.0 * M_PI * j / len);
    }
    return true;
  }

  /// In place forward transform
  void process(float* re, float* im) {
    // bit reversal
    for (int i = 1, j = 0; i < len; i++) {
      int bit = len >> 1;
      for (; j & bit; bit >>= 1) j ^= bit;
      j ^= bit;
      if (i < j) {
        float t = re[i];
        re[i] = re[j];
        re[j] = t;
        t = im[i];
        im[i] = im[j];
        im[j] = t;
      }
    }
    for (int size = 2; size <= len; size *= 2) {
      int half = size / 2;
      int step = len / size;
      for (int start = 0; start < len; start += size) {
        for (int k = 0; k < half; k++) {
          float c = cos_table[k * step], s = sin_table[k * step];
          int a = start + k, b = a + half;
          float tr = re[b] * c - im[b] * s;
          float ti = re[b] * s + im[b] * c;
          re[b] = re[a] - tr;
          im[b] = im[a] - ti;
          re[a] += tr;
          im[a] += ti;
        }
      }
    }
  }

  int size() const { return len; }

 protected:
  int len = 0;
  audio_tools::Vector<float> cos_table, sin_table;
};

}  // namespace tinymodem
