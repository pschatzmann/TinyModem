#pragma once

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include "AudioTools.h"

namespace tinymodem {

/// Reverses the ordering of the bits on an integer
inline uint64_t bitReverse(uint64_t value, unsigned bits) {
  uint64_t out = 0;
  while (bits--) {
    out = (out << 1) | (value & 1);
    value >>= 1;
  }
  return out;
}

/// Gets "bits" bits from "value" starting "offset" bits from the start
inline uint64_t bitWindow(uint64_t value, unsigned offset, unsigned bits) {
  uint64_t mask = (bits >= 64) ? ~0ULL : ((1ULL << bits) - 1);
  return (value >> offset) & mask;
}

/**
 * @brief Converts between characters and the data bits of a frame
 * @copyright GPLv3
 */
class DataCodec {
 public:
  virtual ~DataCodec() = default;
  /// resets the internal state (called when the carrier is acquired)
  virtual void reset() {}
  /// Converts a character into 0, 1 or 2 data words: returns the count
  virtual int encode(uint32_t words[2], uint8_t ch) {
    words[0] = ch;
    return 1;
  }
  /// Decodes the data bits of a frame: returns the number of written bytes
  virtual size_t decode(Print& out, uint64_t bits, unsigned n_bits) = 0;
};

/// ASCII 8-bit data (passthrough)
class AsciiCodec : public DataCodec {
 public:
  size_t decode(Print& out, uint64_t bits, unsigned /*n_bits*/) override {
    return out.write((uint8_t)(bits & 0xFF));
  }
};

/// Raw N-bit binary data: prints the bits as '0' and '1' followed by newline
class BinaryCodec : public DataCodec {
 public:
  size_t decode(Print& out, uint64_t bits, unsigned n_bits) override {
    for (unsigned j = 0; j < n_bits; j++) out.write((bits >> j & 1) + '0');
    out.write('\n');
    return n_bits + 1;
  }
};

/// Baudot (ITA2) 5-bit data
class BaudotCodec : public DataCodec {
 public:
  /// Unshift on space
  void setUSOS(bool usos) { this->usos = usos; }

  void reset() override { charset = 1; }

  /// Forgets the shift state, so that the next encoded character is preceded
  /// by a LTRS or FIGS code (call at the start of each transmission)
  void resetShift() { charset = 0; }

  int encode(uint32_t words[2], uint8_t ch) override {
    ch = toupper(ch);
    if (ch >= 0x60) return 0;  // non-encodable
    int n = 0;
    uint8_t charset_mask = encodeTable(ch)[1];
    if ((charset & charset_mask) == 0) {
      if (charset_mask == 0) return 0;  // non-encodable
      if (charset == 0) charset = 1;
      if (charset_mask != 3) charset = charset_mask;
      words[n++] = charset == 1 ? LTRS : FIGS;
    }
    words[n++] = encodeTable(ch)[0];
    // TX un-shift on space
    if (ch == ' ' && usos) charset = 1;
    return n;
  }

  size_t decode(Print& out, uint64_t bits, unsigned /*n_bits*/) override {
    uint8_t databits = bits & 0x1F;
    if (databits == FIGS) {
      charset = 2;
      return 0;
    }
    if (databits == LTRS) {
      charset = 1;
      return 0;
    }
    // RX un-shift on space
    if (databits == SPACE && usos) charset = 1;
    // letters or U.S. figures
    return out.write(decodeTable(databits)[charset == 1 ? 0 : 1]);
  }

 protected:
  static constexpr uint8_t LTRS = 0x1F;
  static constexpr uint8_t FIGS = 0x1B;
  static constexpr uint8_t SPACE = 0x04;
  /// 0 unknown, 1 LTRS, 2 FIGS
  unsigned charset = 0;
  bool usos = true;

  /// letter, U.S. figs, CCITT No.2 figs (Europe)
  static const char* decodeTable(uint8_t idx) {
    static const char table[32][3] = {
        {'_', '^', '^'},  {'E', '3', '3'},   {0xA, 0xA, 0xA},
        {'A', '-', '-'},  {' ', ' ', ' '},   {'S', 0x7, '\''},
        {'I', '8', '8'},  {'U', '7', '7'},   {0xD, 0xD, 0xD},
        {'D', '$', '^'},  {'R', '4', '4'},   {'J', '\'', 0x7},
        {'N', ',', ','},  {'F', '!', '!'},   {'C', ':', ':'},
        {'K', '(', '('},  {'T', '5', '5'},   {'Z', '"', '+'},
        {'L', ')', ')'},  {'W', '2', '2'},   {'H', '#', '%'},
        {'Y', '6', '6'},  {'P', '0', '0'},   {'Q', '1', '1'},
        {'O', '9', '9'},  {'B', '?', '?'},   {'G', '&', '&'},
        {'%', '%', '%'},  {'M', '.', '.'},   {'X', '/', '/'},
        {'V', ';', '='},  {'%', '%', '%'},
    };
    return table[idx];
  }

  /// index: ascii char; values: bits, ltrs(1) or figs(2) or both(3)
  static const uint8_t* encodeTable(uint8_t idx) {
    static const uint8_t table[0x60][2] = {
        // 0x00
        {0x00, 3}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
        {0x05, 2}, {0, 0}, {0, 0}, {0x02, 3}, {0, 0}, {0, 0}, {0x08, 3},
        {0, 0}, {0, 0},
        // 0x10
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
        // 0x20:  ! " # $ % & ' ( ) * + , - . /
        {0x04, 3}, {0x0d, 2}, {0x11, 2}, {0x14, 2}, {0x09, 2}, {0, 0},
        {0x1a, 2}, {0x0b, 2}, {0x0f, 2}, {0x12, 2}, {0, 0}, {0x12, 2},
        {0x0c, 2}, {0x03, 2}, {0x1c, 2}, {0x1d, 2},
        // 0x30: 0-9 : ; < = > ?
        {0x16, 2}, {0x17, 2}, {0x13, 2}, {0x01, 2}, {0x0a, 2}, {0x10, 2},
        {0x15, 2}, {0x07, 2}, {0x06, 2}, {0x18, 2}, {0x0e, 2}, {0x1e, 2},
        {0, 0}, {0, 0}, {0, 0}, {0x19, 2},
        // 0x40: @ A-O
        {0, 0}, {0x03, 1}, {0x19, 1}, {0x0e, 1}, {0x09, 1}, {0x01, 1},
        {0x0d, 1}, {0x1a, 1}, {0x14, 1}, {0x06, 1}, {0x0b, 1}, {0x0f, 1},
        {0x12, 1}, {0x1c, 1}, {0x0c, 1}, {0x18, 1},
        // 0x50: P-Z [ \ ] ^ _
        {0x16, 1}, {0x17, 1}, {0x0a, 1}, {0x05, 1}, {0x10, 1}, {0x07, 1},
        {0x1e, 1}, {0x13, 1}, {0x1d, 1}, {0x15, 1}, {0x11, 1}, {0, 0},
        {0, 0}, {0, 0}, {0, 0}, {0, 0},
    };
    return table[idx];
  }
};

/// Caller-ID (USA SDMF/MDMF) decoder. See
/// http://melabs.com/resources/callerid.htm
class CallerIDCodec : public DataCodec {
 public:
  void reset() override {
    msg_type = 0;
    ndata = 0;
  }

  size_t decode(Print& out, uint64_t bits, unsigned /*n_bits*/) override {
    if (msg_type == 0) {
      if (bits == MSG_MDMF || bits == MSG_SDMF)
        msg_type = bits;
      else
        return 0;
      buf[ndata++] = bits;
      return 0;
    }
    if (ndata >= sizeof(buf)) {
      reset();
      return 0;
    }
    buf[ndata++] = bits;
    // collect bytes until we have the message length + type + checksum
    unsigned msglen = buf[1];
    if (ndata < msglen + 2) return 0;

    size_t n = out.print("CALLER-ID\n");
    n += msg_type == MSG_MDMF ? decodeMDMF(out) : decodeSDMF(out);
    reset();
    return n;
  }

 protected:
  static constexpr uint8_t MSG_MDMF = 0x80;
  static constexpr uint8_t MSG_SDMF = 0x04;
  static constexpr uint8_t DATA_DATETIME = 0x01;
  static constexpr uint8_t DATA_PHONE = 0x02;
  static constexpr uint8_t DATA_PHONE_NA = 0x04;
  static constexpr uint8_t DATA_NAME = 0x07;
  static constexpr uint8_t DATA_NAME_NA = 0x08;
  unsigned msg_type = 0;
  unsigned ndata = 0;
  uint8_t buf[256];

  static const char* typeName(unsigned type) {
    static const char* names[] = {"unknown0:", "Time:",     "Phone:",
                                  "unknown3:", "Phone:",    "unknown5:",
                                  "unknown6:", "Name:",     "Name:"};
    return names[type];
  }

  static size_t printFmt(Print& out, const char* fmt, ...) {
    char tmp[80];
    va_list args;
    va_start(args, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    return out.print(tmp);
  }

  static size_t printPhone(Print& out, const uint8_t* m) {
    return printFmt(out, "%.3s-%.3s-%.4s\n", m, m + 3, m + 6);
  }

  static size_t printDateTime(Print& out, const uint8_t* m) {
    return printFmt(out, "%.2s/%.2s %.2s:%.2s\n", m, m + 2, m + 4, m + 6);
  }

  size_t decodeMDMF(Print& out) {
    size_t n = 0;
    unsigned i = 0;
    unsigned msglen = buf[1];
    const uint8_t* m = buf + 2;
    while (i < msglen) {
      unsigned type = *m++;
      if (type > DATA_NAME_NA) return n;  // bad datastream
      unsigned len = *m++;
      if (m + 2 + len >= buf + sizeof(buf)) return n;  // bad datastream
      n += printFmt(out, "%-6s ", typeName(type));
      const char* prdata = nullptr;
      unsigned prlen = 0;
      switch (type) {
        case DATA_DATETIME:
          if (len >= 8) n += printDateTime(out, m);
          break;
        case DATA_PHONE:
          if (len == 10) {
            n += printPhone(out, m);
            break;
          }
          // fallthrough
        case DATA_NAME:
          prdata = (const char*)m;
          prlen = len;
          break;
        case DATA_PHONE_NA:
        case DATA_NAME_NA:
          if (len == 1 && *m == 'O') {
            prdata = "[N/A]";
            prlen = 5;
          } else if (len == 1 && *m == 'P') {
            prdata = "[blocked]";
            prlen = 9;
          }
          break;
        default:
          break;
      }
      if (prdata) {
        n += out.write((const uint8_t*)prdata, prlen);
        n += out.write('\n');
      }
      m += len;
      i += len + 2;
    }
    return n;
  }

  size_t decodeSDMF(Print& out) {
    size_t n = 0;
    unsigned msglen = buf[1];
    const uint8_t* m = buf + 2;
    n += printFmt(out, "%-6s ", typeName(DATA_DATETIME));
    n += printDateTime(out, m);
    if (msglen < 8) return n;
    m += 8;
    n += printFmt(out, "%-6s ", typeName(DATA_PHONE));
    unsigned len = msglen - 8;
    if (len == 10) {
      n += printPhone(out, m);
    } else {
      n += out.write(m, len);
      n += out.write('\n');
    }
    return n;
  }
};

/// UIC-751-3 ground-train or train-ground decoder
class UICCodec : public DataCodec {
 public:
  UICCodec(bool trainToGround = false) { train = trainToGround; }
  void setTrainToGround(bool flag) { train = flag; }

  size_t decode(Print& out, uint64_t input, unsigned /*n_bits*/) override {
    unsigned code = bitReverse(bitWindow(input, 24, 8), 8);
    char tmp[100];
    snprintf(tmp, sizeof(tmp),
             "Train ID: %X%X%X%X%X%X - Message: %02X (%s)\n",
             (unsigned)bitWindow(input, 0, 4), (unsigned)bitWindow(input, 4, 4),
             (unsigned)bitWindow(input, 8, 4), (unsigned)bitWindow(input, 12, 4),
             (unsigned)bitWindow(input, 16, 4),
             (unsigned)bitWindow(input, 20, 4), code, meaning(code));
    return out.print(tmp);
  }

 protected:
  bool train = false;

  struct Message {
    int code;
    const char* meaning;
  };

  const char* meaning(unsigned code) {
    static const Message ground_to_train[] = {
        {0x00, "Test"},          {0x02, "Run slower"},
        {0x03, "Extension of telegram"},
        {0x04, "Run faster"},    {0x06, "Written order"},
        {0x08, "Speech"},        {0x09, "Emergency stop"},
        {0x0C, "Announcem. by loudspeaker"},
        {0x55, "Idle"},          {-1, nullptr}};
    static const Message train_to_ground[] = {
        {0x08, "Communic. desired"},
        {0x0A, "Acknowl. of order"},
        {0x06, "Advice"},
        {0x00, "Test"},
        {0x09, "Train staff wish to comm."},
        {0x0C, "Telephone link desired"},
        {0x03, "Extension of telegram"},
        {-1, nullptr}};
    for (const Message* m = train ? train_to_ground : ground_to_train;
         m->code != -1; m++) {
      if (m->code == (int)code) return m->meaning;
    }
    return "Unknown";
  }
};

/// Print which replaces non printable characters with '.'
class PrintFilter : public Print {
 public:
  void setOutput(Print* out) { p_out = out; }
  size_t write(uint8_t ch) override {
    if (p_out == nullptr) return 0;
    return p_out->write(isprint(ch) || isspace(ch) ? ch : '.');
  }
  size_t write(const uint8_t* data, size_t len) override {
    for (size_t j = 0; j < len; j++) write(data[j]);
    return len;
  }

 protected:
  Print* p_out = nullptr;
};

}  // namespace tinymodem
