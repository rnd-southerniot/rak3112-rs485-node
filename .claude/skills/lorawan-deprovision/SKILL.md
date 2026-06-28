---
name: lorawan-deprovision
description: Delete (deprovision) a test LoRaWAN device from Southern IoT's production ChirpStack (chirpstack.siot.solutions) and confirm it's gone. Use to clean up after firmware bring-up / a join test, to remove a throwaway fw-/TEST- device, or to undo a provisioning. Always run this after a test device has served its purpose so nothing is left behind.
---

# lorawan-deprovision

Removes one device from Southern IoT's **production** ChirpStack via **gRPC-web**
(`DeviceService.Delete`), then confirms removal with `DeviceService.List` (a single `Get` can read
stale immediately after a delete, so we list-and-check). Idempotent — deleting an already-gone device
is fine.

> Targets `https://chirpstack.siot.solutions`. Background: [docs/LORAWAN_PROVISIONING.md](../../../docs/LORAWAN_PROVISIONING.md).

## When to use

- A `fw-…` / `TEST-…` device created by [lorawan-provision-device](../lorawan-provision-device/SKILL.md)
  has served its purpose — remove it so no test state is left on the cluster.
- You need to undo a provisioning.

Only deletes the single DevEUI you pass. Deleting production field devices is destructive — be sure
of the DevEUI.

## Prerequisites (env vars)

```bash
export CS_BASE=https://chirpstack.siot.solutions
export UA="Mozilla/5.0"            # REQUIRED (Cloudflare bot rule)
export CS_API_TOKEN=…              # or CS_ADMIN_USER/CS_ADMIN_PASS
# one-time: uv venv .v && . .v/bin/activate && uv pip install chirpstack-api grpcio
```

## Run

```bash
python3 .claude/skills/lorawan-deprovision/deprovision.py <dev_eui-16hex>
# or: DEV_EUI=<eui> python3 .claude/skills/lorawan-deprovision/deprovision.py
```

## Smoke test (PASS/FAIL)

```bash
python3 .claude/skills/lorawan-deprovision/deprovision.py <eui>
# PASS: "PASS: device <eui> removed (confirmed absent…)".
# FAIL: "FAIL:" — "still present after Delete" (retry / token scope); HTTP 403 -> UA missing;
#       unreachable -> network/CS_BASE. (Never rotate the token for an auth-shaped symptom.)
```

## Cleanup note

This **is** the cleanup step for the provisioning flow. After it,
[lorawan-verify-join](../lorawan-verify-join/SKILL.md) on the same DevEUI should report `NOT FOUND`.
