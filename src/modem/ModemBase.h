#pragma once

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "AudioTools.h"

namespace tinymodem {

/**
 * @brief Default sine generator of the encoders: like minimodem it keeps the
 * phase continuous when the frequency is changed (continuous phase FSK) and
 * restarts it at phase 0 after silence (frequency 0).
 * @copyright GPLv3
 */
template <class T = int16_t>
class FSKToneGenerator : public audio_tools::FloatSineGenerator<T> {
 public:
  void setFrequency(float frequency) override {
    this->m_frequency = frequency;
    if (frequency == 0.0f) this->m_cycles = 0.0f;
  }

  T readSample() override {
    if (this->m_frequency == 0.0f) return 0;
    float value = this->m_amplitude * sinf(2.0f * (float)M_PI * this->m_cycles);
    this->m_cycles += this->m_frequency * this->m_deltaTime;
    this->m_cycles -= floorf(this->m_cycles);
    return (T)lroundf(value);
  }
};

/**
 * @brief Writes tones of a SoundGenerator as 16 bit PCM with the requested
 * number of channels and volume to a Print.
 * @copyright GPLv3
 */
class ToneOutput {
 public:
  void setOutput(Print& out) { p_print = &out; }

  void setGenerator(audio_tools::SoundGenerator<int16_t>& generator) {
    p_generator = &generator;
  }

  audio_tools::SoundGenerator<int16_t>& generator() { return *p_generator; }

  /// Starts the generator: volume is the amplitude (0.0 - 1.0)
  bool begin(audio_tools::AudioInfo info, float volume) {
    channels = info.channels;
    audio_tools::AudioInfo gen_info = info;
    gen_info.channels = 1;  // we copy the sample to all channels ourselves
    if (!p_generator->begin(gen_info)) {
      LOGE("tone generator could not be started");
      return false;
    }
    gain = amplitude(volume);
    return true;
  }

  /// Writes a tone (frequency 0 = silence) for the indicated number of
  /// samples (frames)
  void tone(float freq, size_t nsamples) {
    if (p_print == nullptr) return;
    p_generator->setFrequency(freq);
    int frames_per_buffer = BUFFER_FRAMES * 2 / channels;
    while (nsamples > 0) {
      int frames = nsamples < (size_t)frames_per_buffer ? nsamples
                                                        : frames_per_buffer;
      int idx = 0;
      for (int j = 0; j < frames; j++) {
        int16_t sample = freq == 0.0f ? 0 : scale(p_generator->readSample());
        for (int ch = 0; ch < channels; ch++) buffer[idx++] = sample;
      }
      // blocking write
      const uint8_t* data = (const uint8_t*)buffer;
      size_t open = idx * sizeof(int16_t);
      while (open > 0) {
        size_t written = p_print->write(data, open);
        data += written;
        open -= written;
      }
      nsamples -= frames;
    }
  }

  /// minimodem's conversion of the volume to a 16 bit amplitude
  static int32_t amplitude(float volume) {
    float mag = 32767.0f * volume + 0.5f;
    if (volume > 1.0f) mag = 32767;  // clamp to 1.0 to avoid overflow
    if (mag < 1) mag = 1;            // "short epsilon"
    return (int)mag;
  }

 protected:
  static constexpr int BUFFER_FRAMES = 64;
  Print* p_print = nullptr;
  FSKToneGenerator<int16_t> default_generator;
  audio_tools::SoundGenerator<int16_t>* p_generator = &default_generator;
  int32_t gain = 32767;  // volume in Q15
  int channels = 1;
  int16_t buffer[BUFFER_FRAMES * 2];

  int16_t scale(int16_t sample) {
    if (gain == 32767) return sample;
    return (int16_t)((int32_t)sample * gain / 32767);
  }
};

/**
 * @brief Writes float samples (-1.0 to 1.0) as 16 bit PCM with the requested
 * number of channels and volume to a Print.
 * @copyright GPLv3
 */
class SampleOutput {
 public:
  void setOutput(Print& out) { p_print = &out; }

  void begin(int channels, float volume) {
    this->channels = channels;
    gain = ToneOutput::amplitude(volume);
    idx = 0;
  }

  /// Writes a sample to all channels
  void write(float sample) {
    if (sample > 1.0f) sample = 1.0f;
    if (sample < -1.0f) sample = -1.0f;
    int16_t value = lroundf(sample * gain);
    for (int ch = 0; ch < channels; ch++) {
      buffer[idx++] = value;
      if (idx == BUFFER_SIZE) flush();
    }
  }

  /// Writes the buffered samples
  void flush() {
    if (p_print == nullptr || idx == 0) {
      idx = 0;
      return;
    }
    const uint8_t* data = (const uint8_t*)buffer;
    size_t open = idx * sizeof(int16_t);
    while (open > 0) {
      size_t written = p_print->write(data, open);
      data += written;
      open -= written;
    }
    idx = 0;
  }

 protected:
  static constexpr int BUFFER_SIZE = 128;
  Print* p_print = nullptr;
  int16_t buffer[BUFFER_SIZE];
  int idx = 0;
  int channels = 1;
  int32_t gain = 32767;
};

/**
 * @brief Assembles the PCM frames from the written bytes and provides the
 * samples of the selected channel as float (-1.0 to 1.0). Supports 8, 16 and
 * 32 bit signed samples.
 * @copyright GPLv3
 */
class PcmInput {
 public:
  static constexpr int MAX_CHANNELS = 8;

  /// Defines the format: returns false (and logs the reason) if not supported
  bool begin(int bits_per_sample, int channels, int channel) {
    if (!(bits_per_sample == 8 || bits_per_sample == 16 ||
          bits_per_sample == 32)) {
      LOGE("bits_per_sample must be 8, 16 or 32, not %d", bits_per_sample);
      return false;
    }
    if (channels < 1 || channels > MAX_CHANNELS) {
      LOGE("invalid number of channels: %d", channels);
      return false;
    }
    if (channel >= channels) {
      LOGE("channel %d is not available (channels=%d)", channel, channels);
      return false;
    }
    bits = bits_per_sample;
    int bytes = bits / 8;
    frame_bytes = bytes * channels;
    sample_offset = bytes * channel;
    partial_len = 0;
    return true;
  }

  /// Drops an incomplete frame
  void reset() { partial_len = 0; }

  /// Calls on_sample(float) for each complete frame
  template <class F>
  void write(const uint8_t* data, size_t len, F on_sample) {
    size_t j = 0;
    // complete a frame from the previous call
    while (partial_len > 0 && j < len) {
      partial[partial_len++] = data[j++];
      if (partial_len == frame_bytes) {
        on_sample(sampleValue(partial + sample_offset));
        partial_len = 0;
      }
    }
    // complete frames
    for (; len - j >= (size_t)frame_bytes; j += frame_bytes)
      on_sample(sampleValue(data + j + sample_offset));
    // keep the remaining bytes for the next call
    while (j < len) partial[partial_len++] = data[j++];
  }

 protected:
  uint8_t partial[MAX_CHANNELS * 4];
  int partial_len = 0;
  int frame_bytes = 2;
  int sample_offset = 0;
  int bits = 16;

  float sampleValue(const uint8_t* ptr) {
    switch (bits) {
      case 8:
        return (int8_t)ptr[0] / 128.0f;
      case 16: {
        int16_t v;
        memcpy(&v, ptr, sizeof(v));
        return v / 32768.0f;
      }
      default: {
        int32_t v;
        memcpy(&v, ptr, sizeof(v));
        return v / 2147483648.0f;
      }
    }
  }
};

/**
 * @brief FIFO for received data which grows when needed: e.g. one decoding
 * step can produce more data than expected (like a Caller-ID message).
 * @copyright GPLv3
 */
class ByteQueue : public Print {
 public:
  size_t write(uint8_t ch) override {
    data.push_back(ch);
    return 1;
  }
  int available() { return data.size() - pos; }
  int peek() { return available() > 0 ? data[pos] : -1; }
  int read() {
    int result = peek();
    if (result < 0) return result;
    if (++pos == (size_t)data.size()) {
      clear();
    } else if (pos >= 64 && pos * 2 >= (size_t)data.size()) {
      // drop the consumed data
      size_t n = data.size() - pos;
      memmove(data.data(), data.data() + pos, n);
      data.resize(n);
      pos = 0;
    }
    return result;
  }
  void clear() {
    data.clear();
    pos = 0;
  }

 protected:
  audio_tools::Vector<uint8_t> data;
  size_t pos = 0;
};

/**
 * @brief Common API of the modulators (FSKEncoder, MFSKEncoder): the written
 * data is converted to 16 bit PCM audio which is written to the output.
 * @copyright GPLv3
 */
class ModemEncoder : public audio_tools::AudioEncoder {
 public:
  void setOutput(Print& out) override { tones.setOutput(out); }

  /// Replaces the default tone generator: the generator must change the
  /// frequency immediately and keep the phase continuous in setFrequency()
  /// (e.g. the AudioTools FastIntSineGenerator). Call it before begin().
  void setGenerator(audio_tools::SoundGenerator<int16_t>& generator) {
    tones.setGenerator(generator);
  }

  /// Provides the active tone generator
  audio_tools::SoundGenerator<int16_t>& getGenerator() {
    return tones.generator();
  }

  /// Ends the transmission (and adds optional silence): returns true if a
  /// transmission was ended
  virtual bool endTransmission(size_t silence_samples = 0) = 0;

  /// true between the first character and endTransmission()
  virtual bool isTransmitting() = 0;

  /// Sends an idle carrier: returns false if this is not supported
  virtual bool writeIdle(size_t nsamples) {
    (void)nsamples;
    return false;
  }

  /// Writes the indicated number of samples of silence
  void writeSilence(size_t nsamples) {
    if (*this) tones.tone(0, nsamples);
  }

  /// Writes a tone for the indicated number of samples
  void writeTone(float freq, size_t nsamples) {
    if (*this) tones.tone(freq, nsamples);
  }

  /// Ends the transmission
  void flush() { endTransmission(); }

  /// Symbols (bits for FSK) per second
  virtual float symbolRate() = 0;

  const char* mime() override { return "audio/pcm"; }

 protected:
  ToneOutput tones;
};

/**
 * @brief Common API of the demodulators (FSKDecoder, MFSKDecoder): PCM audio
 * data is written to the decoder and the decoded data is written to the
 * output. Status messages (### CARRIER and ### NOCARRIER) are written to the
 * status output.
 * @copyright GPLv3
 */
class ModemDecoder : public audio_tools::AudioDecoder {
 public:
  /// Defines the output for the ### CARRIER / ### NOCARRIER messages
  void setStatusOutput(Print& out) { p_status = &out; }

  /// Suppresses the status messages
  void setQuiet(bool quiet) { is_quiet = quiet; }

  /// true while a carrier is detected
  virtual bool isCarrier() = 0;

  /// true if the decoding was stopped (rx_one)
  virtual bool isFinished() = 0;

  /// Processes all buffered samples (e.g. at the end of the input)
  virtual void flush() = 0;

  const char* mime() override { return "audio/pcm"; }

 protected:
  Print* p_status = nullptr;
  bool is_quiet = false;

  void status(const char* fmt, ...) {
    if (p_status == nullptr || is_quiet) return;
    char tmp[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    p_status->print(tmp);
  }
};

}  // namespace tinymodem
