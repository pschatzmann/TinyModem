/**
 * @file receive-i2s.ino
 * @brief Receives data from an I2S microphone or ADC and prints it to Serial.
 * You can send data from your desktop with
 * `echo "Hello Arduino" | minimodem --tx 1200` (or `tinymodem`).
 */
#include "AudioTools.h"
#include "TinyModem.h"

I2SStream i2s;                                  // audio input
FSKDecoder decoder;                             // PCM -> text
EncodedAudioOutput modem(&Serial, &decoder);    // decoded text to Serial
StreamCopy copier(modem, i2s);                  // copy i2s -> modem
ModemConfig cfg;

void setup() {
  Serial.begin(115200);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);

  cfg.setMode("1200");
  cfg.sample_rate = 22050;
  // I2S microphones like the INMP441 provide 32 bit stereo data
  cfg.channels = 2;
  cfg.bits_per_sample = 32;
  cfg.channel = 0;  // channel which is used for decoding

  auto i2s_cfg = i2s.defaultConfig(RX_MODE);
  i2s_cfg.copyFrom(cfg);
  i2s.begin(i2s_cfg);

  decoder.setStatusOutput(Serial);  // ### CARRIER / ### NOCARRIER
  decoder.begin(cfg);
  modem.begin(cfg);
}

void loop() { copier.copy(); }
