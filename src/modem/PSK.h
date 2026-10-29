#pragma once

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "AudioTools.h"

namespace tinymodem {

/// Modulation of the PSK modem
enum class PSKModulation {
  /// 1 bit per symbol, PSK31 Varicode
  BPSK,
  /// 2 bits per symbol: rate 1/2 convolutional code (K=5) on 1 data bit per
  /// symbol, PSK31 Varicode
  QPSK,
  /// 3 bits per symbol without FEC, MFSK Varicode
  PSK8,
};

/**
 * @brief Configuration of the PSK modem (PSKEncoder, PSKDecoder): differential
 * PSK with cosine shaped transitions like PSK31, compatible with fldigi.
 *
 * setMode() selects "psk31", "psk63", "psk125", "psk250" or "psk500" (BPSK,
 * 31.25 to 500 baud), "qpsk31" to "qpsk500" (QPSK with a K=5 convolutional
 * code, same speed with FEC) or "8psk125", "8psk250", "8psk500" and
 * "8psk1000" (8PSK without FEC, 3 bits per symbol). The carrier frequency is
 * 1000 Hz by default: 8psk500 and 8psk1000 raise it to twice the symbol rate.
 * @copyright GPLv3
 */
struct PSKConfig : public audio_tools::AudioInfo {
  PSKConfig() {
    sample_rate = 48000;
    channels = 1;
    bits_per_sample = 16;
  }

  /// symbols per second
  float symbol_rate = 31.25f;
  /// modulation (bits per symbol)
  PSKModulation modulation = PSKModulation::BPSK;
  /// carrier frequency in Hz
  float carrier_freq = 1000.0f;
  /// transmit amplitude (0.0 - 1.0)
  float volume = 1.0f;
  /// transmitter: number of phase reversals before the data
  int preamble_symbols = 32;
  /// transmitter: number of symbols with a steady carrier after the data
  int postamble_symbols = 32;
  /// receiver: frequency tracking (automatic frequency control)
  bool afc = true;
  /// receiver: minimum signal level (1.0 = full scale)
  float min_amplitude = 0.005f;
  /// receiver: stop after the first transmission
  bool rx_one = false;
  /// receiver: input channel which is used for the decoding
  uint8_t channel = 0;
  /// receiver: (testing only) amplitude of added uniform noise
  float rx_noise = 0.0f;

  /// Selects e.g. "psk31", "qpsk63" or "8psk250": returns false if invalid.
  /// The other settings are kept.
  bool setMode(const char* mode) {
    PSKModulation mod;
    float rate = modeRate(mode, mod);
    if (rate <= 0) return false;
    symbol_rate = rate;
    modulation = mod;
    if (carrier_freq < 2 * symbol_rate) carrier_freq = 2 * symbol_rate;
    return true;
  }

  /// bits per symbol: 1 (BPSK), 2 (QPSK) or 3 (8PSK)
  int bitsPerSymbol() const {
    return modulation == PSKModulation::BPSK   ? 1
           : modulation == PSKModulation::QPSK ? 2
                                               : 3;
  }

  /// true if the mode is a PSK mode
  static bool isMode(const char* mode) {
    PSKModulation mod;
    return modeRate(mode, mod) > 0;
  }

  /// number of samples per symbol
  float samplesPerSymbol() const { return sample_rate / symbol_rate; }

  /// Checks the configuration: returns false (and logs the reason) if invalid
  bool isValid() const {
    if (symbol_rate <= 0 || carrier_freq <= 0) {
      LOGE("PSK: invalid symbol_rate or carrier_freq");
      return false;
    }
    if (carrier_freq + symbol_rate >= sample_rate / 2.0f) {
      LOGE("PSK: the carrier must be below %.1f Hz",
           (double)(sample_rate / 2.0f - symbol_rate));
      return false;
    }
    if (carrier_freq < 2 * symbol_rate) {
      LOGE("PSK: the carrier must be at least twice the symbol rate");
      return false;
    }
    if (samplesPerSymbol() < 8) {
      LOGE("PSK: at least 8 samples per symbol are needed");
      return false;
    }
    return true;
  }

 protected:
  static float modeRate(const char* mode, PSKModulation& mod) {
    if (mode == nullptr) return 0;
    mod = PSKModulation::BPSK;
    if (tolower(mode[0]) == 'q') {
      mod = PSKModulation::QPSK;
      mode++;
    } else if (mode[0] == '8') {
      mod = PSKModulation::PSK8;
      mode++;
    }
    const char* prefix = "psk";
    for (int j = 0; j < 3; j++)
      if (tolower(mode[j]) != prefix[j]) return 0;
    const char* number = mode + 3;
    for (const char* c = number; *c; c++)
      if (!isdigit((unsigned char)*c)) return 0;
    int n = atoi(number);
    if (mod == PSKModulation::PSK8)
      return (n == 125 || n == 250 || n == 500 || n == 1000) ? n : 0;
    switch (n) {
      case 31:
        return 31.25f;
      case 63:
        return 62.5f;
      case 125:
      case 250:
      case 500:
        return n;
      default:
        return 0;
    }
  }
};

/**
 * @brief The PSK31 Varicode (as extended by fldigi to all 256 byte values):
 * frequent characters have short codes. A code starts and ends with 1 and
 * contains no "00": the characters are separated by "00".
 * @copyright GPLv3
 */
class Varicode {
 public:
  /// Varicode bits of a character
  struct Code {
    uint16_t bits;   // most significant bit is sent first
    uint8_t length;  // number of bits
  };

  /// Code of the indicated byte
  static Code encode(uint8_t ch) { return table()[ch]; }

  /// Byte of the indicated code: returns -1 if the code is unknown
  static int decode(uint16_t bits) {
    for (int j = 0; j < 256; j++)
      if (table()[j].bits == bits) return j;
    return -1;
  }

 protected:
  static const Code* table() {
    // generated from fldigi's varicodetab1 (src/psk/pskvaricode.cxx)
    static const Code codes[256] = {
      {0x2AB, 10}, {0x2DB, 10}, {0x2ED, 10}, {0x377, 10}, {0x2EB, 10}, {0x35F, 10},
      {0x2EF, 10}, {0x2FD, 10}, {0x2FF, 10}, {0x0EF, 8}, {0x01D, 5}, {0x36F, 10},
      {0x2DD, 10}, {0x01F, 5}, {0x375, 10}, {0x3AB, 10}, {0x2F7, 10}, {0x2F5, 10},
      {0x3AD, 10}, {0x3AF, 10}, {0x35B, 10}, {0x36B, 10}, {0x36D, 10}, {0x357, 10},
      {0x37B, 10}, {0x37D, 10}, {0x3B7, 10}, {0x355, 10}, {0x35D, 10}, {0x3BB, 10},
      {0x2FB, 10}, {0x37F, 10}, {0x001, 1}, {0x1FF, 9}, {0x15F, 9}, {0x1F5, 9},
      {0x1DB, 9}, {0x2D5, 10}, {0x2BB, 10}, {0x17F, 9}, {0x0FB, 8}, {0x0F7, 8},
      {0x16F, 9}, {0x1DF, 9}, {0x075, 7}, {0x035, 6}, {0x057, 7}, {0x1AF, 9},
      {0x0B7, 8}, {0x0BD, 8}, {0x0ED, 8}, {0x0FF, 8}, {0x177, 9}, {0x15B, 9},
      {0x16B, 9}, {0x1AD, 9}, {0x1AB, 9}, {0x1B7, 9}, {0x0F5, 8}, {0x1BD, 9},
      {0x1ED, 9}, {0x055, 7}, {0x1D7, 9}, {0x2AF, 10}, {0x2BD, 10}, {0x07D, 7},
      {0x0EB, 8}, {0x0AD, 8}, {0x0B5, 8}, {0x077, 7}, {0x0DB, 8}, {0x0FD, 8},
      {0x155, 9}, {0x07F, 7}, {0x1FD, 9}, {0x17D, 9}, {0x0D7, 8}, {0x0BB, 8},
      {0x0DD, 8}, {0x0AB, 8}, {0x0D5, 8}, {0x1DD, 9}, {0x0AF, 8}, {0x06F, 7},
      {0x06D, 7}, {0x157, 9}, {0x1B5, 9}, {0x15D, 9}, {0x175, 9}, {0x17B, 9},
      {0x2AD, 10}, {0x1F7, 9}, {0x1EF, 9}, {0x1FB, 9}, {0x2BF, 10}, {0x16D, 9},
      {0x2DF, 10}, {0x00B, 4}, {0x05F, 7}, {0x02F, 6}, {0x02D, 6}, {0x003, 2},
      {0x03D, 6}, {0x05B, 7}, {0x02B, 6}, {0x00D, 4}, {0x1EB, 9}, {0x0BF, 8},
      {0x01B, 5}, {0x03B, 6}, {0x00F, 4}, {0x007, 3}, {0x03F, 6}, {0x1BF, 9},
      {0x015, 5}, {0x017, 5}, {0x005, 3}, {0x037, 6}, {0x07B, 7}, {0x06B, 7},
      {0x0DF, 8}, {0x05D, 7}, {0x1D5, 9}, {0x2B7, 10}, {0x1BB, 9}, {0x2B5, 10},
      {0x2D7, 10}, {0x3B5, 10}, {0x3BD, 10}, {0x3BF, 10}, {0x3D5, 10}, {0x3D7, 10},
      {0x3DB, 10}, {0x3DD, 10}, {0x3DF, 10}, {0x3EB, 10}, {0x3ED, 10}, {0x3EF, 10},
      {0x3F5, 10}, {0x3F7, 10}, {0x3FB, 10}, {0x3FD, 10}, {0x3FF, 10}, {0x555, 11},
      {0x557, 11}, {0x55B, 11}, {0x55D, 11}, {0x55F, 11}, {0x56B, 11}, {0x56D, 11},
      {0x56F, 11}, {0x575, 11}, {0x577, 11}, {0x57B, 11}, {0x57D, 11}, {0x57F, 11},
      {0x5AB, 11}, {0x5AD, 11}, {0x5AF, 11}, {0x5B5, 11}, {0x5B7, 11}, {0x5BB, 11},
      {0x5BD, 11}, {0x5BF, 11}, {0x5D5, 11}, {0x5D7, 11}, {0x5DB, 11}, {0x5DD, 11},
      {0x5DF, 11}, {0x5EB, 11}, {0x5ED, 11}, {0x5EF, 11}, {0x5F5, 11}, {0x5F7, 11},
      {0x5FB, 11}, {0x5FD, 11}, {0x5FF, 11}, {0x6AB, 11}, {0x6AD, 11}, {0x6AF, 11},
      {0x6B5, 11}, {0x6B7, 11}, {0x6BB, 11}, {0x6BD, 11}, {0x6BF, 11}, {0x6D5, 11},
      {0x6D7, 11}, {0x6DB, 11}, {0x6DD, 11}, {0x6DF, 11}, {0x6EB, 11}, {0x6ED, 11},
      {0x6EF, 11}, {0x6F5, 11}, {0x6F7, 11}, {0x6FB, 11}, {0x6FD, 11}, {0x6FF, 11},
      {0x755, 11}, {0x757, 11}, {0x75B, 11}, {0x75D, 11}, {0x75F, 11}, {0x76B, 11},
      {0x76D, 11}, {0x76F, 11}, {0x775, 11}, {0x777, 11}, {0x77B, 11}, {0x77D, 11},
      {0x77F, 11}, {0x7AB, 11}, {0x7AD, 11}, {0x7AF, 11}, {0x7B5, 11}, {0x7B7, 11},
      {0x7BB, 11}, {0x7BD, 11}, {0x7BF, 11}, {0x7D5, 11}, {0x7D7, 11}, {0x7DB, 11},
      {0x7DD, 11}, {0x7DF, 11}, {0x7EB, 11}, {0x7ED, 11}, {0x7EF, 11}, {0x7F5, 11},
      {0x7F7, 11}, {0x7FB, 11}, {0x7FD, 11}, {0x7FF, 11}, {0xAAB, 12}, {0xAAD, 12},
      {0xAAF, 12}, {0xAB5, 12}, {0xAB7, 12}, {0xABB, 12}, {0xABD, 12}, {0xABF, 12},
      {0xAD5, 12}, {0xAD7, 12}, {0xADB, 12}, {0xADD, 12}, {0xADF, 12}, {0xAEB, 12},
      {0xAED, 12}, {0xAEF, 12}, {0xAF5, 12}, {0xAF7, 12}, {0xAFB, 12}, {0xAFD, 12},
      {0xAFF, 12}, {0xB55, 12}, {0xB57, 12}, {0xB5B, 12}
    };
    return codes;
  }
};

/**
 * @brief The IZ8BLY MFSK Varicode (as used by fldigi for 8PSK): a code starts
 * with 1 and does not contain "001", so a character ends when "001" is
 * received (the 1 starts the next character).
 * @copyright GPLv3
 */
class MFSKVaricode {
 public:
  using Code = Varicode::Code;

  /// Code of the indicated byte
  static Code encode(uint8_t ch) { return table()[ch]; }

  /// Byte of the indicated code: returns -1 if the code is unknown
  static int decode(uint16_t bits) {
    for (int j = 0; j < 256; j++)
      if (table()[j].bits == bits) return j;
    return -1;
  }

 protected:
  static const Code* table() {
    // generated from fldigi's varicode table (src/mfsk/mfskvaricode.cxx)
    static const Code codes[256] = {
      {0x75C, 11}, {0x760, 11}, {0x768, 11}, {0x76C, 11}, {0x770, 11}, {0x774, 11},
      {0x778, 11}, {0x77C, 11}, {0x0A8, 8}, {0x780, 11}, {0x7A0, 11}, {0x7A8, 11},
      {0x7AC, 11}, {0x0AC, 8}, {0x7B0, 11}, {0x7B4, 11}, {0x7B8, 11}, {0x7BC, 11},
      {0x7C0, 11}, {0x7D0, 11}, {0x7D4, 11}, {0x7D8, 11}, {0x7DC, 11}, {0x7E0, 11},
      {0x7E8, 11}, {0x7EC, 11}, {0x7F0, 11}, {0x7F4, 11}, {0x7F8, 11}, {0x7FC, 11},
      {0x800, 12}, {0xA00, 12}, {0x004, 3}, {0x1C0, 9}, {0x1FC, 9}, {0x2D8, 10},
      {0x2A8, 10}, {0x2A0, 10}, {0x200, 10}, {0x1BC, 9}, {0x1F4, 9}, {0x1F0, 9},
      {0x2B4, 10}, {0x1E0, 9}, {0x0A0, 8}, {0x1D8, 9}, {0x1D4, 9}, {0x1E8, 9},
      {0x0E0, 8}, {0x0F0, 8}, {0x140, 9}, {0x154, 9}, {0x174, 9}, {0x160, 9},
      {0x16C, 9}, {0x1A0, 9}, {0x180, 9}, {0x1AC, 9}, {0x1EC, 9}, {0x1F8, 9},
      {0x2C0, 10}, {0x1DC, 9}, {0x2BC, 10}, {0x1D0, 9}, {0x280, 10}, {0x0BC, 8},
      {0x100, 9}, {0x0D4, 8}, {0x0DC, 8}, {0x0B8, 8}, {0x0F8, 8}, {0x150, 9},
      {0x158, 9}, {0x0C0, 8}, {0x1B4, 9}, {0x17C, 9}, {0x0F4, 8}, {0x0E8, 8},
      {0x0FC, 8}, {0x0D0, 8}, {0x0EC, 8}, {0x1B0, 9}, {0x0D8, 8}, {0x0B4, 8},
      {0x0B0, 8}, {0x15C, 9}, {0x1A8, 9}, {0x168, 9}, {0x170, 9}, {0x178, 9},
      {0x1B8, 9}, {0x2E8, 10}, {0x2D0, 10}, {0x2EC, 10}, {0x2D4, 10}, {0x2B0, 10},
      {0x2AC, 10}, {0x014, 5}, {0x060, 7}, {0x038, 6}, {0x034, 6}, {0x008, 4},
      {0x050, 7}, {0x058, 7}, {0x030, 6}, {0x018, 5}, {0x080, 8}, {0x070, 7},
      {0x02C, 6}, {0x040, 7}, {0x01C, 5}, {0x010, 5}, {0x054, 7}, {0x078, 7},
      {0x020, 6}, {0x028, 6}, {0x00C, 4}, {0x03C, 6}, {0x06C, 7}, {0x068, 7},
      {0x074, 7}, {0x05C, 7}, {0x07C, 7}, {0x2DC, 10}, {0x2B8, 10}, {0x2E0, 10},
      {0x2F0, 10}, {0xA80, 12}, {0xAA0, 12}, {0xAA8, 12}, {0xAAC, 12}, {0xAB0, 12},
      {0xAB4, 12}, {0xAB8, 12}, {0xABC, 12}, {0xAC0, 12}, {0xAD0, 12}, {0xAD4, 12},
      {0xAD8, 12}, {0xADC, 12}, {0xAE0, 12}, {0xAE8, 12}, {0xAEC, 12}, {0xAF0, 12},
      {0xAF4, 12}, {0xAF8, 12}, {0xAFC, 12}, {0xB00, 12}, {0xB40, 12}, {0xB50, 12},
      {0xB54, 12}, {0xB58, 12}, {0xB5C, 12}, {0xB60, 12}, {0xB68, 12}, {0xB6C, 12},
      {0xB70, 12}, {0xB74, 12}, {0xB78, 12}, {0xB7C, 12}, {0x2F4, 10}, {0x2F8, 10},
      {0x2FC, 10}, {0x300, 10}, {0x340, 10}, {0x350, 10}, {0x354, 10}, {0x358, 10},
      {0x35C, 10}, {0x360, 10}, {0x368, 10}, {0x36C, 10}, {0x370, 10}, {0x374, 10},
      {0x378, 10}, {0x37C, 10}, {0x380, 10}, {0x3A0, 10}, {0x3A8, 10}, {0x3AC, 10},
      {0x3B0, 10}, {0x3B4, 10}, {0x3B8, 10}, {0x3BC, 10}, {0x3C0, 10}, {0x3D0, 10},
      {0x3D4, 10}, {0x3D8, 10}, {0x3DC, 10}, {0x3E0, 10}, {0x3E8, 10}, {0x3EC, 10},
      {0x3F0, 10}, {0x3F4, 10}, {0x3F8, 10}, {0x3FC, 10}, {0x400, 11}, {0x500, 11},
      {0x540, 11}, {0x550, 11}, {0x554, 11}, {0x558, 11}, {0x55C, 11}, {0x560, 11},
      {0x568, 11}, {0x56C, 11}, {0x570, 11}, {0x574, 11}, {0x578, 11}, {0x57C, 11},
      {0x580, 11}, {0x5A0, 11}, {0x5A8, 11}, {0x5AC, 11}, {0x5B0, 11}, {0x5B4, 11},
      {0x5B8, 11}, {0x5BC, 11}, {0x5C0, 11}, {0x5D0, 11}, {0x5D4, 11}, {0x5D8, 11},
      {0x5DC, 11}, {0x5E0, 11}, {0x5E8, 11}, {0x5EC, 11}, {0x5F0, 11}, {0x5F4, 11},
      {0x5F8, 11}, {0x5FC, 11}, {0x600, 11}, {0x680, 11}, {0x6A0, 11}, {0x6A8, 11},
      {0x6AC, 11}, {0x6B0, 11}, {0x6B4, 11}, {0x6B8, 11}, {0x6BC, 11}, {0x6C0, 11},
      {0x6D0, 11}, {0x6D4, 11}, {0x6D8, 11}, {0x6DC, 11}, {0x6E0, 11}, {0x6E8, 11},
      {0x6EC, 11}, {0x6F0, 11}, {0x6F4, 11}, {0x6F8, 11}, {0x6FC, 11}, {0x700, 11},
      {0x740, 11}, {0x750, 11}, {0x754, 11}, {0x758, 11}
    };
    return codes;
  }
};

/**
 * @brief Rate 1/2 convolutional code with K=5 (polynomials 0x17 and 0x19)
 * of QPSK31 (fldigi) and a soft decision Viterbi decoder.
 * @copyright GPLv3
 */
class QPSKCode {
 public:
  static constexpr int K = 5;
  static constexpr int STATES = 1 << (K - 1);
  /// decisions which are kept for the trace back (decoding delay in bits)
  static constexpr int DEPTH = 32;

  /// encoder output (2 bits) for the register of the last K input bits
  static int output(int reg) {
    return parity(reg & 0x17) | (parity(reg & 0x19) << 1);
  }

  /// Encodes one bit: returns the 2 bit symbol
  int encode(int bit) {
    shreg = ((shreg << 1) | (bit & 1)) & ((1 << K) - 1);
    return output(shreg);
  }

  /// Resets the encoder and decoder state
  void reset() {
    shreg = 0;
    for (int s = 0; s < STATES; s++) metrics[s] = 0;
    count = 0;
    pos = 0;
  }

  /// Adds the metrics of the 4 possible symbols (higher = more likely):
  /// returns the decoded bit of DEPTH steps ago or -1
  int decode(const float metric[4]) {
    float next[STATES];
    uint16_t decisions = 0;
    for (int s = 0; s < STATES; s++) {
      // predecessors: the oldest bit (msb of the predecessor) was 0 or 1
      int bit = s & 1;
      int p0 = s >> 1, p1 = (s >> 1) | (STATES >> 1);
      float m0 = metrics[p0] + metric[output((p0 << 1) | bit)];
      float m1 = metrics[p1] + metric[output((p1 << 1) | bit)];
      if (m1 > m0) {
        next[s] = m1;
        decisions |= 1 << s;
      } else {
        next[s] = m0;
      }
    }
    float best = next[0];
    for (int s = 1; s < STATES; s++)
      if (next[s] > best) best = next[s];
    for (int s = 0; s < STATES; s++) metrics[s] = next[s] - best;
    history[pos] = decisions;
    pos = (pos + 1) % DEPTH;
    if (count < DEPTH) count++;
    return count < DEPTH ? -1 : traceBack(DEPTH);
  }

  /// Provides the bits which are still in the decoder (oldest first)
  template <class F>
  void flush(F on_bit) {
    // the bit of DEPTH steps ago was already provided
    int n = count == DEPTH ? DEPTH - 1 : count;
    for (int j = n; j > 0; j--) on_bit(traceBack(j));
    count = 0;
  }

 protected:
  int shreg = 0;
  float metrics[STATES] = {0};
  uint16_t history[DEPTH];
  int pos = 0;
  int count = 0;

  static int parity(int v) {
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return v & 1;
  }

  /// Input bit of depth steps ago on the best path
  int traceBack(int depth) {
    int state = 0;
    for (int s = 1; s < STATES; s++)
      if (metrics[s] > metrics[state]) state = s;
    int idx = pos;
    for (int j = 0; j < depth - 1; j++) {
      idx = (idx + DEPTH - 1) % DEPTH;
      int msb = (history[idx] >> state) & 1;
      state = (state >> 1) | (msb << (K - 2));
    }
    return state & 1;
  }
};

}  // namespace tinymodem
