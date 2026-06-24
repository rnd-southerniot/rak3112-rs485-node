/*
 * payload.h — compact versioned binary encoder for LoRaWAN uplinks (ADR-005). Pure C, host-tested.
 *
 * Wire format (big-endian), common 3-byte header then device-specific body:
 *   [0] schema version (TELEMETRY_SCHEMA_VERSION)
 *   [1] device type    (telemetry_device_t)
 *   [2] flags          (TELEMETRY_FLAG_*)
 *   [3..] device body, fixed-point scaled integers (see ADR-005 / docs).
 * The matching ChirpStack decoder lives in tools/chirpstack_mfm384_decoder.js.
 */
#ifndef PAYLOAD_H
#define PAYLOAD_H

#include <stddef.h>
#include <stdint.h>

#include "telemetry.h"

/* Largest payload this module emits (MFM384 = 19 B). Stays well under the AS923 DR3 53-byte cap. */
#define PAYLOAD_MAX 24u

/*
 * Encode an MFM384 sample (19 bytes). Body: V1N,V2N,V3N (×10, u16), Total kW (×10, i16),
 * Total kWh (×100, u32), Freq (×100, u16), Avg PF (×1000, i16). Out-of-range values saturate to
 * the field's limits. Returns bytes written, or 0 on NULL args / cap < 19.
 */
size_t payload_encode_mfm384(const mfm384_sample_t *s, uint8_t flags, uint8_t *out, size_t cap);

/* Encode an RS-FSJT sample (5 bytes). Body: wind (×100, u16). Returns bytes written, or 0. */
size_t payload_encode_rsfsjt(const rsfsjt_sample_t *s, uint8_t flags, uint8_t *out, size_t cap);

#endif /* PAYLOAD_H */
