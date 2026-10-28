#pragma once
/**
 * @file TinyModemRTOS.h
 * @brief TinyModem.h plus the stream classes which receive in a separate
 * task (ModemStreamRTOS, MFSKStreamRTOS, PSKStreamRTOS, OFDMStreamRTOS,
 * AX25StreamRTOS). Supported on ESP32, on RP2040 with FreeRTOS (select Tools >
 * Operating System > FreeRTOS SMP and include <FreeRTOS.h> before
 * AudioTools.h) and on the desktop.
 */
#include "TinyModem.h"
#include "modem/ModemStreamRTOS.h"
