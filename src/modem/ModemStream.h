#pragma once

#include <string.h>

#include "AudioTools.h"
#include "AX25Decoder.h"
#include "AX25Encoder.h"
#include "FSKDecoder.h"
#include "FSKEncoder.h"
#include "MFSKDecoder.h"
#include "MFSKEncoder.h"
#include "PSKDecoder.h"
#include "PSKEncoder.h"
#include "OFDMDecoder.h"
#include "OFDMEncoder.h"
#include "ModemBase.h"
#include "ModemConfig.h"

namespace tinymodem {

/**
 * @brief Arduino Stream API for a modem (encoder and decoder): the written
 * characters are modulated to the audio output and the audio input is
 * demodulated into characters that can be read. You can use only the output
 * (transmit only), only the input (receive only) or both.
 *
 * The received data is only processed when you call available(), read(),
 * peek() or update(), so call one of them regularly in your loop(). No
 * decoded data is lost: the input is only read while less than
 * rx_buffer_size bytes are waiting to be read.
 * @copyright GPLv3
 */
template <class Encoder, class Decoder, class Config>
class BasicModemStream : public Stream {
 public:
  using ConfigType = Config;

  BasicModemStream() = default;
  /// transmit only
  BasicModemStream(Print& audio_out) { setOutput(audio_out); }
  /// transmit and receive with separate output and input
  BasicModemStream(Print& audio_out, Stream& audio_in) {
    setOutput(audio_out);
    setInput(audio_in);
  }
  /// use the same audio stream: TX_MODE (transmit only), RX_MODE (receive
  /// only) or RXTX_MODE (both)
  BasicModemStream(Stream& audio, audio_tools::RxTxMode mode) {
    if (mode & audio_tools::TX_MODE) setOutput(audio);
    if (mode & audio_tools::RX_MODE) setInput(audio);
  }

  /// Defines the audio output (transmitter)
  void setOutput(Print& audio_out) { p_audio_out = &audio_out; }
  /// Defines the audio input (receiver)
  void setInput(Stream& audio_in) { p_audio_in = &audio_in; }
  /// Defines the output for the ### CARRIER / ### NOCARRIER messages
  void setStatusOutput(Print& out) { decoder.setStatusOutput(out); }

  Config defaultConfig() { return encoder.defaultConfig(); }

  /// Starts the modem: the input is only read while less than rx_buffer_size
  /// decoded bytes are waiting to be read
  bool begin(Config cfg, int rx_buffer_size = 256) {
    bool ok = true;
    if (p_audio_out != nullptr) {
      encoder.setOutput(*p_audio_out);
      ok = encoder.begin(cfg) && ok;
    }
    if (p_audio_in != nullptr) {
      rx_limit = rx_buffer_size;
      rx_queue.clear();
      decoder.setOutput(rx_queue);
      ok = decoder.begin(cfg) && ok;
    }
    return ok;
  }

  void end() {
    encoder.end();
    decoder.end();
  }

  size_t write(uint8_t ch) override { return encoder.write(&ch, 1); }

  size_t write(const uint8_t* data, size_t len) override {
    return encoder.write(data, len);
  }

  /// Sends the trailer: call this at the end of a message
  void flush() override { encoder.endTransmission(); }

  int available() override {
    update();
    return rx_queue.available();
  }

  int read() override {
    update();
    return rx_queue.read();
  }

  int peek() override {
    update();
    return rx_queue.peek();
  }

  /// Reads the available data (does not wait for more data)
  size_t readBytes(uint8_t* data, size_t len) {
    size_t n = 0;
    while (n < len) {
      int ch = read();
      if (ch < 0) break;
      data[n++] = ch;
    }
    return n;
  }

  size_t readBytes(char* data, size_t len) {
    return readBytes((uint8_t*)data, len);
  }

  /// Reads the available audio input and feeds it to the decoder: returns
  /// the number of processed bytes
  size_t update() {
    if (p_audio_in == nullptr || !decoder) return 0;
    // stop reading the input while the received data is not consumed
    if (rx_queue.available() >= rx_limit) return 0;
    return readInput();
  }

  /// true while a carrier is detected
  bool isCarrier() { return decoder.isCarrier(); }

  Encoder& getEncoder() { return encoder; }
  Decoder& getDecoder() { return decoder; }

 protected:
  using ReceiveQueue = ByteQueue;

  Print* p_audio_out = nullptr;
  Stream* p_audio_in = nullptr;
  Encoder encoder;
  Decoder decoder;
  ReceiveQueue rx_queue;
  int rx_limit = 256;
  uint8_t copy_buffer[256];

  /// Reads the available audio input and feeds it to the decoder
  size_t readInput() {
    int len = p_audio_in->available();
    if (len <= 0) return 0;
    if (len > (int)sizeof(copy_buffer)) len = sizeof(copy_buffer);
    len = p_audio_in->readBytes(copy_buffer, len);
    if (len <= 0) return 0;
    return decoder.write(copy_buffer, len);
  }
};

/**
 * @brief Arduino Stream API for the minimodem compatible FSK modem: print()
 * transmits, read() receives. See BasicModemStream.
 * @copyright GPLv3
 */
class ModemStream : public BasicModemStream<FSKEncoder, FSKDecoder, ModemConfig> {
 public:
  using BasicModemStream::BasicModemStream;
};

/**
 * @brief Arduino Stream API for the multi-tone FSK modem: print() transmits,
 * read() receives. See BasicModemStream.
 * @copyright GPLv3
 */
class MFSKStream : public BasicModemStream<MFSKEncoder, MFSKDecoder, MFSKConfig> {
 public:
  using BasicModemStream::BasicModemStream;
};

/**
 * @brief Arduino Stream API for the AX.25 packet radio modem (APRS): print()
 * transmits the frames, read() provides the received frames, both as monitor
 * text lines or KISS frames. See BasicModemStream.
 * @copyright GPLv3
 */
class AX25Stream : public BasicModemStream<AX25Encoder, AX25Decoder, AX25Config> {
 public:
  using BasicModemStream::BasicModemStream;
};

/**
 * @brief Arduino Stream API for the PSK modem (PSK31 to PSK500): print()
 * transmits, read() receives. See BasicModemStream.
 * @copyright GPLv3
 */
class PSKStream : public BasicModemStream<PSKEncoder, PSKDecoder, PSKConfig> {
 public:
  using BasicModemStream::BasicModemStream;
};

/**
 * @brief Arduino Stream API for the OFDM modem: print() collects the data,
 * flush() sends it as frame, read() receives. See BasicModemStream.
 * @copyright GPLv3
 */
class OFDMStream : public BasicModemStream<OFDMEncoder, OFDMDecoder, OFDMConfig> {
 public:
  using BasicModemStream::BasicModemStream;
};

}  // namespace tinymodem
