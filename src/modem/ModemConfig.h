#pragma once

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "AudioTools.h"

namespace tinymodem {

/// Data encoding which is used for the payload of a frame
enum class DataFormat {
  ASCII,       ///< ASCII 8-N-1 (or 7-N-1)
  BAUDOT,      ///< Baudot 5 bit (RTTY, TDD)
  BINARY,      ///< print the raw data bits as '0'/'1' (receive only)
  CALLERID,    ///< Bell 202 Caller-ID (SDMF/MDMF, receive only)
  UIC_TRAIN,   ///< UIC-751-3 train to ground (receive only)
  UIC_GROUND,  ///< UIC-751-3 ground to train (receive only)
};

/**
 * @brief Configuration of the FSK modem. The fields correspond to the command
 * line options of minimodem. Call setMode() to select one of the predefined
 * baud modes ("1200", "300", "rtty", "tdd", "same", "callerid", "uic-train",
 * "uic-ground", "V.21" or any number N for a Bell-like N bps modem) first and
 * then change the individual fields. Fields which are left at 0 (or -1) are
 * derived automatically when the encoder or decoder is started.
 * @author Phil Schatzmann (port), Kamal Mostafa (minimodem)
 * @copyright GPLv3
 */
struct ModemConfig : public audio_tools::AudioInfo {
  ModemConfig() {
    sample_rate = 48000;
    channels = 1;
    bits_per_sample = 16;
  }

  /// baud rate in bits per second (default: 1200 baud Bell 202)
  float baud_rate = 1200.0f;
  /// mark frequency in Hz (0 = auto)
  float mark_freq = 0.0f;
  /// space frequency in Hz (0 = auto)
  float space_freq = 0.0f;
  /// receive filter bandwidth in Hz (0 = auto)
  float bandwidth = 0.0f;
  /// number of data bits per frame (0 = auto)
  int data_bits = 0;
  /// number of start bits (-1 = auto)
  int start_bits = -1;
  /// number of stop bits (-1 = auto)
  float stop_bits = -1.0f;
  /// invert the start and stop bits
  bool invert_start_stop = false;
  /// swap the mark and space frequencies
  bool inverted = false;
  /// transmit the most significant data bit first
  bool msb_first = false;
  /// receiver: synchronize on sync_byte
  bool rx_sync = false;
  /// transmitter: number of sync bytes which are sent as preamble
  int tx_sync_bytes = 0;
  /// sync byte value (-1 = none)
  long long sync_byte = -1;
  /// minimum confidence threshold (signal-to-noise squelch)
  float confidence = 1.5f;
  /// stop searching for a better frame when this confidence was reached
  float search_limit = 2.3f;
  /// automatically detect the carrier frequency
  bool auto_carrier = false;
  /// minimum magnitude for the carrier auto detection
  float auto_carrier_threshold = 0.001f;
  /// transmit amplitude (0.0 - 1.0)
  float volume = 1.0f;
  /// number of mark bits which are sent before the first frame
  int leader_bits = 2;
  /// number of mark bits which are sent after the last frame
  int trailer_bits = 2;
  /// receiver: stop after the first carrier was lost
  bool rx_one = false;
  /// receiver: output the data bits as '0'/'1' text
  bool binary_output = false;
  /// receiver: output n raw bits without framing (0 = disabled)
  int binary_raw_bits = 0;
  /// Baudot: unshift on space
  bool baudot_usos = true;
  /// receiver: replace non printable characters with '.'
  bool print_filter = false;
  /// data encoding
  DataFormat format = DataFormat::ASCII;
  /// custom expected frame pattern ('0','1','d')
  const char* expect_bits = nullptr;
  /// receiver: input channel which is used for the decoding
  uint8_t channel = 0;
  /// receiver: (testing only) amplitude of added uniform noise
  float rx_noise = 0.0f;
  /// receiver: the sample buffer holds at least sample_rate / buffer_divisor
  /// samples (0 = use the minimum size)
  int buffer_divisor = 12;
  /// derived: shift which is used for the carrier auto detection
  int autodetect_shift = 0;

  /// Selects one of the predefined baud modes: returns false if invalid. All
  /// mode related fields (baud rate, frequencies, bandwidth, framing, sync,
  /// format) are reset first, so the result does not depend on a previous
  /// mode. With keep_options=true the data_bits and stop_bits which were set
  /// before are kept where minimodem allows options to override the mode
  /// (like the minimodem command line).
  bool setMode(const char* mode, bool keep_options = false) {
    if (mode == nullptr) return false;
    if (!keep_options) resetMode();
    is_resolved = false;
    if (equalsIgnoreCase(mode, "rtty")) {
      format = DataFormat::BAUDOT;
      baud_rate = 45.45f;
      if (data_bits == 0) data_bits = 5;
      if (stop_bits < 0) stop_bits = 1.5f;
    } else if (equalsIgnoreCase(mode, "tdd")) {
      format = DataFormat::BAUDOT;
      baud_rate = 45.45f;
      if (data_bits == 0) data_bits = 5;
      if (stop_bits < 0) stop_bits = 2.0f;
      mark_freq = 1400;
      space_freq = 1800;
    } else if (equalsIgnoreCase(mode, "same")) {
      // http://www.nws.noaa.gov/nwr/nwrsame.htm
      baud_rate = 520.0f + 5 / 6.0f;
      data_bits = 8;
      start_bits = 0;
      stop_bits = 0;
      rx_sync = true;
      tx_sync_bytes = 16;
      sync_byte = 0xAB;
      mark_freq = 2083.0f + 1 / 3.0f;
      space_freq = 1562.5f;
      bandwidth = baud_rate;
    } else if (startsWithIgnoreCase(mode, "caller")) {
      format = DataFormat::CALLERID;
      baud_rate = 1200;
      data_bits = 8;
    } else if (startsWithIgnoreCase(mode, "uic")) {
      format = strlen(mode) > 4 && tolower(mode[4]) == 't'
                   ? DataFormat::UIC_TRAIN
                   : DataFormat::UIC_GROUND;
      baud_rate = 600;
      data_bits = 39;
      mark_freq = 1300;
      space_freq = 1700;
      start_bits = 8;
      stop_bits = 0;
      expect_bits = "11110010ddddddddddddddddddddddddddddddddddddddd";
    } else if (startsWithIgnoreCase(mode, "V.21")) {
      baud_rate = 300;
      mark_freq = 980;
      space_freq = 1180;
      data_bits = 8;
    } else {
      baud_rate = atof(mode);
      if (data_bits == 0) data_bits = 8;
    }
    return baud_rate > 0.0f;
  }

  /// Returns true if the selected format supports transmitting
  bool isTxSupported() const {
    return format == DataFormat::ASCII || format == DataFormat::BAUDOT;
  }

  /// Derives the missing parameters: returns false if the configuration is
  /// invalid. This is called automatically by the encoder and decoder.
  bool resolve() {
    if (is_resolved) return true;
    if (baud_rate <= 0.0f) return false;
    if (data_bits == 0) data_bits = 8;

    if (binary_output || binary_raw_bits) format = DataFormat::BINARY;
    if (binary_raw_bits) {
      start_bits = 0;
      stop_bits = 0;
      data_bits = binary_raw_bits;
    }

    if (baud_rate >= 400) {
      // Bell 202: baud=1200 mark=1200 space=2200
      autodetect_shift = -(baud_rate * 5 / 6);
      if (mark_freq == 0) mark_freq = baud_rate / 2 + 600;
      if (space_freq == 0) space_freq = mark_freq - autodetect_shift;
      if (bandwidth == 0) bandwidth = 200;
    } else if (baud_rate >= 100) {
      // Bell 103: baud=300 mark=1270 space=1070
      autodetect_shift = 200;
      if (mark_freq == 0) mark_freq = 1270;
      if (space_freq == 0) space_freq = mark_freq - autodetect_shift;
      if (bandwidth == 0) bandwidth = 50;
    } else {
      // RTTY: baud=45.45 mark/space=variable shift=-170
      autodetect_shift = 170;
      if (mark_freq == 0) mark_freq = 1585;
      if (space_freq == 0) space_freq = mark_freq - autodetect_shift;
      if (bandwidth == 0) bandwidth = 10;
    }

    // defaults: 1 start bit, 1 stop bit
    if (start_bits < 0) start_bits = 1;
    if (stop_bits < 0) stop_bits = 1.0f;

    if (frameBits() > 64) return false;
    if (start_bits > 20) return false;

    // do not transmit any leader tone if no start bits
    if (start_bits == 0) leader_bits = 0;

    if (inverted) {
      float t = mark_freq;
      mark_freq = space_freq;
      space_freq = t;
    }

    // restrict bandwidth to <= data rate
    if (bandwidth > baud_rate) bandwidth = baud_rate;

    // sanitize confidence search limit
    if (search_limit < confidence) search_limit = confidence;

    is_resolved = true;
    return true;
  }

  /// number of bits of a frame: start + data + stop bits
  unsigned frameBits() const {
    return data_bits + start_bits + stop_bits;
  }

  /// true after resolve() was called successfully
  bool isResolved() const { return is_resolved; }

 protected:
  bool is_resolved = false;

  void resetMode() {
    baud_rate = 1200.0f;
    mark_freq = 0.0f;
    space_freq = 0.0f;
    bandwidth = 0.0f;
    data_bits = 0;
    start_bits = -1;
    stop_bits = -1.0f;
    rx_sync = false;
    tx_sync_bytes = 0;
    sync_byte = -1;
    format = DataFormat::ASCII;
    expect_bits = nullptr;
    autodetect_shift = 0;
  }

  static bool equalsIgnoreCase(const char* a, const char* b) {
    while (*a && *b) {
      if (tolower(*a++) != tolower(*b++)) return false;
    }
    return *a == *b;
  }

  static bool startsWithIgnoreCase(const char* str, const char* prefix) {
    while (*prefix) {
      if (tolower(*str++) != tolower(*prefix++)) return false;
    }
    return true;
  }
};

}  // namespace tinymodem
