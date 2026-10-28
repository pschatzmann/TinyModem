/**
 * @file loopback.ino
 * @brief Runs on any board without additional hardware: the text is modulated
 * by the FSKEncoder and the generated PCM data is demodulated again by the
 * FSKDecoder which prints the received text to Serial.
 */
#include "AudioTools.h"
#include "TinyModem.h"

FSKDecoder decoder;                                   // PCM -> text
EncodedAudioOutput receiver(&Serial, &decoder);       // decoded text to Serial
FSKEncoder encoder;                                   // text -> PCM
EncodedAudioOutput transmitter(&receiver, &encoder);  // PCM to receiver
ModemConfig cfg;

void setup() {
  Serial.begin(115200);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);

  cfg.setMode("1200");  // Bell 202
  cfg.sample_rate = 8000;

  decoder.setStatusOutput(Serial);  // ### CARRIER / ### NOCARRIER
  decoder.begin(cfg);
  encoder.begin(cfg);
  receiver.begin(cfg);
  transmitter.begin(cfg);
}

void loop() {
  transmitter.println("Hello from the Modem!");
  // send the trailer and some silence, so that the decoder detects the end
  encoder.endTransmission(cfg.sample_rate / 2);
  delay(1000);
}
