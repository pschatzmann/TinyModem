/**
 * @file ggwave-loopback.ino
 * @brief Runs on any board without additional hardware: the text is modulated
 * by the ggwave compatible GGWaveEncoder and the generated PCM data is
 * demodulated again by the GGWaveDecoder which prints the received text to
 * Serial.
 */
#include "AudioTools.h"
#include "TinyModem.h"

GGWaveDecoder decoder;                                // PCM -> text
EncodedAudioOutput receiver(&Serial, &decoder);       // decoded text to Serial
GGWaveEncoder encoder;                                // text -> PCM
EncodedAudioOutput transmitter(&receiver, &encoder);  // PCM to receiver
GGWaveConfig cfg;

void setup() {
  Serial.begin(115200);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);

  cfg.protocol = GGWAVE_AUDIBLE_FAST;  // or cfg.setMode("ggwave-fast")
  cfg.sample_rate = 24000;             // must be a multiple of 187.5 Hz

  decoder.setStatusOutput(Serial);  // ### CARRIER / ### NOCARRIER
  decoder.begin(cfg);
  encoder.begin(cfg);
  receiver.begin(cfg);
  transmitter.begin(cfg);
}

void loop() {
  transmitter.print("Hello from ggwave!");  // up to 140 bytes per message
  // send the message and some silence
  encoder.endTransmission(cfg.sample_rate / 2);
  delay(1000);
}
