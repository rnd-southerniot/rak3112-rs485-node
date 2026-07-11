# Note for Fahim — CRM / ChirpStack / firmware-service findings (2026-07-12)

Context: I stood up a **dev mirror of prod** on `10.10.8.140` (runs your exact prod images) + a real
LoRaWAN gateway, and ran the **full CRM flashing workflow end-to-end for both products**. Several findings
below affect **production** — please check items **#1 and #2** on prod, they are likely latent there too.

---

## 1. ⚠️ CROSS-TENANT GATEWAY — likely a live prod issue

The CRM registers every device into a **new per-client ChirpStack tenant** (migration
`chirpstack_tenant_per_client`). But a gateway belongs to **one** tenant. In ChirpStack v4, if the
gateway's tenant has **private gateways enabled**, devices in *other* tenants **cannot join through it** —
the join-request is received and the appKey matches, but ChirpStack sends **no join-accept** (device logs
`-1116` RX timeout forever).

On dev this blocked 100% of joins until I set, on the **gateway's tenant**:
- `private_gateways_up = false`
- `private_gateways_down = false`

**Action:** confirm your prod ChirpStack gateway-tenant has **private gateways OFF** (Tenant → settings), or
that gateways are otherwise shared across the per-client tenants. Otherwise real client devices won't join
even though provisioning "succeeds". (gRPC: `TenantService.Update` with those two flags false.)

---

## 2. ⚠️ SENSEFLOW ARTIFACT IS A STUB — don't ship gate-9 for real RF

The senseflow firmware has TWO LoRaWAN implementations:
- **`main/lorawan_service.c` = a STUB** (`init_stub` / `join_success` / `uplink_stub_ok`) — it **never
  touches the SX1262**. This is what the **gate runner (gates 0–9, incl. gate-9 `live_publish`)** uses. A
  build served from the gate-9 artifact will *simulate* join+uplink and **never reach ChirpStack**.
- **`CONFIG_APP_LORA_SMOKE=y` = the REAL field app** (RadioLib + `siot-lorawan-node`, same stack as
  careflow): real OTAA join → profile-driven read (ADR-006) + ADR-005 encode → **real uplink**.

The pre-built `senseflow-p4-green` artifact I found was the **gate-9 stub** (also hard-requires a real
BMP280 + e-ink display → `abort()`/bootloop on any board without them). **When senseflow is enabled on
prod, the CRM artifact must be built from `LORA_SMOKE`, not the gate runner.** I verified LORA_SMOKE works:
real RAK3312 WisBlock node → CRM flash → provision → **real join + uplink** in ChirpStack.

---

## 3. ✅ Careflow flasher "NVS profile missing" (C6) — still needs your redeploy

(unchanged from before) The step-5 boot-verify false-negative is fixed by redeploying the firmware-service
from rak3112 `main` (C6 = careflow `/v1/provisioning-protocol` now advertises `prov-profile`; also brings
the 5th profile `honeywell-eem400-scanned`). Live fw-svc = `fahim0173/rak3112-firmware-service:b60ea9d`
(pre-C6). Redeploy from `main`:
```
git -C rak3112-rs485-node checkout <main-sha>
DOCKER_BUILDKIT=1 docker build --secret id=component_repo_token,env=COMPONENT_REPO_TOKEN \
  -t fahim0173/rak3112-firmware-service:<main-sha> .
docker push fahim0173/rak3112-firmware-service:<main-sha>
kubectl -n firmware set image deploy/firmware-service firmware-service=fahim0173/rak3112-firmware-service:<main-sha>
# verify: GET /v1/provisioning-protocol?product=careflow now lists prov-profile
```

---

## 4. ✅ Both firmware paths proven E2E via the CRM workflow (on the dev mirror)

Full path for **each** product: CRM task → firmware-build (5-part flash-set) → `esptool` flash →
CRM **mint** (appKey) → NVS provision (creds + device-profile) → `flash-result` VERIFIED → status walk →
**factory scan** (registers in ChirpStack) → **real OTAA join via the gateway** → **uplink in ChirpStack**.
- **Careflow**: real RAK3112 + **RS-FSJT** sensor (real Modbus read, `dev=0x02`), joined, uplinking.
- **Senseflow**: real **RAK3312 WisBlock** node (LORA_SMOKE), joined (`devAddr=0136ca2d`), uplinking
  (STALE payload — the WisBlock node has motion sensors, no BME280 driver yet; RF path is what's proven).

Workflow gotchas I hit (FYI, in case they surface for you):
- Mint is gated by **D-14 over-flash**: a `hardware_procurements` row (`taskId`,`productNodeId`,`quantity`)
  must exist first (set in the requirements/procurement step).
- Task status is a state machine: `INITIALIZATION → SCHEDULED_VISIT → HARDWARE_PROCUREMENT_COMPLETE →
  HARDWARE_PREPARED_COMPLETE → READY_FOR_INSTALLATION`; factory scan requires `READY_FOR_INSTALLATION`.
- `flash-result` requires `binarySha256`.

---

## 5. Senseflow prod-enablement checklist (when you're ready)

Senseflow is not on prod yet (`SENSEFLOW_ROOT` unset, no senseflow product). To enable:
1. Build the **LORA_SMOKE** senseflow artifact (`.bin` + boot parts + `compiled_sensors.json` +
   `device-profiles/`), publish + mount it, set `SENSEFLOW_ROOT` in the `firmware` deployment.
2. Create a **senseflow product** + node (firmwareProductId=`senseflow`) + curated I²C profiles in the
   prod CRM (like `CF`).
3. Ensure the gateway-tenant fix (#1) is in place so senseflow devices can join.

---

## 6. Housekeeping
- **Rotate the CRM seed admin password** — `admin@southerneleven.com / SiotAdmin2026!` is in a public repo.
- The dev mirror on `10.10.8.140` now runs your prod `scomm-crm-api` + `scomm-crm-web` images + the careflow
  firmware-service + a real gateway — a faithful place to test provisioning/flashing before prod.
