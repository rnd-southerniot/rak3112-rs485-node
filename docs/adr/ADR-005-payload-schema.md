# ADR-005 — LoRaWAN uplink payload schema (Phase 6c)

- **Status:** Proposed (bench-pending — to be validated against ChirpStack in the home lab)
- **Date:** 2026-06-24
- **Phase:** 6c (Modbus field protocol → LoRaWAN uplink)
- **Closes:** the Phase 6 "ADR-005 = payload schema" deliverable
- **Relates to:** ADR-003 (RadioLib stack), ADR-004 (AS923-1 sub-band), Phase 6 MFM384 bring-up

## Context

The node samples a Modbus field device and uplinks selected measurands over LoRaWAN AS923-1
(Class A, OTAA — Phase 5). Two device types are in scope:

- **SELEC MFM384** 3-phase multifunction meter — FC04 input registers, float32 **CDAB** word order,
  9600 8N1, unit 1 (verified live on hardware 2026-06-24; see RUNBOOK Phase 6 Attempt 4).
- **RS-FSJT-N01** wind sensor — FC03 holding reg 0, uint16, value/10 = m/s, 4800 8N1.

Constraints: AS923-1 with dwell time caps the application payload at **53 bytes at DR3 (SF9)**
(Phase 5 operating point); smaller is better for airtime/fair-use. Raw float32 per parameter (4 B)
is wasteful and not needed at the meter's display resolution.

## Decision

A **compact, versioned, big-endian binary** payload. A 3-byte common header identifies schema and
device; the body is device-specific **fixed-point scaled integers** sized to each field's real range
and resolution. A single ChirpStack JS codec (`tools/chirpstack_mfm384_decoder.js`) decodes both
device types by branching on the device byte.

### Common header (3 bytes)

| Offset | Field | Type | Notes |
|---|---|---|---|
| 0 | schema version | u8 | `TELEMETRY_SCHEMA_VERSION` = 1. Bump on ANY layout/scaling change. |
| 1 | device type | u8 | 0x01 = MFM384, 0x02 = RS-FSJT-N01 |
| 2 | flags | u8 | bit0 = simulated data, bit1 = stale (last read failed) |

### MFM384 body (16 bytes → 19 total)

| Offset | Field | Type | Scale | Range | Unit |
|---|---|---|---|---|---|
| 3 | V1N | u16 | ×10 | 0–6553.5 | V |
| 5 | V2N | u16 | ×10 | 0–6553.5 | V |
| 7 | V3N | u16 | ×10 | 0–6553.5 | V |
| 9 | Total kW | i16 | ×10 | ±3276.7 | kW (signed: −export) |
| 11 | Total kWh | u32 | ×100 | 0–42,949,672.95 | kWh |
| 15 | Frequency | u16 | ×100 | 0–655.35 | Hz |
| 17 | Avg PF | i16 | ×1000 | −1.000…+1.000 | — |

### RS-FSJT-N01 body (2 bytes → 5 total)

| Offset | Field | Type | Scale | Range | Unit |
|---|---|---|---|---|---|
| 3 | wind speed | u16 | ×100 | 0–655.35 | m/s |

Out-of-range values **saturate** to the field limits (never wrap); negative inputs to unsigned
fields clamp to 0. Encoder: `firmware/components/payload/payload.c` (pure C, host-tested in
`tests/host/test_payload.c` — exact-byte + signed + saturation cases).

## Rationale

- **Versioned** so the decoder can evolve without silent misparses; the CRM rejects unknown versions.
- **Device byte** lets one node/codec serve both the meter and the wind sensor (and future devices)
  without separate fPorts or profiles.
- **Fixed-point** matches the meter's display resolution (0.1 V, 0.01 Hz, 0.001 PF) at ~⅓ the bytes
  of raw float32; the full MFM384 payload is 19 B, comfortably inside the DR3 budget.
- **Simulated/stale flags** make bench/sim uplinks unambiguous in the CRM and prevent a failed read
  from masquerading as real zeros.
- **Subset, not the whole register map:** the full MFM384 map (V/I per phase, per-phase kW/kVAr/PF,
  etc.) lives in `telemetry.h`; the uplink carries the operationally useful subset. Expanding the
  payload is a version bump + decoder update, not a redesign.

## Consequences / open items

- Field set is the Phase-6c default (3 phase voltages + total kW + total kWh + freq + avg PF). If the
  deployment needs per-phase current/PF, bump `TELEMETRY_SCHEMA_VERSION` and extend both ends.
- **Validation pending (home lab):** build with `sdkconfig.defaults.sim-mfm384`, join the CRM
  ChirpStack (`10.10.8.140`), and confirm the decoder renders sane fields (~230 V, ~50 Hz, kWh
  climbing) with the `simulated` flag set. Then repeat real with the meter, and the RS-FSJT real read.
- RS-FSJT scale is taken as raw/10 (MODBUS_MAP.md); a /100 variant exists in one reference — confirm
  against the physical sensor on the home bench and, if different, fix `meter_read_rsfsjt` (firmware
  side only; the wire payload scale ×100 is independent and stays).
