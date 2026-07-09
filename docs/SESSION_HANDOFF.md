# Session handoff — 2026-07-09 (rak3112-rs485-node)

Personal handoff of a long working session. Everything below is the *current* state; nothing is
pending mid-flight. Repo `main` is at **`1046f5e`**, **no open PRs**.

---

## 1. What got done this session (all merged to `main`)

| # | PR | Commit | What |
|---|---|---|---|
| 1 | — | — | **Careflow node recovery + decode fix** (no PR — bench/ops) |
| 2 | #15 | `84af341` | **WS2812 node-status LED** (GPIO38): boot/sensor-wait/joining/idle/fault + uplink flash |
| 3 | #16 | `6a1b00a` | **Pi 5 RS-485 scanner/profiling station** (P0–P5): firmware `scan-*` console + `pi-scanner/` FastAPI+kiosk |
| 4 | #17 | `0ab9b72` | **`honeywell-eem400-scanned` device profile** (type_byte 5) — produced blind by the scanner |
| 5 | #18 | `1046f5e` | **Kiosk touch controls** (on-screen keyboard + Exit) + white-screen/bodyless-api fixes |

Plus a bench-only branch **`test/eem400-slave-sim`** (pushed, not merged) — the EEM400 Modbus slave
emulator firmware used to blind-test the scanner.

### 1a. Careflow decode/re-join fix (start of session)
The careflow node `3c:dc:75:6f:85:dc` was uplinking but **not decoding** on dev ChirpStack — root
cause was a **session desync**: after a reflash the board restored a stale NVS LoRaWAN session and kept
transmitting `uplink OK` (unconfirmed = no feedback) while ChirpStack had dropped the session, so
`last_seen` was frozen. **Fix = force a fresh OTAA join by erasing NVS** (`esptool erase_region 0x9000
0x6000`) → re-provision from `firmware/.env` → JOINED → RS-FSJT uplinks decoded end-to-end (14 uplinks,
`{device:"RS-FSJT-N01", wind_mps, stale:false}`). Captured in memory `lorawan-session-desync-recovery`.

### 1b. Pi scanner station (the big build — P0–P5)
Turns an **unknown** RS-485 Modbus device into a reusable `device-profiles/profiles/<model>.json`.
- **Firmware** `firmware/main/scan_console.c` (`CONFIG_APP_SCAN_CONSOLE`, default off): read-only
  `scan-cfg/-probe/-ids/-read/-sweep` wrapping `modbus_master_*`, registered only in the unprovisioned
  idle branch. Build variant `sdkconfig.defaults.scanner`.
- **Host** `pi-scanner/`: `node_console` → `sweep_engine` (discover + register map) → `inference`
  (structure only) → operator labels → `profile_emitter` (shells out to the committed generators) →
  `onboarding`. FastAPI + zero-framework kiosk UI. 23 host tests, CI job `pi-scanner-tests`.
- **Validated blind on hardware** via a two-board loopback (see §2). Two real bugs the live test
  surfaced + fixed: inference float-eagerness on integer meters; lossless payload scale.

### 1c. Pi kiosk deployment (co-developed with the on-device Claude Code)
Deployed the scanner as a touchscreen kiosk on the bench Pi. Solved: white-screen after reboot
(Chromium crash-restore flag + stale **SingletonLock**), touch-only controls (on-screen keyboard +
two-tap Exit), and the keyboard mechanism — **squeekboard auto-shows via Chromium `--enable-wayland-ime
--wayland-text-input-version=3` + a labwc Maximize windowRule on a `--app` (not `--kiosk`) window**.
Also fixed a latent **bodyless-`api()` GET→404** on Confirm/Reset/Retry.

### 1d. Handoff docs for Fahim (deploy is tomorrow morning)
`docs/DEPLOY_KICKOFF.md` (deploy runbook/prompt) and `docs/FAHIM_HANDOFF.md` (full architecture + CRM/
ChirpStack flow for both node products). **Both untracked** (local files, not committed).

---

## 2. Bench hardware — current wiring

| Board (MAC) | Role | Where |
|---|---|---|
| `3c:dc:75:6f:7d:c4` | **Scanner node** (scan firmware) | on the **Pi's** USB → `/dev/ttyACM0` |
| `3c:dc:75:6f:81:78` | **EEM400 slave-sim** (emulated device; fw on `test/eem400-slave-sim`) | powered on bench, **CN1 jumpered A/B/GND to the scanner node** |
| `3c:dc:75:6f:85:dc` | Careflow project board (RS-FSJT field node, joined dev ChirpStack) | **disconnected** (by Arif) |

Backup: the stepper firmware that was on `7d:c4` before repurposing is saved at
`…/scratchpad/stepper-7dc4-backup.bin` (SHA `b1a8b1ac…`) — restore with `esptool write_flash 0x0 <file>`.

---

## 3. The bench Pi (kiosk)

- **`pi@192.168.68.109`** — Raspberry Pi 5, hostname `mcp-gateway`, aarch64, Raspberry Pi OS bookworm /
  labwc (Wayland), 10" touch. **Also runs the MCP Pi Gateway on :8000** — the scanner uses **:8080**,
  don't touch :8000.
- **SSH needs the explicit key:** `ssh -i ~/.ssh/id_ed25519_siot_dev_m5 -o IdentitiesOnly=yes pi@192.168.68.109`
  (default key isn't offered).
- **App:** `~/scanner/pi-scanner` (rsync'd, **not** a git checkout). **Service:** `careflow-scanner`
  (systemd, enabled, `127.0.0.1:8080`). **Kiosk:** `~/.config/autostart/careflow-kiosk.desktop` →
  `~/scanner/kiosk.sh` (Chromium `--app`). Boots straight into the wizard.
- **Manage:** `systemctl is-active careflow-scanner`; reload kiosk (survives SSH) =
  `systemd-run --user -q --unit=careflow-kiosk bash ~/scanner/kiosk.sh` after `pkill -f '[c]hromium'`;
  view the session live from the Mac via an SSH tunnel: `ssh -L 8080:localhost:8080 … pi@… 'exec …'`
  then open `http://localhost:8080`.
- Host configs are committed in the repo at `pi-scanner/deploy/{labwc-rc.xml,wf-panel-pi.ini,kiosk.sh,
  careflow-kiosk.desktop,careflow-scanner.service}` + `pi-scanner/deploy/README.md`.

---

## 4. Tomorrow (2026-07-09 AM) — deploy with Fahim

- **Task:** deploy the firmware-build/provision service (`api/`) to Fahim's k8s. **Recommendation:
  Option A** (redeploy this repo's `api/`) — ship the new scanner profile + P7 hardening as one small
  reversible step. **Do NOT** also start the hub (`siot-node-firmware-automation`) cutover — separate,
  hardware-verify first.
- **Start point:** hand Fahim's Claude Code `docs/DEPLOY_KICKOFF.md`; first concrete step is diffing the
  cluster's deployed image git-sha vs `main` HEAD (big version jump). Success check = `GET /v1/sensors`
  lists `honeywell-eem400-scanned`; then confirm the Pi reaches the service at `http://10.10.8.169:8000`.
- **Guardrails:** two ChirpStack stacks — **prompt dev (10.10.8.140) vs production
  (crm/chirpstack.siot.solutions, Fahim's k8s 10.10.8.168) before any CRM/ChirpStack action**;
  production is read-only unless Arif authorizes; no `:latest`; pin the image to the git sha.

---

## 5. Deferred / open (not blocking)

- **Hub cutover** — move `api/`+`tools/`+`device-profiles/` to `siot-node-firmware-automation` (needs
  hardware-verify; Fahim-coordinated).
- **device_profile v1 → v2** unification (senseflow uses v2 multi-bus; careflow still v1).
- **P5 live uplink leg for the scanner** — the scanner's discover→infer→package is proven blind on
  hardware; the LoRaWAN uplink-verify leg was skipped (already proven in P5/P6). Would need `7d:c4`
  registered as a device on dev ChirpStack.
- **Real-MFM384 uplink** leg (meter not on the home bench).
- **`85:dc` field node** — left disconnected; was the joined RS-FSJT node on dev ChirpStack.
- **Two untracked docs** (`DEPLOY_KICKOFF.md`, `FAHIM_HANDOFF.md`, and this `SESSION_HANDOFF.md`) — not
  committed. Offer stands to open a small docs PR if you want them in the repo.

---

## 6. Gotchas learned this session (save future time)

- **USB-Serial-JTAG console:** read with **DTR asserted** (`dtr=True`) to flush TX — the *opposite* of
  the UART rule. Reset on the USJ is a USB control sequence (esptool), not DTR/RTS toggling.
- **LoRaWAN session desync:** `uplink OK` on an *unconfirmed* frame means nothing about acceptance —
  check ChirpStack `last_seen`. Fix a desync with a fresh OTAA join (erase NVS `lorawan`+`prov`).
- **Kiosk (labwc/Wayland):** `--kiosk` hides the on-screen keyboard layer → use `--app` + a labwc
  Maximize windowRule; clear Chromium `SingletonLock/Socket/Cookie` before relaunch or it maps no
  window (shows only the desktop); `--enable-wayland-ime` is what makes squeekboard type into fields.
- **Launching GUI over SSH:** `setsid`/`nohup` get reaped when the SSH session ends — use
  `systemd-run --user --unit=…` (survives disconnect) or the desktop autostart.
- **gitleaks CI (`fetch-depth: 0`) scans *all* fetched commits** — a `profileKey` on another branch can
  fail an unrelated PR; the allowlist must reach `main` (merge the profile PR first).
- **`api()` helper GETs when bodyless** — action endpoints are POST; pass `{}` to force POST.
- **`pkill -f "uvicorn app.main"`** matches your own SSH command's arg string → kills your session; use
  `fuser -k 8080/tcp` or the `[u]vicorn` regex trick.

---

## 7. Fast resume pointers

- Memory index: `…/memory/MEMORY.md` (updated: `pi-scanner-station`, `lorawan-session-desync-recovery`).
- Architecture + CRM/ChirpStack flow: `docs/FAHIM_HANDOFF.md`.
- Scanner station internals: `pi-scanner/README.md`; phase plan: `~/.claude/plans/generic-jingling-owl.md`.
- Authoritative CRM sequence: `docs/CRM_PROVISIONING_WORKFLOW.md` + `docs/PROVISIONING_API_CONTRACT.md`.
