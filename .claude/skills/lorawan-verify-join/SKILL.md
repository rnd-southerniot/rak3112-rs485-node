---
name: lorawan-verify-join
description: Verify a LoRaWAN device's presence and OTAA join in Southern IoT's production ChirpStack (chirpstack.siot.solutions). Given a DevEUI, reports whether the device exists, its last_seen_at, and the activation DevAddr — confirming an OTAA join succeeded during firmware bring-up. Use after provisioning + flashing to check the node joined, or to look up a device's status. Read-only.
---

# lorawan-verify-join

Read-only check of one device in Southern IoT's **production** ChirpStack via **gRPC-web**:
`DeviceService.Get` (presence + `last_seen_at`) and `DeviceService.GetActivation` (the OTAA DevAddr).
Creates no state.

> Targets `https://chirpstack.siot.solutions`. Live uplinks stream on MQTT topic
> `application/<APP_ID>/device/<DevEUI>/event/up`. Background: [docs/LORAWAN_PROVISIONING.md](../../../docs/LORAWAN_PROVISIONING.md).

## When to use

- Right after [lorawan-provision-device](../lorawan-provision-device/SKILL.md) + flashing, to confirm
  the node completed its OTAA join (a DevAddr appears once it joins).
- To look up whether a device exists and when it was last heard from.

## Prerequisites (env vars)

```bash
export CS_BASE=https://chirpstack.siot.solutions
export UA="Mozilla/5.0"            # REQUIRED (Cloudflare bot rule)
export CS_API_TOKEN=…              # or CS_ADMIN_USER/CS_ADMIN_PASS
# one-time: uv venv .v && . .v/bin/activate && uv pip install chirpstack-api grpcio
```

## Run

```bash
python3 .claude/skills/lorawan-verify-join/verify.py <dev_eui-16hex>
# or: DEV_EUI=<eui> python3 .claude/skills/lorawan-verify-join/verify.py
```

## Smoke test (PASS/FAIL)

```bash
python3 .claude/skills/lorawan-verify-join/verify.py <eui>
# PASS (exists):  "PASS: device present" + name + last_seen. If the node has joined, a
#                 "JOINED  DevAddr <hex>" line; otherwise "activation none yet".
# NOT FOUND:      "NOT FOUND: no device <eui>…" and exit 1 — the device isn't registered, OR the
#                 tenant token isn't authorized for it (same signal here). This is the documented
#                 UNAUTHENTICATED-as-not-found quirk — do NOT rotate the token.
# FAIL:           "FAIL:" (HTTP 403 -> UA missing; unreachable -> network/CS_BASE).
```

## Cleanup

This skill is read-only — nothing to clean up. Remove the device itself with
[lorawan-deprovision](../lorawan-deprovision/SKILL.md) when the test is done.
