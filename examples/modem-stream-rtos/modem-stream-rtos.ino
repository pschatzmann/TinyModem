/**
 * @file modem-stream-rtos.ino
 * @brief Serial over audio like the modem-stream example, but the received
 * audio is decoded in a separate FreeRTOS task: loop() is never blocked by
 * the decoding and no audio is lost while loop() is busy. Runs on ESP32 and
 * on RP2040: there select Tools > Operating System > FreeRTOS SMP.
 */
#if defined(ARDUINO_ARCH_RP2040) && defined(__FREERTOS)
#include <FreeRTOS.h>  // must be included before AudioTools.h
#endif
#include "AudioTools.h"
#include "TinyModemRTOS.h"

I2SStream i2s;  // audio input and output
ModemStreamRTOS modem(i2s, RXTX_MODE);

void setup() {
  Serial.begin(115200);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);

  ModemConfig cfg = modem.defaultConfig();  // 1200 baud Bell 202
  cfg.sample_rate = 22050;
  cfg.channels = 2;

  auto i2s_cfg = i2s.defaultConfig(RXTX_MODE);
  i2s_cfg.copyFrom(cfg);
  i2s.begin(i2s_cfg);

  // optional: receive task with 8 KB stack, priority 1, on core 0
  // modem.setTaskConfig(8 * 1024, 1, 0);
  modem.begin(cfg);
}

void loop() {
  // transmit
  while (Serial.available()) {
    char ch = Serial.read();
    modem.write(ch);
    if (ch == '\n') modem.flush();  // end of transmission
  }
  // receive: the data was already decoded by the receive task
  while (modem.available()) {
    Serial.write(modem.read());
  }
}
