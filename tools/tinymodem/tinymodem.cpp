/**
 * tinymodem command line tool which is based on the header only TinyModem
 * library and the arduino-audio-tools. The command line options and the
 * output of the FSK modes are compatible with the original minimodem.
 *
 * Copyright (C) 2026 Phil Schatzmann
 * Based on minimodem: Copyright (C) 2011-2020 Kamal Mostafa
 * <kamal@whence.com>
 *
 * License GPLv3+: GNU GPL version 3 or later
 */
#include <float.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include <chrono>

#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecWAV.h"
#ifdef TINYMODEM_PORTAUDIO
#include "AudioTools/AudioLibs/PortAudioStream.h"
#endif
#include "TinyModem.h"

using namespace audio_tools;

static const char* program_name = "tinymodem";
static volatile bool rx_stop = false;

/// Print which writes unbuffered to a file descriptor (stdout/stderr)
class FdPrint : public Print {
 public:
  FdPrint(int fd) : fd(fd) {}
  size_t write(uint8_t ch) override { return write(&ch, 1); }
  size_t write(const uint8_t* data, size_t len) override {
    size_t done = 0;
    while (done < len) {
      ssize_t n = ::write(fd, data + done, len - done);
      if (n <= 0) {
        perror("write");
        return done;
      }
      done += n;
    }
    return len;
  }

 protected:
  int fd;
};

/// Print which writes to a FILE
class FilePrint : public Print {
 public:
  FilePrint(FILE* file) : file(file) {}
  size_t write(uint8_t ch) override { return write(&ch, 1); }
  size_t write(const uint8_t* data, size_t len) override {
    return fwrite(data, 1, len, file);
  }

 protected:
  FILE* file;
};

/// Print which counts the written bytes (benchmarks)
class NullPrint : public Print {
 public:
  size_t write(uint8_t) override { return 1; }
  size_t write(const uint8_t*, size_t len) override { return len; }
};

/// Print which forwards the data to an AudioWriter (e.g. a decoder)
class WriterPrint : public Print {
 public:
  WriterPrint(AudioWriter& writer) : writer(writer) {}
  size_t write(uint8_t ch) override { return write(&ch, 1); }
  size_t write(const uint8_t* data, size_t len) override {
    return writer.write(data, len);
  }

 protected:
  AudioWriter& writer;
};

static FdPrint out_stdout(1);
static FdPrint out_stderr(2);

static void version() {
  printf(
      "tinymodem %s\n"
      "Copyright (C) 2026 Phil Schatzmann\n"
      "Based on minimodem: Copyright (C) 2011-2020 Kamal Mostafa "
      "<kamal@whence.com>\n"
      "License GPLv3+: GNU GPL version 3 or later "
      "<http://gnu.org/licenses/gpl.html>.\n"
      "This is free software: you are free to change and redistribute it.\n"
      "There is NO WARRANTY, to the extent permitted by law.\n",
      TINYMODEM_VERSION);
}

static void usage() {
  fprintf(stderr,
          "usage: tinymodem [--tx|--rx] [options] {baudmode}\n"
          "		    -t, --tx, --transmit, --write\n"
          "		    -r, --rx, --receive,  --read     (default)\n"
          "		[options]\n"
          "		    -a, --auto-carrier\n"
          "		    -i, --inverted\n"
          "		    -c, --confidence {min-confidence-threshold}\n"
          "		    -l, --limit {max-confidence-search-limit}\n"
          "		    -8, --ascii		ASCII  8-N-1\n"
          "		    -7,			ASCII  7-N-1\n"
          "		    -5, --baudot	Baudot 5-N-1\n"
          "		    -u, --usos {0|1}\n"
          "		    -f, --file {filename.wav}\n"
          "		    -b, --bandwidth {rx_bandwidth}\n"
          "		    -v, --volume {amplitude or 'E'}\n"
          "		    -M, --mark {mark_freq}\n"
          "		    -S, --space {space_freq}\n"
          "		    --startbits {n}\n"
          "		    --stopbits {n.n}\n"
          "		    --invert-start-stop\n"
          "		    --sync-byte {0xXX}\n"
          "		    -q, --quiet\n"
          "		    -R, --samplerate {rate}\n"
          "		    -V, --version\n"
          "		    -A, --alsa[=device]   (uses PortAudio)\n"
          "		    -s, --sndio[=device]  (uses PortAudio)\n"
          "		    --lut={tx_sin_table_len}  (ignored)\n"
          "		    --float-samples           (ignored)\n"
          "		    --rx-one\n"
          "		    --benchmarks\n"
          "		    --binary-output\n"
          "		    --binary-raw {nbits}\n"
          "		    --print-filter\n"
          "		    --print-eot\n"
          "		    --tx-carrier\n"
          "		    --packets             (CRC + Reed-Solomon FEC)\n"
          "		    --kiss                (ax25: KISS frames)\n"
          "		{baudmode}\n"
          "	    any_number_N       Bell-like      N bps --ascii\n"
          "		    1200       Bell202     1200 bps --ascii\n"
          "		     300       Bell103      300 bps --ascii\n"
          "		    rtty       RTTY       45.45 bps --baudot "
          "--stopbits=1.5\n"
          "		     tdd       TTY/TDD    45.45 bps --baudot "
          "--stopbits=2.0\n"
          "		    same       NOAA SAME 520.83 bps --sync-byte=0xAB ...\n"
          "		callerid       Bell202 CID 1200 bps\n"
          "     uic{-train,-ground}       UIC-751-3 Train/Ground 600 bps\n"
          "		    V.21       V.21         300 bps\n"
          "     mfsk{4,8,16,32,64}       Multi-tone FSK 1000-187 bps\n"
          "		aprs, ax25     AX.25 packet radio 1200 bps\n"
          "     psk{31,63,125,250,500}   BPSK with Varicode 31.25-500 bps\n"
          "     ofdm{16,32,64}           OFDM with DQPSK 3200 bps\n"
          "     ggwave[-u,-dt]-{normal,fast,fastest}  ggwave compatible\n"
          "     olivia[-{tones}-{bw}]    Olivia (fldigi), e.g. olivia-8-250\n"
          "     contestia[-{tones}-{bw}] Contestia (fldigi), e.g. contestia-8-250\n"
          "     css{5-10}[-{bw}]         Chirp spread spectrum, e.g. css7, css9-500\n"
          "		    dtmf       DTMF touch tones 0-9 * # A-D\n");
  exit(1);
}

static void rx_stop_sighandler(int) { rx_stop = true; }

/// Updates the RIFF and data chunk sizes of a WAV file
static void fixWavHeader(FILE* file) {
  fflush(file);
  long size = ftell(file);
  uint8_t header[256];
  fseek(file, 0, SEEK_SET);
  size_t n = fread(header, 1, sizeof(header), file);
  for (size_t pos = 12; pos + 8 <= n; pos++) {
    if (memcmp(header + pos, "data", 4) == 0) {
      uint32_t riff_size = size - 8;
      uint32_t data_size = size - (pos + 8);
      fseek(file, 4, SEEK_SET);
      fwrite(&riff_size, 4, 1, file);
      fseek(file, pos + 4, SEEK_SET);
      fwrite(&data_size, 4, 1, file);
      break;
    }
  }
  fseek(file, 0, SEEK_END);
}

/// Packet format of the --packets option
using CliPacketEncoder = PacketEncoder<64, 16>;
using CliPacketDecoder = PacketDecoder<64, 16>;

/// Reads stdin and transmits the characters. Port of fsk_transmit_stdin().
/// The data is written to data_out, which is the encoder or a packet encoder
/// (packets != nullptr) which writes to the encoder.
static void transmitStdin(ModemEncoder& encoder, Print& data_out,
                          CliPacketEncoder* packets, size_t sample_rate,
                          bool tx_interactive, bool tx_carrier,
                          bool print_eot) {
  size_t flush_nsamples = tx_interactive ? sample_rate / 2 : 0;
  // arbitrary chosen timeout value: 1/25 of a second
  unsigned idle_carrier_usec = 1000000 / 25;
  // end the transmission if no data arrives within (a bit more than) a
  // symbol
  float rate = encoder.symbolRate();
  long eot_usec = 1000000 / (rate + rate * 0.03f);
  bool block_input = tx_interactive && !tx_carrier;
  int fd = fileno(stdin);

  auto endTransmission = [&]() {
    if (packets != nullptr) packets->flush();
    if (encoder.endTransmission(flush_nsamples) && print_eot)
      fprintf(stderr, "### EOT\n");
  };
  auto isPending = [&]() {
    return encoder.isTransmitting() ||
           (packets != nullptr && packets->pending() > 0);
  };

  while (true) {
    fd_set fdset;
    FD_ZERO(&fdset);
    FD_SET(fd, &fdset);
    struct timeval tv = {0, 0};
    bool idle = false;
    if (block_input) {
      if (isPending()) {
        // emulate the minimodem SIGALRM end-of-transmission timer
        tv.tv_usec = eot_usec;
        if (select(fd + 1, &fdset, nullptr, nullptr, &tv) == 0) {
          endTransmission();
          continue;
        }
      }
    } else {
      // If !tx_interactive (i.e. writing to an audio file) emit idle tone
      // when stdin blocks for idle_carrier_usec
      if (!tx_interactive) tv.tv_usec = idle_carrier_usec;
      idle = select(fd + 1, &fdset, nullptr, nullptr, &tv) == 0;
    }

    if (idle) {
      // send the buffered packet before the idle tone
      if (packets != nullptr) packets->flush();
      encoder.writeIdle(idle_carrier_usec * sample_rate / 1000000);
      continue;
    }
    uint8_t ch;
    if (read(fd, &ch, 1) <= 0) break;  // EOF or error
    data_out.write(&ch, 1);
  }
  endTransmission();
}

static int benchmarks() {
  printf("tinymodem %s benchmarks\n", TINYMODEM_VERSION);
  fflush(stdout);
  NullPrint null_out;
  FSKEncoder encoder(null_out);
  ModemConfig cfg = encoder.defaultConfig();
  cfg.sample_rate = 48000;
  encoder.begin(cfg);
  auto start = std::chrono::steady_clock::now();
  unsigned nframes = cfg.sample_rate / 10;
  for (unsigned i = 0; i < 10 * 10; i++) {
    encoder.writeTone(1000, nframes / 2);
    encoder.writeTone(1777, nframes / 2);
  }
  std::chrono::duration<double> tx = std::chrono::steady_clock::now() - start;
  printf("generate-tones-S16-mono: 10.0 sec of audio in %.3f sec\n",
         tx.count());

  // decode 10 sec of 1200 baud data
  FSKDecoder decoder(null_out);
  decoder.begin(cfg);
  WriterPrint to_decoder(decoder);
  FSKEncoder modulator(to_decoder);
  modulator.begin(cfg);
  size_t nchars = 10 * 1200 / 10;
  start = std::chrono::steady_clock::now();
  for (size_t j = 0; j < nchars; j++) modulator.write((const uint8_t*)"U", 1);
  modulator.end();
  decoder.end();
  std::chrono::duration<double> rx = std::chrono::steady_clock::now() - start;
  printf("modulate+demodulate-1200-S16-mono: 10.0 sec of audio in %.3f sec\n",
         rx.count());

  // decode 10 sec of mfsk16 data
  MFSKDecoder mfsk_decoder(null_out);
  MFSKConfig mfsk_cfg = mfsk_decoder.defaultConfig();
  mfsk_cfg.sample_rate = 48000;
  mfsk_decoder.begin(mfsk_cfg);
  WriterPrint to_mfsk_decoder(mfsk_decoder);
  MFSKEncoder mfsk_modulator(to_mfsk_decoder);
  mfsk_modulator.begin(mfsk_cfg);
  nchars = 10 * mfsk_cfg.bitRate() / 8;
  start = std::chrono::steady_clock::now();
  for (size_t j = 0; j < nchars; j++)
    mfsk_modulator.write((const uint8_t*)"U", 1);
  mfsk_modulator.end();
  mfsk_decoder.end();
  rx = std::chrono::steady_clock::now() - start;
  printf("modulate+demodulate-mfsk16-S16-mono: 10.0 sec of audio in %.3f sec\n",
         rx.count());
  return 1;
}

int main(int argc, char* argv[]) {
  ModemConfig cfg;
  int TX_mode = -1;
  bool quiet_mode = false;
  bool tx_print_eot = false;
  bool tx_carrier = false;
  bool use_audio_device = false;
  bool use_packets = false;
  bool use_kiss = false;
  char* filename = nullptr;

  program_name = strrchr(argv[0], '/');
  program_name = program_name ? program_name + 1 : argv[0];

  enum {
    MINIMODEM_OPT_UNUSED = 256,  // placeholder
    MINIMODEM_OPT_MSBFIRST,
    MINIMODEM_OPT_STARTBITS,
    MINIMODEM_OPT_STOPBITS,
    MINIMODEM_OPT_INVERT_START_STOP,
    MINIMODEM_OPT_SYNC_BYTE,
    MINIMODEM_OPT_LUT,
    MINIMODEM_OPT_FLOAT_SAMPLES,
    MINIMODEM_OPT_RX_ONE,
    MINIMODEM_OPT_BENCHMARKS,
    MINIMODEM_OPT_BINARY_OUTPUT,
    MINIMODEM_OPT_BINARY_RAW,
    MINIMODEM_OPT_PRINT_FILTER,
    MINIMODEM_OPT_XRXNOISE,
    MINIMODEM_OPT_PRINT_EOT,
    MINIMODEM_OPT_TXCARRIER,
    MINIMODEM_OPT_PACKETS,
    MINIMODEM_OPT_KISS
  };

  static struct option long_options[] = {
      {"version", 0, 0, 'V'},
      {"tx", 0, 0, 't'},
      {"transmit", 0, 0, 't'},
      {"write", 0, 0, 't'},
      {"rx", 0, 0, 'r'},
      {"receive", 0, 0, 'r'},
      {"read", 0, 0, 'r'},
      {"confidence", 1, 0, 'c'},
      {"limit", 1, 0, 'l'},
      {"auto-carrier", 0, 0, 'a'},
      {"inverted", 0, 0, 'i'},
      {"ascii", 0, 0, '8'},
      {"", 0, 0, '7'},
      {"baudot", 0, 0, '5'},
      {"usos", 1, 0, 'u'},
      {"msb-first", 0, 0, MINIMODEM_OPT_MSBFIRST},
      {"file", 1, 0, 'f'},
      {"bandwidth", 1, 0, 'b'},
      {"volume", 1, 0, 'v'},
      {"mark", 1, 0, 'M'},
      {"space", 1, 0, 'S'},
      {"startbits", 1, 0, MINIMODEM_OPT_STARTBITS},
      {"stopbits", 1, 0, MINIMODEM_OPT_STOPBITS},
      {"invert-start-stop", 0, 0, MINIMODEM_OPT_INVERT_START_STOP},
      {"sync-byte", 1, 0, MINIMODEM_OPT_SYNC_BYTE},
      {"quiet", 0, 0, 'q'},
      {"alsa", 2, 0, 'A'},
      {"sndio", 2, 0, 's'},
      {"samplerate", 1, 0, 'R'},
      {"lut", 1, 0, MINIMODEM_OPT_LUT},
      {"float-samples", 0, 0, MINIMODEM_OPT_FLOAT_SAMPLES},
      {"rx-one", 0, 0, MINIMODEM_OPT_RX_ONE},
      {"benchmarks", 0, 0, MINIMODEM_OPT_BENCHMARKS},
      {"binary-output", 0, 0, MINIMODEM_OPT_BINARY_OUTPUT},
      {"binary-raw", 1, 0, MINIMODEM_OPT_BINARY_RAW},
      {"print-filter", 0, 0, MINIMODEM_OPT_PRINT_FILTER},
      {"print-eot", 0, 0, MINIMODEM_OPT_PRINT_EOT},
      {"Xrxnoise", 1, 0, MINIMODEM_OPT_XRXNOISE},
      {"tx-carrier", 0, 0, MINIMODEM_OPT_TXCARRIER},
      {"packets", 0, 0, MINIMODEM_OPT_PACKETS},
      {"kiss", 0, 0, MINIMODEM_OPT_KISS},
      {0, 0, 0, 0}};

  int c;
  int option_index;
  while ((c = getopt_long(argc, argv, "Vtrc:l:ai875u:f:b:v:M:S:T:qs::A::R:",
                          long_options, &option_index)) != -1) {
    switch (c) {
      case 'V':
        version();
        exit(0);
      case 't':
        if (TX_mode == 0) usage();
        TX_mode = 1;
        break;
      case 'r':
        if (TX_mode == 1) usage();
        TX_mode = 0;
        break;
      case 'c':
        cfg.confidence = atof(optarg);
        break;
      case 'l':
        cfg.search_limit = atof(optarg);
        break;
      case 'a':
        cfg.auto_carrier = true;
        break;
      case 'i':
        cfg.inverted = true;
        break;
      case 'f':
        filename = optarg;
        break;
      case '8':
        cfg.data_bits = 8;
        break;
      case '7':
        cfg.data_bits = 7;
        break;
      case '5':
        cfg.data_bits = 5;
        cfg.format = DataFormat::BAUDOT;
        break;
      case 'u':
        cfg.baudot_usos = atoi(optarg) != 0;
        break;
      case MINIMODEM_OPT_MSBFIRST:
        cfg.msb_first = true;
        break;
      case 'b':
        cfg.bandwidth = atof(optarg);
        if (cfg.bandwidth == 0) usage();
        break;
      case 'v':
        cfg.volume = optarg[0] == 'E' ? FLT_EPSILON : atof(optarg);
        if (cfg.volume <= 0.0f) usage();
        break;
      case 'M':
        cfg.mark_freq = atof(optarg);
        if (cfg.mark_freq <= 0) usage();
        break;
      case 'S':
        cfg.space_freq = atof(optarg);
        if (cfg.space_freq <= 0) usage();
        break;
      case MINIMODEM_OPT_STARTBITS:
        cfg.start_bits = atoi(optarg);
        if (cfg.start_bits < 0 || cfg.start_bits > 20) usage();
        break;
      case MINIMODEM_OPT_STOPBITS:
        cfg.stop_bits = atof(optarg);
        if (cfg.stop_bits < 0) usage();
        break;
      case MINIMODEM_OPT_INVERT_START_STOP:
        cfg.invert_start_stop = true;
        break;
      case MINIMODEM_OPT_SYNC_BYTE:
        cfg.rx_sync = true;
        cfg.tx_sync_bytes = 16;
        cfg.sync_byte = strtol(optarg, nullptr, 0);
        break;
      case 'q':
        quiet_mode = true;
        break;
      case 'A':
      case 's':
        use_audio_device = true;
        break;
      case 'R':
        cfg.sample_rate = atoi(optarg);
        if (cfg.sample_rate == 0) usage();
        break;
      case MINIMODEM_OPT_LUT:
      case MINIMODEM_OPT_FLOAT_SAMPLES:
        // accepted for compatibility
        break;
      case MINIMODEM_OPT_RX_ONE:
        cfg.rx_one = true;
        break;
      case MINIMODEM_OPT_BENCHMARKS:
        benchmarks();
        exit(0);
      case MINIMODEM_OPT_BINARY_OUTPUT:
        cfg.binary_output = true;
        break;
      case MINIMODEM_OPT_BINARY_RAW:
        cfg.binary_raw_bits = atoi(optarg);
        break;
      case MINIMODEM_OPT_PRINT_FILTER:
        cfg.print_filter = true;
        break;
      case MINIMODEM_OPT_XRXNOISE:
        cfg.rx_noise = atof(optarg);
        break;
      case MINIMODEM_OPT_TXCARRIER:
        tx_carrier = true;
        break;
      case MINIMODEM_OPT_PRINT_EOT:
        tx_print_eot = true;
        break;
      case MINIMODEM_OPT_PACKETS:
        use_packets = true;
        break;
      case MINIMODEM_OPT_KISS:
        use_kiss = true;
        break;
      default:
        usage();
    }
  }
  if (TX_mode == -1) TX_mode = 0;
  if (optind >= argc) usage();

  const char* modem_mode = argv[optind++];
  bool mfsk = MFSKConfig::isMode(modem_mode);
  bool ax25 = AX25Config::isMode(modem_mode);
  bool psk = PSKConfig::isMode(modem_mode);
  bool ofdm = OFDMConfig::isMode(modem_mode);
  bool ggwave = GGWaveConfig::isMode(modem_mode);
  bool dtmf = DTMFConfig::isMode(modem_mode);
  bool olivia = OliviaConfig::isMode(modem_mode);
  bool css = CSSConfig::isMode(modem_mode);
  MFSKConfig mfsk_cfg;
  AX25Config ax25_cfg;
  PSKConfig psk_cfg;
  OFDMConfig ofdm_cfg;
  GGWaveConfig ggwave_cfg;
  DTMFConfig dtmf_cfg;
  OliviaConfig olivia_cfg;
  CSSConfig css_cfg;
  if (css) {
    css_cfg.setMode(modem_mode);
    css_cfg.sample_rate = cfg.sample_rate;
    css_cfg.volume = cfg.volume;
    css_cfg.rx_one = cfg.rx_one;
    css_cfg.rx_noise = cfg.rx_noise;
    if (cfg.mark_freq > 0) css_cfg.center_freq = cfg.mark_freq;
    if (tx_carrier) {
      fprintf(stderr, "E: --tx-carrier is not supported by %s.\n", modem_mode);
      return 1;
    }
    if (!css_cfg.isValid()) {
      fprintf(stderr, "E: invalid %s configuration\n", modem_mode);
      return 1;
    }
  } else if (olivia) {
    olivia_cfg.setMode(modem_mode);
    olivia_cfg.sample_rate = cfg.sample_rate;
    olivia_cfg.volume = cfg.volume;
    olivia_cfg.rx_one = cfg.rx_one;
    olivia_cfg.rx_noise = cfg.rx_noise;
    if (cfg.mark_freq > 0) olivia_cfg.center_freq = cfg.mark_freq;
    if (tx_carrier || use_packets) {
      fprintf(stderr, "E: --tx-carrier and --packets are not supported by %s.\n",
              modem_mode);
      return 1;
    }
    if (!olivia_cfg.isValid()) {
      fprintf(stderr, "E: invalid %s configuration\n", modem_mode);
      return 1;
    }
  } else if (dtmf) {
    dtmf_cfg.sample_rate = cfg.sample_rate;
    dtmf_cfg.volume = cfg.volume;
    dtmf_cfg.rx_one = cfg.rx_one;
    dtmf_cfg.rx_noise = cfg.rx_noise;
    if (tx_carrier || use_packets) {
      fprintf(stderr, "E: --tx-carrier and --packets are not supported by %s.\n",
              modem_mode);
      return 1;
    }
    if (!dtmf_cfg.isValid()) {
      fprintf(stderr, "E: invalid %s configuration\n", modem_mode);
      return 1;
    }
  } else if (ggwave) {
    ggwave_cfg.setMode(modem_mode);
    ggwave_cfg.sample_rate = cfg.sample_rate;
    ggwave_cfg.volume = cfg.volume;
    ggwave_cfg.rx_one = cfg.rx_one;
    ggwave_cfg.rx_noise = cfg.rx_noise;
    if (tx_carrier) {
      fprintf(stderr, "E: --tx-carrier is not supported by %s.\n", modem_mode);
      return 1;
    }
    if (!ggwave_cfg.isValid()) {
      fprintf(stderr, "E: invalid %s configuration\n", modem_mode);
      return 1;
    }
  } else if (ofdm) {
    ofdm_cfg.setMode(modem_mode);
    ofdm_cfg.sample_rate = cfg.sample_rate;
    ofdm_cfg.volume = cfg.volume;
    ofdm_cfg.rx_one = cfg.rx_one;
    ofdm_cfg.rx_noise = cfg.rx_noise;
    if (cfg.mark_freq > 0) ofdm_cfg.first_freq = cfg.mark_freq;
    if (tx_carrier) {
      fprintf(stderr, "E: --tx-carrier is not supported by %s.\n", modem_mode);
      return 1;
    }
    if (!ofdm_cfg.isValid()) {
      fprintf(stderr, "E: invalid %s configuration\n", modem_mode);
      return 1;
    }
  } else if (psk) {
    psk_cfg.setMode(modem_mode);
    psk_cfg.sample_rate = cfg.sample_rate;
    psk_cfg.volume = cfg.volume;
    psk_cfg.rx_one = cfg.rx_one;
    psk_cfg.rx_noise = cfg.rx_noise;
    if (cfg.mark_freq > 0) psk_cfg.carrier_freq = cfg.mark_freq;
    if (!psk_cfg.isValid()) {
      fprintf(stderr, "E: invalid %s configuration\n", modem_mode);
      return 1;
    }
  } else if (ax25) {
    ax25_cfg.setMode(modem_mode);
    ax25_cfg.sample_rate = cfg.sample_rate;
    ax25_cfg.volume = cfg.volume;
    ax25_cfg.rx_noise = cfg.rx_noise;
    if (cfg.mark_freq > 0) ax25_cfg.mark_freq = cfg.mark_freq;
    if (cfg.space_freq > 0) ax25_cfg.space_freq = cfg.space_freq;
    ax25_cfg.format = use_kiss ? AX25Format::KISS : AX25Format::TEXT;
    if (use_packets) {
      fprintf(stderr, "E: --packets is not supported by %s.\n", modem_mode);
      return 1;
    }
    if (!ax25_cfg.isValid()) {
      fprintf(stderr, "E: invalid %s configuration\n", modem_mode);
      return 1;
    }
  } else if (mfsk) {
    if (!mfsk_cfg.setMode(modem_mode)) usage();
    // options which are supported by the MFSK modem
    mfsk_cfg.sample_rate = cfg.sample_rate;
    mfsk_cfg.volume = cfg.volume;
    mfsk_cfg.rx_one = cfg.rx_one;
    mfsk_cfg.rx_noise = cfg.rx_noise;
    if (cfg.mark_freq > 0) mfsk_cfg.base_freq = cfg.mark_freq;
    if (tx_carrier) {
      fprintf(stderr, "E: --tx-carrier is not supported by %s.\n",
              modem_mode);
      return 1;
    }
    if (!mfsk_cfg.isValid()) {
      fprintf(stderr, "E: invalid %s configuration\n", modem_mode);
      return 1;
    }
  } else {
    // the options (e.g. -5, --stopbits) override the mode defaults
    if (!cfg.setMode(modem_mode, true)) usage();
    if (TX_mode && !cfg.isTxSupported()) {
      fprintf(stderr, "E: %s --tx mode is not supported.\n", modem_mode);
      return 1;
    }
    if (cfg.format == DataFormat::CALLERID && cfg.auto_carrier)
      fprintf(stderr, "W: callerid with --auto-carrier is not recommended.\n");
    ModemConfig check = cfg;
    if (!check.resolve()) {
      fprintf(stderr, "E: total number of bits per frame must be <= 64.\n");
      return 1;
    }
  }
  AudioInfo audio_info = mfsk   ? (AudioInfo)mfsk_cfg
                         : ax25 ? (AudioInfo)ax25_cfg
                         : psk  ? (AudioInfo)psk_cfg
                         : ofdm ? (AudioInfo)ofdm_cfg
                         : ggwave ? (AudioInfo)ggwave_cfg
                         : dtmf ? (AudioInfo)dtmf_cfg
                         : olivia ? (AudioInfo)olivia_cfg
                         : css ? (AudioInfo)css_cfg
                                : (AudioInfo)cfg;
  if (filename == nullptr) use_audio_device = true;

#ifndef TINYMODEM_PORTAUDIO
  if (use_audio_device) {
    fprintf(stderr,
            "E: no audio device support (built without PortAudio): "
            "use --file\n");
    return 1;
  }
#endif

  /*
   * Transmit
   */
  if (TX_mode) {
    FSKEncoder fsk_encoder;
    MFSKEncoder mfsk_encoder;
    AX25Encoder ax25_encoder;
    PSKEncoder psk_encoder;
    OFDMEncoder ofdm_encoder;
    GGWaveEncoder ggwave_encoder;
    DTMFEncoder dtmf_encoder;
    OliviaEncoder olivia_encoder;
    CSSEncoder css_encoder;
    ModemEncoder& encoder = mfsk     ? (ModemEncoder&)mfsk_encoder
                            : ax25   ? (ModemEncoder&)ax25_encoder
                            : psk    ? (ModemEncoder&)psk_encoder
                            : ofdm   ? (ModemEncoder&)ofdm_encoder
                            : ggwave ? (ModemEncoder&)ggwave_encoder
                            : dtmf   ? (ModemEncoder&)dtmf_encoder
                            : olivia ? (ModemEncoder&)olivia_encoder
                            : css    ? (ModemEncoder&)css_encoder
                                     : (ModemEncoder&)fsk_encoder;
    auto beginEncoder = [&]() {
      bool ok = mfsk   ? mfsk_encoder.begin(mfsk_cfg)
                : ax25 ? ax25_encoder.begin(ax25_cfg)
                : psk  ? psk_encoder.begin(psk_cfg)
                : ofdm ? ofdm_encoder.begin(ofdm_cfg)
                : ggwave ? ggwave_encoder.begin(ggwave_cfg)
                : dtmf ? dtmf_encoder.begin(dtmf_cfg)
                : olivia ? olivia_encoder.begin(olivia_cfg)
                : css ? css_encoder.begin(css_cfg)
                       : fsk_encoder.begin(cfg);
      if (!ok) fprintf(stderr, "E: invalid configuration\n");
      return ok;
    };
    // the data is written to the encoder or to the packet encoder
    WriterPrint to_encoder(encoder);
    CliPacketEncoder packets(to_encoder);
    Print& data_out = use_packets ? (Print&)packets : (Print&)to_encoder;
    CliPacketEncoder* p_packets = use_packets ? &packets : nullptr;

    if (filename) {
      FILE* file = fopen(filename, "w+b");
      if (file == nullptr) {
        perror(filename);
        return 1;
      }
      FilePrint file_out(file);
      WAVEncoder wav;
      EncodedAudioOutput wav_out(&file_out, &wav);
      wav_out.begin(audio_info);
      encoder.setOutput(wav_out);
      if (!beginEncoder()) return 1;
      transmitStdin(encoder, data_out, p_packets, audio_info.sample_rate,
                    false, tx_carrier, tx_print_eot);
      encoder.end();
      wav_out.end();
      fixWavHeader(file);
      fclose(file);
      return 0;
    }
#ifdef TINYMODEM_PORTAUDIO
    PortAudioStream audio;
    auto pa_cfg = audio.defaultConfig(TX_MODE);
    pa_cfg.copyFrom(audio_info);
    if (!audio.begin(pa_cfg)) {
      fprintf(stderr, "E: could not open the audio output\n");
      return 1;
    }
    encoder.setOutput(audio);
    if (!beginEncoder()) return 1;
    transmitStdin(encoder, data_out, p_packets, audio_info.sample_rate, true,
                  tx_carrier, tx_print_eot);
    encoder.end();
    audio.end();
#endif
    return 0;
  }

  /*
   * Receive
   */
  FSKDecoder fsk_decoder;
  MFSKDecoder mfsk_decoder;
  AX25Decoder ax25_decoder;
  PSKDecoder psk_decoder;
  OFDMDecoder ofdm_decoder;
  GGWaveDecoder ggwave_decoder;
  DTMFDecoder dtmf_decoder;
  OliviaDecoder olivia_decoder;
  CSSDecoder css_decoder;
  ModemDecoder& decoder = mfsk     ? (ModemDecoder&)mfsk_decoder
                          : ax25   ? (ModemDecoder&)ax25_decoder
                          : psk    ? (ModemDecoder&)psk_decoder
                          : ofdm   ? (ModemDecoder&)ofdm_decoder
                          : ggwave ? (ModemDecoder&)ggwave_decoder
                          : dtmf   ? (ModemDecoder&)dtmf_decoder
                          : olivia ? (ModemDecoder&)olivia_decoder
                          : css    ? (ModemDecoder&)css_decoder
                                   : (ModemDecoder&)fsk_decoder;
  // the decoded data is written to stdout or to the packet decoder
  CliPacketDecoder packets(out_stdout);
  decoder.setOutput(use_packets ? (Print&)packets : (Print&)out_stdout);
  decoder.setStatusOutput(out_stderr);
  decoder.setQuiet(quiet_mode);
  auto beginDecoder = [&]() {
    bool ok = mfsk   ? mfsk_decoder.begin(mfsk_cfg)
              : ax25 ? ax25_decoder.begin(ax25_cfg)
              : psk  ? psk_decoder.begin(psk_cfg)
              : ofdm ? ofdm_decoder.begin(ofdm_cfg)
              : ggwave ? ggwave_decoder.begin(ggwave_cfg)
              : dtmf ? dtmf_decoder.begin(dtmf_cfg)
              : olivia ? olivia_decoder.begin(olivia_cfg)
              : css ? css_decoder.begin(css_cfg)
                     : fsk_decoder.begin(cfg);
    if (!ok) fprintf(stderr, "E: invalid configuration\n");
    return ok;
  };
  auto reportPackets = [&]() {
    if (!use_packets || quiet_mode) return;
    const PacketStatistics& stats = packets.statistics();
    fprintf(stderr,
            "### PACKETS received=%u corrected=%u corrected_bytes=%u "
            "dropped=%u ###\n",
            stats.received, stats.corrected, stats.corrected_bytes,
            stats.dropped);
  };
  signal(SIGINT, rx_stop_sighandler);
  uint8_t buffer[1024];

  if (filename) {
    FILE* file = fopen(filename, "rb");
    if (file == nullptr) {
      perror(filename);
      return 1;
    }
    WriterPrint to_decoder(decoder);
    WAVDecoder wav;
    wav.setConvert8Bit(true);
    wav.setConvert24Bit(true);
    wav.setOutput(to_decoder);
    wav.addNotifyAudioChange(decoder);
    wav.begin();
    if (!beginDecoder()) return 1;
    size_t n;
    while (!rx_stop && !decoder.isFinished() &&
           (n = fread(buffer, 1, sizeof(buffer), file)) > 0) {
      wav.write(buffer, n);
    }
    // The WAVDecoder converts 24 bit and float samples in batches of 256
    // samples and end() drops an incomplete batch: we push it out with
    // silence. Silence at the end does not change the decoding result.
    memset(buffer, 0, sizeof(buffer));
    for (int j = 0; j < 2; j++) wav.write(buffer, sizeof(buffer));
    decoder.end();
    fclose(file);
    reportPackets();
    return 0;
  }

#ifdef TINYMODEM_PORTAUDIO
  PortAudioStream audio;
  auto pa_cfg = audio.defaultConfig(RX_MODE);
  pa_cfg.copyFrom(audio_info);
  if (!audio.begin(pa_cfg)) {
    fprintf(stderr, "E: could not open the audio input\n");
    return 1;
  }
  if (!beginDecoder()) return 1;
  while (!rx_stop && !decoder.isFinished()) {
    size_t n = audio.readBytes(buffer, sizeof(buffer));
    decoder.write(buffer, n);
  }
  decoder.end();
  audio.end();
  reportPackets();
#endif
  return 0;
}
