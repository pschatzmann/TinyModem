/**
 * @file aprs-beacon.ino
 * @brief Sends an APRS position report every minute via an I2S DAC: connect
 * the audio output to the microphone input of a 2m radio (with PTT control)
 * or decode it on your desktop with direwolf or
 * `tinymodem --rx aprs`. Replace N0CALL with your callsign: you need an
 * amateur radio license to transmit.
 */
#include "AudioTools.h"
#include "TinyModem.h"

I2SStream i2s;                   // audio output
AX25Stream aprs(i2s);            // transmit only
AX25Config cfg;

void setup() {
  Serial.begin(115200);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);

  cfg = aprs.defaultConfig();    // 1200 baud AFSK
  cfg.sample_rate = 22050;
  cfg.channels = 2;
  cfg.volume = 0.5;

  auto i2s_cfg = i2s.defaultConfig(TX_MODE);
  i2s_cfg.copyFrom(cfg);
  i2s.begin(i2s_cfg);

  aprs.begin(cfg);
}

void loop() {
  // one frame per line in the monitor format: SRC>DEST,PATH:info
  aprs.println("N0CALL-9>APRS,WIDE1-1,WIDE2-1:!4903.50N/07201.75W-TinyModem");
  aprs.flush();  // end of the transmission
  delay(60000);
}
