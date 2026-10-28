/**
 * @file mfsk-loopback.ino
 * @brief Runs on any board without additional hardware: the text is modulated
 * by the multi-tone MFSKEncoder and the generated PCM data is demodulated
 * again by the MFSKDecoder which prints the received text to Serial.
 */
#include "AudioTools.h"
#include "TinyModem.h"

MFSKDecoder decoder;                                  // PCM -> text
EncodedAudioOutput receiver(&Serial, &decoder);       // decoded text to Serial
MFSKEncoder encoder;                                  // text -> PCM
EncodedAudioOutput transmitter(&receiver, &encoder);  // PCM to receiver
MFSKConfig cfg;

void setup() {
  Serial.begin(115200);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);

  cfg.setMode("mfsk16");  // 16 tones, 500 bit/s
  cfg.sample_rate = 8000;

  decoder.setStatusOutput(Serial);  // ### CARRIER / ### NOCARRIER
  decoder.begin(cfg);
  encoder.begin(cfg);
  receiver.begin(cfg);
  transmitter.begin(cfg);
}

void loop() {
  transmitter.println("Hello from the MFSK Modem!");
  // send the end marker and some silence
  encoder.endTransmission(cfg.sample_rate / 2);
  delay(1000);
}
