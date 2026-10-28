#pragma once

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "AudioTools.h"

namespace tinymodem {

/// Format of the data which is written to the AX25Encoder and provided by
/// the AX25Decoder
enum class AX25Format {
  /// one frame per line in the monitor format: SRC>DEST,DIGI1,DIGI2:info
  TEXT,
  /// KISS frames (0xC0 0x00 frame 0xC0), used by APRS and packet radio
  /// applications
  KISS,
};

/**
 * @brief Configuration of the AX.25 packet radio modem (AX25Encoder,
 * AX25Decoder): Bell 202 AFSK with 1200 baud, NRZI and HDLC framing, as used
 * by APRS.
 * @copyright GPLv3
 */
struct AX25Config : public audio_tools::AudioInfo {
  AX25Config() {
    sample_rate = 48000;
    channels = 1;
    bits_per_sample = 16;
  }

  /// bits per second
  float baud_rate = 1200.0f;
  /// mark frequency (1 = no tone change) in Hz
  float mark_freq = 1200.0f;
  /// space frequency in Hz
  float space_freq = 2200.0f;
  /// transmit amplitude (0.0 - 1.0)
  float volume = 1.0f;
  /// transmitter: flags which are sent before the first frame (in ms)
  int tx_delay_ms = 300;
  /// transmitter: flags which are sent after the last frame (in ms)
  int tx_tail_ms = 50;
  /// format of the written and decoded data
  AX25Format format = AX25Format::TEXT;
  /// receiver: input channel which is used for the decoding
  uint8_t channel = 0;
  /// receiver: (testing only) amplitude of added uniform noise
  float rx_noise = 0.0f;

  /// Selects the mode "ax25" or "aprs" (both are AFSK 1200): returns false if
  /// the mode is not supported
  bool setMode(const char* mode) {
    if (!isMode(mode)) return false;
    baud_rate = 1200.0f;
    mark_freq = 1200.0f;
    space_freq = 2200.0f;
    return true;
  }

  /// true if the mode name is "ax25" or "aprs"
  static bool isMode(const char* mode) {
    return mode != nullptr &&
           (equalsIgnoreCase(mode, "ax25") || equalsIgnoreCase(mode, "aprs"));
  }

  /// Checks the configuration: returns false (and logs the reason) if invalid
  bool isValid() const {
    if (baud_rate <= 0 || mark_freq <= 0 || space_freq <= 0) {
      LOGE("AX25: invalid baud rate or frequencies");
      return false;
    }
    float max_freq = mark_freq > space_freq ? mark_freq : space_freq;
    if (max_freq >= sample_rate / 2.0f) {
      LOGE("AX25: frequencies must be below %.1f Hz",
           (double)(sample_rate / 2.0f));
      return false;
    }
    if (sample_rate / baud_rate < 4) {
      LOGE("AX25: at least 4 samples per bit are needed");
      return false;
    }
    return true;
  }

 protected:
  static bool equalsIgnoreCase(const char* a, const char* b) {
    while (*a && *b) {
      if (tolower(*a++) != tolower(*b++)) return false;
    }
    return *a == *b;
  }
};

/**
 * @brief Conversion between AX.25 frames and the monitor text format
 * (SRC>DEST,DIGI1,DIGI2*:info) and the KISS framing.
 * @copyright GPLv3
 */
class AX25Frame {
 public:
  static constexpr int MAX_FRAME = 330;  // 10 addresses + 256 info + extra
  static constexpr int MIN_FRAME = 15;   // 2 addresses + control (no FCS)
  static constexpr uint8_t KISS_FEND = 0xC0;
  static constexpr uint8_t KISS_FESC = 0xDB;
  static constexpr uint8_t KISS_TFEND = 0xDC;
  static constexpr uint8_t KISS_TFESC = 0xDD;

  /// AX.25 frame check sequence: CRC-16-CCITT with reflected bits
  static uint16_t fcs(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
      crc ^= data[i];
      for (int b = 0; b < 8; b++)
        crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : (crc >> 1);
    }
    return crc ^ 0xFFFF;
  }

  /// Converts a monitor text line (SRC>DEST,DIGI:info) into an AX.25 UI
  /// frame (without FCS): returns the frame length or 0 if invalid
  static int fromText(const char* text, size_t len, uint8_t* frame) {
    const char* end = text + len;
    const char* gt = (const char*)memchr(text, '>', len);
    const char* colon = (const char*)memchr(text, ':', len);
    if (gt == nullptr || colon == nullptr || colon < gt) return 0;
    int n = 0;
    // destination first, then source, then the digipeaters
    const char* dest = gt + 1;
    const char* dest_end = dest;
    while (dest_end < colon && *dest_end != ',') dest_end++;
    if (!encodeAddress(dest, dest_end, frame + n, true, false)) return 0;
    n += 7;
    if (!encodeAddress(text, gt, frame + n, false, false)) return 0;
    n += 7;
    const char* digi = dest_end;
    while (digi < colon) {
      digi++;  // skip the ','
      const char* digi_end = digi;
      while (digi_end < colon && *digi_end != ',') digi_end++;
      if (n + 7 > 7 * 10) return 0;  // max 8 digipeaters
      if (!encodeAddress(digi, digi_end, frame + n, false, true)) return 0;
      n += 7;
      digi = digi_end;
    }
    frame[n - 1] |= 0x01;  // last address
    frame[n++] = 0x03;     // UI frame
    frame[n++] = 0xF0;     // no layer 3
    size_t info_len = end - (colon + 1);
    if (n + info_len > MAX_FRAME) return 0;
    memcpy(frame + n, colon + 1, info_len);
    return n + info_len;
  }

  /// Converts an AX.25 frame (without FCS) into a monitor text line
  /// (without newline): returns the text length or 0 if invalid
  static int toText(const uint8_t* frame, size_t len, char* text,
                    size_t text_size) {
    // find the end of the address field
    size_t naddr = 0;
    while (naddr < 10 && (naddr + 1) * 7 <= len) {
      naddr++;
      if (frame[naddr * 7 - 1] & 0x01) break;
    }
    if (naddr < 2 || !(frame[naddr * 7 - 1] & 0x01)) return 0;
    size_t pos = naddr * 7;
    if (pos >= len) return 0;
    char call[12];
    size_t n = 0;
    auto append = [&](const char* str, size_t str_len) {
      if (n + str_len < text_size) {
        memcpy(text + n, str, str_len);
        n += str_len;
      }
    };
    decodeAddress(frame + 7, call);  // source
    append(call, strlen(call));
    append(">", 1);
    decodeAddress(frame, call);  // destination
    append(call, strlen(call));
    // the last repeated digipeater is marked with a '*'
    int last_repeated = -1;
    for (size_t j = 2; j < naddr; j++)
      if (frame[j * 7 + 6] & 0x80) last_repeated = j;
    for (size_t j = 2; j < naddr; j++) {
      append(",", 1);
      decodeAddress(frame + j * 7, call);
      append(call, strlen(call));
      if ((int)j == last_repeated) append("*", 1);
    }
    append(":", 1);
    // skip control and PID of UI frames
    uint8_t control = frame[pos++];
    if ((control & 0xEF) == 0x03 && pos < len) pos++;
    append((const char*)frame + pos, len - pos);
    text[n] = 0;
    return n;
  }

  /// Writes the frame as KISS data frame for port 0
  static size_t writeKiss(Print& out, const uint8_t* frame, size_t len) {
    size_t n = out.write(KISS_FEND);
    n += out.write((uint8_t)0x00);
    for (size_t j = 0; j < len; j++) {
      if (frame[j] == KISS_FEND) {
        n += out.write(KISS_FESC);
        n += out.write(KISS_TFEND);
      } else if (frame[j] == KISS_FESC) {
        n += out.write(KISS_FESC);
        n += out.write(KISS_TFESC);
      } else {
        n += out.write(frame[j]);
      }
    }
    n += out.write(KISS_FEND);
    return n;
  }

 protected:
  static bool encodeAddress(const char* start, const char* end, uint8_t* out,
                            bool command, bool digipeater) {
    bool repeated = false;
    if (end > start && end[-1] == '*') {
      repeated = true;
      end--;
    }
    const char* dash = start;
    while (dash < end && *dash != '-') dash++;
    int call_len = dash - start;
    if (call_len < 1 || call_len > 6) return false;
    for (int j = 0; j < 6; j++) {
      char ch = j < call_len ? toupper(start[j]) : ' ';
      if (!isalnum((unsigned char)ch) && ch != ' ') return false;
      out[j] = ch << 1;
    }
    int ssid = 0;
    if (dash < end) {
      ssid = atoi(dash + 1);
      if (ssid < 0 || ssid > 15) return false;
    }
    uint8_t flags = 0x60;  // reserved bits
    if (command || (digipeater && repeated)) flags |= 0x80;
    out[6] = flags | (ssid << 1);
    return true;
  }

  static void decodeAddress(const uint8_t* in, char* call) {
    int n = 0;
    for (int j = 0; j < 6; j++) {
      char ch = (in[j] >> 1) & 0x7F;
      if (ch != ' ') call[n++] = ch;
    }
    int ssid = (in[6] >> 1) & 0x0F;
    if (ssid > 0) n += sprintf(call + n, "-%d", ssid);
    call[n] = 0;
  }
};

}  // namespace tinymodem
