/**
 * @file ggwave-send-i2s.ino
 * @brief Transmits a ggwave message every few seconds via an I2S DAC (e.g. a
 * PCM5102) and a speaker. You can receive it with the Waver app, with the
 * ggwave web demo or on your desktop with `tinymodem --rx ggwave-fast`.
 */
#include "AudioTools.h"
#include "TinyModem.h"

I2SStream i2s;                                 // audio output
GGWaveEncoder encoder;                         // text -> PCM
EncodedAudioOutput modem(&i2s, &encoder);      // Print API
GGWaveConfig cfg;

void setup() {
  Serial.begin(115200);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);

  cfg.protocol = GGWAVE_AUDIBLE_FAST;  // ggwave protocol id 1
  cfg.sample_rate = 48000;  // 44100 is not supported
  cfg.channels = 2;         // most I2S DACs are stereo
  cfg.volume = 0.5;

  auto i2s_cfg = i2s.defaultConfig(TX_MODE);
  i2s_cfg.copyFrom(cfg);
  // i2s_cfg.pin_bck = 14; i2s_cfg.pin_ws = 15; i2s_cfg.pin_data = 22;
  i2s.begin(i2s_cfg);

  encoder.begin(cfg);
  modem.begin(cfg);
}

void loop() {
  modem.print("Hello from Arduino");
  // send the message and 2 seconds of silence
  encoder.endTransmission(cfg.sample_rate * 2);
}
