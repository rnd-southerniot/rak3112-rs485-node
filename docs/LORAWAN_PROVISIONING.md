# LoRaWAN Provisioning — Southern IoT ChirpStack

Three Claude Code skills register / verify / remove a LoRaWAN device in Southern IoT's **production**
ChirpStack during firmware bring-up, and map the credentials into a RAK node.

| Skill | What it does |
|---|---|
| [`lorawan-provision-device`](../.claude/skills/lorawan-provision-device/SKILL.md) | create one OTAA device + keys, print DevEUI/AppKey |
| [`lorawan-verify-join`](../.claude/skills/lorawan-verify-join/SKILL.md) | report presence, `last_seen_at`, and the activation DevAddr |
| [`lorawan-deprovision`](../.claude/skills/lorawan-deprovision/SKILL.md) | delete a test device + confirm removal |

Shared gRPC-web client: [`.claude/skills/_shared/cs_grpcweb.py`](../.claude/skills/_shared/cs_grpcweb.py).

## Environment

```bash
export CS_BASE=https://chirpstack.siot.solutions   # production cluster
export UA="Mozilla/5.0"                              # REQUIRED — Cloudflare blocks non-browser UAs
export CS_API_TOKEN=…                                # ChirpStack → Tenant 'Southern IoT' → API keys
# dev fallback only: export CS_ADMIN_USER=admin CS_ADMIN_PASS=…

# one-time Python env:
uv venv .v && . .v/bin/activate && uv pip install chirpstack-api grpcio
```

Copy [`.env.example`](../.env.example) → `.env` and fill in `CS_API_TOKEN` (the file's other fields
are non-secret defaults). **Never commit a real token.**

## The facts that make this work

- **Cluster:** `https://chirpstack.siot.solutions` (production). NOT the dev copy at `10.10.8.140:8080`
  (different instance, different data).
- **gRPC-web only.** No REST/JSON gateway exists; native gRPC is blocked by Cloudflare. Every call is
  `POST /api.<Service>/<Method>` with `Content-Type: application/grpc-web+proto` **and a browser
  `User-Agent`** — without the UA, Cloudflare returns 403 / error 1010.
- **Region AS923, OTAA, LoRaWAN 1.0.3.** For OTAA 1.0.x, set BOTH `nwk_key` and `app_key` to the AppKey.
- **Known IDs (production):** tenant `d4f227c3-2763-459e-bc6a-61d13f1a242b` · application
  `7d5c0d50-4a4b-496d-ae7f-8ac61c4b3b18` (SouthernIoT-Devices) · device profile
  `b14d1236-070b-49bb-a401-215bb29ae2b2` (OTAA-AS923).
- **`UNAUTHENTICATED` == "not found".** A tenant-scoped API key returns `UNAUTHENTICATED` (not
  `NOT_FOUND`) for a device that doesn't exist yet. The skills treat that as not-found. **This is a
  ChirpStack quirk — never rotate the token for this symptom.**
- **Live uplinks:** MQTT topic `application/<APP_ID>/device/<DevEUI>/event/up`.

## End-to-end (provision → flash → verify → clean up)

```bash
# 1) register a device (random DevEUI + AppKey)
python3 .claude/skills/lorawan-provision-device/provision.py        # note the printed DevEUI + AppKey

# 2) flash those into the node (see the AT cheatsheet below), power it, let it join

# 3) confirm the join landed
python3 .claude/skills/lorawan-verify-join/verify.py <DevEUI>       # expect a JOINED DevAddr line

# 4) remove the test device
python3 .claude/skills/lorawan-deprovision/deprovision.py <DevEUI>  # expect "removed"
```

## RAK3172 AT-command cheatsheet (map the provisioned creds into firmware)

The provisioned `DevEUI` (16 hex) + `AppKey` (32 hex) go into the modem like this. `JoinEUI`/`AppEUI`
is unconstrained on this profile — all-zero is fine.

```text
AT+NWM=1                 # LoRaWAN mode
AT+NJM=1                 # OTAA join mode
AT+BAND=8                # AS923  (verify the band index on your RAK firmware build!)
AT+DEVEUI=<16hex>        # the provisioned DevEUI
AT+APPEUI=0000000000000000   # JoinEUI/AppEUI — all-zero OK on this profile
AT+APPKEY=<32hex>        # the provisioned AppKey
AT+JOIN=1:0:8:0          # join: start, no-auto, 8s retry interval, 0 retries
                         # -> watch for  +EVT:JOINED
```

- `+EVT:JOINED` ⇒ OTAA succeeded; `lorawan-verify-join` will then show a DevAddr.
- **`Invalid MIC`** ⇒ the AppKey on the device ≠ the AppKey in ChirpStack. Re-check `AT+APPKEY` and the
  provisioned key (re-run provision to print the authoritative stored key).
- No join at all ⇒ check `AT+BAND` index, antenna, and gateway coverage; confirm the device shows in
  ChirpStack with `lorawan-verify-join`.

> RAK3112 (ESP32-S3 + SX1262) joins via its on-firmware RadioLib stack rather than AT commands — for
> that node, this AT block is the reference mapping; load the same DevEUI/AppKey via its provisioning
> path (`tools/provision_nvs.py`).

## Guardrails

- **No secrets in the tree** — env vars only; `.env.example` ships empty placeholders.
- Test devices use a `fw-` / `TEST-` name prefix and **must** be deprovisioned. For >1 device, confirm
  with the operator first.
- Idempotent: `ALREADY_EXISTS` is ignored on create; `UNAUTHENTICATED` is treated as not-found on reads.
