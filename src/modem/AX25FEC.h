#pragma once

#include <ctype.h>
#include <stdint.h>
#include <string.h>

#include "AX25.h"
#include "ReedSolomon.h"

namespace tinymodem {

/**
 * @brief FX.25: AX.25 frames with Reed-Solomon forward error correction (as
 * implemented by direwolf). The complete HDLC frame (flag, bit stuffed frame
 * with FCS, flags as fill) is the data part of a Reed-Solomon code word,
 * which is preceded by a 64 bit correlation tag that identifies the code. A
 * receiver without FX.25 still decodes the HDLC frame in the data part.
 * @copyright GPLv3
 */
class FX25 {
 public:
  /// a code (correlation tag)
  struct Tag {
    uint64_t value;
    /// transmitted data bytes
    int data;
    /// check bytes
    int check;
  };

  static constexpr int BLOCK_SIZE = 255;
  static constexpr int MAX_DATA = 239;
  static constexpr int FIRST_TAG = 1;
  static constexpr int LAST_TAG = 11;
  /// max number of wrong bits in a received tag
  static constexpr int TAG_TOLERANCE = 8;

  /// The tag 1 - 11
  static const Tag& tag(int index) {
    static const Tag tags[12] = {
        {0x566ED2717946107Eull, 0, 0},     // reserved
        {0xB74DB7DF8A532F3Eull, 239, 16},  // RS(255,239)
        {0x26FF60A600CC8FDEull, 128, 16},  // RS(144,128)
        {0xC7DC0508F3D9B09Eull, 64, 16},   // RS(80,64)
        {0x8F056EB4369660EEull, 32, 16},   // RS(48,32)
        {0x6E260B1AC5835FAEull, 223, 32},  // RS(255,223)
        {0xFF94DC634F1CFF4Eull, 128, 32},  // RS(160,128)
        {0x1EB7B9CDBC09C00Eull, 64, 32},   // RS(96,64)
        {0xDBF869BD2DBB1776ull, 32, 32},   // RS(64,32)
        {0x3ADB0C13DEAE2836ull, 191, 64},  // RS(255,191)
        {0xAB69DB6A543188D6ull, 128, 64},  // RS(192,128)
        {0x4A4ABEC4A724B796ull, 64, 64},   // RS(128,64)
    };
    return tags[index];
  }

  /// Finds the tag which matches the received 64 bits: returns 0 if none
  static int findTag(uint64_t bits) {
    for (int j = FIRST_TAG; j <= LAST_TAG; j++)
      if (popcount(bits ^ tag(j).value) <= TAG_TOLERANCE) return j;
    return 0;
  }

  /// Smallest tag with the indicated check bytes (16, 32, 64) for the data
  /// length: returns 0 if the data is too long
  static int pickTag(int check_bytes, int data_len) {
    int result = 0;
    for (int j = FIRST_TAG; j <= LAST_TAG; j++) {
      const Tag& t = tag(j);
      if (t.check == check_bytes && t.data >= data_len &&
          (result == 0 || t.data < tag(result).data))
        result = j;
    }
    return result;
  }

  /// Creates the code word for a frame (without FCS): block receives the
  /// data part followed by the check bytes. Returns the tag (0 if the frame
  /// is too long).
  static int encode(const uint8_t* frame, int len, int check_bytes,
                    uint8_t* block, int& block_len) {
    uint8_t data[BLOCK_SIZE];
    int data_len = stuff(frame, len, data, MAX_DATA);
    if (data_len <= 0) return 0;
    int t = pickTag(check_bytes, data_len);
    if (t == 0) return 0;
    const Tag& tg = tag(t);
    // the data part is padded with flags, the shortened part is 0
    memset(data + tg.data, 0, BLOCK_SIZE - tg.data);
    uint8_t codeword[BLOCK_SIZE];
    RSCodec::encode(data, BLOCK_SIZE - tg.check, tg.check, codeword, 1);
    memcpy(block, codeword, tg.data);
    memcpy(block + tg.data, codeword + BLOCK_SIZE - tg.check, tg.check);
    block_len = tg.data + tg.check;
    return t;
  }

  /// Corrects the received data part and check bytes of the tag in place
  /// (data = tag.data bytes, check bytes follow): returns the number of
  /// corrected bytes or -1
  static int correct(int t, uint8_t* received) {
    const Tag& tg = tag(t);
    uint8_t codeword[BLOCK_SIZE] = {0};
    memcpy(codeword, received, tg.data);
    memcpy(codeword + BLOCK_SIZE - tg.check, received + tg.data, tg.check);
    int result = RSCodec::correct(codeword, BLOCK_SIZE - tg.check, tg.check, 1);
    if (result < 0) return -1;
    // the shortened part must still be 0
    for (int j = tg.data; j < BLOCK_SIZE - tg.check; j++)
      if (codeword[j] != 0) return -1;
    memcpy(received, codeword, tg.data);
    return result;
  }

  /// Extracts the HDLC frame from the data part: returns the frame length
  /// (without FCS) or 0 if there is no valid frame
  static int extract(const uint8_t* data, int len, uint8_t* frame,
                     int frame_size) {
    uint8_t history = 0;
    int ones = 0, bits = 0, n = 0;
    bool in_frame = false;
    uint8_t current = 0;
    for (int j = 0; j < len; j++) {
      for (int b = 0; b < 8; b++) {
        int bit = (data[j] >> b) & 1;
        history = (history >> 1) | (bit << 7);
        if (history == 0x7E) {
          if (in_frame && n >= AX25Frame::MIN_FRAME + 2) {
            uint16_t fcs = frame[n - 2] | (frame[n - 1] << 8);
            return AX25Frame::fcs(frame, n - 2) == fcs ? n - 2 : 0;
          }
          in_frame = true;
          n = bits = ones = 0;
          current = 0;
          continue;
        }
        if (!in_frame) continue;
        if (bit) {
          if (++ones > 6) return 0;
        } else {
          if (ones == 5) {  // stuffed bit
            ones = 0;
            continue;
          }
          ones = 0;
        }
        current |= bit << bits;
        if (++bits == 8) {
          if (n >= frame_size) return 0;
          frame[n++] = current;
          current = 0;
          bits = 0;
        }
      }
    }
    return 0;
  }

 protected:
  static int popcount(uint64_t v) {
    int n = 0;
    for (; v; v &= v - 1) n++;
    return n;
  }

  /// HDLC frame (flag, bit stuffed frame with FCS, flag) padded with flags:
  /// returns the number of used bytes or -1 if it does not fit
  static int stuff(const uint8_t* frame, int len, uint8_t* out, int size) {
    memset(out, 0, size);
    int nbits = 0;
    int max_bits = size * 8;
    auto put = [&](int bit) {
      if (nbits >= max_bits) return false;
      if (bit) out[nbits >> 3] |= 1 << (nbits & 7);
      nbits++;
      return true;
    };
    for (int b = 0; b < 8; b++) put((0x7E >> b) & 1);
    uint16_t fcs = AX25Frame::fcs(frame, len);
    int ones = 0;
    for (int j = 0; j < len + 2; j++) {
      uint8_t value = j < len ? frame[j] : (j == len ? fcs & 0xFF : fcs >> 8);
      for (int b = 0; b < 8; b++) {
        int bit = (value >> b) & 1;
        if (!put(bit)) return -1;
        if (bit) {
          if (++ones == 5) {
            if (!put(0)) return -1;
            ones = 0;
          }
        } else {
          ones = 0;
        }
      }
    }
    for (int b = 0; b < 8; b++)
      if (!put((0x7E >> b) & 1)) return -1;
    int used = (nbits + 7) / 8;
    // fill with the (rotated) flag pattern
    for (int b = 0; nbits < max_bits; b++) put((0x7E >> (b & 7)) & 1);
    return used;
  }
};

/**
 * @brief IL2P (Improved Layer 2 Protocol, as implemented by direwolf): an
 * AX.25 frame is sent as a scrambled header with Reed-Solomon parity and a
 * scrambled payload in Reed-Solomon blocks, without HDLC framing and NRZI.
 * Header type 1 compresses the addresses and the control field of frames
 * with 2 addresses, header type 0 carries the complete AX.25 frame as
 * payload.
 * @copyright GPLv3
 */
class IL2P {
 public:
  static constexpr uint8_t PREAMBLE = 0x55;
  static constexpr uint32_t SYNC_WORD = 0xF15E48;
  static constexpr int HEADER_SIZE = 13;
  static constexpr int HEADER_PARITY = 2;
  static constexpr int MAX_PAYLOAD = 1023;
  static constexpr int MAX_ENCODED =
      HEADER_SIZE + HEADER_PARITY + MAX_PAYLOAD + 5 * 16;

  /// Encodes a frame (without FCS) into header and payload (without the
  /// sync word): returns the length or 0 if the frame can't be encoded
  static int encode(const uint8_t* frame, int len, bool max_fec,
                    uint8_t* out) {
    uint8_t hdr[HEADER_SIZE];
    int info_pos = type1Header(frame, len, max_fec, hdr);
    const uint8_t* payload = frame + info_pos;
    int payload_len = len - info_pos;
    if (info_pos < 0) {
      // type 0: the complete frame is the payload
      if (len < 14 || len > MAX_PAYLOAD) return 0;
      memset(hdr, 0, sizeof(hdr));
      setField(hdr, 7, 0, 1, max_fec);
      setField(hdr, 7, 11, 10, len);
      payload = frame;
      payload_len = len;
    }
    uint8_t scrambled[HEADER_SIZE];
    scramble(hdr, scrambled, HEADER_SIZE);
    RSCodec::encode(scrambled, HEADER_SIZE, HEADER_PARITY, out);
    int n = HEADER_SIZE + HEADER_PARITY;
    int k = encodePayload(payload, payload_len, max_fec, out + n);
    if (k < 0) return 0;
    return n + k;
  }

  /// Corrects and descrambles a received header (15 bytes): returns the
  /// encoded payload length or -1 if the header is invalid
  static int decodeHeader(const uint8_t* received, uint8_t* hdr,
                          int* corrected = nullptr) {
    uint8_t block[HEADER_SIZE + HEADER_PARITY];
    memcpy(block, received, sizeof(block));
    int e = RSCodec::correct(block, HEADER_SIZE, HEADER_PARITY);
    if (e < 0) return -1;
    if (corrected) *corrected = e;
    descramble(block, hdr, HEADER_SIZE);
    Blocks p;
    if (!blocks(payloadLength(hdr), maxFec(hdr), p)) return -1;
    return p.encoded;
  }

  /// Decodes the frame from the decoded header and the encoded payload:
  /// returns the frame length (without FCS) or 0 if invalid
  static int decodeFrame(const uint8_t* hdr, const uint8_t* encoded,
                         uint8_t* frame, int frame_size,
                         int* corrected = nullptr) {
    int payload_len = payloadLength(hdr);
    bool type0 = getField(hdr, 7, 1, 1) == 0;
    if (type0 && payload_len < AX25Frame::MIN_FRAME) return 0;
    if (frame_size < 16) return 0;
    // type 1: addresses, control and pid come from the header
    int n = type0 ? 0 : type1Frame(hdr, frame);
    if (n < 0 || (!type0 && n == 0) || n + payload_len > frame_size) return 0;
    // the payload is decoded directly into the frame
    if (payload_len > 0 && !decodePayload(encoded, payload_len, maxFec(hdr),
                                          frame + n, corrected))
      return 0;
    return n + payload_len;
  }

  static int payloadLength(const uint8_t* hdr) {
    return getField(hdr, 7, 11, 10);
  }

  static bool maxFec(const uint8_t* hdr) { return getField(hdr, 7, 0, 1); }

  /// Scrambles a block (the scrambler output is delayed by 5 bits)
  static void scramble(const uint8_t* in, uint8_t* out, int len) {
    int state = 0x00f;
    memset(out, 0, len);
    bool skipping = true;
    int ob = 0, om = 0x80;
    auto put = [&](int s) {
      if (s) out[ob] |= om;
      om >>= 1;
      if (om == 0) {
        om = 0x80;
        ob++;
      }
    };
    for (int ib = 0; ib < len; ib++) {
      for (int im = 0x80; im != 0; im >>= 1) {
        int s = scrambleBit((in[ib] & im) != 0, state);
        if (ib == 0 && im == 0x04) skipping = false;
        if (!skipping) put(s);
      }
    }
    for (int n = 0; n < 5; n++) put(scrambleBit(0, state));
  }

  static void descramble(const uint8_t* in, uint8_t* out, int len) {
    int state = 0x1f0;
    memset(out, 0, len);
    for (int b = 0; b < len; b++) {
      for (int m = 0x80; m != 0; m >>= 1) {
        int bit = (in[b] & m) != 0;
        int d = (bit ^ state) & 1;
        state = ((state >> 1) | (bit << 8)) ^ (bit << 3);
        if (d) out[b] |= m;
      }
    }
  }

 protected:
  /// Partitioning of the payload into Reed-Solomon blocks
  struct Blocks {
    int count = 0, small_size = 0, large_count = 0, parity = 0, encoded = 0;
  };

  static bool blocks(int len, bool max_fec, Blocks& p) {
    p = Blocks();
    if (len < 0 || len > MAX_PAYLOAD) return false;
    if (len == 0) return true;
    p.count = max_fec ? (len + 238) / 239 : (len + 246) / 247;
    p.small_size = len / p.count;
    p.large_count = len - p.count * p.small_size;
    if (max_fec) {
      p.parity = 16;
    } else if (p.small_size <= 61) {
      p.parity = 2;
    } else if (p.small_size <= 123) {
      p.parity = 4;
    } else if (p.small_size <= 185) {
      p.parity = 6;
    } else {
      p.parity = 8;
    }
    p.encoded = len + p.count * p.parity;
    return true;
  }

  static int encodePayload(const uint8_t* payload, int len, bool max_fec,
                           uint8_t* out) {
    Blocks p;
    if (!blocks(len, max_fec, p)) return -1;
    int n = 0;
    for (int b = 0; b < p.count; b++) {
      int size = p.small_size + (b < p.large_count ? 1 : 0);
      uint8_t scrambled[255];
      scramble(payload, scrambled, size);
      RSCodec::encode(scrambled, size, p.parity, out + n);
      payload += size;
      n += size + p.parity;
    }
    return n;
  }

  static bool decodePayload(const uint8_t* in, int len, bool max_fec,
                            uint8_t* payload, int* corrected) {
    Blocks p;
    if (!blocks(len, max_fec, p)) return false;
    for (int b = 0; b < p.count; b++) {
      int size = p.small_size + (b < p.large_count ? 1 : 0);
      uint8_t block[255];
      memcpy(block, in, size + p.parity);
      int e = RSCodec::correct(block, size, p.parity);
      if (e < 0) return false;
      if (corrected) *corrected += e;
      descramble(block, payload, size);
      in += size + p.parity;
      payload += size;
    }
    return true;
  }

  static int scrambleBit(int in, int& state) {
    int out = ((state >> 4) ^ state) & 1;
    state = ((((in ^ state) & 1) << 9) | (state ^ ((state & 1) << 4))) >> 1;
    return out;
  }

  static void setField(uint8_t* hdr, int bit, int lsb_index, int width,
                       int value) {
    while (width > 0 && value != 0) {
      if (value & 1) hdr[lsb_index] |= 1 << bit;
      value >>= 1;
      lsb_index--;
      width--;
    }
  }

  static int getField(const uint8_t* hdr, int bit, int lsb_index, int width) {
    int result = 0;
    for (int idx = lsb_index - width + 1; idx <= lsb_index; idx++)
      result = (result << 1) | ((hdr[idx] >> bit) & 1);
    return result;
  }

  static int encodePid(uint8_t pid) {
    if ((pid & 0x30) == 0x20 || (pid & 0x30) == 0x10) return 0x2;
    switch (pid) {
      case 0x01: return 0x3;
      case 0x06: return 0x4;
      case 0x07: return 0x5;
      case 0x08: return 0x6;
      case 0xcc: return 0xb;
      case 0xcd: return 0xc;
      case 0xce: return 0xd;
      case 0xcf: return 0xe;
      case 0xf0: return 0xf;
      default: return -1;
    }
  }

  static uint8_t decodePid(int pid) {
    static const uint8_t pids[16] = {0xf0, 0xf0, 0x20, 0x01, 0x06, 0x07,
                                     0x08, 0xf0, 0xf0, 0xf0, 0xf0, 0xcc,
                                     0xcd, 0xce, 0xcf, 0xf0};
    return pids[pid & 15];
  }

  /// U frame control fields (without poll/final) in the order of IL2P
  static const uint8_t* uFrames() {
    static const uint8_t codes[8] = {0x2F, 0x43, 0x0F, 0x63,
                                     0x87, 0x03, 0xAF, 0xE3};
    return codes;
  }

  /// Type 1 header of a frame with 2 addresses: returns the position of the
  /// payload in the frame or -1 if a type 1 header is not possible
  static int type1Header(const uint8_t* frame, int len, bool max_fec,
                         uint8_t* hdr) {
    memset(hdr, 0, HEADER_SIZE);
    if (len < 15 || !(frame[13] & 0x01) || (frame[6] & 0x01)) return -1;
    for (int j = 0; j < 12; j++) {
      int ch = frame[j < 6 ? j : j + 1] >> 1;
      if (ch < ' ' || ch > '_') return -1;
      hdr[j] = ch - ' ';
    }
    hdr[12] = (((frame[6] >> 1) & 0x0F) << 4) | ((frame[13] >> 1) & 0x0F);
    int cbit = (frame[6] & 0x80) ? 1 : 0;  // command (or 11)
    uint8_t c = frame[14];
    int pf = (c >> 4) & 1;
    int pos = 15;
    if ((c & 1) == 0) {
      // I frame
      if (len < 16) return -1;
      int pid = encodePid(frame[15]);
      if (pid < 0) return -1;
      setField(hdr, 6, 4, 4, pid);
      setField(hdr, 6, 11, 7, (pf << 6) | (((c >> 5) & 7) << 3) | ((c >> 1) & 7));
      pos = 16;
    } else if ((c & 3) == 1) {
      // S frame
      if (len != 15) return -1;
      setField(hdr, 6, 11, 7,
               (pf << 6) | (((c >> 5) & 7) << 3) | (cbit << 2) | ((c >> 2) & 3));
    } else {
      // U frame
      int type = -1;
      for (int j = 0; j < 8; j++)
        if ((c & ~0x10) == uFrames()[j]) type = j;
      if (type < 0) return -1;
      if (type == 5) {
        // UI
        if (len < 16) return -1;
        int pid = encodePid(frame[15]);
        if (pid < 0) return -1;
        setField(hdr, 6, 0, 1, 1);
        setField(hdr, 6, 4, 4, pid);
        pos = 16;
      } else {
        setField(hdr, 6, 4, 4, 1);
      }
      setField(hdr, 6, 11, 7, (pf << 6) | (type << 3) | (cbit << 2));
    }
    if (len - pos > MAX_PAYLOAD) return -1;
    setField(hdr, 7, 0, 1, max_fec);
    setField(hdr, 7, 1, 1, 1);
    setField(hdr, 7, 11, 10, len - pos);
    return pos;
  }

  /// AX.25 frame (addresses, control, pid) of a type 1 header: returns the
  /// length or 0 if invalid
  static int type1Frame(const uint8_t* hdr, uint8_t* frame) {
    // callsigns: upper case letters and digits, trailing spaces
    for (int a = 0; a < 2; a++) {
      bool end = false;
      for (int j = 0; j < 6; j++) {
        char ch = (hdr[a * 6 + j] & 0x3f) + ' ';
        if (ch == ' ') {
          end = true;
        } else if (end || !(isupper((unsigned char)ch) || isdigit((unsigned char)ch))) {
          return 0;
        }
        frame[a * 7 + j] = ch << 1;
      }
    }
    if (frame[0] == (' ' << 1) || frame[7] == (' ' << 1)) return 0;
    int pid = getField(hdr, 6, 4, 4);
    int ui = getField(hdr, 6, 0, 1);
    int control = getField(hdr, 6, 11, 7);
    int pf = (control >> 6) & 1;
    bool command = (control & 0x04) != 0;
    int n = 14;
    if (pid == 0) {
      // S frame
      frame[n++] = (((control >> 3) & 7) << 5) | (pf << 4) | ((control & 3) << 2) | 1;
    } else if (pid == 1) {
      // U frame
      uint8_t c = uFrames()[(control >> 3) & 7] | (pf << 4);
      frame[n++] = c;
      if ((c & ~0x10) == 0x03) frame[n++] = 0xf0;
    } else if (ui) {
      frame[n++] = 0x03 | (pf << 4);
      frame[n++] = decodePid(pid);
    } else {
      // I frame: always a command
      command = true;
      frame[n++] = (((control >> 3) & 7) << 5) | (pf << 4) | ((control & 7) << 1);
      frame[n++] = decodePid(pid);
    }
    frame[6] = 0x60 | (((hdr[12] >> 4) & 0x0F) << 1) | (command ? 0x80 : 0);
    frame[13] = 0x60 | ((hdr[12] & 0x0F) << 1) | (command ? 0 : 0x80) | 0x01;
    return n;
  }
};

}  // namespace tinymodem
