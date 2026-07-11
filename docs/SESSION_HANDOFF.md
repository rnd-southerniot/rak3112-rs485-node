# Session handoff — 2026-07-12 (rak3112-rs485-node)

Handoff of a long **operational** session: stood up a full dev mirror of prod and proved the **entire
CRM flashing workflow end-to-end for BOTH products** (careflow + senseflow), on real hardware, up to
ChirpStack. **No careflow firmware code changed this session** — `main` is at **`ac31d77`** (C6, #24).
Open PR: **#25** (`provision-node-prod` skill — docs, unmerged, your call).

> The substance of this session lives in **auto-memory** (`dev-full-flash-pipeline`,
> `dev-crm-mirrors-prod`, `dev-gateway-on-140-chirpstack`, `next-session-brainstorm-context`) and in the
> **`NOTE_FOR_FAHIM`** (findings that affect prod). This doc is the index.

---

## 1. What got done (operational — dev mirror + E2E, no repo code changes)

| Area | Result |
|---|---|
| **Dev CRM mirror** | `10.10.8.140` now runs Fahim's **exact prod images** (`scomm-crm-api` + `scomm-crm-web`) + the careflow firmware-service + MinIO/Postgres. DB reset → prod schema (24 migrations) → seeded `admin@southerneleven.com`. Contract verified == prod. |
| **Real gateway** | RAKPiOS SX1302 gw (`192.168.68.106`) moved onto dev ChirpStack `10.10.8.140` as concentrator EUI **`0016C001F13681DF`** — ONLINE, relaying uplinks. |
| **Flasher UI** | Prod Phase-6 WebSerial flasher (prod `scomm-crm-web` image) + nginx `/crm-api` proxy → reach via `ssh -L 8090:localhost:8090 siot-new` → `http://localhost:8090`. |
| **fw-svc both products** | Overlay image (C6 + 5 profiles) over `b60ea9d` + senseflow artifact mounted (`SENSEFLOW_ROOT`) → `/v1/products = [careflow, senseflow]`. |
| **E2E careflow** | Real RAK3112 `85:dc` + **real RS-FSJT** sensor: CRM flash → mint → provision → factory-scan → **real join via gateway → uplink** in ChirpStack. PASS |
| **E2E senseflow** | Real **RAK3312 WisBlock** node `89:24` (`LORA_SMOKE` firmware): same CRM flow → **real join → uplink** (`devAddr=0136ca2d`). PASS |

## 2. Key findings (in `NOTE_FOR_FAHIM` — affect PROD)

1. **Cross-tenant gateway.** The CRM registers each device into a **per-client ChirpStack tenant**; a
   private gateway then blocks all cross-tenant joins (appKey matches, no join-accept, board logs `-1116`).
   Fix on the **gateway's tenant**: `private_gateways_up = false`, `private_gateways_down = false`. **Prod
   likely has this latent.**
2. **Senseflow artifact must be `CONFIG_APP_LORA_SMOKE`, not the gate-9 stub.** The senseflow gate runner
   (`main/lorawan_service.c`) is a pure **LoRaWAN stub** (`init_stub`/`uplink_stub_ok`, never hits the
   radio). The real SX1262 field app is behind `LORA_SMOKE` (real join + profile-driven read + encode +
   uplink). The pre-built `senseflow-p4-green` artifact was the stub → never joins real ChirpStack.
3. **Careflow C6 redeploy** (still pending on Fahim): live fw-svc `b60ea9d` is pre-C6; redeploy from `main`
   (`ac31d77`) for `prov-profile` + the 5th profile.

## 3. State of things

- **Repos:** careflow `main` = `ac31d77` (clean, pushed); senseflow (`rak4630-e-ink`) `feat/buttons-field-app`
  = `e2f6534` (clean, pushed). PR #25 open (provision-node-prod skill).
- **Dev env** (`ssh siot-new` = `10.10.8.140`, `/root/southern-iot`, docker compose): mirror + gateway +
  both products live. Backups on box: compose `.premirror-*`, `southern-iot-scomm-web:premirror-web`,
  `senseflow-artifact/` currently holds the **LORA_SMOKE** build.
- **Bench:** `85:dc` careflow (RS-FSJT) + `89:24` RAK3312 WisBlock senseflow — both joined/uplinking to dev
  ChirpStack.

## 4. Next session — BRAINSTORM (not started here, by design)

Plan Arif's **standalone platform** with a **multi-agent workflow** (see memory
`next-session-brainstorm-context`): fresh VM + independent repos (no existing repo patched) — ops stack
(CRM/ChirpStack/NodeRED/InfluxDB, single compose) + one multi-product firmware repo (device-profiles +
decoder lib + knowledge-base service) + binaries hub + **Grafana/Home-Assistant dashboards with MCP
automation** + **Mattermost** integration. Everything up to ChirpStack is proven PoC; the **dashboard layer
is the genuinely new work**.

## 5. Open items

- PR #25 (provision-node-prod skill) — merge decision.
- Fahim: gateway private-flag check · senseflow LORA_SMOKE artifact · careflow C6 redeploy · senseflow
  prod-enablement (SENSEFLOW_ROOT + product + profiles).
- New WisBlock senseflow hardware (RAK1921 OLED, RAK1905/12025/12034 motion sensors) — **no drivers/profiles
  yet** = firmware dev for the new plan.
- Rotate the public-repo CRM seed password (`SiotAdmin2026!`).
