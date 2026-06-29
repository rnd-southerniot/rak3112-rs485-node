/*
 * telemetry.h — device sample structs, SELEC MFM384 register map, and payload schema constants.
 *
 * Pure C, no SDK dependency, so the payload encoder (payload.c) builds host-side under ctest.
 * The MFM384 map is the ETS-proven map (southern-rnd/ETS-LORA-Test, same physical meter): FC04
 * INPUT registers, float32, CDAB word order, 9600 8N1, unit 1 — verified live on hardware
 * 2026-06-24 (V1N @ reg 0 read ~50.5 V on the bench source). Each parameter spans 2 registers.
 */
#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdint.h>

/* Device type — payload byte 1. */
typedef enum {
    TELEMETRY_DEV_MFM384 = 0x01,
    TELEMETRY_DEV_RSFSJT = 0x02,
} telemetry_device_t;

/* Payload schema version — payload byte 0. Bump on ANY field/layout/scaling change. */
#define TELEMETRY_SCHEMA_VERSION 0x01u

/* Common header length: [schema][device-type][flags]. */
#define TELEMETRY_HEADER_LEN 3u

/* Flags — payload byte 2 (bitfield). */
#define TELEMETRY_FLAG_SIMULATED 0x01u /* values synthesized, not read from a real device */
#define TELEMETRY_FLAG_STALE 0x02u     /* last Modbus read failed; values may be stale/zero */

/*
 * SELEC MFM384 register map — FC04 input registers, float32 (CDAB). Wire addresses.
 * 40001/30001 notation maps to wire 0. ETS-proven subset (intermediate addresses follow the same
 * +2 stride but are only listed here where confirmed against the same meter).
 */
#define MFM384_REG_V1N 0u
#define MFM384_REG_V2N 2u
#define MFM384_REG_V3N 4u
#define MFM384_REG_AVG_VLN 6u
#define MFM384_REG_V12 8u
#define MFM384_REG_V23 10u
#define MFM384_REG_V31 12u
#define MFM384_REG_AVG_VLL 14u
#define MFM384_REG_I1 16u
#define MFM384_REG_I3 20u
#define MFM384_REG_AVG_I 22u
#define MFM384_REG_KW_PH1 24u
#define MFM384_REG_KW_PH2 26u
#define MFM384_REG_KW_PH3 28u
#define MFM384_REG_KVAR_PH3 42u
#define MFM384_REG_TOTAL_KW 44u
#define MFM384_REG_TOTAL_KVAR 46u
#define MFM384_REG_PF1 48u
#define MFM384_REG_PF2 50u
#define MFM384_REG_PF3 52u
#define MFM384_REG_AVG_PF 54u
#define MFM384_REG_FREQ 56u
#define MFM384_REG_TOTAL_KWH 58u

/* RS-FSJT-N01 wind sensor — FC03 holding reg 40001 (wire 0), uint16, value/10 = m/s, 4800 8N1. */
#define RSFSJT_REG_WIND 0u

/* The MFM384 measurands carried in the uplink (subset of the full map, all ETS-proven addresses).
 */
typedef struct {
    float v1n;       /* phase-1 line-neutral voltage (V) */
    float v2n;       /* phase-2 (V) */
    float v3n;       /* phase-3 (V) */
    float total_kw;  /* total active power (kW), signed (negative = export) */
    float total_kwh; /* total active energy (kWh), accumulating */
    float freq;      /* frequency (Hz) */
    float avg_pf;    /* average power factor (-1..+1) */
} mfm384_sample_t;

typedef struct {
    float wind_mps; /* wind speed (m/s) */
} rsfsjt_sample_t;

#endif /* TELEMETRY_H */
