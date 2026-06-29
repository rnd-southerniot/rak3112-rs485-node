/* SPDX-License-Identifier: MIT */
/* ADR-005: compact LoRaWAN payload schema for rak3112-rs485-node.
 * Header: [schema=0x01][device-type][flags]  (3 bytes).
 * Body: fixed-point, big-endian, device-specific.
 * Max frame: 53 bytes (AS923 DR3). */
#pragma once

#include <stdint.h>

#define TELEMETRY_SCHEMA_VERSION  UINT8_C(0x01)
#define TELEMETRY_HEADER_LEN      UINT8_C(3)

/* Device-type bytes (byte 1 of header). */
#define TELEMETRY_DEV_MFM384      UINT8_C(0x02)
#define TELEMETRY_DEV_EEM400      UINT8_C(0x03)  /* Honeywell EEM400C-D-MO */
#define TELEMETRY_DEV_DEEPSEA     UINT8_C(0x04)  /* Deep Sea Electronics    */

/* Flags (byte 2 of header, bitmask). */
#define TELEMETRY_FLAG_SIMULATED  UINT8_C(0x01)
#define TELEMETRY_FLAG_STALE      UINT8_C(0x02)
