/**
 * @file benchmark.ino
 * @brief Measures how fast the modulators and the demodulators run on your
 * board. A real time factor above 1.0 means that the board is fast enough
 * for the selected mode and sample rate. No audio hardware is needed.
 */
#include "AudioTools.h"
#include "TinyModem.h"

const int SAMPLE_RATE = 8000;
const char* TEXT = "The quick brown fox jumps over the lazy dog 0123456789\n";

/// Print which collects the generated audio
class AudioBuffer : public Print {
 public:
  Vector<uint8_t> data;
  size_t write(uint8_t ch) override { return write(&ch, 1); }
  size_t write(const uint8_t* buffer, size_t len) override {
    for (size_t j = 0; j < len; j++) {
      uint8_t value = buffer[j];
      data.push_back(value);
    }
    return len;
  }
};

/// Print which counts the decoded characters
class Counter : public Print {
 public:
  size_t count = 0;
  size_t write(uint8_t) override {
    count++;
    return 1;
  }
};

void report(const char* name, const char* what, uint32_t us, float audio_sec) {
  Serial.printf("%s %s: %.2f sec of audio in %.3f sec: real time factor %.1f\n",
                name, what, audio_sec, us / 1000000.0f,
                audio_sec * 1000000.0f / us);
}

/// Modulates the text, demodulates it again and reports the speed
template <class Encoder, class Decoder, class Config>
void benchmark(const char* name, Config cfg, const char* text = TEXT) {
  // modulate
  AudioBuffer audio;
  Encoder encoder(audio);
  encoder.begin(cfg);
  uint32_t start = micros();
  encoder.write((const uint8_t*)text, strlen(text));
  encoder.end();
  uint32_t tx_us = micros() - start;
  float audio_sec = audio.data.size() / 2.0f / cfg.sample_rate;
  report(name, "modulate", tx_us, audio_sec);

  // demodulate in chunks like an audio stream
  Counter counter;
  Decoder decoder(counter);
  decoder.begin(cfg);
  start = micros();
  for (size_t pos = 0; pos < audio.data.size(); pos += 512) {
    size_t len = min((size_t)512, audio.data.size() - pos);
    decoder.write(audio.data.data() + pos, len);
  }
  decoder.end();
  uint32_t rx_us = micros() - start;
  report(name, "demodulate", rx_us, audio_sec);
  Serial.printf("%s decoded %d of %d characters\n", name, (int)counter.count,
                (int)strlen(text));
}

void setup() {
  Serial.begin(115200);
  while (!Serial);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);

  ModemConfig fsk;
  fsk.setMode("1200");
  fsk.sample_rate = SAMPLE_RATE;
  benchmark<FSKEncoder, FSKDecoder>("1200", fsk);

  for (const char* mode : {"mfsk16", "mfsk64"}) {
    MFSKConfig mfsk;
    mfsk.setMode(mode);
    mfsk.sample_rate = SAMPLE_RATE;
    benchmark<MFSKEncoder, MFSKDecoder>(mode, mfsk);
  }

  PSKConfig psk;
  psk.setMode("psk125");
  psk.sample_rate = SAMPLE_RATE;
  benchmark<PSKEncoder, PSKDecoder>("psk125", psk);

  OFDMConfig ofdm;
  ofdm.setMode("ofdm32");
  ofdm.sample_rate = SAMPLE_RATE;
  benchmark<OFDMEncoder, OFDMDecoder>("ofdm32", ofdm);

  AX25Config ax25;
  ax25.sample_rate = SAMPLE_RATE;
  benchmark<AX25Encoder, AX25Decoder>(
      "aprs", ax25, "N0CALL>APRS:>The quick brown fox jumps over the lazy dog\n");
}

void loop() {}
