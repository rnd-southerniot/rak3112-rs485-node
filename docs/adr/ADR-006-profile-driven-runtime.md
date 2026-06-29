# ADR-006 — Profile-driven runtime (generic Modbus reader + scan-for-ID)

- **Status:** Proposed (increment 1 — pure core — implemented; rest gated below)
- **Date:** 2026-06-24
- **Relates to:** `device-profiles/` (the profile library), ADR-005 (payload schema),
  `docs/PROVISIONING_API_CONTRACT.md` (NVS plane), `docs/CRM_FIRMWARE_AGENT_GUIDE.md`

## Context

Today each field device is a hand-written matched set in firmware (`meter_read_<dev>` +
`payload_encode_<dev>` + a compiled device-type byte). Adding a device = a firmware PR. The CRM now
serves machine-readable **device profiles** (`device-profiles/`) carrying the full register map +
scaling + ADR-005 payload mapping, minus the slave ID. To let the CRM provision **any** profiled
device with **no firmware rebuild**, the firmware must consume a profile at runtime.

## Decision

Make the field path **data-driven**: a `device_profile_t` (bus params + measurand table + payload
table) drives one **generic Modbus reader** and one **generic ADR-005 encoder**. The profile is
stored in NVS, written by the CRM (extends the Plane-B provisioning console), and the **slave ID is
auto-discovered by a bus scan at commissioning** (the profile omits it). One firmware image serves
all profiled devices.

The existing compiled readers (`meter_*.c`) stay as (a) the proven oracle for host tests and (b) a
compile-time fallback; they are not the growth path.

## Architecture

```
CRM profile (JSON)  --provision-->  NVS blob  --load-->  device_profile_t
                                                              │
   scan-for-ID (modbus_master_scan) ── unit ID ──────────────┤
                                                              ▼
   generic reader: for each measurand -> modbus_master_read -> dp_decode() -> values[]
                                                              ▼
   dp_encode_payload(profile, values, flags) -> ADR-005 frame -> lora_send
```

Numeric model (the pure core, this increment):
- `dp_decode(regs, type, word, scale, offset)` — decode `u16/i16/u32/i32/float32` with word order
  `ABCD/CDAB/BADC/DCBA` → engineering float. Generalises `modbus_regs_to_f32`.
- `dp_encode_payload(profile, values[], flags, out)` — 3-byte ADR-005 header + walk the payload
  field table (per-field `u8/i8/u16/i16/u32/i32` + scale, saturating, big-endian).

## Increment plan (gated)

| # | Scope | Gate |
|---|---|---|
| **1** | **Pure core** `components/device_profile` (`dp_decode`/`dp_encode_payload`) + host tests | **this PR** — host ctest green; KATs per type/word-order + a full-frame encode matches the Phase 6c MFM384 bytes |
| 2 | NVS profile blob (serialize/deserialize, versioned) + `prov-profile` console cmd + `provision_nvs.py`/CRM writer | flash + `prov-show` reflects a written profile; survives reboot |
| 3 | On-target generic reader + app wiring behind `CONFIG_APP_FIELD_PROFILE_DRIVEN` | sim + real read of MFM384 via profile == compiled path, byte-identical uplink |
| 4 | Auto-scan-for-ID at commission (reuse `modbus_master_scan`; persist discovered unit) | node finds the slave ID with no provisioned `unit`, then uplinks |
| 5 | Bench-validate all 4 profiles (MFM384/RS-FSJT real where available; EEM400/DSE via sim/real) + soak | each profile decodes sane in ChirpStack; budgets green |

## Consequences

- **+** New device or client = profile data, no firmware rebuild; EEM400/DSE become flashable without
  porting C. One image to test/OTA/certify.
- **−** A runtime interpreter (bounded: DSE's 21 measurands is the worst case) + an NVS blob (versioned;
  size bounded ~ a few hundred bytes). Slightly larger `.text` and per-sample cost than compiled — fine
  off the hot path.
- Schema/firmware coupling handled by a `profile_blob_version` byte; firmware rejects unknown versions.

## Risks / mitigations

- **Wrong map bricks a silent read** → the profile is validated (`validate_profiles.py`) before the CRM
  serves it; the node logs decoded values on first uplink for operator sanity-check.
- **NVS blob corruption** → versioned + CRC; fall back to the compiled default + STALE flag.
- **Bus-scan picks the wrong unit on a shared bus** → scan match rule + optional CRM-pinned unit override.
