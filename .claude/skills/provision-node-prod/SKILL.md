---
name: provision-node-prod
description: Register/provision a careflow node in the PRODUCTION SCOMM CRM → ChirpStack WITHOUT flashing — a pre-flash write smoke test. Uses the CRM's one-shot register-device endpoint (CRM owns the ChirpStack creds) to create a device in production ChirpStack, verifies it, and (for a throwaway) deletes it. Validated end-to-end 2026-07-11. WRITES to production — confirm first. Flash the physical board separately.
---

# provision-node-prod

Exercise the **production** CRM → ChirpStack provisioning write-path **without flashing a board**, so you
get a green/red before a real-time flash. Validated end-to-end on 2026-07-11 (throwaway create → verify
→ delete, prod left clean). The dev-stack bench equivalent (build → flash → provision → verify) is
[provision-node](../provision-node/SKILL.md).

> **⚠ WRITES to production** (Fahim's k8s). Production is read-only by default (two-stack rule + global
> §11/§14). **Confirm the exact write with the user before running.** For a throwaway, use a `SMOKE-…`
> name + a clearly-fake DevEUI (`f0…`) and **delete it after**. To provision the *real* board, register
> its real DevEUI so the later physical flash joins what was registered.

## Prod endpoints (confirmed 2026-07-11)

| Thing | Value |
|---|---|
| CRM API base | **`http://10.10.8.168:30400`** — the `scomm-api` NodePort (k8s ns `scomm-crm`), routes at **root** (no `/crm-api` prefix). Public alt: `https://crm.siot.solutions/crm-api` (Cloudflare → needs `User-Agent: Mozilla/5.0`; `crm.siot.solutions` root is the Next.js UI only). |
| CRM creds | `~/.config/siot/rak3112-crm-prod.env` → `CRM_BASE`/`CRM_EMAIL`/`CRM_PASSWORD`. `admin@southerneleven.com` = ADMIN (drives every step; exempt from per-step role checks). |
| ChirpStack (verify/delete) | `CS_BASE=https://chirpstack.siot.solutions` + `CHIRPSTACK_ADMIN_API_TOKEN` (`~/.config/siot/chirpstack-prod.env`) + `UA=Mozilla/5.0`. gRPC-web; confirmed working. |
| Board under test | DevEUI/AppKey in `firmware/.env` (MAC `3c:dc:75:6f:85:dc` → DevEUI `3cdc75fffe6f85dc`). |

## Contract (from siot-crm-review, Phase 4/6)

- **Quick register (Path 2, what this skill uses):** `POST /chirpstack/register-device`
  `{devEui, appKey, name, applicationId, deviceProfileId}` (roles ADMIN/HARDWARE_ENGINEER). The CRM
  wraps ChirpStack `DeviceService.Create + CreateKeys`. **Idempotent** (`ALREADY_EXISTS` = success).
  ⚠ **`applicationId` + `deviceProfileId` are REQUIRED** — Phase-4 *tenant-per-client*, no default
  fallback. (The `register_via_crm.sh` in siot-crm-review is stale — it omits them and now fails.)
- **Discover the target app/profile** (don't hardcode a client's IDs): `GET /chirpstack/status`
  (→ `templateDeviceProfileId`, `gatewayTenantId`), or `GET /chirpstack/device/<eui>` of an
  already-onboarded device (→ its `applicationId` + `deviceProfileId`). E.g. `85:dc` currently sits in
  app `fd23ae10-…` / profile `20979d82-…`; the deprovision test app default is `7d5c0d50-…`.
- **Full onboarding workflow (Path 3)** auto-provisions on `PUT /workflow/tasks/{id}/status/READY_FOR_INSTALLATION`
  after: `POST /workflow/tasks` → `…/SCHEDULED_VISIT` → `…/REQUIREMENTS_COMPLETE` (reportData) →
  `…/HARDWARE_PROCUREMENT_COMPLETE` (hardwareList) → `…/HARDWARE_PREPARED_COMPLETE`
  (deviceList w/ devEui+appKey) → `…/pre-installation-checklist` (9× true). **Blocked on prod today:**
  the hardware catalog is **empty** (no `hardwareId`), so Path 3 can't supply procurement/prepared
  hardware. Use Path 2 for the smoke.
- **`provision_node.py` (the dev tool) does NOT work on prod:** prod product code is **`CF`** (not
  `RAK3112-RS485-AS923` → it would create a duplicate product) and the hardware catalog is empty (it
  would then crash). Do not point it at prod.

## Workflow (Path-2 smoke, validated)

1. **Confirm PROD + scope** with the user. Fill/verify `~/.config/siot/rak3112-crm-prod.env`
   (`CRM_BASE=http://10.10.8.168:30400`). `set -a; source` both env files; `set +a`.
2. **Login:** `POST /auth/login {email,password}` → `access_token` (redact it).
3. **Pick target app/profile** via `GET /chirpstack/device/<known-eui>` or `GET /chirpstack/status`.
4. **Register:** `POST /chirpstack/register-device {devEui, appKey, name, applicationId, deviceProfileId}`
   → expect `HTTP 201 {success:true}`. AppKey from `firmware/.env` (real board) — never printed.
5. **Verify:** `GET /chirpstack/device/<eui>` → `found:true`. Optionally the ChirpStack read directly:
   `DEV_EUI=<eui> uv run --with chirpstack-api python \
   ~/Developer/projects/siot-crm-review/.claude/skills/lorawan-verify-join/verify.py` (needs `CS_BASE`/
   `CS_API_TOKEN`/`UA`; `chirpstack_api` isn't installed system-wide → `uv run --with chirpstack-api`).
6. **Report** PASS/FAIL. For the real board, hand off for physical flash. **For a throwaway, DELETE it
   (step 7).**
7. **Cleanup (throwaway):** `DEV_EUI=<eui> CS_APP_ID=<same app> uv run --with chirpstack-api python \
   ~/Developer/projects/siot-crm-review/.claude/skills/lorawan-deprovision/deprovision.py`
   → `REMOVED=true`; then re-confirm `GET /chirpstack/device/<eui>` → `found:false`.

## Guardrails

- **Production writes — confirm every run.** Throwaways: `SMOKE-…` name + `f0…` DevEUI + delete after.
- **AppKey is a secret** — from `firmware/.env` only, redacted in every log, never committed.
- **No flash here.** This skill never touches a board. Physical flash = [provision-node](../provision-node/SKILL.md).
- **Bench joins go to DEV** (`10.10.8.140`) by default. Use this prod skill only for a genuine prod
  onboarding or an explicitly-authorized prod smoke test.
- **⚠ Flash AppKey must match the registered one.** If the board was already onboarded (e.g. `85:dc`
  was registered + uplinked on prod 2026-07-05), the ChirpStack device holds the CRM-minted AppKey —
  the board must be flashed with **that** key, not a stale `firmware/.env` value, or it won't join.
  Confirm the registered AppKey with Fahim before a real flash.

## Rollback

Throwaway devices: `deprovision.py` (above). A real onboarded device: `deprovision.py` deletes the
ChirpStack device; CRM tasks have no delete API (revert the task, or delete the row in CRM Postgres).
Only remove devices you created in this run; confirm before deleting anything else.
