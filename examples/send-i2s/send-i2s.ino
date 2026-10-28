/**
 * @file send-i2s.ino
 * @brief Transmits a message every few seconds via an I2S DAC (e.g. a
 * PCM5102). You can receive it on your desktop with
 * `minimodem --rx 1200` or `tinymodem --rx 1200`.
 */
#include "AudioTools.h"
#include "TinyModem.h"

I2SStream i2s;                               // audio output
FSKEncoder encoder;                          // text -> PCM
EncodedAudioOutput modem(&i2s, &encoder);    // Print API
ModemConfig cfg;

void setup() {
  Serial.begin(115200);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);

  cfg.setMode("1200");  // or "300", "rtty", "same", "V.21" ...
  cfg.sample_rate = 22050;
  cfg.channels = 2;  // most I2S DACs are stereo
  cfg.volume = 0.5;

  auto i2s_cfg = i2s.defaultConfig(TX_MODE);
  i2s_cfg.copyFrom(cfg);
  // i2s_cfg.pin_bck = 14; i2s_cfg.pin_ws = 15; i2s_cfg.pin_data = 22;
  i2s.begin(i2s_cfg);

  encoder.begin(cfg);
  modem.begin(cfg);
}

void loop() {
  modem.println("Hello from Arduino");
  // send the trailer and 2 seconds of silence
  encoder.endTransmission(cfg.sample_rate * 2);
}
