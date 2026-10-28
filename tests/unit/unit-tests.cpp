/**
 * Unit tests for the header only Modem library. They run on the desktop
 * with the minimal Arduino emulation of the arduino-audio-tools.
 *
 * usage: unit-tests [test-name]   (all tests if no name is given)
 */
#include <stdio.h>
#include <string.h>

#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "AudioTools.h"
#include "TinyModemRTOS.h"

static int failures = 0;

#define CHECK(cond)                                                   \
  do {                                                                \
    if (!(cond)) {                                                    \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
              #cond);                                                 \
      failures++;                                                     \
    }                                                                 \
  } while (0)

#define CHECK_EQ(a, b)                                                      \
  do {                                                                      \
    auto va = (a);                                                          \
    auto vb = (b);                                                          \
    if (!(va == vb)) {                                                      \
      std::stringstream ss;                                                 \
      ss << va << " != " << vb;                                             \
      fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s (%s)\n", __FILE__,  \
              __LINE__, #a, #b, ss.str().c_str());                          \
      failures++;                                                           \
    }                                                                       \
  } while (0)

#define CHECK_NEAR(a, b, eps) CHECK(fabs((a) - (b)) <= (eps))

/// Print which collects the output in a string
class StringPrint : public Print {
 public:
  size_t write(uint8_t ch) override {
    str += (char)ch;
    return 1;
  }
  size_t write(const uint8_t* data, size_t len) override {
    str.append((const char*)data, len);
    return len;
  }
  std::string str;
};

/// Print which collects 16 bit samples
class SamplePrint : public Print {
 public:
  size_t write(uint8_t ch) override { return write(&ch, 1); }
  size_t write(const uint8_t* data, size_t len) override {
    bytes.insert(bytes.end(), data, data + len);
    return len;
  }
  std::vector<int16_t> samples() {
    std::vector<int16_t> result(bytes.size() / 2);
    memcpy(result.data(), bytes.data(), result.size() * 2);
    return result;
  }
  std::vector<uint8_t> bytes;
};

/// Unbounded in memory Stream (audio loopback)
class LoopbackStream : public Stream {
 public:
  size_t write(uint8_t ch) override {
    data.push_back(ch);
    return 1;
  }
  size_t write(const uint8_t* buf, size_t len) override {
    data.insert(data.end(), buf, buf + len);
    return len;
  }
  int available() override { return data.size(); }
  int read() override {
    if (data.empty()) return -1;
    int result = data.front();
    data.pop_front();
    return result;
  }
  int peek() override { return data.empty() ? -1 : data.front(); }
  size_t readBytes(uint8_t* buf, size_t len) override {
    size_t n = len < data.size() ? len : data.size();
    for (size_t j = 0; j < n; j++) {
      buf[j] = data.front();
      data.pop_front();
    }
    return n;
  }
  std::deque<uint8_t> data;
};

/// LoopbackStream which can be used from several threads
class SyncLoopbackStream : public LoopbackStream {
 public:
  size_t write(uint8_t ch) override { return write(&ch, 1); }
  size_t write(const uint8_t* buf, size_t len) override {
    std::lock_guard<std::mutex> guard(mtx);
    return LoopbackStream::write(buf, len);
  }
  int available() override {
    std::lock_guard<std::mutex> guard(mtx);
    return LoopbackStream::available();
  }
  size_t readBytes(uint8_t* buf, size_t len) override {
    std::lock_guard<std::mutex> guard(mtx);
    return LoopbackStream::readBytes(buf, len);
  }

 protected:
  std::mutex mtx;
};

static std::string readFile(const char* name) {
  std::string path = std::string(TEST_DATA_DIR) + "/" + name;
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static ModemConfig modeConfig(const char* mode) {
  ModemConfig cfg;
  cfg.setMode(mode);
  return cfg;
}

/// Modulates the text and returns the samples
static std::vector<int16_t> modulate(ModemConfig tx_cfg, const std::string& text,
                                     size_t silence = 0) {
  SamplePrint audio;
  FSKEncoder encoder(audio);
  CHECK(encoder.begin(tx_cfg));
  encoder.write((const uint8_t*)text.data(), text.size());
  encoder.endTransmission(silence);
  encoder.end();
  return audio.samples();
}

/// Demodulates the samples (written in chunks of the indicated size)
static std::string demodulate(ModemConfig rx_cfg,
                              const std::vector<int16_t>& samples,
                              std::string* status = nullptr,
                              size_t chunk = 1024) {
  StringPrint text, status_out;
  FSKDecoder decoder(text);
  decoder.setStatusOutput(status_out);
  CHECK(decoder.begin(rx_cfg));
  const uint8_t* data = (const uint8_t*)samples.data();
  size_t len = samples.size() * 2;
  for (size_t pos = 0; pos < len; pos += chunk) {
    size_t n = len - pos < chunk ? len - pos : chunk;
    decoder.write(data + pos, n);
  }
  decoder.end();
  if (status) *status = status_out.str;
  return text.str;
}

static std::string loopback(ModemConfig tx_cfg, ModemConfig rx_cfg,
                            const std::string& text,
                            std::string* status = nullptr) {
  return demodulate(rx_cfg, modulate(tx_cfg, text), status);
}

static const char* TEXT = "The quick brown fox jumps over the lazy dog 0123456789\n";
static const char* BAUDOT_TEXT = "THE QUICK BROWN FOX 123-456\n";

// ---------------------------------------------------------------------------

static void test_config_modes() {
  ModemConfig c = modeConfig("1200");
  CHECK(c.resolve());
  CHECK_EQ(c.mark_freq, 1200.0f);
  CHECK_EQ(c.space_freq, 2200.0f);
  CHECK_EQ(c.bandwidth, 200.0f);
  CHECK_EQ(c.data_bits, 8);
  CHECK_EQ(c.start_bits, 1);
  CHECK_EQ(c.stop_bits, 1.0f);

  c = modeConfig("300");
  CHECK(c.resolve());
  CHECK_EQ(c.mark_freq, 1270.0f);
  CHECK_EQ(c.space_freq, 1070.0f);
  CHECK_EQ(c.bandwidth, 50.0f);

  c = modeConfig("rtty");
  CHECK(c.resolve());
  CHECK(c.format == DataFormat::BAUDOT);
  CHECK_EQ(c.data_bits, 5);
  CHECK_EQ(c.stop_bits, 1.5f);
  CHECK_EQ(c.mark_freq, 1585.0f);
  CHECK_EQ(c.space_freq, 1415.0f);
  CHECK_EQ(c.bandwidth, 10.0f);

  c = modeConfig("TDD");
  CHECK(c.resolve());
  CHECK_EQ(c.mark_freq, 1400.0f);
  CHECK_EQ(c.space_freq, 1800.0f);
  CHECK_EQ(c.stop_bits, 2.0f);

  c = modeConfig("same");
  CHECK(c.resolve());
  CHECK_EQ(c.sync_byte, 0xABLL);
  CHECK_EQ(c.start_bits, 0);
  CHECK_EQ(c.leader_bits, 0);

  c = modeConfig("callerid");
  CHECK(c.resolve());
  CHECK(c.format == DataFormat::CALLERID);
  CHECK(!c.isTxSupported());

  c = modeConfig("uic-train");
  CHECK(c.resolve());
  CHECK(c.format == DataFormat::UIC_TRAIN);
  c = modeConfig("uic-ground");
  CHECK(c.format == DataFormat::UIC_GROUND);

  c = modeConfig("V.21");
  CHECK(c.resolve());
  CHECK_EQ(c.mark_freq, 980.0f);
  CHECK_EQ(c.space_freq, 1180.0f);

  ModemConfig invalid;
  CHECK(!invalid.setMode("abc"));
  CHECK(!invalid.resolve());

  // inverted swaps mark and space; resolve() is idempotent
  c = modeConfig("1200");
  c.inverted = true;
  CHECK(c.resolve());
  CHECK(c.resolve());
  CHECK_EQ(c.mark_freq, 2200.0f);
  CHECK_EQ(c.space_freq, 1200.0f);

  // bandwidth is limited to the baud rate
  c = modeConfig("100");
  CHECK(c.resolve());
  CHECK_EQ(c.bandwidth, 50.0f);
  c = modeConfig("30");
  CHECK(c.resolve());
  CHECK_EQ(c.bandwidth, 10.0f);
  c = modeConfig("5");
  CHECK(c.resolve());
  CHECK_EQ(c.bandwidth, 5.0f);

  // max 64 bits per frame
  c = modeConfig("1200");
  c.data_bits = 63;
  CHECK(!c.resolve());

  // binary raw output
  c = modeConfig("1200");
  c.binary_raw_bits = 16;
  CHECK(c.resolve());
  CHECK(c.format == DataFormat::BINARY);
  CHECK_EQ(c.data_bits, 16);
  CHECK_EQ(c.start_bits, 0);
}

static void test_baudot_codec() {
  BaudotCodec encoder, decoder;
  StringPrint out;
  decoder.reset();
  std::string text = "RYRY 123 HELLO, WORLD!\r\n";
  for (char ch : text) {
    uint32_t words[2];
    int n = encoder.encode(words, ch);
    CHECK(n >= 1 && n <= 2);
    for (int j = 0; j < n; j++) {
      CHECK(words[j] < 32);
      decoder.decode(out, words[j], 5);
    }
  }
  CHECK_EQ(out.str, text);

  // lower case is converted to upper case, non encodable chars are skipped
  uint32_t words[2];
  CHECK_EQ(encoder.encode(words, '*'), 0);
  CHECK_EQ(encoder.encode(words, '@'), 0);
  CHECK(encoder.encode(words, 'a') >= 1);

  // unshift on space
  BaudotCodec usos;
  StringPrint out2;
  usos.reset();
  usos.decode(out2, 0x1B, 5);  // FIGS
  usos.decode(out2, 0x17, 5);  // '1'
  usos.decode(out2, 0x04, 5);  // SPACE -> LTRS
  usos.decode(out2, 0x17, 5);  // 'Q'
  CHECK_EQ(out2.str, std::string("1 Q"));

  BaudotCodec no_usos;
  StringPrint out3;
  no_usos.setUSOS(false);
  no_usos.reset();
  no_usos.decode(out3, 0x1B, 5);
  no_usos.decode(out3, 0x17, 5);
  no_usos.decode(out3, 0x04, 5);
  no_usos.decode(out3, 0x17, 5);
  CHECK_EQ(out3.str, std::string("1 1"));
}

static void test_callerid_codec(const char* bytes_file, const char* txt_file) {
  std::string bytes = readFile(bytes_file);
  std::string expected = readFile(txt_file);
  CHECK(!bytes.empty());
  CallerIDCodec codec;
  StringPrint out;
  codec.reset();
  for (unsigned char ch : bytes) codec.decode(out, ch, 8);
  CHECK_EQ(out.str, expected);
}

static void test_callerid_mdmf() {
  test_callerid_codec("testdata-callerid-mdmf.bytes",
                      "testdata-callerid-mdmf.txt");
}

static void test_callerid_sdmf() {
  test_callerid_codec("testdata-callerid-sdmf.bytes",
                      "testdata-callerid-sdmf.txt");
}

static uint64_t uicBits(const uint8_t id[6], uint8_t code) {
  uint64_t bits = 0;
  for (int j = 0; j < 6; j++) bits |= (uint64_t)id[j] << (4 * j);
  bits |= bitReverse(code, 8) << 24;
  return bits;
}

static void test_uic_codec() {
  const uint8_t id[6] = {1, 2, 3, 0xA, 0xB, 0xC};
  UICCodec ground(false);
  StringPrint out;
  ground.decode(out, uicBits(id, 0x09), 39);
  CHECK_EQ(out.str,
           std::string("Train ID: 123ABC - Message: 09 (Emergency stop)\n"));

  UICCodec train(true);
  StringPrint out2;
  train.decode(out2, uicBits(id, 0x09), 39);
  CHECK_EQ(out2.str, std::string(
                         "Train ID: 123ABC - Message: 09 (Train staff wish to "
                         "comm.)\n"));

  StringPrint out3;
  train.decode(out3, uicBits(id, 0x77), 39);
  CHECK_EQ(out3.str, std::string("Train ID: 123ABC - Message: 77 (Unknown)\n"));
}

static void test_binary_codec() {
  BinaryCodec codec;
  StringPrint out;
  CHECK_EQ(codec.decode(out, 0b1011, 4), (size_t)5);
  CHECK_EQ(out.str, std::string("1101\n"));
}

static void test_bit_helpers() {
  CHECK_EQ(bitReverse(0b0011, 4), 0b1100ULL);
  CHECK_EQ(bitWindow(0xABCD, 4, 8), 0xBCULL);
  CHECK_EQ(bitWindow(~0ULL, 0, 64), ~0ULL);
}

static void test_encoder_signal() {
  ModemConfig cfg = modeConfig("1200");
  cfg.volume = 0.5f;
  std::vector<int16_t> samples = modulate(cfg, "A");
  // leader(2) + frame(10) + trailer(2) bits of 40 samples
  CHECK_EQ(samples.size(), (size_t)(14 * 40));
  int16_t max = 0;
  for (int16_t s : samples) max = s > max ? s : max;
  CHECK(max <= 16384 && max > 16000);

  // deterministic
  CHECK(modulate(cfg, TEXT) == modulate(cfg, TEXT));

  // stereo: identical channels
  cfg.channels = 2;
  std::vector<int16_t> stereo = modulate(cfg, "A");
  CHECK_EQ(stereo.size(), (size_t)(2 * 14 * 40));
  bool same = true;
  for (size_t j = 0; j < stereo.size(); j += 2)
    same = same && stereo[j] == stereo[j + 1];
  CHECK(same);

  // tx not supported
  FSKEncoder encoder;
  CHECK(!encoder.begin(modeConfig("callerid")));
  CHECK(!encoder.begin(modeConfig("uic-train")));
  ModemConfig bits24 = modeConfig("1200");
  bits24.bits_per_sample = 24;
  CHECK(!encoder.begin(bits24));
}

static void test_custom_generator() {
  // phase continuous fixed point generator: decodes in all modes
  const char* modes[] = {"1200", "300", "rtty", "tdd"};
  for (const char* mode : modes) {
    ModemConfig cfg = modeConfig(mode);
    cfg.volume = 0.5f;
    bool baudot = cfg.format == DataFormat::BAUDOT;
    std::string text = baudot ? BAUDOT_TEXT : TEXT;
    SamplePrint audio;
    audio_tools::FastIntSineGenerator<int16_t> generator;
    FSKEncoder encoder(audio, generator);
    CHECK(&encoder.getGenerator() == &generator);
    CHECK(encoder.begin(cfg));
    encoder.write((const uint8_t*)text.data(), text.size());
    encoder.end();
    std::string status;
    CHECK_EQ(demodulate(cfg, audio.samples(), &status), text);
    size_t pos = status.find("ampl=");
    CHECK(pos != std::string::npos);
    CHECK_NEAR(atof(status.c_str() + pos + 5), 0.5, 0.03);
  }

  // the default generator reproduces minimodem: silence is 0
  ModemConfig cfg = modeConfig("1200");
  SamplePrint audio;
  FSKEncoder encoder(audio);
  CHECK(encoder.begin(cfg));
  encoder.writeSilence(100);
  std::vector<int16_t> samples = audio.samples();
  CHECK_EQ(samples.size(), (size_t)100);
  bool silent = true;
  for (int16_t s : samples) silent = silent && s == 0;
  CHECK(silent);
}

static void check_loopback(const char* mode, const std::string& text) {
  std::string status;
  ModemConfig cfg = modeConfig(mode);
  std::string result = loopback(cfg, cfg, text, &status);
  if (result != text)
    fprintf(stderr, "mode %s: '%s' != '%s'\n", mode, result.c_str(),
            text.c_str());
  CHECK_EQ(result, text);
  CHECK(status.find("### CARRIER") != std::string::npos);
  CHECK(status.find("### NOCARRIER") != std::string::npos);
}

static void test_loopback_modes() {
  check_loopback("1200", TEXT);
  check_loopback("300", TEXT);
  check_loopback("12000", TEXT);
  check_loopback("V.21", TEXT);
  check_loopback("same", TEXT);
  check_loopback("rtty", BAUDOT_TEXT);
  check_loopback("tdd", BAUDOT_TEXT);
  check_loopback("100", TEXT);
}

static void test_loopback_rate_perfect() {
  std::string status;
  ModemConfig cfg = modeConfig("1200");
  CHECK_EQ(loopback(cfg, cfg, TEXT, &status), std::string(TEXT));
  CHECK(status.find("### CARRIER 1200 @ 1200.0 Hz ###") != std::string::npos);
  CHECK(status.find("(rate perfect)") != std::string::npos);
  char expected[80];
  snprintf(expected, sizeof(expected), "ndata=%d ", (int)strlen(TEXT));
  CHECK(status.find(expected) != std::string::npos);
}

static void test_loopback_options() {
  ModemConfig cfg = modeConfig("1200");
  cfg.msb_first = true;
  CHECK_EQ(loopback(cfg, cfg, TEXT), std::string(TEXT));

  cfg = modeConfig("1200");
  cfg.invert_start_stop = true;
  CHECK_EQ(loopback(cfg, cfg, TEXT), std::string(TEXT));

  cfg = modeConfig("1200");
  cfg.inverted = true;
  CHECK_EQ(loopback(cfg, cfg, TEXT), std::string(TEXT));

  cfg = modeConfig("1200");
  cfg.start_bits = 2;
  cfg.stop_bits = 2;
  CHECK_EQ(loopback(cfg, cfg, TEXT), std::string(TEXT));

  cfg = modeConfig("300");
  cfg.stop_bits = 1.5f;
  CHECK_EQ(loopback(cfg, cfg, TEXT), std::string(TEXT));

  cfg = modeConfig("1200");
  cfg.data_bits = 7;
  CHECK_EQ(loopback(cfg, cfg, TEXT), std::string(TEXT));

  cfg = modeConfig("1200");
  cfg.mark_freq = 1500;
  cfg.space_freq = 2500;
  CHECK_EQ(loopback(cfg, cfg, TEXT), std::string(TEXT));

  cfg = modeConfig("1200");
  cfg.rx_sync = true;
  cfg.tx_sync_bytes = 16;
  cfg.sync_byte = 0xAB;
  CHECK_EQ(loopback(cfg, cfg, TEXT), std::string(TEXT));

  // 5 bit baudot with a numeric baud mode
  cfg = modeConfig("300");
  cfg.format = DataFormat::BAUDOT;
  cfg.data_bits = 5;
  CHECK_EQ(loopback(cfg, cfg, BAUDOT_TEXT), std::string(BAUDOT_TEXT));
}

static void test_loopback_sample_rates() {
  int rates[] = {8000, 11025, 16000, 22050, 44100, 48000};
  for (int rate : rates) {
    ModemConfig cfg = modeConfig("1200");
    cfg.sample_rate = rate;
    std::string result = loopback(cfg, cfg, TEXT);
    if (result != TEXT) fprintf(stderr, "sample rate %d failed\n", rate);
    CHECK_EQ(result, std::string(TEXT));
  }
  ModemConfig cfg = modeConfig("rtty");
  cfg.sample_rate = 8000;
  CHECK_EQ(loopback(cfg, cfg, BAUDOT_TEXT), std::string(BAUDOT_TEXT));
}

static void test_rate_slop() {
  ModemConfig rx = modeConfig("300");
  float rates[] = {292, 299, 300, 301, 308};
  for (float rate : rates) {
    ModemConfig tx = rx;
    tx.baud_rate = rate;
    std::string result = loopback(tx, rx, TEXT);
    if (result != TEXT) fprintf(stderr, "tx rate %f failed\n", rate);
    CHECK_EQ(result, std::string(TEXT));
  }
}

static void test_amplitude() {
  float volumes[] = {1.0f, 0.3f, 0.01f};
  for (float volume : volumes) {
    ModemConfig cfg = modeConfig("1200");
    cfg.volume = volume;
    std::string status;
    CHECK_EQ(loopback(cfg, cfg, TEXT, &status), std::string(TEXT));
    size_t pos = status.find("ampl=");
    CHECK(pos != std::string::npos);
    float ampl = atof(status.c_str() + pos + 5);
    CHECK_NEAR(ampl, volume, 0.01);
  }
}

static void test_noise() {
  float noise_levels[] = {0.0f, 0.05f, 0.1f, 0.3f};
  for (float noise : noise_levels) {
    ModemConfig tx = modeConfig("1200");
    tx.volume = 0.5f;
    ModemConfig rx = modeConfig("1200");
    rx.rx_noise = noise;
    rx.rx_one = true;
    std::string result = loopback(tx, rx, TEXT);
    if (result != TEXT) fprintf(stderr, "noise %f failed\n", noise);
    CHECK_EQ(result, std::string(TEXT));
  }
}

static void test_auto_carrier() {
  ModemConfig tx = modeConfig("1200");
  tx.mark_freq = 1600;
  tx.space_freq = 2600;
  ModemConfig rx = modeConfig("1200");
  rx.auto_carrier = true;
  std::string status;
  // the first characters are used for the carrier detection
  std::string result = loopback(tx, rx, std::string("UUUU") + TEXT, &status);
  CHECK(result.find(TEXT) != std::string::npos);
  CHECK(status.find("@ 1600.0 Hz") != std::string::npos);
}

static void test_chunk_sizes() {
  ModemConfig cfg = modeConfig("1200");
  std::vector<int16_t> samples = modulate(cfg, TEXT);
  size_t chunks[] = {1, 3, 7, 160, 4096};
  for (size_t chunk : chunks) {
    CHECK_EQ(demodulate(cfg, samples, nullptr, chunk), std::string(TEXT));
  }
}

static void test_input_formats() {
  ModemConfig cfg = modeConfig("1200");
  std::vector<int16_t> mono = modulate(cfg, TEXT);

  // 32 bit
  std::vector<int32_t> s32(mono.size());
  for (size_t j = 0; j < mono.size(); j++) s32[j] = (int32_t)mono[j] << 16;
  {
    StringPrint text;
    FSKDecoder decoder(text);
    ModemConfig rx = cfg;
    rx.bits_per_sample = 32;
    CHECK(decoder.begin(rx));
    decoder.write((const uint8_t*)s32.data(), s32.size() * 4);
    decoder.end();
    CHECK_EQ(text.str, std::string(TEXT));
  }

  // 8 bit
  std::vector<int8_t> s8(mono.size());
  for (size_t j = 0; j < mono.size(); j++) s8[j] = mono[j] >> 8;
  {
    StringPrint text;
    FSKDecoder decoder(text);
    ModemConfig rx = cfg;
    rx.bits_per_sample = 8;
    CHECK(decoder.begin(rx));
    decoder.write((const uint8_t*)s8.data(), s8.size());
    decoder.end();
    CHECK_EQ(text.str, std::string(TEXT));
  }

  // stereo: signal on channel 1 only
  std::vector<int16_t> stereo(mono.size() * 2);
  for (size_t j = 0; j < mono.size(); j++) {
    stereo[2 * j] = 0;
    stereo[2 * j + 1] = mono[j];
  }
  {
    StringPrint text;
    FSKDecoder decoder(text);
    ModemConfig rx = cfg;
    rx.channels = 2;
    rx.channel = 1;
    CHECK(decoder.begin(rx));
    decoder.write((const uint8_t*)stereo.data(), stereo.size() * 2);
    decoder.end();
    CHECK_EQ(text.str, std::string(TEXT));
  }

  // invalid formats
  FSKDecoder decoder;
  ModemConfig rx = cfg;
  rx.bits_per_sample = 24;
  CHECK(!decoder.begin(rx));
  rx = cfg;
  rx.channel = 1;
  CHECK(!decoder.begin(rx));
}

static void test_audio_info_change() {
  // the decoder restarts with the new sample rate (e.g. from a WAV header)
  ModemConfig tx = modeConfig("1200");
  tx.sample_rate = 22050;
  std::vector<int16_t> samples = modulate(tx, TEXT);
  StringPrint text;
  FSKDecoder decoder(text);
  CHECK(decoder.begin(modeConfig("1200")));
  audio_tools::AudioInfo info(22050, 1, 16);
  decoder.setAudioInfo(info);
  CHECK_EQ(decoder.config().sample_rate, (sample_rate_t)22050);
  decoder.write((const uint8_t*)samples.data(), samples.size() * 2);
  decoder.end();
  CHECK_EQ(text.str, std::string(TEXT));
}

static void test_multiple_transmissions() {
  ModemConfig cfg = modeConfig("1200");
  SamplePrint audio;
  FSKEncoder encoder(audio);
  CHECK(encoder.begin(cfg));
  encoder.write((const uint8_t*)"ABC", 3);
  CHECK(encoder.isTransmitting());
  CHECK(encoder.endTransmission(cfg.sample_rate / 2));
  CHECK(!encoder.isTransmitting());
  CHECK(!encoder.endTransmission());
  encoder.write((const uint8_t*)"DEF", 3);
  encoder.end();

  std::string status;
  CHECK_EQ(demodulate(cfg, audio.samples(), &status), std::string("ABCDEF"));
  size_t first = status.find("### CARRIER");
  CHECK(first != std::string::npos);
  CHECK(status.find("### CARRIER", first + 1) != std::string::npos);

  // rx_one stops after the first transmission
  ModemConfig rx = cfg;
  rx.rx_one = true;
  CHECK_EQ(demodulate(rx, audio.samples()), std::string("ABC"));
}

static void test_output_filters() {
  ModemConfig cfg = modeConfig("1200");
  ModemConfig rx = cfg;
  rx.print_filter = true;
  std::string text("\x01\x7f" "A\tB\n", 6);
  CHECK_EQ(loopback(cfg, rx, text), std::string("..A\tB\n"));

  rx = cfg;
  rx.binary_output = true;
  CHECK_EQ(loopback(cfg, rx, "U"), std::string("10101010\n"));

  // quiet: no status messages
  StringPrint out, status;
  FSKDecoder decoder(out);
  decoder.setStatusOutput(status);
  decoder.setQuiet(true);
  CHECK(decoder.begin(cfg));
  std::vector<int16_t> samples = modulate(cfg, TEXT);
  decoder.write((const uint8_t*)samples.data(), samples.size() * 2);
  decoder.end();
  CHECK_EQ(out.str, std::string(TEXT));
  CHECK(status.str.empty());
}

static void test_callerid_rx() {
  // Bell 202 ASCII transmission which is received in callerid mode
  std::string bytes = readFile("testdata-callerid-mdmf.bytes");
  std::string expected = readFile("testdata-callerid-mdmf.txt");
  CHECK_EQ(loopback(modeConfig("1200"), modeConfig("callerid"), bytes),
           expected);
}

static void test_uic_rx() {
  // synthesize a UIC-751-3 frame: 8 sync bits "11110010" + 39 data bits
  ModemConfig cfg = modeConfig("uic-ground");
  ModemConfig tx = cfg;
  tx.resolve();
  const uint8_t id[6] = {9, 8, 7, 6, 5, 4};
  uint64_t data = uicBits(id, 0x08);
  SamplePrint audio;
  FSKEncoder encoder(audio);
  ModemConfig gen = modeConfig("600");
  gen.mark_freq = tx.mark_freq;
  gen.space_freq = tx.space_freq;
  CHECK(encoder.begin(gen));
  size_t bit = encoder.bitSamples();
  encoder.writeTone(tx.mark_freq, bit * 4);
  for (int rep = 0; rep < 2; rep++) {
    const char* sync = "11110010";
    for (int j = 0; j < 8; j++)
      encoder.writeTone(sync[j] == '1' ? tx.mark_freq : tx.space_freq, bit);
    for (int j = 0; j < 39; j++)
      encoder.writeTone((data >> j) & 1 ? tx.mark_freq : tx.space_freq, bit);
  }
  encoder.writeTone(tx.mark_freq, bit * 4);
  std::string result = demodulate(cfg, audio.samples());
  CHECK(result.find("Train ID: 987654 - Message: 08 (Speech)\n") !=
        std::string::npos);
}

static void test_stream_api() {
  LoopbackStream audio;
  ModemStream modem(audio, audio);
  ModemConfig cfg = modem.defaultConfig();
  StringPrint status;
  modem.setStatusOutput(status);
  CHECK(modem.begin(cfg, 1024));
  modem.print("Hello World");
  modem.flush();
  modem.getEncoder().writeSilence(cfg.sample_rate / 4);

  std::string result;
  for (int j = 0; j < 10000 && audio.available() > 0; j++) {
    while (modem.available() > 0) result += (char)modem.read();
  }
  while (modem.available() > 0) result += (char)modem.read();
  CHECK_EQ(result, std::string("Hello World"));
  CHECK_EQ(modem.read(), -1);
  CHECK(status.str.find("### CARRIER") != std::string::npos);
}

static void test_mode_switch() {
  // setMode() does not depend on the previous mode
  FSKEncoder encoder;
  ModemConfig c = encoder.defaultConfig();
  CHECK(c.setMode("rtty"));
  CHECK_EQ(c.data_bits, 5);
  CHECK_EQ(c.stop_bits, 1.5f);
  CHECK(c.format == DataFormat::BAUDOT);
  CHECK_EQ(loopback(c, c, BAUDOT_TEXT), std::string(BAUDOT_TEXT));

  c.setMode("same");
  c.setMode("1200");
  CHECK(!c.rx_sync);
  CHECK_EQ(c.tx_sync_bytes, 0);
  CHECK_EQ(c.start_bits, -1);
  CHECK(c.expect_bits == nullptr);
  CHECK(c.resolve());
  CHECK_EQ(c.mark_freq, 1200.0f);

  c.setMode("uic-train");
  c.setMode("tdd");
  CHECK(c.format == DataFormat::BAUDOT);
  CHECK_EQ(c.start_bits, -1);
  CHECK(c.expect_bits == nullptr);

  // user settings which are not part of the mode are kept
  c.volume = 0.3f;
  c.inverted = true;
  c.setMode("300");
  CHECK_EQ(c.volume, 0.3f);
  CHECK(c.inverted);

  // command line: options override the mode defaults
  ModemConfig opt;
  opt.data_bits = 7;
  opt.stop_bits = 2;
  CHECK(opt.setMode("1200", true));
  CHECK_EQ(opt.data_bits, 7);
  CHECK_EQ(opt.stop_bits, 2.0f);

  // "uic" without direction
  ModemConfig uic;
  CHECK(uic.setMode("uic"));
  CHECK(uic.format == DataFormat::UIC_GROUND);
}

static void test_config_reuse() {
  // change the configuration of a running decoder and restart it
  StringPrint text;
  FSKDecoder decoder(text);
  decoder.setQuiet(true);
  CHECK(decoder.begin(modeConfig("1200")));
  CHECK_EQ(decoder.activeConfig().mark_freq, 1200.0f);
  decoder.config().setMode("300");
  CHECK(decoder.begin());
  CHECK_EQ(decoder.activeConfig().mark_freq, 1270.0f);
  CHECK_EQ(decoder.activeConfig().bandwidth, 50.0f);
  std::vector<int16_t> samples = modulate(modeConfig("300"), TEXT);
  decoder.write((const uint8_t*)samples.data(), samples.size() * 2);
  decoder.end();
  CHECK_EQ(text.str, std::string(TEXT));

  // a copy of the active configuration is resolved again
  FSKEncoder encoder;
  CHECK(encoder.begin(modeConfig("1200")));
  ModemConfig copy = encoder.config();
  copy.baud_rate = 300;
  CHECK(encoder.begin(copy));
  CHECK_EQ(encoder.activeConfig().mark_freq, 1270.0f);
}

static void test_baudot_transmissions() {
  // the shift state must not leak into the next transmission
  const char* modes[] = {"rtty", "tdd"};
  for (const char* mode : modes) {
    ModemConfig cfg = modeConfig(mode);
    cfg.sample_rate = 8000;
    SamplePrint audio;
    FSKEncoder encoder(audio);
    CHECK(encoder.begin(cfg));
    const char* parts[] = {"123\n", "456\n", "ABC\n", "7 X\n"};
    std::string expected;
    for (const char* part : parts) {
      encoder.write((const uint8_t*)part, strlen(part));
      encoder.endTransmission(cfg.sample_rate);
      expected += part;
    }
    encoder.end();
    CHECK_EQ(demodulate(cfg, audio.samples()), expected);
  }
}

static void test_stream_modes() {
  LoopbackStream audio;
  // a Stream with a single argument is used for output only
  ModemStream tx(audio);
  CHECK(tx.begin(tx.defaultConfig()));
  tx.print("A");
  tx.flush();
  CHECK(audio.available() > 0);
  CHECK_EQ(tx.available(), 0);

  LoopbackStream audio2;
  ModemStream tx2(audio2, TX_MODE);
  CHECK(tx2.begin(tx2.defaultConfig()));
  CHECK(tx2.print("A") > 0);
  CHECK(audio2.available() > 0);

  // receive only: nothing is transmitted
  LoopbackStream audio3;
  ModemStream rx(audio3, RX_MODE);
  CHECK(rx.begin(rx.defaultConfig()));
  CHECK_EQ(rx.print("A"), (size_t)0);
  CHECK_EQ(audio3.available(), 0);

  // both
  LoopbackStream audio4;
  ModemStream both(audio4, RXTX_MODE);
  CHECK(both.begin(both.defaultConfig()));
  both.print("Hi");
  both.flush();
  both.getEncoder().writeSilence(both.defaultConfig().sample_rate / 4);
  std::string result;
  while (audio4.available() > 0 || both.available() > 0) {
    while (both.available() > 0) result += (char)both.read();
  }
  CHECK_EQ(result, std::string("Hi"));
}

static void test_stream_no_loss() {
  // a Caller-ID message is decoded at once: a small receive buffer and a
  // slow reader must not lose data
  std::string bytes = readFile("testdata-callerid-mdmf.bytes");
  std::string expected = readFile("testdata-callerid-mdmf.txt");
  std::vector<int16_t> samples = modulate(modeConfig("1200"), bytes,
                                          48000 / 4);
  LoopbackStream audio;
  audio.write((const uint8_t*)samples.data(), samples.size() * 2);
  ModemStream modem(audio, RX_MODE);
  CHECK(modem.begin(modeConfig("callerid"), 4));
  std::string result;
  for (int j = 0; j < 1000000 && (audio.available() > 0 || modem.available() > 0); j++) {
    // read one character at a time
    if (modem.available() > 0) result += (char)modem.read();
  }
  modem.end();
  while (modem.available() > 0) result += (char)modem.read();
  CHECK_EQ(result, expected);
}

/// Reads from the stream until the expected length or a timeout
static std::string readFor(Stream& in, size_t len, int timeout_ms = 5000) {
  std::string result;
  auto start = std::chrono::steady_clock::now();
  while (result.size() < len &&
         std::chrono::steady_clock::now() - start <
             std::chrono::milliseconds(timeout_ms)) {
    if (in.available() > 0)
      result += (char)in.read();
    else
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return result;
}

static void test_stream_rtos() {
  // receive only: the task decodes, a slow reader with a small buffer
  // must not lose the Caller-ID message
  std::string bytes = readFile("testdata-callerid-mdmf.bytes");
  std::string expected = readFile("testdata-callerid-mdmf.txt");
  std::vector<int16_t> samples = modulate(modeConfig("1200"), bytes, 48000);
  SyncLoopbackStream audio;
  audio.write((const uint8_t*)samples.data(), samples.size() * 2);
  ModemStreamRTOS rx(audio, RX_MODE);
  CHECK(rx.begin(modeConfig("callerid"), 4));
  CHECK(rx.isRunning());
  CHECK_EQ(readFor(rx, expected.size()), expected);
  rx.end();
  CHECK(!rx.isRunning());

  // restart with another mode
  std::vector<int16_t> samples300 = modulate(modeConfig("300"), TEXT, 48000);
  audio.write((const uint8_t*)samples300.data(), samples300.size() * 2);
  CHECK(rx.begin(modeConfig("300")));
  CHECK_EQ(readFor(rx, strlen(TEXT)), std::string(TEXT));
  rx.end();

  // transmit and receive at the same time
  SyncLoopbackStream audio2;
  ModemStreamRTOS modem(audio2, RXTX_MODE);
  StringPrint status;
  ModemConfig cfg = modem.defaultConfig();
  CHECK(modem.begin(cfg));
  std::string sent;
  for (int j = 0; j < 5; j++) {
    modem.print(TEXT);
    modem.flush();
    modem.getEncoder().writeSilence(cfg.sample_rate / 10);
    sent += TEXT;
  }
  modem.getEncoder().writeSilence(cfg.sample_rate / 2);
  CHECK_EQ(readFor(modem, sent.size()), sent);
  modem.end();
  CHECK_EQ(modem.update(), (size_t)0);
}

/// Collects the written bytes
struct BytePrint : public Print {
  std::string data;
  size_t write(uint8_t ch) override {
    data += (char)ch;
    return 1;
  }
};

static void test_packet_codec() {
  // round trip of all sizes, including the framing bytes 0x7E and 0x7D
  for (int size : {1, 2, 17, 63, 64, 65, 200}) {
    std::string payload;
    for (int j = 0; j < size; j++) payload += (char)(j * 37 + 0x7D);
    payload[0] = 0x7E;
    BytePrint wire, out;
    PacketEncoder<64, 16> encoder(wire);
    PacketDecoder<64, 16> decoder(out);
    encoder.write((const uint8_t*)payload.data(), payload.size());
    encoder.flush();
    decoder.write((const uint8_t*)wire.data.data(), wire.data.size());
    CHECK_EQ(out.data, payload);
    CHECK_EQ(decoder.statistics().received, (unsigned)((size + 63) / 64));
    CHECK_EQ(decoder.statistics().dropped, 0u);
  }

  // up to ECC_BYTES / 2 wrong bytes are corrected
  std::string payload = "Reed-Solomon corrects up to 8 bytes per packet";
  for (int nerrors : {1, 4, 8, 9, 12}) {
    BytePrint wire, out;
    PacketEncoder<64, 16> encoder(wire);
    PacketDecoder<64, 16> decoder(out);
    encoder.write((const uint8_t*)payload.data(), payload.size());
    encoder.flush();
    // corrupt data bytes, but not the flags and escapes
    std::string& w = wire.data;
    int done = 0;
    for (size_t j = 1; j + 1 < w.size() && done < nerrors; j += 5) {
      if ((uint8_t)w[j] == 0x7E || (uint8_t)w[j] == 0x7D ||
          (uint8_t)w[j - 1] == 0x7D)
        continue;
      uint8_t v = (uint8_t)w[j] ^ 0x5A;
      if (v == 0x7E || v == 0x7D) v ^= 0x01;
      w[j] = (char)v;
      done++;
    }
    decoder.write((const uint8_t*)w.data(), w.size());
    if (nerrors <= 8) {
      CHECK_EQ(out.data, payload);
      CHECK_EQ(decoder.statistics().corrected, 1u);
      CHECK_EQ(decoder.statistics().corrected_bytes, (unsigned)nerrors);
    } else {
      // never deliver wrong data
      CHECK_EQ(out.data, std::string(""));
      CHECK_EQ(decoder.statistics().dropped, 1u);
    }
  }

  // a lost byte drops the packet, the next packet is received
  BytePrint wire, out;
  PacketEncoder<64, 16> encoder(wire);
  PacketDecoder<64, 16> decoder(out);
  encoder.write((const uint8_t*)"first", 5);
  encoder.flush();
  encoder.write((const uint8_t*)"second", 6);
  encoder.flush();
  wire.data.erase(3, 1);
  decoder.write((const uint8_t*)wire.data.data(), wire.data.size());
  CHECK_EQ(out.data, std::string("second"));
  CHECK_EQ(decoder.statistics().dropped, 1u);
}

static void test_packet_modem() {
  // packets over a noisy FSK channel: no wrong data is delivered
  ModemConfig cfg = modeConfig("1200");
  cfg.sample_rate = 8000;
  SyncLoopbackStream audio;
  ModemStream tx(audio);
  CHECK(tx.begin(cfg));
  PacketStream<32, 16> tx_packets(tx);
  std::vector<std::string> lines;
  for (int j = 0; j < 40; j++) {
    char line[40];
    snprintf(line, sizeof(line), "packet %02d: the quick brown fox\n", j);
    lines.push_back(line);
    tx_packets.print(line);
    tx_packets.flush();  // one packet per transmission
  }
  tx.getEncoder().writeSilence(cfg.sample_rate / 4);

  ModemConfig rx_cfg = cfg;
  rx_cfg.rx_noise = 0.15f;
  ModemStream rx(audio, RX_MODE);
  CHECK(rx.begin(rx_cfg));
  PacketStream<32, 16> rx_packets(rx);
  CHECK(rx_packets.begin());
  std::string result;
  while (audio.available() > 0) {
    while (rx_packets.available() > 0) result += (char)rx_packets.read();
  }
  rx.end();
  while (rx_packets.available() > 0) result += (char)rx_packets.read();

  // all received lines are correct and in order
  std::istringstream in(result);
  std::string line;
  size_t idx = 0, count = 0;
  while (std::getline(in, line)) {
    line += "\n";
    while (idx < lines.size() && lines[idx] != line) idx++;
    CHECK(idx < lines.size());
    count++;
  }
  const PacketStatistics& stats = rx_packets.statistics();
  fprintf(stderr, "packets: received=%u corrected=%u bytes=%u dropped=%u\n",
          stats.received, stats.corrected, stats.corrected_bytes,
          stats.dropped);
  CHECK_EQ(count, (size_t)stats.received);
  CHECK(stats.received >= 30);
  CHECK(stats.corrected > 0);
}

/// MFSK: modulates the text in the indicated number of transmissions
static std::vector<int16_t> mfskModulate(MFSKConfig cfg, const std::string& text,
                                         int parts = 1) {
  SamplePrint audio;
  MFSKEncoder encoder(audio);
  CHECK(encoder.begin(cfg));
  size_t chunk = text.size() / parts;
  for (int p = 0; p < parts; p++) {
    size_t len = p == parts - 1 ? text.size() - p * chunk : chunk;
    encoder.write((const uint8_t*)text.data() + p * chunk, len);
    encoder.endTransmission(cfg.sample_rate / 20);
  }
  encoder.end();
  return audio.samples();
}

/// MFSK: demodulates 16 bit mono samples in chunks of the indicated size
static std::string mfskDemodulate(MFSKConfig cfg,
                                  const std::vector<int16_t>& samples,
                                  std::string* status = nullptr,
                                  size_t chunk = 1024) {
  StringPrint text, status_out;
  MFSKDecoder decoder(text);
  decoder.setStatusOutput(status_out);
  CHECK(decoder.begin(cfg));
  const uint8_t* data = (const uint8_t*)samples.data();
  size_t len = samples.size() * 2;
  for (size_t pos = 0; pos < len; pos += chunk)
    decoder.write(data + pos, len - pos < chunk ? len - pos : chunk);
  decoder.end();
  if (status) *status = status_out.str;
  return text.str;
}

static MFSKConfig mfskConfig(const char* mode, int sample_rate = 8000) {
  MFSKConfig cfg;
  CHECK(cfg.setMode(mode));
  cfg.sample_rate = sample_rate;
  return cfg;
}

static int countOf(const std::string& str, const char* what) {
  int n = 0;
  for (size_t pos = 0; (pos = str.find(what, pos)) != std::string::npos; pos++)
    n++;
  return n;
}

static void test_mfsk_config() {
  MFSKConfig cfg;
  CHECK(cfg.setMode("mfsk16"));
  CHECK_EQ(cfg.tones, 16);
  CHECK_EQ(cfg.bitsPerSymbol(), 4);
  CHECK_EQ(cfg.bitRate(), 500.0f);
  CHECK_EQ(cfg.toneFrequency(15), 2875.0f);
  CHECK_EQ(cfg.syncLength(), 2);
  CHECK(cfg.setMode("MFSK4"));
  CHECK_EQ(cfg.syncLength(), 4);
  CHECK(cfg.setMode("mfsk64"));
  CHECK_EQ(cfg.bitsPerSymbol(), 6);
  CHECK(!cfg.setMode("mfsk5"));
  CHECK(!cfg.setMode("1200"));
  CHECK(MFSKConfig::isMode("mfsk32"));
  CHECK(!MFSKConfig::isMode("mfs"));
  CHECK(!MFSKConfig::isMode("rtty"));

  // invalid configurations
  MFSKConfig bad = mfskConfig("mfsk16");
  bad.tones = 12;
  CHECK(!bad.isValid());
  bad = mfskConfig("mfsk16", 4000);  // highest tone above nyquist
  CHECK(!bad.isValid());
  bad = mfskConfig("mfsk4");
  bad.symbol_rate = 2000;  // 4 samples per symbol
  bad.tone_spacing = 100;
  CHECK(!bad.isValid());
  MFSKEncoder encoder;
  bad = mfskConfig("mfsk16");
  bad.bits_per_sample = 8;
  CHECK(!encoder.begin(bad));

  // gray code
  for (unsigned v = 0; v < 64; v++) {
    CHECK_EQ(grayDecode(grayEncode(v)), v);
    unsigned diff = grayEncode(v) ^ grayEncode(v + 1);
    CHECK(diff != 0 && (diff & (diff - 1)) == 0);  // one bit differs
  }
}

static void test_mfsk_loopback() {
  std::string binary;
  for (int j = 0; j < 256; j++) binary += (char)j;
  for (const char* mode : {"mfsk4", "mfsk8", "mfsk16", "mfsk32", "mfsk64"}) {
    for (int rate : {8000, 22050, 48000}) {
      MFSKConfig cfg = mfskConfig(mode, rate);
      std::string status;
      std::string result = mfskDemodulate(cfg, mfskModulate(cfg, TEXT), &status);
      if (result != TEXT) fprintf(stderr, "%s %d failed\n", mode, rate);
      CHECK_EQ(result, std::string(TEXT));
      CHECK_EQ(countOf(status, "### CARRIER"), 1);
      CHECK_EQ(countOf(status, "### NOCARRIER ndata=55 "), 1);
    }
    // all byte values
    MFSKConfig cfg = mfskConfig(mode);
    CHECK_EQ(mfskDemodulate(cfg, mfskModulate(cfg, binary)), binary);
  }
}

static void test_mfsk_robust() {
  std::string text;
  for (int j = 0; j < 10; j++) text += TEXT;

  // noise: more tones are more robust
  for (const char* mode : {"mfsk4", "mfsk16", "mfsk64"}) {
    MFSKConfig cfg = mfskConfig(mode);
    MFSKConfig rx = cfg;
    rx.rx_noise = 0.5f;
    CHECK_EQ(mfskDemodulate(rx, mfskModulate(cfg, text)), text);
  }
  MFSKConfig cfg = mfskConfig("mfsk64");
  MFSKConfig rx = cfg;
  rx.rx_noise = 1.0f;
  CHECK_EQ(mfskDemodulate(rx, mfskModulate(cfg, text)), text);

  // different clocks of sender and receiver
  for (const char* mode : {"mfsk4", "mfsk16", "mfsk64"}) {
    for (float error : {-0.03f, -0.01f, 0.005f, 0.02f}) {
      MFSKConfig tx = mfskConfig(mode);
      tx.sample_rate = 8000 * (1.0f + error);
      if (mfskDemodulate(mfskConfig(mode), mfskModulate(tx, text)) != text)
        fprintf(stderr, "%s clock error %.3f failed\n", mode, error);
      CHECK_EQ(mfskDemodulate(mfskConfig(mode), mfskModulate(tx, text)), text);
    }
  }

  // noise and clock error and many short transmissions
  for (const char* mode : {"mfsk4", "mfsk16"}) {
    MFSKConfig tx = mfskConfig(mode);
    tx.sample_rate = 8000 * 1.02f;
    MFSKConfig rx = mfskConfig(mode);
    rx.rx_noise = 0.5f;
    std::string status;
    CHECK_EQ(mfskDemodulate(rx, mfskModulate(tx, text, 20), &status), text);
    CHECK_EQ(countOf(status, "### CARRIER"), 20);
  }

  // volume
  for (float volume : {1.0f, 0.1f, 0.01f}) {
    MFSKConfig cfg = mfskConfig("mfsk16");
    cfg.volume = volume;
    std::string status;
    CHECK_EQ(mfskDemodulate(cfg, mfskModulate(cfg, TEXT), &status),
             std::string(TEXT));
    size_t pos = status.find("ampl=");
    CHECK(pos != std::string::npos);
    CHECK_NEAR(atof(status.c_str() + pos + 5), volume, volume * 0.05);
  }

  // chunk sizes
  cfg = mfskConfig("mfsk16");
  std::vector<int16_t> samples = mfskModulate(cfg, TEXT);
  for (size_t chunk : {1, 3, 64, 1000, 100000})
    CHECK_EQ(mfskDemodulate(cfg, samples, nullptr, chunk), std::string(TEXT));

  // rx_one stops after the first transmission
  cfg.rx_one = true;
  CHECK_EQ(mfskDemodulate(cfg, mfskModulate(cfg, "ABCDEF", 2)),
           std::string("ABC"));
}

static void test_mfsk_formats() {
  // 8 and 32 bit stereo input with the signal on channel 1
  MFSKConfig cfg = mfskConfig("mfsk16");
  std::vector<int16_t> samples = mfskModulate(cfg, TEXT);
  for (int bits : {8, 32}) {
    std::vector<uint8_t> data;
    for (int16_t s : samples) {
      for (int ch = 0; ch < 2; ch++) {
        int16_t v = ch == 1 ? s : 0;
        if (bits == 8) {
          data.push_back((uint8_t)(int8_t)(v >> 8));
        } else {
          int32_t v32 = (int32_t)v << 16;
          uint8_t b[4];
          memcpy(b, &v32, 4);
          data.insert(data.end(), b, b + 4);
        }
      }
    }
    MFSKConfig rx = cfg;
    rx.bits_per_sample = bits;
    rx.channels = 2;
    rx.channel = 1;
    StringPrint text;
    MFSKDecoder decoder(text);
    CHECK(decoder.begin(rx));
    decoder.write(data.data(), data.size());
    decoder.end();
    CHECK_EQ(text.str, std::string(TEXT));
  }

  // stereo output: identical channels
  cfg.channels = 2;
  std::vector<int16_t> stereo = mfskModulate(cfg, "A");
  bool same = true;
  for (size_t j = 0; j < stereo.size(); j += 2)
    same = same && stereo[j] == stereo[j + 1];
  CHECK(same);
}

static void test_mfsk_stream() {
  // MFSKStream
  SyncLoopbackStream audio;
  MFSKStream modem(audio, RXTX_MODE);
  MFSKConfig cfg = mfskConfig("mfsk16");
  CHECK(modem.begin(cfg));
  modem.print("Hello MFSK");
  modem.flush();
  modem.getEncoder().writeSilence(cfg.sample_rate / 4);
  std::string result;
  while (audio.available() > 0 || modem.available() > 0)
    while (modem.available() > 0) result += (char)modem.read();
  CHECK_EQ(result, std::string("Hello MFSK"));

  // packets over a very noisy MFSK channel
  SyncLoopbackStream audio2;
  MFSKStream tx(audio2);
  CHECK(tx.begin(cfg));
  PacketStream<32, 16> tx_packets(tx);
  std::string sent;
  for (int j = 0; j < 10; j++) {
    char line[40];
    snprintf(line, sizeof(line), "packet %02d: the quick brown fox\n", j);
    sent += line;
    tx_packets.print(line);
    tx_packets.flush();
  }
  tx.getEncoder().writeSilence(cfg.sample_rate / 4);
  MFSKConfig rx_cfg = cfg;
  rx_cfg.rx_noise = 1.2f;
  MFSKStream rx(audio2, RX_MODE);
  CHECK(rx.begin(rx_cfg));
  PacketStream<32, 16> rx_packets(rx);
  result.clear();
  while (audio2.available() > 0)
    while (rx_packets.available() > 0) result += (char)rx_packets.read();
  rx.end();
  while (rx_packets.available() > 0) result += (char)rx_packets.read();
  CHECK_EQ(result, sent);

  // MFSKStreamRTOS
  std::vector<int16_t> samples = mfskModulate(cfg, TEXT);
  SyncLoopbackStream audio3;
  audio3.write((const uint8_t*)samples.data(), samples.size() * 2);
  MFSKStreamRTOS rtos(audio3, RX_MODE);
  CHECK(rtos.begin(cfg));
  CHECK_EQ(readFor(rtos, strlen(TEXT)), std::string(TEXT));
  rtos.end();
}

static const char* APRS_TEXT =
    "N0CALL>APRS,WIDE1-1,WIDE2-1:!4903.50N/07201.75W-Test 1\n"
    "N0CALL-7>APDW12:>Status message 2\n"
    "WB2OSZ-15>TEST,RELAY*:,The quick brown fox jumps over the lazy dog!\n";

static std::vector<int16_t> ax25Modulate(AX25Config cfg, const std::string& data) {
  SamplePrint audio;
  AX25Encoder encoder(audio);
  CHECK(encoder.begin(cfg));
  encoder.write((const uint8_t*)data.data(), data.size());
  encoder.endTransmission(cfg.sample_rate / 10);
  encoder.end();
  return audio.samples();
}

static std::string ax25Demodulate(AX25Config cfg,
                                  const std::vector<int16_t>& samples,
                                  std::string* status = nullptr) {
  StringPrint text, status_out;
  AX25Decoder decoder(text);
  decoder.setStatusOutput(status_out);
  CHECK(decoder.begin(cfg));
  decoder.write((const uint8_t*)samples.data(), samples.size() * 2);
  decoder.end();
  if (status) *status = status_out.str;
  return text.str;
}

static void test_ax25_frame() {
  // FCS check value of CRC-16/X-25
  CHECK_EQ(AX25Frame::fcs((const uint8_t*)"123456789", 9), (uint16_t)0x906E);

  // text -> frame -> text
  const char* lines[] = {"N0CALL>APRS:>Hello",
                         "N0CALL-15>APDW12,WIDE1-1,WIDE2-2:!4903.50N/07201.75W-",
                         "AB1CDE-3>CQ,RELAY*,WIDE:info with : colon"};
  for (const char* line : lines) {
    uint8_t frame[AX25Frame::MAX_FRAME];
    int len = AX25Frame::fromText(line, strlen(line), frame);
    CHECK(len > 0);
    char text[400];
    CHECK(AX25Frame::toText(frame, len, text, sizeof(text)) > 0);
    CHECK_EQ(std::string(text), std::string(line));
  }

  // address encoding: callsign shifted, SSID, command bit, last address
  uint8_t frame[AX25Frame::MAX_FRAME];
  int len = AX25Frame::fromText("N0CALL-7>APRS:x", 15, frame);
  CHECK_EQ(len, 7 + 7 + 2 + 1);
  CHECK_EQ(frame[0], (uint8_t)('A' << 1));
  CHECK_EQ(frame[6], (uint8_t)0xE0);              // dest: command bit
  CHECK_EQ(frame[7], (uint8_t)('N' << 1));
  CHECK_EQ(frame[13], (uint8_t)(0x60 | (7 << 1) | 1));  // source, last
  CHECK_EQ(frame[14], (uint8_t)0x03);
  CHECK_EQ(frame[15], (uint8_t)0xF0);

  // invalid input
  CHECK_EQ(AX25Frame::fromText("no separator", 12, frame), 0);
  CHECK_EQ(AX25Frame::fromText("TOOLONGCALL>APRS:x", 18, frame), 0);
  CHECK_EQ(AX25Frame::fromText("N0CALL-16>APRS:x", 16, frame), 0);
  CHECK(AX25Config::isMode("APRS") && AX25Config::isMode("ax25"));
  CHECK(!AX25Config::isMode("1200"));
}

static void test_ax25_loopback() {
  for (int rate : {8000, 11025, 22050, 44100, 48000}) {
    AX25Config cfg;
    cfg.sample_rate = rate;
    std::string status;
    std::string result = ax25Demodulate(cfg, ax25Modulate(cfg, APRS_TEXT), &status);
    if (result != APRS_TEXT) fprintf(stderr, "ax25 %d failed\n", rate);
    CHECK_EQ(result, std::string(APRS_TEXT));
    CHECK(status.find("frames=3 fcs_errors=0") != std::string::npos);
  }

  // noise and a different clock of the sender
  AX25Config cfg;
  cfg.sample_rate = 48000;
  AX25Config rx = cfg;
  rx.rx_noise = 0.5f;
  CHECK_EQ(ax25Demodulate(rx, ax25Modulate(cfg, APRS_TEXT)), std::string(APRS_TEXT));
  for (float error : {-0.01f, 0.01f}) {
    AX25Config tx = cfg;
    tx.sample_rate = 48000 * (1.0f + error);
    CHECK_EQ(ax25Demodulate(cfg, ax25Modulate(tx, APRS_TEXT)),
             std::string(APRS_TEXT));
  }

  // a corrupted frame is not provided
  std::vector<int16_t> samples = ax25Modulate(cfg, "N0CALL>APRS:>Hello\n");
  for (size_t j = samples.size() / 2; j < samples.size() / 2 + 200; j++)
    samples[j] = -samples[j];
  std::string status;
  CHECK_EQ(ax25Demodulate(cfg, samples, &status), std::string(""));
}

static void test_ax25_kiss() {
  AX25Config cfg;
  cfg.format = AX25Format::KISS;
  // KISS frames from the text
  uint8_t frame[AX25Frame::MAX_FRAME];
  StringPrint kiss;
  std::istringstream in(APRS_TEXT);
  std::string line;
  while (std::getline(in, line)) {
    int len = AX25Frame::fromText(line.data(), line.size(), frame);
    // a byte which needs to be escaped
    frame[len++] = 0xC0;
    AX25Frame::writeKiss(kiss, frame, len);
  }
  CHECK(kiss.str.find("\xDB\xDC") != std::string::npos);
  // KISS -> audio -> KISS
  CHECK_EQ(ax25Demodulate(cfg, ax25Modulate(cfg, kiss.str)), kiss.str);
}

static void test_ax25_stream() {
  SyncLoopbackStream audio;
  AX25Stream modem(audio, RXTX_MODE);
  AX25Config cfg = modem.defaultConfig();
  CHECK(modem.begin(cfg));
  modem.print(APRS_TEXT);
  modem.flush();
  modem.getEncoder().writeSilence(cfg.sample_rate / 4);
  std::string result;
  while (audio.available() > 0 || modem.available() > 0)
    while (modem.available() > 0) result += (char)modem.read();
  CHECK_EQ(result, std::string(APRS_TEXT));
}

static std::string pskRun(PSKConfig tx, PSKConfig rx, const std::string& text,
                          int parts = 1, std::string* status = nullptr) {
  SamplePrint audio;
  PSKEncoder encoder(audio);
  CHECK(encoder.begin(tx));
  size_t chunk = text.size() / parts;
  for (int p = 0; p < parts; p++) {
    size_t len = p == parts - 1 ? text.size() - p * chunk : chunk;
    encoder.write((const uint8_t*)text.data() + p * chunk, len);
    encoder.endTransmission(tx.sample_rate / 5);
  }
  encoder.end();
  std::vector<int16_t> samples = audio.samples();
  StringPrint out, status_out;
  PSKDecoder decoder(out);
  decoder.setStatusOutput(status_out);
  CHECK(decoder.begin(rx));
  decoder.write((const uint8_t*)samples.data(), samples.size() * 2);
  decoder.end();
  if (status) *status = status_out.str;
  return out.str;
}

static PSKConfig pskConfig(const char* mode, int sample_rate = 8000) {
  PSKConfig cfg;
  CHECK(cfg.setMode(mode));
  cfg.sample_rate = sample_rate;
  return cfg;
}

static void test_psk_varicode() {
  // known codes of the PSK31 specification
  CHECK_EQ(Varicode::encode(' ').bits, (uint16_t)0x1);
  CHECK_EQ(Varicode::encode('e').bits, (uint16_t)0x3);
  CHECK_EQ(Varicode::encode('E').bits, (uint16_t)0x77);   // 1110111
  CHECK_EQ(Varicode::encode('\n').bits, (uint16_t)0x1D);  // 11101
  CHECK_EQ((int)Varicode::encode('\n').length, 5);
  for (int j = 0; j < 256; j++) {
    Varicode::Code code = Varicode::encode(j);
    CHECK_EQ(Varicode::decode(code.bits), j);
    // starts and ends with 1, no "00"
    CHECK((code.bits >> (code.length - 1)) == 1 && (code.bits & 1) == 1);
    for (int b = 0; b + 1 < code.length; b++)
      CHECK(((code.bits >> b) & 3) != 0);
  }
  PSKConfig cfg;
  CHECK(cfg.setMode("PSK63"));
  CHECK_EQ(cfg.symbol_rate, 62.5f);
  CHECK(!cfg.setMode("psk64"));
  CHECK(PSKConfig::isMode("psk500") && !PSKConfig::isMode("mfsk16"));
}

static void test_psk_loopback() {
  std::string binary;
  for (int j = 0; j < 256; j++) binary += (char)j;
  for (const char* mode : {"psk31", "psk63", "psk125", "psk250", "psk500"}) {
    for (int rate : {8000, 48000}) {
      PSKConfig cfg = pskConfig(mode, rate);
      std::string status;
      std::string result = pskRun(cfg, cfg, TEXT, 1, &status);
      if (result != TEXT) fprintf(stderr, "%s %d failed\n", mode, rate);
      CHECK_EQ(result, std::string(TEXT));
      CHECK(status.find("### NOCARRIER ndata=55 ") != std::string::npos);
    }
  }
  PSKConfig cfg = pskConfig("psk125");
  CHECK_EQ(pskRun(cfg, cfg, binary), binary);
}

static void test_psk_robust() {
  std::string text;
  for (int j = 0; j < 5; j++) text += TEXT;
  // noise
  for (const char* mode : {"psk31", "psk125"}) {
    PSKConfig tx = pskConfig(mode);
    PSKConfig rx = tx;
    rx.rx_noise = 1.0f;
    CHECK_EQ(pskRun(tx, rx, text), text);
  }
  // frequency offset (automatic frequency control)
  for (float offset : {-10.0f, -4.0f, 7.0f}) {
    PSKConfig tx = pskConfig("psk31");
    tx.carrier_freq += offset;
    CHECK_EQ(pskRun(tx, pskConfig("psk31"), text), text);
  }
  PSKConfig tx = pskConfig("psk125");
  tx.carrier_freq += 30;
  CHECK_EQ(pskRun(tx, pskConfig("psk125"), text), text);
  // different clocks
  for (float error : {-0.01f, 0.005f, 0.01f}) {
    for (const char* mode : {"psk31", "psk125"}) {
      PSKConfig tx = pskConfig(mode);
      tx.sample_rate = 8000 * (1.0f + error);
      CHECK_EQ(pskRun(tx, pskConfig(mode), text), text);
    }
  }
  // many transmissions
  std::string status;
  PSKConfig cfg = pskConfig("psk63");
  CHECK_EQ(pskRun(cfg, cfg, text, 5, &status), text);
  CHECK_EQ(countOf(status, "### CARRIER"), 5);
  // rx_one
  cfg.rx_one = true;
  CHECK_EQ(pskRun(cfg, cfg, "ABCDEF", 2), std::string("ABC"));
}

static void test_psk_stream() {
  SyncLoopbackStream audio;
  PSKStream modem(audio, RXTX_MODE);
  PSKConfig cfg = modem.defaultConfig();
  cfg.setMode("psk125");
  CHECK(modem.begin(cfg));
  modem.print("Hello PSK");
  modem.flush();
  modem.getEncoder().writeSilence(cfg.sample_rate / 4);
  std::string result;
  while (audio.available() > 0 || modem.available() > 0)
    while (modem.available() > 0) result += (char)modem.read();
  CHECK_EQ(result, std::string("Hello PSK"));
}

static std::vector<int16_t> ofdmModulate(OFDMConfig cfg, const std::string& text,
                                         int parts = 1) {
  SamplePrint audio;
  OFDMEncoder encoder(audio);
  CHECK(encoder.begin(cfg));
  size_t chunk = text.size() / parts;
  for (int p = 0; p < parts; p++) {
    size_t len = p == parts - 1 ? text.size() - p * chunk : chunk;
    encoder.write((const uint8_t*)text.data() + p * chunk, len);
    encoder.endTransmission(cfg.sample_rate / 10);
  }
  encoder.end();
  return audio.samples();
}

static std::string ofdmDemodulate(OFDMConfig cfg,
                                  const std::vector<int16_t>& samples,
                                  std::string* status = nullptr,
                                  size_t chunk = 4096) {
  StringPrint out, status_out;
  OFDMDecoder decoder(out);
  decoder.setStatusOutput(status_out);
  CHECK(decoder.begin(cfg));
  const uint8_t* data = (const uint8_t*)samples.data();
  size_t len = samples.size() * 2;
  for (size_t pos = 0; pos < len; pos += chunk)
    decoder.write(data + pos, len - pos < chunk ? len - pos : chunk);
  decoder.end();
  if (status) *status = status_out.str;
  return out.str;
}

static OFDMConfig ofdmConfig(const char* mode, int sample_rate = 8000) {
  OFDMConfig cfg;
  CHECK(cfg.setMode(mode));
  cfg.sample_rate = sample_rate;
  return cfg;
}

static void test_ofdm_config() {
  OFDMConfig cfg = ofdmConfig("ofdm32");
  CHECK_EQ(cfg.carriers, 32);
  CHECK_EQ(cfg.fftSize(), 128);
  CHECK_EQ(cfg.prefixSize(), 32);
  CHECK_EQ(cfg.firstBin() % 2, 0);
  CHECK_NEAR(cfg.bitRate(), 3200.0, 0.1);
  CHECK_NEAR(cfg.carrierFrequency(31), 2937.5, 0.1);
  CHECK(cfg.isValid());
  CHECK(!cfg.setMode("ofdm8"));
  CHECK(OFDMConfig::isMode("OFDM64") && !OFDMConfig::isMode("psk31"));
  OFDMConfig bad = ofdmConfig("ofdm64", 4000);  // above nyquist
  CHECK(!bad.isValid());
  CHECK_EQ(OFDMConfig::headerSymbols(16), 2);
  CHECK_EQ(OFDMConfig::headerSymbols(64), 1);
}

static void test_ofdm_loopback() {
  std::string binary, big;
  for (int j = 0; j < 256; j++) binary += (char)j;
  for (int j = 0; j < 20; j++) big += TEXT;  // more than one frame
  for (const char* mode : {"ofdm16", "ofdm32", "ofdm64"}) {
    for (int rate : {8000, 22050, 48000}) {
      OFDMConfig cfg = ofdmConfig(mode, rate);
      std::string status;
      std::string result = ofdmDemodulate(cfg, ofdmModulate(cfg, TEXT), &status);
      if (result != TEXT) fprintf(stderr, "%s %d failed\n", mode, rate);
      CHECK_EQ(result, std::string(TEXT));
      CHECK(status.find("len=55") != std::string::npos);
    }
    OFDMConfig cfg = ofdmConfig(mode);
    CHECK_EQ(ofdmDemodulate(cfg, ofdmModulate(cfg, binary)), binary);
    std::string status;
    CHECK_EQ(ofdmDemodulate(cfg, ofdmModulate(cfg, big), &status), big);
    CHECK_EQ(countOf(status, "### CARRIER"), 3);  // 512 + 512 + 76 bytes
  }
}

static void test_ofdm_robust() {
  std::string text;
  for (int j = 0; j < 8; j++) text += TEXT;
  for (const char* mode : {"ofdm16", "ofdm32", "ofdm64"}) {
    // noise
    OFDMConfig rx = ofdmConfig(mode);
    rx.rx_noise = 0.1f;
    CHECK_EQ(ofdmDemodulate(rx, ofdmModulate(ofdmConfig(mode), text)), text);
    // different clocks
    for (float error : {-0.001f, 0.001f}) {
      OFDMConfig tx = ofdmConfig(mode);
      tx.sample_rate = 8000 * (1.0f + error);
      CHECK_EQ(ofdmDemodulate(ofdmConfig(mode), ofdmModulate(tx, text)), text);
    }
    // echo of 1 ms with half the amplitude (within the cyclic prefix)
    OFDMConfig cfg = ofdmConfig(mode);
    std::vector<int16_t> s = ofdmModulate(cfg, text), echo(s.size());
    for (size_t i = 0; i < s.size(); i++)
      echo[i] = (s[i] + (i >= 8 ? s[i - 8] / 2 : 0)) * 2 / 3;
    CHECK_EQ(ofdmDemodulate(cfg, echo), text);
    // a steady tone before the frame: its periodic signal looks like an
    // endless sync plateau
    std::vector<int16_t> tone;
    for (int i = 0; i < 2 * 8000; i++)
      tone.push_back(16000 * sin(2 * M_PI * 1000 * i / 8000));
    tone.resize(tone.size() + 800);  // 0.1 s pause
    std::vector<int16_t> frame = ofdmModulate(cfg, text);
    tone.insert(tone.end(), frame.begin(), frame.end());
    CHECK_EQ(ofdmDemodulate(cfg, tone), text);
  }
  // volume, chunk sizes and many frames
  OFDMConfig cfg = ofdmConfig("ofdm32");
  cfg.volume = 0.05f;
  CHECK_EQ(ofdmDemodulate(cfg, ofdmModulate(cfg, text)), text);
  cfg.volume = 1.0f;
  std::vector<int16_t> samples = ofdmModulate(cfg, text, 10);
  for (size_t chunk : {1, 7, 1000}) {
    std::string status;
    CHECK_EQ(ofdmDemodulate(cfg, samples, &status, chunk), text);
    CHECK_EQ(countOf(status, "### CARRIER"), 10);
  }
  // the global sample index wraps around during the frames (after about 25
  // hours at 48000 Hz)
  struct WrapDecoder : public OFDMDecoder {
    using OFDMDecoder::OFDMDecoder;
    void startAt(uint32_t index) { base = now = index; }
  };
  StringPrint wrap_out;
  WrapDecoder wrap_decoder(wrap_out);
  CHECK(wrap_decoder.begin(cfg));
  wrap_decoder.startAt(0xFFFFFFFFu - 3000);
  wrap_decoder.write((const uint8_t*)samples.data(), samples.size() * 2);
  wrap_decoder.end();
  CHECK_EQ(wrap_out.str, text);
  cfg.rx_one = true;
  CHECK_EQ(ofdmDemodulate(cfg, ofdmModulate(cfg, "ABCDEF", 2)), std::string("ABC"));
}

static void test_ofdm_stream() {
  SyncLoopbackStream audio;
  OFDMStream modem(audio, RXTX_MODE);
  OFDMConfig cfg = modem.defaultConfig();
  CHECK(modem.begin(cfg));
  modem.print("Hello OFDM");
  modem.flush();
  modem.getEncoder().writeSilence(cfg.sample_rate / 4);
  std::string result;
  while (audio.available() > 0 || modem.available() > 0)
    while (modem.available() > 0) result += (char)modem.read();
  CHECK_EQ(result, std::string("Hello OFDM"));
}

/// With rx_one the decoding stops after the first transmission and stays
/// stopped after flush() until begin() is called again
template <class Encoder, class Decoder, class Config>
static void check_rx_one(Config cfg) {
  SamplePrint audio;
  Encoder encoder(audio);
  CHECK(encoder.begin(cfg));
  for (const char* text : {"AB", "CD"}) {
    encoder.write((const uint8_t*)text, 2);
    encoder.endTransmission(cfg.sample_rate / 2);
  }
  encoder.end();
  std::vector<int16_t> samples = audio.samples();
  const uint8_t* data = (const uint8_t*)samples.data();
  StringPrint out;
  Decoder decoder(out);
  cfg.rx_one = true;
  CHECK(decoder.begin(cfg));
  decoder.write(data, samples.size() * 2);
  decoder.flush();
  CHECK(decoder.isFinished());
  decoder.write(data, samples.size() * 2);
  decoder.flush();
  CHECK_EQ(out.str, std::string("AB"));
  CHECK(decoder.begin(cfg));
  CHECK(!decoder.isFinished());
}

static void test_rx_one() {
  ModemConfig fsk = modeConfig("1200");
  check_rx_one<FSKEncoder, FSKDecoder>(fsk);
  check_rx_one<MFSKEncoder, MFSKDecoder>(mfskConfig("mfsk16"));
  PSKConfig psk;
  CHECK(psk.setMode("psk125"));
  psk.sample_rate = 8000;
  check_rx_one<PSKEncoder, PSKDecoder>(psk);
  check_rx_one<OFDMEncoder, OFDMDecoder>(ofdmConfig("ofdm32"));
}

static void test_audiotools_pipeline() {
  // same setup as examples/loopback
  StringPrint text, status;
  FSKDecoder decoder;
  audio_tools::EncodedAudioOutput receiver(&text, &decoder);
  FSKEncoder encoder;
  audio_tools::EncodedAudioOutput transmitter(&receiver, &encoder);
  ModemConfig cfg;
  cfg.setMode("1200");
  cfg.sample_rate = 8000;
  decoder.setStatusOutput(status);
  CHECK(decoder.begin(cfg));
  CHECK(encoder.begin(cfg));
  CHECK(receiver.begin(cfg));
  CHECK(transmitter.begin(cfg));
  for (int j = 0; j < 3; j++) {
    transmitter.print("Hello from the Modem!\n");
    encoder.endTransmission(cfg.sample_rate / 2);
  }
  CHECK_EQ(text.str, std::string("Hello from the Modem!\n"
                                 "Hello from the Modem!\n"
                                 "Hello from the Modem!\n"));
  CHECK_EQ(decoder.config().sample_rate, (sample_rate_t)8000);
  CHECK(status.str.find("### NOCARRIER") != std::string::npos);
}

// ---------------------------------------------------------------------------

struct TestCase {
  const char* name;
  void (*fn)();
};

static TestCase tests[] = {
    {"config-modes", test_config_modes},
    {"baudot-codec", test_baudot_codec},
    {"callerid-mdmf", test_callerid_mdmf},
    {"callerid-sdmf", test_callerid_sdmf},
    {"uic-codec", test_uic_codec},
    {"binary-codec", test_binary_codec},
    {"bit-helpers", test_bit_helpers},
    {"encoder-signal", test_encoder_signal},
    {"custom-generator", test_custom_generator},
    {"loopback-modes", test_loopback_modes},
    {"loopback-rate-perfect", test_loopback_rate_perfect},
    {"loopback-options", test_loopback_options},
    {"loopback-sample-rates", test_loopback_sample_rates},
    {"rate-slop", test_rate_slop},
    {"amplitude", test_amplitude},
    {"noise", test_noise},
    {"auto-carrier", test_auto_carrier},
    {"chunk-sizes", test_chunk_sizes},
    {"input-formats", test_input_formats},
    {"audio-info-change", test_audio_info_change},
    {"multiple-transmissions", test_multiple_transmissions},
    {"output-filters", test_output_filters},
    {"callerid-rx", test_callerid_rx},
    {"uic-rx", test_uic_rx},
    {"stream-api", test_stream_api},
    {"audiotools-pipeline", test_audiotools_pipeline},
    {"mode-switch", test_mode_switch},
    {"config-reuse", test_config_reuse},
    {"baudot-transmissions", test_baudot_transmissions},
    {"stream-modes", test_stream_modes},
    {"stream-no-loss", test_stream_no_loss},
    {"stream-rtos", test_stream_rtos},
    {"packet-codec", test_packet_codec},
    {"packet-modem", test_packet_modem},
    {"mfsk-config", test_mfsk_config},
    {"mfsk-loopback", test_mfsk_loopback},
    {"mfsk-robust", test_mfsk_robust},
    {"mfsk-formats", test_mfsk_formats},
    {"mfsk-stream", test_mfsk_stream},
    {"ax25-frame", test_ax25_frame},
    {"ax25-loopback", test_ax25_loopback},
    {"ax25-kiss", test_ax25_kiss},
    {"ax25-stream", test_ax25_stream},
    {"psk-varicode", test_psk_varicode},
    {"psk-loopback", test_psk_loopback},
    {"psk-robust", test_psk_robust},
    {"psk-stream", test_psk_stream},
    {"ofdm-config", test_ofdm_config},
    {"ofdm-loopback", test_ofdm_loopback},
    {"ofdm-robust", test_ofdm_robust},
    {"ofdm-stream", test_ofdm_stream},
    {"rx-one", test_rx_one},
};

int main(int argc, char* argv[]) {
  int count = 0;
  for (TestCase& test : tests) {
    if (argc > 1 && strcmp(argv[1], test.name) != 0) continue;
    int before = failures;
    test.fn();
    printf("%s: %s\n", failures == before ? "PASS" : "FAIL", test.name);
    count++;
  }
  if (count == 0) {
    fprintf(stderr, "unknown test: %s\n", argc > 1 ? argv[1] : "");
    return 2;
  }
  return failures == 0 ? 0 : 1;
}
