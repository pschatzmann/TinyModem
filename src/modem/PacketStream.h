#pragma once

#include <string.h>

#include "AudioTools.h"
#include "AudioTools/Communication/FEC/ReedSolomon/rs.hpp"
#include "ModemBase.h"

namespace tinymodem {

/// CRC-16-CCITT (polynomial 0x1021, initial value 0xFFFF)
inline uint16_t crc16(const uint8_t* data, size_t len, uint16_t crc = 0xFFFF) {
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
  }
  return crc;
}

/// Statistics of the received packets
struct PacketStatistics {
  /// packets which were delivered
  unsigned received = 0;
  /// delivered packets which needed an error correction
  unsigned corrected = 0;
  /// number of corrected bytes
  unsigned corrected_bytes = 0;
  /// frames which could not be corrected or failed the CRC check
  unsigned dropped = 0;
};

/// Constants of the packet format
struct PacketFormat {
  static constexpr uint8_t FLAG = 0x7E;
  static constexpr uint8_t ESC = 0x7D;
  static constexpr uint8_t ESC_XOR = 0x20;
  static constexpr int CRC_BYTES = 2;
};

/**
 * @brief Splits the written data into packets with a CRC-16 and Reed-Solomon
 * forward error correction, which are framed with HDLC-style flags (0x7E)
 * and byte stuffing, and writes them to the output (e.g. a ModemStream).
 *
 * A packet is sent when MAX_PAYLOAD bytes were written or when you call
 * flush(). The packet is transmitted as payload + CRC + ECC_BYTES parity
 * bytes: shorter packets use a shortened Reed-Solomon code, so they don't
 * need any padding. The receiver (PacketDecoder) corrects up to
 * ECC_BYTES / 2 wrong bytes per packet.
 * @tparam MAX_PAYLOAD maximum number of data bytes per packet
 * @tparam ECC_BYTES number of Reed-Solomon parity bytes per packet
 * @copyright GPLv3
 */
template <uint8_t MAX_PAYLOAD = 64, uint8_t ECC_BYTES = 16>
class PacketEncoder : public Print {
  static_assert(MAX_PAYLOAD >= 1, "MAX_PAYLOAD must be at least 1");
  static_assert(ECC_BYTES >= 2, "ECC_BYTES must be at least 2");
  static_assert(MAX_PAYLOAD + PacketFormat::CRC_BYTES + ECC_BYTES <= 255,
                "a packet is limited to 255 bytes");

 public:
  PacketEncoder() = default;
  PacketEncoder(Print& out) { setOutput(out); }

  void setOutput(Print& out) { p_out = &out; }

  size_t write(uint8_t ch) override { return write(&ch, 1); }

  size_t write(const uint8_t* data, size_t len) override {
    if (p_out == nullptr) return 0;
    for (size_t j = 0; j < len; j++) {
      payload[payload_len++] = data[j];
      if (payload_len == MAX_PAYLOAD) writePacket();
    }
    return len;
  }

  /// Sends the buffered data as packet
  void flush() override {
    if (payload_len > 0) writePacket();
  }

  /// Number of buffered bytes which were not sent yet
  int pending() const { return payload_len; }

 protected:
  static constexpr int MSG_LEN = MAX_PAYLOAD + PacketFormat::CRC_BYTES;
  Print* p_out = nullptr;
  uint8_t payload[MAX_PAYLOAD];
  int payload_len = 0;
  RS::ReedSolomon<MSG_LEN, ECC_BYTES> rs;

  void writePacket() {
    // shortened code: the unused leading message bytes are 0 and not sent
    uint8_t msg[MSG_LEN] = {0};
    uint8_t codeword[MSG_LEN + ECC_BYTES];
    int skip = MAX_PAYLOAD - payload_len;
    memcpy(msg + skip, payload, payload_len);
    uint16_t crc = crc16(payload, payload_len);
    msg[MSG_LEN - 2] = crc >> 8;
    msg[MSG_LEN - 1] = crc & 0xFF;
    rs.Encode(msg, codeword);

    p_out->write(PacketFormat::FLAG);
    for (int j = skip; j < MSG_LEN + ECC_BYTES; j++) writeEscaped(codeword[j]);
    p_out->write(PacketFormat::FLAG);
    payload_len = 0;
  }

  void writeEscaped(uint8_t b) {
    if (b == PacketFormat::FLAG || b == PacketFormat::ESC) {
      p_out->write(PacketFormat::ESC);
      b ^= PacketFormat::ESC_XOR;
    }
    p_out->write(b);
  }
};

/**
 * @brief Receives the packets of a PacketEncoder: the written bytes (e.g.
 * the output of a modem decoder) are deframed, corrected and checked, and
 * the payload of the valid packets is written to the output.
 * @tparam MAX_PAYLOAD must be the same as in the PacketEncoder
 * @tparam ECC_BYTES must be the same as in the PacketEncoder
 * @copyright GPLv3
 */
template <uint8_t MAX_PAYLOAD = 64, uint8_t ECC_BYTES = 16>
class PacketDecoder : public Print {
 public:
  PacketDecoder() = default;
  PacketDecoder(Print& out) { setOutput(out); }

  void setOutput(Print& out) { p_out = &out; }

  size_t write(uint8_t ch) override {
    if (ch == PacketFormat::FLAG) {
      if (frame_len > 0 || overflow) processFrame();
      frame_len = 0;
      overflow = false;
      escaped = false;
      return 1;
    }
    if (ch == PacketFormat::ESC) {
      escaped = true;
      return 1;
    }
    if (escaped) {
      ch ^= PacketFormat::ESC_XOR;
      escaped = false;
    }
    if (frame_len < MAX_FRAME)
      frame[frame_len++] = ch;
    else
      overflow = true;
    return 1;
  }

  size_t write(const uint8_t* data, size_t len) override {
    for (size_t j = 0; j < len; j++) write(data[j]);
    return len;
  }

  /// Statistics of the received packets
  const PacketStatistics& statistics() const { return stats; }

  /// Resets the statistics and drops an incomplete frame
  void reset() {
    stats = PacketStatistics();
    frame_len = 0;
    overflow = false;
    escaped = false;
  }

 protected:
  static constexpr int MSG_LEN = MAX_PAYLOAD + PacketFormat::CRC_BYTES;
  static constexpr int MAX_FRAME = MSG_LEN + ECC_BYTES;
  Print* p_out = nullptr;
  uint8_t frame[MAX_FRAME];
  int frame_len = 0;
  bool overflow = false;
  bool escaped = false;
  PacketStatistics stats;
  RS::ReedSolomon<MSG_LEN, ECC_BYTES> rs;

  void processFrame() {
    int n = frame_len;
    if (overflow || n < PacketFormat::CRC_BYTES + ECC_BYTES + 1) {
      stats.dropped++;
      return;
    }
    // restore the full codeword of the shortened code
    uint8_t codeword[MAX_FRAME] = {0};
    int skip = MAX_FRAME - n;
    memcpy(codeword + skip, frame, n);
    uint8_t msg[MSG_LEN];
    if (rs.Decode(codeword, msg) != 0) {
      stats.dropped++;
      return;
    }
    // the padding must still be 0
    for (int j = 0; j < skip; j++) {
      if (msg[j] != 0) {
        stats.dropped++;
        return;
      }
    }
    int payload_len = n - ECC_BYTES - PacketFormat::CRC_BYTES;
    const uint8_t* payload = msg + skip;
    uint16_t crc = crc16(payload, payload_len);
    if (msg[MSG_LEN - 2] != (crc >> 8) || msg[MSG_LEN - 1] != (crc & 0xFF)) {
      stats.dropped++;
      return;
    }
    // count the corrected bytes
    uint8_t corrected[MAX_FRAME];
    rs.Encode(msg, corrected);
    int errors = 0;
    for (int j = skip; j < MAX_FRAME; j++)
      if (corrected[j] != codeword[j]) errors++;
    stats.received++;
    if (errors > 0) {
      stats.corrected++;
      stats.corrected_bytes += errors;
    }
    if (p_out != nullptr) p_out->write(payload, payload_len);
  }
};

/**
 * @brief Reliable data transfer over any Stream (e.g. a ModemStream, an
 * MFSKStream or a Serial): the written data is sent in packets with CRC-16
 * and Reed-Solomon forward error correction, and only correct data can be
 * read. Corrupted packets which can't be corrected are dropped.
 *
 * A packet is sent when MAX_PAYLOAD bytes were written or when you call
 * flush(), which also calls flush() of the underlying stream (which ends the
 * transmission of a modem). Both sides must use the same template
 * parameters.
 * @tparam MAX_PAYLOAD maximum number of data bytes per packet
 * @tparam ECC_BYTES number of Reed-Solomon parity bytes: up to ECC_BYTES / 2
 * wrong bytes per packet are corrected
 * @copyright GPLv3
 */
template <uint8_t MAX_PAYLOAD = 64, uint8_t ECC_BYTES = 16>
class PacketStream : public Stream {
 public:
  PacketStream() { decoder.setOutput(rx_queue); }
  /// send and receive
  PacketStream(Stream& io) {
    decoder.setOutput(rx_queue);
    setStream(io);
  }
  /// send only
  PacketStream(Print& out) {
    decoder.setOutput(rx_queue);
    setOutput(out);
  }

  /// Defines the stream for sending and receiving
  void setStream(Stream& io) {
    p_in = &io;
    setOutput(io);
  }

  /// Defines the output (send only)
  void setOutput(Print& out) {
    p_out = &out;
    encoder.setOutput(out);
  }

  /// Resets the receive state and the statistics
  bool begin() {
    decoder.setOutput(rx_queue);
    decoder.reset();
    rx_queue.clear();
    return true;
  }

  size_t write(uint8_t ch) override { return encoder.write(&ch, 1); }

  size_t write(const uint8_t* data, size_t len) override {
    return encoder.write(data, len);
  }

  /// Sends the buffered data as packet and flushes the underlying stream
  void flush() override {
    encoder.flush();
    if (p_out != nullptr) p_out->flush();
  }

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

  /// Reads the available data from the underlying stream
  void update() {
    if (p_in == nullptr) return;
    uint8_t buffer[64];
    int len;
    while ((len = p_in->available()) > 0) {
      if (len > (int)sizeof(buffer)) len = sizeof(buffer);
      len = p_in->readBytes(buffer, len);
      if (len <= 0) break;
      decoder.write(buffer, len);
    }
  }

  /// Statistics of the received packets
  const PacketStatistics& statistics() const { return decoder.statistics(); }

 protected:
  Print* p_out = nullptr;
  Stream* p_in = nullptr;
  PacketEncoder<MAX_PAYLOAD, ECC_BYTES> encoder;
  PacketDecoder<MAX_PAYLOAD, ECC_BYTES> decoder;
  ByteQueue rx_queue;
};

}  // namespace tinymodem
