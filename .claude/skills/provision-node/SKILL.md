---
name: provision-node
description: Bring up one RAK3112/RAK3312 SIoT node end-to-end — build the pinned firmware, safety-gated flash (confirm board MAC first), register in ChirpStack (Plane A), write NVS creds + device config (Plane B), and verify the OTAA join + a decoded uplink. Product-aware (careflow Modbus / senseflow I2C). Use when onboarding or re-provisioning a physical node on a flashing station. Prompts for dev vs production ChirpStack.
---

# provision-node

Full node bring-up on a flashing station: **build → safety-gated flash → provision (Plane A + B) →
verify**. One generic image per product; each unit differs by **data** (device profile + LoRaWAN
creds) provisioned at flash time. Background: the CRM firmware agent guide + `docs/PROVISIONING_API_CONTRACT.md`.

> **Runs on a host physically connected to the board** (flashes over USB, reads the boot log).
> **Two stacks — always prompt first:** developer ChirpStack `10.10.8.140` vs production
> `chirpstack.siot.solutions`. Support the selected one (see the two-stack rule in memory).

## When to use

- Onboarding a new node, or re-provisioning one after a firmware/creds/profile change.
- After it, confirm the join with [lorawan-verify-join](../lorawan-verify-join/SKILL.md).

## Inputs

| Input | Source | Secret |
|---|---|---|
| `PRODUCT` | careflow \| senseflow | no |
| `DEVICE_PROFILE` | e.g. `selec-mfm384`, `bosch-bme280` | no |
| `DEVEUI` | derived from board MAC (EUI-48→EUI-64 FF:FE); or provided | no |
| `APPKEY` | minted by CRM/ChirpStack; via `firmware/.env` / env — **never inline** | **yes** |
| Modbus cfg / profile blob | product-specific (`prov-modbus` vs `prov-profile`) | no |

## Workflow

1. **Build** the pinned firmware (one image). careflow: `idf.py -C firmware build`. senseflow: same,
   `CONFIG_APP_LORA_SMOKE=y` for the real field app. Do not regenerate per device.
2. **Hardware safety gate (mandatory).** List ports; read the chip MAC (`esptool ... read_mac`) and
   **confirm it is the intended board** before any write. Two boards on the bench are disambiguated by
   MAC. Never `erase_flash`.
3. **Flash** over USB (`idf.py -p <port> flash`, preserves NVS) and confirm a clean boot (PSRAM,
   console, no bootloop).
4. **Plane A — register in ChirpStack** (network identity). `tools/provision_node.py --product <p>`
   walks the SCOMM CRM onboarding → ChirpStack registration on the selected stack; or register
   directly via gRPC-web ([lorawan-provision-device](../lorawan-provision-device/SKILL.md)). Mints the
   AppKey.
5. **Plane B — write NVS** (device identity + config). `tools/provision_nvs.py --product <p> -p <port>`
   over the `prov>` console: `prov creds <deveui> <joineui> <appkey>`, then `prov-modbus …` (careflow)
   or `prov-profile <hexblob>` (senseflow), then `prov-done`. Additive — LoRaWAN nonces/session survive.
6. **Verify.** Watch the boot log for `[creds:NVS]` + `uplink OK`; confirm in ChirpStack the device
   `last_seen` advances and an uplink **decodes** (temp/pressure or Modbus registers) via its
   device-profile codec (install it first if missing — see
   [install-chirpstack-decoder](../install-chirpstack-decoder/SKILL.md)).

## Guardrails

- **Confirm the board MAC before every flash.** Never `esptool erase_flash` / full-chip erase.
- **AppKey is a secret** — read from `firmware/.env` / env, never printed (redact in all logs), never
  committed.
- Prompt for **dev vs production** ChirpStack before any CRM/ChirpStack step; production (Fahim's k8s)
  is operationally sensitive.
- One image, provision by **data**. Device selection (MFM384/RS-FSJT) + slave/unit ID are NVS values,
  not separate builds.

## CRM WebSerial flash-flow gotchas (VM-141 platform)

- **provisioning-protocol placeholder = flasher context key (HARD contract).** The flasher substitutes
  each `<placeholder>` in a command `syntax` with `context[name]`, where the flasher's keys are camelCase
  `{devEui, joinEui, appKey, baud, parity, stopBits, slaveId, blobHex}`. A wrong placeholder name →
  `renderProvisioningCommands: no context value for <NAME>` and the flash fails. `blobHex` (NOT `hexblob`)
  comes from `GET /provisioning/firmware-build/profile-blob/<key>?taskId&nodeId`. Any new `prov-*` command
  in `api/routers/builds.py` must use these exact names. (Fixed `<hexblob>`→`<blobHex>` 2026-07-13.)
- **Editing a product recreates its productNode with a new id** → orphans an already-open flash page
  (profile-blob/protocol 404 → hexblob error). **Configure profiles before flashing; hard-reload after any
  product edit.**
- **A FAILED flash blocks retry via the D-14 over-flash gate** — the failed `device_provisionings` row
  (`nvsStatus=FAILED`, no `devEui`) still counts. Delete it to retry.
- **A deleted device "reappears" in the flash modal** = stale client-side modal state, not a contract bug
  (`GET /provisioning/task/<id>/devices` is the truth). Reload clears it.
- **WebSerial needs Chrome/Edge + a secure context** — `ssh -L 8090:localhost:8090 siot-ops` → open
  `http://localhost:8090` (localhost is secure over HTTP); the raw IP is blocked.
