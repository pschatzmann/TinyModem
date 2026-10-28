/**
 * @file packet-modem.ino
 * @brief Reliable serial over audio: each line which you type in the Serial
 * Monitor is sent as packet with CRC and Reed-Solomon error correction over
 * the multi-tone MFSK modem. Only correct lines are printed on the receiving
 * side. This needs an audio codec board which supports input and output
 * (e.g. an AudioKit or an I2S DAC + an I2S ADC or microphone).
 *
 * You can test it with the command line tool on your desktop:
 * `tinymodem --rx --packets mfsk16` and
 * `tinymodem --tx --packets mfsk16`.
 */
#include "AudioTools.h"
#include "TinyModem.h"

I2SStream i2s;                       // audio input and output
MFSKStream modem(i2s, RXTX_MODE);    // multi-tone FSK
PacketStream<64, 16> packets(modem); // CRC + error correction
unsigned last_dropped = 0;

void setup() {
  Serial.begin(115200);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);

  MFSKConfig cfg = modem.defaultConfig();  // mfsk16: 500 bit/s
  cfg.sample_rate = 22050;
  cfg.channels = 2;

  auto i2s_cfg = i2s.defaultConfig(RXTX_MODE);
  i2s_cfg.copyFrom(cfg);
  i2s.begin(i2s_cfg);

  modem.begin(cfg);
  packets.begin();
}

void loop() {
  // transmit: one packet per line
  while (Serial.available()) {
    char ch = Serial.read();
    packets.write(ch);
    if (ch == '\n') packets.flush();  // send the packet
  }
  // receive: only correct data is provided
  while (packets.available()) {
    Serial.write(packets.read());
  }
  // report packets which could not be corrected
  const PacketStatistics& stats = packets.statistics();
  if (stats.dropped != last_dropped) {
    last_dropped = stats.dropped;
    Serial.printf("[%u packets dropped]\n", stats.dropped);
  }
}
