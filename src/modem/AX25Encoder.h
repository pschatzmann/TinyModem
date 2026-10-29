#pragma once

#include "AX25.h"
#include "AX25FEC.h"
#include "AudioTools.h"
#include "ModemBase.h"

namespace tinymodem {

/**
 * @brief AX.25 packet radio modulator (APRS, 1200 baud AFSK). The written
 * data is converted to AX.25 frames, which are sent with HDLC framing (flags,
 * bit stuffing, frame check sequence) and NRZI coding.
 *
 * In AX25Format::TEXT each line (terminated by '\n') is a frame in the
 * monitor format SRC>DEST,DIGI1,DIGI2:info. In AX25Format::KISS the data
 * consists of KISS frames. A frame is sent as soon as it is complete: the
 * first frame of a transmission is preceded by tx_delay_ms of flags. Call
 * endTransmission() (or flush()) to send the tail flags.
 *
 * AX25Config::layer2 selects FX.25 (the HDLC frame is sent as data of a
 * Reed-Solomon code word with a correlation tag) or IL2P (preamble 0x55, sync
 * word, scrambled header and payload with Reed-Solomon parity, without NRZI)
 * instead of standard AX.25.
 * @copyright GPLv3
 */
class AX25Encoder : public ModemEncoder {
 public:
  AX25Encoder() = default;
  AX25Encoder(Print& out) { setOutput(out); }

  /// Provides a default configuration: APRS (1200 baud AFSK)
  AX25Config defaultConfig() {
    AX25Config result;
    result.setMode("aprs");
    return result;
  }

  /// Defines the sample rate and number of channels of the generated audio
  void setAudioInfo(audio_tools::AudioInfo info) override {
    cfg.sample_rate = info.sample_rate;
    cfg.channels = info.channels;
    cfg.bits_per_sample = info.bits_per_sample;
    if (is_active) begin();
  }

  audio_tools::AudioInfo audioInfo() override { return cfg; }

  using audio_tools::AudioEncoder::begin;

  /// Starts the processing with the indicated configuration
  bool begin(AX25Config config) {
    cfg = config;
    return begin();
  }

  bool begin() override {
    is_active = false;
    if (!cfg.isValid()) return false;
    if (cfg.bits_per_sample != 16) {
      LOGE("AX25Encoder: bits_per_sample must be 16, not %d",
           (int)cfg.bits_per_sample);
      return false;
    }
    if (cfg.channels < 1) {
      LOGE("AX25Encoder: invalid number of channels: %d", (int)cfg.channels);
      return false;
    }
    if (!tones.begin(cfg, cfg.volume)) return false;
    samples_per_bit = (double)cfg.sample_rate / cfg.baud_rate;
    transmitting = false;
    input_len = 0;
    kiss_state = KissState::IDLE;
    is_active = true;
    return true;
  }

  void end() override {
    endTransmission();
    is_active = false;
  }

  /// Provide the frames as text lines or KISS frames
  size_t write(const uint8_t* data, size_t len) override {
    if (!is_active) return 0;
    for (size_t j = 0; j < len; j++) {
      if (cfg.format == AX25Format::KISS)
        writeKiss(data[j]);
      else
        writeText(data[j]);
    }
    return len;
  }

  /// Sends a frame (without FCS)
  bool writeFrame(const uint8_t* frame, size_t len) {
    if (!is_active || len < (size_t)AX25Frame::MIN_FRAME) return false;
    if (cfg.layer2 == AX25Layer2::FX25) return writeFX25(frame, len);
    if (cfg.layer2 == AX25Layer2::IL2P) return writeIL2P(frame, len);
    startTransmission();
    sendFlag();
    ones = 0;
    for (size_t j = 0; j < len; j++) sendByte(frame[j]);
    uint16_t fcs = AX25Frame::fcs(frame, len);
    sendByte(fcs & 0xFF);
    sendByte(fcs >> 8);
    sendFlag();
    frames++;
    return true;
  }

  /// Sends the tail flags (and optional silence): returns true if a
  /// transmission was ended.
  bool endTransmission(size_t silence_samples = 0) override {
    if (!is_active || !transmitting) return false;
    sendPreamble(cfg.tx_tail_ms);
    if (silence_samples) tones.tone(0, silence_samples);
    transmitting = false;
    return true;
  }

  /// Sends flags for the indicated number of samples
  bool writeIdle(size_t nsamples) override {
    if (!is_active) return false;
    if (!transmitting) {
      transmitting = true;
      bit_index = 0;
      nrzi_tone = true;
    }
    size_t n = nsamples / (samples_per_bit * 8) + 1;
    for (size_t j = 0; j < n; j++) {
      if (cfg.layer2 == AX25Layer2::IL2P)
        sendRawByte(IL2P::PREAMBLE);
      else
        sendFlag();
    }
    return true;
  }

  /// true between the first frame and endTransmission()
  bool isTransmitting() override { return transmitting; }

  /// Bits per second
  float symbolRate() override { return cfg.baud_rate; }

  /// Number of sent frames
  size_t frameCount() { return frames; }

  /// The configuration which was provided in begin()
  AX25Config& config() { return cfg; }

  operator bool() override { return is_active; }

 protected:
  enum class KissState { IDLE, COMMAND, DATA, ESCAPE };
  AX25Config cfg;
  bool is_active = false;
  bool transmitting = false;
  double samples_per_bit = 40;
  uint64_t bit_index = 0;
  bool nrzi_tone = true;  // true = mark
  int ones = 0;
  size_t frames = 0;
  // input
  char input[AX25Frame::MAX_FRAME + 100];
  int input_len = 0;
  uint8_t frame[AX25Frame::MAX_FRAME];
  KissState kiss_state = KissState::IDLE;
  bool kiss_data_frame = false;

  void writeText(uint8_t ch) {
    if (ch == '\r') return;
    if (ch != '\n') {
      if (input_len < (int)sizeof(input)) input[input_len++] = ch;
      return;
    }
    if (input_len > 0) {
      int len = AX25Frame::fromText(input, input_len, frame);
      if (len > 0)
        writeFrame(frame, len);
      else
        LOGW("AX25Encoder: invalid frame: %.*s", input_len, input);
    }
    input_len = 0;
  }

  void writeKiss(uint8_t ch) {
    switch (kiss_state) {
      case KissState::IDLE:
        if (ch == AX25Frame::KISS_FEND) kiss_state = KissState::COMMAND;
        break;
      case KissState::COMMAND:
        if (ch == AX25Frame::KISS_FEND) break;  // repeated FEND
        // only data frames (command 0) are sent
        kiss_data_frame = (ch & 0x0F) == 0;
        input_len = 0;
        kiss_state = KissState::DATA;
        break;
      case KissState::DATA:
        if (ch == AX25Frame::KISS_FEND) {
          if (kiss_data_frame && input_len > 0)
            writeFrame((const uint8_t*)input, input_len);
          kiss_state = KissState::COMMAND;
        } else if (ch == AX25Frame::KISS_FESC) {
          kiss_state = KissState::ESCAPE;
        } else {
          addKiss(ch);
        }
        break;
      case KissState::ESCAPE:
        if (ch == AX25Frame::KISS_TFEND) addKiss(AX25Frame::KISS_FEND);
        if (ch == AX25Frame::KISS_TFESC) addKiss(AX25Frame::KISS_FESC);
        kiss_state = KissState::DATA;
        break;
    }
  }

  void addKiss(uint8_t ch) {
    if (input_len < AX25Frame::MAX_FRAME) input[input_len++] = ch;
  }

  void startTransmission() {
    if (transmitting) return;
    transmitting = true;
    bit_index = 0;
    nrzi_tone = true;
    sendPreamble(cfg.tx_delay_ms);
  }

  /// Flags (IL2P: 0x55) for the indicated time
  void sendPreamble(int ms) {
    int n = ms * cfg.baud_rate / 8000;
    if (n < 1) n = 1;
    for (int j = 0; j < n; j++) {
      if (cfg.layer2 == AX25Layer2::IL2P)
        sendRawByte(IL2P::PREAMBLE);
      else
        sendFlag();
    }
  }

  /// FX.25: correlation tag and Reed-Solomon code word (NRZI, no stuffing)
  bool writeFX25(const uint8_t* frame, size_t len) {
    uint8_t block[FX25::BLOCK_SIZE];
    int block_len = 0;
    int tag = FX25::encode(frame, len, cfg.fx25_check_bytes, block, block_len);
    if (tag == 0) {
      LOGW("AX25Encoder: frame is too long for FX.25");
      return false;
    }
    startTransmission();
    uint64_t value = FX25::tag(tag).value;
    for (int k = 0; k < 8; k++) sendBits((value >> (8 * k)) & 0xFF);
    for (int j = 0; j < block_len; j++) sendBits(block[j]);
    sendFlag();
    frames++;
    return true;
  }

  /// IL2P: preamble, sync word and the encoded frame (no NRZI)
  bool writeIL2P(const uint8_t* frame, size_t len) {
    // a frame of up to MAX_FRAME bytes needs at most 2 payload blocks
    uint8_t encoded[IL2P::HEADER_SIZE + IL2P::HEADER_PARITY +
                    AX25Frame::MAX_FRAME + 2 * 16];
    int n = IL2P::encode(frame, len, cfg.il2p_max_fec, encoded);
    if (n > 0) {
      startTransmission();
      sendRawByte(IL2P::PREAMBLE);
      for (int k = 2; k >= 0; k--) sendRawByte((IL2P::SYNC_WORD >> (8 * k)) & 0xFF);
      for (int j = 0; j < n; j++) sendRawByte(encoded[j]);
      frames++;
    } else {
      LOGW("AX25Encoder: frame can't be encoded with IL2P");
    }
    return n > 0;
  }

  /// least significant bit first without bit stuffing (NRZI)
  void sendBits(uint8_t value) {
    for (int b = 0; b < 8; b++) sendBit((value >> b) & 1);
  }

  /// most significant bit first without NRZI: 1 = mark
  void sendRawByte(uint8_t value) {
    for (int b = 7; b >= 0; b--) {
      nrzi_tone = (value >> b) & 1;
      sendTone();
    }
  }

  /// 0x7E without bit stuffing
  void sendFlag() {
    for (int b = 0; b < 8; b++) sendBit((0x7E >> b) & 1);
  }

  /// least significant bit first with bit stuffing
  void sendByte(uint8_t value) {
    for (int b = 0; b < 8; b++) {
      int bit = (value >> b) & 1;
      sendBit(bit);
      if (bit) {
        if (++ones == 5) {
          sendBit(0);
          ones = 0;
        }
      } else {
        ones = 0;
      }
    }
  }

  /// NRZI: a 0 changes the tone, a 1 keeps it
  void sendBit(int bit) {
    if (bit == 0) nrzi_tone = !nrzi_tone;
    sendTone();
  }

  /// sends the current tone for one bit
  void sendTone() {
    size_t start = llround(bit_index * samples_per_bit);
    size_t stop = llround((bit_index + 1) * samples_per_bit);
    bit_index++;
    tones.tone(nrzi_tone ? cfg.mark_freq : cfg.space_freq, stop - start);
  }
};

}  // namespace tinymodem
