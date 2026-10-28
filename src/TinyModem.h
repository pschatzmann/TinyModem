#pragma once
/**
 * @file TinyModem.h
 * @brief Header only software audio modem library based on the
 * arduino-audio-tools: FSK (compatible with minimodem), multi-tone FSK, PSK,
 * OFDM, AX.25/APRS and packets with error correction. Include
 * "AudioTools.h" before this file.
 */
#include "AudioTools.h"
#include "modem/AX25.h"
#include "modem/AX25Decoder.h"
#include "modem/AX25Encoder.h"
#include "modem/DataCodecs.h"
#include "modem/FSKDecoder.h"
#include "modem/FSKDemodulator.h"
#include "modem/FSKEncoder.h"
#include "modem/MFSKConfig.h"
#include "modem/OFDM.h"
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
