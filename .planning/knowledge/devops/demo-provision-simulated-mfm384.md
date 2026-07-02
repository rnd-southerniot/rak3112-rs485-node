# Demo: provision + simulated MFM384 → ChirpStack decode (no hardware)

**Category:** devops
**Tags:** chirpstack, lorawan, mfm384, provisioning, codec, payload, adr-005, demo
**Date:** 2026-06-29

## Context
How to demo the simulated-MFM384 telemetry path end-to-end WITHOUT a board on the bench: register a
device server-side, and prove the firmware's sim payload decodes through the real ChirpStack codec.
Two independent legs — the decode leg needs no credentials.

## Detail

### Leg 1 — local sim encode → real codec decode (no token, no hardware)
The sim path is honest: firmware `meter_sim_mfm384()` produces a real `mfm384_sample_t`, fed to the
SAME `payload_encode_mfm384()` (ADR-005) the live-meter path uses. Only difference is header byte 2
= `TELEMETRY_FLAG_SIMULATED (0x01)` → decoder tags `simulated:true`. Reproduce the encoder in JS
(saturating fixed-point: `v*scale+0.5` then integer-truncate; big-endian; `total_kw`/`avg_pf` are
i16 two's-complement) and feed the bytes to the actual `tools/chirpstack_mfm384_decoder.js`.

- Header `01 01 01` = schema v1 / device MFM384 (0x01) / flags SIMULATED.
- MFM384 body = 19 B: V1N,V2N,V3N (u16 ÷10 V), TotalkW (i16 ÷10), TotalkWh (u32 ÷100), Freq (u16 ÷100),
  AvgPF (i16 ÷1000). Well under AS923 DR3 53-B cap.
- Example: `01010108fd091608e20032000186c1138803c8` →
  `{device:"MFM384", v1n:230.1, v2n:232.6, v3n:227.4, total_kW:5, total_kWh:1000.33, freq_Hz:50,
   avg_pf:0.968, simulated:true}`
- Load the bare-function codec safely with `require()` of a temp copy — never `new Function(src)`.

### Leg 2 — server-side provision (needs valid auth)
Python env once: `uv venv .v && . .v/bin/activate && uv pip install chirpstack-api grpcio`.
Provision (admin-login fallback — see gotcha on why the cluster token fails):
```bash
. .v/bin/activate && CS_BASE=https://chirpstack.siot.solutions UA=Mozilla/5.0 \
  CS_ADMIN_USER=admin CS_ADMIN_PASS=admin \
  python3 .claude/skills/lorawan-provision-device/provision.py
```
Prints name `fw-<eui-tail>`, random DevEUI + AppKey, JoinEUI all-zero. Then ALWAYS clean up:
```bash
. .v/bin/activate && CS_BASE=https://chirpstack.siot.solutions UA=Mozilla/5.0 \
  CS_ADMIN_USER=admin CS_ADMIN_PASS=admin \
  python3 .claude/skills/lorawan-deprovision/deprovision.py <dev_eui>
```

### Production ChirpStack target (verified IDs, 2026-06-29)
- Host `https://chirpstack.siot.solutions` (Cloudflare → NPM → NodePort 30808 → `chirpstack.iot.svc:8080`, RKE2)
- Tenant *Southern IoT* `d4f227c3-2763-459e-bc6a-61d13f1a242b`
- Application *SouthernIoT-Devices* `7d5c0d50-4a4b-496d-ae7f-8ac61c4b3b18`
- Device profile *OTAA-AS923* `b14d1236-070b-49bb-a401-215bb29ae2b2`
- UA env is REQUIRED (Cloudflare blocks non-browser UAs).

## Usage
Run leg 1 anytime to show the codec round-trip. Run leg 2 only when a server-side registration is
actually needed (real board to flash). Test devices (`fw-…`) MUST be deprovisioned after — verified
clean on 2026-06-29 (`PASS: device … removed`). With no board, the device has `Last seen: never`.

## Related
- `.planning/knowledge/gotchas/chirpstack-cluster-token-not-an-api-key.md`
- `firmware/components/payload/{payload.c,include/{payload.h,telemetry.h}}`, `firmware/components/meter/meter.c`
- `tools/chirpstack_mfm384_decoder.js`, `docs/LORAWAN_PROVISIONING.md`
- Skills: `lorawan-provision-device`, `lorawan-deprovision`, `lorawan-verify-join`, `cluster-connect`
