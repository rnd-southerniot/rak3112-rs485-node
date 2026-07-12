---
name: lorawan-provision-device
description: Provision (create) ONE OTAA LoRaWAN device directly in Southern IoT's production ChirpStack (chirpstack.siot.solutions) during firmware bring-up. Generates a random DevEUI + AppKey (or uses provided ones), registers the device + keys via gRPC-web, and prints the credentials to flash into a RAK3172/RAK3112 node. Use when you need to register a new node, get a DevEUI/AppKey for an OTAA join, or onboard a test device. Always pair with lorawan-deprovision for test devices.
---

# lorawan-provision-device

Creates one OTAA device in Southern IoT's **production** ChirpStack via **gRPC-web**
(`DeviceService.Create` → `CreateKeys`), then confirms it exists and prints the credentials.
Idempotent — re-running with the same DevEUI is safe (`ALREADY_EXISTS` is ignored; the stored key is
read back so the printed AppKey is authoritative).

> **⚠ ALWAYS CHECK FIRST — a pre-existing DevEUI keeps its OLD AppKey.** `CreateKeys` is a **no-op on
> an existing device**, so if the DevEUI is already registered (common: dev-DB not fully reset, or a
> device from a prior bring-up), a freshly-passed `APP_KEY` is **silently ignored** — the STORED key
> wins. Flashing your intended key then fails to join with an **AppKey/MIC mismatch (RadioLib `-1116`,
> "DevNonce persisted")**. The script now **pre-checks `GetKeys` and WARNs** on a mismatch; heed it.
> To actually apply YOUR key (and clear the DevNonce history so the fresh join is accepted):
> **`lorawan-deprovision <deveui>` then re-run** `provision.py` with your `APP_KEY`. Otherwise flash the
> **stored** key the script prints. Rule of thumb: the on-node NVS AppKey MUST equal CS `GetKeys`.

> Targets `https://chirpstack.siot.solutions` (production). **Not** the dev copy at `10.10.8.140:8080`.
> Region AS923, OTAA, LoRaWAN 1.0.3. Background + AT cheatsheet: [docs/LORAWAN_PROVISIONING.md](../../../docs/LORAWAN_PROVISIONING.md).

## When to use

- You're bringing up a node and need a DevEUI/AppKey registered server-side for an OTAA join.
- You want a throwaway test device to validate the join path (then deprovision it).

For **>1 device**, confirm with the operator first — this creates real state.

## Prerequisites (env vars — never hardcode)

```bash
export CS_BASE=https://chirpstack.siot.solutions
export UA="Mozilla/5.0"            # REQUIRED — Cloudflare returns 403/1010 without a browser UA
export CS_API_TOKEN=…              # ChirpStack → Tenant 'Southern IoT' → API keys (preferred)
# dev fallback only: export CS_ADMIN_USER=admin CS_ADMIN_PASS=…
```

One-time Python env (the scripts need `chirpstack-api` + `grpcio`):

```bash
uv venv .v && . .v/bin/activate && uv pip install chirpstack-api grpcio
```

Known IDs (production defaults, baked in; override via env if needed): tenant
`d4f227c3-…`, application `7d5c0d50-…` (SouthernIoT-Devices), device profile `b14d1236-…` (OTAA-AS923).

## Run

```bash
# random DevEUI + AppKey:
python3 .claude/skills/lorawan-provision-device/provision.py

# or pin a DevEUI / AppKey:
DEV_EUI=70b3d5xxxxxxxxxx APP_KEY=<32hex> python3 .claude/skills/lorawan-provision-device/provision.py
```

It prints `name`, `DevEUI`, `AppKey`, `JoinEUI` (all-zero for this profile), and `app_id`.

## Smoke test (PASS/FAIL)

```bash
python3 .claude/skills/lorawan-provision-device/provision.py
# PASS: stdout starts with "PASS: device provisioned" and lists a 16-hex DevEUI + 32-hex AppKey.
# FAIL: any "FAIL:" line. HTTP 403 -> UA env missing (NOT a bad token). "not present after Create"
#       -> CS_API_TOKEN scope/network (a tenant token reads UNAUTHENTICATED for a missing device —
#          do NOT rotate the token for this).
```

## Cleanup

Test devices must be removed. Note the printed DevEUI and run:

```bash
DEV_EUI=<eui> python3 .claude/skills/lorawan-deprovision/deprovision.py <eui>
```

See [lorawan-verify-join](../lorawan-verify-join/SKILL.md) to confirm the join, and
[lorawan-deprovision](../lorawan-deprovision/SKILL.md) to remove it.
