#pragma once
/**
 * @file TinyModem.h
 * @brief Header only software audio modem library based on the
 * arduino-audio-tools: FSK (compatible with minimodem), multi-tone FSK, PSK,
 * OFDM, AX.25/APRS, ggwave, Olivia/Contestia, chirp spread spectrum, DTMF and packets with error correction. Include
 * "AudioTools.h" before this file.
 */
#include "AudioTools.h"
#include "modem/AX25.h"
#include "modem/AX25Decoder.h"
#include "modem/AX25Encoder.h"
#include "modem/CSS.h"
#include "modem/CSSDecoder.h"
#include "modem/CSSEncoder.h"
#include "modem/DataCodecs.h"
#include "modem/DSPUtils.h"
#include "modem/DTMF.h"
#include "modem/DTMFDecoder.h"
#include "modem/DTMFEncoder.h"
#include "modem/FSKDecoder.h"
#include "modem/FSKDemodulator.h"
#include "modem/FSKEncoder.h"
#include "modem/GGWave.h"
#include "modem/GGWaveDecoder.h"
#include "modem/GGWaveEncoder.h"
#include "modem/MFSKConfig.h"
#include "modem/OFDM.h"
#include "modem/Olivia.h"
#include "modem/OliviaDecoder.h"
#include "modem/OliviaEncoder.h"
#include "modem/OFDMDecoder.h"
#include "modem/OFDMEncoder.h"
#include "modem/PSK.h"
#include "modem/PSKDecoder.h"
#include "modem/PSKEncoder.h"
#include "modem/MFSKDecoder.h"
#include "modem/MFSKEncoder.h"
#include "modem/ModemStream.h"
#include "modem/ModemBase.h"
#include "modem/ModemConfig.h"
#include "modem/PacketStream.h"

#define TINYMODEM_VERSION "1.0.0"

#ifndef TINYMODEM_NO_NAMESPACE
using namespace tinymodem;
#endif
