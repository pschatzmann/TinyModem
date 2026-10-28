/**
 * @file modem-stream.ino
 * @brief Serial over audio: the ModemStream is a regular Arduino Stream.
 * What you type in the Serial Monitor is transmitted (when you press enter)
 * and the received data is printed. This needs an audio codec board which
 * supports input and output (e.g. an AudioKit or an I2S DAC + an I2S ADC or
 * microphone).
 */
#include "AudioTools.h"
#include "TinyModem.h"

I2SStream i2s;                 // audio input and output
ModemStream modem(i2s, RXTX_MODE);

void setup() {
  Serial.begin(115200);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);

  ModemConfig cfg = modem.defaultConfig();  // 1200 baud Bell 202
  cfg.sample_rate = 22050;
  cfg.channels = 2;

  auto i2s_cfg = i2s.defaultConfig(RXTX_MODE);
  i2s_cfg.copyFrom(cfg);
  i2s.begin(i2s_cfg);

  modem.begin(cfg);
}

void loop() {
  // transmit
  while (Serial.available()) {
    char ch = Serial.read();
    modem.write(ch);
    if (ch == '\n') modem.flush();  // end of transmission
  }
  // receive
  while (modem.available()) {
    Serial.write(modem.read());
  }
}
