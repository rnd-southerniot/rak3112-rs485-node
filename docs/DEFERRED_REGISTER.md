# Deferred / Open-Issue Register — rak3112-rs485-node (careflow) + platform

Compiled **2026-07-13** from session handouts (auto-memory), `CLAUDE.md` §7 open questions, the
`.planning/knowledge` base, and open PRs. Status reconciled to the current platform state (2026-07-13).
Purpose: clear every item before the production E2E test.

**Legend** — 🔴 blocks the careflow E2E flash/test · 🟠 production-hardening (before field) ·
🟢 not blocking (other product / hygiene / already done).

---

## 0. IMMEDIATE — clear before re-running the E2E test

| # | Item | Status | Source |
|---|---|---|---|
| I1 | **prov-profile `<hexblob>`→`<blobHex>` contract fix.** Code fixed in 3 repos; merge **PR #8** (hub `siot-firmware-automation`), **PR #3** (`siot-node-firmware-automation`), **PR #14** (siot-ops docs) + commit rak3112 (guard-blocked) → **rebuild+redeploy the `firmware-automation` hub** → verify `/v1/provisioning-protocol?product=careflow` returns `prov-profile <blobHex>`. | 🔴 code done, deploy pending | `crm-flasher-protocol-placeholder-contract` |
| I2 | **D-14 over-flash gate counts FAILED provisionings** → blocks retry. Workaround: delete the failed `device_provisionings` row (`nvsStatus=FAILED`, no `devEui`). Proper CRM-side fix (don't count FAILED / Re-run reuses row) is Fahim's. | 🔴 workaround only | `crm-flash-workflow-gotchas` |
| I3 | **Flash-flow UX traps** — editing a product rotates the productNode id (hard-reload the flash page); WebSerial needs Chrome/Edge + `localhost` tunnel; stale flash modal shows a deleted device (reload). | 🔴 operator procedure | `crm-flash-workflow-gotchas` |

---

## 1. FIRMWARE — Phase 7 production hardening (before field deployment)

| # | Item | Status |
|---|---|---|
| P7a | **OTA network transport (OQ-10 / ADR-007).** Only *local* OTA + rollback proven (7c). WiFi/HTTPS OTA, then LoRaWAN **FUOTA**, NOT built → node cannot update over-the-air (only re-flash). Blocked on bench WiFi creds. **Served fw `careflow-2026.07-e230bbb` IS OTA-capable — verified: `ota-data@0xf000` + `app@0x20000` (ota_0).** | 🟠 open |
| P7b | **Light-sleep duty-cycle (7b / OQ-9).** Default OFF; **wedges the node on USB/VBUS** → needs a battery / no-VBUS bench to enable + measure. | 🟠 open |
| P7c | **OQ-12** — average-current target (sets the 7b gate). Proposed ≤10 mA @60 s; confirm vs RT6160 quiescent + battery budget. | 🟠 open (TBD) |
| P7d | **Phase 7e soak + sign-off + tag `phase-7-production-green`.** Git tags stop at `phase-6-modbus-green`; 7c is on `main` but Phase 7 is not closed. | 🟠 open |
| P7e | **OQ-11 / ADR-008 provisioning transport** — WiFi SoftAP field portal (bench console NVS writer done). | 🟠 open |
| P7f | **Real-MFM384 uplink leg** — read path proven; uplink unexercised (meter off bench). Run when the meter returns. | 🟢 on meter return |
| P7g | **ADR-003 / ADR-004 formal close** (RadioLib stack / AS923 sub-band) — Phase 5 sign-off doc scope. | 🟢 doc |
| P7h | **OQ-7 PARLIO patch not vendored** — local `v5.5.4-dirty` off-compile-path; accepted asymmetry, documented (RUNBOOK). | 🟢 accepted |
| — | OQ-1 (RS-232 DNP), OQ-2 (PSRAM octal), OQ-5 (antenna) — **CLOSED** in ADR-001. OQ-6/OQ-8 closed. | 🟢 closed |

---

## 2. SECOND PRODUCT (senseflow) — blocked on hardware/drivers (NOT blocking careflow)

| # | Item | Status |
|---|---|---|
| S1 | **B4 RAK3312-motion drivers** — RAK12025 (I3G4250D), RAK12034 (BMX160), RAK1921 (SSD1306), RAK1905 mag/AK8963 — **BLOCKED on user-provided register maps**; firmware PAUSED per Arif; bench-verify together. | 🟢 blocked/paused |
| S2 | New WisBlock sensors need device-profiles + drivers (new-session firmware dev). | 🟢 |
| S3 | `libs/sensors_i2c` cosmetic promotion. | 🟢 deferred |
| S4 | Senseflow physical button-press test. | 🟢 deferred |

---

## 3. DEVICE-PROFILES / CRM DATA

| # | Item | Status |
|---|---|---|
| D1 | **v1→v2 profile migration** (careflow v1 Modbus-only blob vs senseflow v2 bus-discriminated). Deferred; **PLAN in a fresh gated session** — touches the deployed CRM + board re-provisioning. | 🟠 plan-first |
| D2 | device-profiles **increment 5** — validate all 4 profiles end-to-end (gated, bench). | 🟢 |
| D3 | decoder-from-profile (CRM follow-up). | 🟢 |

---

## 4. HUB CUTOVER — OLD k8s stack (Fahim/operator; SEPARATE from the new VM-141 platform)

| # | Item | Status |
|---|---|---|
| H1 | Point Fahim's k8s `firmware-service` image build at the `siot-node-firmware-automation` hub (Docker Hub push + kubectl). Code-prep DONE (model-B Dockerfile, `docs/DEPLOY.md`); deploy blocked on registry-push + kubectl access. **Unrelated to the new standalone platform** (which serves careflow from `siot-firmware-automation` on VM 141). | 🟢 Fahim, other stack |

---

## 5. OPS / SECURITY (platform)

| # | Item | Status |
|---|---|---|
| O1 | **Remaining secret rotation** — standing follow-up (siot-ops `CLAUDE.md` §3 workflow). | 🟠 open |
| O2 | **CRM seed creds in a PUBLIC repo** (`siot-crm-review` / prod-140) — real exposure; rotate/remove upstream. (VM-141 CRM logins ARE rotated — `crm-logins.env`.) | 🟠 open |
| O3 | Resolved per current state (2026-07-13): VM-CS `admin/admin` **rotated** ✅ · `HASS_TOKEN` minted / ha-automation apply LIVE ✅ · D4/D5 (ha-automation/ha-mcp) deployed as compose ✅ · ha-mcp + siot-kb gateway upstreams LIVE ✅ · CRM 4 follow-ups DONE ✅. | 🟢 done |

---

## 6. CI / TOOLING / BENCH-VERIFY (hygiene)

| # | Item | Status |
|---|---|---|
| C1 | `api/` pytest suite not in CI (only firmware jobs run). | 🟢 |
| C2 | clang-format pre-commit hook deferred (21/38 firmware files predate `.clang-format`). | 🟢 |
| C3 | RS-FSJT bus-scan tool not pushed / CI'd. | 🟢 |
| C4 | RAK1905 genuine-vs-clone bench check; B4 sensor checks (I3G4250D FS→mdps, BMX160 mag trim). | 🟢 bench |

---

### Minimal set to run the careflow production E2E test
Clear **I1 + I2 + I3** (§0). Everything in §1 is production-hardening you can land before *field* deployment
but does not block a bench E2E (join → decode → fan-out) provided the node is flashed via WebSerial and the
hub is redeployed with the `<blobHex>` fix. §2/§4 are other-product / other-stack; §5-O1/O2 are security
hygiene to close before going to production.
