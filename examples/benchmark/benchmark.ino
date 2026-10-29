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

/// Print which counts the decoded characters
class Counter : public Print {
 public:
  size_t count = 0;
  size_t write(uint8_t) override {
    count++;
    return 1;
  }
};

/// Print which forwards the generated audio to the decoder and measures the
/// time which is spent in the decoder: so the audio doesn't need any memory
class DecoderInput : public Print {
 public:
  DecoderInput(AudioDecoder& decoder) : decoder(decoder) {}
  size_t write(uint8_t ch) override { return write(&ch, 1); }
  size_t write(const uint8_t* data, size_t len) override {
    uint32_t start = micros();
    decoder.write(data, len);
    decoder_us += micros() - start;
    bytes += len;
    return len;
  }
  uint32_t decoder_us = 0;
  size_t bytes = 0;

 protected:
  AudioDecoder& decoder;
};

void report(const char* name, const char* what, uint32_t us, float audio_sec) {
  Serial.printf("%s %s: %.2f sec of audio in %.3f sec: real time factor %.1f\n",
                name, what, audio_sec, us / 1000000.0f,
                audio_sec * 1000000.0f / us);
}

/// Modulates the text, demodulates it at the same time and reports the speed
/// of both. The encoder and decoder are allocated on the heap: some of them
/// are too big for the stack of loop().
template <class Encoder, class Decoder, class Config>
void benchmark(const char* name, Config cfg, const char* text = TEXT) {
  Counter counter;
  Decoder& decoder = *new Decoder(counter);
  decoder.begin(cfg);
  DecoderInput input(decoder);
  Encoder& encoder = *new Encoder(input);
  encoder.begin(cfg);

  uint32_t start = micros();
  encoder.write((const uint8_t*)text, strlen(text));
  encoder.end();
  uint32_t total_us = micros() - start;
  start = micros();
  decoder.end();
  uint32_t rx_us = input.decoder_us + (micros() - start);
  uint32_t tx_us = total_us - input.decoder_us;

  float audio_sec = input.bytes / 2.0f / cfg.sample_rate;
  report(name, "modulate", tx_us, audio_sec);
  report(name, "demodulate", rx_us, audio_sec);
  Serial.printf("%s decoded %d of %d characters\n", name, (int)counter.count,
                (int)strlen(text));
  delete &encoder;
  delete &decoder;
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

  for (const char* mode : {"psk125", "qpsk125", "8psk250"}) {
    PSKConfig psk;
    psk.setMode(mode);
    psk.sample_rate = SAMPLE_RATE;
    benchmark<PSKEncoder, PSKDecoder>(mode, psk);
  }

  OFDMConfig ofdm;
  ofdm.setMode("ofdm32");
  ofdm.sample_rate = SAMPLE_RATE;
  benchmark<OFDMEncoder, OFDMDecoder>("ofdm32", ofdm);

  for (const char* mode : {"aprs", "fx25", "il2p"}) {
    AX25Config ax25;
    ax25.setMode(mode);
    ax25.sample_rate = SAMPLE_RATE;
    benchmark<AX25Encoder, AX25Decoder>(
        mode, ax25, "N0CALL>APRS:>The quick brown fox jumps over the lazy dog\n");
  }

  // Olivia is slow (1 - 5 characters per second): we use a shorter text
  for (const char* mode : {"olivia-8-250", "olivia-32-1000", "contestia-8-250"}) {
    OliviaConfig olivia;
    olivia.setMode(mode);
    olivia.sample_rate = SAMPLE_RATE;
    benchmark<OliviaEncoder, OliviaDecoder>(mode, olivia, "HELLO OLIVIA\n");
  }

  for (const char* mode : {"css7", "css10"}) {
    CSSConfig css;
    css.setMode(mode);
    css.sample_rate = SAMPLE_RATE;
    benchmark<CSSEncoder, CSSDecoder>(mode, css);
  }

  DTMFConfig dtmf;
  dtmf.sample_rate = SAMPLE_RATE;
  benchmark<DTMFEncoder, DTMFDecoder>("dtmf", dtmf, "0123456789*#ABCD");
}

void loop() {}
